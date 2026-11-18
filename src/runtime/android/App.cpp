// HydrogenVM — an Android game's activity: the Java classes its native code calls, and the
// lifecycle and frame loop a GLSurfaceView would drive. See Android.hpp.
#include "runtime/android/BionicInternal.hpp"
#include "runtime/android/Jni.hpp"
#include "runtime/android/Surface.hpp"
#include "runtime/audio/Audio.hpp"
#include "runtime/gl/HostGL.hpp"
#include "runtime/uikit/UIKit.hpp"
#include "loader/Apk.hpp"
#include "loader/Zip.hpp"
#include "core/cpu/Engine.hpp"
#include <algorithm>
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


namespace {

using namespace std::chrono_literals;

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
    // Relative links, so they survive the app's data folder moving (iOS can give an app a new
    // container path); a link left from an earlier install that no longer leads to the assets is
    // made again.
    for (auto& e : fs::directory_iterator(assetsDir(p), ec)) {
        const fs::path link = files / e.path().filename();
        const fs::path target = fs::relative(e.path(), files, ec);
        const fs::file_status st = fs::symlink_status(link, ec);
        if (fs::is_symlink(st)) {
            std::error_code rc;
            if (fs::read_symlink(link, rc) == (target.empty() ? e.path() : target) && fs::exists(link, rc)) continue;
            fs::remove(link, rc);
        } else if (fs::exists(st)) {
            continue;   // the game's own file or folder
        }
        fs::create_symlink(target.empty() ? e.path() : target, link, ec);
    }
}
std::string externalRoot(Process& p) { return p.homePath() + "/Library/Caches/sdcard"; }
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
    // Once per game run: the app keeps running between games, and a game re-imported in the
    // same session has a fresh folder to link again.
    struct Linked { explicit Linked(Process&) {} std::mutex mu; std::set<std::string> pkgs; };
    auto& l = p.service<Linked>();
    std::lock_guard<std::mutex> lk(l.mu);
    if (l.pkgs.insert(pkg).second) linkExternalFiles(p, fs::path(prefix) / pkg / "files");
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
    j.staticFields["com/unity3d/player/UnityPlayer.currentActivity"] = activity;   // Unity's AndroidJavaClass reads it
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
    auto listAssets = [](Jni& j, Ctx& x, jobject, const std::vector<JValue>& a) {
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
    };
    j.method(kAM, "list", "(Ljava/lang/String;)[Ljava/lang/String;", listAssets);
    j.method(kAct, "listDir", "(Ljava/lang/String;)[Ljava/lang/String;", listAssets);
    j.method("android/app/Activity", "listDir", "(Ljava/lang/String;)[Ljava/lang/String;", listAssets);
    j.method(kAct, "externalPermitted", "()Z", [](Jni&, Ctx&, jobject, const auto&) { return JValue(1); });
    j.method("android/app/Activity", "externalPermitted", "()Z", [](Jni&, Ctx&, jobject, const auto&) { return JValue(1); });
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
        const std::string host = p.homePath() + "/Documents";
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

    // Ubisoft's mobile SDK and UAF activity: offline answers.
    const std::string kUafAct = "com/ubisoft/uaf/GameActivity";
    j.method(kUafAct, "GetMacAddress", "()Ljava/lang/String;", [](Jni& j, Ctx&, jobject, const auto&) { return JValue(j.newString("02:00:00:00:00:00")); });
    for (const char* n : {"isTwitterAvailable", "isGamePadConnected", "isFacebookAvailable", "isNetworkAvailable", "isGooglePlayServicesAvailable"})
        j.method(kUafAct, n, "()Z", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kUafAct, "isEveryPlayAvailable", "()Z", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    // Generic Java calls by name: battery questions are the ones asked; everything else answers 0.
    j.method(kUafAct, "CallGenericJavaMethod_Int", "(Ljava/lang/String;I)I", [](Jni& j, Ctx& x, jobject, const auto& a) {
        const std::string name = j.string(jobject(a[0]));
        if (name == "getBatteryLevel") return JValue(100);
        if (name == "getBatteryChargingMode") return JValue(1);
        x.proc.reportDefaulted("UAF CallGenericJavaMethod_Int(\"" + name + "\") -> 0");
        return JValue(0);
    });
    const std::string kUbiUtils = "ubisoft/mobile/mobileSDK/Utils";
    j.method(kUbiUtils, "getDeviceGameVersion", "()Ljava/lang/String;", [&p](Jni& j, Ctx&, jobject, const auto&) {
        const auto* info = p.infoPlist();
        return JValue(j.newString(info ? info->getString("CFBundleShortVersionString", "1.0") : "1.0"));
    });
    // No account was ever stored on this device: the SDK's keychain answers null.
    j.method("ubisoft/mobile/mobileAuth/MobileAuth", "keyChainGetDataForKey", "(Ljava/lang/String;)Ljava/lang/String;",
             [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    for (const char* cls : {"ubisoft/mobile/mobileSDK/Utils", "ubisoft/mobile/mobileAuth/MobileAuth"}) j.quietClasses.insert(cls);

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

    // SDL2 / SDL3 Java activity and audio manager callbacks.
    const std::string kSdlAct = "org/libsdl/app/SDLActivity", kSdlAud = "org/libsdl/app/SDLAudioManager";
    for (const char* sig : {"()Landroid/content/Context;", "()Landroid/app/Activity;"})
        j.method(kSdlAct, "getContext", sig, [activity](Jni&, Ctx&, jobject, const auto&) { return JValue(activity); });
    j.method(kSdlAct, "getNativeSurface", "()Landroid/view/Surface;", [](Jni& j, Ctx&, jobject, const auto&) {
        auto& s = j.staticFields["hvm.sdl.surface"];
        if (!s) {
            const jobject surf = j.newObject("android/view/Surface");
            if (JObject* o = j.get(surf)) o->permanent = true;
            s = surf;
        }
        return JValue(s);
    });
    j.method(kSdlAct, "getDisplayDPI", "()Landroid/util/DisplayMetrics;", [](Jni& j, Ctx&, jobject, const auto&) {
        const jobject dm = j.newObject("android/util/DisplayMetrics");
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(dm)) {
            o->fields["xdpi"] = jfloat(160.0f);
            o->fields["ydpi"] = jfloat(160.0f);
            o->fields["density"] = jfloat(1.0f);
            o->fields["scaledDensity"] = jfloat(1.0f);
            o->fields["densityDpi"] = 160;
        }
        return JValue(dm);
    });
    for (const auto& [name, sig] : std::vector<std::pair<std::string, std::string>>{
             {"getManifestEnvironmentVariables", "()Z"},
             {"setActivityTitle", "(Ljava/lang/String;)Z"},
             {"sendMessage", "(II)Z"},
             {"createCustomCursor", "([IIIII)I"},
             {"setCustomCursor", "(I)Z"},
             {"setSystemCursor", "(I)Z"}})
        j.method(kSdlAct, name, sig, [](Jni&, Ctx&, jobject, const auto&) { return JValue(1); });
    j.method(kSdlAct, "getPreferredLocales", "()Ljava/lang/String;", [](Jni& j, Ctx&, jobject, const auto&) {
        return JValue(j.newString("en_US"));
    });
    j.method(kSdlAct, "requestPermission", "(Ljava/lang/String;I)V", [](Jni& j, Ctx& x, jobject cls, const auto& a) {
        j.callNative(x, "org/libsdl/app/SDLActivity", "nativePermissionResult", cls, {a.size() > 1 ? uint32_t(a[1]) : 0u, 1u});
        return JValue(0);
    });
    j.method(kSdlAud, "audioOpen", "(IIII)[I", [](Jni& j, Ctx&, jobject, const auto& a) {
        const jobject arr = j.newArray('I', 4);
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(arr); o && o->data.size() >= 16) {
            uint32_t* w = reinterpret_cast<uint32_t*>(o->data.data());
            w[0] = a.size() > 0 && a[0] ? uint32_t(a[0]) : 44100u;
            w[1] = a.size() > 1 ? uint32_t(a[1]) : 1u;
            w[2] = a.size() > 2 && a[2] ? uint32_t(a[2]) : 2u;
            w[3] = a.size() > 3 && a[3] ? uint32_t(a[3]) : 1024u;
        }
        return JValue(arr);
    });
    for (const char* sig : {"(IZII)[I", "(IIII)[I"})
        j.method(kSdlAud, "captureOpen", sig, [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    for (const char* cls : {"org/libsdl/app/SDLActivity", "org/libsdl/app/SDLAudioManager",
                            "org/libsdl/app/SDLControllerManager", "org/libsdl/app/SDLInputConnection",
                            "org/libsdl/app/HIDDeviceManager", "org/libsdl/app/SDL"})
        j.quietClasses.insert(cls);
}

// ------------------------------------------------------------- the activity ---


uint32_t glFn(Process& p, const char* name) {
    auto a = p.lookupSymbol(std::string("_") + name);
    if (!a) throw std::runtime_error(std::string("Android: no GL function ") + name);
    return *a;
}

// Framebuffer functions: the OES extension names in ES 1.1, the core names in ES 2.
std::string fbName(int api, const char* core) { return api >= 2 ? std::string(core) : std::string(core) + "OES"; }
uint32_t glFn(Process& p, const std::string& name) { return glFn(p, name.c_str()); }

int linkedGlesApi(Process& p) {
    auto& L = p.service<Library>();
    return L.needs("libGLESv2.so") || L.needs("libGLESv3.so") ? 2 : 1;
}

void createContext(Process& p, GlSurface& g, int api, uint32_t shareSharegroup) {
    if (!HostGL::enabled(p)) throw std::runtime_error("Android games need the GPU renderer (HostGL)");
    g.api = api;
    g.context = p.allocStatic(16);
    g.sharegroup = shareSharegroup ? shareSharegroup : p.allocStatic(16);
    HostGL::contextCreate(p, g.context, api, g.sharegroup);
}

void attachDrawable(Process& p, CPU::Cpu& c, GlSurface& g, int w, int h, bool depthStencil) {
    g.layer = p.allocStatic(16);
    g.width = w;
    g.height = h;
    const int api = g.api;
    const uint32_t names = p.allocStatic(16);
    p.callGuest(c, glFn(p, fbName(api, "glGenFramebuffers")), {1, names});
    p.callGuest(c, glFn(p, fbName(api, "glGenRenderbuffers")), {2, names + 4});
    g.fbo = p.mem().read<uint32_t>(names);
    g.color = p.mem().read<uint32_t>(names + 4);
    g.depth = p.mem().read<uint32_t>(names + 8);
    constexpr uint32_t kFb = 0x8D40, kRb = 0x8D41, kColor0 = 0x8CE0, kDepthAtt = 0x8D00, kStencilAtt = 0x8D20;
    constexpr uint32_t kDepth24 = 0x81A6, kDepth24Stencil8 = 0x88F0;
    p.callGuest(c, glFn(p, fbName(api, "glBindFramebuffer")), {kFb, g.fbo});
    p.callGuest(c, glFn(p, fbName(api, "glBindRenderbuffer")), {kRb, g.color});
    if (!HostGL::renderbufferStorageFromDrawable(p, g.context, g.layer, w, h))
        throw std::runtime_error("Android: no drawable storage for the surface");
    p.callGuest(c, glFn(p, fbName(api, "glFramebufferRenderbuffer")), {kFb, kColor0, kRb, g.color});
    if (depthStencil) {
        // Depth with stencil where the context has packed depth-stencil (ES 2 games use stencil
        // for masks); ES 1.1 surfaces keep the 24-bit depth they always had.
        p.callGuest(c, glFn(p, fbName(api, "glBindRenderbuffer")), {kRb, g.depth});
        const uint32_t fmt = api >= 2 ? kDepth24Stencil8 : kDepth24;
        p.callGuest(c, glFn(p, fbName(api, "glRenderbufferStorage")), {kRb, fmt, uint32_t(w), uint32_t(h)});
        p.callGuest(c, glFn(p, fbName(api, "glFramebufferRenderbuffer")), {kFb, kDepthAtt, kRb, g.depth});
        if (api >= 2) p.callGuest(c, glFn(p, fbName(api, "glFramebufferRenderbuffer")), {kFb, kStencilAtt, kRb, g.depth});
    }
    p.callGuest(c, glFn(p, fbName(api, "glBindRenderbuffer")), {kRb, g.color});
    p.callGuest(c, glFn(p, "glViewport"), {0, 0, uint32_t(w), uint32_t(h)});
}

// The GLSurfaceView: a host GL context of the version the game links and its window framebuffer.
GlSurface makeSurface(Process& p, CPU::Cpu& c, const Surface& s) {
    GlSurface g;
    createContext(p, g, linkedGlesApi(p));
    HostGL::contextMakeCurrent(p, g.context);
    attachDrawable(p, c, g, s.width, s.height);
    return g;
}

// The window surface current on each thread: its framebuffer stands for framebuffer 0.
namespace { thread_local GlSurface* tCurrent = nullptr; }
void setCurrentSurface(GlSurface* g) { tCurrent = g; }
GlSurface* currentSurface() { return tCurrent; }

// Framebuffer 0 is the surface on Android; here it is our framebuffer object.
void glBindFramebufferRemap(Ctx& x, uint32_t target, uint32_t fb) {
    x.proc.callGuest(x.cpu, glFn(x.proc, "glBindFramebufferOES"), {target, fb == 0 && tCurrent && tCurrent->fbo ? tCurrent->fbo : fb});
}
void glBindFramebufferCoreRemap(Ctx& x, uint32_t target, uint32_t fb) {
    x.proc.callGuest(x.cpu, glFn(x.proc, "glBindFramebuffer"), {target, fb == 0 && tCurrent && tCurrent->fbo ? tCurrent->fbo : fb});
}
void glGetIntegervRemap(Ctx& x, uint32_t pname, GuestPtr out) {
    x.proc.callGuest(x.cpu, glFn(x.proc, "glGetIntegerv"), {pname, out});
    if (pname == 0x8CA6 && tCurrent && tCurrent->fbo && x.mem.read<uint32_t>(out) == tCurrent->fbo) x.mem.write<uint32_t>(out, 0);
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

// An Android window surface is opaque: the compositor ignores the alpha a game leaves in it, and
// many games clear with alpha 0. Here the drawable is composited with its alpha, so it is set to
// 1 before each present (colour writes masked to alpha only), and the game's state restored.
void makeOpaque(Process& p, CPU::Cpu& c, GlSurface& g) {
    static const char key = 0;
    uint32_t& scratch = p.processSlot(&key);
    if (!scratch) scratch = p.allocStatic(64);
    auto& m = p.mem();
    p.callGuest(c, glFn(p, "glGetIntegerv"), {0x8CA6 /*FRAMEBUFFER_BINDING*/, scratch});
    p.callGuest(c, glFn(p, "glGetBooleanv"), {0x0C23 /*COLOR_WRITEMASK*/, scratch + 4});
    p.callGuest(c, glFn(p, "glGetFloatv"), {0x0C22 /*COLOR_CLEAR_VALUE*/, scratch + 8});
    const uint32_t scissor = uint32_t(p.callGuest(c, glFn(p, "glIsEnabled"), {0x0C11 /*SCISSOR_TEST*/})) & 0xFF;
    const uint32_t fb = m.read<uint32_t>(scratch);
    p.callGuest(c, glFn(p, fbName(g.api, "glBindFramebuffer")), {0x8D40, g.fbo});
    if (scissor) p.callGuest(c, glFn(p, "glDisable"), {0x0C11});
    p.callGuest(c, glFn(p, "glColorMask"), {0, 0, 0, 1});
    const uint32_t one = 0x3F800000u, zero = 0;
    p.callGuest(c, glFn(p, "glClearColor"), {zero, zero, zero, one});
    p.callGuest(c, glFn(p, "glClear"), {0x4000 /*COLOR_BUFFER_BIT*/});
    p.callGuest(c, glFn(p, "glClearColor"), {m.read<uint32_t>(scratch + 8), m.read<uint32_t>(scratch + 12),
                                             m.read<uint32_t>(scratch + 16), m.read<uint32_t>(scratch + 20)});
    p.callGuest(c, glFn(p, "glColorMask"), {m.read<uint8_t>(scratch + 4), m.read<uint8_t>(scratch + 5),
                                            m.read<uint8_t>(scratch + 6), m.read<uint8_t>(scratch + 7)});
    if (scissor) p.callGuest(c, glFn(p, "glEnable"), {0x0C11});
    p.callGuest(c, glFn(p, fbName(g.api, "glBindFramebuffer")), {0x8D40, fb});
}

void present(Process& p, CPU::Cpu& c, GlSurface& g, const Surface& s) {
    makeOpaque(p, c, g);
    uint32_t layer = 0;
    int w = 0, h = 0;
    bool direct = false;
    std::vector<uint8_t> rgba;
    // The drawable must be the bound renderbuffer while it's presented.
    static const char key = 0;
    uint32_t& saved = p.processSlot(&key);
    if (!saved) saved = p.allocStatic(4);
    p.callGuest(c, glFn(p, "glGetIntegerv"), {0x8CA7, saved});
    p.callGuest(c, glFn(p, fbName(g.api, "glBindRenderbuffer")), {0x8D41, g.color});
    const bool ok = HostGL::presentRenderbuffer(p, g.context, layer, w, h, rgba, direct);
    p.callGuest(c, glFn(p, fbName(g.api, "glBindRenderbuffer")), {0x8D41, p.mem().read<uint32_t>(saved)});
    if (!ok) return;
    if (direct) {
        if (!g.placed) { placeSurface(p, g, s); g.placed = true; }
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

void registerSurfaceFunctions(Process& p) {
    // Framebuffer 0 and its binding query go to the surface.
    p.defineFunction("android:glBindFramebufferOES", [](Ctx& x) { callWrapped<&glBindFramebufferRemap>(x); });
    p.defineFunction("android:glBindFramebuffer", [](Ctx& x) { callWrapped<&glBindFramebufferCoreRemap>(x); });
    p.defineFunction("android:glGetIntegerv", [](Ctx& x) { callWrapped<&glGetIntegervRemap>(x); });
}

namespace {

// ------------------------------------------------------------ engine drivers ---
// What a game's Java activity does around the native code differs per engine. A driver plays
// that part: start-up before and after the GL surface exists, one frame, a touch, pause and
// resume. The shared loop (surface, presenting, pacing, input queue) is runActivity's.

struct Driver {
    virtual ~Driver() = default;
    virtual const char* name() const = 0;
    virtual void create(Ctx&) {}                                       // activity onCreate / onResume
    virtual void surfaceReady(Ctx&, const Surface&) {}                 // GL context current, surface sized
    virtual void frame(Ctx&) = 0;
    virtual void touch(Ctx&, TouchPhase, float x, float y, uint32_t id, uint32_t activeCount) = 0;
    virtual bool key(Ctx&, int32_t /*keyCode*/, bool /*down*/) { return false; }
    virtual void pause(Ctx&) {}
    virtual void resume(Ctx&) {}
    virtual bool wantsExit(Ctx&) { return false; }
    // The engine sets up EGL on the window itself and presents with eglSwapBuffers (Egl.cpp);
    // otherwise HydrogenVM makes the GL context and presents after each frame, as GLSurfaceView.
    virtual bool ownsGl() const { return false; }
};

// EA's Blast framework (Dead Space and other EA games of 2011-2013).
struct EaBlastDriver final : Driver {
    Jni& j;
    jobject activity = 0, renderer = 0, touchCls = 0;
    int idDown = 0, idMove = 0, idUp = 0, idCancel = 0;
    JValue touchModule = 0;
    explicit EaBlastDriver(Jni& jni) : j(jni) {}
    const char* name() const override { return "EA Blast"; }
    static bool matches(const Library& lib) { return lib.find("Java_com_ea_blast_MainActivity_NativeOnCreate").has_value(); }
    void call(Ctx& x, const char* n, std::vector<uint32_t> w = {}) {
        if (!j.callNative(x, "com/ea/blast/MainActivity", n, activity, w))
            x.proc.logf(LogLevel::Warning, "Android: the game has no MainActivity.%s", n);
    }
    void create(Ctx& x) override {
        activity = jobject(j.staticFields["hvm.activity"]);
        const auto& lib = x.proc.service<Library>();
        j.callNative(x, "com/ea/EAThread/EAThread", "Init", j.classFor("com/ea/EAThread/EAThread"), {});
        j.callNative(x, "com/ea/EAIO/EAIO", "Startup", j.classFor("com/ea/EAIO/EAIO"), {uint32_t(j.staticFields["hvm.assets"])});
        // The activity's onCreate starts EA's audio core (it makes the AudioTrack and hands it to
        // the native Init) before the game's own NativeOnCreate.
        if (lib.find("Java_com_ea_EAAudioCore_AndroidEAAudioCore_Init"))
            j.invoke(x, j.methodId("com/ea/EAAudioCore/AndroidEAAudioCore", "Startup", "()V", true),
                     j.classFor("com/ea/EAAudioCore/AndroidEAAudioCore"), {});
        call(x, "NativeOnCreate");
        call(x, "NativeOnResume");
    }
    void surfaceReady(Ctx& x, const Surface& s) override {
        renderer = j.newObject("com/ea/blast/AndroidRenderer");
        j.callNative(x, "com/ea/blast/AndroidRenderer", "NativeOnSurfaceCreated", renderer, {});
        j.callNative(x, "com/ea/blast/AndroidRenderer", "NativeOnSurfaceChanged", renderer, {uint32_t(s.width), uint32_t(s.height)});
        call(x, "NativeOnWindowFocusChanged", {1});
        touchCls = j.classFor("com/ea/blast/TouchSurfaceAndroid");   // its natives are static
        auto rawId = [&](const char* n) { JValue v = 0; j.callNative(x, "com/ea/blast/TouchSurfaceAndroid", n, touchCls, {}, &v); return int(uint32_t(v)); };
        idDown = rawId("NativeGetIdRawPointerDown"); idMove = rawId("NativeGetIdRawPointerMove");
        idUp = rawId("NativeGetIdRawPointerUp"); idCancel = rawId("NativeGetIdRawPointerCancel");
        j.callNative(x, "com/ea/blast/ModuleCatalog", "NativeGetModuleTypeIdTouchScreen", j.classFor("com/ea/blast/ModuleCatalog"), {}, &touchModule);
    }
    void frame(Ctx& x) override { j.callNative(x, "com/ea/blast/AndroidRenderer", "NativeOnDrawFrame", renderer, {}); }
    void touch(Ctx& x, TouchPhase ph, float px, float py, uint32_t id, uint32_t) override {
        const int action = ph == TouchPhase::Began ? idDown : ph == TouchPhase::Moved ? idMove : ph == TouchPhase::Cancelled ? idCancel : idUp;
        j.callNative(x, "com/ea/blast/TouchSurfaceAndroid", "NativeOnPointerEvent", touchCls,
                     {uint32_t(action), uint32_t(touchModule), id, uint32_t(jfloat(px)), uint32_t(jfloat(py))});
    }
    void pause(Ctx& x) override { call(x, "NativeOnWindowFocusChanged", {0}); call(x, "NativeOnPause"); }
    void resume(Ctx& x) override { call(x, "NativeOnResume"); call(x, "NativeOnWindowFocusChanged", {1}); }
    bool wantsExit(Ctx& x) override {
        JValue code = 0;
        return j.callNative(x, "com/ea/blast/MainActivity", "NativeGetExitCode", activity, {}, &code) && int32_t(code) > 0;
    }
};

// Ubisoft's UAF framework (Rayman Adventures and other Ubisoft mobile games of 2015-2017):
// GameActivity.initEngine, then the GL2JNIView renderer's init and step.
struct UbisoftUafDriver final : Driver {
    Jni& j;
    jobject lib = 0;
    explicit UbisoftUafDriver(Jni& jni) : j(jni) {}
    const char* name() const override { return "Ubisoft UAF"; }
    static bool matches(const Library& l) { return l.find("Java_com_ubisoft_uaf_UAFJNILib_step").has_value(); }
    void call(Ctx& x, const char* n, std::vector<uint32_t> w = {}) {
        if (!j.callNative(x, "com/ubisoft/uaf/UAFJNILib", n, lib, w))
            x.proc.logf(LogLevel::Warning, "Android: the game has no UAFJNILib.%s", n);
    }
    void generic(Ctx& x, const char* method, const std::string& text, int32_t i = 0) {
        call(x, "callGenericNativeMethod", {j.newString(method), uint32_t(i), uint32_t(jfloat(0)), j.newString(text)});
    }
    void create(Ctx& x) override {
        lib = j.classFor("com/ubisoft/uaf/UAFJNILib");
        const jobject activity = jobject(j.staticFields["hvm.activity"]), assets = jobject(j.staticFields["hvm.assets"]);
        call(x, "InitGlobal", {activity});
        call(x, "InitMobileSDK");
        generic(x, "setDeviceModel", "hydrogenvm");
        generic(x, "setDeviceCodeName", "hydrogenvm");
        generic(x, "setOSVersion", "Android 19");
        call(x, "setupDeviceInfo", {j.newString("hydrogenvm"), j.newString("hydrogenvm"), j.newString("19"), j.newString("en"), assets});
    }
    // Paths as GL2JNIView.init makes them: the app's external files folder, and the main OBB
    // (Android/obb/<package>/<main file>) — here the .obb shipped inside the APK's assets.
    std::string externalFiles, obbFile;
    void paths(Ctx& x) {
        if (!obbFile.empty()) return;
        std::error_code ec;
        const std::string pkg = x.proc.infoPlist() ? x.proc.infoPlist()->getString("CFBundleIdentifier") : "app";
        const fs::path sd = externalRoot(x.proc);
        const fs::path files = sd / "Android/data" / pkg / "files", obbDir = sd / "Android/obb" / pkg;
        fs::create_directories(files, ec);
        fs::create_directories(obbDir, ec);
        for (auto& e : fs::directory_iterator(assetsDir(x.proc), ec)) {
            if (e.path().extension() != ".obb") continue;
            const fs::path link = obbDir / e.path().filename();
            if (!fs::exists(fs::symlink_status(link, ec))) fs::create_symlink(e.path(), link, ec);
            if (obbFile.empty() || e.path().filename().string().rfind("main.", 0) == 0) obbFile = x.proc.guestPath(link.string());
        }
        externalFiles = x.proc.guestPath(files.string());
        // PackInstaller: the first launch unpacks the main OBB (a zip) into the files folder,
        // which is where the engine reads its bundles from.
        const fs::path marker = files / ".hvm-obb-unpacked";
        if (!obbFile.empty() && !fs::exists(marker, ec)) {
            const std::string host = x.proc.hostPath(obbFile);
            try {
                Loader::ZipReader zip(host);
                const size_t n = zip.extract("", files.string(), true);
                std::ofstream(marker) << fs::path(host).filename().string() << "\n";
                x.proc.logf(LogLevel::Info, "Android: unpacked %zu files of %s into the game's files folder", n,
                            fs::path(host).filename().c_str());
            } catch (const std::exception& e) {
                x.proc.logf(LogLevel::Warning, "Android: couldn't unpack the OBB: %s", e.what());
            }
        }
    }
    void surfaceReady(Ctx& x, const Surface& s) override {
        const jobject assets = jobject(j.staticFields["hvm.assets"]);
        paths(x);
        generic(x, "setAppRootPath", externalFiles);
        call(x, "initNativeEngine", {j.newString("en_US"), j.newString(externalFiles), j.newString(obbFile), assets});
        call(x, "init", {uint32_t(s.width), uint32_t(s.height), j.newString("en_US"), 0});
        call(x, "setNativeScreenResolution", {uint32_t(s.width), uint32_t(s.height)});
        call(x, "setDeviceOrientation", {uint32_t(s.width), uint32_t(s.height)});
        call(x, "setDeviceTouchOrientation", {uint32_t(s.width), uint32_t(s.height)});
        call(x, "resume");
    }
    void frame(Ctx& x) override { call(x, "step"); }
    // touch(x, y, prevX, prevY, action, pointerId, pointerCount); actions as MotionEvent's
    // (0 down, 1 up, 2 move, 3 cancel).
    void touch(Ctx& x, TouchPhase ph, float px, float py, uint32_t id, uint32_t count) override {
        const uint32_t action = ph == TouchPhase::Began ? 0 : ph == TouchPhase::Moved ? 2 : ph == TouchPhase::Cancelled ? 3 : 1;
        call(x, "touch", {uint32_t(jfloat(px)), uint32_t(jfloat(py)), uint32_t(jfloat(px)), uint32_t(jfloat(py)), action, id, count});
    }
    void pause(Ctx& x) override { call(x, "pause"); }
    void resume(Ctx& x) override { call(x, "resume"); }
};

// Unity 4/5 (UnityPlayer + NativeLoader): libmain loads Mono and Unity and registers the
// player's natives; Unity makes its own EGL context on the window and renders in nativeRender.
struct UnityDriver final : Driver {
    Jni& j;
    jobject player = 0;
    explicit UnityDriver(Jni& jni) : j(jni) {}
    const char* name() const override { return "Unity"; }
    bool ownsGl() const override { return true; }
    static bool matches(const Library& l) {
        for (const auto& lib : l.libs) if (lib->name == "libunity.so") return true;
        return false;
    }
    bool call(Ctx& x, const char* n, std::vector<uint32_t> w = {}, JValue* r = nullptr) {
        if (j.callNative(x, "com/unity3d/player/UnityPlayer", n, player, w, r)) return true;
        x.proc.logf(LogLevel::Warning, "Android: Unity has no UnityPlayer.%s", n);
        return false;
    }
    void create(Ctx& x) override {
        player = j.newObject("com/unity3d/player/UnityPlayer");
        if (JObject* o = j.get(player)) o->permanent = true;
        const jobject activity = jobject(j.staticFields["hvm.activity"]);
        auto& L = x.proc.service<Library>();
        const std::string libDir = x.proc.guestPath(L.libDir);
        // NativeLoader.load(nativeLibraryDir): dlopens libmono and libunity, RegisterNatives.
        JValue loaded = 0;
        if (!j.callNative(x, "com/unity3d/player/NativeLoader", "load", j.classFor("com/unity3d/player/NativeLoader"),
                          {j.newString(libDir)}, &loaded))
            x.proc.log(LogLevel::Warning, "Android: libmain has no NativeLoader.load");
        else if (!(loaded & 0xFF))
            x.proc.log(LogLevel::Warning, "Android: NativeLoader.load returned false");
        // The APK: Unity reads its data (assets/bin/Data) from inside it.
        call(x, "nativeFile", {j.newString(x.proc.guestPath(x.proc.config().bundlePath + "/base.apk"))});
        call(x, "initJni", {activity});
        call(x, "nativeInitWWW", {j.classFor("com/unity3d/player/WWW")});
    }
    void surfaceReady(Ctx& x, const Surface&) override {
        const jobject surface = j.newObject("android/view/Surface");
        if (JObject* o = j.get(surface)) o->permanent = true;
        call(x, "nativeRecreateGfxState", {0, surface});
        call(x, "nativeResume");
        call(x, "nativeFocusChanged", {1});
    }
    void frame(Ctx& x) override { call(x, "nativeRender"); }
    void touch(Ctx& x, TouchPhase ph, float px, float py, uint32_t id, uint32_t count) override {
        // A MotionEvent for nativeInjectEvent (its accessors are in Java.cpp).
        const jobject ev = makeMotionEvent(j, ph, px, py, id, count);
        call(x, "nativeInjectEvent", {ev});
        j.remove(ev);
    }
    void pause(Ctx& x) override { call(x, "nativeFocusChanged", {0}); call(x, "nativePause"); }
    void resume(Ctx& x) override { call(x, "nativeResume"); call(x, "nativeFocusChanged", {1}); }
};

// Liquid Wars OS (com.xenris.liquidwarsos): GLSurfaceView renderer in libnativeinterface.so.
struct LiquidWarsDriver final : Driver {
    Jni& j;
    jobject cls = 0;
    int width = 0, height = 0;
    explicit LiquidWarsDriver(Jni& jni) : j(jni) {}
    const char* name() const override { return "LiquidWars"; }
    static bool matches(const Library& l) {
        return l.find("Java_com_xenris_liquidwarsos_NativeInterface_onDrawFrame").has_value();
    }
    void create(Ctx& x) override {
        cls = j.classFor("com/xenris/liquidwarsos/NativeInterface");
        j.callNative(x, "com/xenris/liquidwarsos/NativeInterface", "init", cls, {uint32_t(j.staticFields["hvm.assets"])});
        j.callNative(x, "com/xenris/liquidwarsos/NativeInterface", "createGame", cls, {0, 0, 1, 100});
    }
    void surfaceReady(Ctx& x, const Surface& s) override {
        width = s.width;
        height = s.height;
        j.callNative(x, "com/xenris/liquidwarsos/NativeInterface", "onSurfaceCreated", cls, {});
        j.callNative(x, "com/xenris/liquidwarsos/NativeInterface", "onSurfaceChanged", cls, {uint32_t(s.width), uint32_t(s.height)});
    }
    void frame(Ctx& x) override {
        j.callNative(x, "com/xenris/liquidwarsos/NativeInterface", "stepDots", cls, {});
        j.callNative(x, "com/xenris/liquidwarsos/NativeInterface", "onDrawFrame", cls, {});
    }
    void touch(Ctx& x, TouchPhase, float px, float py, uint32_t, uint32_t) override {
        if (width <= 0 || height <= 0) return;
        const int gx = int(px * 640.0f / float(width));
        const int gy = int((float(height) - py) * 480.0f / float(height));
        j.callNative(x, "com/xenris/liquidwarsos/NativeInterface", "setPlayerPosition", cls, {0, uint32_t(gx), uint32_t(gy)});
    }
};

// SDL2 and SDL3 (org.libsdl.app.SDLActivity): SDL creates its own EGL context on the window
// surface and runs SDL_main on its own thread.
struct SdlDriver final : Driver {
    Jni& j;
    jobject actCls = 0;
    float width = 0, height = 0;
    bool sdl3 = false;
    std::atomic<bool> exited{false};
    explicit SdlDriver(Jni& jni) : j(jni) {}
    const char* name() const override { return sdl3 ? "SDL3" : "SDL2"; }
    bool ownsGl() const override { return true; }
    static bool matches(const Library& l) {
        for (const auto& lib : l.libs)
            if (lib->name == "libSDL2.so" || lib->name == "libSDL3.so") return true;
        return false;
    }
    void create(Ctx& x) override {
        actCls = j.classFor("org/libsdl/app/SDLActivity");
        for (const auto& lib : x.proc.service<Library>().libs)
            if (lib->name == "libSDL3.so") sdl3 = true;
        linkExternalFiles(x.proc, fs::path(x.proc.homePath()) / "Library/files");
        linkExternalData(x.proc, externalRoot(x.proc));
    }
    void surfaceReady(Ctx& x, const Surface& s) override {
        width = float(s.width);
        height = float(s.height);
        j.callNative(x, "org/libsdl/app/SDLActivity", "nativeSetupJNI", actCls, {});
        j.callNative(x, "org/libsdl/app/SDLAudioManager", "nativeSetupJNI", j.classFor("org/libsdl/app/SDLAudioManager"), {});
        j.callNative(x, "org/libsdl/app/SDLControllerManager", "nativeSetupJNI", j.classFor("org/libsdl/app/SDLControllerManager"), {});
        j.callNative(x, "org/libsdl/app/SDLActivity", "nativeInitMainThread", actCls, {});
        j.callNative(x, "org/libsdl/app/SDLActivity", "nativeAddTouch", actCls, {0, j.newString("touch")});
        bool resSdl3 = sdl3;
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (j.registeredNatives.count("org/libsdl/app/SDLActivity.nativeSetScreenResolution(IIIIFF)V")) resSdl3 = true;
        }
        if (resSdl3)
            j.callNative(x, "org/libsdl/app/SDLActivity", "nativeSetScreenResolution", actCls,
                         {uint32_t(s.width), uint32_t(s.height), uint32_t(s.width), uint32_t(s.height),
                          uint32_t(jfloat(1.0f)), uint32_t(jfloat(60.0f))});
        else
            j.callNative(x, "org/libsdl/app/SDLActivity", "nativeSetScreenResolution", actCls,
                         {uint32_t(s.width), uint32_t(s.height), uint32_t(s.width), uint32_t(s.height),
                          1, uint32_t(jfloat(60.0f))});
        j.callNative(x, "org/libsdl/app/SDLActivity", "onNativeResize", actCls, {});
        j.callNative(x, "org/libsdl/app/SDLActivity", "onNativeSurfaceCreated", actCls, {});
        j.callNative(x, "org/libsdl/app/SDLActivity", "onNativeSurfaceChanged", actCls, {});
        j.callNative(x, "org/libsdl/app/SDLActivity", "nativeResume", actCls, {});
        j.callNative(x, "org/libsdl/app/SDLActivity", "nativeFocusChanged", actCls, {1});

        std::string mainLib = "libmain.so";
        for (const auto& lib : x.proc.service<Library>().libs)
            if (lib->so.image.symbols.count("SDL_main")) { mainLib = lib->name; break; }
        const uint32_t entry = x.proc.registerHostFunction("__hvm_sdl_main", [this, mainLib](Ctx& tx) {
            const jobject args = j.newArray('L', 0);
            if (JObject* o = j.get(args)) o->permanent = true;
            j.callNative(tx, "org/libsdl/app/SDLActivity", "nativeRunMain", actCls,
                         {j.newString(mainLib), j.newString("SDL_main"), args});
            exited.store(true);
        });
        x.proc.startThread(entry, 0, 2u << 20, true);
    }
    void frame(Ctx&) override {}
    void touch(Ctx& x, TouchPhase ph, float px, float py, uint32_t id, uint32_t) override {
        const uint32_t action = ph == TouchPhase::Began ? 0 : ph == TouchPhase::Moved ? 2 : ph == TouchPhase::Cancelled ? 3 : 1;
        const float nx = width > 0 ? px / width : 0.5f, ny = height > 0 ? py / height : 0.5f;
        j.callNative(x, "org/libsdl/app/SDLActivity", "onNativeTouch", actCls,
                     {0, id, action, uint32_t(jfloat(nx)), uint32_t(jfloat(ny)), uint32_t(jfloat(1.0f))});
    }
    void pause(Ctx& x) override {
        j.callNative(x, "org/libsdl/app/SDLActivity", "nativeFocusChanged", actCls, {0});
        j.callNative(x, "org/libsdl/app/SDLActivity", "nativePause", actCls, {});
    }
    void resume(Ctx& x) override {
        j.callNative(x, "org/libsdl/app/SDLActivity", "nativeResume", actCls, {});
        j.callNative(x, "org/libsdl/app/SDLActivity", "nativeFocusChanged", actCls, {1});
    }
    bool wantsExit(Ctx&) override { return exited.load(); }
};

// Pelya's commandergenius SDL 1.2 port (libsdl-1.2.so + libsdl_main.so): DemoRenderer.nativeInit
// runs SDL_main on the GL thread and calls DemoRenderer.swapBuffers()I to present each frame.
struct Sdl12Driver final : Driver {
    Jni& j;
    std::string pkg;
    jobject renderer = 0, audio = 0, view = 0;
    bool ran = false, exited = false;
    explicit Sdl12Driver(Jni& jni) : j(jni) {}
    const char* name() const override { return "SDL 1.2"; }
    static std::string findPkg(const Library& l) {
        const std::string pre = "Java_", suf = "_DemoRenderer_nativeInit";
        for (const auto& lib : l.libs)
            for (const auto& [sym, _] : lib->so.image.symbols)
                if (sym.rfind(pre, 0) == 0 && sym.size() > pre.size() + suf.size() &&
                    sym.compare(sym.size() - suf.size(), suf.size(), suf) == 0) {
                    std::string p = sym.substr(pre.size(), sym.size() - pre.size() - suf.size());
                    for (char& c : p) if (c == '_') c = '/';
                    return p;
                }
        return {};
    }
    static bool matches(const Library& l) { return !findPkg(l).empty(); }
    void create(Ctx& x) override {
        pkg = findPkg(x.proc.service<Library>());
        const fs::path files = fs::path(x.proc.homePath()) / "Library/files";
        std::error_code ec;
        fs::create_directories(files, ec);
        linkExternalFiles(x.proc, files);
        for (auto& e : fs::directory_iterator(assetsDir(x.proc), ec)) {
            if (e.path().extension() != ".zip") continue;
            try {
                Loader::ZipReader zip(e.path().string());
                zip.extract("", files.string(), true);
            } catch (const std::exception&) {}
        }
        const std::string rCls = pkg + "/DemoRenderer", aCls = pkg + "/AudioThread", vCls = pkg + "/DemoGLSurfaceView";
        for (const std::string& c : {rCls, aCls, vCls, pkg + "/MainActivity", pkg + "/Settings"}) j.quietClasses.insert(c);
        j.method(rCls, "swapBuffers", "()I", [](Jni&, Ctx& tx, jobject, const auto&) {
            if (GlSurface* g = currentSurface()) present(tx.proc, tx.cpu, *g, surfaceFor(tx.proc));
            return JValue(tx.proc.stopping() ? 0 : 1);
        });
        j.method(aCls, "initAudio", "(IIII)I", [](Jni& jni, Ctx&, jobject, const auto& a) {
            const uint32_t sz = a.size() > 3 && a[3] ? uint32_t(a[3]) : 4096u;
            const jobject buf = jni.newArray('B', sz);
            if (JObject* o = jni.get(buf)) o->permanent = true;
            jni.staticFields["hvm.sdl12.audiobuf"] = buf;
            return JValue(sz);
        });
        j.method(aCls, "getBuffer", "()[B", [](Jni& jni, Ctx&, jobject, const auto&) {
            auto& buf = jni.staticFields["hvm.sdl12.audiobuf"];
            if (!buf) {
                const jobject b = jni.newArray('B', 4096);
                if (JObject* o = jni.get(b)) o->permanent = true;
                buf = b;
            }
            return JValue(buf);
        });
        for (const char* m : {"fillBuffer", "initAudioThread", "deinitAudio", "pauseAudioPlayback", "resumeAudioPlayback"})
            j.method(aCls, m, "()I", [](Jni&, Ctx&, jobject, const auto&) { return JValue(1); });
    }
    void surfaceReady(Ctx& x, const Surface& s) override {
        renderer = j.newObject(pkg + "/DemoRenderer");
        if (JObject* o = j.get(renderer)) o->permanent = true;
        audio = j.newObject(pkg + "/AudioThread");
        if (JObject* o = j.get(audio)) o->permanent = true;
        view = j.newObject(pkg + "/DemoGLSurfaceView");
        if (JObject* o = j.get(view)) o->permanent = true;
        j.callNative(x, pkg + "/AudioThread", "nativeAudioInitJavaCallbacks", audio, {});
        j.callNative(x, pkg + "/DemoRenderer", "nativeInitJavaCallbacks", renderer, {});
        j.callNative(x, pkg + "/Settings", "nativeInitKeymap", j.classFor(pkg + "/Settings"), {});
        j.callNative(x, pkg + "/DemoRenderer", "nativeResize", renderer, {uint32_t(s.width), uint32_t(s.height), 0});
    }
    void frame(Ctx& x) override {
        if (ran) return;
        ran = true;
        const std::string dataDir = x.proc.guestPath((fs::path(x.proc.homePath()) / "Library/files").string());
        j.callNative(x, pkg + "/DemoRenderer", "nativeInit", renderer, {j.newString(dataDir), j.newString("sdl"), 0, 0});
        exited = true;
    }
    void touch(Ctx& x, TouchPhase ph, float px, float py, uint32_t id, uint32_t) override {
        const uint32_t action = ph == TouchPhase::Began ? 0 : ph == TouchPhase::Moved ? 2 : 1;
        j.callNative(x, pkg + "/DemoGLSurfaceView", "nativeMotionEvent", view,
                     {uint32_t(int(px)), uint32_t(int(py)), action, id, 1000, 1000});
    }
    bool wantsExit(Ctx&) override { return exited; }
};

// NativeActivity (android.app.NativeActivity + android_native_app_glue):
// ANativeActivity_onCreate populates ANativeActivityCallbacks, then the game's android_main
// thread runs its own ALooper_pollAll + EGL loop on ANativeWindow and AInputQueue.
struct NativeActivityDriver final : Driver {
    Jni& j;
    uint32_t activityPtr = 0, callbacksPtr = 0;
    explicit NativeActivityDriver(Jni& jni) : j(jni) {}
    const char* name() const override { return "NativeActivity"; }
    bool ownsGl() const override { return true; }
    static bool matches(const Library& l) { return l.find("ANativeActivity_onCreate").has_value(); }
    void cb(Ctx& x, uint32_t offset, std::initializer_list<uint32_t> args) {
        if (!callbacksPtr) return;
        const uint32_t fn = x.mem.read<uint32_t>(callbacksPtr + offset);
        if (fn) x.proc.callGuest(x.cpu, fn, args);
    }
    void create(Ctx& x) override {
        const auto& lib = x.proc.service<Library>();
        const auto onCreate = lib.find("ANativeActivity_onCreate");
        if (!onCreate) return;
        const std::string pkg = x.proc.infoPlist() ? x.proc.infoPlist()->getString("CFBundleIdentifier", "app") : "app";
        const std::string internal = x.proc.homePath() + "/Documents";
        const std::string external = externalRoot(x.proc) + "/Android/data/" + pkg + "/files";
        const std::string obb = externalRoot(x.proc) + "/Android/obb/" + pkg;
        std::error_code ec;
        fs::create_directories(internal, ec);
        fs::create_directories(external, ec);
        fs::create_directories(obb, ec);
        linkExternalData(x.proc, externalRoot(x.proc));

        callbacksPtr = x.proc.allocStatic(64);
        activityPtr = x.proc.allocStatic(40);
        x.mem.write<uint32_t>(activityPtr + 0, callbacksPtr);
        x.mem.write<uint32_t>(activityPtr + 4, j.vm);
        x.mem.write<uint32_t>(activityPtr + 8, j.envFor(x.proc));
        x.mem.write<uint32_t>(activityPtr + 12, uint32_t(j.staticFields["hvm.activity"]));
        x.mem.write<uint32_t>(activityPtr + 16, x.proc.allocCString(x.proc.guestPath(internal)));
        x.mem.write<uint32_t>(activityPtr + 20, x.proc.allocCString(x.proc.guestPath(external)));
        x.mem.write<int32_t>(activityPtr + 24, 19);   // sdkVersion
        x.mem.write<uint32_t>(activityPtr + 28, 0);   // instance
        x.mem.write<uint32_t>(activityPtr + 32, assetManagerHandle(x.proc));
        x.mem.write<uint32_t>(activityPtr + 36, x.proc.allocCString(x.proc.guestPath(obb)));

        x.proc.callGuest(x.cpu, *onCreate, {activityPtr, 0, 0});
        cb(x, 0, {activityPtr});   // onStart
        cb(x, 4, {activityPtr});   // onResume
    }
    void surfaceReady(Ctx& x, const Surface&) override {
        cb(x, 44, {activityPtr, inputQueue(x.proc)});           // onInputQueueCreated
        cb(x, 28, {activityPtr, screenNativeWindow(x.proc)});   // onNativeWindowCreated
        cb(x, 24, {activityPtr, 1});                            // onWindowFocusChanged(true)
    }
    void frame(Ctx&) override {}
    void touch(Ctx& x, TouchPhase ph, float px, float py, uint32_t id, uint32_t count) override {
        pushTouchEvent(x.proc, ph, px, py, id, count);
    }
    void pause(Ctx& x) override {
        cb(x, 24, {activityPtr, 0});   // onWindowFocusChanged(false)
        cb(x, 12, {activityPtr});      // onPause
    }
    void resume(Ctx& x) override {
        cb(x, 4, {activityPtr});       // onResume
        cb(x, 24, {activityPtr, 1});   // onWindowFocusChanged(true)
    }
};

// Marmalade SDK (libs3e_android.so, e.g. Vector): LoaderActivity / LoaderThread.
struct MarmaladeDriver final : Driver {
    Jni& j;
    jobject loaderThread = 0;
    bool threaded = false;
    std::atomic<bool> exited{false};
    explicit MarmaladeDriver(Jni& jni) : j(jni) {}
    const char* name() const override { return "Marmalade"; }
    bool ownsGl() const override { return threaded; }
    static bool matches(const Library& l) {
        for (const auto& lib : l.libs)
            if (lib->name.rfind("libs3e", 0) == 0) return true;
        return l.find("Java_com_ideaworks3d_marmalade_LoaderThread_runNative").has_value() ||
               l.find("Java_com_ideaworks3d_marmalade_LoaderThread_initNative").has_value();
    }
    void create(Ctx& x) override {
        const std::string kAct = "com/ideaworks3d/marmalade/LoaderActivity";
        const std::string kThr = "com/ideaworks3d/marmalade/LoaderThread";
        const std::string kView = "com/ideaworks3d/marmalade/LoaderView";
        for (const auto& c : {kAct, kThr, kView, std::string("com/ideaworks3d/marmalade/LoaderAPI"),
                              std::string("com/ideaworks3d/marmalade/ResourceUtility")})
            j.quietClasses.insert(c);
        const jobject act = jobject(j.staticFields["hvm.activity"]);
        const jobject assets = jobject(j.staticFields["hvm.assets"]);
        j.method(kAct, "getAssets", "()Landroid/content/res/AssetManager;", [assets](Jni&, Ctx&, jobject, const auto&) { return JValue(assets); });
        j.method(kThr, "getActivity", "()Lcom/ideaworks3d/marmalade/LoaderActivity;", [act](Jni&, Ctx&, jobject, const auto&) { return JValue(act); });
        loaderThread = j.newObject(kThr);
        if (JObject* o = j.get(loaderThread)) o->permanent = true;

        const auto& lib = x.proc.service<Library>();
        threaded = lib.find("Java_com_ideaworks3d_marmalade_LoaderThread_runNative").has_value();
        j.callNative(x, kThr, "initNative", loaderThread, {});
        j.callNative(x, kAct, "onCreateNative", act, {});
    }
    void surfaceReady(Ctx& x, const Surface& s) override {
        const std::string kThr = "com/ideaworks3d/marmalade/LoaderThread";
        const std::string kView = "com/ideaworks3d/marmalade/LoaderView";
        j.callNative(x, kView, "onSurfaceCreatedNative", j.newObject(kView), {});
        j.callNative(x, kView, "onSurfaceChangedNative", j.newObject(kView), {uint32_t(s.width), uint32_t(s.height)});
        if (threaded) {
            const uint32_t entry = x.proc.registerHostFunction("__hvm_marmalade_main", [this, kThr](Ctx& tx) {
                j.callNative(tx, kThr, "runNative", loaderThread, {});
                exited.store(true);
            });
            x.proc.startThread(entry, 0, 2u << 20, true);
        }
    }
    void frame(Ctx& x) override {
        if (!threaded) j.callNative(x, "com/ideaworks3d/marmalade/LoaderThread", "onDrawFrameNative", loaderThread, {});
    }
    void touch(Ctx& x, TouchPhase ph, float px, float py, uint32_t id, uint32_t) override {
        const uint32_t action = ph == TouchPhase::Began ? 0 : ph == TouchPhase::Moved ? 2 : 1;
        j.callNative(x, "com/ideaworks3d/marmalade/LoaderThread", "onTouchEventNative", loaderThread,
                     {id, action, uint32_t(int(px)), uint32_t(int(py))});
    }
    void pause(Ctx& x) override { j.callNative(x, "com/ideaworks3d/marmalade/LoaderThread", "onPauseNative", loaderThread, {}); }
    void resume(Ctx& x) override { j.callNative(x, "com/ideaworks3d/marmalade/LoaderThread", "onResumeNative", loaderThread, {}); }
    bool wantsExit(Ctx&) override { return exited.load(); }
};

// Allegro 5 (org.liballeg.android.AllegroActivity, e.g. Open Surge).
struct AllegroDriver final : Driver {
    Jni& j;
    jobject act = 0, surf = 0, touchCls = 0;
    explicit AllegroDriver(Jni& jni) : j(jni) {}
    const char* name() const override { return "Allegro 5"; }
    bool ownsGl() const override { return true; }
    static bool matches(const Library& l) {
        return l.find("Java_org_liballeg_android_AllegroActivity_nativeOnCreate").has_value();
    }
    void create(Ctx& x) override {
        const std::string kAct = "org/liballeg/android/AllegroActivity";
        const std::string kSurf = "org/liballeg/android/AllegroSurface";
        for (const char* c : {"org/liballeg/android/AllegroActivity", "org/liballeg/android/AllegroSurface",
                              "org/liballeg/android/AllegroAPKList", "org/liballeg/android/AllegroAPKStream",
                              "org/liballeg/android/Const", "org/liballeg/android/KeyListener",
                              "org/liballeg/android/TouchListener", "org/liballeg/android/Sensors"})
            j.quietClasses.insert(c);
        act = j.newObject(kAct);
        if (JObject* o = j.get(act)) o->permanent = true;
        surf = j.newObject(kSurf);
        if (JObject* o = j.get(surf)) o->permanent = true;
        touchCls = j.classFor("org/liballeg/android/TouchListener");
        j.method(kAct, "getResources", "()Landroid/content/res/Resources;", [](Jni& jni, Ctx&, jobject, const auto&) {
            return JValue(jni.newObject("android/content/res/Resources"));
        });
        j.method(kAct, "getAssets", "()Landroid/content/res/AssetManager;", [](Jni& jni, Ctx&, jobject, const auto&) {
            return JValue(jni.staticFields["hvm.assets"]);
        });
        j.method(kAct, "getUserLibName", "()Ljava/lang/String;", [](Jni& jni, Ctx& tx, jobject, const auto&) {
            return JValue(jni.newString(tx.proc.service<Library>().name));
        });
        j.method(kAct, "getResourcesDir", "()Ljava/lang/String;", [](Jni& jni, Ctx& tx, jobject, const auto&) {
            const std::string d = tx.proc.homePath() + "/Documents";
            std::error_code ec; fs::create_directories(d, ec);
            return JValue(jni.newString(tx.proc.guestPath(d)));
        });
        j.method(kAct, "getPubDataDir", "()Ljava/lang/String;", [](Jni& jni, Ctx& tx, jobject, const auto&) {
            const std::string d = tx.proc.homePath() + "/Documents";
            std::error_code ec; fs::create_directories(d, ec);
            return JValue(jni.newString(tx.proc.guestPath(d)));
        });
        j.method(kAct, "getTempDir", "()Ljava/lang/String;", [](Jni& jni, Ctx& tx, jobject, const auto&) {
            const std::string d = tx.proc.homePath() + "/tmp";
            std::error_code ec; fs::create_directories(d, ec);
            return JValue(jni.newString(tx.proc.guestPath(d)));
        });
        j.method(kAct, "getApkPath", "()Ljava/lang/String;", [](Jni& jni, Ctx& tx, jobject, const auto&) {
            return JValue(jni.newString(tx.proc.guestPath(tx.proc.config().bundlePath + "/base.apk")));
        });
        j.method(kAct, "getModel", "()Ljava/lang/String;", [](Jni& jni, Ctx&, jobject, const auto&) {
            return JValue(jni.newString("HydrogenVM"));
        });
        j.method(kAct, "getManufacturer", "()Ljava/lang/String;", [](Jni& jni, Ctx&, jobject, const auto&) {
            return JValue(jni.newString("HydrogenVM"));
        });
        j.method(kAct, "showMessageBox", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)I", [](Jni&, Ctx&, jobject, const auto&) {
            return JValue(1);
        });
        j.method(kAct, "postRunnable", "(Ljava/lang/Runnable;)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
        j.callNative(x, kAct, "nativeOnCreate", act, {});
    }
    void surfaceReady(Ctx& x, const Surface& s) override {
        j.callNative(x, "org/liballeg/android/AllegroSurface", "nativeOnCreate", surf, {});
        j.callNative(x, "org/liballeg/android/AllegroSurface", "nativeOnChange", surf, {1u, uint32_t(s.width), uint32_t(s.height)});
        j.callNative(x, "org/liballeg/android/AllegroActivity", "nativeOnResume", act, {});
    }
    void frame(Ctx&) override {}
    void touch(Ctx& x, TouchPhase ph, float px, float py, uint32_t id, uint32_t) override {
        const uint32_t action = ph == TouchPhase::Began ? 0 : ph == TouchPhase::Moved ? 2 : ph == TouchPhase::Cancelled ? 3 : 1;
        j.callNative(x, "org/liballeg/android/TouchListener", "nativeOnTouch", touchCls,
                     {id, action, uint32_t(jfloat(px)), uint32_t(jfloat(py))});
    }
    void pause(Ctx& x) override { j.callNative(x, "org/liballeg/android/AllegroActivity", "nativeOnPause", act, {}); }
    void resume(Ctx& x) override { j.callNative(x, "org/liballeg/android/AllegroActivity", "nativeOnResume", act, {}); }
};

// HyperRogue (com.roguetemple.hyperroid.HyperRogue).
struct HyperRogueDriver final : Driver {
    Jni& j;
    jobject cls = 0;
    explicit HyperRogueDriver(Jni& jni) : j(jni) {}
    const char* name() const override { return "HyperRogue"; }
    static bool matches(const Library& l) {
        return l.find("Java_com_roguetemple_hyperroid_HyperRogue_draw").has_value();
    }
    void create(Ctx& x) override {
        const std::string kHR = "com/roguetemple/hyperroid/HyperRogue";
        j.quietClasses.insert(kHR);
        cls = j.classFor(kHR);
        const std::string files = x.proc.homePath() + "/Documents";
        std::error_code ec;
        fs::create_directories(files, ec);
        j.callNative(x, kHR, "setFilesDir", cls, {j.newString(x.proc.guestPath(files))});
        j.callNative(x, kHR, "initGame", cls, {});
    }
    void surfaceReady(Ctx& x, const Surface&) override {
        j.callNative(x, "com/roguetemple/hyperroid/HyperRogue", "glhrinit", cls, {});
    }
    void frame(Ctx& x) override {
        j.callNative(x, "com/roguetemple/hyperroid/HyperRogue", "update", cls, {});
        j.callNative(x, "com/roguetemple/hyperroid/HyperRogue", "draw", cls, {});
    }
    void touch(Ctx&, TouchPhase, float, float, uint32_t, uint32_t) override {}
};

// Cocos2d-x (org.cocos2dx.lib.Cocos2dxRenderer / Cocos2dxActivity / Cocos2dxHelper).
struct Cocos2dxDriver final : Driver {
    Jni& j;
    jobject renderer = 0;
    explicit Cocos2dxDriver(Jni& jni) : j(jni) {}
    const char* name() const override { return "Cocos2d-x"; }
    static bool matches(const Library& l) {
        return l.find("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInit").has_value() ||
               l.find("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender").has_value();
    }
    void create(Ctx&) override {
        for (const char* c : {"org/cocos2dx/lib/Cocos2dxActivity", "org/cocos2dx/lib/Cocos2dxRenderer",
                              "org/cocos2dx/lib/Cocos2dxHelper", "org/cocos2dx/lib/Cocos2dxBitmap"})
            j.quietClasses.insert(c);
        renderer = j.newObject("org/cocos2dx/lib/Cocos2dxRenderer");
        if (JObject* o = j.get(renderer)) o->permanent = true;
    }
    void surfaceReady(Ctx& x, const Surface& s) override {
        j.callNative(x, "org/cocos2dx/lib/Cocos2dxRenderer", "nativeInit", renderer, {uint32_t(s.width), uint32_t(s.height)});
    }
    void frame(Ctx& x) override {
        j.callNative(x, "org/cocos2dx/lib/Cocos2dxRenderer", "nativeRender", renderer, {});
    }
    void touch(Ctx& x, TouchPhase ph, float px, float py, uint32_t id, uint32_t) override {
        const std::string kR = "org/cocos2dx/lib/Cocos2dxRenderer";
        if (ph == TouchPhase::Began)
            j.callNative(x, kR, "nativeTouchesBegin", renderer, {id, uint32_t(jfloat(px)), uint32_t(jfloat(py))});
        else if (ph == TouchPhase::Moved) {
            const jobject ids = j.newArray('I', 1), xs = j.newArray('F', 1), ys = j.newArray('F', 1);
            if (JObject* o = j.get(ids)) std::memcpy(o->data.data(), &id, 4);
            if (JObject* o = j.get(xs)) std::memcpy(o->data.data(), &px, 4);
            if (JObject* o = j.get(ys)) std::memcpy(o->data.data(), &py, 4);
            j.callNative(x, kR, "nativeTouchesMove", renderer, {ids, xs, ys});
        } else if (ph == TouchPhase::Cancelled) {
            const jobject ids = j.newArray('I', 1), xs = j.newArray('F', 1), ys = j.newArray('F', 1);
            if (JObject* o = j.get(ids)) std::memcpy(o->data.data(), &id, 4);
            if (JObject* o = j.get(xs)) std::memcpy(o->data.data(), &px, 4);
            if (JObject* o = j.get(ys)) std::memcpy(o->data.data(), &py, 4);
            j.callNative(x, kR, "nativeTouchesCancel", renderer, {ids, xs, ys});
        } else {
            j.callNative(x, kR, "nativeTouchesEnd", renderer, {id, uint32_t(jfloat(px)), uint32_t(jfloat(py))});
        }
    }
    void pause(Ctx& x) override { j.callNative(x, "org/cocos2dx/lib/Cocos2dxRenderer", "nativeOnPause", renderer, {}); }
    void resume(Ctx& x) override { j.callNative(x, "org/cocos2dx/lib/Cocos2dxRenderer", "nativeOnResume", renderer, {}); }
};

// GameMaker (com.yoyogames.runner.RunnerJNILib).
struct GameMakerDriver final : Driver {
    Jni& j;
    jobject cls = 0;
    int width = 0, height = 0;
    explicit GameMakerDriver(Jni& jni) : j(jni) {}
    const char* name() const override { return "GameMaker"; }
    static bool matches(const Library& l) {
        return l.find("Java_com_yoyogames_runner_RunnerJNILib_Process").has_value() ||
               l.find("Java_com_yoyogames_runner_RunnerJNILib_Startup").has_value();
    }
    void create(Ctx& x) override {
        const std::string kGM = "com/yoyogames/runner/RunnerJNILib";
        j.quietClasses.insert(kGM);
        cls = j.classFor(kGM);
        const std::string apk = x.proc.guestPath(x.proc.config().bundlePath + "/base.apk");
        const std::string save = x.proc.guestPath(x.proc.homePath() + "/Documents");
        const std::string pkg = x.proc.infoPlist() ? x.proc.infoPlist()->getString("CFBundleIdentifier", "app") : "app";
        j.callNative(x, kGM, "Startup", cls, {j.newString(apk), j.newString(save), j.newString(pkg), 0, 0});
    }
    void surfaceReady(Ctx& x, const Surface& s) override {
        width = s.width;
        height = s.height;
        j.callNative(x, "com/yoyogames/runner/RunnerJNILib", "RestoreGraphics", cls, {});
    }
    void frame(Ctx& x) override {
        j.callNative(x, "com/yoyogames/runner/RunnerJNILib", "Process", cls,
                     {uint32_t(width), uint32_t(height), 0, 0, 0, 0, 0, 60});
    }
    void touch(Ctx& x, TouchPhase ph, float px, float py, uint32_t id, uint32_t) override {
        const uint32_t type = ph == TouchPhase::Began ? 0 : ph == TouchPhase::Moved ? 2 : 1;
        j.callNative(x, "com/yoyogames/runner/RunnerJNILib", "TouchEvent", cls,
                     {type, id, uint32_t(jfloat(px)), uint32_t(jfloat(py))});
    }
};

std::unique_ptr<Driver> driverFor(Process& p, Jni& j) {
    const auto& lib = p.service<Library>();
    if (EaBlastDriver::matches(lib)) return std::make_unique<EaBlastDriver>(j);
    if (UbisoftUafDriver::matches(lib)) return std::make_unique<UbisoftUafDriver>(j);
    if (UnityDriver::matches(lib)) return std::make_unique<UnityDriver>(j);
    if (LiquidWarsDriver::matches(lib)) return std::make_unique<LiquidWarsDriver>(j);
    if (SdlDriver::matches(lib)) return std::make_unique<SdlDriver>(j);
    if (Sdl12Driver::matches(lib)) return std::make_unique<Sdl12Driver>(j);
    if (MarmaladeDriver::matches(lib)) return std::make_unique<MarmaladeDriver>(j);
    if (AllegroDriver::matches(lib)) return std::make_unique<AllegroDriver>(j);
    if (HyperRogueDriver::matches(lib)) return std::make_unique<HyperRogueDriver>(j);
    if (Cocos2dxDriver::matches(lib)) return std::make_unique<Cocos2dxDriver>(j);
    if (GameMakerDriver::matches(lib)) return std::make_unique<GameMakerDriver>(j);
    if (NativeActivityDriver::matches(lib)) return std::make_unique<NativeActivityDriver>(j);
    return nullptr;
}

} // namespace

int runActivity(Process& p, CPU::Cpu& c) {
    Jni& j = jni(p);
    Ctx x{p, c, p.mem()};
    const auto& lib = p.service<Library>();
    const Surface s = surfaceFor(p);
    {   // The player follows UIKit's screen and orientation; there is no UIKit app to set them.
        auto& u = UIKit::ui(p);
        u.screenW = float(p.config().screenWidth);
        u.screenH = float(p.config().screenHeight);
        u.scale = s.scale;
        u.statusBarOrientation = s.portrait ? 1 : 3;   // UIInterfaceOrientationLandscapeRight
    }
    // The Android Test Lab (tests/android/): a library exporting hvm_test_main(JNIEnv*, JavaVM*)
    // runs its checks instead of an activity; the exit code is the number of failures.
    if (const auto test = lib.find("hvm_test_main")) {
        if (const auto onLoad = lib.find("JNI_OnLoad")) p.callGuest(c, *onLoad, {j.vm, 0});
        const uint32_t failures = uint32_t(p.callGuest(c, *test, {j.envFor(p), j.vm}));
        p.logf(LogLevel::Info, "Android Test Lab: %u failure(s)", failures);
        return int(failures);
    }
    auto driver = driverFor(p, j);
    if (!driver) throw std::runtime_error("This Android game's engine isn't supported yet (" + lib.name +
                                          " has no Java activity HydrogenVM knows how to play)");
    p.logf(LogLevel::Info, "Android: %s (%s), surface %dx%d", lib.name.c_str(), driver->name(), s.width, s.height);

    // System.loadLibrary of each library calls its JNI_OnLoad, once, in load order.
    std::vector<std::pair<std::string, uint32_t>> onLoads;
    {
        auto& L = p.service<Library>();
        std::lock_guard<std::recursive_mutex> lk(L.mu);
        for (const auto& l : L.libs)
            if (auto it = l->so.image.symbols.find("JNI_OnLoad"); it != l->so.image.symbols.end()) onLoads.emplace_back(l->name, it->second);
    }
    for (const auto& [name, fn] : onLoads) {
        const uint32_t v = uint32_t(p.callGuest(c, fn, {j.vm, 0}));
        p.logf(LogLevel::Info, "Android: %s JNI_OnLoad returned 0x%x", name.c_str(), v);
    }
    driver->create(x);
    GlSurface g;
    if (!driver->ownsGl()) {
        g = makeSurface(p, c, s);
        setCurrentSurface(&g);
    }
    driver->surfaceReady(x, s);

    // Touches, keys, joystick, and accelerometer: the host's touches are in portrait screen points; the surface is in pixels.
    std::mutex inMu;
    std::deque<Pointer> pending;
    struct Ev { TouchPhase phase; float x, y; uint32_t id; };
    struct KeyEv { int32_t code; bool down; };
    std::deque<Ev> events;
    std::deque<KeyEv> keyEvents;
    const float portraitW = float(p.config().screenWidth);
    p.setTouchHandler([&, portraitW](TouchPhase phase, float tx, float ty, uint32_t finger) {
        std::lock_guard<std::mutex> lk(inMu);
        if (s.portrait) events.push_back({phase, tx * s.scale, ty * s.scale, finger});
        else events.push_back({phase, ty * s.scale, (portraitW - tx) * s.scale, finger});
    });
    p.setAccelerationHandler([&](float gx, float gy, float gz) { setAndroidAcceleration(p, gx, gy, gz); });
    p.setKeyHandler([&](int32_t code, bool down) {
        std::lock_guard<std::mutex> lk(inMu);
        keyEvents.push_back({code, down});
    });
    p.setJoystickHandler([&](float lx, float ly, float rx, float ry, float hatX, float hatY) {
        pushJoystickEvent(p, lx, ly, rx, ry, hatX, hatY);
    });
    std::set<uint32_t> down;
    std::map<uint32_t, uint32_t> pointers;   // host finger id -> Android pointer id

    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    bool paused = false;
    while (!p.stopping()) {
        if (p.backgroundRequested() && !paused) {
            p.mixer().setAllPaused(true);
            driver->pause(x);
            paused = true;
            p.setBackgrounded(true);
        }
        if (paused) {
            p.waitWhileBackgrounded();
            if (p.stopping()) break;
            driver->resume(x);
            p.mixer().setAllPaused(false);
            paused = false;
            next = clock::now();
        }
        std::deque<Ev> batch;
        std::deque<KeyEv> keyBatch;
        {
            std::lock_guard<std::mutex> lk(inMu);
            batch.swap(events);
            keyBatch.swap(keyEvents);
        }
        for (const auto& k : keyBatch) {
            if (!driver->key(x, k.code, k.down)) pushKeyEvent(p, k.code, k.down);
        }
        // Android numbers pointers from 0 (the first finger down is 0, the next free number for
        // each later one); engines treat pointer 0 as the primary touch (Unity's taps and mouse
        // emulation). The host's finger ids are mapped to those numbers.
        for (const auto& e : batch) {
            uint32_t pointer = 0;
            if (auto it = pointers.find(e.id); it != pointers.end()) {
                pointer = it->second;
            } else {
                while (std::any_of(pointers.begin(), pointers.end(), [&](const auto& kv) { return kv.second == pointer; })) ++pointer;
                if (e.phase == TouchPhase::Began) pointers[e.id] = pointer;
            }
            if (e.phase == TouchPhase::Began) down.insert(e.id);
            driver->touch(x, e.phase, e.x, e.y, pointer, uint32_t(std::max<size_t>(down.size(), 1)));
            if (e.phase == TouchPhase::Ended || e.phase == TouchPhase::Cancelled) {
                down.erase(e.id);
                pointers.erase(e.id);
            }
        }
        driver->frame(x);
        if (!driver->ownsGl()) {
            p.perf().gameSwaps++;   // engines that swap themselves count in eglSwapBuffers
            present(p, c, g, s);
        }
        if (driver->wantsExit(x)) {
            p.log(LogLevel::Info, "Android: the game asked to exit");
            break;
        }
        next += std::chrono::microseconds(16667);
        const auto now = clock::now();
        if (next > now) std::this_thread::sleep_until(next);
        else if (now - next > 100ms) next = now;
    }
    if (!paused) driver->pause(x);
    setCurrentSurface(nullptr);
    p.setTouchHandler({});
    p.setAccelerationHandler({});
    p.setKeyHandler({});
    p.setJoystickHandler({});
    return 0;
}

} // namespace Hydrogen::Runtime::Android
