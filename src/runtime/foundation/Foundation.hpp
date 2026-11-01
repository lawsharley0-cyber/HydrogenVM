// HydrogenVM — shared Foundation helpers (strings, numbers, collections,
// property-list conversion, run loop) used across the Foundation and UIKit
// modules.
#pragma once
#include "runtime/objc/ObjC.hpp"
#include "loader/Plist.hpp"
#include <atomic>
#include <deque>
#include <functional>
#include <utility>

namespace Hydrogen::Runtime::Foundation {

using ObjC::id;
using ObjC::Class;
using ObjC::SEL;
using ObjC::IMP;
using ObjC::rt;

// NSStringEncoding values.
enum : uint32_t {
    kNSASCII = 1, kNSNextStep = 2, kNSJapaneseEUC = 3, kNSUTF8 = 4, kNSISOLatin1 = 5, kNSNonLossyASCII = 7,
    kNSShiftJIS = 8, kNSUnicode = 10, kNSWindowsCP1252 = 12, kNSMacOSRoman = 30,
    kNSUTF16BE = 0x90000100, kNSUTF16LE = 0x94000100, kNSUTF32 = 0x8C000100,
};

// ---- strings
struct StringData : ObjC::HostObject {
    std::u16string s;
    bool isMutable = false;
    uint32_t utf8Buffer = 0;     // guest buffer returned by UTF8String (freed on dealloc)
    uint32_t utf8Len = 0;
    ~StringData() override;
    Process* proc = nullptr;
};

// Create an autoreleased (or +1 when `owned`) NSString.
id makeString(Ctx& x, const std::u16string& s, bool owned = false, bool isMutable = false);
id makeString(Ctx& x, const std::string& utf8, bool owned = false, bool isMutable = false);
// Read any NSString-like object (ours, constant strings, guest subclasses).
std::u16string stringValue(Ctx& x, id str);
std::string utf8(Ctx& x, id str);
bool isString(Ctx& x, id obj);
// Constant string for host use (never freed).
id constString(Process& p, const std::string& utf8);
std::string decodeBytes(const uint8_t* p, size_t n, uint32_t encoding, bool* ok = nullptr);
std::string encodeBytes(const std::u16string& s, uint32_t encoding, bool lossy, bool* ok = nullptr);

// ---- numbers & values
id makeNumberInt(Ctx& x, int64_t v, bool owned = false);
id makeNumberDouble(Ctx& x, double v, bool owned = false);
id makeNumberFloat(Ctx& x, float v, bool owned = false);
id makeBool(Ctx& x, bool v);   // shared kCFBooleanTrue / False
bool isNumber(Ctx& x, id obj);
double numberDouble(Ctx& x, id n);
int64_t numberInt(Ctx& x, id n);
std::string numberToString(Ctx& x, id n);
char numberType(Ctx& x, id n);                 // objCType: 'c','i','q','f','d'
bool isBoolean(Ctx& x, id n);
id makeValue(Ctx& x, const void* bytes, uint32_t size, const std::string& objcType, bool owned = false);

// ---- collections
id makeArray(Ctx& x, const std::vector<id>& items, bool owned = false, bool isMutable = false);
std::vector<id> arrayItems(Ctx& x, id array);   // works for any NSArray (uses -count/-objectAtIndex: if needed)
bool isArray(Ctx& x, id obj);
bool dictSetRetainingKey(Ctx& x, id dict, id key, id value);
id makeDict(Ctx& x, const std::vector<std::pair<id, id>>& entries, bool owned = false, bool isMutable = false);
std::vector<std::pair<id, id>> dictEntries(Ctx& x, id dict);
id dictGet(Ctx& x, id dict, id key);
id dictGet(Ctx& x, id dict, const std::string& key);
bool isDict(Ctx& x, id obj);
id makeSet(Ctx& x, const std::vector<id>& items, bool owned = false, bool isMutable = false);
std::vector<id> setItems(Ctx& x, id set);
id makeIndexSet(Ctx& x, const std::set<uint32_t>& indices, bool owned = false, bool isMutable = false);
std::vector<uint32_t> indexSetIndices(Ctx& x, id indexSet);

// ---- data
id makeData(Ctx& x, const void* bytes, uint32_t n, bool owned = false, bool isMutable = false);
id makeDataNoCopy(Ctx& x, uint32_t bytes, uint32_t n, std::function<void(Ctx&)> onRelease);
std::vector<uint8_t> dataBytes(Ctx& x, id data);
bool isData(Ctx& x, id obj);

// ---- property lists
id plistToObject(Ctx& x, const Plist::Value& v, bool mutableContainers = false);
Plist::ValuePtr objectToPlist(Ctx& x, id obj);   // nullptr if not a plist type

// ---- equality & hashing (Cocoa semantics, with fast paths)
bool objectsEqual(Ctx& x, id a, id b);
uint32_t objectHash(Ctx& x, id a);
std::string describe(Ctx& x, id obj);            // -description as UTF-8

// ---- NSRange (by value: location, length)
struct Range { uint32_t location, length; };
inline void writeRange(Ctx& x, GuestPtr out, Range r) { x.mem.write<uint32_t>(out, r.location); x.mem.write<uint32_t>(out + 4, r.length); }
constexpr uint32_t kNotFound = 0x7FFFFFFF;

// ---- run loop
struct RunLoop {
    explicit RunLoop(Process& p) : proc(p) {}
    Process& proc;
    struct Timer {
        id timer = 0;           // NSTimer object (0 for internal timers)
        double fireAt = 0;      // seconds (reference-date based)
        double interval = 0;
        bool repeats = false;
        bool valid = true;
        std::function<void(CPU::Cpu&)> fire;
    };
    // A version-0 CFRunLoopSource: its perform callback runs on the next loop
    // iteration after it is signalled. One state object is shared by every loop
    // the source is added to; whichever loop gets to it first performs it.
    struct Source {
        long order = 0;
        std::atomic<bool> signalled{false};
        std::atomic<bool> valid{true};
        std::function<void(CPU::Cpu&)> perform;
    };
    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::shared_ptr<Timer>> timers;
    std::vector<std::shared_ptr<Source>> sources;
    std::deque<std::function<void(CPU::Cpu&)>> queue;   // performSelectorOnMainThread, dispatch main queue
    std::vector<std::function<void(CPU::Cpu&, double now)>> frameHooks;   // CADisplayLink, input pumping
    bool stopFlag = false;
    double nextFrameAt = 0;     // next frame-hook deadline (60 Hz)
    void addFrameHook(std::function<void(CPU::Cpu&, double)> h) { std::lock_guard<std::mutex> lk(mu); frameHooks.push_back(std::move(h)); }

    std::shared_ptr<Timer> addTimer(double delay, double interval, bool repeats, std::function<void(CPU::Cpu&)> fire, id timerObj = 0);
    void post(std::function<void(CPU::Cpu&)> fn);    // any thread
    void addSource(const std::shared_ptr<Source>& s);
    void removeSource(const std::shared_ptr<Source>& s);
    bool hasSource(const std::shared_ptr<Source>& s);
    void wake() { std::lock_guard<std::mutex> lk(mu); cv.notify_all(); }   // any thread
    // Process due work; returns true if anything ran. Waits up to `maxWait` seconds for work.
    bool runOnce(CPU::Cpu& c, double maxWait);
    // Run until `until` (absolute, reference-date seconds) or stop().
    void runUntil(CPU::Cpu& c, double until, bool returnAfterSource = false);
    void stop() { std::lock_guard<std::mutex> lk(mu); stopFlag = true; cv.notify_all(); }
};
RunLoop& mainRunLoop(Process& p);
RunLoop& runLoopForObject(Ctx& x, id loop);   // an NSRunLoop / CFRunLoop object (current loop if 0)
double now();          // seconds since 2001-01-01 (NSDate reference)
double dateInterval(Ctx& x, id date);                    // NSDate -> reference-date seconds
id makeDate(Ctx& x, double t, bool owned = false);       // reference-date seconds -> NSDate

// ---- bundle / URLs (Bundle.cpp)
std::string bundleResource(Ctx& x, const std::string& name, const std::string& type, const std::string& subdir = {});
id makeFileURL(Ctx& x, const std::string& path, bool isDir = false);
std::string urlFilePath(Ctx& x, id url);
std::string urlString(Ctx& x, id url);
bool isURL(Ctx& x, id obj);
id localizedString(Ctx& x, id key, id value, id table);

// ---- exceptions
[[noreturn]] void throwGuestException(Ctx& x, const std::string& name, const std::string& reason);

// ---- notifications (Notifications.cpp)
void postNotification(Ctx& x, id name, id object, id userInfo = 0);
void postNotification(Ctx& x, const std::string& name, id object, id userInfo = 0);

// ---- dispatch / operation queues (Dispatch.cpp)
// Run host work on a dispatch queue / NSOperationQueue, on a guest thread of
// that queue (main queue work runs on the main run loop).
void dispatchAsyncHost(Ctx& x, id queue, std::function<void(Ctx&)> fn);
void enqueueOnOperationQueue(Ctx& x, id opQueue, std::function<void(Ctx&)> fn);
id mainDispatchQueue(Process& p);

// ---- misc
id nullObject(Process& p);                               // [NSNull null]
bool characterSetContains(Ctx& x, id set, char16_t c);
// Lands `exception` in a guest @catch on this host→guest call. With no handler,
// runs the uncaught-exception handler and then terminates.
[[noreturn]] void raiseObjCException(Ctx& x, id exception);

// ---- class registration
void registerStrings(Process& p, ObjC::Runtime& r);
void registerNumbers(Process& p, ObjC::Runtime& r);
void registerFormatters(Process& p, ObjC::Runtime& r);
void registerSecurity(Process& p, ObjC::Runtime& r);
void registerNetwork(Process& p, ObjC::Runtime& r);
void registerCalendar(Process& p, ObjC::Runtime& r);
void registerInvocation(Process& p, ObjC::Runtime& r);
void registerURLLoading(Process& p, ObjC::Runtime& r);
void registerFoundationExtras(Process& p, ObjC::Runtime& r);
void registerCompatibilityClasses(Process& p, ObjC::Runtime& r);
void registerCollections(Process& p, ObjC::Runtime& r);
void registerData(Process& p, ObjC::Runtime& r);
void registerBundle(Process& p, ObjC::Runtime& r);
void registerDefaults(Process& p, ObjC::Runtime& r);
void registerNotifications(Process& p, ObjC::Runtime& r);
void registerRunLoop(Process& p, ObjC::Runtime& r);
void registerDispatch(Process& p, ObjC::Runtime& r);
void registerMisc(Process& p, ObjC::Runtime& r);
void registerCoreFoundation(Process& p, ObjC::Runtime& r);
void registerArchiving(Process& p, ObjC::Runtime& r);

// Convenience: autorelease an owned object from host code.
inline id autoreleased(Ctx& x, id obj) { return rt(x).autorelease(x.cpu, obj); }

} // namespace Hydrogen::Runtime::Foundation
