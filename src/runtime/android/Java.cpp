// HydrogenVM — the Java platform classes Android games' native code calls through JNI, shared by
// every engine: the activity as a Context (its folders, package, class loader), java.io.File,
// java.lang.ClassLoader / Class, and SDK classes whose calls change nothing for an offline game
// (analytics, ads, social). Engine-specific classes live with their driver (App.cpp).
#include "runtime/android/Jni.hpp"
#include <chrono>
#include <ctime>
#include <cstdio>
#include <filesystem>
#include <random>
#include <system_error>

namespace fs = std::filesystem;

namespace Hydrogen::Runtime::Android {
namespace {

std::string ensureDir(Process& p, const fs::path& host) {
    std::error_code ec;
    fs::create_directories(host, ec);
    return p.guestPath(host.string());
}
std::string packageName(Process& p) {
    const auto* info = p.infoPlist();
    return info ? info->getString("CFBundleIdentifier", "app") : std::string("app");
}
fs::path sdcard(Process& p) { return fs::path(p.homePath()) / "Library/Caches/sdcard"; }

jobject newFile(Jni& j, const std::string& guestPath) {
    const jobject f = j.newObject("java/io/File");
    if (JObject* o = j.get(f)) o->str = guestPath;   // a File keeps its path as its string
    return f;
}
std::string filePath(Jni& j, jobject f) {
    std::lock_guard<std::recursive_mutex> lk(j.mu);
    JObject* o = j.get(f);
    return o ? o->str : std::string();
}

} // namespace

jobject makeMotionEvent(Jni& j, TouchPhase phase, float x, float y, uint32_t id, uint32_t pointersDown) {
    // One pointer per event. The first finger down and the last up are ACTION_DOWN / ACTION_UP;
    // the others are POINTER_DOWN / POINTER_UP (index 0, the only pointer in the event).
    uint32_t action;
    switch (phase) {
        case TouchPhase::Began: action = pointersDown <= 1 ? 0 : 5; break;
        case TouchPhase::Moved: action = 2; break;
        case TouchPhase::Cancelled: action = 3; break;
        default: action = pointersDown <= 1 ? 1 : 6; break;
    }
    const jobject ev = j.newObject("android/view/MotionEvent");
    // Event times are SystemClock.uptimeMillis(): the same CLOCK_MONOTONIC the game reads.
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint64_t now = uint64_t(ts.tv_sec) * 1000 + uint64_t(ts.tv_nsec) / 1000000;
    std::lock_guard<std::recursive_mutex> lk(j.mu);
    if (JObject* o = j.get(ev)) {
        o->fields["action"] = action;
        o->fields["x"] = jfloat(x);
        o->fields["y"] = jfloat(y);
        o->fields["id"] = id;
        o->fields["time"] = now;
    }
    return ev;
}

void registerJavaPlatform(Jni& j) {
    Process& p = j.proc;
    // The part of the class hierarchy games test with IsInstanceOf / GetSuperclass.
    j.superclasses = {
        {"android/app/Activity", "android/view/ContextThemeWrapper"},
        {"android/view/ContextThemeWrapper", "android/content/ContextWrapper"},
        {"android/content/ContextWrapper", "android/content/Context"},
        {"android/app/Application", "android/content/ContextWrapper"},
        {"android/app/NativeActivity", "android/app/Activity"},
        {"com/unity3d/player/UnityPlayerNativeActivity", "android/app/NativeActivity"},
        {"com/unity3d/player/UnityPlayer", "android/widget/FrameLayout"},
        {"android/widget/FrameLayout", "android/view/ViewGroup"},
        {"android/view/ViewGroup", "android/view/View"},
        {"android/view/MotionEvent", "android/view/InputEvent"},
        {"android/view/KeyEvent", "android/view/InputEvent"},
        {"java/io/FileInputStream", "java/io/InputStream"},
    };
    // The base Activity uses the launcher object allocated by the activity driver.
    j.method("android/app/Activity", "onCreate", "(Landroid/os/Bundle;)V",
             [](Jni& j, Ctx&, jobject self, const auto&) {
                 std::lock_guard lock(j.mu);
                 j.staticFields["hvm.activity"] = self;
                 return JValue(0);
             });
    // Context / Activity. Method lookup falls back to these classes for any activity class
    // (Jni::invoke), so they serve com.foo.GameActivity, NativeActivity and the rest alike.
    for (const char* ctx : {"android/content/Context", "android/app/Activity"}) {
        const std::string c = ctx;
        j.method(c, "getFilesDir", "()Ljava/io/File;", [&p](Jni& j, Ctx&, jobject, const auto&) {
            return JValue(newFile(j, ensureDir(p, fs::path(p.homePath()) / "Library/files")));
        });
        j.method(c, "getCacheDir", "()Ljava/io/File;", [&p](Jni& j, Ctx&, jobject, const auto&) {
            return JValue(newFile(j, ensureDir(p, fs::path(p.homePath()) / "Library/Caches/app")));
        });
        j.method(c, "getExternalFilesDir", "(Ljava/lang/String;)Ljava/io/File;", [&p](Jni& j, Ctx&, jobject, const auto& a) {
            fs::path dir = sdcard(p) / "Android/data" / packageName(p) / "files";
            if (const std::string sub = j.string(jobject(a[0])); !sub.empty()) dir /= sub;
            return JValue(newFile(j, ensureDir(p, dir)));
        });
        j.method(c, "getExternalCacheDir", "()Ljava/io/File;", [&p](Jni& j, Ctx&, jobject, const auto&) {
            return JValue(newFile(j, ensureDir(p, sdcard(p) / "Android/data" / packageName(p) / "cache")));
        });
        j.method(c, "getObbDir", "()Ljava/io/File;", [&p](Jni& j, Ctx&, jobject, const auto&) {
            return JValue(newFile(j, ensureDir(p, sdcard(p) / "Android/obb" / packageName(p))));
        });
        j.method(c, "getPackageName", "()Ljava/lang/String;", [&p](Jni& j, Ctx&, jobject, const auto&) {
            return JValue(j.newString(packageName(p)));
        });
        for (const char* m : {"getPackageCodePath", "getPackageResourcePath"})
            j.method(c, m, "()Ljava/lang/String;", [&p](Jni& j, Ctx&, jobject, const auto&) {
                const std::string host = p.config().bundlePath + "/base.apk";
                std::error_code ec;
                if (!fs::exists(host, ec)) {
                    static constexpr uint8_t kEmptyZip[22] = {'P', 'K', 5, 6};
                    if (std::FILE* f = std::fopen(host.c_str(), "wb")) {
                        std::fwrite(kEmptyZip, 1, sizeof kEmptyZip, f);
                        std::fclose(f);
                    }
                }
                return JValue(j.newString(p.guestPath(p.config().bundlePath) + "/base.apk"));
            });
        j.method(c, "getClassLoader", "()Ljava/lang/ClassLoader;", [](Jni& j, Ctx&, jobject, const auto&) {
            return JValue(j.classFor("java/lang/ClassLoader"));
        });
        j.method(c, "getApplicationContext", "()Landroid/content/Context;", [](Jni& j, Ctx&, jobject self, const auto&) {
            return JValue(self ? self : jobject(j.staticFields["hvm.activity"]));
        });
        j.method(c, "getApplication", "()Landroid/app/Application;", [](Jni& j, Ctx&, jobject, const auto&) {
            return JValue(jobject(j.staticFields["hvm.activity"]));
        });
        j.method(c, "getAssets", "()Landroid/content/res/AssetManager;", [](Jni& j, Ctx&, jobject, const auto&) {
            return JValue(jobject(j.staticFields["hvm.assets"]));
        });
    }
    const jobject mounted = j.newString("mounted"), mountedRo = j.newString("mounted_ro");
    if (JObject* o = j.get(mounted)) o->permanent = true;
    if (JObject* o = j.get(mountedRo)) o->permanent = true;
    j.staticFields["android/os/Environment.MEDIA_MOUNTED"] = mounted;
    j.staticFields["android/os/Environment.MEDIA_MOUNTED_READ_ONLY"] = mountedRo;
    j.method("android/os/Environment", "getExternalStorageDirectory", "()Ljava/io/File;", [&p](Jni& j, Ctx&, jobject, const auto&) {
        return JValue(newFile(j, ensureDir(p, sdcard(p))));
    });
    j.method("android/os/Environment", "getExternalStorageState", "()Ljava/lang/String;", [mounted](Jni&, Ctx&, jobject, const auto&) {
        return JValue(mounted);
    });

    // java.io.File
    const std::string kFile = "java/io/File";
    j.method(kFile, "<init>", "(Ljava/lang/String;)V", [](Jni& j, Ctx&, jobject self, const auto& a) {
        const std::string path = j.string(jobject(a[0]));
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->str = path;
        return JValue(0);
    });
    j.method(kFile, "<init>", "(Ljava/io/File;Ljava/lang/String;)V", [](Jni& j, Ctx&, jobject self, const auto& a) {
        const std::string path = filePath(j, jobject(a[0])) + "/" + j.string(jobject(a[1]));
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->str = path;
        return JValue(0);
    });
    for (const char* n : {"getPath", "getAbsolutePath", "getCanonicalPath", "toString"})
        j.method(kFile, n, "()Ljava/lang/String;", [](Jni& j, Ctx&, jobject self, const auto&) { return JValue(j.newString(filePath(j, self))); });
    j.method(kFile, "getName", "()Ljava/lang/String;", [](Jni& j, Ctx&, jobject self, const auto&) {
        return JValue(j.newString(fs::path(filePath(j, self)).filename().string()));
    });
    j.method(kFile, "exists", "()Z", [&p](Jni& j, Ctx&, jobject self, const auto&) {
        std::error_code ec;
        return JValue(fs::exists(p.hostPath(filePath(j, self)), ec) ? 1 : 0);
    });
    j.method(kFile, "isDirectory", "()Z", [&p](Jni& j, Ctx&, jobject self, const auto&) {
        std::error_code ec;
        return JValue(fs::is_directory(p.hostPath(filePath(j, self)), ec) ? 1 : 0);
    });
    for (const char* n : {"mkdirs", "mkdir"})
        j.method(kFile, n, "()Z", [&p](Jni& j, Ctx&, jobject self, const auto&) {
            std::error_code ec;
            return JValue(fs::create_directories(p.hostPath(filePath(j, self)), ec) ? 1 : 0);
        });
    j.method(kFile, "length", "()J", [&p](Jni& j, Ctx&, jobject self, const auto&) {
        std::error_code ec;
        const auto n = fs::file_size(p.hostPath(filePath(j, self)), ec);
        return JValue(ec ? 0 : uint64_t(n));
    });

    // java.lang.ClassLoader / Class: classes are the JNI's own (FindClass-able) ones.
    j.method("java/lang/ClassLoader", "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;", [](Jni& j, Ctx&, jobject, const auto& a) {
        std::string name = j.string(jobject(a[0]));
        for (char& c : name) if (c == '.') c = '/';
        return JValue(j.classFor(name));
    });
    j.method("java/lang/Class", "getName", "()Ljava/lang/String;", [](Jni& j, Ctx&, jobject self, const auto&) {
        std::string name;
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* o = j.get(self)) name = o->cls;
        }
        for (char& c : name) if (c == '/') c = '.';
        return JValue(j.newString(name));
    });

    // android.view.MotionEvent (one pointer; see makeMotionEvent).
    const std::string kMe = "android/view/MotionEvent";
    auto field = [](Jni& j, jobject ev, const char* f) -> uint64_t {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* o = j.get(ev);
        return o ? o->fields[f] : 0;
    };
    // MotionEvent.obtain(other): a copy the game keeps (Unity queues it for its own thread)
    // until it calls recycle().
    j.method(kMe, "obtain", "(Landroid/view/MotionEvent;)Landroid/view/MotionEvent;", [kMe](Jni& j, Ctx&, jobject, const auto& a) {
        std::map<std::string, uint64_t> fields;
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            JObject* o = j.get(jobject(a[0]));
            if (!o) return JValue(0);
            fields = o->fields;
        }
        const jobject copy = j.newObject(kMe);
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* c = j.get(copy)) c->fields = std::move(fields);
        return JValue(copy);
    });
    j.method(kMe, "recycle", "()V", [](Jni& j, Ctx&, jobject s, const auto&) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(s); o && !o->permanent) j.remove(s);
        return JValue(0);
    });
    j.method(kMe, "getAction", "()I", [field](Jni& j, Ctx&, jobject s, const auto&) {
        return JValue(field(j, s, "action")); });
    j.method(kMe, "getActionMasked", "()I", [field](Jni& j, Ctx&, jobject s, const auto&) { return JValue(field(j, s, "action") & 0xFF); });
    j.method(kMe, "getActionIndex", "()I", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kMe, "getPointerCount", "()I", [](Jni&, Ctx&, jobject, const auto&) { return JValue(1); });
    j.method(kMe, "getPointerId", "(I)I", [field](Jni& j, Ctx&, jobject s, const auto&) { return JValue(field(j, s, "id")); });
    j.method(kMe, "findPointerIndex", "(I)I", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    for (const char* sig : {"()F", "(I)F"}) {
        j.method(kMe, "getX", sig, [field](Jni& j, Ctx&, jobject s, const auto&) { return JValue(field(j, s, "x")); });
        j.method(kMe, "getY", sig, [field](Jni& j, Ctx&, jobject s, const auto&) { return JValue(field(j, s, "y")); });
        j.method(kMe, "getRawX", sig, [field](Jni& j, Ctx&, jobject s, const auto&) { return JValue(field(j, s, "x")); });
        j.method(kMe, "getRawY", sig, [field](Jni& j, Ctx&, jobject s, const auto&) { return JValue(field(j, s, "y")); });
        j.method(kMe, "getPressure", sig, [](Jni&, Ctx&, jobject, const auto&) { return jfloat(1.0f); });
        j.method(kMe, "getSize", sig, [](Jni&, Ctx&, jobject, const auto&) { return jfloat(0.1f); });
        j.method(kMe, "getTouchMajor", sig, [](Jni&, Ctx&, jobject, const auto&) { return jfloat(20.0f); });
        j.method(kMe, "getTouchMinor", sig, [](Jni&, Ctx&, jobject, const auto&) { return jfloat(20.0f); });
        j.method(kMe, "getOrientation", sig, [](Jni&, Ctx&, jobject, const auto&) { return jfloat(0.0f); });
    }
    j.method(kMe, "getAxisValue", "(I)F", [field](Jni& j, Ctx&, jobject s, const auto& a) {
        return a[0] == 0 ? JValue(field(j, s, "x")) : a[0] == 1 ? JValue(field(j, s, "y")) : jfloat(0.0f);
    });
    j.method(kMe, "getAxisValue", "(II)F", [field](Jni& j, Ctx&, jobject s, const auto& a) {
        return a[0] == 0 ? JValue(field(j, s, "x")) : a[0] == 1 ? JValue(field(j, s, "y")) : jfloat(0.0f);
    });
    for (const char* n : {"getEventTime", "getDownTime"})
        j.method(kMe, n, "()J", [field](Jni& j, Ctx&, jobject s, const auto&) { return JValue(field(j, s, "time")); });
    j.method(kMe, "getSource", "()I", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0x1002); });   // SOURCE_TOUCHSCREEN
    j.method(kMe, "getToolType", "(I)I", [](Jni&, Ctx&, jobject, const auto&) { return JValue(1); });       // TOOL_TYPE_FINGER
    for (const char* n : {"getDeviceId", "getHistorySize", "getMetaState", "getFlags", "getEdgeFlags", "getButtonState"})
        j.method(kMe, n, "()I", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("android/view/InputEvent", "getSource", "()I", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0x1002); });

    // java.util.UUID: random (version 4) ids, as strings.
    j.method("java/util/UUID", "randomUUID", "()Ljava/util/UUID;", [](Jni& j, Ctx& x, jobject, const auto&) {
        const uint64_t a = x.proc.entropy(), b = x.proc.entropy();
        char buf[40];
        std::snprintf(buf, sizeof buf, "%08x-%04x-4%03x-%04x-%012llx", uint32_t(a >> 32), uint32_t(a >> 16) & 0xFFFF,
                      uint32_t(a) & 0xFFF, uint32_t((b >> 48) & 0x3FFF) | 0x8000, (unsigned long long)(b & 0xFFFFFFFFFFFFull));
        const jobject u = j.newObject("java/util/UUID");
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(u)) o->str = buf;
        return JValue(u);
    });
    j.method("java/util/UUID", "toString", "()Ljava/lang/String;", [](Jni& j, Ctx&, jobject self, const auto&) {
        std::string s;
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* o = j.get(self)) s = o->str;
        }
        return JValue(j.newString(s));
    });

    // SDKs that do nothing useful offline: every method returns 0 / false / null, quietly.
    for (const char* cls : {"com/flurry/android/FlurryAgent", "org/OpenUDID/OpenUDID_manager", "com/tapjoy/Tapjoy",
                            "com/crashlytics/android/Crashlytics", "io/fabric/sdk/android/Fabric",
                            "com/everyplay/Everyplay/Everyplay", "com/google/android/gms/analytics/Tracker",
                            "com/facebook/AppEventsLogger", "com/chartboost/sdk/Chartboost",
                            "com/adjust/sdk/Adjust", "com/appsflyer/AppsFlyerLib"})
        j.quietClasses.insert(cls);
    registerJavaServices(j);
}

} // namespace Hydrogen::Runtime::Android
