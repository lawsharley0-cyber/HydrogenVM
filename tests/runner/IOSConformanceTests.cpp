// Independent contracts, not captured HydrogenVM outputs. Linux checks the
// emulated 32-bit API; it is not a test of the host's malloc or iOS hardware.
#include "runtime/foundation/Foundation.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unistd.h>

using namespace Hydrogen;
using namespace Hydrogen::Runtime;

int runIOSConformanceTests() {
    int failed = 0, passed = 0;
    auto check = [&](bool ok, const char* name) {
        std::printf("%s iOS contract: %s\n", ok ? "ok" : "FAIL", name);
        ok ? ++passed : ++failed;
    };
    const auto home = std::filesystem::temp_directory_path() /
                      ("hvm-ios-conformance-" + std::to_string(getpid()));
    std::filesystem::create_directories(home);
    {
        ProcessConfig cfg; cfg.homePath = home.string();
        cfg.log = [](LogLevel, const std::string&) {};
        Process p(cfg);
        auto& cpu = p.mainThread().cpu;
        cpu.r[CPU::kSP] = p.heap().allocZeroed(65536) + 65536 - 256;
        Ctx x{p, cpu, p.mem()}; auto& r = ObjC::rt(p);
        auto call = [&](const char* symbol, std::initializer_list<uint32_t> args) {
            return uint32_t(p.callGuest(cpu, *p.lookupSymbol(symbol), args));
        };
        auto send = [&](uint32_t obj, const char* sel, std::initializer_list<uint32_t> args = {}) {
            return uint32_t(r.send(cpu, obj, sel, args));
        };

        // Historical 32-bit scalable-zone classes, not modern nano malloc:
        // apple-oss-distributions/libmalloc tag libmalloc-53.1.1, src/magazine_malloc.c
        // SHIFT_TINY_QUANTUM=4, NUM_TINY_SLOTS=32; SHIFT_SMALL_QUANTUM=9,
        // SMALL_LIMIT=15*1024. Minion Rush's 1852 -> 2048 observation is also
        // recorded in Heap.cpp and commit 1cea9584 (#483).
        // https://github.com/apple-oss-distributions/libmalloc/tree/libmalloc-53.1.1
        const std::pair<uint32_t, uint32_t> classes[] = {
            {1,16}, {15,16}, {16,16}, {17,32}, {495,496}, {496,496},
            {497,512}, {511,512}, {512,512}, {513,1024}, {1023,1024},
            {1024,1024}, {1025,1536}, {1852,2048}, {15359,15360},
            {15360,15360}, {15361,16384}, {16384,16384}, {16385,20480}
        };
        for (auto [requested, capacity] : classes) {
            const auto address = call("_malloc", {requested});
            check(address && !(address & 15) && call("_malloc_size", {address}) == capacity,
                  ("malloc alignment/capacity for " + std::to_string(requested)).c_str());
            call("_free", {address});
        }
        // malloc(3), calloc(3), realloc(3): zero-filled bytes and preservation
        // are public API contracts; realloc need not preserve its address when
        // it grows beyond capacity. Do not assert a particular free-list layout.
        // https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man3/malloc.3.html
        auto block = call("_calloc", {7, 13});
        bool zeroed = block != 0;
        if (block) for (unsigned i = 0; i < 91; ++i) zeroed &= p.mem().read<uint8_t>(block+i) == 0;
        check(zeroed, "calloc zeroes the complete requested product");
        if (block) {
            for (unsigned i = 0; i < 91; ++i) p.mem().write<uint8_t>(block+i, uint8_t(i));
            const auto inClass = call("_realloc", {block, 95});
            // The historical allocator keeps the address when size fits its
            // existing tiny class (realloc implementation in the source above).
            check(inClass == block, "realloc within tiny-class capacity stays in place");
            block = inClass;
            const auto grown = call("_realloc", {block, 2049});
            bool preserved = grown != 0;
            if (grown) for (unsigned i=0; i<91; ++i) preserved &= p.mem().read<uint8_t>(grown+i) == uint8_t(i);
            check(preserved, "realloc growth preserves the original prefix");
            if (grown) block = grown;
            const auto capacity = call("_malloc_size", {block});
            const auto impossible = call("_realloc", {block, 0xfffffff0u});
            check(!impossible && call("_malloc_size", {block}) == capacity && p.mem().read<uint8_t>(block+90) == 90,
                  "failed realloc preserves allocation and contents");
            call("_free", {block});
        }

        // https://developer.apple.com/documentation/foundation/nslock
        // https://developer.apple.com/documentation/foundation/nsrecursivelock
        for (const auto name : {"NSLock", "NSRecursiveLock"}) {
            const auto lock = send(r.getClass(name), "new");
            const bool first = send(lock, "tryLock") != 0;
            const bool second = send(lock, "tryLock") != 0;
            check(first && second == (std::string(name) == "NSRecursiveLock"),
                  (std::string(name) + " re-entry contract").c_str());
            if (second) send(lock, "unlock");
            if (first) send(lock, "unlock");
            const bool reacquired = send(lock, "tryLock") != 0;
            check(reacquired, (std::string(name) + " reacquires after balanced unlock").c_str());
            if (reacquired) send(lock, "unlock");
        }
        // @synchronized uses recursive monitors; nil is an allowed no-op.
        // https://developer.apple.com/library/archive/documentation/Cocoa/Conceptual/Multithreading/ThreadSafety/ThreadSafety.html
        const auto object = send(r.getClass("NSObject"), "new");
        check(call("_objc_sync_enter", {object}) == 0 && call("_objc_sync_enter", {object}) == 0 &&
              call("_objc_sync_exit", {object}) == 0 && call("_objc_sync_exit", {object}) == 0 &&
              call("_objc_sync_enter", {0}) == 0 && call("_objc_sync_exit", {0}) == 0,
              "Objective-C synchronized monitor permits recursion and nil");

        // Signals with no waiters are not saved for a future waiter, and a
        // timed wait reacquires the condition lock before returning.
        // https://developer.apple.com/documentation/foundation/nscondition
        const auto condition = send(r.getClass("NSCondition"), "new");
        send(condition, "lock"); send(condition, "signal"); send(condition, "broadcast");
        const auto until = Foundation::makeDate(x, Foundation::now()+.03);
        check(send(condition, "waitUntilDate:", {until}) == 0,
              "NSCondition does not retain signals made without waiters");
        check(send(condition, "tryLock") == 0, "NSCondition timed wait returns owning its lock");
        send(condition, "unlock");

        // Signal selects a condition waiter even when a lock contender shares
        // the implementation's condition variable; broadcast releases the rest.
        // NSCondition documentation above: hold its lock around predicate/signals.
        struct Waiters { std::atomic<int> ready{0}, woke{0}, finished{0}, contenders{0}; };
        auto state=std::make_shared<Waiters>();
        auto waitFor=[](const std::atomic<int>& value, int n) {
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
            while(value.load()<n && std::chrono::steady_clock::now()<deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return value.load()>=n;
        };
        const auto waiter=p.registerHostFunction("ios_condition_waiter",[condition,state](Ctx& c) {
            auto& methods=ObjC::rt(c.proc);
            methods.send(c.cpu,condition,"lock");
            ++state->ready;
            auto date=Foundation::makeDate(c,Foundation::now()+2);
            if(methods.send(c.cpu,condition,"waitUntilDate:",{date})) ++state->woke;
            methods.send(c.cpu,condition,"unlock");
            ++state->finished;
        });
        std::vector<uint32_t> tids;
        for(int i=0;i<3;++i) tids.push_back(p.startThread(waiter,0,64u<<10,false));
        const bool ready=waitFor(state->ready,3);
        send(condition,"lock"); // all three have now released the lock into wait
        const auto contender=p.registerHostFunction("ios_condition_contender",[condition,state](Ctx& c) {
            ++state->contenders;
            ObjC::rt(c.proc).send(c.cpu,condition,"lock");
            ObjC::rt(c.proc).send(c.cpu,condition,"unlock");
        });
        tids.push_back(p.startThread(contender,0,64u<<10,false));
        const bool competing=waitFor(state->contenders,1);
        send(condition,"signal"); send(condition,"unlock");
        const bool single=waitFor(state->woke,1);
        check(ready && competing && single && state->woke.load()==1,
              "NSCondition signal reaches one condition waiter with a lock contender");
        send(condition,"lock"); send(condition,"broadcast"); send(condition,"unlock");
        check(waitFor(state->finished,3) && state->woke.load()==3,
              "NSCondition broadcast wakes all remaining condition waiters");
        for(auto tid:tids) { uint32_t value=0; p.joinThread(tid,&value); }

        // NSObject's explicit onThread selector runs on the selected thread's
        // run loop, not the caller. Use an asynchronous request and bounded wait
        // so a missing dispatch cannot hang fulltest indefinitely.
        // https://developer.apple.com/documentation/objectivec/nsobject/perform(_:on:with:waituntildone:)
        struct CrossThread { std::atomic<uint32_t> thread{0}, calledOn{0}, argument{0}; std::atomic<bool> done{false}; };
        auto cross=std::make_shared<CrossThread>();
        auto targetClass=r.defineClass("IOSConformanceTarget","NSObject");
        targetClass.method("accept:",[cross](Ctx& c) {
            cross->calledOn=c.proc.currentThread().id; cross->argument=c.cpu.r[2]; cross->done=true;
        },"v@:@");
        const auto target=send(targetClass.cls(),"new");
        const auto worker=p.registerHostFunction("ios_selector_worker",[cross](Ctx& c) {
            cross->thread=uint32_t(ObjC::rt(c.proc).send(c.cpu,ObjC::rt(c.proc).getClass("NSThread"),"currentThread"));
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
            while(!cross->done && std::chrono::steady_clock::now()<deadline) {
                const auto loop=uint32_t(ObjC::rt(c.proc).send(c.cpu,ObjC::rt(c.proc).getClass("NSRunLoop"),"currentRunLoop"));
                Foundation::runLoopForObject(c,loop).runOnce(c.cpu,.01);
            }
        });
        const auto workerID=p.startThread(worker,0,64u<<10,false);
        const auto available=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(!cross->thread && std::chrono::steady_clock::now()<available) std::this_thread::yield();
        const auto argument=Foundation::makeString(x,"thread argument");
        if(cross->thread) send(target,"performSelector:onThread:withObject:waitUntilDone:",
                              {r.sel("accept:"),cross->thread.load(),argument,0});
        uint32_t ignored=0; p.joinThread(workerID,&ignored);
        check(cross->done && cross->argument==argument && cross->calledOn!=p.mainThread().id,
              "performSelector:onThread delivers object on the target worker");

        // Apple run-loop guide: only sources/timers in the selected mode are
        // eligible; common is a membership set, not a mode that runs itself.
        // https://developer.apple.com/library/archive/documentation/Cocoa/Conceptual/Multithreading/RunLoopManagement/RunLoopManagement.html
        Foundation::RunLoop loop(p); // independent worker loop, no host pump
        const std::string custom="s:ConformanceMode", other="s:OtherMode";
        const auto start = std::chrono::steady_clock::now();
        check(!loop.runModeOnce(cpu, Foundation::now()+.5, custom) &&
              std::chrono::steady_clock::now()-start < std::chrono::milliseconds(250),
              "empty worker run mode returns NO without waiting for its deadline");
        unsigned fired=0;
        auto timer = loop.addTimer(0, 0, false, [&](CPU::Cpu&) { ++fired; }, 0, custom);
        loop.runOnce(cpu, 0, other);
        check(fired==0, "private-mode timer does not fire in another mode");
        loop.runOnce(cpu, 0, custom);
        check(fired==1 && !timer->valid, "one-shot timer fires once in its registered mode");
        auto common = loop.addTimer(0, 0, false, [&](CPU::Cpu&) { ++fired; }, 0, "s:kCFRunLoopCommonModes");
        loop.runOnce(cpu, 0, "s:UITrackingRunLoopMode");
        check(fired==2 && !common->valid, "common-mode timer fires in tracking mode");

        // A port is an input source even when it receives no messages. See
        // NSRunLoop addPort:forMode: and the recorded AoD observation in #484.
        // https://developer.apple.com/documentation/foundation/nsrunloop
        const auto nsloop=send(r.getClass("NSRunLoop"), "currentRunLoop");
        const auto port=send(r.getClass("NSPort"), "port");
        const auto mode=Foundation::makeString(x, "ConformancePortMode");
        send(nsloop, "addPort:forMode:", {port, mode});
        const auto portStart=std::chrono::steady_clock::now();
        const auto date=Foundation::makeDate(x, Foundation::now()+.04);
        check(send(nsloop, "runMode:beforeDate:", {mode,date}) != 0 &&
              std::chrono::steady_clock::now()-portStart >= std::chrono::milliseconds(20),
              "port-only run mode waits instead of returning in a busy loop");
        send(nsloop, "removePort:forMode:", {port,mode});
        check(send(nsloop, "runMode:beforeDate:", {mode,date}) == 0,
              "removing the final port makes the private mode empty");

        // NSFileManager fileExistsAtPath:isDirectory: distinguishes an existing
        // directory from a regular file. An absent path returns NO; the out
        // parameter's value for an absent path is intentionally not asserted.
        // https://developer.apple.com/documentation/foundation/filemanager/fileexists(atpath:isdirectory:)
        const auto dir=home/"Documents"/"contract";
        std::filesystem::create_directories(dir);
        std::ofstream(dir/"file") << "probe";
        const auto fm=send(r.getClass("NSFileManager"), "defaultManager");
        const auto out=p.heap().allocZeroed(4);
        const auto directory=Foundation::makeString(x, dir.string());
        const auto file=Foundation::makeString(x, (dir/"file").string());
        const auto missing=Foundation::makeString(x, (dir/"absent").string());
        check(send(fm,"fileExistsAtPath:isDirectory:",{directory,out}) && p.mem().read<uint8_t>(out),
              "NSFileManager recognizes a directory");
        p.mem().write<uint32_t>(out, 0xffffffff);
        check(send(fm,"fileExistsAtPath:isDirectory:",{file,out}) && !p.mem().read<uint8_t>(out),
              "NSFileManager recognizes a regular file");
        check(!send(fm,"fileExistsAtPath:",{missing}), "NSFileManager returns NO for an absent path");

        // NSNumberFormatter's default style is NoStyle. Actual current locale
        // is device/user-dependent: never assert all iPhones default to en_US.
        // https://developer.apple.com/documentation/foundation/numberformatter/numberstyle
        const auto formatter=send(r.getClass("NSNumberFormatter"),"new");
        check(send(formatter,"numberStyle")==0, "NSNumberFormatter starts with NoStyle");
        const auto number=Foundation::makeNumberInt(x,1234);
        check(Foundation::utf8(x,send(formatter,"stringFromNumber:",{number}))=="1234",
              "NoStyle formats an integer without grouping");
        const auto locale=send(r.getClass("NSLocale"),"currentLocale");
        check(locale && !Foundation::utf8(x,send(locale,"localeIdentifier")).empty(),
              "NSLocale currentLocale has an identifier (no universal locale assumption)");

        // Offline is the emulator's documented policy, not an inference about
        // physical reachability. SC flags bit 1 is kSCNetworkReachabilityFlagsReachable.
        // https://developer.apple.com/documentation/systemconfiguration/scnetworkreachabilityflags
        const auto reach=call("_SCNetworkReachabilityCreateWithName",{0,p.allocCString("example.invalid")});
        check(reach && call("_SCNetworkReachabilityGetFlags",{reach,out}) && p.mem().read<uint32_t>(out)==0,
              "default offline policy reports no reachable route");
        call("_CFRelease", {reach});
    }
    std::filesystem::remove_all(home);
    std::printf("iOS conformance: %d passed, %d failed\n",passed,failed);
    return failed;
}

#ifdef HVM_IOS_CONFORMANCE_MAIN
int main() { return runIOSConformanceTests(); }
#endif
