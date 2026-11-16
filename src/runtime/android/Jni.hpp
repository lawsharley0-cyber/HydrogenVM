// HydrogenVM — the JNI layer for Android games (internal to runtime/android).
//
// Java objects are host objects behind opaque handles; Java methods are host functions
// looked up by class, name and signature. A method nobody implemented that the library
// itself exports as a native (Java_<class>_<name>) is called there, as a VM would.
#pragma once
#include "runtime/android/Android.hpp"
#include "runtime/Process.hpp"
#include <any>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Hydrogen::Runtime::Android {

using jobject = uint32_t;   // a handle; 0 is null

struct JObject {
    std::string cls;                       // Java class name, slash form: java/lang/String
    bool isClass = false;                  // a jclass (cls is the class it stands for)
    std::string str;                       // java/lang/String (modified UTF-8 kept as UTF-8)
    char elem = 0;                         // arrays: element type (Z B C S I J F D, or L)
    std::vector<uint8_t> data;             // primitive array contents
    std::vector<jobject> refs;             // object arrays
    std::map<std::string, uint64_t> fields;
    std::any host;                         // host state (an open asset, an audio track, ...)
    bool permanent = false;                // never deleted by Delete*Ref
};

// A Java value: the raw bits, as JNI passes them.
using JValue = uint64_t;
inline JValue jfloat(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }
inline float asFloat(JValue v) { const uint32_t b = uint32_t(v); float f; std::memcpy(&f, &b, 4); return f; }
inline JValue jdouble(double d) { uint64_t b; std::memcpy(&b, &d, 8); return b; }

class Jni;
using JMethod = std::function<JValue(Jni& j, Ctx& x, jobject self, const std::vector<JValue>& args)>;

class Jni {
public:
    explicit Jni(Process& p);
    Process& proc;

    // Objects.
    jobject add(JObject o);
    JObject* get(jobject h);
    void remove(jobject h);
    jobject classFor(const std::string& name);         // the (permanent) jclass for a class name
    jobject newString(const std::string& s);
    jobject newArray(char elem, uint32_t length);
    jobject newObject(const std::string& cls);
    std::string string(jobject h);                      // "" for null / not a string

    // Methods implemented here: "class.name(sig)ret".
    void method(const std::string& cls, const std::string& name, const std::string& sig, JMethod fn);
    // Call a method by id (from Call*Method / NewObject); `self` is the object or class.
    JValue invoke(Ctx& x, uint32_t methodId, jobject self, const std::vector<JValue>& args);
    uint32_t methodId(const std::string& cls, const std::string& name, const std::string& sig, bool isStatic);
    uint32_t fieldId(const std::string& cls, const std::string& name, const std::string& sig, bool isStatic);

    // Guest tables.
    uint32_t vm = 0;                                     // JavaVM*
    uint32_t envFor(Process& p);                         // JNIEnv* of the calling guest thread

    // Calls the library's exported native `Java_<cls>_<name>` with (env, self, args...).
    // Returns false when the library has no such function.
    bool callNative(Ctx& x, const std::string& cls, const std::string& name, jobject self,
                    const std::vector<uint32_t>& words, JValue* result = nullptr);

    std::recursive_mutex mu;
    struct MethodInfo { std::string cls, name, sig; bool isStatic; };
    std::vector<MethodInfo> methods;
    struct FieldInfo { std::string cls, name, sig; bool isStatic; };
    std::vector<FieldInfo> fieldInfos;
    std::map<std::string, uint64_t> staticFields;        // "class.name"
    std::map<std::string, JMethod> impls;               // "class.name(sig)"
    std::unordered_map<jobject, JObject> objects;
    std::unordered_map<std::string, jobject> classes;
    std::map<std::string, uint32_t> registeredNatives;  // "class.name(sig)" -> guest fn (RegisterNatives)
    uint32_t nextHandle = 0x10000;
    uint32_t envTable = 0;
};

Jni& jni(Process& p);
// Argument types of a method signature ("(ILjava/lang/String;[BF)V" -> "IL[F"), and its return type.
std::string argTypes(const std::string& sig);
char returnType(const std::string& sig);

// The Java classes the game's native code uses (App.cpp).
void registerJavaClasses(Jni& j);

} // namespace Hydrogen::Runtime::Android
