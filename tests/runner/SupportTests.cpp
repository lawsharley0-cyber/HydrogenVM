#include "runtime/Support.hpp"
#include "runtime/foundation/Foundation.hpp"
#include "runtime/objc/ObjC.hpp"
#include <cstdio>
#include <filesystem>
#include <memory>
#include <thread>
#include <unistd.h>

using namespace Hydrogen::Runtime;

namespace {
int32_t zero(Ctx&) { return 0; }
}

int main() {
    int passed = 0, failed = 0;
    auto check = [&](bool ok, const char* name) {
        std::printf("%s %s\n", ok ? "ok" : "FAIL", name);
        ok ? ++passed : ++failed;
    };
    ProcessConfig cfg;
    cfg.log = [](LogLevel, const std::string&) {};
    const auto home = std::filesystem::temp_directory_path() / ("hvm-support-" + std::to_string(getpid()));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    } cleanup{home};
    cfg.homePath = home.string();
    Process p(cfg);
    auto& registry = supportRegistry(p);
    for (const char* name : {"_mprotect", "_madvise", "_msync", "_glDrawBuffers", "_glReadBuffer",
                            "_glGenQueriesEXT", "_glGenQueries", "_glDeleteQueriesEXT", "_glDeleteQueries",
                            "_glIsQueryEXT", "_glIsQuery", "_glBeginQueryEXT", "_glBeginQuery",
                            "_glEndQueryEXT", "_glEndQuery", "_glGetQueryivEXT", "_glGetQueryiv",
                            "_glGetQueryObjectuivEXT", "_glGetQueryObjectuiv", "_AUGraphGetIndNode",
                            "_AUGraphGetNodeCount", "_AudioUnitAddRenderNotify", "_SCNetworkReachabilityGetFlags"}) {
        const auto r = registry.find(name);
        check(r && r->status == SupportStatus::Stub && !r->provenance.empty(), name);
    }
    const auto playable = registry.find("-[AVAsset isPlayable]");
    check(playable && playable->kind == SupportKind::InstanceMethod &&
          playable->status == SupportStatus::Stub, "AVAsset playable metadata");
    check(registry.find("_dlopen") && registry.find("_dlopen")->status == SupportStatus::Approx,
          "dlopen partial implementation is explicit");
    check(!registry.find("_unclassified"), "unknown metadata is not Real");
    const auto address = supportDef<&zero>(p, "_support_test", SupportStatus::Real, "Test-only constant oracle");
    check(p.lookupSymbol("_support_test") == address && registry.find("_support_test")->status == SupportStatus::Real,
          "function helper preserves binding and metadata");
    auto builder = ObjC::rt(p).defineClass("HVMStatusTest", "NSObject");
    supportMethod(p, builder, "HVMStatusTest", "ready", [](Ctx& x) { x.cpu.r[0] = 1; },
                  SupportStatus::Approx, "Test-only class method", "c@:", true);
    check(registry.find("+[HVMStatusTest ready]")->kind == SupportKind::ClassMethod &&
          ObjC::rt(p).lookup(builder.meta(), ObjC::rt(p).sel("ready")) != 0,
          "class and instance methods use distinct names");
    for (const auto status : {SupportStatus::Real, SupportStatus::Approx, SupportStatus::Stub, SupportStatus::Unavailable})
        check(supportStatusName(status)[0] != '\0', "all four statuses have names");
    supportDef<&zero>(p, "_unavailable", SupportStatus::Unavailable, "Test-only unsupported operation");
    check(registry.find("_unavailable")->status == SupportStatus::Unavailable,
          "Unavailable is retained as a distinct status");
    for (const char* invalid : {"", "  \t", "first\nsecond", "first\rsecond"}) {
        bool rejected = false;
        try { supportDef<&zero>(p, "_invalid", SupportStatus::Stub, invalid); }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected && !p.lookupSymbol("_invalid") && !registry.find("_invalid"),
              "invalid provenance rejected before binding");
    }
    std::thread writer([&] {
        for (int i = 0; i < 100; ++i)
            registry.record({SupportKind::Function, "_concurrent", SupportStatus::Stub, "Test writer"});
    });
    for (int i = 0; i < 100; ++i) (void)registry.snapshot();
    writer.join();
    check(registry.find("_concurrent").has_value(), "concurrent snapshot and registration");
    {
        Process other(cfg);
        check(!supportRegistry(other).find("_support_test"), "registry belongs to its process");
    }
    auto copy = registry.snapshot();
    copy.clear();
    check(!registry.snapshot().empty(), "snapshot does not expose mutable storage");
    {
        auto& r = ObjC::rt(p);
        auto& cpu = p.mainThread().cpu;
        p.mem().map(0xBEF00000u, 1u << 20, Hydrogen::Memory::kProtRW);
        cpu.r[Hydrogen::CPU::kSP] = 0xBF000000u - 256;
        const auto playerClass = r.getClass("GKLocalPlayer");
        const ObjC::id player = playerClass ? ObjC::id(r.send(cpu, playerClass, "localPlayer")) : 0;
        check(player && player == ObjC::id(r.send(cpu, playerClass, "localPlayer")) &&
              !(r.send(cpu, player, "isAuthenticated") & 0xFF), "GameKit local player is a stable unauthenticated singleton");

        struct Result { int calls = 0; uint32_t value = ~0u; int32_t code = 0; std::string domain; };
        auto exercise = [&](ObjC::id target, const char* selector, bool hasValue, int32_t expectedCode, const char* name) {
            auto result = std::make_shared<Result>();
            const uint32_t invoke = p.defineFunction(std::string("test_gk_") + selector, [result, hasValue, &r](Ctx& cx) {
                ++result->calls;
                result->value = hasValue ? cx.cpu.r[1] : 0;
                const ObjC::id error = hasValue ? cx.cpu.r[2] : cx.cpu.r[1];
                if (error) {
                    result->code = int32_t(r.send(cx.cpu, error, "code"));
                    result->domain = Foundation::utf8(cx, ObjC::id(r.send(cx.cpu, error, "domain")));
                }
            });
            const uint32_t desc = p.heap().allocZeroed(8);
            p.mem().write<uint32_t>(desc + 4, 20);
            const uint32_t block = p.heap().allocZeroed(20);
            p.mem().write<uint32_t>(block + 12, invoke);
            p.mem().write<uint32_t>(block + 16, desc);
            if (target) r.send(cpu, target, selector, {block});
            const bool deferred = result->calls == 0;
            p.heap().free(block); // the callback must retain a copy
            Foundation::mainRunLoop(p).runOnce(cpu, 0.05);
            Foundation::mainRunLoop(p).runOnce(cpu, 0);
            // Keep the descriptor alive until process teardown if delivery is delayed.
            check(deferred && result->calls == 1 && result->value == 0 && result->code == expectedCode &&
                  result->domain == "GKErrorDomain", name);
        };
        const auto make = [&](const char* name) -> ObjC::id {
            const auto cls = r.getClass(name);
            return cls ? ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, cls, r.selAlloc)), r.selInit)) : 0;
        };
        exercise(player, "authenticateWithCompletionHandler:", false, 2, "GameKit authentication completes with cancellation");
        exercise(make("GKLeaderboard"), "loadScoresWithCompletionHandler:", true, 6, "GameKit score lookup completes with nil and not-authenticated error");
        exercise(make("GKScore"), "reportScoreWithCompletionHandler:", false, 6, "GameKit score reporting completes with not-authenticated error");
    }
    std::printf("support: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
