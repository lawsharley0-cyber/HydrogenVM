#include "runtime/android/dex/JniBridge.hpp"
#include "runtime/android/dex/CoreCollections.hpp"
#include "runtime/android/dex/CoreLang.hpp"
#include "runtime/android/dex/CoreMath.hpp"
#include "runtime/android/dex/CoreRandom.hpp"
#include "runtime/android/dex/CoreRegex.hpp"
#include "runtime/android/dex/GuestScheduler.hpp"

#include <fstream>
#include <unordered_map>
#include <utility>

namespace Hydrogen::Runtime::Android::Dex {
namespace {

std::u16string fromUtf8(std::string_view text) {
    std::u16string out;
    for (size_t pos = 0; pos < text.size();) {
        const auto first = uint8_t(text[pos++]);
        uint32_t ch = first;
        unsigned count = 0;
        if ((first & 0xe0) == 0xc0) { ch &= 31; count = 1; }
        else if ((first & 0xf0) == 0xe0) { ch &= 15; count = 2; }
        else if ((first & 0xf8) == 0xf0) { ch &= 7; count = 3; }
        else if (first > 127) { out.push_back(0xfffd); continue; }
        for (unsigned n = 0; n < count; ++n) {
            if (pos == text.size() || (uint8_t(text[pos]) & 0xc0) != 0x80) { ch = 0xfffd; break; }
            ch = ch << 6 | (uint8_t(text[pos++]) & 63);
        }
        if (ch > 0x10ffff) ch = 0xfffd;
        if (ch > 0xffff) {
            ch -= 0x10000;
            out.push_back(char16_t(0xd800 + (ch >> 10)));
            ch = 0xdc00 + (ch & 1023);
        }
        out.push_back(char16_t(ch));
    }
    return out;
}

std::string toMutf8(std::u16string_view text) {
    std::string out;
    for (const uint32_t c : text) {
        if (c > 0 && c < 128) out += char(c);
        else if (c < 2048) {
            out += char(0xc0 | (c >> 6));
            out += char(0x80 | (c & 63));
        } else {
            out += char(0xe0 | (c >> 12));
            out += char(0x80 | ((c >> 6) & 63));
            out += char(0x80 | (c & 63));
        }
    }
    return out;
}

std::u16string descriptor(std::string_view cls) {
    const auto name = fromUtf8(cls);
    if (name == u"[L") return u"[Ljava/lang/Object;";
    return cls.starts_with('[') ? name : u"L" + name + u";";
}

std::string className(std::u16string_view type) {
    if (type.size() > 2 && type.front() == u'L' && type.back() == u';')
        type = type.substr(1, type.size() - 2);
    return toMutf8(type);
}

ValueKind kind(char type) {
    if (type == 'L' || type == '[') return ValueKind::Reference;
    if (type == 'J' || type == 'D') return ValueKind::Wide;
    if (type == 'V') return ValueKind::Void;
    return ValueKind::Word;
}

size_t elementBytes(char type) {
    switch (type) {
        case 'B': case 'Z': return 1;
        case 'S': case 'C': return 2;
        case 'J': case 'D': return 8;
        default: return 4;
    }
}

class Bridge {
public:
    explicit Bridge(Process& p) : process(p) {
        const std::filesystem::path root(p.bundlePath());
        std::vector<std::string> names;
        std::error_code ec;
        for (std::filesystem::directory_iterator i(root, ec), end; !ec && i != end; i.increment(ec)) {
            const auto name = i->path().filename().string();
            if (name.starts_with("classes") && name.ends_with(".dex") && i->is_regular_file(ec))
                names.push_back(name);
        }
        if (names.empty()) return;
        auto path = std::make_shared<ClassPath>(std::move(names), [root](const std::string& name) {
            std::ifstream in(root / name, std::ios::binary | std::ios::ate);
            const auto count = in.tellg();
            if (count < 112 || uint64_t(count) > (256u << 20)) throw FormatError("invalid imported DEX length");
            in.seekg(0);
            std::vector<uint8_t> bytes(static_cast<size_t>(count));
            if (!in.read(reinterpret_cast<char*>(bytes.data()), count)) throw FormatError("cannot read imported DEX");
            return bytes;
        });
        machine = std::make_unique<Machine>(std::move(path),
            [&p](std::function<void()> work) { return guestScheduler(p)(std::move(work)); },
            Machine::Host{
                [this](auto type, auto name, auto sig, bool stat, bool native, auto args) {
                    return external(type, name, sig, stat, native, args);
                },
                [this](auto type) { return hasClass(type); },
                [this](auto type, auto name, auto sig, auto receiver, auto write) {
                    return externalField(type, name, sig, receiver, write);
                },
            });
    }

    std::shared_ptr<ArrayBacking> arrayBacking(Jni& j, jobject handle, char elem) {
        const auto width = elementBytes(elem);
        auto read = [this, &j, handle, elem, width](uint32_t i) -> Value {
            uint64_t bits = 0;
            {
                std::lock_guard lock(j.mu);
                const auto* obj = j.get(handle);
                if (!obj) throw FormatError("lost JNI array reference");
                if (elem == 'L') bits = obj->refs.at(i);
                else std::memcpy(&bits, obj->data.data() + size_t(i) * width, width);
            }
            if (elem == 'L') bits = importReference(j, jobject(bits));
            if (elem == 'B') bits = uint32_t(int32_t(int8_t(bits)));
            if (elem == 'S') bits = uint32_t(int32_t(int16_t(bits)));
            if (elem == 'Z') bits &= 1;
            return {kind(elem), bits};
        };
        auto write = [this, &j, handle, elem, width](uint32_t i, Value value) {
            const auto alias = elem == 'L' ? exportReference(j, uint32_t(value.bits)) : 0;
            std::lock_guard lock(j.mu);
            auto* obj = j.get(handle);
            if (!obj) throw FormatError("lost JNI array reference");
            if (elem == 'L') obj->refs.at(i) = alias;
            else std::memcpy(obj->data.data() + size_t(i) * width, &value.bits, width);
        };
        return std::make_shared<ArrayBacking>(ArrayBacking{std::move(read), std::move(write)});
    }

    uint32_t importReference(Jni& j, jobject handle) {
        if (!handle) return 0;
        std::lock_guard lock(references);
        if (auto it = fromJni.find(handle); it != fromJni.end()) return it->second;
        std::string cls, text;
        bool isClassObj = false;
        size_t length = 0;
        char elem;
        {
            std::lock_guard jniLock(j.mu);
            const auto* object = j.get(handle);
            if (!object) throw FormatError("invalid JNI object reference");
            isClassObj = object->isClass;
            cls = object->cls;
            text = object->str;
            elem = object->elem;
            if (elem) length = elem == 'L' ? object->refs.size() : object->data.size() / elementBytes(elem);
        }
        auto& heap = machine->heap();
        uint32_t ref;
        if (isClassObj) {
            ref = machine->classMirror(descriptor(cls));
        } else if (elem) {
            if (length > INT32_MAX) throw JavaException{u"Ljava/lang/OutOfMemoryError;"};
            ref = heap.array(descriptor(cls), int32_t(length));
        } else {
            ref = cls == "java/lang/String" ? heap.newString(fromUtf8(text)) : heap.object(descriptor(cls));
        }
        fromJni.emplace(handle, ref);
        toJni.try_emplace(ref, handle);
        if (elem) heap.bindArray(ref, arrayBacking(j, handle, elem), false);
        return ref;
    }

    jobject exportReference(Jni& j, uint32_t reference) {
        if (!reference) return 0;
        std::lock_guard lock(references);
        if (auto it = toJni.find(reference); it != toJni.end()) {
            std::lock_guard jniLock(j.mu);
            if (j.get(it->second)) return it->second;
        }
        auto& heap = machine->heap();
        const auto type = heap.typeOf(reference);
        const auto cls = className(type);
        const bool array = type.starts_with(u'[');
        const char elem = array ? char(type[1] == u'[' ? u'L' : type[1]) : 0;
        jobject out;
        if (array) {
            out = j.newArray(elem, heap.length(reference));
            std::lock_guard jniLock(j.mu);
            if (auto* object = j.get(out)) object->cls = cls;
        } else if (cls == "java/lang/Class") {
            if (auto target = machine->classMirrorType(reference)) {
                out = j.classFor(className(*target));
            } else {
                out = j.newObject(cls);
            }
        } else {
            out = cls == "java/lang/String" ? j.newString(toMutf8(heap.string(reference))) : j.newObject(cls);
        }
        toJni[reference] = out;
        fromJni[out] = reference;
        if (array) heap.bindArray(reference, arrayBacking(j, out, elem), true);
        return out;
    }

    Value toDex(Jni& j, char type, JValue value) {
        const auto valueKind = kind(type);
        return {valueKind, valueKind == ValueKind::Reference ? importReference(j, jobject(value)) : value};
    }

    JValue toNative(Jni& j, Value value) {
        return value.kind == ValueKind::Reference ? exportReference(j, uint32_t(value.bits)) : value.bits;
    }

    bool hasClass(std::u16string_view descriptor) {
        if (CoreCollections::hasClass(descriptor) || CoreLang::hasClass(descriptor) ||
            CoreMath::hasClass(descriptor) || CoreRandom::hasClass(descriptor) ||
            CoreRegex::hasClass(descriptor)) return true;
        const std::string cls = className(descriptor);
        auto& j = jni(process);
        {
            std::lock_guard lock(j.mu);
            if (j.superclasses.contains(cls)) return true;
            const std::string prefix = cls + ".";
            const auto method = j.impls.lower_bound(prefix);
            const auto field = j.staticFields.lower_bound(prefix);
            if ((method != j.impls.end() && method->first.starts_with(prefix)) ||
                (field != j.staticFields.end() && field->first.starts_with(prefix))) return true;
        }
        process.reportMissing("DEX class " + cls);
        return false;
    }

    std::optional<Value> externalField(std::u16string_view owner, std::u16string_view name,
                                       std::u16string_view signature, std::optional<uint32_t> receiver,
                                       std::optional<Value> write) {
        if (signature.empty()) return std::nullopt;
        auto& j = jni(process);
        const std::string cls = className(owner), member = toMutf8(name);
        JValue result = 0;
        const auto written = write ? toNative(j, *write) : 0;
        const auto object = receiver ? exportReference(j, *receiver) : 0;
        {
            std::lock_guard lock(j.mu);
            if (receiver) {
                if (!object) throw JavaException{u"Ljava/lang/NullPointerException;"};
                auto* slot = j.get(object);
                if (!slot) return std::nullopt;
                const auto it = slot->fields.find(member);
                if (it == slot->fields.end()) return std::nullopt;
                if (write) it->second = written;
                result = it->second;
            } else {
                const auto it = j.staticFields.find(cls + "." + member);
                if (it == j.staticFields.end()) return std::nullopt;
                if (write) it->second = written;
                result = it->second;
            }
        }
        return toDex(j, char(signature.front()), result);
    }

    std::optional<Value> external(std::u16string_view owner, std::u16string_view name,
                                  std::u16string_view signature, bool stat, bool native,
                                  std::span<const Value> args) {
        if (!native) {
            if (auto value = collections.invoke(machine->heap(), owner, name, signature, stat, args)) return value;
            if (auto value = lang.invoke(machine->heap(), owner, name, signature, stat, args)) return value;
            if (auto value = CoreMath::invoke(owner, name, signature, stat, args)) return value;
            if (auto value = random.invoke(machine->heap(), owner, name, signature, stat, args)) return value;
            if (auto value = regex.invoke(machine->heap(), lang, owner, name, signature, stat, args)) return value;
        }
        auto& j = jni(process);
        Ctx x{process, process.currentThread().cpu, process.mem()};
        const auto cls = className(owner), method = toMutf8(name), sig = toMutf8(signature);
        const auto self = stat ? j.classFor(cls) : exportReference(j, uint32_t(args.front().bits));
        std::vector<JValue> values;
        for (const auto& value : args.subspan(stat ? 0 : 1)) values.push_back(toNative(j, value));
        JValue result = 0;
        if (native) {
            if (!j.dexNative(x, cls, method, sig, self, values, result)) return std::nullopt;
        } else {
            JMethod target;
            {
                std::lock_guard lock(j.mu);
                std::string startType = cls;
                if (!stat) {
                    if (const auto* obj = j.get(self); obj && !obj->isClass && !obj->cls.empty())
                        startType = obj->cls;
                }
                auto lookup = [&](std::string type) -> JMethod {
                    for (unsigned depth = 0; depth < 64 && !type.empty(); ++depth, type = j.superOf(type)) {
                        if (auto it = j.impls.find(type + "." + method + sig); it != j.impls.end())
                            return it->second;
                    }
                    return {};
                };
                target = lookup(startType);
                if (!target && startType != cls) target = lookup(cls);
            }
            if (!target) {
                process.reportMissing("DEX " + cls + "." + method + sig);
                return std::nullopt;
            }
            result = target(j, x, self, values);
        }
        auto& exception = pendingException(process);
        if (const auto thrown = std::exchange(exception, 0u)) {
            const auto ref = importReference(j, thrown);
            throw JavaException{machine->heap().typeOf(ref), ref};
        }
        return toDex(j, returnType(sig), result);
    }

    Process& process;
    std::recursive_mutex references;
    std::unordered_map<jobject, uint32_t> fromJni;
    std::unordered_map<uint32_t, jobject> toJni;
    CoreCollections collections;
    CoreLang lang;
    CoreRandom random;
    CoreRegex regex;
    std::unique_ptr<Machine> machine;
};

} // namespace

uint32_t& pendingException(Process& process) {
    static const char key = 0;
    return process.threadSlot(&key);
}

bool invokeFromJni(Jni& j, Ctx& context, const Jni::MethodInfo& method, jobject receiver,
                   const std::vector<JValue>& args, const std::string& receiverClass,
                   bool nonvirtual, JValue& result) {
    auto& bridge = context.proc.service<Bridge>();
    if (!bridge.machine) return false;
    auto& machine = *bridge.machine;
    const auto cls = descriptor(method.cls), name = fromUtf8(method.name), sig = fromUtf8(method.sig);
    const auto actual = method.isStatic || nonvirtual || receiverClass.empty() ? cls : descriptor(receiverClass);
    if (!machine.hasMethod(actual, name, sig, method.isStatic) &&
        !machine.hasMethod(cls, name, sig, method.isStatic)) return false;
    const auto parameterTypes = argTypes(method.sig);
    if (parameterTypes.size() != args.size()) throw FormatError("JNI DEX argument count mismatch");
    std::vector<Value> converted;
    if (!method.isStatic) converted.push_back({ValueKind::Reference, bridge.importReference(j, receiver)});
    for (size_t i = 0; i < args.size(); ++i) converted.push_back(bridge.toDex(j, parameterTypes[i], args[i]));
    const auto execution = method.isStatic ? machine.callStatic(cls, name, sig, converted) :
        machine.callInstance(cls, name, sig, converted, nonvirtual || method.name == "<init>");
    if (const auto* exception = std::get_if<JavaException>(&execution)) {
        const uint32_t ref = exception->reference ? exception->reference : machine.heap().object(exception->type);
        pendingException(context.proc) = bridge.exportReference(j, ref);
        result = 0;
    } else {
        result = bridge.toNative(j, std::get<Value>(execution));
    }
    return true;
}

bool invokeActivity(Jni& j, Ctx& context, const std::string& type, jobject receiver,
                    const std::string& name, const std::string& signature, const std::vector<JValue>& args) {
    auto& pending = pendingException(context.proc);
    JValue result = 0;
    if (!invokeFromJni(j, context, {type, name, signature, false}, receiver, args, type,
                       name == "<init>", result)) return false;
    const auto exception = std::exchange(pending, 0u);
    if (exception) {
        auto& bridge = context.proc.service<Bridge>();
        std::string description;
        jobject cur = exception;
        for (int depth = 0; cur && depth < 4; ++depth) {
            std::string cls = "unknown Java exception";
            {
                std::lock_guard lock(j.mu);
                if (const auto* value = j.get(cur)) cls = value->cls;
            }
            if (depth > 0) description += " caused by ";
            description += cls;
            const uint32_t dexRef = bridge.importReference(j, cur);
            if (auto msg = bridge.lang.invoke(bridge.machine->heap(), u"Ljava/lang/Throwable;", u"getMessage",
                                              u"()Ljava/lang/String;", false, std::array<Value, 1>{{{ValueKind::Reference, dexRef}}})) {
                if (msg->bits) {
                    const std::string text = toMutf8(bridge.machine->heap().string(uint32_t(msg->bits)));
                    if (!text.empty()) description += ": " + text;
                }
            }
            cur = 0;
            if (auto cause = bridge.lang.invoke(bridge.machine->heap(), u"Ljava/lang/Throwable;", u"getCause",
                                                u"()Ljava/lang/Throwable;", false, std::array<Value, 1>{{{ValueKind::Reference, dexRef}}})) {
                if (cause->bits && uint32_t(cause->bits) != dexRef) cur = bridge.exportReference(j, uint32_t(cause->bits));
            }
        }
        throw std::runtime_error("DEX activity " + type + "." + name + signature + " raised " + description);
    }
    return true;
}

bool instantiateFromJni(Jni& j, Ctx& context, const std::string& cls, const std::string& ctorSig,
                        const std::vector<JValue>& args, jobject& out) {
    auto& bridge = context.proc.service<Bridge>();
    if (!bridge.machine) return false;
    auto& machine = *bridge.machine;
    const auto type = descriptor(cls);
    const auto info = machine.classInfo(type);
    if (!info.defined) return false;
    const auto paramTypes = argTypes(ctorSig);
    if (paramTypes.size() != args.size()) return false;
    std::vector<Value> converted;
    converted.reserve(args.size());
    for (size_t i = 0; i < args.size(); ++i) converted.push_back(bridge.toDex(j, paramTypes[i], args[i]));
    const auto execution = machine.instantiate(type, fromUtf8(ctorSig), converted);
    if (const auto* exception = std::get_if<JavaException>(&execution)) {
        const uint32_t ref = exception->reference ? exception->reference : machine.heap().object(exception->type);
        pendingException(context.proc) = bridge.exportReference(j, ref);
        out = 0;
    } else {
        out = jobject(bridge.toNative(j, std::get<Value>(execution)));
    }
    return true;
}

bool classInfoFromJni(Process& process, const std::string& cls, std::string& superCls, uint32_t& flags) {
    auto& bridge = process.service<Bridge>();
    if (!bridge.machine) return false;
    const auto info = bridge.machine->classInfo(descriptor(cls));
    if (!info.defined) return false;
    superCls = info.superName.empty() ? std::string{} : className(info.superName);
    flags = info.flags;
    return true;
}

bool isAssignableFromJni(Process& process, const std::string& fromCls, const std::string& toCls) {
    auto& bridge = process.service<Bridge>();
    if (!bridge.machine) return fromCls == toCls;
    return bridge.machine->isAssignable(descriptor(fromCls), descriptor(toCls));
}

} // namespace Hydrogen::Runtime::Android::Dex
