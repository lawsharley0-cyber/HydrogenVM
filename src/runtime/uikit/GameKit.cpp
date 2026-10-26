// HydrogenVM — GameKit with Game Center "not available": the local player never
// authenticates, and every request completes (asynchronously, on the main
// queue) with a GKErrorDomain error, which is what games see when the user
// cancels the Game Center sign-in. Games that weak-link GameKit and test
// NSClassFromString(@"GKLocalPlayer") see no Game Center at all.
#include "UIKit.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace Hydrogen::Runtime::UIKit {
namespace {

using Foundation::makeString;
using ObjC::id;

constexpr int32_t GKErrorCancelled = 2, GKErrorNotAuthenticated = 6;

id gkError(Ctx& x, int32_t code) {
    auto& r = ObjC::rt(x);
    const Class err = r.getClass("NSError");
    if (!err) return 0;
    return id(r.send(x.cpu, err, "errorWithDomain:code:userInfo:", {Foundation::constString(x.proc, "GKErrorDomain"), uint32_t(code), 0}));
}

// Calls `block(args..., error)` later on the main queue. `before` = number of
// nil arguments that precede the error.
void completeLater(Ctx& x, uint32_t block, int before, int32_t code) {
    if (!block) return;
    const uint32_t b = ObjC::blockCopy(x, block);
    Process* p = &x.proc;
    Foundation::mainRunLoop(x.proc).post([p, b, before, code](CPU::Cpu& c) {
        Ctx y{*p, c, p->mem()};
        const id err = gkError(y, code);
        switch (before) {
            case 0: ObjC::callBlock(y, b, {err}); break;
            case 1: ObjC::callBlock(y, b, {0, err}); break;
            default: ObjC::callBlock(y, b, {0, 0, err}); break;
        }
        ObjC::blockRelease(y, b);
    });
}

uint32_t arg(Ctx& x, int i) { return x.cpu.r[2 + i]; }
void ret(Ctx& x, uint32_t v) { x.cpu.r[0] = v; }

void lenient(ObjC::Runtime& r, Class c) {
    // Unimplemented methods return nil instead of stopping the app, and
    // NSClassFromString() reports the class as absent.
    if (auto* ci = r.info(c)) ci->stub = true;
    if (auto* mi = r.info(r.classOf(c))) mi->stub = true;
}

struct GameKitState { explicit GameKitState(Process&) {} id localPlayer = 0; };

} // namespace

void registerGameKit(Process& p, ObjC::Runtime& r) {
    for (const auto& [sym, value] : std::initializer_list<std::pair<const char*, const char*>>{
             {"_GKPlayerAuthenticationDidChangeNotificationName", "GKPlayerAuthenticationDidChangeNotificationName"},
             {"_GKPlayerDidChangeNotificationName", "GKPlayerDidChangeNotificationName"},
             {"_GKErrorDomain", "GKErrorDomain"}}) {
        const uint32_t cell = p.defineDataBlock(sym, 4);
        p.mem().write<uint32_t>(cell, Foundation::constString(p, value));
    }

    ObjC::ClassBuilder player = r.defineClass("GKPlayer", "NSObject");
    lenient(r, player.cls());
    player.method("playerID", [](Ctx& x) { ret(x, 0); });
    player.method("alias", [](Ctx& x) { ret(x, 0); });
    player.method("displayName", [](Ctx& x) { ret(x, 0); });
    player.classMethod("loadPlayersForIdentifiers:withCompletionHandler:", [](Ctx& x) { completeLater(x, arg(x, 1), 1, GKErrorNotAuthenticated); }, "v@:@@?");

    ObjC::ClassBuilder local = r.defineClass("GKLocalPlayer", "GKPlayer");
    lenient(r, local.cls());
    local.classMethod("localPlayer", [](Ctx& x) {
        auto& st = x.proc.service<GameKitState>();
        if (!st.localPlayer) {
            auto& r = ObjC::rt(x);
            st.localPlayer = id(r.send(x.cpu, id(r.send(x.cpu, r.getClass("GKLocalPlayer"), r.selAlloc)), r.selInit));
        }
        ret(x, st.localPlayer);
    });
    local.method("isAuthenticated", [](Ctx& x) { ret(x, 0); }, "c@:");
    local.method("isUnderage", [](Ctx& x) { ret(x, 0); }, "c@:");
    local.method("friends", [](Ctx& x) { ret(x, 0); });
    local.method("authenticateWithCompletionHandler:", [](Ctx& x) { completeLater(x, arg(x, 0), 0, GKErrorCancelled); }, "v@:@?");
    local.method("setAuthenticateHandler:", [](Ctx& x) { completeLater(x, arg(x, 0), 1, GKErrorCancelled); }, "v@:@?");   // (viewController, error)
    local.method("authenticateHandler", [](Ctx& x) { ret(x, 0); });
    local.method("loadFriendsWithCompletionHandler:", [](Ctx& x) { completeLater(x, arg(x, 0), 1, GKErrorNotAuthenticated); }, "v@:@?");

    ObjC::ClassBuilder score = r.defineClass("GKScore", "NSObject");
    lenient(r, score.cls());
    score.method("initWithCategory:", [](Ctx& x) { ret(x, x.cpu.r[0]); }, "@@:@");
    score.method("initWithLeaderboardIdentifier:", [](Ctx& x) { ret(x, x.cpu.r[0]); }, "@@:@");
    score.method("reportScoreWithCompletionHandler:", [](Ctx& x) { completeLater(x, arg(x, 0), 0, GKErrorNotAuthenticated); }, "v@:@?");
    score.classMethod("reportScores:withCompletionHandler:", [](Ctx& x) { completeLater(x, arg(x, 1), 0, GKErrorNotAuthenticated); }, "v@:@@?");

    ObjC::ClassBuilder ach = r.defineClass("GKAchievement", "NSObject");
    lenient(r, ach.cls());
    // A local object holding what the game sets, as on a device that isn't signed in.
    struct AchievementData : ObjC::HostObject { id identifier = 0; double percent = 0; bool banner = false; };
    auto AD = [](Ctx& x) -> AchievementData& { return ObjC::rt(x).host<AchievementData>(x.cpu.r[0]); };
    ach.method("initWithIdentifier:", [AD](Ctx& x) {
        AchievementData& d = AD(x);
        auto& r = ObjC::rt(x);
        if (d.identifier) r.release(x.cpu, d.identifier);
        d.identifier = x.cpu.r[2] ? id(r.send(x.cpu, x.cpu.r[2], "copy")) : 0;
        ret(x, x.cpu.r[0]);
    }, "@@:@");
    ach.method("identifier", [AD](Ctx& x) { ret(x, AD(x).identifier); }, "@@:");
    ach.method("setIdentifier:", [AD](Ctx& x) {
        AchievementData& d = AD(x);
        auto& r = ObjC::rt(x);
        const id v = x.cpu.r[2] ? id(r.send(x.cpu, x.cpu.r[2], "copy")) : 0;
        if (d.identifier) r.release(x.cpu, d.identifier);
        d.identifier = v;
    }, "v@:@");
    ach.method("percentComplete", [AD](Ctx& x) {   // double, returned in r0:r1 (softfp)
        uint64_t bits; const double v = AD(x).percent; std::memcpy(&bits, &v, 8);
        x.cpu.r[0] = uint32_t(bits); x.cpu.r[1] = uint32_t(bits >> 32);
    }, "d@:");
    ach.method("setPercentComplete:", [AD](Ctx& x) {
        const uint64_t bits = (uint64_t(x.cpu.r[3]) << 32) | x.cpu.r[2];
        double v; std::memcpy(&v, &bits, 8);
        AD(x).percent = std::clamp(std::isfinite(v) ? v : 0.0, 0.0, 100.0);
    }, "v@:d");
    ach.method("isCompleted", [AD](Ctx& x) { ret(x, AD(x).percent >= 100.0); }, "c@:");
    ach.method("completed", [AD](Ctx& x) { ret(x, AD(x).percent >= 100.0); }, "c@:");
    ach.method("isHidden", [](Ctx& x) { ret(x, 0); }, "c@:");
    ach.method("showsCompletionBanner", [AD](Ctx& x) { ret(x, AD(x).banner); }, "c@:");
    ach.method("setShowsCompletionBanner:", [AD](Ctx& x) { AD(x).banner = (x.cpu.r[2] & 0xFF) != 0; }, "v@:c");
    ach.method("lastReportedDate", [](Ctx& x) { ret(x, 0); }, "@@:");
    ach.method("dealloc", [AD](Ctx& x) {
        AchievementData& d = AD(x);
        if (d.identifier) ObjC::rt(x).release(x.cpu, d.identifier);
        d.identifier = 0;
        ObjC::rt(x).disposeInstance(x.cpu, x.cpu.r[0]);
    }, "v@:");
    ach.method("reportAchievementWithCompletionHandler:", [](Ctx& x) { completeLater(x, arg(x, 0), 0, GKErrorNotAuthenticated); }, "v@:@?");
    ach.classMethod("loadAchievementsWithCompletionHandler:", [](Ctx& x) { completeLater(x, arg(x, 0), 1, GKErrorNotAuthenticated); }, "v@:@?");
    ach.classMethod("resetAchievementsWithCompletionHandler:", [](Ctx& x) { completeLater(x, arg(x, 0), 0, GKErrorNotAuthenticated); }, "v@:@?");
    ach.classMethod("reportAchievements:withCompletionHandler:", [](Ctx& x) { completeLater(x, arg(x, 1), 0, GKErrorNotAuthenticated); }, "v@:@@?");

    ObjC::ClassBuilder desc = r.defineClass("GKAchievementDescription", "NSObject");
    lenient(r, desc.cls());
    desc.classMethod("loadAchievementDescriptionsWithCompletionHandler:", [](Ctx& x) { completeLater(x, arg(x, 0), 1, GKErrorNotAuthenticated); }, "v@:@?");

    ObjC::ClassBuilder board = r.defineClass("GKLeaderboard", "NSObject");
    lenient(r, board.cls());
    board.method("loadScoresWithCompletionHandler:", [](Ctx& x) { completeLater(x, arg(x, 0), 1, GKErrorNotAuthenticated); }, "v@:@?");
    board.classMethod("loadCategoriesWithCompletionHandler:", [](Ctx& x) { completeLater(x, arg(x, 0), 2, GKErrorNotAuthenticated); }, "v@:@?");
    board.classMethod("loadLeaderboardsWithCompletionHandler:", [](Ctx& x) { completeLater(x, arg(x, 0), 1, GKErrorNotAuthenticated); }, "v@:@?");

    // View controllers: blank screens if an app presents one anyway.
    for (const char* name : {"GKLeaderboardViewController", "GKAchievementViewController", "GKGameCenterViewController",
                             "GKFriendRequestComposeViewController", "GKMatchmakerViewController"})
        lenient(r, r.defineClass(name, "UIViewController").cls());
    for (const char* name : {"GKMatchmaker", "GKMatchRequest", "GKMatch", "GKTurnBasedMatch", "GKInvite", "GKNotificationBanner", "GKSession", "GKPeerPickerController"})
        lenient(r, r.defineClass(name, "NSObject").cls());
}

} // namespace Hydrogen::Runtime::UIKit
