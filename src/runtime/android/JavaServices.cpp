// HydrogenVM — Android framework services a game's native code asks for through JNI: the
// device description (android.os.Build), system services (window and display metrics,
// telephony, connectivity, vibrator), SharedPreferences kept in a file per name, the launch
// Intent, the package manager's own-package queries, the locale, and interface proxies
// (bitter.jnibridge.JNIBridge, which Unity uses for Runnable and listener callbacks).
//
// The answers describe an offline phone: no network, no SIM, one display the size of the
// game's surface.
#include "runtime/android/Jni.hpp"
#include "runtime/android/BionicInternal.hpp"
#include "runtime/android/Surface.hpp"
#include "runtime/audio/Audio.hpp"
#include "loader/Apk.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <unordered_map>

namespace fs = std::filesystem;

namespace Hydrogen::Runtime::Android {
namespace {

std::vector<uint8_t> readHostBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {});
}

std::vector<uint8_t> readFdBytes(int fd, int64_t offset, int64_t length) {
    if (fd < 0) return {};
    const off_t saved = ::lseek(fd, 0, SEEK_CUR);
    const off_t total = ::lseek(fd, 0, SEEK_END);
    if (total < 0) {
        if (saved >= 0) ::lseek(fd, saved, SEEK_SET);
        return {};
    }
    const int64_t start = std::max<int64_t>(0, offset);
    int64_t count = length >= 0 ? length : (int64_t(total) - start);
    if (start + count > int64_t(total)) count = std::max<int64_t>(0, int64_t(total) - start);
    std::vector<uint8_t> out(size_t(count), 0);
    if (count > 0) {
        ::lseek(fd, off_t(start), SEEK_SET);
        size_t got = 0;
        while (got < size_t(count)) {
            const ssize_t n = ::read(fd, out.data() + got, size_t(count) - got);
            if (n <= 0) break;
            got += size_t(n);
        }
        out.resize(got);
    }
    if (saved >= 0) ::lseek(fd, saved, SEEK_SET);
    return out;
}

std::string packageName(Process& p) {
    const auto* info = p.infoPlist();
    return info ? info->getString("CFBundleIdentifier", "app") : std::string("app");
}
std::string str(Jni& j, JValue v) { return j.string(jobject(v)); }
uint64_t& field(Jni& j, jobject h, const std::string& f) {
    static uint64_t none;
    JObject* o = j.get(h);
    if (!o) { none = 0; return none; }
    return o->fields[f];
}
jobject permanentString(Jni& j, const std::string& s) {
    const jobject h = j.newString(s);
    std::lock_guard<std::recursive_mutex> lk(j.mu);
    if (JObject* o = j.get(h)) o->permanent = true;
    return h;
}
jobject singleton(Jni& j, const std::string& cls) {
    const std::string key = "hvm.singleton." + cls;
    {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (auto it = j.staticFields.find(key); it != j.staticFields.end()) return jobject(it->second);
    }
    const jobject h = j.newObject(cls);
    std::lock_guard<std::recursive_mutex> lk(j.mu);
    if (JObject* o = j.get(h)) o->permanent = true;
    j.staticFields[key] = h;
    return h;
}

// ------------------------------------------------------- SharedPreferences ---
// One file per preferences name: "type<TAB>key<TAB>value" lines, with \\, \t and \n escaped.
struct Prefs {
    std::string file;
    std::map<std::string, std::pair<char, std::string>> values;   // key -> (type Z I J F S, text)
};
std::string escape(const std::string& s) {
    std::string r;
    for (char c : s) {
        if (c == '\\') r += "\\\\"; else if (c == '\t') r += "\\t"; else if (c == '\n') r += "\\n"; else r += c;
    }
    return r;
}
std::string unescape(const std::string& s) {
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) { const char n = s[++i]; r += n == 't' ? '\t' : n == 'n' ? '\n' : n; }
        else r += s[i];
    }
    return r;
}
void loadPrefs(Prefs& pr) {
    std::ifstream in(pr.file);
    std::string line;
    while (std::getline(in, line)) {
        const size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
        if (a != 1 || b == std::string::npos) continue;
        pr.values[unescape(line.substr(2, b - 2))] = {line[0], unescape(line.substr(b + 1))};
    }
}
void savePrefs(const Prefs& pr) {
    std::error_code ec;
    fs::create_directories(fs::path(pr.file).parent_path(), ec);
    const std::string tmp = pr.file + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        for (const auto& [k, v] : pr.values) out << v.first << '\t' << escape(k) << '\t' << escape(v.second) << '\n';
    }
    fs::rename(tmp, pr.file, ec);
}
std::shared_ptr<Prefs> prefsOf(Jni& j, jobject h) {
    std::lock_guard<std::recursive_mutex> lk(j.mu);
    JObject* o = j.get(h);
    if (!o) return nullptr;
    if (auto* sp = std::any_cast<std::shared_ptr<Prefs>>(&o->host)) return *sp;
    return nullptr;
}

// --------------------------------------------------------------- proxies ---
// A JNIBridge proxy: an object of the interface's class whose methods go to the library's
// native JNIBridge.invoke(long ptr, Method, Object[]).
jobject boxed(Jni& j, char t, JValue v) {
    const char* cls = nullptr;
    switch (t) {
        case 'Z': cls = "java/lang/Boolean"; break;
        case 'B': cls = "java/lang/Byte"; break;
        case 'C': cls = "java/lang/Character"; break;
        case 'S': cls = "java/lang/Short"; break;
        case 'I': cls = "java/lang/Integer"; break;
        case 'J': cls = "java/lang/Long"; break;
        case 'F': cls = "java/lang/Float"; break;
        case 'D': cls = "java/lang/Double"; break;
        default: return jobject(v);
    }
    const jobject b = j.newObject(cls);
    std::lock_guard<std::recursive_mutex> lk(j.mu);
    field(j, b, "value") = v;
    return b;
}

// FMOD's Java output (org.fmod.FMODAudioDevice): a thread that asks the library for mixed
// PCM (fmodProcess into a direct buffer) and writes it to an AudioTrack, which paces it.
// fmodGetInfo: 0 sample rate, 1 DSP buffer length (frames), 2 number of DSP buffers.
void fmodThread(Ctx& x, uint32_t self) {
    if (GuestThread* t = x.proc.currentThreadIfGuest()) t->role.store("FMOD audio thread: mixes the game's sound into an AudioTrack");
    Jni& j = jni(x.proc);
    const std::string kF = "org/fmod/FMODAudioDevice";
    const std::string runKey = "hvm.fmod.running." + std::to_string(self);
    auto running = [&] {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        return j.staticFields[runKey] != 0 && !x.proc.stopping();
    };
    auto info = [&](uint32_t what) {
        JValue r = 0;
        j.callNative(x, kF, "fmodGetInfo", self, {what}, &r);
        return int32_t(r);
    };
    int32_t rate = 0;
    while (running() && (rate = info(0)) <= 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!running()) return;
    int32_t len = info(1), num = info(2);
    x.proc.logf(LogLevel::Info, "FMOD: Java audio output, %d Hz, %d-frame buffers x %d", rate, len, num);
    if (len <= 0 || len > 16384) len = 1024;
    const uint32_t bytes = uint32_t(len) * 4;   // 16-bit stereo
    const uint32_t buf = x.proc.heap().allocZeroed(bytes);
    const jobject dbb = j.newObject("java/nio/DirectByteBuffer");
    const jobject arr = j.newArray('S', uint32_t(len) * 2);
    const jobject track = j.newObject("android/media/AudioTrack");
    {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(dbb)) { o->fields["hvm.address"] = buf; o->fields["hvm.capacity"] = bytes; o->permanent = true; }
        if (JObject* o = j.get(arr)) o->permanent = true;
    }
    const uint32_t trackBytes = std::max<uint32_t>(8192, uint32_t(len) * uint32_t(std::max(num, 2)) * 4);
    j.invoke(x, j.methodId("android/media/AudioTrack", "<init>", "(IIIIII)V", false), track, {3, uint32_t(rate), 12, 2, trackBytes, 1});
    j.invoke(x, j.methodId("android/media/AudioTrack", "play", "()V", false), track, {});
    const uint32_t write = j.methodId("android/media/AudioTrack", "write", "([SII)I", false);
    while (running()) {
        j.callNative(x, kF, "fmodProcess", self, {dbb});
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* a = j.get(arr)) std::memcpy(a->data.data(), x.mem.host(buf), bytes);
        }
        j.invoke(x, write, track, {arr, 0, uint32_t(len) * 2});
    }
    j.invoke(x, j.methodId("android/media/AudioTrack", "stop", "()V", false), track, {});
    j.invoke(x, j.methodId("android/media/AudioTrack", "release", "()V", false), track, {});
}

} // namespace

void registerFmodBridge(Process& p) { p.def<&fmodThread>("__hvm_fmod_audio"); }

JValue invokeProxy(Jni& j, Ctx& x, jobject self, const std::string& name, const std::string& sig,
                   const std::vector<JValue>& args) {
    uint64_t ptr = 0;
    std::string iface;
    {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* o = j.get(self);
        if (!o) return 0;
        ptr = o->fields["hvm.proxy"];
        iface = o->cls;
    }
    if (name == "hashCode") return JValue(self);
    if (name == "equals") return JValue(args.empty() ? 0 : (jobject(args[0]) == self ? 1 : 0));
    if (name == "toString") return JValue(j.newString(iface + "$Proxy"));
    const jobject method = j.newObject("java/lang/reflect/Method");
    const std::string types = argTypes(sig);
    jobject array = 0;
    if (!types.empty()) {
        array = j.newArray('L', uint32_t(types.size()));
        for (size_t i = 0; i < types.size() && i < args.size(); ++i) {
            const jobject b = boxed(j, types[i], args[i]);
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* a = j.get(array)) a->refs[i] = b;
        }
    }
    {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* m = j.get(method)) {
            m->str = name;
            m->fields["hvm.declaring"] = j.classFor(iface);
            m->host = sig;
        }
    }
    JValue r = 0;
    j.callNative(x, "bitter/jnibridge/JNIBridge", "invoke", j.classFor("bitter/jnibridge/JNIBridge"),
                 {uint32_t(ptr), uint32_t(ptr >> 32), method, array}, &r);
    // A primitive result comes back boxed.
    const char rt = returnType(sig);
    if (rt != 'L' && rt != '[' && rt != 'V' && r) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* b = j.get(jobject(r))) return b->fields["value"];
        return 0;
    }
    return rt == 'V' ? 0 : r;
}

void registerJavaServices(Jni& j) {
    Process& p = j.proc;
    const Surface surf = surfaceFor(p);

    // android.os.Build: a generic ARM phone on Android 4.4.
    const std::map<std::string, std::string> build = {
        {"MODEL", "HydrogenVM"}, {"MANUFACTURER", "HydrogenVM"}, {"BRAND", "hydrogen"}, {"DEVICE", "hvm"},
        {"PRODUCT", "hvm"}, {"BOARD", "hvm"}, {"HARDWARE", "hvm"}, {"ID", "KTU84P"}, {"DISPLAY", "KTU84P"},
        {"CPU_ABI", "armeabi-v7a"}, {"CPU_ABI2", "armeabi"}, {"SERIAL", "unknown"}, {"TAGS", "release-keys"},
        {"TYPE", "user"}, {"HOST", "hydrogenvm"}, {"USER", "hvm"}, {"BOOTLOADER", "unknown"}, {"RADIO", "unknown"},
        {"FINGERPRINT", "hydrogen/hvm/hvm:4.4.4/KTU84P/1:user/release-keys"}};
    for (const auto& [k, v] : build) j.staticFields["android/os/Build." + k] = permanentString(j, v);
    int sdkInt = 19;
    {
        std::ifstream minIn(p.config().bundlePath + "/AndroidManifest.xml", std::ios::binary);
        if (minIn) {
            const std::vector<uint8_t> xml((std::istreambuf_iterator<char>(minIn)), {});
            const int minSdk = Loader::parseManifest(xml).minSdkVersion;
            if (minSdk > sdkInt) sdkInt = minSdk;
        }
    }
    j.staticFields["android/os/Build$VERSION.SDK_INT"] = uint64_t(sdkInt);
    j.staticFields["android/os/Build$VERSION.SDK"] = permanentString(j, std::to_string(sdkInt));
    j.staticFields["android/os/Build$VERSION.RELEASE"] = permanentString(j, sdkInt >= 21 ? "5.0" : "4.4.4");
    j.staticFields["android/os/Build$VERSION.INCREMENTAL"] = permanentString(j, "1");
    j.staticFields["android/os/Build$VERSION.CODENAME"] = permanentString(j, "REL");

    // Context.*_SERVICE names and the services HydrogenVM has.
    const std::map<std::string, std::string> services = {
        {"WINDOW_SERVICE", "window"}, {"DISPLAY_SERVICE", "display"}, {"TELEPHONY_SERVICE", "phone"},
        {"CONNECTIVITY_SERVICE", "connectivity"}, {"VIBRATOR_SERVICE", "vibrator"}, {"AUDIO_SERVICE", "audio"},
        {"POWER_SERVICE", "power"}, {"SENSOR_SERVICE", "sensor"}, {"ACTIVITY_SERVICE", "activity"},
        {"WIFI_SERVICE", "wifi"}, {"INPUT_METHOD_SERVICE", "input_method"}, {"LOCATION_SERVICE", "location"},
        {"MEDIA_ROUTER_SERVICE", "media_router"}, {"KEYGUARD_SERVICE", "keyguard"}, {"INPUT_SERVICE", "input"},
        {"UI_MODE_SERVICE", "uimode"}, {"STORAGE_SERVICE", "storage"}, {"NOTIFICATION_SERVICE", "notification"},
        {"ALARM_SERVICE", "alarm"}, {"CLIPBOARD_SERVICE", "clipboard"}, {"ACCOUNT_SERVICE", "account"},
        {"LAYOUT_INFLATER_SERVICE", "layout_inflater"}, {"VIBRATOR_MANAGER_SERVICE", "vibrator_manager"}};
    for (const auto& [k, v] : services) j.staticFields["android/content/Context." + k] = permanentString(j, v);
    const std::map<std::string, std::string> serviceClass = {
        {"window", "android/view/WindowManager"}, {"display", "android/hardware/display/DisplayManager"},
        {"phone", "android/telephony/TelephonyManager"}, {"connectivity", "android/net/ConnectivityManager"},
        {"vibrator", "android/os/Vibrator"}, {"vibrator_manager", "android/os/VibratorManager"},
        {"audio", "android/media/AudioManager"}, {"sensor", "android/hardware/SensorManager"},
        {"power", "android/os/PowerManager"}, {"activity", "android/app/ActivityManager"},
        {"media_router", "android/media/MediaRouter"}, {"input_method", "android/view/inputmethod/InputMethodManager"},
        {"uimode", "android/app/UiModeManager"}, {"keyguard", "android/app/KeyguardManager"},
        {"input", "android/hardware/input/InputManager"}, {"clipboard", "android/content/ClipboardManager"},
        {"layout_inflater", "android/view/LayoutInflater"}};
    for (const char* ctx : {"android/content/Context", "android/app/Activity"}) {
        j.method(ctx, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;", [&p, serviceClass](Jni& j, Ctx&, jobject self, const auto& a) {
            const std::string name = str(j, a[0]);
            auto it = serviceClass.find(name);
            if (it == serviceClass.end()) {
                p.reportDefaulted("Context.getSystemService(\"" + name + "\") -> null");
                return JValue(0);
            }
            const jobject svc = singleton(j, it->second);
            if (name == "layout_inflater" && self) {
                std::lock_guard<std::recursive_mutex> lk(j.mu);
                if (JObject* o = j.get(svc)) o->fields["hvm.context"] = self;
            }
            return JValue(svc);
        });
        j.method(ctx, "getIntent", "()Landroid/content/Intent;", [](Jni& j, Ctx&, jobject, const auto&) {
            return JValue(singleton(j, "android/content/Intent"));
        });
        j.method(ctx, "getPackageManager", "()Landroid/content/pm/PackageManager;", [](Jni& j, Ctx&, jobject, const auto&) {
            return JValue(singleton(j, "android/content/pm/PackageManager"));
        });
        j.method(ctx, "getContentResolver", "()Landroid/content/ContentResolver;", [](Jni& j, Ctx&, jobject, const auto&) {
            return JValue(singleton(j, "android/content/ContentResolver"));
        });
        j.method(ctx, "getWindowManager", "()Landroid/view/WindowManager;", [](Jni& j, Ctx&, jobject, const auto&) {
            return JValue(singleton(j, "android/view/WindowManager"));
        });
        j.method(ctx, "getWindow", "()Landroid/view/Window;", [](Jni& j, Ctx&, jobject, const auto&) {
            return JValue(singleton(j, "android/view/Window"));
        });
        j.method(ctx, "getRequestedOrientation", "()I", [surf](Jni&, Ctx&, jobject, const auto&) {
            return JValue(surf.portrait ? 1 : 0);
        });
        j.method(ctx, "checkCallingOrSelfPermission", "(Ljava/lang/String;)I", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
        j.method(ctx, "checkSelfPermission", "(Ljava/lang/String;)I", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
        // Runs on the calling thread: the game's UI work happens when it asks.
        j.method(ctx, "runOnUiThread", "(Ljava/lang/Runnable;)V", [](Jni& j, Ctx& x, jobject, const auto& a) {
            const jobject r = jobject(a[0]);
            if (!r) return JValue(0);
            const uint32_t id = j.methodId("java/lang/Runnable", "run", "()V", false);
            j.invoke(x, id, r, {});
            return JValue(0);
        });
        j.method(ctx, "getSharedPreferences", "(Ljava/lang/String;I)Landroid/content/SharedPreferences;", [&p](Jni& j, Ctx&, jobject, const auto& a) {
            std::string name = str(j, a[0]);
            for (char& c : name) if (c == '/') c = '_';
            const std::string key = "hvm.prefs." + name;
            {
                std::lock_guard<std::recursive_mutex> lk(j.mu);
                if (auto it = j.staticFields.find(key); it != j.staticFields.end()) return JValue(it->second);
            }
            auto pr = std::make_shared<Prefs>();
            pr->file = (fs::path(p.homePath()) / "Library/shared_prefs" / (name + ".prefs")).string();
            loadPrefs(*pr);
            const jobject h = j.newObject("android/content/SharedPreferences");
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* o = j.get(h)) { o->permanent = true; o->host = pr; }
            j.staticFields[key] = h;
            return JValue(h);
        });
    }
    j.method("android/app/Activity", "getPreferences", "(I)Landroid/content/SharedPreferences;", [](Jni& j, Ctx& x, jobject self, const auto&) {
        const uint32_t id = j.methodId("android/content/Context", "getSharedPreferences", "(Ljava/lang/String;I)Landroid/content/SharedPreferences;", false);
        return j.invoke(x, id, self, {JValue(j.newString("activity")), 0});
    });

    // SharedPreferences and its Editor (the editor writes through; commit / apply save).
    const std::string kSp = "android/content/SharedPreferences", kEd = "android/content/SharedPreferences$Editor";
    j.superclasses["android/content/SharedPreferences$Editor"] = "java/lang/Object";
    auto getter = [&j, kSp](const char* name, const char* sig, char type) {
        j.method(kSp, name, sig, [type](Jni& j, Ctx&, jobject self, const auto& a) -> JValue {
            auto pr = prefsOf(j, self);
            const std::string key = str(j, a[0]);
            if (!pr) return a.size() > 1 ? a[1] : 0;
            auto it = pr->values.find(key);
            if (it == pr->values.end() || it->second.first != type) return a[1];
            const std::string& v = it->second.second;
            switch (type) {
                case 'S': return JValue(j.newString(v));
                case 'Z': return JValue(v == "true" ? 1 : 0);
                case 'I': return JValue(uint32_t(std::stol(v)));
                case 'J': return JValue(uint64_t(std::stoll(v)));
                case 'F': return jfloat(std::stof(v));
            }
            return 0;
        });
    };
    getter("getString", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", 'S');
    getter("getInt", "(Ljava/lang/String;I)I", 'I');
    getter("getLong", "(Ljava/lang/String;J)J", 'J');
    getter("getFloat", "(Ljava/lang/String;F)F", 'F');
    getter("getBoolean", "(Ljava/lang/String;Z)Z", 'Z');
    j.method(kSp, "contains", "(Ljava/lang/String;)Z", [](Jni& j, Ctx&, jobject self, const auto& a) {
        auto pr = prefsOf(j, self);
        return JValue(pr && pr->values.count(str(j, a[0])) ? 1 : 0);
    });
    j.method(kSp, "edit", "()Landroid/content/SharedPreferences$Editor;", [kEd](Jni& j, Ctx&, jobject self, const auto&) {
        auto pr = prefsOf(j, self);
        const jobject e = j.newObject(kEd);
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(e)) o->host = pr;
        return JValue(e);
    });
    auto putter = [&j, kEd](const char* name, const char* sig, char type) {
        j.method(kEd, name, sig, [type](Jni& j, Ctx&, jobject self, const auto& a) -> JValue {
            if (auto pr = prefsOf(j, self)) {
                std::string v;
                switch (type) {
                    case 'S': v = str(j, a[1]); break;
                    case 'Z': v = a[1] ? "true" : "false"; break;
                    case 'I': v = std::to_string(int32_t(a[1])); break;
                    case 'J': v = std::to_string(int64_t(a[1])); break;
                    case 'F': { std::ostringstream os; os.precision(9); os << asFloat(a[1]); v = os.str(); break; }
                }
                if (type == 'S' && !a[1]) pr->values.erase(str(j, a[0]));
                else pr->values[str(j, a[0])] = {type, v};
            }
            return JValue(self);
        });
    };
    putter("putString", "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/SharedPreferences$Editor;", 'S');
    putter("putInt", "(Ljava/lang/String;I)Landroid/content/SharedPreferences$Editor;", 'I');
    putter("putLong", "(Ljava/lang/String;J)Landroid/content/SharedPreferences$Editor;", 'J');
    putter("putFloat", "(Ljava/lang/String;F)Landroid/content/SharedPreferences$Editor;", 'F');
    putter("putBoolean", "(Ljava/lang/String;Z)Landroid/content/SharedPreferences$Editor;", 'Z');
    j.method(kEd, "remove", "(Ljava/lang/String;)Landroid/content/SharedPreferences$Editor;", [](Jni& j, Ctx&, jobject self, const auto& a) {
        if (auto pr = prefsOf(j, self)) pr->values.erase(str(j, a[0]));
        return JValue(self);
    });
    j.method(kEd, "clear", "()Landroid/content/SharedPreferences$Editor;", [](Jni& j, Ctx&, jobject self, const auto&) {
        if (auto pr = prefsOf(j, self)) pr->values.clear();
        return JValue(self);
    });
    j.method(kEd, "commit", "()Z", [](Jni& j, Ctx&, jobject self, const auto&) {
        if (auto pr = prefsOf(j, self)) savePrefs(*pr);
        return JValue(1);
    });
    j.method(kEd, "apply", "()V", [](Jni& j, Ctx&, jobject self, const auto&) {
        if (auto pr = prefsOf(j, self)) savePrefs(*pr);
        return JValue(0);
    });

    // The launch Intent: MAIN, no data, no extras.
    const std::string kIntent = "android/content/Intent";
    j.method(kIntent, "getAction", "()Ljava/lang/String;", [](Jni& j, Ctx&, jobject, const auto&) {
        return JValue(j.newString("android.intent.action.MAIN"));
    });
    j.quietClasses.insert(kIntent);   // getData, getExtras, ...: null
    j.method(kIntent, "getStringExtra", "(Ljava/lang/String;)Ljava/lang/String;", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kIntent, "getDataString", "()Ljava/lang/String;", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kIntent, "getBooleanExtra", "(Ljava/lang/String;Z)Z", [](Jni&, Ctx&, jobject, const auto& a) { return a[1]; });
    j.method(kIntent, "getIntExtra", "(Ljava/lang/String;I)I", [](Jni&, Ctx&, jobject, const auto& a) { return a[1]; });

    // Window / display: one display the size of the game's surface, 60 Hz, 320 dpi.
    const std::string kWm = "android/view/WindowManager", kDisp = "android/view/Display", kDm = "android/util/DisplayMetrics";
    j.method(kWm, "getDefaultDisplay", "()Landroid/view/Display;", [kDisp](Jni& j, Ctx&, jobject, const auto&) {
        return JValue(singleton(j, kDisp));
    });
    j.method("android/hardware/display/DisplayManager", "getDisplay", "(I)Landroid/view/Display;", [kDisp](Jni& j, Ctx&, jobject, const auto&) {
        return JValue(singleton(j, kDisp));
    });
    j.method("android/hardware/display/DisplayManager", "getDisplays", "()[Landroid/view/Display;", [kDisp](Jni& j, Ctx&, jobject, const auto&) {
        const jobject arr = j.newArray('L', 1);
        const jobject d = singleton(j, kDisp);
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* a = j.get(arr)) a->refs[0] = d;
        return JValue(arr);
    });
    auto fillMetrics = [surf](Jni& j, jobject m) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* o = j.get(m);
        if (!o) return;
        o->fields["widthPixels"] = uint32_t(surf.width);
        o->fields["heightPixels"] = uint32_t(surf.height);
        o->fields["densityDpi"] = 320;
        o->fields["density"] = jfloat(2.0f);
        o->fields["scaledDensity"] = jfloat(2.0f);
        o->fields["xdpi"] = jfloat(320.0f);
        o->fields["ydpi"] = jfloat(320.0f);
    };
    for (const char* n : {"getMetrics", "getRealMetrics"})
        j.method(kDisp, n, "(Landroid/util/DisplayMetrics;)V", [fillMetrics](Jni& j, Ctx&, jobject, const auto& a) {
            fillMetrics(j, jobject(a[0]));
            return JValue(0);
        });
    j.method(kDm, "<init>", "()V", [fillMetrics](Jni& j, Ctx&, jobject self, const auto&) { fillMetrics(j, self); return JValue(0); });
    for (const char* n : {"getWidth"}) j.method(kDisp, n, "()I", [surf](Jni&, Ctx&, jobject, const auto&) { return JValue(uint32_t(surf.width)); });
    for (const char* n : {"getHeight"}) j.method(kDisp, n, "()I", [surf](Jni&, Ctx&, jobject, const auto&) { return JValue(uint32_t(surf.height)); });
    for (const char* n : {"getRotation", "getOrientation", "getDisplayId"})
        j.method(kDisp, n, "()I", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kDisp, "getRefreshRate", "()F", [](Jni&, Ctx&, jobject, const auto&) { return jfloat(60.0f); });
    j.method(kDisp, "getName", "()Ljava/lang/String;", [](Jni& j, Ctx&, jobject, const auto&) { return JValue(j.newString("Built-in Screen")); });
    j.method("android/content/res/Resources", "getDisplayMetrics", "()Landroid/util/DisplayMetrics;", [fillMetrics, kDm](Jni& j, Ctx&, jobject, const auto&) {
        const jobject m = j.newObject(kDm);
        fillMetrics(j, m);
        return JValue(m);
    });
    for (const char* ctx : {"android/content/Context", "android/app/Activity"})
        j.method(ctx, "getResources", "()Landroid/content/res/Resources;", [](Jni& j, Ctx&, jobject, const auto&) {
            return JValue(singleton(j, "android/content/res/Resources"));
        });

    // Telephony: no SIM; connectivity: offline (no active network); vibrator: none.
    const std::string kTm = "android/telephony/TelephonyManager";
    for (const char* n : {"getNetworkOperatorName", "getNetworkOperator", "getSimOperator", "getSimOperatorName"})
        j.method(kTm, n, "()Ljava/lang/String;", [](Jni& j, Ctx&, jobject, const auto&) { return JValue(j.newString("")); });
    for (const char* n : {"getNetworkCountryIso", "getSimCountryIso"})
        j.method(kTm, n, "()Ljava/lang/String;", [](Jni& j, Ctx&, jobject, const auto&) { return JValue(j.newString("us")); });
    j.method(kTm, "getDeviceId", "()Ljava/lang/String;", [](Jni& j, Ctx&, jobject, const auto&) { return JValue(j.newString("000000000000000")); });
    for (const char* n : {"getPhoneType", "getNetworkType", "getSimState", "getCallState", "getDataState"})
        j.method(kTm, n, "()I", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.quietClasses.insert("android/net/ConnectivityManager");   // getActiveNetworkInfo -> null: offline
    j.quietClasses.insert("android/net/NetworkInfo");
    j.quietClasses.insert("android/os/Vibrator");               // hasVibrator false, vibrate does nothing
    j.quietClasses.insert("android/media/MediaRouter");
    j.quietClasses.insert("android/app/ApplicationErrorReport");
    j.quietClasses.insert("android/view/Window");
    j.quietClasses.insert("android/os/PowerManager");
    j.quietClasses.insert("android/view/inputmethod/InputMethodManager");

    // Settings: a fixed android_id.
    for (const char* cls : {"android/provider/Settings$Secure", "android/provider/Settings$System"})
        j.method(cls, "getString", "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;", [](Jni& j, Ctx&, jobject, const auto& a) {
            return JValue(str(j, a[1]) == "android_id" ? j.newString("4879647263656e00") : jobject(0));
        });
    j.staticFields["android/provider/Settings$Secure.ANDROID_ID"] = permanentString(j, "android_id");

    // PackageManager: the game's own package, version 1.0; nothing else installed.
    const std::string kPm = "android/content/pm/PackageManager";
    j.method(kPm, "getPackageInfo", "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;", [&p](Jni& j, Ctx&, jobject, const auto& a) {
        if (str(j, a[0]) != packageName(p)) return JValue(0);
        const jobject pi = j.newObject("android/content/pm/PackageInfo");
        const jobject name = j.newString(packageName(p));
        const jobject ver = j.newString(p.infoPlist() ? p.infoPlist()->getString("CFBundleShortVersionString", "1.0") : std::string("1.0"));
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(pi)) {
            o->fields["packageName"] = name;
            o->fields["versionName"] = ver;
            o->fields["versionCode"] = 1;
            o->fields["firstInstallTime"] = 0;
            o->fields["lastUpdateTime"] = 0;
        }
        return JValue(pi);
    });
    j.method(kPm, "getApplicationInfo", "(Ljava/lang/String;I)Landroid/content/pm/ApplicationInfo;", [&p](Jni& j, Ctx&, jobject, const auto&) {
        const jobject ai = j.newObject("android/content/pm/ApplicationInfo");
        const jobject dir = j.newString(p.guestPath(p.config().bundlePath) + "/base.apk");
        const jobject lib = j.newString(p.guestPath(p.config().bundlePath) + "/lib/armeabi-v7a");
        const jobject data = j.newString(p.guestPath(p.homePath()));
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(ai)) {
            o->fields["sourceDir"] = dir;
            o->fields["publicSourceDir"] = dir;
            o->fields["nativeLibraryDir"] = lib;
            o->fields["dataDir"] = data;
            o->fields["targetSdkVersion"] = 19;
        }
        return JValue(ai);
    });
    j.method(kPm, "hasSystemFeature", "(Ljava/lang/String;)Z", [](Jni& j, Ctx&, jobject, const auto& a) {
        const std::string f = str(j, a[0]);
        return JValue(f.rfind("android.hardware.touchscreen", 0) == 0 || f == "android.hardware.opengles.aep" ? 1 : 0);
    });
    j.method(kPm, "checkPermission", "(Ljava/lang/String;Ljava/lang/String;)I", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kPm, "resolveActivity", "(Landroid/content/Intent;I)Landroid/content/pm/ResolveInfo;", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kPm, "getInstallerPackageName", "(Ljava/lang/String;)Ljava/lang/String;", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });

    // java.util.Locale: en_US.
    const std::string kLoc = "java/util/Locale";
    j.method(kLoc, "getDefault", "()Ljava/util/Locale;", [kLoc](Jni& j, Ctx&, jobject, const auto&) { return JValue(singleton(j, kLoc)); });
    j.method(kLoc, "getLanguage", "()Ljava/lang/String;", [](Jni& j, Ctx&, jobject, const auto&) { return JValue(j.newString("en")); });
    j.method(kLoc, "getCountry", "()Ljava/lang/String;", [](Jni& j, Ctx&, jobject, const auto&) { return JValue(j.newString("US")); });
    j.method(kLoc, "toString", "()Ljava/lang/String;", [](Jni& j, Ctx&, jobject, const auto&) { return JValue(j.newString("en_US")); });
    j.method(kLoc, "getISO3Language", "()Ljava/lang/String;", [](Jni& j, Ctx&, jobject, const auto&) { return JValue(j.newString("eng")); });
    j.method(kLoc, "getISO3Country", "()Ljava/lang/String;", [](Jni& j, Ctx&, jobject, const auto&) { return JValue(j.newString("USA")); });

    // java.lang.reflect.Method (proxy calls), and JNIBridge itself.
    const std::string kMeth = "java/lang/reflect/Method";
    j.method(kMeth, "getName", "()Ljava/lang/String;", [](Jni& j, Ctx&, jobject self, const auto&) {
        std::string n;
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* o = j.get(self)) n = o->str;
        }
        return JValue(j.newString(n));
    });
    j.method(kMeth, "getDeclaringClass", "()Ljava/lang/Class;", [](Jni& j, Ctx&, jobject self, const auto&) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        return JValue(field(j, self, "hvm.declaring"));
    });
    j.method(kMeth, "getParameterTypes", "()[Ljava/lang/Class;", [](Jni& j, Ctx&, jobject self, const auto&) {
        std::string sig;
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* o = j.get(self)) if (auto* s = std::any_cast<std::string>(&o->host)) sig = *s;
        }
        const std::string t = argTypes(sig);
        return JValue(j.newArray('L', uint32_t(t.size())));
    });
    j.method(kMeth, "hashCode", "()I", [](Jni& j, Ctx& x, jobject self, const auto&) {
        std::string n;
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* o = j.get(self)) n = o->str;
        }
        (void)x;
        uint32_t h = 0;
        for (char c : n) h = h * 31 + uint8_t(c);   // String.hashCode of the name
        return JValue(h);
    });
    j.method(kMeth, "toString", "()Ljava/lang/String;", [](Jni& j, Ctx&, jobject self, const auto&) {
        std::string n;
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* o = j.get(self)) n = o->str;
        }
        return JValue(j.newString("public abstract void " + n + "()"));
    });
    const std::string kBridge = "bitter/jnibridge/JNIBridge";
    j.method(kBridge, "newInterfaceProxy", "(JLjava/lang/Class;)Ljava/lang/Object;", [](Jni& j, Ctx&, jobject, const auto& a) {
        std::string iface = "java/lang/Object";
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* c = j.get(jobject(a[1]))) iface = c->cls;
        }
        const jobject h = j.newObject(iface);
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(h)) o->fields["hvm.proxy"] = a[0];
        return JValue(h);
    });
    j.method(kBridge, "disableInterfaceProxy", "(Ljava/lang/Object;)V", [](Jni& j, Ctx&, jobject, const auto& a) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(jobject(a[0]))) o->fields["hvm.proxy"] = 0;
        return JValue(0);
    });

    // Unity's Java side: input devices (none beyond the touchscreen), splash and indicator.
    j.method("android/view/InputDevice", "getDeviceIds", "()[I", [](Jni& j, Ctx&, jobject, const auto&) {
        return JValue(j.newArray('I', 0));
    });
    for (const char* n : {"setFullscreen"})
        j.method("com/unity3d/player/UnityPlayer", n, "(Z)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    for (const char* n : {"startActivityIndicator", "stopActivityIndicator"})
        j.method("com/unity3d/player/UnityPlayer", n, "()V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("com/unity3d/player/UnityPlayer", "getSplashMode", "()I", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    for (const char* ctx : {"android/content/Context", "android/app/Activity"})
        j.method(ctx, "setRequestedOrientation", "(I)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    // ReflectionHelper.getMethodID(class, name, signature, isStatic): a Method for FromReflectedMethod.
    j.method("com/unity3d/player/ReflectionHelper", "getMethodID", "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/String;Z)Ljava/lang/reflect/Method;",
             [](Jni& j, Ctx&, jobject, const auto& a) {
        const std::string name = str(j, a[1]), sig = str(j, a[2]);
        const jobject m = j.newObject("java/lang/reflect/Method");
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(m)) { o->str = name; o->host = sig; o->fields["hvm.declaring"] = a[0]; o->fields["hvm.static"] = a[3] & 0xFF; }
        return JValue(m);
    });
    // Class.forName: every class "exists". A game's Java-side plugins (ads, purchases, social
    // SDKs) have no host implementation; their methods answer 0 / null (reported as missing), so
    // the game's own setup code carries on instead of throwing (Unity's AndroidJavaClass calls
    // forName through reflection, with an Object return type).
    for (const char* sig : {"(Ljava/lang/String;)Ljava/lang/Class;", "(Ljava/lang/String;)Ljava/lang/Object;"})
        j.method("java/lang/Class", "forName", sig, [](Jni& j, Ctx&, jobject, const auto& a) {
            std::string name = str(j, a[0]);
            for (char& c : name) if (c == '.') c = '/';
            return JValue(j.classFor(name));
        });

    // ReflectionHelper.getFieldID(class, name, signature, isStatic): a Field for FromReflectedField.
    j.method("com/unity3d/player/ReflectionHelper", "getFieldID", "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/String;Z)Ljava/lang/reflect/Field;",
             [](Jni& j, Ctx&, jobject, const auto& a) {
        const std::string name = str(j, a[1]), sig = str(j, a[2]);
        const jobject f = j.newObject("java/lang/reflect/Field");
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(f)) { o->str = name; o->host = sig; o->fields["hvm.declaring"] = a[0]; o->fields["hvm.static"] = a[3] & 0xFF; }
        return JValue(f);
    });
    j.method("java/lang/Object", "getClass", "()Ljava/lang/Class;", [](Jni& j, Ctx&, jobject self, const auto&) {
        std::string cls = "java/lang/Object";
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* o = j.get(self)) cls = o->isClass ? "java/lang/Class" : o->cls;
        }
        return JValue(j.classFor(cls));
    });
    // UnityPlayer.currentActivity, and its loadLibrary (the game's libraries are already loaded).
    j.method("com/unity3d/player/UnityPlayer", "loadLibrary", "(Ljava/lang/String;)Z", [](Jni&, Ctx&, jobject, const auto&) { return JValue(1); });

    // Libraries: everything the game ships is loaded at start, so loading one again succeeds and
    // findLibrary names its file.
    for (const char* sig : {"(Ljava/lang/String;)V"}) {
        j.method("java/lang/System", "load", sig, [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
        j.method("java/lang/System", "loadLibrary", sig, [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    }
    j.method("java/lang/ClassLoader", "findLibrary", "(Ljava/lang/String;)Ljava/lang/String;", [&p](Jni& j, Ctx&, jobject, const auto& a) {
        const std::string dir = p.guestPath(p.service<Library>().libDir);
        return JValue(j.newString(dir + "/lib" + str(j, a[0]) + ".so"));
    });
    j.method("java/lang/Class", "getName", "()Ljava/lang/String;", [](Jni& j, Ctx&, jobject self, const auto&) {
        std::string n;
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* o = j.get(self)) n = o->cls;
        }
        for (char& c : n) if (c == '/') c = '.';
        return JValue(j.newString(n));
    });
    for (const char* ctx : {"android/content/Context", "android/app/Activity"})
        j.method(ctx, "getClassLoader", "()Ljava/lang/ClassLoader;", [](Jni& j, Ctx&, jobject, const auto&) {
            return JValue(singleton(j, "java/lang/ClassLoader"));
        });

    // FMOD's Java audio device.
    const std::string kFmod = "org/fmod/FMODAudioDevice";
    j.method(kFmod, "start", "()V", [&p](Jni& j, Ctx& x, jobject self, const auto&) {
        const std::string runKey = "hvm.fmod.running." + std::to_string(self);
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (j.staticFields[runKey]) return JValue(0);
            j.staticFields[runKey] = 1;
            if (JObject* o = j.get(self)) o->permanent = true;
        }
        if (auto fn = p.lookupSymbol("__hvm_fmod_audio")) x.proc.startThread(*fn, self, 512u << 10, true);
        return JValue(0);
    });
    for (const char* n : {"stop", "close"})
        j.method(kFmod, n, "()V", [](Jni& j, Ctx&, jobject self, const auto&) {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            j.staticFields["hvm.fmod.running." + std::to_string(self)] = 0;
            return JValue(0);
        });
    j.method(kFmod, "isRunning", "()Z", [](Jni& j, Ctx&, jobject self, const auto&) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        return JValue(j.staticFields["hvm.fmod.running." + std::to_string(self)] ? 1 : 0);
    });
    // No microphone.
    j.method(kFmod, "startAudioRecord", "(III)I", [](Jni&, Ctx&, jobject, const auto&) { return JValue(uint32_t(-1)); });
    j.method(kFmod, "stopAudioRecord", "()V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });

    // Boxed primitives the proxies and games use.
    for (const auto& [cls, sig] : std::vector<std::pair<std::string, std::string>>{
             {"java/lang/Integer", "I"}, {"java/lang/Long", "J"}, {"java/lang/Float", "F"}, {"java/lang/Double", "D"},
             {"java/lang/Boolean", "Z"}, {"java/lang/Short", "S"}, {"java/lang/Byte", "B"}, {"java/lang/Character", "C"}}) {
        static const std::map<char, std::string> getterName = {{'I', "intValue"}, {'J', "longValue"}, {'F', "floatValue"},
            {'D', "doubleValue"}, {'Z', "booleanValue"}, {'S', "shortValue"}, {'B', "byteValue"}, {'C', "charValue"}};
        j.method(cls, getterName.at(sig[0]), "()" + sig, [](Jni& j, Ctx&, jobject self, const auto&) {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            return JValue(field(j, self, "value"));
        });
        j.method(cls, "<init>", "(" + sig + ")V", [](Jni& j, Ctx&, jobject self, const auto& a) {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            field(j, self, "value") = a[0];
            return JValue(0);
        });
        j.method(cls, "valueOf", "(" + sig + ")L" + cls + ";", [cls = cls](Jni& j, Ctx&, jobject, const auto& a) {
            const jobject b = j.newObject(cls);
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            field(j, b, "value") = a[0];
            return JValue(b);
        });
    }

    // android.media.MediaPlayer: streams long files on the shared mixer, decodes short ones.
    struct MediaPlayerState {
        std::mutex mu;
        uint32_t voice = 0;
        std::shared_ptr<Audio::AudioSource> source;
        Audio::Pcm pcm;
        int sampleRate = 44100;
        double durationSec = 0.0;
        double positionSec = 0.0;
        float leftVol = 1.0f, rightVol = 1.0f;
        bool looping = false;
        bool prepared = false;
        bool playing = false;
        std::string dataPath;
        int fd = -1;
        int64_t fdOffset = 0, fdLength = -1;
        jobject onPrepared = 0, onCompletion = 0, onError = 0, onSeekComplete = 0;
        ~MediaPlayerState() {
            if (fd >= 0) ::close(fd);
        }
        Audio::VoiceParams params() const {
            Audio::VoiceParams vp;
            vp.gain = 0.5f * (leftVol + rightVol);
            const float sum = leftVol + rightVol;
            vp.pan = sum > 0.0f ? std::clamp((rightVol - leftVol) / sum, -1.0f, 1.0f) : 0.0f;
            vp.loop = looping;
            return vp;
        }
    };
    auto mpState = [](Jni& j, jobject self) -> std::shared_ptr<MediaPlayerState> {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* o = j.get(self);
        if (!o) return nullptr;
        auto* s = std::any_cast<std::shared_ptr<MediaPlayerState>>(&o->host);
        return s ? *s : nullptr;
    };
    auto mpLoad = [](Process& proc, MediaPlayerState& mp) -> bool {
        if (mp.prepared) return true;
        mp.source.reset();
        mp.pcm.reset();
        mp.durationSec = 0.0;
        if (!mp.dataPath.empty()) {
            std::string path = mp.dataPath;
            if (path.rfind("file://", 0) == 0) path.erase(0, 7);
            std::string host = proc.hostPath(path);
            if (!fs::exists(host)) {
                const std::string asset = proc.config().bundlePath + "/assets/" + path;
                if (fs::exists(asset)) host = asset;
            }
            if (auto src = Audio::openStream(host)) {
                mp.source = std::move(src);
                mp.sampleRate = mp.source->sampleRate();
                mp.durationSec = mp.sampleRate > 0 ? double(mp.source->frames()) / mp.sampleRate : 0.0;
                mp.prepared = true;
                return true;
            }
            std::vector<uint8_t> bytes = readHostBytes(host);
            if (!bytes.empty()) {
                Audio::DecodedAudio dec = Audio::decode(bytes.data(), bytes.size());
                if (!dec.stereoPcm.empty()) {
                    mp.pcm = std::make_shared<std::vector<int16_t>>(std::move(dec.stereoPcm));
                    mp.sampleRate = dec.sampleRate > 0 ? dec.sampleRate : 44100;
                    mp.durationSec = dec.duration > 0.0 ? dec.duration : double(mp.pcm->size() / 2) / mp.sampleRate;
                    mp.prepared = true;
                    return true;
                }
            }
        } else if (mp.fd >= 0) {
            std::vector<uint8_t> bytes = readFdBytes(mp.fd, mp.fdOffset, mp.fdLength);
            if (!bytes.empty()) {
                if (auto src = Audio::openStream(bytes)) {
                    mp.source = std::move(src);
                    mp.sampleRate = mp.source->sampleRate();
                    mp.durationSec = mp.sampleRate > 0 ? double(mp.source->frames()) / mp.sampleRate : 0.0;
                    mp.prepared = true;
                    return true;
                }
                Audio::DecodedAudio dec = Audio::decode(bytes.data(), bytes.size());
                if (!dec.stereoPcm.empty()) {
                    mp.pcm = std::make_shared<std::vector<int16_t>>(std::move(dec.stereoPcm));
                    mp.sampleRate = dec.sampleRate > 0 ? dec.sampleRate : 44100;
                    mp.durationSec = dec.duration > 0.0 ? dec.duration : double(mp.pcm->size() / 2) / mp.sampleRate;
                    mp.prepared = true;
                    return true;
                }
            }
        }
        return false;
    };
    const std::string kMP = "android/media/MediaPlayer";
    j.method(kMP, "<init>", "()V", [](Jni& j, Ctx&, jobject self, const auto&) {
        auto mp = std::make_shared<MediaPlayerState>();
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) { o->host = mp; o->permanent = true; }
        return JValue(0);
    });
    j.method(kMP, "setDataSource", "(Ljava/lang/String;)V", [mpState](Jni& j, Ctx&, jobject self, const auto& a) {
        auto mp = mpState(j, self);
        if (!mp) return JValue(0);
        const std::string path = str(j, a[0]);
        std::lock_guard<std::mutex> lk(mp->mu);
        mp->dataPath = path;
        if (mp->fd >= 0) { ::close(mp->fd); mp->fd = -1; }
        mp->prepared = false;
        return JValue(0);
    });
    j.method(kMP, "setDataSource", "(Ljava/io/FileDescriptor;)V", [mpState](Jni& j, Ctx&, jobject self, const auto& a) {
        auto mp = mpState(j, self);
        if (!mp) return JValue(0);
        int rawFd = -1;
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* f = j.get(jobject(a[0]))) rawFd = int(f->fields["descriptor"]);
        }
        std::lock_guard<std::mutex> lk(mp->mu);
        if (mp->fd >= 0) ::close(mp->fd);
        mp->fd = rawFd >= 0 ? ::dup(rawFd) : -1;
        mp->fdOffset = 0;
        mp->fdLength = -1;
        mp->dataPath.clear();
        mp->prepared = false;
        return JValue(0);
    });
    j.method(kMP, "setDataSource", "(Ljava/io/FileDescriptor;JJ)V", [mpState](Jni& j, Ctx&, jobject self, const auto& a) {
        auto mp = mpState(j, self);
        if (!mp) return JValue(0);
        int rawFd = -1;
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* f = j.get(jobject(a[0]))) rawFd = int(f->fields["descriptor"]);
        }
        std::lock_guard<std::mutex> lk(mp->mu);
        if (mp->fd >= 0) ::close(mp->fd);
        mp->fd = rawFd >= 0 ? ::dup(rawFd) : -1;
        mp->fdOffset = int64_t(a[1]);
        mp->fdLength = int64_t(a[2]);
        mp->dataPath.clear();
        mp->prepared = false;
        return JValue(0);
    });
    j.method(kMP, "setDataSource", "(Landroid/content/res/AssetFileDescriptor;)V", [mpState](Jni& j, Ctx&, jobject self, const auto& a) {
        auto mp = mpState(j, self);
        if (!mp) return JValue(0);
        int rawFd = -1;
        int64_t len = -1;
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* afd = j.get(jobject(a[0]))) {
                len = int64_t(afd->fields["hvm.length"]);
                if (JObject* f = j.get(jobject(afd->fields["hvm.fd"]))) rawFd = int(f->fields["descriptor"]);
            }
        }
        std::lock_guard<std::mutex> lk(mp->mu);
        if (mp->fd >= 0) ::close(mp->fd);
        mp->fd = rawFd >= 0 ? ::dup(rawFd) : -1;
        mp->fdOffset = 0;
        mp->fdLength = len;
        mp->dataPath.clear();
        mp->prepared = false;
        return JValue(0);
    });
    j.method(kMP, "prepare", "()V", [mpState, mpLoad](Jni& j, Ctx& x, jobject self, const auto&) {
        auto mp = mpState(j, self);
        if (!mp) return JValue(0);
        std::lock_guard<std::mutex> lk(mp->mu);
        mpLoad(x.proc, *mp);
        return JValue(0);
    });
    j.method(kMP, "prepareAsync", "()V", [mpState, mpLoad](Jni& j, Ctx& x, jobject self, const auto&) {
        auto mp = mpState(j, self);
        if (!mp) return JValue(0);
        jobject listener = 0;
        bool ok = false;
        {
            std::lock_guard<std::mutex> lk(mp->mu);
            ok = mpLoad(x.proc, *mp);
            listener = mp->onPrepared;
        }
        if (ok && listener) {
            const uint32_t mid = j.methodId("android/media/MediaPlayer$OnPreparedListener", "onPrepared", "(Landroid/media/MediaPlayer;)V", false);
            j.invoke(x, mid, listener, {self});
        }
        return JValue(0);
    });
    j.method(kMP, "start", "()V", [mpState, mpLoad](Jni& j, Ctx& x, jobject self, const auto&) {
        auto mp = mpState(j, self);
        if (!mp) return JValue(0);
        std::lock_guard<std::mutex> lk(mp->mu);
        if (!mp->prepared) mpLoad(x.proc, *mp);
        auto& mixer = x.proc.mixer();
        if (mp->voice != 0) {
            if (mixer.playing(mp->voice) || mp->positionSec < mp->durationSec) {
                mixer.setParams(mp->voice, mp->params());
                mixer.setPaused(mp->voice, false);
                mp->playing = true;
                return JValue(0);
            }
            mixer.release(mp->voice);
            mp->voice = 0;
        }
        if (mp->source) {
            mp->voice = mixer.playSource(mp->source, mp->params(), mp->positionSec);
            mp->playing = mp->voice != 0;
        } else if (mp->pcm && !mp->pcm->empty()) {
            mp->voice = mixer.play(mp->pcm, mp->sampleRate, mp->params(), mp->positionSec, false);
            mp->playing = mp->voice != 0;
        }
        return JValue(0);
    });
    j.method(kMP, "pause", "()V", [mpState](Jni& j, Ctx& x, jobject self, const auto&) {
        auto mp = mpState(j, self);
        if (!mp) return JValue(0);
        std::lock_guard<std::mutex> lk(mp->mu);
        if (mp->voice != 0) {
            mp->positionSec = x.proc.mixer().positionSeconds(mp->voice);
            x.proc.mixer().setPaused(mp->voice, true);
        }
        mp->playing = false;
        return JValue(0);
    });
    j.method(kMP, "stop", "()V", [mpState](Jni& j, Ctx& x, jobject self, const auto&) {
        auto mp = mpState(j, self);
        if (!mp) return JValue(0);
        std::lock_guard<std::mutex> lk(mp->mu);
        if (mp->voice != 0) {
            x.proc.mixer().stop(mp->voice);
            x.proc.mixer().release(mp->voice);
            mp->voice = 0;
        }
        mp->playing = false;
        mp->positionSec = 0.0;
        return JValue(0);
    });
    j.method(kMP, "reset", "()V", [mpState](Jni& j, Ctx& x, jobject self, const auto&) {
        auto mp = mpState(j, self);
        if (!mp) return JValue(0);
        std::lock_guard<std::mutex> lk(mp->mu);
        if (mp->voice != 0) {
            x.proc.mixer().stop(mp->voice);
            x.proc.mixer().release(mp->voice);
            mp->voice = 0;
        }
        mp->source.reset();
        mp->pcm.reset();
        mp->playing = false;
        mp->prepared = false;
        mp->looping = false;
        mp->positionSec = 0.0;
        mp->durationSec = 0.0;
        mp->dataPath.clear();
        if (mp->fd >= 0) { ::close(mp->fd); mp->fd = -1; }
        return JValue(0);
    });
    j.method(kMP, "release", "()V", [mpState](Jni& j, Ctx& x, jobject self, const auto&) {
        auto mp = mpState(j, self);
        if (!mp) return JValue(0);
        std::lock_guard<std::mutex> lk(mp->mu);
        if (mp->voice != 0) {
            x.proc.mixer().stop(mp->voice);
            x.proc.mixer().release(mp->voice);
            mp->voice = 0;
        }
        mp->source.reset();
        mp->pcm.reset();
        mp->playing = false;
        mp->prepared = false;
        if (mp->fd >= 0) { ::close(mp->fd); mp->fd = -1; }
        return JValue(0);
    });
    j.method(kMP, "seekTo", "(I)V", [mpState](Jni& j, Ctx& x, jobject self, const auto& a) {
        auto mp = mpState(j, self);
        if (!mp) return JValue(0);
        jobject listener = 0;
        {
            std::lock_guard<std::mutex> lk(mp->mu);
            mp->positionSec = std::max(0.0, double(int32_t(a[0])) / 1000.0);
            if (mp->voice != 0) x.proc.mixer().seekSeconds(mp->voice, mp->positionSec);
            listener = mp->onSeekComplete;
        }
        if (listener) {
            const uint32_t mid = j.methodId("android/media/MediaPlayer$OnSeekCompleteListener", "onSeekComplete", "(Landroid/media/MediaPlayer;)V", false);
            j.invoke(x, mid, listener, {self});
        }
        return JValue(0);
    });
    j.method(kMP, "setLooping", "(Z)V", [mpState](Jni& j, Ctx& x, jobject self, const auto& a) {
        auto mp = mpState(j, self);
        if (!mp) return JValue(0);
        std::lock_guard<std::mutex> lk(mp->mu);
        mp->looping = (uint32_t(a[0]) & 0xFF) != 0;
        if (mp->voice != 0) x.proc.mixer().setParams(mp->voice, mp->params());
        return JValue(0);
    });
    j.method(kMP, "isLooping", "()Z", [mpState](Jni& j, Ctx&, jobject self, const auto&) {
        auto mp = mpState(j, self);
        if (!mp) return JValue(0);
        std::lock_guard<std::mutex> lk(mp->mu);
        return JValue(mp->looping ? 1 : 0);
    });
    j.method(kMP, "setVolume", "(FF)V", [mpState](Jni& j, Ctx& x, jobject self, const auto& a) {
        auto mp = mpState(j, self);
        if (!mp) return JValue(0);
        std::lock_guard<std::mutex> lk(mp->mu);
        mp->leftVol = std::clamp(asFloat(a[0]), 0.0f, 1.0f);
        mp->rightVol = std::clamp(asFloat(a[1]), 0.0f, 1.0f);
        if (mp->voice != 0) x.proc.mixer().setParams(mp->voice, mp->params());
        return JValue(0);
    });
    j.method(kMP, "isPlaying", "()Z", [mpState](Jni& j, Ctx& x, jobject self, const auto&) {
        auto mp = mpState(j, self);
        if (!mp) return JValue(0);
        jobject completion = 0;
        bool nowPlaying = false;
        {
            std::lock_guard<std::mutex> lk(mp->mu);
            if (mp->playing && mp->voice != 0) {
                if (x.proc.mixer().playing(mp->voice)) {
                    nowPlaying = true;
                } else {
                    mp->playing = false;
                    completion = mp->onCompletion;
                }
            }
        }
        if (completion) {
            const uint32_t mid = j.methodId("android/media/MediaPlayer$OnCompletionListener", "onCompletion", "(Landroid/media/MediaPlayer;)V", false);
            j.invoke(x, mid, completion, {self});
        }
        return JValue(nowPlaying ? 1 : 0);
    });
    j.method(kMP, "getCurrentPosition", "()I", [mpState](Jni& j, Ctx& x, jobject self, const auto&) {
        auto mp = mpState(j, self);
        if (!mp) return JValue(0);
        std::lock_guard<std::mutex> lk(mp->mu);
        const double sec = mp->voice != 0 ? x.proc.mixer().positionSeconds(mp->voice) : mp->positionSec;
        return JValue(int32_t(sec * 1000.0));
    });
    j.method(kMP, "getDuration", "()I", [mpState](Jni& j, Ctx&, jobject self, const auto&) {
        auto mp = mpState(j, self);
        if (!mp) return JValue(0);
        std::lock_guard<std::mutex> lk(mp->mu);
        return JValue(int32_t(mp->durationSec * 1000.0));
    });
    j.method(kMP, "setAudioStreamType", "(I)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kMP, "setWakeMode", "(Landroid/content/Context;I)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kMP, "setScreenOnWhilePlaying", "(Z)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kMP, "setOnPreparedListener", "(Landroid/media/MediaPlayer$OnPreparedListener;)V", [mpState](Jni& j, Ctx&, jobject self, const auto& a) {
        if (auto mp = mpState(j, self)) { std::lock_guard<std::mutex> lk(mp->mu); mp->onPrepared = jobject(a[0]); }
        return JValue(0);
    });
    j.method(kMP, "setOnCompletionListener", "(Landroid/media/MediaPlayer$OnCompletionListener;)V", [mpState](Jni& j, Ctx&, jobject self, const auto& a) {
        if (auto mp = mpState(j, self)) { std::lock_guard<std::mutex> lk(mp->mu); mp->onCompletion = jobject(a[0]); }
        return JValue(0);
    });
    j.method(kMP, "setOnErrorListener", "(Landroid/media/MediaPlayer$OnErrorListener;)V", [mpState](Jni& j, Ctx&, jobject self, const auto& a) {
        if (auto mp = mpState(j, self)) { std::lock_guard<std::mutex> lk(mp->mu); mp->onError = jobject(a[0]); }
        return JValue(0);
    });
    j.method(kMP, "setOnSeekCompleteListener", "(Landroid/media/MediaPlayer$OnSeekCompleteListener;)V", [mpState](Jni& j, Ctx&, jobject self, const auto& a) {
        if (auto mp = mpState(j, self)) { std::lock_guard<std::mutex> lk(mp->mu); mp->onSeekComplete = jobject(a[0]); }
        return JValue(0);
    });

    // android.media.SoundPool & SoundPool$Builder
    struct SoundPoolState {
        struct Sound { int id = 0; Audio::Pcm pcm; int rate = 44100; };
        struct Stream {
            uint32_t voice = 0;
            int soundId = 0;
            float left = 1.0f, right = 1.0f, rate = 1.0f;
            int loop = 0;
            int priority = 0;
            Audio::VoiceParams params() const {
                Audio::VoiceParams vp;
                vp.gain = 0.5f * (left + right);
                const float sum = left + right;
                vp.pan = sum > 0.0f ? std::clamp((right - left) / sum, -1.0f, 1.0f) : 0.0f;
                vp.pitch = std::clamp(rate, 0.5f, 2.0f);
                vp.loop = loop != 0;
                return vp;
            }
        };
        std::mutex mu;
        int nextSoundId = 1;
        int nextStreamId = 1;
        std::unordered_map<int, Sound> sounds;
        std::unordered_map<int, Stream> streams;
        jobject onLoadComplete = 0;
    };
    auto spState = [](Jni& j, jobject self) -> std::shared_ptr<SoundPoolState> {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* o = j.get(self);
        if (!o) return nullptr;
        auto* s = std::any_cast<std::shared_ptr<SoundPoolState>>(&o->host);
        return s ? *s : nullptr;
    };
    auto spStoreDecoded = [](Jni& j, Ctx& x, jobject self, SoundPoolState& sp, const std::vector<uint8_t>& bytes) -> int {
        if (bytes.empty()) return 0;
        Audio::DecodedAudio dec = Audio::decode(bytes.data(), bytes.size());
        if (dec.stereoPcm.empty()) return 0;
        int sid = 0;
        jobject listener = 0;
        {
            std::lock_guard<std::mutex> lk(sp.mu);
            sid = sp.nextSoundId++;
            SoundPoolState::Sound snd;
            snd.id = sid;
            snd.pcm = std::make_shared<std::vector<int16_t>>(std::move(dec.stereoPcm));
            snd.rate = dec.sampleRate > 0 ? dec.sampleRate : 44100;
            sp.sounds[sid] = std::move(snd);
            listener = sp.onLoadComplete;
        }
        if (listener) {
            const uint32_t mid = j.methodId("android/media/SoundPool$OnLoadCompleteListener", "onLoadComplete", "(Landroid/media/SoundPool;II)V", false);
            j.invoke(x, mid, listener, {self, uint32_t(sid), 0});
        }
        return sid;
    };
    const std::string kSP = "android/media/SoundPool";
    j.method(kSP, "<init>", "(III)V", [](Jni& j, Ctx&, jobject self, const auto&) {
        auto sp = std::make_shared<SoundPoolState>();
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) { o->host = sp; o->permanent = true; }
        return JValue(0);
    });
    j.method(kSP, "load", "(Ljava/lang/String;I)I", [spState, spStoreDecoded](Jni& j, Ctx& x, jobject self, const auto& a) {
        auto sp = spState(j, self);
        if (!sp) return JValue(0);
        std::string path = str(j, a[0]);
        std::string host = x.proc.hostPath(path);
        if (!fs::exists(host)) {
            const std::string asset = x.proc.config().bundlePath + "/assets/" + path;
            if (fs::exists(asset)) host = asset;
        }
        return JValue(spStoreDecoded(j, x, self, *sp, readHostBytes(host)));
    });
    j.method(kSP, "load", "(Landroid/content/Context;II)I", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kSP, "load", "(Landroid/content/res/AssetFileDescriptor;I)I", [spState, spStoreDecoded](Jni& j, Ctx& x, jobject self, const auto& a) {
        auto sp = spState(j, self);
        if (!sp) return JValue(0);
        int rawFd = -1;
        int64_t len = -1;
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* afd = j.get(jobject(a[0]))) {
                len = int64_t(afd->fields["hvm.length"]);
                if (JObject* f = j.get(jobject(afd->fields["hvm.fd"]))) rawFd = int(f->fields["descriptor"]);
            }
        }
        return JValue(spStoreDecoded(j, x, self, *sp, readFdBytes(rawFd, 0, len)));
    });
    j.method(kSP, "load", "(Ljava/io/FileDescriptor;JJI)I", [spState, spStoreDecoded](Jni& j, Ctx& x, jobject self, const auto& a) {
        auto sp = spState(j, self);
        if (!sp) return JValue(0);
        int rawFd = -1;
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* f = j.get(jobject(a[0]))) rawFd = int(f->fields["descriptor"]);
        }
        return JValue(spStoreDecoded(j, x, self, *sp, readFdBytes(rawFd, int64_t(a[1]), int64_t(a[2]))));
    });
    j.method(kSP, "play", "(IFFIIF)I", [spState](Jni& j, Ctx& x, jobject self, const auto& a) {
        auto sp = spState(j, self);
        if (!sp) return JValue(0);
        const int soundId = int(int32_t(a[0]));
        const float left = std::clamp(asFloat(a[1]), 0.0f, 1.0f);
        const float right = std::clamp(asFloat(a[2]), 0.0f, 1.0f);
        const int priority = int(int32_t(a[3]));
        const int loop = int(int32_t(a[4]));
        const float rate = asFloat(a[5]);
        std::lock_guard<std::mutex> lk(sp->mu);
        auto it = sp->sounds.find(soundId);
        if (it == sp->sounds.end() || !it->second.pcm || it->second.pcm->empty()) return JValue(0);
        SoundPoolState::Stream st;
        st.soundId = soundId;
        st.left = left;
        st.right = right;
        st.priority = priority;
        st.loop = loop;
        st.rate = rate > 0.0f ? rate : 1.0f;
        st.voice = x.proc.mixer().play(it->second.pcm, it->second.rate, st.params(), 0.0, loop == 0);
        if (st.voice == 0) return JValue(0);
        const int streamId = sp->nextStreamId++;
        sp->streams[streamId] = st;
        return JValue(streamId);
    });
    j.method(kSP, "stop", "(I)V", [spState](Jni& j, Ctx& x, jobject self, const auto& a) {
        auto sp = spState(j, self);
        if (!sp) return JValue(0);
        std::lock_guard<std::mutex> lk(sp->mu);
        auto it = sp->streams.find(int(int32_t(a[0])));
        if (it != sp->streams.end()) {
            x.proc.mixer().stop(it->second.voice);
            x.proc.mixer().release(it->second.voice);
            sp->streams.erase(it);
        }
        return JValue(0);
    });
    j.method(kSP, "pause", "(I)V", [spState](Jni& j, Ctx& x, jobject self, const auto& a) {
        auto sp = spState(j, self);
        if (!sp) return JValue(0);
        std::lock_guard<std::mutex> lk(sp->mu);
        auto it = sp->streams.find(int(int32_t(a[0])));
        if (it != sp->streams.end()) x.proc.mixer().setPaused(it->second.voice, true);
        return JValue(0);
    });
    j.method(kSP, "resume", "(I)V", [spState](Jni& j, Ctx& x, jobject self, const auto& a) {
        auto sp = spState(j, self);
        if (!sp) return JValue(0);
        std::lock_guard<std::mutex> lk(sp->mu);
        auto it = sp->streams.find(int(int32_t(a[0])));
        if (it != sp->streams.end()) x.proc.mixer().setPaused(it->second.voice, false);
        return JValue(0);
    });
    j.method(kSP, "autoPause", "()V", [spState](Jni& j, Ctx& x, jobject self, const auto&) {
        auto sp = spState(j, self);
        if (!sp) return JValue(0);
        std::lock_guard<std::mutex> lk(sp->mu);
        for (const auto& [_, st] : sp->streams) x.proc.mixer().setPaused(st.voice, true);
        return JValue(0);
    });
    j.method(kSP, "autoResume", "()V", [spState](Jni& j, Ctx& x, jobject self, const auto&) {
        auto sp = spState(j, self);
        if (!sp) return JValue(0);
        std::lock_guard<std::mutex> lk(sp->mu);
        for (const auto& [_, st] : sp->streams) x.proc.mixer().setPaused(st.voice, false);
        return JValue(0);
    });
    j.method(kSP, "setVolume", "(IFF)V", [spState](Jni& j, Ctx& x, jobject self, const auto& a) {
        auto sp = spState(j, self);
        if (!sp) return JValue(0);
        std::lock_guard<std::mutex> lk(sp->mu);
        auto it = sp->streams.find(int(int32_t(a[0])));
        if (it != sp->streams.end()) {
            it->second.left = std::clamp(asFloat(a[1]), 0.0f, 1.0f);
            it->second.right = std::clamp(asFloat(a[2]), 0.0f, 1.0f);
            x.proc.mixer().setParams(it->second.voice, it->second.params());
        }
        return JValue(0);
    });
    j.method(kSP, "setRate", "(IF)V", [spState](Jni& j, Ctx& x, jobject self, const auto& a) {
        auto sp = spState(j, self);
        if (!sp) return JValue(0);
        std::lock_guard<std::mutex> lk(sp->mu);
        auto it = sp->streams.find(int(int32_t(a[0])));
        if (it != sp->streams.end()) {
            it->second.rate = asFloat(a[1]);
            x.proc.mixer().setParams(it->second.voice, it->second.params());
        }
        return JValue(0);
    });
    j.method(kSP, "setLoop", "(II)V", [spState](Jni& j, Ctx& x, jobject self, const auto& a) {
        auto sp = spState(j, self);
        if (!sp) return JValue(0);
        std::lock_guard<std::mutex> lk(sp->mu);
        auto it = sp->streams.find(int(int32_t(a[0])));
        if (it != sp->streams.end()) {
            it->second.loop = int(int32_t(a[1]));
            x.proc.mixer().setParams(it->second.voice, it->second.params());
        }
        return JValue(0);
    });
    j.method(kSP, "setPriority", "(II)V", [spState](Jni& j, Ctx&, jobject self, const auto& a) {
        auto sp = spState(j, self);
        if (!sp) return JValue(0);
        std::lock_guard<std::mutex> lk(sp->mu);
        auto it = sp->streams.find(int(int32_t(a[0])));
        if (it != sp->streams.end()) it->second.priority = int(int32_t(a[1]));
        return JValue(0);
    });
    j.method(kSP, "unload", "(I)Z", [spState](Jni& j, Ctx&, jobject self, const auto& a) {
        auto sp = spState(j, self);
        if (!sp) return JValue(0);
        std::lock_guard<std::mutex> lk(sp->mu);
        return JValue(sp->sounds.erase(int(int32_t(a[0]))) ? 1 : 0);
    });
    j.method(kSP, "release", "()V", [spState](Jni& j, Ctx& x, jobject self, const auto&) {
        auto sp = spState(j, self);
        if (!sp) return JValue(0);
        std::lock_guard<std::mutex> lk(sp->mu);
        for (const auto& [_, st] : sp->streams) {
            x.proc.mixer().stop(st.voice);
            x.proc.mixer().release(st.voice);
        }
        sp->streams.clear();
        sp->sounds.clear();
        return JValue(0);
    });
    j.method(kSP, "setOnLoadCompleteListener", "(Landroid/media/SoundPool$OnLoadCompleteListener;)V", [spState](Jni& j, Ctx&, jobject self, const auto& a) {
        if (auto sp = spState(j, self)) { std::lock_guard<std::mutex> lk(sp->mu); sp->onLoadComplete = jobject(a[0]); }
        return JValue(0);
    });
    const std::string kSPB = "android/media/SoundPool$Builder";
    j.method(kSPB, "<init>", "()V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kSPB, "setMaxStreams", "(I)Landroid/media/SoundPool$Builder;", [](Jni&, Ctx&, jobject self, const auto&) { return JValue(self); });
    j.method(kSPB, "setAudioAttributes", "(Landroid/media/AudioAttributes;)Landroid/media/SoundPool$Builder;", [](Jni&, Ctx&, jobject self, const auto&) { return JValue(self); });
    j.method(kSPB, "build", "()Landroid/media/SoundPool;", [](Jni& j, Ctx&, jobject, const auto&) {
        const jobject pool = j.newObject("android/media/SoundPool");
        auto sp = std::make_shared<SoundPoolState>();
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(pool)) { o->host = sp; o->permanent = true; }
        return JValue(pool);
    });
}

} // namespace Hydrogen::Runtime::Android
