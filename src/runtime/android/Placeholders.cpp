// HydrogenVM — placeholders for what Android games need and HydrogenVM doesn't have yet.
//
// The tables in placeholders/ are written by tools/android_gaps.py from the gap ledger (the
// missing functions and Java methods found across scanned games). A placeholder is not support:
// it is registered as SupportStatus::Stub, answers a safe default, and is reported as
// "placeholder" each time a game reaches it (once per name), so reports keep listing it as work
// to do. Anything really implemented is never replaced: placeholders are added last and skip
// names that already exist.
//
// Java: void / numbers / booleans answer 0; String answers ""; an object of a game's own or a
// third-party plugin class answers an object of the declared type (Unity's AndroidJavaObject
// throws on null); a platform object (android.*, java.*) answers null, as Android does for
// "none"; arrays answer an empty array.
// Native: Zero answers 0; Fail answers -1 with errno ENOSYS (calls whose failure is reported
// that way, so the game sees a failure rather than a success that did nothing).
#include "runtime/android/Android.hpp"
#include "runtime/android/BionicInternal.hpp"
#include "runtime/android/Jni.hpp"
#include "runtime/Support.hpp"
#include <string>
#include <vector>

namespace Hydrogen::Runtime::Android {
namespace {

enum class Native { Zero, Fail };
struct NativeEntry { const char* symbol; Native kind; };
struct JniEntry { const char* cls; const char* name; const char* sig; };

const std::vector<NativeEntry>& nativeTable() {
    static const std::vector<NativeEntry> t = {
#include "runtime/android/placeholders/native.inc"
    };
    return t;
}
const std::vector<JniEntry>& jniTable() {
    static const std::vector<JniEntry> t = {
#include "runtime/android/placeholders/jni.inc"
    };
    return t;
}

} // namespace

void registerNativePlaceholders(Process& p) {
    for (const auto& e : nativeTable()) {
        const std::string name = std::string("android:") + e.symbol;
        if (p.lookupSymbol(name) || p.lookupSymbol(std::string("_") + e.symbol)) continue;   // real (Android or iOS) wins
        const std::string what = std::string("placeholder ") + e.symbol;
        const Native kind = e.kind;
        p.defineFunction(name, [what, kind](Ctx& x) {
            x.proc.reportDefaulted(what);
            if (kind == Native::Fail) {
                setLinuxErrno(x, 38);   // ENOSYS
                x.cpu.r[0] = 0xFFFFFFFFu;
            } else {
                x.cpu.r[0] = 0;
                x.cpu.r[1] = 0;
            }
        });
        supportRegistry(p).record({SupportKind::Function, name, SupportStatus::Stub,
                                   kind == Native::Fail ? "placeholder: -1, errno ENOSYS (tools/android_gaps.py)"
                                                        : "placeholder: answers 0 (tools/android_gaps.py)"});
    }
}

void registerJniPlaceholders(Jni& j) {
    for (const auto& e : jniTable()) {
        const std::string cls = e.cls, name = e.name, sig = e.sig;
        if (j.impls.count(cls + "." + name + sig)) continue;
        const std::string what = "placeholder JNI " + cls + "." + name + sig;
        const char rt = returnType(sig);
        std::string type;
        if (rt == 'L') {
            const size_t open = sig.rfind(")L");
            type = open == std::string::npos ? "java/lang/Object" : sig.substr(open + 2, sig.size() - open - 3);
        }
        const char elem = rt == '[' && sig.size() > sig.rfind(")[") + 2 ? sig[sig.rfind(")[") + 2] : 0;
        j.method(cls, name, sig, [what, rt, type, cls, elem](Jni& j, Ctx& x, jobject, const auto&) -> JValue {
            x.proc.reportDefaulted(what);
            if (rt == '[') return JValue(j.newArray(elem ? elem : 'L', 0));
            if (rt != 'L') return 0;
            if (type == "java/lang/String") return JValue(j.newString(""));
            if (isPluginClass(type)) return JValue(j.newObject(type));
            if (type == "java/lang/Object" && isPluginClass(cls)) return JValue(j.newObject(cls));
            return 0;
        });
    }
}

} // namespace Hydrogen::Runtime::Android
