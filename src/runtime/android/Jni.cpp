// HydrogenVM — JavaVM and JNIEnv for Android games. See Jni.hpp.
#include "runtime/android/Jni.hpp"
#include "runtime/android/dex/JniBridge.hpp"
#include "core/cpu/Engine.hpp"
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace Hydrogen::Runtime::Android {

std::string argTypes(const std::string& sig) {
    std::string out;
    size_t i = sig.find('(');
    if (i == std::string::npos) return out;
    for (++i; i < sig.size() && sig[i] != ')'; ++i) {
        char c = sig[i];
        if (c == '[') {
            while (i < sig.size() && sig[i] == '[') ++i;
            if (i < sig.size() && sig[i] == 'L') i = sig.find(';', i);
            out.push_back('L');
            continue;
        }
        if (c == 'L') { i = sig.find(';', i); out.push_back('L'); continue; }
        out.push_back(c);
    }
    return out;
}
char returnType(const std::string& sig) {
    const size_t i = sig.find(')');
    if (i == std::string::npos || i + 1 >= sig.size()) return 'V';
    return sig[i + 1] == '[' ? 'L' : sig[i + 1];
}

// ------------------------------------------------------------------ objects ---

Jni::Jni(Process& p) : proc(p) {}

jobject Jni::add(JObject o) {
    std::lock_guard<std::recursive_mutex> lk(mu);
    const jobject h = nextHandle;
    nextHandle += 4;
    objects.emplace(h, std::move(o));
    return h;
}
JObject* Jni::get(jobject h) {
    std::lock_guard<std::recursive_mutex> lk(mu);
    auto it = objects.find(h);
    return it == objects.end() ? nullptr : &it->second;
}
void Jni::remove(jobject h) {
    std::lock_guard<std::recursive_mutex> lk(mu);
    auto it = objects.find(h);
    if (it != objects.end() && !it->second.permanent) objects.erase(it);
}
jobject Jni::classFor(const std::string& name) {
    std::lock_guard<std::recursive_mutex> lk(mu);
    auto it = classes.find(name);
    if (it != classes.end()) return it->second;
    JObject o;
    o.cls = name;
    o.isClass = true;
    o.permanent = true;
    const jobject h = add(std::move(o));
    classes.emplace(name, h);
    return h;
}
jobject Jni::newString(const std::string& s) {
    JObject o;
    o.cls = "java/lang/String";
    o.str = s;
    return add(std::move(o));
}
static size_t elemSize(char e) {
    switch (e) { case 'Z': case 'B': return 1; case 'C': case 'S': return 2; case 'J': case 'D': return 8; default: return 4; }
}
jobject Jni::newArray(char elem, uint32_t length) {
    JObject o;
    o.cls = std::string("[") + elem;
    o.elem = elem;
    if (elem == 'L') o.refs.assign(length, 0);
    else o.data.assign(size_t(length) * elemSize(elem), 0);
    return add(std::move(o));
}
jobject Jni::newObject(const std::string& cls) {
    JObject o;
    o.cls = cls;
    return add(std::move(o));
}
std::string Jni::string(jobject h) {
    std::lock_guard<std::recursive_mutex> lk(mu);
    JObject* o = get(h);
    return o ? o->str : std::string();
}

// ------------------------------------------------------------------ methods ---

void Jni::method(const std::string& cls, const std::string& name, const std::string& sig, JMethod fn) {
    impls[cls + "." + name + sig] = std::move(fn);
}
uint32_t Jni::methodId(const std::string& cls, const std::string& name, const std::string& sig, bool isStatic) {
    std::lock_guard<std::recursive_mutex> lk(mu);
    for (size_t i = 0; i < methods.size(); ++i)
        if (methods[i].cls == cls && methods[i].name == name && methods[i].sig == sig) return uint32_t(0x100 + i * 4);
    methods.push_back({cls, name, sig, isStatic});
    return uint32_t(0x100 + (methods.size() - 1) * 4);
}
uint32_t Jni::fieldId(const std::string& cls, const std::string& name, const std::string& sig, bool isStatic) {
    std::lock_guard<std::recursive_mutex> lk(mu);
    for (size_t i = 0; i < fieldInfos.size(); ++i)
        if (fieldInfos[i].cls == cls && fieldInfos[i].name == name) return uint32_t(0x100 + i * 4);
    fieldInfos.push_back({cls, name, sig, isStatic});
    return uint32_t(0x100 + (fieldInfos.size() - 1) * 4);
}

// Java_ name mangling (JNI spec): '/' -> '_', '_' -> "_1", ';' -> "_2", '[' -> "_3", non-ASCII -> _0xxxx.
static std::string mangle(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (c == '/') out += '_';
        else if (c == '_') out += "_1";
        else if (c == ';') out += "_2";
        else if (c == '[') out += "_3";
        else if (std::isalnum(c)) out += char(c);
        else { char b[8]; std::snprintf(b, sizeof b, "_0%04x", c); out += b; }
    }
    return out;
}

std::string Jni::superOf(const std::string& cls) const {
    if (cls == "java/lang/Object") return {};
    auto it = superclasses.find(cls);
    if (it != superclasses.end()) return it->second;
    if (cls.size() > 8 && cls.compare(cls.size() - 8, 8, "Activity") == 0) return "android/app/Activity";
    return "java/lang/Object";
}
bool Jni::isSubclass(const std::string& cls, const std::string& of) const {
    std::string c = cls;
    for (int depth = 0; depth < 32 && !c.empty(); ++depth) {
        if (c == of) return true;
        c = superOf(c);
    }
    return false;
}

// HVM_JNI_TRACE=1: each Java method the game calls, and each native the activity calls, once.
void Jni::traceOnce(const std::string& what) {
    static const bool on = [] { const char* e = std::getenv("HVM_JNI_TRACE"); return e && *e && *e != '0'; }();
    if (!on) return;
    std::lock_guard<std::recursive_mutex> lk(mu);
    if (traced.insert(what).second) proc.log(LogLevel::Info, what);
}

bool Jni::callNative(Ctx& x, const std::string& cls, const std::string& name, jobject self,
                     const std::vector<uint32_t>& words, JValue* result) {
    return callNative(x, cls, name, self, words, result, {});
}

bool Jni::callNative(Ctx& x, const std::string& cls, const std::string& name, jobject self,
                     const std::vector<uint32_t>& words, JValue* result, const std::string& signature) {
    traceOnce("JNI native " + cls + "." + name);
    uint32_t fn = 0;
    {
        std::lock_guard<std::recursive_mutex> lk(mu);
        if (!signature.empty()) {
            const auto it = registeredNatives.find(cls + "." + name + signature);
            if (it != registeredNatives.end()) fn = it->second;
        } else {
            for (auto& [k, v] : registeredNatives)
                if (k.rfind(cls + "." + name + "(", 0) == 0) { fn = v; break; }
        }
    }
    if (!fn) {
        auto& L = proc.service<Library>();
        std::lock_guard<std::recursive_mutex> lk(L.mu);
        const auto symbol = "Java_" + mangle(cls) + "_" + mangle(name);
        auto a = L.find(symbol);
        if (!a && !signature.empty())
            a = L.find(symbol + "__" + mangle(signature.substr(1, signature.find(')') - 1)));
        if (!a) return false;
        fn = *a;
    }
    std::vector<uint32_t> args{envFor(proc), self};
    args.insert(args.end(), words.begin(), words.end());
    const uint64_t r = proc.callGuest(x.cpu, fn, args);
    if (result) *result = r;
    return true;
}

// Guest words for a native call: 64-bit values at an even word (AAPCS).
static std::vector<uint32_t> nativeWords(const std::string& types, const std::vector<JValue>& args) {
    std::vector<uint32_t> w;
    for (size_t i = 0; i < types.size() && i < args.size(); ++i) {
        if (types[i] == 'J' || types[i] == 'D') {
            if (w.size() % 2) w.push_back(0);   // env and self take words 0-1
            w.push_back(uint32_t(args[i]));
            w.push_back(uint32_t(args[i] >> 32));
        } else {
            w.push_back(uint32_t(args[i]));
        }
    }
    return w;
}

bool isPluginClass(const std::string& cls) {
    for (const char* platform : {"android/", "java/", "javax/", "dalvik/", "org/json/", "org/xml/", "org/w3c/", "com/android/"})
        if (cls.rfind(platform, 0) == 0) return false;
    return !cls.empty();
}

bool Jni::dexNative(Ctx& x, const std::string& cls, const std::string& name, const std::string& sig,
                    jobject self, const std::vector<JValue>& args, JValue& result) {
    return callNative(x, cls, name, self, nativeWords(argTypes(sig), args), &result, sig);
}

JValue Jni::invoke(Ctx& x, uint32_t id, jobject self, const std::vector<JValue>& args) {
    return invoke(x, id, self, args, false);
}

JValue Jni::invoke(Ctx& x, uint32_t id, jobject self, const std::vector<JValue>& args, bool nonvirtual) {
    MethodInfo m;
    {
        std::lock_guard<std::recursive_mutex> lk(mu);
        const uint32_t i = (id - 0x100) / 4;
        if (id < 0x100 || i >= methods.size()) throw std::runtime_error("JNI: bad method id");
        m = methods[i];
    }
    traceOnce("JNI call " + m.cls + "." + m.name + m.sig);
    // The implementation for the method's class, or (virtual calls) for the object's own class.
    std::string objCls;
    bool proxy = false;
    {
        std::lock_guard<std::recursive_mutex> lk(mu);
        if (JObject* o = get(self); o && !o->isClass) {
            objCls = o->cls;
            auto it = o->fields.find("hvm.proxy");
            proxy = it != o->fields.end() && it->second;
        }
    }
    if (proxy) return invokeProxy(*this, x, self, m.name, m.sig, args);
    JValue interpreted = 0;
    if (interpretsDex.load(std::memory_order_relaxed) &&
        Dex::invokeFromJni(*this, x, m, self, args, objCls, nonvirtual, interpreted)) return interpreted;
    // Then the platform classes every activity and view inherits from: a game's own activity
    // class (com/foo/GameActivity) calls getFilesDir, getAssets, ... of Context.
    static const std::string kContext = "android/content/Context", kActivity = "android/app/Activity",
                             kObject = "java/lang/Object";
    // The object's class and its superclasses, then the method's class and its superclasses.
    for (const std::string& start : {objCls, m.cls}) {
        std::string c = start;
        for (int depth = 0; depth < 32 && !c.empty(); ++depth, c = superOf(c)) {
            auto it = impls.find(c + "." + m.name + m.sig);
            if (it != impls.end()) return it->second(*this, x, self, args);
        }
    }
    for (const std::string* c : {&kActivity, &kContext, &kObject}) {
        auto it = impls.find(*c + "." + m.name + m.sig);
        if (it != impls.end()) return it->second(*this, x, self, args);
    }
    // Reflection-style callers (Unity's AndroidJavaObject.Call<T>) ask for an object-returning
    // method as returning java.lang.Object: the same name and arguments with any object return.
    if (const size_t close = m.sig.find(')'); close != std::string::npos && close + 1 < m.sig.size() &&
                                              (m.sig[close + 1] == 'L' || m.sig[close + 1] == '[')) {
        const std::string params = m.sig.substr(0, close + 1);
        for (const std::string& start : {objCls, m.cls, kActivity, kContext, kObject}) {
            std::string c = start;
            for (int depth = 0; depth < 32 && !c.empty(); ++depth, c = superOf(c)) {
                const std::string prefix = c + "." + m.name + params;
                for (auto it = impls.lower_bound(prefix); it != impls.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it) {
                    const char r0 = it->first[prefix.size()];
                    if (r0 == 'L' || r0 == '[') return it->second(*this, x, self, args);
                }
            }
        }
    }
    JValue r = 0;
    if (callNative(x, m.cls, m.name, self, nativeWords(argTypes(m.sig), args), &r)) return r;
    if (quietClasses.count(m.cls) || quietClasses.count(objCls)) return 0;
    if (m.name == "<init>") return 0;   // constructors of helper objects: nothing to set up
    {
        // The first string argument too: generic "call by name" methods differ only in it.
        std::string what = "JNI " + m.cls + "." + m.name + m.sig;
        const std::string types = argTypes(m.sig);
        for (size_t i = 0; i < types.size() && i < args.size(); ++i)
            if (types[i] == 'L') {
                std::lock_guard<std::recursive_mutex> lk(mu);
                if (JObject* o = get(jobject(args[i])); o && o->cls == "java/lang/String") { what += " (\"" + o->str.substr(0, 60) + "\")"; break; }
            }
        proc.reportMissing(what);
    }
    const char rt = returnType(m.sig);
    if (rt == 'L') {
        // A string-returning getter we don't know: an empty string is safer than null.
        if (m.sig.size() >= 19 && m.sig.compare(m.sig.size() - 18, 18, "Ljava/lang/String;") == 0) return newString("");
        // A game's own Java plugins (ad, purchase, social SDKs; not android/ or java/ classes):
        // an object of the declared type. Unity's AndroidJavaObject throws on null, which stops
        // the script that asked (and with it, things like the title screen's input).
        if (isPluginClass(m.cls)) {
            const size_t open = m.sig.rfind(")L");
            std::string type = open == std::string::npos ? "java/lang/Object" : m.sig.substr(open + 2, m.sig.size() - open - 3);
            if (type == "java/lang/Object") type = m.cls;
            return newObject(type);
        }
    }
    return 0;
}

// ---------------------------------------------------------------- the env ---

namespace {

constexpr int kEnvSlots = 233;

Jni& J(Ctx& x) { return jni(x.proc); }
uint32_t arg(Ctx& x, int i) { return i < 4 ? x.cpu.r[i] : x.mem.read<uint32_t>(x.cpu.r[CPU::kSP] + uint32_t(i - 4) * 4); }
void ret(Ctx& x, uint64_t v) { x.cpu.r[0] = uint32_t(v); x.cpu.r[1] = uint32_t(v >> 32); }

// Reads Java arguments from a C argument list (varargs or va_list): floats come promoted to double.
std::vector<JValue> readArgs(ArgReader& rd, const std::string& types) {
    std::vector<JValue> out;
    for (char t : types) {
        if (t == 'J') out.push_back(rd.dword());
        else if (t == 'D') out.push_back(rd.dword());
        else if (t == 'F') { const uint64_t b = rd.dword(); double d; std::memcpy(&d, &b, 8); out.push_back(jfloat(float(d))); }
        else out.push_back(rd.word());
    }
    return out;
}
std::vector<JValue> readJvalues(Ctx& x, uint32_t p, const std::string& types) {
    std::vector<JValue> out;
    for (size_t i = 0; i < types.size(); ++i) {
        const uint32_t a = p + uint32_t(i) * 8;
        switch (types[i]) {
            case 'J': case 'D': out.push_back(x.mem.read<uint64_t>(a)); break;
            case 'Z': case 'B': out.push_back(x.mem.read<uint8_t>(a)); break;
            case 'C': case 'S': out.push_back(x.mem.read<uint16_t>(a)); break;
            default: out.push_back(x.mem.read<uint32_t>(a)); break;
        }
    }
    return out;
}
std::string methodSig(Jni& j, uint32_t id) {
    std::lock_guard<std::recursive_mutex> lk(j.mu);
    const uint32_t i = (id - 0x100) / 4;
    return id >= 0x100 && i < j.methods.size() ? j.methods[i].sig : std::string("()V");
}

// Call<Type>Method family. mode: 0 varargs, 1 va_list, 2 jvalue*; firstArg: index of the first
// call argument (3 for virtual and static, 4 for nonvirtual which has a class first).
void callMethod(Ctx& x, int mode, bool nonvirtual) {
    Jni& j = J(x);
    const jobject self = arg(x, 1);
    const uint32_t mid = arg(x, nonvirtual ? 3 : 2);
    const std::string types = argTypes(methodSig(j, mid));
    const int first = nonvirtual ? 4 : 3;
    std::vector<JValue> args;
    if (mode == 0) { ArgReader rd(x.cpu, x.mem, uint32_t(first)); args = readArgs(rd, types); }
    else if (mode == 1) { ArgReader rd(x.mem, arg(x, first)); args = readArgs(rd, types); }
    else args = readJvalues(x, arg(x, first), types);
    ret(x, j.invoke(x, mid, self, args, nonvirtual));
}
void newObject(Ctx& x, int mode) {
    Jni& j = J(x);
    const jobject cls = arg(x, 1);
    const uint32_t mid = arg(x, 2);
    const std::string types = argTypes(methodSig(j, mid));
    std::vector<JValue> args;
    if (mode == 0) { ArgReader rd(x.cpu, x.mem, 3); args = readArgs(rd, types); }
    else if (mode == 1) { ArgReader rd(x.mem, arg(x, 3)); args = readArgs(rd, types); }
    else args = readJvalues(x, arg(x, 3), types);
    JObject* c = j.get(cls);
    const jobject obj = j.newObject(c ? c->cls : "java/lang/Object");
    j.invoke(x, mid, obj, args, true);
    ret(x, obj);
}

uint64_t fieldGet(Jni& j, jobject obj, uint32_t fid, bool isStatic) {
    std::lock_guard<std::recursive_mutex> lk(j.mu);
    const uint32_t i = (fid - 0x100) / 4;
    if (fid < 0x100 || i >= j.fieldInfos.size()) return 0;
    const auto& f = j.fieldInfos[i];
    if (isStatic) { auto it = j.staticFields.find(f.cls + "." + f.name); return it == j.staticFields.end() ? 0 : it->second; }
    JObject* o = j.get(obj);
    if (!o) return 0;
    auto it = o->fields.find(f.name);
    return it == o->fields.end() ? 0 : it->second;
}
void fieldSet(Jni& j, jobject obj, uint32_t fid, bool isStatic, uint64_t v) {
    std::lock_guard<std::recursive_mutex> lk(j.mu);
    const uint32_t i = (fid - 0x100) / 4;
    if (fid < 0x100 || i >= j.fieldInfos.size()) return;
    const auto& f = j.fieldInfos[i];
    if (isStatic) { j.staticFields[f.cls + "." + f.name] = v; return; }
    if (JObject* o = j.get(obj)) o->fields[f.name] = v;
}

// Array element storage: a guest copy handed out by Get*ArrayElements, written back on release.
struct Pinned { jobject array; uint32_t bytes; };
std::mutex pinMu;
std::unordered_map<uint32_t, Pinned> pinned;   // guest buffer -> array

uint32_t pinArray(Ctx& x, jobject h) {
    Jni& j = J(x);
    std::lock_guard<std::recursive_mutex> lk(j.mu);
    JObject* o = j.get(h);
    if (!o || o->elem == 'L' || o->elem == 0) return 0;
    const uint32_t n = uint32_t(o->data.size());
    const uint32_t buf = x.proc.heap().alloc(std::max<uint32_t>(n, 8));
    if (n) std::memcpy(x.mem.host(buf), o->data.data(), n);
    std::lock_guard<std::mutex> pl(pinMu);
    pinned[buf] = {h, n};
    return buf;
}
void unpinArray(Ctx& x, uint32_t buf, int32_t mode) {
    Jni& j = J(x);
    Pinned pin;
    {
        std::lock_guard<std::mutex> pl(pinMu);
        auto it = pinned.find(buf);
        if (it == pinned.end()) return;
        pin = it->second;
        if (mode != 1 /*JNI_COMMIT*/) pinned.erase(it);
    }
    if (mode != 2 /*JNI_ABORT*/) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(pin.array); o && o->data.size() >= pin.bytes && pin.bytes)
            std::memcpy(o->data.data(), x.mem.host(buf), pin.bytes);
    }
    if (mode != 1) x.proc.heap().free(buf);
}

} // namespace

Jni& jni(Process& p) {
    auto& j = p.service<Jni>();
    if (!j.envTable) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (!j.envTable) {
            auto fnAddr = [&](const std::string& name, Process::HostFn fn) { return p.registerHostFunction("JNI " + name, std::move(fn)); };
            const uint32_t table = p.allocStatic(kEnvSlots * 4);
            for (int i = 0; i < kEnvSlots; ++i) {
                const std::string nm = "function " + std::to_string(i);
                p.mem().write<uint32_t>(table + uint32_t(i) * 4, fnAddr(nm, [nm](Ctx& x) {
                    x.proc.reportMissing("JNI " + nm);
                    x.cpu.r[0] = 0; x.cpu.r[1] = 0;
                }));
            }
            auto set = [&](int slot, const char* name, Process::HostFn fn) { p.mem().write<uint32_t>(table + uint32_t(slot) * 4, fnAddr(name, std::move(fn))); };

            set(4, "GetVersion", [](Ctx& x) { ret(x, 0x00010006); });
            set(6, "FindClass", [](Ctx& x) { ret(x, J(x).classFor(x.proc.readCString(arg(x, 1), 512))); });
            set(10, "GetSuperclass", [](Ctx& x) {
                JObject* c = J(x).get(arg(x, 1));
                const std::string s = c ? J(x).superOf(c->cls) : std::string();
                ret(x, s.empty() ? 0 : J(x).classFor(s));
            });
            set(11, "IsAssignableFrom", [](Ctx& x) {   // (clazz1, clazz2): clazz1 can be cast to clazz2
                JObject* a = J(x).get(arg(x, 1)); JObject* b = J(x).get(arg(x, 2));
                ret(x, a && b && J(x).isSubclass(a->cls, b->cls));
            });
            set(13, "Throw", [](Ctx& x) {
                const auto exception = arg(x, 1);
                if (!J(x).get(exception)) { ret(x, uint32_t(-1)); return; }
                Dex::pendingException(x.proc) = exception;
                ret(x, 0);
            });
            set(14, "ThrowNew", [](Ctx& x) {
                auto& j = J(x);
                std::lock_guard lock(j.mu);
                const auto* type = j.get(arg(x, 1));
                if (!type || !type->isClass) { ret(x, uint32_t(-1)); return; }
                JObject exception;
                exception.cls = type->cls;
                if (const auto message = arg(x, 2)) exception.str = x.proc.readCString(message, 512);
                Dex::pendingException(x.proc) = j.add(std::move(exception));
                ret(x, 0);
            });
            set(15, "ExceptionOccurred", [](Ctx& x) { ret(x, Dex::pendingException(x.proc)); });
            set(16, "ExceptionDescribe", [](Ctx& x) { ret(x, 0); });
            set(17, "ExceptionClear", [](Ctx& x) { Dex::pendingException(x.proc) = 0; ret(x, 0); });
            set(18, "FatalError", [](Ctx& x) { throw std::runtime_error("JNI FatalError: " + x.proc.readCString(arg(x, 1), 1024)); });
            // Reflection: a java.lang.reflect.Method keeps its name, signature (host) and class.
            set(7, "FromReflectedMethod", [](Ctx& x) {
                Jni& j = J(x);
                std::string cls, name, sig;
                {
                    std::lock_guard<std::recursive_mutex> lk(j.mu);
                    JObject* m = j.get(arg(x, 1));
                    if (!m) { ret(x, 0); return; }
                    name = m->str;
                    if (auto* s = std::any_cast<std::string>(&m->host)) sig = *s;
                    if (JObject* c = j.get(jobject(m->fields["hvm.declaring"]))) cls = c->cls;
                }
                bool isStatic = false;
                {
                    std::lock_guard<std::recursive_mutex> lk(j.mu);
                    if (JObject* m = j.get(arg(x, 1))) isStatic = m->fields["hvm.static"] != 0;
                }
                ret(x, j.methodId(cls.empty() ? "java/lang/Object" : cls, name, sig.empty() ? "()V" : sig, isStatic));
            });
            set(8, "FromReflectedField", [](Ctx& x) {
                Jni& j = J(x);
                std::string cls, name, sig;
                bool isStatic = false;
                {
                    std::lock_guard<std::recursive_mutex> lk(j.mu);
                    JObject* f = j.get(arg(x, 1));
                    if (!f) { ret(x, 0); return; }
                    name = f->str;
                    if (auto* s = std::any_cast<std::string>(&f->host)) sig = *s;
                    isStatic = f->fields["hvm.static"] != 0;
                    if (JObject* c = j.get(jobject(f->fields["hvm.declaring"]))) cls = c->cls;
                }
                ret(x, j.fieldId(cls.empty() ? "java/lang/Object" : cls, name, sig.empty() ? "Ljava/lang/Object;" : sig, isStatic));
            });
            set(9, "ToReflectedMethod", [](Ctx& x) {
                Jni& j = J(x);
                const uint32_t id = arg(x, 2);
                Jni::MethodInfo mi;
                {
                    std::lock_guard<std::recursive_mutex> lk(j.mu);
                    const uint32_t i = (id - 0x100) / 4;
                    if (id < 0x100 || i >= j.methods.size()) { ret(x, 0); return; }
                    mi = j.methods[i];
                }
                const jobject m = j.newObject("java/lang/reflect/Method");
                const jobject c = j.classFor(mi.cls);
                std::lock_guard<std::recursive_mutex> lk(j.mu);
                if (JObject* o = j.get(m)) { o->str = mi.name; o->host = mi.sig; o->fields["hvm.declaring"] = c; o->fields["hvm.static"] = mi.isStatic ? 1 : 0; }
                ret(x, m);
            });
            // Direct byte buffers: a guest address and a capacity.
            set(229, "NewDirectByteBuffer", [](Ctx& x) {
                Jni& j = J(x);
                const uint64_t cap = uint64_t(arg(x, 3)) << 32 | arg(x, 2);
                const jobject b = j.newObject("java/nio/DirectByteBuffer");
                std::lock_guard<std::recursive_mutex> lk(j.mu);
                if (JObject* o = j.get(b)) { o->fields["hvm.address"] = arg(x, 1); o->fields["hvm.capacity"] = cap; }
                ret(x, b);
            });
            set(230, "GetDirectBufferAddress", [](Ctx& x) {
                Jni& j = J(x);
                std::lock_guard<std::recursive_mutex> lk(j.mu);
                JObject* o = j.get(arg(x, 1));
                ret(x, o ? uint32_t(o->fields["hvm.address"]) : 0);
            });
            set(231, "GetDirectBufferCapacity", [](Ctx& x) {
                Jni& j = J(x);
                std::lock_guard<std::recursive_mutex> lk(j.mu);
                JObject* o = j.get(arg(x, 1));
                ret(x, o && o->fields.count("hvm.address") ? o->fields["hvm.capacity"] : uint64_t(-1));
            });
            set(19, "PushLocalFrame", [](Ctx& x) { ret(x, 0); });
            set(20, "PopLocalFrame", [](Ctx& x) { ret(x, arg(x, 1)); });
            set(21, "NewGlobalRef", [](Ctx& x) { if (JObject* o = J(x).get(arg(x, 1))) o->permanent = true; ret(x, arg(x, 1)); });
            set(22, "DeleteGlobalRef", [](Ctx& x) { ret(x, 0); });
            set(23, "DeleteLocalRef", [](Ctx& x) { ret(x, 0); });
            set(24, "IsSameObject", [](Ctx& x) { ret(x, arg(x, 1) == arg(x, 2)); });
            set(25, "NewLocalRef", [](Ctx& x) { ret(x, arg(x, 1)); });
            set(26, "EnsureLocalCapacity", [](Ctx& x) { ret(x, 0); });
            set(27, "AllocObject", [](Ctx& x) { JObject* c = J(x).get(arg(x, 1)); ret(x, J(x).newObject(c ? c->cls : "java/lang/Object")); });
            set(28, "NewObject", [](Ctx& x) { newObject(x, 0); });
            set(29, "NewObjectV", [](Ctx& x) { newObject(x, 1); });
            set(30, "NewObjectA", [](Ctx& x) { newObject(x, 2); });
            set(31, "GetObjectClass", [](Ctx& x) { JObject* o = J(x).get(arg(x, 1)); ret(x, J(x).classFor(o ? o->cls : "java/lang/Object")); });
            set(32, "IsInstanceOf", [](Ctx& x) {
                if (!arg(x, 1)) { ret(x, 1); return; }   // null is an instance of every class
                JObject* o = J(x).get(arg(x, 1)); JObject* c = J(x).get(arg(x, 2));
                ret(x, o && c && J(x).isSubclass(o->isClass ? std::string("java/lang/Class") : o->cls, c->cls));
            });
            auto getMethod = [](bool isStatic) {
                return [isStatic](Ctx& x) {
                    JObject* c = J(x).get(arg(x, 1));
                    const std::string name = x.proc.readCString(arg(x, 2), 256), sig = x.proc.readCString(arg(x, 3), 1024);
                    ret(x, J(x).methodId(c ? c->cls : "?", name, sig, isStatic));
                };
            };
            set(33, "GetMethodID", getMethod(false));
            set(113, "GetStaticMethodID", getMethod(true));
            for (int t = 0; t < 10; ++t) {
                for (int mode = 0; mode < 3; ++mode) {
                    set(34 + t * 3 + mode, "Call<T>Method", [mode](Ctx& x) { callMethod(x, mode, false); });
                    set(64 + t * 3 + mode, "CallNonvirtual<T>Method", [mode](Ctx& x) { callMethod(x, mode, true); });
                    set(114 + t * 3 + mode, "CallStatic<T>Method", [mode](Ctx& x) { callMethod(x, mode, false); });
                }
            }
            auto getField = [](bool isStatic) {
                return [isStatic](Ctx& x) {
                    JObject* c = J(x).get(arg(x, 1));
                    ret(x, J(x).fieldId(c ? c->cls : "?", x.proc.readCString(arg(x, 2), 256), x.proc.readCString(arg(x, 3), 1024), isStatic));
                };
            };
            set(94, "GetFieldID", getField(false));
            set(144, "GetStaticFieldID", getField(true));
            for (int t = 0; t < 9; ++t) {
                set(95 + t, "Get<T>Field", [](Ctx& x) { ret(x, fieldGet(J(x), arg(x, 1), arg(x, 2), false)); });
                set(145 + t, "GetStatic<T>Field", [](Ctx& x) { ret(x, fieldGet(J(x), arg(x, 1), arg(x, 2), true)); });
                // Set<T>Field(env, obj, id, value): long and double values start at an even register.
                const bool wide = t == 6 || t == 8;
                set(104 + t, "Set<T>Field", [wide](Ctx& x) {
                    fieldSet(J(x), arg(x, 1), arg(x, 2), false, wide ? (uint64_t(arg(x, 5)) << 32 | arg(x, 4)) : arg(x, 3));
                    ret(x, 0);
                });
                set(154 + t, "SetStatic<T>Field", [wide](Ctx& x) {
                    fieldSet(J(x), arg(x, 1), arg(x, 2), true, wide ? (uint64_t(arg(x, 5)) << 32 | arg(x, 4)) : arg(x, 3));
                    ret(x, 0);
                });
            }
            set(163, "NewString", [](Ctx& x) {
                std::string s;
                for (uint32_t i = 0; i < arg(x, 2); ++i) {
                    const uint16_t c = x.mem.read<uint16_t>(arg(x, 1) + i * 2);
                    if (c < 0x80) s += char(c);
                    else if (c < 0x800) { s += char(0xC0 | (c >> 6)); s += char(0x80 | (c & 63)); }
                    else { s += char(0xE0 | (c >> 12)); s += char(0x80 | ((c >> 6) & 63)); s += char(0x80 | (c & 63)); }
                }
                ret(x, J(x).newString(s));
            });
            set(164, "GetStringLength", [](Ctx& x) { ret(x, J(x).string(arg(x, 1)).size()); });
            set(165, "GetStringChars", [](Ctx& x) {
                const std::string s = J(x).string(arg(x, 1));
                const uint32_t buf = x.proc.heap().alloc(uint32_t(s.size() * 2 + 2));
                for (size_t i = 0; i < s.size(); ++i) x.mem.write<uint16_t>(buf + uint32_t(i) * 2, uint8_t(s[i]));
                x.mem.write<uint16_t>(buf + uint32_t(s.size()) * 2, 0);
                if (arg(x, 2)) x.mem.write<uint8_t>(arg(x, 2), 1);
                ret(x, buf);
            });
            set(166, "ReleaseStringChars", [](Ctx& x) { x.proc.heap().free(arg(x, 2)); ret(x, 0); });
            set(167, "NewStringUTF", [](Ctx& x) { ret(x, arg(x, 1) ? J(x).newString(x.proc.readCString(arg(x, 1))) : 0); });
            set(168, "GetStringUTFLength", [](Ctx& x) { ret(x, J(x).string(arg(x, 1)).size()); });
            set(169, "GetStringUTFChars", [](Ctx& x) {
                if (arg(x, 2)) x.mem.write<uint8_t>(arg(x, 2), 1);
                ret(x, x.proc.allocCString(J(x).string(arg(x, 1))));
            });
            set(170, "ReleaseStringUTFChars", [](Ctx& x) { if (arg(x, 2)) x.proc.heap().free(arg(x, 2)); ret(x, 0); });
            set(171, "GetArrayLength", [](Ctx& x) {
                JObject* o = J(x).get(arg(x, 1));
                ret(x, !o ? 0 : o->elem == 'L' ? o->refs.size() : o->data.size() / elemSize(o->elem));
            });
            set(172, "NewObjectArray", [](Ctx& x) {
                const jobject a = J(x).newArray('L', arg(x, 1));
                if (JObject* o = J(x).get(a)) for (auto& r : o->refs) r = arg(x, 3);
                ret(x, a);
            });
            set(173, "GetObjectArrayElement", [](Ctx& x) {
                JObject* o = J(x).get(arg(x, 1));
                ret(x, o && arg(x, 2) < o->refs.size() ? o->refs[arg(x, 2)] : 0);
            });
            set(174, "SetObjectArrayElement", [](Ctx& x) {
                JObject* o = J(x).get(arg(x, 1));
                if (o && arg(x, 2) < o->refs.size()) o->refs[arg(x, 2)] = arg(x, 3);
                ret(x, 0);
            });
            const char kTypes[] = "ZBCSIJFD";
            for (int t = 0; t < 8; ++t) {
                const char e = kTypes[t];
                set(175 + t, "New<T>Array", [e](Ctx& x) { ret(x, J(x).newArray(e, arg(x, 1))); });
                set(183 + t, "Get<T>ArrayElements", [](Ctx& x) { if (arg(x, 2)) x.mem.write<uint8_t>(arg(x, 2), 1); ret(x, pinArray(x, arg(x, 1))); });
                set(191 + t, "Release<T>ArrayElements", [](Ctx& x) { unpinArray(x, arg(x, 2), int32_t(arg(x, 3))); ret(x, 0); });
                set(199 + t, "Get<T>ArrayRegion", [e](Ctx& x) {
                    Jni& j = J(x);
                    std::lock_guard<std::recursive_mutex> lk(j.mu);
                    JObject* o = j.get(arg(x, 1));
                    const size_t es = elemSize(e), start = arg(x, 2) * es, n = arg(x, 3) * es;
                    if (o && start + n <= o->data.size() && n) std::memcpy(x.mem.host(arg(x, 4)), o->data.data() + start, n);
                    ret(x, 0);
                });
                set(207 + t, "Set<T>ArrayRegion", [e](Ctx& x) {
                    Jni& j = J(x);
                    std::lock_guard<std::recursive_mutex> lk(j.mu);
                    JObject* o = j.get(arg(x, 1));
                    const size_t es = elemSize(e), start = arg(x, 2) * es, n = arg(x, 3) * es;
                    if (o && start + n <= o->data.size() && n) std::memcpy(o->data.data() + start, x.mem.host(arg(x, 4)), n);
                    ret(x, 0);
                });
            }
            set(215, "RegisterNatives", [](Ctx& x) {
                Jni& j = J(x);
                JObject* c = j.get(arg(x, 1));
                const std::string cls = c ? c->cls : "?";
                for (uint32_t i = 0; i < arg(x, 3); ++i) {
                    const uint32_t m = arg(x, 2) + i * 12;
                    std::lock_guard<std::recursive_mutex> lk(j.mu);
                    j.registeredNatives[cls + "." + x.proc.readCString(x.mem.read<uint32_t>(m), 256) +
                                        x.proc.readCString(x.mem.read<uint32_t>(m + 4), 1024)] = x.mem.read<uint32_t>(m + 8);
                }
                ret(x, 0);
            });
            set(216, "UnregisterNatives", [](Ctx& x) { ret(x, 0); });
            set(217, "MonitorEnter", [](Ctx& x) { ret(x, 0); });
            set(218, "MonitorExit", [](Ctx& x) { ret(x, 0); });
            set(219, "GetJavaVM", [](Ctx& x) { x.mem.write<uint32_t>(arg(x, 1), javaVM(x.proc)); ret(x, 0); });
            set(220, "GetStringRegion", [](Ctx& x) {
                const std::string s = J(x).string(arg(x, 1));
                for (uint32_t i = 0; i < arg(x, 3) && arg(x, 2) + i < s.size(); ++i)
                    x.mem.write<uint16_t>(arg(x, 4) + i * 2, uint8_t(s[arg(x, 2) + i]));
                ret(x, 0);
            });
            set(221, "GetStringUTFRegion", [](Ctx& x) {
                const std::string s = J(x).string(arg(x, 1));
                const std::string part = arg(x, 2) < s.size() ? s.substr(arg(x, 2), arg(x, 3)) : std::string();
                x.mem.copyIn(arg(x, 4), part.c_str(), part.size() + 1);
                ret(x, 0);
            });
            set(222, "GetPrimitiveArrayCritical", [](Ctx& x) { if (arg(x, 2)) x.mem.write<uint8_t>(arg(x, 2), 1); ret(x, pinArray(x, arg(x, 1))); });
            set(223, "ReleasePrimitiveArrayCritical", [](Ctx& x) { unpinArray(x, arg(x, 2), int32_t(arg(x, 3))); ret(x, 0); });
            set(226, "NewWeakGlobalRef", [](Ctx& x) { if (JObject* o = J(x).get(arg(x, 1))) o->permanent = true; ret(x, arg(x, 1)); });
            set(227, "DeleteWeakGlobalRef", [](Ctx& x) { ret(x, 0); });
            set(228, "ExceptionCheck", [](Ctx& x) { ret(x, Dex::pendingException(x.proc) != 0); });
            set(232, "GetObjectRefType", [](Ctx& x) { ret(x, 1); });
            j.envTable = table;

            // JavaVM: DestroyJavaVM, AttachCurrentThread, DetachCurrentThread, GetEnv, AttachCurrentThreadAsDaemon.
            const uint32_t vmTable = p.allocStatic(8 * 4);
            auto vmSet = [&](int slot, const char* name, Process::HostFn fn) { p.mem().write<uint32_t>(vmTable + uint32_t(slot) * 4, fnAddr(name, std::move(fn))); };
            vmSet(3, "DestroyJavaVM", [](Ctx& x) { ret(x, 0); });
            vmSet(4, "AttachCurrentThread", [](Ctx& x) { if (arg(x, 1)) x.mem.write<uint32_t>(arg(x, 1), J(x).envFor(x.proc)); ret(x, 0); });
            vmSet(5, "DetachCurrentThread", [](Ctx& x) { ret(x, 0); });
            vmSet(6, "GetEnv", [](Ctx& x) { if (arg(x, 1)) x.mem.write<uint32_t>(arg(x, 1), J(x).envFor(x.proc)); ret(x, 0); });
            vmSet(7, "AttachCurrentThreadAsDaemon", [](Ctx& x) { if (arg(x, 1)) x.mem.write<uint32_t>(arg(x, 1), J(x).envFor(x.proc)); ret(x, 0); });
            j.vm = p.allocStatic(4);
            p.mem().write<uint32_t>(j.vm, vmTable);
            registerJavaPlatform(j);
            registerJavaClasses(j);
            registerJniPlaceholders(j);
        }
    }
    return j;
}

uint32_t Jni::envFor(Process& p) {
    // One JNIEnv per guest thread (a pointer to the shared function table).
    static const char key = 0;
    uint32_t& env = p.threadSlot(&key);
    if (!env) {
        env = p.allocStatic(4);
        p.mem().write<uint32_t>(env, envTable);
    }
    return env;
}

uint32_t javaVM(Process& p) { return jni(p).vm; }
uint32_t jniEnv(Process& p) { return jni(p).envFor(p); }

} // namespace Hydrogen::Runtime::Android
