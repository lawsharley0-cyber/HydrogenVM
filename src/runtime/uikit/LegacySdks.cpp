// HydrogenVM — Offline / user-declined / no-ad paths for dead 2009–2015 third-party iOS SDKs.
//
// Each intercept is generic at the SDK contract level (no bundle-ID checks), names the
// corpus game(s) it unblocks, and follows the SDK's own documented offline / declined path.
#include "UIKit.hpp"
#include "runtime/Support.hpp"

namespace Hydrogen::Runtime::UIKit {

void applyLegacySdkOverrides(Process& p, ObjC::Runtime& r) {
        // ---- OpenFeint (servers shut down 2012)
        // Corpus games unblocked: Mega Jump 2.0 (2.4.4), Pocket Ants 1.0 (2.7.5),
        // Fruit Ninja 1.0 (2.4.5), Jetpack Joyride (2.12.3).
        // Documented behaviour: when the user declines OpenFeint in
        // OFUserFeintApprovalController (-clickedDontUseFeint), OpenFeint calls
        // +[OpenFeint userDidApproveFeint:NO] (persisting the declined choice in
        // NSUserDefaults so +hasUserSetFeintAccess becomes YES and +hasUserApprovedFeint
        // stays NO) and invokes the caller's deniedDelegate / deniedInvocation without
        // presenting the full-screen modal approval controller.
        if (const Class ofCls = r.getClass("OpenFeint")) {
            auto b = r.builder(ofCls);
            if (r.lookup(r.classOf(ofCls), r.sel("presentUserFeintApprovalModal:deniedDelegate:"))) {
                supportMethod(
                    p, b, "OpenFeint", "presentUserFeintApprovalModal:deniedDelegate:",
                    [](Ctx& x) {
                        const id self = x.cpu.r[0];
                        const uint32_t deniedPtr = x.cpu.r[3];
                        x.proc.logf(LogLevel::Info,
                                    "LegacySdks: OpenFeint taking user-declined offline path (skipping approval modal)");
                        rt(x).send(x.cpu, self, "userDidApproveFeint:", {0});
                        if (deniedPtr) {
                            // OFDelegate layout: { id mTarget; id mUserParam; NSThread* mTargetThread; SEL mSelector; }
                            const id target = x.mem.read<uint32_t>(deniedPtr + 0);
                            const id userParam = x.mem.read<uint32_t>(deniedPtr + 4);
                            const SEL sel = x.mem.read<uint32_t>(deniedPtr + 12);
                            if (target && sel) {
                                rt(x).retain(x.cpu, target);
                                if (userParam) rt(x).retain(x.cpu, userParam);
                                Process* proc = &x.proc;
                                Foundation::mainRunLoop(x.proc).post([proc, target, userParam, sel](CPU::Cpu& c) {
                                    Ctx cx{*proc, c, proc->mem()};
                                    if (rt(cx).respondsTo(target, sel))
                                        rt(cx).send(c, target, sel, {userParam});
                                    if (userParam) rt(cx).release(c, userParam);
                                    rt(cx).release(c, target);
                                });
                            }
                        }
                    },
                    SupportStatus::Approx,
                    "OpenFeint user-declined offline path (Mega Jump, Pocket Ants, Fruit Ninja): calls userDidApproveFeint:NO and posts deniedDelegate",
                    "v16@0:4^{OFDelegate=}8^{OFDelegate=}12", true);
            }
            if (r.lookup(r.classOf(ofCls), r.sel("presentUserFeintApprovalModalInvocation:deniedInvocation:"))) {
                supportMethod(
                    p, b, "OpenFeint", "presentUserFeintApprovalModalInvocation:deniedInvocation:",
                    [](Ctx& x) {
                        const id self = x.cpu.r[0];
                        const id deniedInv = x.cpu.r[3];
                        x.proc.logf(LogLevel::Info,
                                    "LegacySdks: OpenFeint 2.12+ taking user-declined offline path (skipping approval modal)");
                        rt(x).send(x.cpu, self, "userDidApproveFeint:", {0});
                        if (deniedInv) {
                            rt(x).retain(x.cpu, deniedInv);
                            Process* proc = &x.proc;
                            Foundation::mainRunLoop(x.proc).post([proc, deniedInv](CPU::Cpu& c) {
                                Ctx cx{*proc, c, proc->mem()};
                                if (rt(cx).respondsTo(deniedInv, rt(cx).sel("invoke")))
                                    rt(cx).send(c, deniedInv, "invoke");
                                rt(cx).release(c, deniedInv);
                            });
                        }
                    },
                    SupportStatus::Approx,
                    "OpenFeint 2.12+ user-declined offline path (Jetpack Joyride): calls userDidApproveFeint:NO and posts deniedInvocation",
                    "v16@0:4@8@12", true);
            }
        }

        // ---- Everyplay (servers shut down 2018)
        // Corpus game unblocked: Hungry Shark Evolution 2.5.0 (Everyplay build 1760).
        // Documented behaviour: -[Everyplay init] compares a hardcoded SDK build-expiration
        // timestamp against [NSDate date] and schedules -[Everyplay showExpiredMessage] on
        // the main thread, showing a modal UIAlertView ("This build has expired, please update.")
        // with no delegate that eats touches over the game menu. Suppressing the expired-SDK
        // alert keeps the game interactive offline.
        if (const Class epCls = r.getClass("Everyplay")) {
            auto b = r.builder(epCls);
            if (r.lookup(epCls, r.sel("showExpiredMessage"))) {
                supportMethod(
                    p, b, "Everyplay", "showExpiredMessage",
                    [](Ctx& x) {
                        x.proc.logf(LogLevel::Info,
                                    "LegacySdks: suppressed dead Everyplay SDK expired-build alert");
                    },
                    SupportStatus::Approx,
                    "Everyplay dead-SDK expired-build alert suppression (Hungry Shark Evolution)",
                    "v8@0:4", false);
            }
        }
}

void registerLegacySdks(Process& p, ObjC::Runtime& r) {
    p.onLoaded([&p, &r] { applyLegacySdkOverrides(p, r); });
}

} // namespace Hydrogen::Runtime::UIKit
