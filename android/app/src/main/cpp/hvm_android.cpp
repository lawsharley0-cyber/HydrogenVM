// HydrogenVM for Android: the JNI bridge between the Android app (com.hydrogenvm.android.Native)
// and the emulator core. One Session per running game:
//   * the game runs on its own thread with a large stack (as on iPhone);
//   * OpenGL ES runs on the phone's GPU through EGL (runtime/gl/HostGLEgl.cpp); each composed frame
//     arrives through ProcessConfig::present and is copied, turned upright, into the SurfaceView;
//   * touches arrive as positions within the shown picture and are mapped to the game's points;
//   * sound is pulled from the game's mixer by an AAudio stream.
#include "loader/Apk.hpp"
#include "loader/AppBundle.hpp"
#include "runtime/Process.hpp"
#include "runtime/audio/Audio.hpp"
#include "runtime/gl/HostGL.hpp"
#include "runtime/uikit/UIKit.hpp"

#include <aaudio/AAudio.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <pthread.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace RT = Hydrogen::Runtime;
namespace fs = std::filesystem;

namespace {

constexpr const char* kTag = "HydrogenVM";

std::string str(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out = c ? c : "";
    if (c) env->ReleaseStringUTFChars(s, c);
    return out;
}

jobjectArray strings(JNIEnv* env, const std::vector<std::string>& v) {
    jobjectArray a = env->NewObjectArray(jsize(v.size()), env->FindClass("java/lang/String"), nullptr);
    for (size_t i = 0; i < v.size(); ++i) env->SetObjectArrayElement(a, jsize(i), env->NewStringUTF(v[i].c_str()));
    return a;
}

// Display-normalised (u, v) in the upright picture to framebuffer-normalised (x, y) in the game's
// portrait framebuffer, for UIKit's interface orientation (the iPhone app's PlayerGeometry table).
void toFramebuffer(int orientation, float u, float v, float& x, float& y) {
    switch (orientation) {
        case 2: x = 1 - u; y = 1 - v; break;   // upside down
        case 4: x = v; y = 1 - u; break;       // landscape left
        case 3: x = 1 - v; y = u; break;       // landscape right
        default: x = u; y = v; break;
    }
}
bool landscape(int orientation) { return orientation == 3 || orientation == 4; }

struct Session {
    std::shared_ptr<RT::Process> proc;
    std::shared_ptr<RT::Audio::Mixer> mixer;
    std::atomic<bool> running{true};
    std::string exitMessage;
    std::mutex mu;                       // proc, exitMessage, frame, window
    std::condition_variable frameCv;
    std::vector<uint8_t> frame;          // last composed frame, RGBA, portrait framebuffer
    int frameW = 0, frameH = 0, orientation = 1;
    float guestW = 320, guestH = 480;    // the game's screen in points (touch space)
    bool frameReady = false, stopBlit = false;
    ANativeWindow* window = nullptr;
    std::thread blitter;
    AAudioStream* audio = nullptr;
    std::vector<float> left, right;
};

// Copies the newest frame into the window, turned upright (nearest pixel per window pixel).
void blitLoop(Session* s) {
    std::vector<uint8_t> local;
    for (;;) {
        int fw, fh, o;
        ANativeWindow* win;
        {
            std::unique_lock<std::mutex> lk(s->mu);
            s->frameCv.wait(lk, [&] { return s->stopBlit || (s->frameReady && s->window); });
            if (s->stopBlit) return;
            s->frameReady = false;
            local.swap(s->frame);
            fw = s->frameW; fh = s->frameH; o = s->orientation;
            win = s->window;
            ANativeWindow_acquire(win);
        }
        const int dw = landscape(o) ? fh : fw, dh = landscape(o) ? fw : fh;
        ANativeWindow_setBuffersGeometry(win, dw, dh, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
        ANativeWindow_Buffer buf;
        if (ANativeWindow_lock(win, &buf, nullptr) == 0) {
            auto* dst = static_cast<uint32_t*>(buf.bits);
            const auto* src = reinterpret_cast<const uint32_t*>(local.data());
            const int w = std::min(dw, int(buf.width)), h = std::min(dh, int(buf.height));
            for (int y = 0; y < h; ++y) {
                uint32_t* row = dst + size_t(y) * size_t(buf.stride);
                const float v = (float(y) + 0.5f) / float(dh);
                for (int x = 0; x < w; ++x) {
                    float fx, fy;
                    toFramebuffer(o, (float(x) + 0.5f) / float(dw), v, fx, fy);
                    const int sx = std::min(fw - 1, int(fx * float(fw))), sy = std::min(fh - 1, int(fy * float(fh)));
                    row[x] = src[size_t(sy) * size_t(fw) + size_t(sx)] | 0xFF000000u;   // opaque
                }
            }
            ANativeWindow_unlockAndPost(win);
        }
        ANativeWindow_release(win);
        std::lock_guard<std::mutex> lk(s->mu);
        if (s->frame.empty()) s->frame.swap(local);   // reuse the buffer
    }
}

aaudio_data_callback_result_t audioCallback(AAudioStream*, void* user, void* data, int32_t frames) {
    auto* s = static_cast<Session*>(user);
    auto* out = static_cast<float*>(data);
    if (!s->mixer || frames <= 0) { std::memset(out, 0, size_t(frames) * 2 * sizeof(float)); return AAUDIO_CALLBACK_RESULT_CONTINUE; }
    if (s->left.size() < size_t(frames)) { s->left.resize(size_t(frames)); s->right.resize(size_t(frames)); }
    s->mixer->render(s->left.data(), s->right.data(), size_t(frames));
    for (int32_t i = 0; i < frames; ++i) { out[2 * i] = s->left[size_t(i)]; out[2 * i + 1] = s->right[size_t(i)]; }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void startAudio(Session* s) {
    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return;
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, RT::Audio::Mixer::kRate);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(b, audioCallback, s);
    if (AAudioStreamBuilder_openStream(b, &s->audio) == AAUDIO_OK) AAudioStream_requestStart(s->audio);
    else s->audio = nullptr;
    AAudioStreamBuilder_delete(b);
}

void stopAudio(Session* s) {
    if (!s->audio) return;
    AAudioStream_requestStop(s->audio);
    AAudioStream_close(s->audio);
    s->audio = nullptr;
}

struct StartArgs {
    Session* session;
    RT::ProcessConfig cfg;
};

void* gameThread(void* p) {
    std::unique_ptr<StartArgs> args(static_cast<StartArgs*>(p));
    Session* s = args->session;
    std::string message;
    try {
        auto proc = std::make_shared<RT::Process>(args->cfg);
        {
            std::lock_guard<std::mutex> lk(s->mu);
            s->proc = proc;
            s->mixer = proc->mixerPtr();
        }
        proc->load();
        startAudio(s);
        const int code = proc->run();
        if (proc->crashed()) message = "The game stopped (crash). See the log for details.";
        else if (code != 0) message = "The game closed (exit code " + std::to_string(code) + ").";
    } catch (const std::exception& e) {
        message = std::string("The game could not run: ") + e.what();
        __android_log_print(ANDROID_LOG_ERROR, kTag, "%s", message.c_str());
    }
    stopAudio(s);
    std::lock_guard<std::mutex> lk(s->mu);
    s->exitMessage = message;
    s->running = false;
    return nullptr;
}

Session* session(jlong h) { return reinterpret_cast<Session*>(h); }

} // namespace

extern "C" {

// importGame(file, appsRoot) -> {bundlePath, title, bundleId, problem}; problem "" when it can run.
JNIEXPORT jobjectArray JNICALL Java_com_hydrogenvm_android_Native_importGame(JNIEnv* env, jclass, jstring file, jstring root) {
    const std::string path = str(env, file), appsRoot = str(env, root);
    try {
        std::string ext = fs::path(path).extension().string();
        for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
        const bool android = ext == ".apk" || ext == ".xapk" || ext == ".apks" || ext == ".apkm";
        const auto info = android ? Hydrogen::Loader::importApk(path, appsRoot) : Hydrogen::Loader::importIpa(path, appsRoot);
        const std::string title = info.displayName.empty() ? info.name : info.displayName;
        return strings(env, {info.bundlePath, title, info.bundleId, info.runnable ? "" : (info.problem.empty() ? "This game can't run." : info.problem)});
    } catch (const std::exception& e) {
        return strings(env, {"", "", "", std::string("Import failed: ") + e.what()});
    }
}

// start(bundlePath, homePath, higherResolution, displayShortSidePixels, unlockFrameRate, virtualScreenHeight)
JNIEXPORT jlong JNICALL Java_com_hydrogenvm_android_Native_start(JNIEnv* env, jclass, jstring bundle, jstring home,
                                                                  jboolean higherResolution, jint displayShort,
                                                                  jboolean unlockFrameRate, jint virtualHeight) {
    auto* s = new Session();
    auto args = std::make_unique<StartArgs>();
    args->session = s;
    RT::ProcessConfig& cfg = args->cfg;
    cfg.bundlePath = str(env, bundle);
    cfg.homePath = str(env, home);
    cfg.screenWidth = 320;
    cfg.screenHeight = 568;   // UIKit drops to 480 unless the app opts into 4"
    cfg.screenScale = 2;
    cfg.rotateAndroidReadback = true;
    cfg.unlockFrameRate = unlockFrameRate;
    if (higherResolution) { cfg.resolutionScale = 3; cfg.displayShortSidePixels = uint32_t(std::max(0, int(displayShort))); }
    if (virtualHeight >= 480 && virtualHeight <= 1024) setenv("HVM_VIRTUAL_SCREEN_HEIGHT", std::to_string(virtualHeight).c_str(), 1);
    else unsetenv("HVM_VIRTUAL_SCREEN_HEIGHT");
    unsetenv("HVM_SAFE_AREA");
    std::string why;
    cfg.hostGL = RT::HostGL::createEglPlatform(&why);
    if (!cfg.hostGL) __android_log_print(ANDROID_LOG_WARN, kTag, "GPU unavailable (%s): software rendering", why.c_str());
    cfg.log = [](RT::LogLevel lvl, const std::string& msg) {
        const int prio = lvl == RT::LogLevel::Error ? ANDROID_LOG_ERROR : lvl == RT::LogLevel::Warning ? ANDROID_LOG_WARN : ANDROID_LOG_INFO;
        __android_log_print(prio, kTag, "%s", msg.c_str());
    };
    cfg.present = [s](const uint8_t* rgba, int w, int h) {
        std::shared_ptr<RT::Process> p;
        {
            std::lock_guard<std::mutex> lk(s->mu);
            p = s->proc;
        }
        int orientation = 1;
        float gw = 0, gh = 0;
        if (p) {   // read UIKit on this guest render thread, with the pixels
            orientation = RT::UIKit::ui(*p).statusBarOrientation;
            gw = RT::UIKit::ui(*p).screenW;
            gh = RT::UIKit::ui(*p).screenH;
        }
        std::lock_guard<std::mutex> lk(s->mu);
        s->frame.assign(rgba, rgba + size_t(w) * size_t(h) * 4);
        s->frameW = w; s->frameH = h; s->orientation = orientation;
        if (gw > 0 && gh > 0) { s->guestW = gw; s->guestH = gh; }
        s->frameReady = true;
        s->frameCv.notify_one();
    };
    s->blitter = std::thread(blitLoop, s);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16u << 20);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    StartArgs* raw = args.release();
    if (pthread_create(&t, &attr, gameThread, raw) != 0) {
        delete raw;
        s->running = false;
        s->exitMessage = "Could not start the game thread.";
    }
    pthread_attr_destroy(&attr);
    return reinterpret_cast<jlong>(s);
}

JNIEXPORT void JNICALL Java_com_hydrogenvm_android_Native_setSurface(JNIEnv* env, jclass, jlong h, jobject surface) {
    Session* s = session(h);
    ANativeWindow* win = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    std::lock_guard<std::mutex> lk(s->mu);
    if (s->window) ANativeWindow_release(s->window);
    s->window = win;
    if (win && !s->frame.empty()) s->frameReady = true;   // show the last frame again on a new surface
    s->frameCv.notify_one();
}

// The shown picture's width / height (0 before the first frame).
JNIEXPORT jfloat JNICALL Java_com_hydrogenvm_android_Native_aspect(JNIEnv*, jclass, jlong h) {
    Session* s = session(h);
    std::lock_guard<std::mutex> lk(s->mu);
    if (s->frameW <= 0 || s->frameH <= 0) return 0;
    return landscape(s->orientation) ? float(s->frameH) / float(s->frameW) : float(s->frameW) / float(s->frameH);
}

// phase: 0 began, 1 moved, 3 ended, 4 cancelled; (u, v): position within the shown picture, 0...1.
JNIEXPORT void JNICALL Java_com_hydrogenvm_android_Native_touch(JNIEnv*, jclass, jlong h, jint phase, jfloat u, jfloat v, jint finger) {
    Session* s = session(h);
    std::shared_ptr<RT::Process> p;
    int o;
    float gw, gh;
    {
        std::lock_guard<std::mutex> lk(s->mu);
        p = s->proc;
        o = s->orientation;
        gw = s->guestW; gh = s->guestH;
    }
    if (!p) return;
    float x, y;
    toFramebuffer(o, std::clamp(float(u), 0.0f, 1.0f), std::clamp(float(v), 0.0f, 1.0f), x, y);
    p->injectTouch(RT::TouchPhase(phase), std::min(x * gw, gw - 0.5f), std::min(y * gh, gh - 0.5f), uint32_t(finger) + 1);
}

JNIEXPORT void JNICALL Java_com_hydrogenvm_android_Native_pause(JNIEnv*, jclass, jlong h) {
    Session* s = session(h);
    std::shared_ptr<RT::Process> p;
    { std::lock_guard<std::mutex> lk(s->mu); p = s->proc; }
    if (p && s->running) p->enterBackground();
    if (s->audio) AAudioStream_requestPause(s->audio);
}

JNIEXPORT void JNICALL Java_com_hydrogenvm_android_Native_resume(JNIEnv*, jclass, jlong h) {
    Session* s = session(h);
    std::shared_ptr<RT::Process> p;
    { std::lock_guard<std::mutex> lk(s->mu); p = s->proc; }
    if (p && s->running) p->enterForeground();
    if (s->audio) AAudioStream_requestStart(s->audio);
}

JNIEXPORT jboolean JNICALL Java_com_hydrogenvm_android_Native_isRunning(JNIEnv*, jclass, jlong h) { return session(h)->running.load(); }

JNIEXPORT jstring JNICALL Java_com_hydrogenvm_android_Native_exitMessage(JNIEnv* env, jclass, jlong h) {
    Session* s = session(h);
    std::lock_guard<std::mutex> lk(s->mu);
    return env->NewStringUTF(s->exitMessage.c_str());
}

// Asks the game to close (it gets the chance to save, as on iPhone), waits for it, frees the session.
JNIEXPORT void JNICALL Java_com_hydrogenvm_android_Native_stop(JNIEnv*, jclass, jlong h) {
    Session* s = session(h);
    std::shared_ptr<RT::Process> p;
    { std::lock_guard<std::mutex> lk(s->mu); p = s->proc; }
    if (p && s->running) p->requestGracefulStop();
    for (int i = 0; i < 400 && s->running; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(25));
    {
        std::lock_guard<std::mutex> lk(s->mu);
        s->stopBlit = true;
        s->frameCv.notify_one();
    }
    if (s->blitter.joinable()) s->blitter.join();
    if (s->running) return;   // still closing after 10 s: leave it (the process exits with the app)
    {
        std::lock_guard<std::mutex> lk(s->mu);
        if (s->window) ANativeWindow_release(s->window);
        s->window = nullptr;
    }
    delete s;
}

} // extern "C"
