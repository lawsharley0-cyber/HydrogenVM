#include "runtime/android/dex/Machine.hpp"

#include <condition_variable>
#include <map>
#include <set>
#include <thread>

namespace Hydrogen::Runtime::Android::Dex {
namespace {

using Symbol = std::pair<std::u16string, std::u16string>;

struct LinkedClass;
struct BoundMethod {
    LinkedClass* owner;
    EncodedMethod method;
};
struct BoundField {
    LinkedClass* owner;
    Value initial;
    bool isStatic;
};

struct LinkedClass {
    std::u16string name, superName;
    std::shared_ptr<const File> file;
    uint32_t flags = 0;
    LinkedClass* super = nullptr;
    std::vector<LinkedClass*> interfaces;
    std::vector<std::u16string> externalInterfaces;
    std::map<Symbol, BoundMethod> local;
    std::map<Symbol, BoundMethod> vtable;
    std::map<Symbol, BoundField> fields;
    enum Status { Uninitialized, Initializing, Initialized, Failed } status = Uninitialized;
    std::mutex initMutex;
    std::condition_variable initChanged;
    std::thread::id initializer;
};

Value zero(std::u16string_view type) {
    if (type.starts_with(u'[') || type.starts_with(u'L')) return {ValueKind::Reference};
    if (type == u"J" || type == u"D") return {ValueKind::Wide};
    return {ValueKind::Word};
}

Value narrowed(std::u16string_view type, Value value) {
    if (value.kind != zero(type).kind) throw FormatError("field value kind disagrees with type");
    if (type == u"Z") value.bits &= 1;
    if (type == u"B") value.bits = uint32_t(int8_t(uint8_t(value.bits)));
    if (type == u"S") value.bits = uint32_t(int16_t(uint16_t(value.bits)));
    if (type == u"C") value.bits &= 65535;
    return value;
}

const BoundMethod* localMethod(LinkedClass* cls, const Symbol& key) {
    for (; cls; cls = cls->super) {
        const auto it = cls->local.find(key);
        if (it != cls->local.end()) return &it->second;
    }
    return nullptr;
}

BoundField* resolveField(LinkedClass* cls, const Symbol& key) {
    const auto it = cls->fields.find(key);
    if (it != cls->fields.end()) return &it->second;
    for (auto* interface : cls->interfaces)
        if (auto* field = resolveField(interface, key)) return field;
    return cls->super ? resolveField(cls->super, key) : nullptr;
}

} // namespace

struct Machine::State final : Linkage, std::enable_shared_from_this<Machine::State> {
    State(std::shared_ptr<ClassPath> path, Scheduler schedule, Host host)
        : path(std::move(path)), schedule(std::move(schedule)), host(std::move(host)),
          arena(128 * 1024 * 1024, [this](auto from, auto to) { return assignable(from, to); }) {
        if (!this->path) throw FormatError("missing DEX class path");
    }

    std::shared_ptr<ClassPath> path;
    Scheduler schedule;
    Host host;
    Arena arena;
    std::recursive_mutex linkMutex;
    std::map<std::u16string, std::unique_ptr<LinkedClass>, std::less<>> classes;
    std::set<std::u16string, std::less<>> loading;
    std::set<std::u16string, std::less<>> absent;
    std::mutex dataMutex;
    std::map<std::u16string, uint32_t> mirrors;
    std::map<std::pair<uint32_t, const BoundField*>, Value> instanceFields;

    struct Worker {
        std::mutex mutex;
        std::condition_variable done;
        bool finished = false;
        std::exception_ptr fault;
    };
    std::map<uint32_t, std::shared_ptr<Worker>> workers;

    LinkedClass* link(std::u16string_view type, unsigned depth = 0) {
        if (depth >= 256) throw FormatError("DEX class hierarchy is too deep");
        std::lock_guard lock(linkMutex);
        if (auto it = classes.find(type); it != classes.end()) return it->second.get();
        if (absent.contains(type)) return nullptr;
        const auto [key, fresh] = loading.emplace(type);
        if (!fresh) throw FormatError("cyclic DEX class hierarchy");
        struct Remove {
            decltype(loading)& set;
            decltype(loading)::iterator key;
            ~Remove() { set.erase(key); }
        } remove{loading, key};
        const auto location = path->find(type);
        if (!location) {
            absent.emplace(type);
            return nullptr;
        }
        const auto& file = *location->file;
        const auto cls = file.classDef(location->index);
        auto made = std::make_unique<LinkedClass>();
        made->name = type;
        made->file = location->file;
        made->flags = cls.flags;
        if (cls.super != noIndex) {
            made->superName = file.type(cls.super);
            made->super = link(made->superName, depth + 1);
        }
        for (const auto id : cls.interfaces) {
            auto name = file.type(id);
            if (auto* iface = link(name, depth + 1)) made->interfaces.push_back(iface);
            else made->externalInterfaces.push_back(std::move(name));
        }

        if (made->super) made->vtable = made->super->vtable;
        auto addMethods = [&](const auto& methods, bool virt) {
            for (const auto& method : methods) {
                const auto id = file.method(method.index);
                Symbol symbol{file.string(id.name), file.signature(id.proto)};
                const BoundMethod bound{made.get(), method};
                if (!made->local.emplace(symbol, bound).second) throw FormatError("duplicate linked method");
                if (virt) made->vtable.insert_or_assign(std::move(symbol), bound);
            }
        };
        addMethods(cls.directMethods, false);
        addMethods(cls.virtualMethods, true);

        auto addFields = [&](const auto& members, bool stat) {
            for (size_t i = 0; i < members.size(); ++i) {
                const auto id = file.field(members[i].index);
                Symbol symbol{file.string(id.name), file.type(id.type)};
                Value initial = zero(symbol.second);
                if (stat && i < cls.staticValues.size()) {
                    const auto& val = cls.staticValues[i];
                    if (initial.kind == ValueKind::Reference) {
                        if (val.type == 0x17 && symbol.second == u"Ljava/lang/String;")
                            initial.bits = arena.intern(file.string(uint32_t(val.bits)));
                        else if (val.type != 0x1e) throw FormatError("unsupported reference static initializer");
                    } else {
                        if (val.type >= 0x15 && val.type != 0x1f) throw FormatError("nonprimitive static initializer");
                        initial.bits = val.bits;
                    }
                    initial = narrowed(symbol.second, initial);
                }
                if (!made->fields.emplace(std::move(symbol), BoundField{made.get(), initial, stat}).second)
                    throw FormatError("duplicate linked field");
            }
        };
        addFields(cls.staticFields, true);
        addFields(cls.instanceFields, false);
        return classes.emplace(std::u16string(type), std::move(made)).first->second.get();
    }

    LinkedClass& requireClass(std::u16string_view type) {
        if (auto* cls = link(type)) return *cls;
        throw JavaException{u"Ljava/lang/NoClassDefFoundError;"};
    }

    bool assignable(std::u16string_view from, std::u16string_view to) {
        std::set<LinkedClass*> visited;
        std::vector<LinkedClass*> pending{link(from)};
        while (!pending.empty()) {
            auto* cls = pending.back();
            pending.pop_back();
            if (!cls || !visited.insert(cls).second) continue;
            if (cls->name == to || cls->superName == to) return true;
            if (!cls->super && !cls->superName.empty() && arena.assignable(cls->superName, to)) return true;
            for (const auto& iface : cls->externalInterfaces)
                if (arena.assignable(iface, to)) return true;
            pending.push_back(cls->super);
            pending.insert(pending.end(), cls->interfaces.begin(), cls->interfaces.end());
        }
        return false;
    }

    Value callBoundary(std::u16string_view owner, const Symbol& symbol, bool isStatic, bool native,
                       std::span<const Value> args) {
        if (host.call)
            if (auto value = host.call(owner, symbol.first, symbol.second, isStatic, native, args))
                return *value;
        throw JavaException{native ? u"Ljava/lang/UnsatisfiedLinkError;" : u"Ljava/lang/NoSuchMethodError;"};
    }

    Value run(const BoundMethod& method, std::span<const Value> args, Budget& budget) {
        if (method.method.flags & 0x100) {
            const auto& file = *method.owner->file;
            const auto id = file.method(method.method.index);
            return callBoundary(method.owner->name, {file.string(id.name), file.signature(id.proto)},
                                method.method.flags & 8, true, args);
        }
        if (!method.method.codeOffset) throw JavaException{u"Ljava/lang/AbstractMethodError;"};
        const auto result = executeLinked(arena, this, *method.owner->file, method.method, args, budget);
        if (const auto* exception = std::get_if<JavaException>(&result)) throw *exception;
        return std::get<Value>(result);
    }

    void initialize(LinkedClass& cls, Budget& budget) {
        {
            std::unique_lock lock(cls.initMutex);
            const auto self = std::this_thread::get_id();
            cls.initChanged.wait(lock, [&] {
                return cls.status != LinkedClass::Initializing || cls.initializer == self;
            });
            if (cls.status == LinkedClass::Initialized || cls.status == LinkedClass::Initializing) return;
            if (cls.status == LinkedClass::Failed) throw JavaException{u"Ljava/lang/NoClassDefFoundError;"};
            cls.status = LinkedClass::Initializing;
            cls.initializer = self;
        }
        auto finish = [&](LinkedClass::Status status) {
            std::lock_guard lock(cls.initMutex);
            cls.status = status;
            cls.initializer = {};
            cls.initChanged.notify_all();
        };
        try {
            if (cls.super && !(cls.flags & 0x200)) initialize(*cls.super, budget);
            if (const auto it = cls.local.find({u"<clinit>", u"()V"}); it != cls.local.end()) {
                try { run(it->second, {}, budget); }
                catch (const JavaException& err) {
                    if (arena.assignable(err.type, u"Ljava/lang/Error;")) throw;
                    throw JavaException{u"Ljava/lang/ExceptionInInitializerError;"};
                }
            }
        } catch (...) {
            finish(LinkedClass::Failed);
            throw;
        }
        finish(LinkedClass::Initialized);
    }

    uint32_t newInstance(std::u16string type, Budget& budget) override {
        if (auto* cls = link(type)) {
            if (cls->flags & (0x200 | 0x400)) throw JavaException{u"Ljava/lang/InstantiationError;"};
            initialize(*cls, budget);
        } else if (!host.hasClass || !host.hasClass(type)) {
            throw JavaException{u"Ljava/lang/NoClassDefFoundError;"};
        }
        return arena.object(std::move(type));
    }

    uint32_t classObject(std::u16string type) override {
        if (type.starts_with(u'L') && type != u"Ljava/lang/Object;" &&
            type != u"Ljava/lang/String;" && type != u"Ljava/lang/Class;" &&
            !link(type) && (!host.hasClass || !host.hasClass(type)))
            throw JavaException{u"Ljava/lang/NoClassDefFoundError;"};
        std::lock_guard lock(dataMutex);
        auto& mirror = mirrors[type];
        if (!mirror) mirror = arena.object(u"Ljava/lang/Class;");
        return mirror;
    }

    Value field(const File& file, uint32_t index, std::optional<uint32_t> receiver,
                std::optional<Value> write, Budget& budget) override {
        const auto id = file.field(index);
        const auto name = file.type(id.owner);
        const Symbol key{file.string(id.name), file.type(id.type)};
        auto* cls = link(name);
        auto* member = cls ? resolveField(cls, key) : nullptr;
        if (!member) {
            if (host.field) {
                if (auto value = host.field(name, key.first, key.second, receiver, write)) return *value;
            }
            throw JavaException{u"Ljava/lang/NoSuchFieldError;"};
        }
        if (member->isStatic == receiver.has_value()) throw JavaException{u"Ljava/lang/IncompatibleClassChangeError;"};
        if (!receiver) initialize(*member->owner, budget);
        else if (!arena.assignable(arena.typeOf(*receiver), member->owner->name))
            throw FormatError("instance field receiver has unrelated type");
        if (write) {
            *write = narrowed(key.second, *write);
            if (write->kind == ValueKind::Reference && write->bits &&
                !arena.assignable(arena.typeOf(uint32_t(write->bits)), key.second))
                throw FormatError("instance field value has unrelated type");
        }
        std::lock_guard lock(dataMutex);
        if (!receiver) {
            if (write) member->initial = *write;
            return member->initial;
        }
        const auto slot = std::pair{*receiver, static_cast<const BoundField*>(member)};
        if (write) instanceFields.insert_or_assign(slot, *write);
        const auto it = instanceFields.find(slot);
        return it == instanceFields.end() ? member->initial : it->second;
    }

    Value threadMethod(const LinkedClass& receiver, const Symbol& key, uint32_t reference) {
        if (key.second != u"()V") throw JavaException{u"Ljava/lang/NoSuchMethodError;"};
        if (key.first == u"<init>") return {ValueKind::Void};
        if (key.first == u"join") {
            std::shared_ptr<Worker> task;
            {
                std::lock_guard lock(dataMutex);
                if (const auto it = workers.find(reference); it != workers.end()) task = it->second;
            }
            if (task) {
                std::unique_lock lock(task->mutex);
                task->done.wait(lock, [&] { return task->finished; });
                if (task->fault) std::rethrow_exception(task->fault);
            }
            return {ValueKind::Void};
        }
        if (key.first != u"start") throw JavaException{u"Ljava/lang/NoSuchMethodError;"};
        if (!schedule) throw JavaException{u"Ljava/lang/UnsupportedOperationException;"};
        const auto entry = receiver.vtable.find({u"run", u"()V"});
        if (entry == receiver.vtable.end()) throw JavaException{u"Ljava/lang/NoSuchMethodError;"};
        auto task = std::make_shared<Worker>();
        {
            std::lock_guard lock(dataMutex);
            if (!workers.emplace(reference, task).second)
                throw JavaException{u"Ljava/lang/IllegalThreadStateException;"};
        }
        auto work = [self = shared_from_this(), task, method = entry->second, reference] {
            try {
                Budget budget{1'000'000};
                const Value argument{ValueKind::Reference, reference};
                self->run(method, {&argument, 1}, budget);
            } catch (const JavaException&) {
                // An uncaught guest exception ends this Java thread.
            } catch (...) {
                task->fault = std::current_exception();
            }
            std::lock_guard lock(task->mutex);
            task->finished = true;
            task->done.notify_all();
        };
        try {
            if (schedule(std::move(work))) return {ValueKind::Void};
        } catch (...) {
            std::lock_guard lock(dataMutex);
            workers.erase(reference);
            throw;
        }
        std::lock_guard lock(dataMutex);
        workers.erase(reference);
        throw JavaException{u"Ljava/lang/OutOfMemoryError;"};
    }

    Value invoke(const File& file, const EncodedMethod& caller, uint32_t methodIndex,
                 InvokeKind kind, std::span<const Value> args, Budget& budget) override {
        const auto id = file.method(methodIndex);
        const auto owner = file.type(id.owner);
        const Symbol key{file.string(id.name), file.signature(id.proto)};
        if (kind == InvokeKind::Static) return callStatic(owner, key, args, budget);
        if (args.empty() || !args[0].bits) throw JavaException{u"Ljava/lang/NullPointerException;"};
        if (kind == InvokeKind::Direct && owner == u"Ljava/lang/Object;" && key == Symbol{u"<init>", u"()V"})
            return {ValueKind::Void};
        const auto reference = uint32_t(args[0].bits);
        auto* actual = link(arena.typeOf(reference));
        if (!actual) return callBoundary(owner, key, false, false, args);
        auto& receiver = *actual;
        auto* declaredClass = link(owner);
        if (!declaredClass) {
            if (arena.assignable(receiver.name, owner)) {
                if (kind == InvokeKind::Virtual || kind == InvokeKind::Interface) {
                    const auto method = receiver.vtable.find(key);
                    if (method != receiver.vtable.end()) return run(method->second, args, budget);
                }
                if (owner == u"Ljava/lang/Thread;") return threadMethod(receiver, key, reference);
            }
            return callBoundary(owner, key, false, false, args);
        }
        auto& declared = *declaredClass;
        if (!arena.assignable(receiver.name, declared.name)) throw JavaException{u"Ljava/lang/IncompatibleClassChangeError;"};
        const BoundMethod* resolved = nullptr;
        switch (kind) {
            case InvokeKind::Direct: {
                const auto it = declared.local.find(key);
                if (it != declared.local.end()) resolved = &it->second;
                break;
            }
            case InvokeKind::Super: {
                auto* parent = requireClass(file.type(file.method(caller.index).owner)).super;
                if (parent) {
                    const auto it = parent->vtable.find(key);
                    if (it != parent->vtable.end()) resolved = &it->second;
                }
                break;
            }
            case InvokeKind::Virtual: case InvokeKind::Interface: {
                const auto it = receiver.vtable.find(key);
                if (it != receiver.vtable.end()) resolved = &it->second;
                break;
            }
            case InvokeKind::Static: break;
        }
        if (!resolved) throw JavaException{u"Ljava/lang/NoSuchMethodError;"};
        if (resolved->method.flags & 8) throw JavaException{u"Ljava/lang/IncompatibleClassChangeError;"};
        return run(*resolved, args, budget);
    }

    Value callStatic(std::u16string_view owner, const Symbol& key, std::span<const Value> args, Budget& budget) {
        auto* cls = link(owner);
        if (!cls) return callBoundary(owner, key, true, false, args);
        const auto* method = localMethod(cls, key);
        if (!method) throw JavaException{u"Ljava/lang/NoSuchMethodError;"};
        if (!(method->method.flags & 8)) throw JavaException{u"Ljava/lang/IncompatibleClassChangeError;"};
        initialize(*method->owner, budget);
        return run(*method, args, budget);
    }
};

Machine::Machine(std::shared_ptr<ClassPath> classPath, Scheduler scheduler, Host host)
    : state_(std::make_shared<State>(std::move(classPath), std::move(scheduler), std::move(host))) {}
Machine::~Machine() = default;
Heap& Machine::heap() { return state_->arena; }

bool Machine::hasMethod(std::u16string_view type, std::u16string_view name,
                        std::u16string_view signature, bool isStatic) {
    const auto* method = localMethod(state_->link(type), {std::u16string(name), std::u16string(signature)});
    return method && bool(method->method.flags & 8) == isStatic;
}

Execution Machine::callInstance(std::u16string_view type, std::u16string_view name,
                                std::u16string_view signature, std::span<const Value> args, bool nonvirtual) {
    Budget budget{1'000'000};
    try {
        if (args.empty() || !args[0].bits) throw JavaException{u"Ljava/lang/NullPointerException;"};
        auto& cls = state_->requireClass(nonvirtual ? type : state_->arena.typeOf(uint32_t(args[0].bits)));
        const Symbol key{std::u16string(name), std::u16string(signature)};
        const BoundMethod* method = nullptr;
        if (nonvirtual) method = localMethod(&cls, key);
        else if (const auto it = cls.vtable.find(key); it != cls.vtable.end()) method = &it->second;
        if (!method || (method->method.flags & 8)) throw JavaException{u"Ljava/lang/NoSuchMethodError;"};
        if (name == u"<init>") state_->initialize(*method->owner, budget);
        return state_->run(*method, args, budget);
    } catch (const JavaException& exception) { return exception; }
}

Execution Machine::callStatic(std::u16string_view type, std::u16string_view name,
                              std::u16string_view signature, std::span<const Value> args,
                              uint64_t instructionLimit) {
    Budget budget{instructionLimit};
    try { return state_->callStatic(type, {std::u16string(name), std::u16string(signature)}, args, budget); }
    catch (const JavaException& exception) { return exception; }
}

} // namespace Hydrogen::Runtime::Android::Dex
