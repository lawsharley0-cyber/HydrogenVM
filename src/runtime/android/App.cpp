// HydrogenVM — an Android game's activity: the Java classes its native code calls, and the
// lifecycle and frame loop a GLSurfaceView would drive. See Android.hpp.
#include "runtime/android/Jni.hpp"
#include "runtime/audio/Audio.hpp"
#include "runtime/gl/HostGL.hpp"
#include "runtime/uikit/UIKit.hpp"
#include "loader/Apk.hpp"
#include "core/cpu/Engine.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;

namespace Hydrogen::Runtime::Android {
namespace {

using namespace std::chrono_literals;

// The surface: the emulated screen in landscape (games of this era are landscape).
struct Surface {
    int width = 0, height = 0;
    float scale = 2.0f;
    bool portrait = false;
};
Surface surfaceFor(Process& p) {
    const auto& c = p.config();
    Surface s;
    s.scale = c.screenScale;
    const int w = int(float(c.screenWidth) * c.screenScale), h = int(float(c.screenHeight) * c.screenScale);
    // The generated Info.plist (loader/Apk.cpp) says portrait for portrait games.
    s.portrait = false;
    if (const auto* info = p.infoPlist())
        if (const auto* o = info->get("UISupportedInterfaceOrientations"); o && o->type == Plist::Value::Type::Array)
            for (const auto& v : o->array) if (v->s == "UIInterfaceOrientationPortrait") s.portrait = true;
    s.width = s.portrait ? std::min(w, h) : std::max(w, h);
    s.height = s.portrait ? std::max(w, h) : std::min(w, h);
    return s;
}

std::string assetsDir(Process& p) { return p.config().bundlePath + "/assets"; }

// An open asset (AssetManager.open): a host file read through InputStream.
struct AssetStream {
    std::FILE* f = nullptr;
    int64_t size = 0;
    ~AssetStream() { if (f) std::fclose(f); }
};

// An AudioTrack: a stream voice in the mixer, paced like a real track's buffer.
struct Track {
    uint32_t voice = 0;
    int rate = 44100, channels = 2;
    bool playing = false;
    std::chrono::steady_clock::time_point start{};
    uint64_t framesWritten = 0;
};

// The game's view of the device.
std::string deviceString(const std::string& name) {
    if (name == "GetApiLevel") return "10";
    if (name == "GetPlatformVersion") return "2.3.4";
    if (name == "GetDeviceModel") return "HydrogenVM";
    if (name == "GetDeviceName") return "HydrogenVM";
    if (name == "GetManufacturer") return "HydrogenVM";
    if (name == "GetChipset") return "armv7";
    if (name == "GetProcessorArchitecture") return "armeabi-v7a";
    if (name == "GetPlatformRawName" || name == "GetPlatformStdName") return "Android";
    if (name == "GetLanguage") return "en";
    if (name == "GetLocale") return "en_US";
    if (name == "GetTotalRAM") return "1073741824";
    if (name == "GetTouchScreenCount" || name == "GetDisplayCount" || name == "GetAccelerometerCount") return "1";
    if (name == "GetDeviceUniqueId") return "0000000000000000";
    if (name == "IsBatteryStateAvailable" || name == "GetLocationAvailable") return "0";
    return "0";   // counts of hardware we don't have: cameras, keyboards, ...
}

// Games that keep their data on the SD card (Android/data/<package>/files) find it there: the
// folder links to the APK's assets.
void linkExternalFiles(Process& p, const fs::path& files) {
    std::error_code ec;
    fs::create_directories(files, ec);
    for (auto& e : fs::directory_iterator(assetsDir(p), ec)) {
        const fs::path link = files / e.path().filename();
        if (!fs::exists(fs::symlink_status(link, ec))) fs::create_symlink(e.path(), link, ec);
    }
}
std::string externalRoot(Process& p) { return p.config().homePath + "/Library/Caches/sdcard"; }
void linkExternalData(Process& p, const std::string& external) {
    std::ifstream in(p.config().bundlePath + "/AndroidManifest.xml", std::ios::binary);
    const std::vector<uint8_t> m((std::istreambuf_iterator<char>(in)), {});
    const std::string pkg = Loader::parseManifest(m).package;
    if (!pkg.empty()) linkExternalFiles(p, fs::path(external) / "Android/data" / pkg / "files");
}

} // namespace

// The game may name its data folder after another package than the APK's (a repackaged
// build): link whichever Android/data/<package>/files it looks in.
void noteExternalPath(Process& p, const std::string& hostPath) {
    const std::string prefix = externalRoot(p) + "/Android/data/";
    if (hostPath.compare(0, prefix.size(), prefix) != 0) return;
    const size_t end = hostPath.find('/', prefix.size());
    const std::string pkg = hostPath.substr(prefix.size(), end == std::string::npos ? std::string::npos : end - prefix.size());
    if (pkg.empty() || pkg == "." || pkg == ".." || pkg.find('\0') != std::string::npos) return;
    static std::mutex mu;
    static std::set<std::string> linked;
    std::lock_guard<std::mutex> lk(mu);
    if (linked.insert(p.config().homePath + "|" + pkg).second) linkExternalFiles(p, fs::path(prefix) / pkg / "files");
}

void registerJavaClasses(Jni& j) {
    Process& p = j.proc;
    const std::string kAct = "com/ea/blast/MainActivity";

    // The activity: one object for the whole run.
    JObject act;
    act.cls = kAct;
    act.permanent = true;
    const jobject activity = j.add(std::move(act));
    JObject am;
    am.cls = "android/content/res/AssetManager";
    am.permanent = true;
    const jobject assets = j.add(std::move(am));
    j.staticFields["hvm.activity"] = activity;
    j.staticFields["hvm.assets"] = assets;

    j.method(kAct, "GetInstance", "()Lcom/ea/blast/MainActivity;", [activity](Jni&, Ctx&, jobject, const auto&) { return JValue(activity); });
    j.method(kAct, "getAssets", "()Landroid/content/res/AssetManager;", [assets](Jni&, Ctx&, jobject, const auto&) { return JValue(assets); });
    j.method("com/eamobile/Query", "isContentReady", "()Z", [](Jni&, Ctx&, jobject, const auto&) { return JValue(1); });
    j.method("com/eamobile/Query", "getTotalMemory", "()J", [](Jni&, Ctx&, jobject, const auto&) { return JValue(1ull << 30); });
    j.method("com/eamobile/Query", "getVersion", "()Ljava/lang/String;", [](Jni& j, Ctx&, jobject, const auto&) { return JValue(j.newString("1.2.0")); });
    j.method(kAct, "isContentReady", "()Z", [](Jni&, Ctx&, jobject, const auto&) { return JValue(1); });
    j.method(kAct, "getTotalMemory", "()J", [](Jni&, Ctx&, jobject, const auto&) { return JValue(1ull << 30); });
    j.method(kAct, "getVersion", "()Ljava/lang/String;", [](Jni& j, Ctx&, jobject, const auto&) { return JValue(j.newString("1.2.0")); });
    for (const char* n : {"ForceHideVirtualKeyboard"})
        j.method(kAct, n, "()V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });

    // Assets.
    const std::string kAM = "android/content/res/AssetManager";
    j.method(kAM, "open", "(Ljava/lang/String;)Ljava/io/InputStream;", [](Jni& j, Ctx& x, jobject, const auto& a) {
        const std::string name = j.string(jobject(a[0]));
        const std::string path = assetsDir(x.proc) + "/" + name;
        std::FILE* f = std::fopen(path.c_str(), "rb");
        x.proc.recordFileOpen("assets/" + name, "AssetManager.open", f ? 0 : errno, -1, 0, f != nullptr);
        if (!f) return JValue(0);   // the game sees null (Java would throw FileNotFoundException)
        auto s = std::make_shared<AssetStream>();
        s->f = f;
        std::fseek(f, 0, SEEK_END);
        s->size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        JObject o;
        o.cls = "java/io/InputStream";
        o.host = s;
        return JValue(j.add(std::move(o)));
    });
    j.method(kAM, "openFd", "(Ljava/lang/String;)Landroid/content/res/AssetFileDescriptor;", [](Jni& j, Ctx& x, jobject, const auto& a) {
        const std::string name = j.string(jobject(a[0]));
        const std::string path = assetsDir(x.proc) + "/" + name;
        const int fd = ::open(path.c_str(), O_RDONLY);
        x.proc.recordFileOpen("assets/" + name, "AssetManager.openFd", fd >= 0 ? 0 : errno, -1, 0, fd >= 0);
        if (fd < 0) return JValue(0);
        JObject fdo;
        fdo.cls = "java/io/FileDescriptor";
        fdo.fields["descriptor"] = uint32_t(fd);
        const jobject fdObj = j.add(std::move(fdo));
        JObject o;
        o.cls = "android/content/res/AssetFileDescriptor";
        o.fields["hvm.fd"] = fdObj;
        o.fields["hvm.length"] = uint64_t(fs::file_size(path));
        return JValue(j.add(std::move(o)));
    });
    j.method(kAM, "list", "(Ljava/lang/String;)[Ljava/lang/String;", [](Jni& j, Ctx& x, jobject, const auto& a) {
        std::vector<std::string> names;
        std::error_code ec;
        for (auto& e : fs::directory_iterator(assetsDir(x.proc) + "/" + j.string(jobject(a[0])), ec))
            names.push_back(e.path().filename().string());
        const jobject arr = j.newArray('L', uint32_t(names.size()));
        for (size_t i = 0; i < names.size(); ++i) {
            const jobject s = j.newString(names[i]);
            if (JObject* o = j.get(arr)) o->refs[i] = s;
        }
        return JValue(arr);
    });
    const std::string kAFD = "android/content/res/AssetFileDescriptor";
    j.method(kAFD, "getFileDescriptor", "()Ljava/io/FileDescriptor;", [](Jni& j, Ctx&, jobject self, const auto&) {
        JObject* o = j.get(self);
        return JValue(o ? o->fields["hvm.fd"] : 0);
    });
    j.method(kAFD, "getStartOffset", "()J", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kAFD, "getLength", "()J", [](Jni& j, Ctx&, jobject self, const auto&) {
        JObject* o = j.get(self);
        return JValue(o ? o->fields["hvm.length"] : 0);
    });
    j.method(kAFD, "close", "()V", [](Jni& j, Ctx&, jobject self, const auto&) {
        if (JObject* o = j.get(self))
            if (JObject* f = j.get(jobject(o->fields["hvm.fd"]))) { ::close(int(f->fields["descriptor"])); f->fields["descriptor"] = uint32_t(-1); }
        return JValue(0);
    });
    const std::string kIS = "java/io/InputStream";
    auto stream = [](Jni& j, jobject self) -> std::shared_ptr<AssetStream> {
        JObject* o = j.get(self);
        if (!o) return nullptr;
        auto* s = std::any_cast<std::shared_ptr<AssetStream>>(&o->host);
        return s ? *s : nullptr;
    };
    j.method(kIS, "read", "([BII)I", [stream](Jni& j, Ctx&, jobject self, const auto& a) {
        auto s = stream(j, self);
        if (!s || !s->f) return JValue(uint32_t(-1));
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* arr = j.get(jobject(a[0]));
        const uint32_t off = uint32_t(a[1]), len = uint32_t(a[2]);
        if (!arr || size_t(off) + len > arr->data.size()) return JValue(uint32_t(-1));
        const size_t n = std::fread(arr->data.data() + off, 1, len, s->f);
        return JValue(n == 0 && len ? uint32_t(-1) : uint32_t(n));
    });
    j.method(kIS, "read", "()I", [stream](Jni& j, Ctx&, jobject self, const auto&) {
        auto s = stream(j, self);
        const int c = s && s->f ? std::fgetc(s->f) : EOF;
        return JValue(uint32_t(c == EOF ? -1 : c));
    });
    j.method(kIS, "skip", "(J)J", [stream](Jni& j, Ctx&, jobject self, const auto& a) {
        auto s = stream(j, self);
        if (!s || !s->f) return JValue(0);
        const int64_t at = std::ftell(s->f), n = std::max<int64_t>(0, std::min<int64_t>(int64_t(a[0]), s->size - at));
        std::fseek(s->f, long(at + n), SEEK_SET);
        return JValue(uint64_t(n));
    });
    j.method(kIS, "available", "()I", [stream](Jni& j, Ctx&, jobject self, const auto&) {
        auto s = stream(j, self);
        return JValue(s && s->f ? uint32_t(s->size - std::ftell(s->f)) : 0);
    });
    j.method(kIS, "close", "()V", [stream](Jni& j, Ctx&, jobject self, const auto&) {
        if (auto s = stream(j, self); s && s->f) { std::fclose(s->f); s->f = nullptr; }
        j.remove(self);
        return JValue(0);
    });
    for (const char* n : {"mark", "reset"})
        j.method(kIS, n, n[0] == 'm' ? "(I)V" : "()V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kIS, "markSupported", "()Z", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });

    // Device information.
    const std::string kSys = "com/ea/blast/SystemAndroidDelegate";
    for (const char* n : {"GetAccelerometerCount", "GetApiLevel", "GetCameraCount", "GetChipset", "GetCompassCount",
                          "GetDeviceModel", "GetDeviceName", "GetDeviceSubscriberID", "GetDeviceUniqueId", "GetDisplayCount",
                          "GetGyroscopeCount", "GetLanguage", "GetLocale", "GetLocationAvailable", "GetManufacturer",
                          "GetMicrophoneCount", "GetPhoneNumber", "GetPhysicalKeyboardCount", "GetPlatformRawName",
                          "GetPlatformStdName", "GetPlatformVersion", "GetProcessorArchitecture", "GetTotalRAM",
                          "GetTouchPadCount", "GetTouchScreenCount", "GetTrackBallCount", "GetVibratorCount",
                          "GetVirtualKeyboardCount", "IsBatteryStateAvailable"}) {
        const std::string name = n;
        j.method(kSys, name, "()Ljava/lang/String;", [name](Jni& j, Ctx&, jobject, const auto&) { return JValue(j.newString(deviceString(name))); });
    }
    j.method(kSys, "IntentView", "(Ljava/lang/String;)Z", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    const std::string kDisp = "com/ea/blast/DisplayAndroidDelegate";
    j.method(kDisp, "GetDefaultWidth", "()I", [](Jni& j, Ctx&, jobject, const auto&) { return JValue(uint32_t(surfaceFor(j.proc).width)); });
    j.method(kDisp, "GetDefaultHeight", "()I", [](Jni& j, Ctx&, jobject, const auto&) { return JValue(uint32_t(surfaceFor(j.proc).height)); });
    j.method(kDisp, "GetDpiX", "()F", [](Jni&, Ctx&, jobject, const auto&) { return jfloat(326.0f); });
    j.method(kDisp, "GetDpiY", "()F", [](Jni&, Ctx&, jobject, const auto&) { return jfloat(326.0f); });
    j.method(kDisp, "GetStdOrientation", "()I", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kDisp, "SetStdOrientation", "(I)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    const std::string kDir = "com/ea/blast/GetAppDataDirectoryDelegate";
    j.method(kDir, "GetAppDataDirectory", "()Ljava/lang/String;", [&p](Jni& j, Ctx&, jobject, const auto&) {
        const std::string host = p.config().homePath + "/Documents";
        std::error_code ec;
        fs::create_directories(host, ec);
        return JValue(j.newString(p.guestPath(host)));
    });
    j.method(kDir, "GetExternalStorageDirectory", "()Ljava/lang/String;", [&p](Jni& j, Ctx&, jobject, const auto&) {
        const std::string host = externalRoot(p);
        std::error_code ec;
        fs::create_directories(host, ec);
        linkExternalData(p, host);
        return JValue(j.newString(p.guestPath(host)));
    });
    for (const auto& [cls, name, sig] : std::vector<std::tuple<std::string, std::string, std::string>>{
             {"com/ea/blast/PowerManagerAndroid", "ApplyKeepAwake", "(Z)V"},
             {"com/ea/blast/AccelerometerAndroidDelegate", "SetEnabled", "(Z)V"},
             {"com/ea/blast/AccelerometerAndroidDelegate", "SetUpdateFrequency", "(I)V"},
             {"com/ea/blast/DeviceOrientationHandlerAndroidDelegate", "SetEnabled", "(Z)V"},
             {"com/ea/blast/DeviceOrientationHandlerAndroidDelegate", "OnLifeCycleFocusGained", "()V"},
             {"com/ea/blast/DeviceOrientationHandlerAndroidDelegate", "UpdateOrientation", "(I)V"},
             {"java/lang/Object", "<init>", "()V"}})
        j.method(cls, name, sig, [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("com/ea/blast/TouchSurfaceAndroid", "IsTouchScreenMultiTouch", "()Z", [](Jni&, Ctx&, jobject, const auto&) { return JValue(1); });

    // Sound: AudioTrack in streaming mode, mixed by HydrogenVM.
    const std::string kAT = "android/media/AudioTrack";
    auto track = [](Jni& j, jobject self) -> std::shared_ptr<Track> {
        JObject* o = j.get(self);
        if (!o) return nullptr;
        auto* t = std::any_cast<std::shared_ptr<Track>>(&o->host);
        return t ? *t : nullptr;
    };
    j.method(kAT, "getNativeOutputSampleRate", "(I)I", [](Jni&, Ctx&, jobject, const auto&) { return JValue(44100); });
    j.method(kAT, "getMinBufferSize", "(III)I", [](Jni&, Ctx&, jobject, const auto&) { return JValue(8192); });
    j.method(kAT, "<init>", "(IIIIII)V", [](Jni& j, Ctx& x, jobject self, const auto& a) {
        auto t = std::make_shared<Track>();
        t->rate = int(a[1]);
        t->channels = (a[2] == 2 || a[2] == 4) ? 1 : 2;   // CHANNEL_CONFIGURATION_MONO (2) / CHANNEL_OUT_MONO (4)
        t->voice = x.proc.mixer().createStream({});
        if (JObject* o = j.get(self)) { o->host = t; o->permanent = true; }
        return JValue(0);
    });
    j.method(kAT, "play", "()V", [track](Jni& j, Ctx& x, jobject self, const auto&) {
        if (auto t = track(j, self)) {
            x.proc.mixer().setPaused(t->voice, false);
            if (!t->playing) { t->playing = true; t->start = std::chrono::steady_clock::now(); t->framesWritten = 0; }
        }
        return JValue(0);
    });
    for (const char* n : {"stop", "pause", "flush"}) {
        const std::string name = n;
        j.method(kAT, name, "()V", [track, name](Jni& j, Ctx& x, jobject self, const auto&) {
            if (auto t = track(j, self)) {
                if (name == "pause") x.proc.mixer().setPaused(t->voice, true);
                else x.proc.mixer().stop(t->voice);
                if (name != "flush") t->playing = false;
            }
            return JValue(0);
        });
    }
    j.method(kAT, "release", "()V", [track](Jni& j, Ctx& x, jobject self, const auto&) {
        if (auto t = track(j, self)) x.proc.mixer().release(t->voice);
        return JValue(0);
    });
    j.method(kAT, "getPlayState", "()I", [track](Jni& j, Ctx&, jobject self, const auto&) {
        auto t = track(j, self);
        return JValue(t && t->playing ? 3 : 1);   // PLAYSTATE_PLAYING / STOPPED
    });
    j.method(kAT, "setStereoVolume", "(FF)I", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kAT, "write", "([SII)I", [track](Jni& j, Ctx& x, jobject self, const auto& a) {
        auto t = track(j, self);
        const uint32_t off = uint32_t(a[1]), count = uint32_t(a[2]);
        auto pcm = std::make_shared<std::vector<int16_t>>();
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            JObject* arr = j.get(jobject(a[0]));
            if (!t || !arr || (size_t(off) + count) * 2 > arr->data.size()) return JValue(uint32_t(-3));   // ERROR_BAD_VALUE
            const int16_t* s = reinterpret_cast<const int16_t*>(arr->data.data()) + off;
            if (t->channels == 2) pcm->assign(s, s + count);
            else { pcm->reserve(size_t(count) * 2); for (uint32_t i = 0; i < count; ++i) { pcm->push_back(s[i]); pcm->push_back(s[i]); } }
        }
        auto& mixer = x.proc.mixer();
        const uint64_t frames = count / uint32_t(t->channels);
        if (mixer.queuedCount(t->voice) < 32) mixer.enqueue(t->voice, pcm, t->rate, t->framesWritten);
        mixer.takeProcessed(t->voice);
        t->framesWritten += frames;
        // Block like a real track whose buffer is full: until the audio written is at most
        // ~100 ms ahead of the time it has been playing.
        if (t->playing) {
            for (;;) {
                const double played = std::chrono::duration<double>(std::chrono::steady_clock::now() - t->start).count();
                if (double(t->framesWritten) / t->rate <= played + 0.1 || x.proc.stopping()) break;
                std::this_thread::sleep_for(5ms);
            }
        }
        return JValue(count);
    });

    // EA's audio core: Java creates the track and hands it to the native Init.
    j.method("com/ea/EAAudioCore/AndroidEAAudioCore", "Startup", "()V", [](Jni& j, Ctx& x, jobject cls, const auto&) {
        const jobject t = j.newObject("android/media/AudioTrack");
        j.invoke(x, j.methodId("android/media/AudioTrack", "<init>", "(IIIIII)V", false), t, {3, 44100, 12, 2, 8192, 1});
        j.callNative(x, "com/ea/EAAudioCore/AndroidEAAudioCore", "Init", cls, {t, 2, 2, 44100});
        return JValue(0);
    });
    j.method("com/ea/EAAudioCore/AndroidEAAudioCore", "Startup", "(I)V", [](Jni& j, Ctx& x, jobject cls, const auto& a) {
        const jobject t = j.newObject("android/media/AudioTrack");
        const uint32_t channels = a.empty() ? 2 : uint32_t(a[0]);
        j.invoke(x, j.methodId("android/media/AudioTrack", "<init>", "(IIIIII)V", false), t, {3, 44100, channels == 1 ? 4u : 12u, 2, 8192, 1});
        j.callNative(x, "com/ea/EAAudioCore/AndroidEAAudioCore", "Init", cls, {t, 2, channels, 44100});
        return JValue(0);
    });
    j.method("com/ea/EAAudioCore/AndroidEAAudioCore", "Shutdown", "()V", [](Jni& j, Ctx& x, jobject cls, const auto&) {
        j.callNative(x, "com/ea/EAAudioCore/AndroidEAAudioCore", "Release", cls, {});
        return JValue(0);
    });
}

// ------------------------------------------------------------- the activity ---

namespace {

struct GlSurface {
    uint32_t context = 0, sharegroup = 0, layer = 0;
    uint32_t fbo = 0, color = 0, depth = 0;
};

uint32_t glFn(Process& p, const char* name) {
    auto a = p.lookupSymbol(std::string("_") + name);
    if (!a) throw std::runtime_error(std::string("Android: no GL function ") + name);
    return *a;
}

// The GLSurfaceView: a host GL context and a framebuffer whose color buffer is the drawable.
GlSurface makeSurface(Process& p, CPU::Cpu& c, const Surface& s) {
    if (!HostGL::enabled(p)) throw std::runtime_error("Android games need the GPU renderer (HostGL)");
    GlSurface g;
    g.context = p.allocStatic(16);
    g.sharegroup = p.allocStatic(16);
    g.layer = p.allocStatic(16);
    HostGL::contextCreate(p, g.context, 1, g.sharegroup);
    HostGL::contextMakeCurrent(p, g.context);
    const uint32_t names = p.allocStatic(16);
    p.callGuest(c, glFn(p, "glGenFramebuffersOES"), {1, names});
    p.callGuest(c, glFn(p, "glGenRenderbuffersOES"), {2, names + 4});
    g.fbo = p.mem().read<uint32_t>(names);
    g.color = p.mem().read<uint32_t>(names + 4);
    g.depth = p.mem().read<uint32_t>(names + 8);
    constexpr uint32_t kFb = 0x8D40, kRb = 0x8D41, kColor0 = 0x8CE0, kDepthAtt = 0x8D00, kDepth24 = 0x81A6;
    p.callGuest(c, glFn(p, "glBindFramebufferOES"), {kFb, g.fbo});
    p.callGuest(c, glFn(p, "glBindRenderbufferOES"), {kRb, g.color});
    if (!HostGL::renderbufferStorageFromDrawable(p, g.context, g.layer, s.width, s.height))
        throw std::runtime_error("Android: no drawable storage for the surface");
    p.callGuest(c, glFn(p, "glFramebufferRenderbufferOES"), {kFb, kColor0, kRb, g.color});
    p.callGuest(c, glFn(p, "glBindRenderbufferOES"), {kRb, g.depth});
    p.callGuest(c, glFn(p, "glRenderbufferStorageOES"), {kRb, kDepth24, uint32_t(s.width), uint32_t(s.height)});
    p.callGuest(c, glFn(p, "glFramebufferRenderbufferOES"), {kFb, kDepthAtt, kRb, g.depth});
    p.callGuest(c, glFn(p, "glBindRenderbufferOES"), {kRb, g.color});
    p.callGuest(c, glFn(p, "glViewport"), {0, 0, uint32_t(s.width), uint32_t(s.height)});
    return g;
}

GlSurface* gSurface = nullptr;   // for the framebuffer-0 remap below (one Android process at a time)

// Framebuffer 0 is the surface on Android; here it is our framebuffer object.
void glBindFramebufferRemap(Ctx& x, uint32_t target, uint32_t fb) {
    x.proc.callGuest(x.cpu, glFn(x.proc, "glBindFramebufferOES"), {target, fb == 0 && gSurface ? gSurface->fbo : fb});
}
void glGetIntegervRemap(Ctx& x, uint32_t pname, GuestPtr out) {
    x.proc.callGuest(x.cpu, glFn(x.proc, "glGetIntegerv"), {pname, out});
    if (pname == 0x8CA6 && gSurface && x.mem.read<uint32_t>(out) == gSurface->fbo) x.mem.write<uint32_t>(out, 0);
}

// Where the surface sits on the emulated (portrait) screen. A landscape surface is shown as
// UIInterfaceOrientationLandscapeRight: surface point (x, y) at screen point (W - y, x).
void placeSurface(Process& p, GlSurface& g, const Surface& s) {
    const float W = float(p.config().screenWidth), sc = s.scale;
    const int sw = int(W * sc), sh = int(float(p.config().screenHeight) * sc);
    const float bw = float(s.width) / sc, bh = float(s.height) / sc;
    if (s.portrait) {
        const float m[6] = {sc, 0, 0, sc, 0, 0};
        HostGL::layerPlaced(p, g.layer, m, 0, 0, bw, bh, sw, sh);
    } else {
        const float m[6] = {0, sc, -sc, 0, W * sc, 0};
        HostGL::layerPlaced(p, g.layer, m, 0, 0, bw, bh, sw, sh);
    }
}

void present(Process& p, CPU::Cpu& c, GlSurface& g, const Surface& s) {
    uint32_t layer = 0;
    int w = 0, h = 0;
    bool direct = false;
    std::vector<uint8_t> rgba;
    // The drawable must be the bound renderbuffer while it's presented.
    static const char key = 0;
    uint32_t& saved = p.processSlot(&key);
    if (!saved) saved = p.allocStatic(4);
    p.callGuest(c, glFn(p, "glGetIntegerv"), {0x8CA7, saved});
    p.callGuest(c, glFn(p, "glBindRenderbufferOES"), {0x8D41, g.color});
    const bool ok = HostGL::presentRenderbuffer(p, g.context, layer, w, h, rgba, direct);
    p.callGuest(c, glFn(p, "glBindRenderbufferOES"), {0x8D41, p.mem().read<uint32_t>(saved)});
    if (!ok) return;
    if (direct) {
        static const char placedKey = 0;
        if (!p.processSlot(&placedKey)) { placeSurface(p, g, s); p.processSlot(&placedKey) = 1; }
    } else if (p.config().present && w > 0 && h > 0 && rgba.size() >= size_t(w) * size_t(h) * 4) {
        // GL rows are bottom-up. The bench (no screen) keeps the surface as it is; the app's
        // screen is portrait, so a landscape surface is turned the way placeSurface puts it.
        const bool turn = !s.portrait && p.config().rotateAndroidReadback;
        std::vector<uint8_t> out(rgba.size());
        if (!turn) {
            const size_t row = size_t(w) * 4;
            for (int y = 0; y < h; ++y) std::memcpy(out.data() + size_t(y) * row, rgba.data() + size_t(h - 1 - y) * row, row);
            p.config().present(out.data(), w, h);
        } else {
            // Portrait pixel (px, py) shows surface pixel (py, h - 1 - px) (top-down rows).
            const uint32_t* src = reinterpret_cast<const uint32_t*>(rgba.data());
            uint32_t* dst = reinterpret_cast<uint32_t*>(out.data());
            for (int py = 0; py < w; ++py)
                for (int px = 0; px < h; ++px) dst[size_t(py) * size_t(h) + size_t(px)] = src[size_t(px) * size_t(w) + size_t(py)];
            p.config().present(out.data(), h, w);
        }
    }
    p.perf().notePresented();
}

struct Pointer { int action; float x, y; uint32_t id; };

} // namespace

void registerSurfaceFunctions(Process& p) {
    // Framebuffer 0 and its binding query go to the surface.
    p.defineFunction("android:glBindFramebufferOES", [](Ctx& x) { callWrapped<&glBindFramebufferRemap>(x); });
    p.defineFunction("android:glGetIntegerv", [](Ctx& x) { callWrapped<&glGetIntegervRemap>(x); });
}

int runActivity(Process& p, CPU::Cpu& c) {
    Jni& j = jni(p);
    Ctx x{p, c, p.mem()};
    const auto& lib = p.service<Library>();
    const jobject activity = jobject(j.staticFields["hvm.activity"]);
    const Surface s = surfaceFor(p);
    {   // The player follows UIKit's screen and orientation; there is no UIKit app to set them.
        auto& u = UIKit::ui(p);
        u.screenW = float(p.config().screenWidth);
        u.screenH = float(p.config().screenHeight);
        u.scale = s.scale;
        u.statusBarOrientation = s.portrait ? 1 : 3;   // UIInterfaceOrientationLandscapeRight
    }
    p.logf(LogLevel::Info, "Android: %s, surface %dx%d", lib.name.c_str(), s.width, s.height);

    if (auto it = lib.so.image.symbols.find("JNI_OnLoad"); it != lib.so.image.symbols.end()) {
        const uint32_t v = uint32_t(p.callGuest(c, it->second, {j.vm, 0}));
        p.logf(LogLevel::Info, "Android: JNI_OnLoad returned 0x%x", v);
    }
    // Native classes the activity's Java code would start before the game.
    j.callNative(x, "com/ea/EAThread/EAThread", "Init", j.classFor("com/ea/EAThread/EAThread"), {});
    j.callNative(x, "com/ea/EAIO/EAIO", "Startup", j.classFor("com/ea/EAIO/EAIO"), {uint32_t(j.staticFields["hvm.assets"])});

    auto activityCall = [&](const char* name, std::vector<uint32_t> words = {}) {
        if (!j.callNative(x, "com/ea/blast/MainActivity", name, activity, words))
            p.logf(LogLevel::Warning, "Android: the game has no MainActivity.%s", name);
    };
    // The activity's onCreate starts EA's audio core (it makes the AudioTrack and hands it to
    // the native Init) before the game's own NativeOnCreate.
    if (lib.so.image.symbols.count("Java_com_ea_EAAudioCore_AndroidEAAudioCore_Init")) {
        const jobject audioCls = j.classFor("com/ea/EAAudioCore/AndroidEAAudioCore");
        j.invoke(x, j.methodId("com/ea/EAAudioCore/AndroidEAAudioCore", "Startup", "()V", true), audioCls, {});
    }
    activityCall("NativeOnCreate");
    activityCall("NativeOnResume");

    GlSurface g = makeSurface(p, c, s);
    gSurface = &g;
    const jobject renderer = j.newObject("com/ea/blast/AndroidRenderer");
    j.callNative(x, "com/ea/blast/AndroidRenderer", "NativeOnSurfaceCreated", renderer, {});
    j.callNative(x, "com/ea/blast/AndroidRenderer", "NativeOnSurfaceChanged", renderer, {uint32_t(s.width), uint32_t(s.height)});
    activityCall("NativeOnWindowFocusChanged", {1});

    // Touches: the host's are in portrait screen points; the surface is landscape pixels.
    std::mutex inMu;
    std::deque<Pointer> pending;
    const jobject touch = j.classFor("com/ea/blast/TouchSurfaceAndroid");   // its natives are static
    auto rawId = [&](const char* name) {
        JValue v = 0;
        j.callNative(x, "com/ea/blast/TouchSurfaceAndroid", name, touch, {}, &v);
        return int(uint32_t(v));
    };
    const int idDown = rawId("NativeGetIdRawPointerDown"), idMove = rawId("NativeGetIdRawPointerMove");
    const int idUp = rawId("NativeGetIdRawPointerUp"), idCancel = rawId("NativeGetIdRawPointerCancel");
    JValue touchModule = 0;
    j.callNative(x, "com/ea/blast/ModuleCatalog", "NativeGetModuleTypeIdTouchScreen", j.classFor("com/ea/blast/ModuleCatalog"), {}, &touchModule);
    const float portraitW = float(p.config().screenWidth);
    p.setTouchHandler([&, portraitW](TouchPhase phase, float tx, float ty, uint32_t finger) {
        const int action = phase == TouchPhase::Began ? idDown : phase == TouchPhase::Moved ? idMove
                         : phase == TouchPhase::Cancelled ? idCancel : idUp;
        std::lock_guard<std::mutex> lk(inMu);
        if (s.portrait) pending.push_back({action, tx * s.scale, ty * s.scale, finger});
        else pending.push_back({action, ty * s.scale, (portraitW - tx) * s.scale, finger});
    });

    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    bool paused = false;
    while (!p.stopping()) {
        if (p.backgroundRequested() && !paused) {
            activityCall("NativeOnWindowFocusChanged", {0});
            activityCall("NativeOnPause");
            paused = true;
            p.setBackgrounded(true);
        }
        if (paused) {
            p.waitWhileBackgrounded();
            if (p.stopping()) break;
            activityCall("NativeOnResume");
            activityCall("NativeOnWindowFocusChanged", {1});
            paused = false;
            next = clock::now();
        }
        std::deque<Pointer> events;
        {
            std::lock_guard<std::mutex> lk(inMu);
            events.swap(pending);
        }
        for (const auto& e : events)
            j.callNative(x, "com/ea/blast/TouchSurfaceAndroid", "NativeOnPointerEvent", touch,
                         {uint32_t(e.action), uint32_t(touchModule), e.id, uint32_t(jfloat(e.x)), uint32_t(jfloat(e.y))});
        j.callNative(x, "com/ea/blast/AndroidRenderer", "NativeOnDrawFrame", renderer, {});
        present(p, c, g, s);
        JValue exitCode = 0;
        if (j.callNative(x, "com/ea/blast/MainActivity", "NativeGetExitCode", activity, {}, &exitCode) && int32_t(exitCode) > 0) {
            p.logf(LogLevel::Info, "Android: the game asked to exit (%d)", int32_t(exitCode));
            break;
        }
        next += std::chrono::microseconds(16667);
        const auto now = clock::now();
        if (next > now) std::this_thread::sleep_until(next);
        else if (now - next > 100ms) next = now;
    }
    gSurface = nullptr;
    p.setTouchHandler({});
    return 0;
}

} // namespace Hydrogen::Runtime::Android
