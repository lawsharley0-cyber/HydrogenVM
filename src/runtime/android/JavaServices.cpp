// HydrogenVM — Android framework services a game's native code asks for through JNI: the
// device description (android.os.Build), system services (window and display metrics,
// telephony, connectivity, vibrator), SharedPreferences kept in a file per name, the launch
// Intent, the package manager's own-package queries, the locale, and interface proxies
// (bitter.jnibridge.JNIBridge, which Unity uses for Runnable and listener callbacks).
//
// The answers describe an offline phone: no network, no SIM, one display the size of the
// game's surface.
#include "runtime/android/Jni.hpp"
#include "runtime/android/Surface.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace Hydrogen::Runtime::Android {
namespace {

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

} // namespace

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
    j.staticFields["android/os/Build$VERSION.SDK_INT"] = 19;
    j.staticFields["android/os/Build$VERSION.SDK"] = permanentString(j, "19");
    j.staticFields["android/os/Build$VERSION.RELEASE"] = permanentString(j, "4.4.4");
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
        {"ALARM_SERVICE", "alarm"}, {"CLIPBOARD_SERVICE", "clipboard"}, {"ACCOUNT_SERVICE", "account"}};
    for (const auto& [k, v] : services) j.staticFields["android/content/Context." + k] = permanentString(j, v);
    const std::map<std::string, std::string> serviceClass = {
        {"window", "android/view/WindowManager"}, {"display", "android/hardware/display/DisplayManager"},
        {"phone", "android/telephony/TelephonyManager"}, {"connectivity", "android/net/ConnectivityManager"},
        {"vibrator", "android/os/Vibrator"}, {"audio", "android/media/AudioManager"},
        {"power", "android/os/PowerManager"}, {"activity", "android/app/ActivityManager"},
        {"media_router", "android/media/MediaRouter"}, {"input_method", "android/view/inputmethod/InputMethodManager"},
        {"uimode", "android/app/UiModeManager"}, {"keyguard", "android/app/KeyguardManager"},
        {"input", "android/hardware/input/InputManager"}};
    for (const char* ctx : {"android/content/Context", "android/app/Activity"}) {
        j.method(ctx, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;", [&p, serviceClass](Jni& j, Ctx&, jobject, const auto& a) {
            const std::string name = str(j, a[0]);
            auto it = serviceClass.find(name);
            if (it == serviceClass.end()) {
                p.reportDefaulted("Context.getSystemService(\"" + name + "\") -> null");
                return JValue(0);
            }
            return JValue(singleton(j, it->second));
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
        j.method(cls, "valueOf", "(" + sig + ")L" + cls + ";", [cls](Jni& j, Ctx&, jobject, const auto& a) {
            const jobject b = j.newObject(cls);
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            field(j, b, "value") = a[0];
            return JValue(b);
        });
    }
}

} // namespace Hydrogen::Runtime::Android
