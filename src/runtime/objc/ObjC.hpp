// HydrogenVM — Objective-C runtime (modern ABI, 32-bit).
//
// The runtime lives on the host, but every class, metaclass, object and
// selector lives in guest memory with the real ABI layout, so guest code can
// subclass host-implemented classes, read isa pointers, and pass objects
// around freely. Method tables are kept host-side (built from the image's
// method lists at load time) and dispatch happens in the objc_msgSend HLE
// function, which tail-jumps to the IMP — guest or host — with the caller's
// registers untouched.
#pragma once
#include "runtime/Process.hpp"
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

namespace Hydrogen::Runtime::ObjC {

using id = uint32_t;
using Class = uint32_t;
using SEL = uint32_t;
using IMP = uint32_t;

// Host-side state for objects of host-implemented classes.
struct HostObject {
    virtual ~HostObject() = default;
};

// Thrown for an uncaught Objective-C exception / unrecognized selector.
struct ObjCError : std::runtime_error { using std::runtime_error::runtime_error; };

class Runtime;

struct Ivar {
    std::string name, type;
    uint32_t offsetPtr = 0;      // guest address of the ivar offset variable (0 for host classes)
    uint32_t offset = 0;
    uint32_t size = 0, alignment = 0;
};

struct Property { std::string name, attributes; };

struct ClassInfo {
    Class cls = 0;               // guest address of the class object
    Class meta = 0;              // its metaclass (for a metaclass: the root metaclass)
    Class instanceClass = 0;     // for a metaclass: the class it describes
    bool isMeta = false;
    bool host = false;           // implemented by HydrogenVM
    bool stub = false;           // auto-created for a missing framework class
    std::string name;
    uint32_t instanceSize = 4, instanceStart = 4;
    uint32_t ro = 0;             // guest class_ro_t
    std::unordered_map<SEL, IMP> methods;
    std::unordered_map<SEL, std::string> types;
    std::vector<Ivar> ivars;
    std::vector<Property> properties;
    std::vector<uint32_t> protocols;   // guest protocol_t addresses
    // +initialize state (instance class only)
    std::atomic<int> initState{0};      // 0 none, 1 running, 2 done
    uint32_t initThread = 0;
    bool hasCxxConstruct = false, hasCxxDestruct = false;
};

class ClassBuilder {
public:
    ClassBuilder(Runtime& rt, Class cls) : rt_(rt), cls_(cls) {}
    Class cls() const { return cls_; }
    Class meta() const;
    ClassBuilder& method(const std::string& sel, Process::HostFn fn, const std::string& types = "@@:");
    ClassBuilder& classMethod(const std::string& sel, Process::HostFn fn, const std::string& types = "@@:");
    template <auto F> ClassBuilder& m(const std::string& sel, const std::string& types = "@@:") {
        return method(sel, [](Ctx& x) { callWrapped<F>(x); }, types);
    }
    template <auto F> ClassBuilder& cm(const std::string& sel, const std::string& types = "@@:") {
        return classMethod(sel, [](Ctx& x) { callWrapped<F>(x); }, types);
    }
    // Alias an existing selector of this class.
    ClassBuilder& alias(const std::string& sel, const std::string& existing);
private:
    Runtime& rt_;
    Class cls_;
};

class Runtime {
public:
    explicit Runtime(Process& p);
    Process& proc() { return proc_; }

    // ---- selectors
    SEL sel(const std::string& name);
    SEL selFromString(uint32_t guestCString);    // registers using the guest string when new
    std::string selName(SEL s) const;

    // ---- classes
    ClassBuilder defineClass(const std::string& name, const std::string& superName, uint32_t instanceSize = 0);
    ClassBuilder builder(Class cls) { return ClassBuilder(*this, cls); }
    Class getClass(const std::string& name) const;
    Class classOf(id obj) const { return obj ? proc_.mem().read<uint32_t>(obj) : 0; }
    Class superclass(Class cls) const { return cls ? proc_.mem().read<uint32_t>(cls + 4) : 0; }
    bool isMeta(Class cls) const;
    std::string className(Class cls) const;
    std::string objectClassName(id obj) const { return className(classOf(obj)); }
    ClassInfo* info(Class cls) const;
    bool isKindOf(id obj, Class cls) const;
    bool isSubclass(Class sub, Class cls) const;
    // Make a stub class for a framework class HydrogenVM doesn't implement yet.
    Class stubClass(const std::string& name);

    // ---- dispatch
    IMP lookup(Class cls, SEL sel);                  // 0 if not found; no +initialize
    IMP lookupForSend(Class cls, SEL sel);           // runs +initialize if needed
    bool respondsTo(id obj, SEL s);
    // Send a message from host code. Arguments after self/_cmd are 32-bit words.
    uint64_t send(CPU::Cpu& c, id self, SEL s, std::initializer_list<uint32_t> args = {});
    uint64_t send(CPU::Cpu& c, id self, const char* selName, std::initializer_list<uint32_t> args = {}) {
        return send(c, self, sel(selName), args);
    }
    uint64_t sendSuper(CPU::Cpu& c, id self, Class startClass, SEL s, std::initializer_list<uint32_t> args = {});
    void invalidateCaches();
    [[noreturn]] void unrecognized(id self, SEL s);
    void addMethod(Class cls, SEL s, IMP imp, const std::string& types, bool replace = true);

    // ---- objects
    id createInstance(Class cls, uint32_t extraBytes = 0);
    void disposeInstance(CPU::Cpu& c, id obj);       // .cxx_destruct, weak refs, side tables, free
    template <typename T> T& host(id obj) {
        std::lock_guard<std::mutex> lk(hostMu_);
        auto& p = hostData_[obj];
        if (!p) p = std::make_unique<T>();
        return static_cast<T&>(*p);
    }
    template <typename T> T* hostIf(id obj) {
        std::lock_guard<std::mutex> lk(hostMu_);
        auto it = hostData_.find(obj);
        return it == hostData_.end() ? nullptr : dynamic_cast<T*>(it->second.get());
    }
    void setHost(id obj, std::unique_ptr<HostObject> h) { std::lock_guard<std::mutex> lk(hostMu_); hostData_[obj] = std::move(h); }

    // ---- memory management
    id retain(CPU::Cpu& c, id obj);                  // honours overridden -retain
    void release(CPU::Cpu& c, id obj);
    id autorelease(CPU::Cpu& c, id obj);
    id rootRetain(id obj);                           // NSObject's implementation
    bool rootRelease(id obj);                        // true if it should be deallocated
    uint32_t retainCount(id obj);
    void makeImmortal(id obj);
    uint32_t poolPush();
    void poolPop(CPU::Cpu& c, uint32_t token);
    void addToPool(id obj);
    // weak references
    id loadWeak(uint32_t slot);
    void storeWeak(uint32_t slot, id obj);
    void clearWeakSlot(uint32_t slot);

    // ---- ivars, properties
    const Ivar* findIvar(Class cls, const std::string& name, Class* owner = nullptr);
    std::vector<Ivar> allIvars(Class cls);

    // ---- associated objects, @synchronized
    void setAssociated(CPU::Cpu& c, id obj, uint32_t key, id value, uint32_t policy);
    id getAssociated(id obj, uint32_t key);
    void syncEnter(id obj);
    void syncExit(id obj);

    // ---- images
    void registerImage(const Loader::Image& img);
    void callLoadMethods(CPU::Cpu& c);

    // ---- well-known classes and selectors
    Class nsObject = 0, protocolClass = 0;
    SEL selAlloc = 0, selInit = 0, selDealloc = 0, selRetain = 0, selRelease = 0, selAutorelease = 0,
        selInitialize = 0, selLoad = 0, selCxxDestruct = 0, selCxxConstruct = 0, selCopy = 0, selDescription = 0,
        selRetainCount = 0, selForwardingTarget = 0, selDoesNotRecognize = 0, selRespondsTo = 0, selIsEqual = 0, selHash = 0;
    IMP rootRetainImp = 0, rootReleaseImp = 0, rootAutoreleaseImp = 0;
    uint32_t emptyCache = 0;

    struct Protocol { uint32_t addr; std::string name; };
    uint32_t getProtocol(const std::string& name) const;
    std::string protocolName(uint32_t proto) const;
    bool conformsTo(Class cls, uint32_t proto);

    // Exceptions: formats an uncaught exception for the crash report.
    std::function<std::string(id)> describeException;

private:
    void realizeClass(Class cls, std::unordered_set<Class>& visiting);
    void readMethodList(ClassInfo& ci, uint32_t list);
    void attachCategory(uint32_t cat);
    Class allocClassPair(const std::string& name, Class super, uint32_t instanceSize, bool host);
    void registerClassInfo(std::unique_ptr<ClassInfo> ci);
    void initializeClass(CPU::Cpu& c, Class cls);
    void cxxConstruct(CPU::Cpu& c, Class cls, id obj);
    void cxxDestruct(CPU::Cpu& c, Class cls, id obj);

    Process& proc_;
    mutable std::shared_mutex classMu_;
    std::unordered_map<Class, std::unique_ptr<ClassInfo>> classes_;
    std::unordered_map<std::string, Class> classByName_;
    std::unordered_map<std::string, SEL> selByName_;
    std::unordered_map<SEL, std::string> selNames_;
    mutable std::mutex selMu_;
    std::unordered_map<std::string, uint32_t> protocols_;
    std::unordered_map<uint32_t, std::string> protocolNames_;
    std::atomic<uint64_t> cacheGen_{1};

    // refcounts & weak refs
    std::mutex rcMu_;
    std::unordered_map<id, uint32_t> extraRefs_;
    std::unordered_set<id> immortal_;
    std::unordered_set<id> deallocating_;
    std::unordered_map<id, std::unordered_set<uint32_t>> weakRefs_;   // object -> slots
    std::unordered_map<uint32_t, id> weakSlots_;

    std::mutex hostMu_;
    std::unordered_map<id, std::unique_ptr<HostObject>> hostData_;

    std::mutex assocMu_;
    std::unordered_map<id, std::unordered_map<uint32_t, std::pair<id, uint32_t>>> assoc_;

    std::mutex syncMu_;
    std::unordered_map<id, std::unique_ptr<std::recursive_mutex>> syncLocks_;

    std::vector<std::pair<Class, bool>> pendingLoads_;   // (class, isCategoryOwner)
    std::vector<uint32_t> pendingCategoryLoads_;
};

Runtime& rt(Process& p);
inline Runtime& rt(Ctx& x) { return rt(x.proc); }

// Per-thread state (autorelease pools).
struct ThreadState {
    std::vector<id> pool;              // flat stack of autoreleased objects
    std::vector<uint32_t> marks;       // pool boundaries (indices into `pool`)
    id optimizedReturn = 0;            // objc_autoreleaseReturnValue handed this off at +1
};
ThreadState& threadState(Process& p);

void registerNSObject(Runtime& r);
void registerBlocks(Process& p, Runtime& r);
void registerObjCApi(Process& p, Runtime& r);

} // namespace Hydrogen::Runtime::ObjC
