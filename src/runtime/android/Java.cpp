// HydrogenVM — the Java platform classes Android games' native code calls through JNI, shared by
// every engine: the activity as a Context (its folders, package, class loader), java.io.File,
// java.lang.ClassLoader / Class, and SDK classes whose calls change nothing for an offline game
// (analytics, ads, social). Engine-specific classes live with their driver (App.cpp).
#include "runtime/android/Jni.hpp"
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

void registerJavaPlatform(Jni& j) {
    Process& p = j.proc;
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
        j.method(c, "getPackageCodePath", "()Ljava/lang/String;", [&p](Jni& j, Ctx&, jobject, const auto&) {
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
        j.method(c, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;", [&p](Jni& j, Ctx&, jobject, const auto& a) {
            p.reportDefaulted("Context.getSystemService(\"" + j.string(jobject(a[0])) + "\") -> null");
            return JValue(0);
        });
    }
    j.method("android/os/Environment", "getExternalStorageDirectory", "()Ljava/io/File;", [&p](Jni& j, Ctx&, jobject, const auto&) {
        return JValue(newFile(j, ensureDir(p, sdcard(p))));
    });
    j.method("android/os/Environment", "getExternalStorageState", "()Ljava/lang/String;", [](Jni& j, Ctx&, jobject, const auto&) {
        return JValue(j.newString("mounted"));
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

    // java.util.UUID: random (version 4) ids, as strings.
    j.method("java/util/UUID", "randomUUID", "()Ljava/util/UUID;", [](Jni& j, Ctx&, jobject, const auto&) {
        static std::mt19937_64 rng{std::random_device{}()};
        const uint64_t a = rng(), b = rng();
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
}

} // namespace Hydrogen::Runtime::Android
