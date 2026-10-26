// HydrogenVM — UIApplication, UIApplicationMain (the launch sequence), UITouch /
// UIEvent and touch delivery, UIDevice, UIAccelerometer.
#include "UIKit.hpp"
#include <chrono>
#include <filesystem>

namespace fs = std::filesystem;

namespace Hydrogen::Runtime { void flushGuestStdio(Process& p); }
namespace Hydrogen::Runtime::Foundation { void flushUserDefaults(Process& p); }

namespace Hydrogen::Runtime::UIKit {

using Foundation::autoreleased;
using Foundation::makeString;
using Foundation::utf8;

// Gestures.cpp
bool gestureFilterTouches(Ctx& x, id view, const std::vector<id>& touches, int phase, id event);

namespace {

const std::string kP = "{CGPoint=ff}";

double nowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// ================================================================= UITouch ===

struct TouchData : ObjC::HostObject {
    G::Point loc, prev;              // window coordinates
    int32_t phase = 0, tapCount = 1;
    double timestamp = 0;
    id view = 0, window = 0;         // view: retained while the touch lives
    std::vector<id> recognizers;
};
TouchData& TD(Ctx& x, id t) { return rt(x).host<TouchData>(t); }

void t_locationInView(Ctx& x, GuestPtr ret, id self, SEL, id view) {
    TouchData& d = TD(x, self);
    G::Point p = d.loc;
    if (view) p = convertPoint(x, d.loc, d.window, view);
    else if (d.window) p = layerToSuper(layerData(x, viewLayer(x, d.window))).apply(d.loc);
    CG::writeStruct(x, ret, CG::fromG(p));
}
void t_prevLocationInView(Ctx& x, GuestPtr ret, id self, SEL, id view) {
    TouchData& d = TD(x, self);
    G::Point p = d.prev;
    if (view) p = convertPoint(x, d.prev, d.window, view);
    CG::writeStruct(x, ret, CG::fromG(p));
}
int32_t t_phase(Ctx& x, id self, SEL) { return TD(x, self).phase; }
int32_t t_tapCount(Ctx& x, id self, SEL) { return TD(x, self).tapCount; }
double t_timestamp(Ctx& x, id self, SEL) { return TD(x, self).timestamp; }
id t_view(Ctx& x, id self, SEL) { return TD(x, self).view; }
id t_window(Ctx& x, id self, SEL) { return TD(x, self).window; }
id t_recognizers(Ctx& x, id self, SEL) { return Foundation::makeArray(x, TD(x, self).recognizers); }
float t_majorRadius(Ctx&, id, SEL) { return 20; }
void t_dealloc(Ctx& x, id self, SEL) {
    if (TouchData* d = rt(x).hostIf<TouchData>(self)) { const id v = d->view; d->view = 0; if (v) rt(x).release(x.cpu, v); }
    rt(x).disposeInstance(x.cpu, self);
}

// ================================================================= UIEvent ===

struct EventData : ObjC::HostObject { std::vector<id> touches; double timestamp = 0; int32_t type = 0, subtype = 0; };
EventData& ED(Ctx& x, id e) { return rt(x).host<EventData>(e); }
id e_allTouches(Ctx& x, id self, SEL) { return Foundation::makeSet(x, ED(x, self).touches); }
id e_touchesForView(Ctx& x, id self, SEL, id view) {
    std::vector<id> out;
    for (id t : ED(x, self).touches) if (TD(x, t).view == view) out.push_back(t);
    return Foundation::makeSet(x, out);
}
id e_touchesForWindow(Ctx& x, id self, SEL, id w) {
    std::vector<id> out;
    for (id t : ED(x, self).touches) if (TD(x, t).window == w) out.push_back(t);
    return Foundation::makeSet(x, out);
}
id e_touchesForGesture(Ctx& x, id self, SEL, id g) {
    std::vector<id> out;
    for (id t : ED(x, self).touches) { auto& r = TD(x, t).recognizers; if (std::find(r.begin(), r.end(), g) != r.end()) out.push_back(t); }
    return Foundation::makeSet(x, out);
}
double e_timestamp(Ctx& x, id self, SEL) { return ED(x, self).timestamp; }
int32_t e_type(Ctx& x, id self, SEL) { return ED(x, self).type; }
int32_t e_subtype(Ctx& x, id self, SEL) { return ED(x, self).subtype; }
void e_dealloc(Ctx& x, id self, SEL) {
    if (EventData* d = rt(x).hostIf<EventData>(self)) { auto t = std::move(d->touches); d->touches.clear(); for (id o : t) rt(x).release(x.cpu, o); }
    rt(x).disposeInstance(x.cpu, self);
}

// Deliver one event's touches to their views (UIWindow -sendEvent:).
void w_sendEvent(Ctx& x, id self, SEL, id event) {
    EventData* e = rt(x).hostIf<EventData>(event);
    if (!e) return;
    auto& r = rt(x);
    for (int phase : {0, 1, 3, 4}) {
        // group by view
        std::vector<std::pair<id, std::vector<id>>> groups;
        for (id t : e->touches) {
            TouchData& td = TD(x, t);
            if (td.phase != phase || td.window != self || !td.view) continue;
            auto it = std::find_if(groups.begin(), groups.end(), [&](auto& g) { return g.first == td.view; });
            if (it == groups.end()) groups.push_back({td.view, {t}}); else it->second.push_back(t);
        }
        static const char* sels[] = {"touchesBegan:withEvent:", "touchesMoved:withEvent:", nullptr, "touchesEnded:withEvent:", "touchesCancelled:withEvent:"};
        for (auto& [view, ts] : groups) {
            r.retain(x.cpu, view);
            const bool cancelled = gestureFilterTouches(x, view, ts, phase, event);
            const id set = Foundation::makeSet(x, ts);
            if (!cancelled) r.send(x.cpu, view, sels[phase], {set, event});
            else if (phase != 4) {
                // a recognizer took over: the view sees the touches cancelled (once)
                r.send(x.cpu, view, "touchesCancelled:withEvent:", {set, event});
                for (id t : ts) { TouchData& td = TD(x, t); const id v = td.view; td.view = 0; if (v) r.release(x.cpu, v); }
            }
            r.release(x.cpu, view);
        }
    }
}
void app_sendEvent(Ctx& x, id self, SEL, id event) {
    std::vector<id> wins;
    if (EventData* e = rt(x).hostIf<EventData>(event))
        for (id t : e->touches) { const id w = TD(x, t).window; if (w && std::find(wins.begin(), wins.end(), w) == wins.end()) wins.push_back(w); }
    for (id w : wins) rt(x).send(x.cpu, w, "sendEvent:", {event});
}

id topWindowAt(Ctx& x, G::Point screenP) {
    auto& u = ui(x);
    id best = 0;
    float bestLevel = -1e9f;
    for (id w : u.windows) {
        ViewData& d = viewData(x, w);
        if (!d.visible || layerData(x, viewLayer(x, w)).hidden) continue;
        const G::Point wp = layerToSuper(layerData(x, viewLayer(x, w))).inverted().apply(screenP);
        if (!layerData(x, viewLayer(x, w)).bounds.contains(wp)) continue;
        if (!best || d.windowLevel >= bestLevel) { best = w; bestLevel = d.windowLevel; }
    }
    return best ? best : (u.keyWindow ? u.keyWindow : (!u.windows.empty() ? u.windows.front() : 0));
}

struct TapMemory { G::Point p; double t = 0; int count = 0; };

} // namespace

void deliverPendingInput(Ctx& x) {
    auto& u = ui(x);
    std::deque<UIState::HostTouch> pending;
    {
        std::lock_guard<std::mutex> lk(u.inputMu);
        pending.swap(u.pendingTouches);
    }
    if (pending.empty()) return;
    auto& r = rt(x);
    static thread_local TapMemory lastTap;
    for (const auto& ht : pending) {
        if (u.ignoreCount > 0) continue;
        const G::Point sp{ht.x, ht.y};
        auto it = std::find_if(u.activeTouches.begin(), u.activeTouches.end(), [&](auto& t) { return t.hostId == ht.id; });
        id touch = 0;
        if (ht.phase == 0) {
            if (it != u.activeTouches.end()) continue;   // duplicate begin
            const id window = topWindowAt(x, sp);
            if (!window) continue;
            touch = r.createInstance(u.touch);
            TouchData& td = TD(x, touch);
            td.window = window;
            td.loc = td.prev = layerToSuper(layerData(x, viewLayer(x, window))).inverted().apply(sp);
            td.timestamp = ht.time;
            const id event0 = 0;
            id view = hitTest(x, window, td.loc, event0);
            // a view that isn't multi-touch only gets one touch at a time
            if (view && !viewData(x, view).multipleTouch) {
                for (auto& at : u.activeTouches) if (at.view == view) { view = 0; break; }
            }
            td.view = view ? r.retain(x.cpu, view) : 0;
            if (ht.time - lastTap.t < 0.35 && std::hypot(sp.x - lastTap.p.x, sp.y - lastTap.p.y) < 25) td.tapCount = lastTap.count + 1;
            u.activeTouches.push_back({touch, view, ht.id});
        } else {
            if (it == u.activeTouches.end()) continue;
            touch = it->touch;
            TouchData& td = TD(x, touch);
            td.prev = td.loc;
            if (td.window) td.loc = layerToSuper(layerData(x, viewLayer(x, td.window))).inverted().apply(sp);
            td.phase = ht.phase == 1 ? 1 : ht.phase == 3 ? 3 : 4;
            td.timestamp = ht.time;
            if (ht.phase == 1 && td.prev.x == td.loc.x && td.prev.y == td.loc.y) continue;   // no movement: no event
            if (ht.phase >= 3) { lastTap = {sp, ht.time, td.tapCount}; }
        }
        // build the event with all live touches
        const id event = r.createInstance(u.event);
        EventData& ed = ED(x, event);
        ed.timestamp = ht.time;
        for (auto& at : u.activeTouches) ed.touches.push_back(r.retain(x.cpu, at.touch));
        const uint32_t pool = r.poolPush();
        try {
            r.send(x.cpu, u.app ? u.app : event, u.app ? "sendEvent:" : "self", {event});
        } catch (...) { r.poolPop(x.cpu, pool); r.release(x.cpu, event); throw; }
        r.poolPop(x.cpu, pool);
        r.release(x.cpu, event);
        if (ht.phase >= 3) {
            auto it2 = std::find_if(u.activeTouches.begin(), u.activeTouches.end(), [&](auto& t) { return t.hostId == ht.id; });
            if (it2 != u.activeTouches.end()) {
                const id t = it2->touch;
                u.activeTouches.erase(it2);
                r.release(x.cpu, t);
            }
        }
    }
    setNeedsComposite(x);
}

namespace {

// ============================================================ UIApplication ===

id app_shared(Ctx& x, id, SEL) { return ui(x).app; }
id app_delegate(Ctx& x, id, SEL) { return ui(x).appDelegate; }
void app_setDelegate(Ctx& x, id, SEL, id d) { ui(x).appDelegate = d; }
id app_keyWindow(Ctx& x, id, SEL) { return ui(x).keyWindow; }
id app_windows(Ctx& x, id, SEL) {
    std::vector<id> v;
    for (id w : ui(x).windows) if (viewData(x, w).visible) v.push_back(w);
    return Foundation::makeArray(x, v);
}
bool app_statusBarHidden(Ctx& x, id, SEL) { return ui(x).statusBarHidden; }
void app_setStatusBarHidden(Ctx& x, id, SEL, bool h) {
    auto& u = ui(x);
    if (u.statusBarHidden == h) return;
    u.statusBarHidden = h;
    setNeedsComposite(x);
}
void app_setStatusBarHiddenAnim(Ctx& x, id s, SEL c, bool h, uint32_t) { app_setStatusBarHidden(x, s, c, h); }
int32_t app_statusBarOrientation(Ctx& x, id, SEL) { return ui(x).statusBarOrientation; }
void app_setStatusBarOrientation(Ctx& x, id, SEL, int32_t o) {
    auto& u = ui(x);
    if (o < 1 || o > 4 || o == u.statusBarOrientation) return;
    u.statusBarOrientation = o;
    // re-lay out root view controllers for the new orientation
    for (id w : std::vector<id>(u.windows)) if (const id vc = viewData(x, w).rootViewController) rt(x).send(x.cpu, w, "setRootViewController:", {vc});
    setNeedsComposite(x);
}
void app_setStatusBarOrientationAnim(Ctx& x, id s, SEL c, int32_t o, bool) { app_setStatusBarOrientation(x, s, c, o); }
void app_statusBarFrame(Ctx& x, GuestPtr ret, id, SEL) {
    auto& u = ui(x);
    CG::writeStruct(x, ret, u.statusBarHidden ? CGRect{{0, 0}, {0, 0}} : CGRect{{0, 0}, {u.screenW, 20}});
}
int32_t app_statusBarStyle(Ctx&, id, SEL) { return 0; }
void app_ignoreInt(Ctx&, id, SEL, int32_t) {}
void app_ignoreIntBool(Ctx&, id, SEL, int32_t, bool) {}
bool app_false(Ctx&, id, SEL) { return false; }
bool app_true(Ctx&, id, SEL) { return true; }
void app_ignoreBool(Ctx&, id, SEL, bool) {}
int32_t app_state(Ctx&, id, SEL) { return 0; }   // UIApplicationStateActive
double app_backgroundTime(Ctx&, id, SEL) { return 600; }
bool app_openURL(Ctx& x, id, SEL, id url) {
    x.proc.logf(LogLevel::Info, "openURL: %s (not opened)", url ? Foundation::urlString(x, url).c_str() : "(nil)");
    return false;
}
bool app_canOpenURL(Ctx&, id, SEL, id) { return false; }
void app_openURLOptions(Ctx& x, id s, SEL c, id url, id, uint32_t completion) {
    app_openURL(x, s, c, url);
    if (completion) {
        const uint32_t b = ObjC::blockCopy(x, completion);
        Process* p = &x.proc;
        Foundation::mainRunLoop(x.proc).post([p, b](CPU::Cpu& cpu) { Ctx cx{*p, cpu, p->mem()}; ObjC::callBlock(cx, b, {0}); ObjC::blockRelease(cx, b); });
    }
}
struct AppFlags : ObjC::HostObject { bool idle = false, network = false; int32_t badge = 0; };
AppFlags& AF(Ctx& x, id a) { return rt(x).host<AppFlags>(a); }
bool app_idleDisabled(Ctx& x, id self, SEL) { return AF(x, self).idle; }
void app_setIdleDisabled(Ctx& x, id self, SEL, bool b) { AF(x, self).idle = b; }
bool app_network(Ctx& x, id self, SEL) { return AF(x, self).network; }
void app_setNetwork(Ctx& x, id self, SEL, bool b) { AF(x, self).network = b; }
int32_t app_badge(Ctx& x, id self, SEL) { return AF(x, self).badge; }
void app_setBadge(Ctx& x, id self, SEL, int32_t b) { AF(x, self).badge = b; }
void app_beginIgnoring(Ctx& x, id, SEL) { ++ui(x).ignoreCount; }
void app_endIgnoring(Ctx& x, id, SEL) { if (ui(x).ignoreCount > 0) --ui(x).ignoreCount; }
bool app_isIgnoring(Ctx& x, id, SEL) { return ui(x).ignoreCount > 0; }
void app_noop(Ctx&, id, SEL) {}
void app_noopObj(Ctx&, id, SEL, id) {}
uint32_t app_beginBackgroundTask(Ctx&, id, SEL, uint32_t) { return 1; }
uint32_t app_beginBackgroundTaskNamed(Ctx&, id, SEL, id, uint32_t) { return 1; }
void app_endBackgroundTask(Ctx&, id, SEL, uint32_t) {}
id app_nil(Ctx&, id, SEL) { return 0; }
bool app_sendAction(Ctx& x, id self, SEL, SEL action, id target, id sender, id event) {
    auto& r = rt(x);
    if (!action) return false;
    if (!target) {
        // nil-targeted: walk the responder chain from the first responder (or the sender)
        id cur = ui(x).firstResponder ? ui(x).firstResponder : sender;
        int guard = 0;
        while (cur && guard++ < 256) {
            if (r.respondsTo(cur, action)) { target = cur; break; }
            cur = id(r.send(x.cpu, cur, "nextResponder"));
        }
        if (!target && ui(x).appDelegate && r.respondsTo(ui(x).appDelegate, action)) target = ui(x).appDelegate;
        if (!target) return false;
    } else {
        if (!r.respondsTo(target, action)) return false;
    }
    r.send(x.cpu, target, action, {sender, event});
    return true;
}
int32_t app_layoutDirection(Ctx&, id, SEL) { return 0; }
// UIInterfaceOrientationMask from Info.plist (UISupportedInterfaceOrientations[~iphone]);
// iPhone apps without the key get AllButUpsideDown, as on iOS.
uint32_t app_supportedOrientations(Ctx& x, id, SEL, id) {
    const Plist::Value* info = x.proc.infoPlist();
    const Plist::Value* sup = info ? info->get("UISupportedInterfaceOrientations~iphone") : nullptr;
    if (!sup && info) sup = info->get("UISupportedInterfaceOrientations");
    if (!sup || sup->type != Plist::Value::Type::Array || sup->array.empty()) return 0x1A;
    uint32_t mask = 0;
    for (const auto& v : sup->array) {
        if (!v) continue;
        if (v->s == "UIInterfaceOrientationPortrait") mask |= 1u << 1;
        else if (v->s == "UIInterfaceOrientationPortraitUpsideDown") mask |= 1u << 2;
        else if (v->s == "UIInterfaceOrientationLandscapeRight") mask |= 1u << 3;
        else if (v->s == "UIInterfaceOrientationLandscapeLeft") mask |= 1u << 4;
    }
    return mask ? mask : 0x1A;
}
id app_nextResponder(Ctx& x, id, SEL) {
    const id d = ui(x).appDelegate;
    return d && rt(x).isKindOf(d, ui(x).responder) ? d : 0;
}

// ================================================================ UIDevice ===

id dev_current(Ctx& x, id, SEL) { return ui(x).currentDevice; }
id dev_systemVersion(Ctx& x, id, SEL) { return makeString(x, x.proc.config().systemVersion); }
id dev_systemName(Ctx& x, id, SEL) { return makeString(x, "iPhone OS"); }
id dev_model(Ctx& x, id, SEL) { return makeString(x, "iPhone"); }
id dev_name(Ctx& x, id, SEL) { return makeString(x, "HydrogenVM iPhone"); }
std::string hexHash(const std::string& s, size_t len) {
    uint64_t h1 = 1469598103934665603ull, h2 = 0x9E3779B97F4A7C15ull;
    for (unsigned char c : s) { h1 = (h1 ^ c) * 1099511628211ull; h2 = (h2 + c) * 6364136223846793005ull + 1; }
    std::string out;
    char b[17];
    while (out.size() < len) { std::snprintf(b, sizeof b, "%016llx", (unsigned long long)(h1 ^ (h2 << 1))); out += b; h1 = h1 * 31 + 7; h2 ^= h1; }
    return out.substr(0, len);
}
std::string bundleId(Ctx& x) { return x.proc.infoPlist() ? x.proc.infoPlist()->getString("CFBundleIdentifier") : "app"; }
id dev_uniqueId(Ctx& x, id, SEL) { return makeString(x, hexHash("udid:" + bundleId(x), 40)); }
id dev_vendorId(Ctx& x, id, SEL) {
    const std::string h = hexHash("vendor:" + bundleId(x), 32);
    std::string u = h.substr(0, 8) + "-" + h.substr(8, 4) + "-4" + h.substr(13, 3) + "-8" + h.substr(17, 3) + "-" + h.substr(20, 12);
    for (char& c : u) c = char(std::toupper(uint8_t(c)));
    const id uuid = id(rt(x).send(x.cpu, rt(x).getClass("NSUUID"), rt(x).selAlloc));
    return autoreleased(x, id(rt(x).send(x.cpu, uuid, "initWithUUIDString:", {makeString(x, u)})));
}
int32_t dev_orientation(Ctx& x, id, SEL) {
    // UIDeviceOrientation: LandscapeLeft(3) = interface LandscapeRight(4) and vice versa
    switch (ui(x).statusBarOrientation) { case 3: return 4; case 4: return 3; default: return ui(x).statusBarOrientation; }
}
int32_t dev_idiom(Ctx&, id, SEL) { return 0; }
struct DevFlags : ObjC::HostObject { bool battery = false, proximity = false, orientationNotes = false; };
DevFlags& DF(Ctx& x, id d) { return rt(x).host<DevFlags>(d); }
float dev_batteryLevel(Ctx& x, id self, SEL) { return DF(x, self).battery ? 1.0f : -1.0f; }
int32_t dev_batteryState(Ctx& x, id self, SEL) { return DF(x, self).battery ? 3 : 0; }
bool dev_batteryMonitoring(Ctx& x, id self, SEL) { return DF(x, self).battery; }
void dev_setBatteryMonitoring(Ctx& x, id self, SEL, bool b) { DF(x, self).battery = b; }
bool dev_proximity(Ctx& x, id self, SEL) { return DF(x, self).proximity; }
void dev_setProximity(Ctx& x, id self, SEL, bool b) { DF(x, self).proximity = b; }
void dev_beginOrientation(Ctx& x, id self, SEL) { DF(x, self).orientationNotes = true; }
void dev_endOrientation(Ctx& x, id self, SEL) { DF(x, self).orientationNotes = false; }
bool dev_generating(Ctx& x, id self, SEL) { return DF(x, self).orientationNotes; }

// ============================================================ UIAccelerometer ===

struct AccelData : ObjC::HostObject { id delegate = 0; double interval = 0.1; std::shared_ptr<Foundation::RunLoop::Timer> timer; };
struct AccelState { explicit AccelState(Process&) {} id shared = 0; Class accel = 0, acceleration = 0; };
AccelState& ACS(Ctx& x) { return x.proc.service<AccelState>(); }
AccelData& AD(Ctx& x, id a) { return rt(x).host<AccelData>(a); }
struct AccelSample : ObjC::HostObject { double x = 0, y = 0, z = -1, t = 0; };
double as_x(Ctx& x, id self, SEL) { return rt(x).host<AccelSample>(self).x; }
double as_y(Ctx& x, id self, SEL) { return rt(x).host<AccelSample>(self).y; }
double as_z(Ctx& x, id self, SEL) { return rt(x).host<AccelSample>(self).z; }
double as_t(Ctx& x, id self, SEL) { return rt(x).host<AccelSample>(self).t; }
void startAccelTimer(Ctx& x, id acc) {
    AccelData& d = AD(x, acc);
    if (d.timer) d.timer->valid = false;
    d.timer.reset();
    if (!d.delegate) return;
    Process* p = &x.proc;
    d.timer = Foundation::mainRunLoop(x.proc).addTimer(d.interval, std::max(d.interval, 0.01), true, [p, acc](CPU::Cpu& c) {
        Ctx cx{*p, c, p->mem()};
        AccelData& ad = AD(cx, acc);
        if (!ad.delegate) return;
        auto& u = ui(cx);
        const id s = rt(cx).createInstance(ACS(cx).acceleration);
        AccelSample& smp = rt(cx).host<AccelSample>(s);
        smp.x = u.accel[0]; smp.y = u.accel[1]; smp.z = u.accel[2]; smp.t = nowSeconds();
        cx.mem.write<double>(s + 4, smp.t);
        cx.mem.write<double>(s + 12, smp.x);
        cx.mem.write<double>(s + 20, smp.y);
        cx.mem.write<double>(s + 28, smp.z);
        const uint32_t pool = rt(cx).poolPush();
        rt(cx).send(c, ad.delegate, "accelerometer:didAccelerate:", {acc, s});
        rt(cx).poolPop(c, pool);
        rt(cx).release(c, s);
    });
}
id acc_shared(Ctx& x, id, SEL) {
    auto& s = ACS(x);
    if (!s.shared) { s.shared = rt(x).createInstance(s.accel); rt(x).makeImmortal(s.shared); }
    return s.shared;
}
id acc_delegate(Ctx& x, id self, SEL) { return AD(x, self).delegate; }
void acc_setDelegate(Ctx& x, id self, SEL, id d) { AD(x, self).delegate = d; startAccelTimer(x, self); }
double acc_interval(Ctx& x, id self, SEL) { return AD(x, self).interval; }
void acc_setInterval(Ctx& x, id self, SEL, double i) { AD(x, self).interval = std::clamp(i, 0.01, 1.0); startAccelTimer(x, self); }

// ======================================================== UIApplicationMain ===

int32_t orientationFromString(const std::string& s) {
    if (s == "UIInterfaceOrientationLandscapeLeft") return 3;
    if (s == "UIInterfaceOrientationLandscapeRight") return 4;
    if (s == "UIInterfaceOrientationPortraitUpsideDown") return 2;
    return 1;
}

void configureScreen(Ctx& x) {
    auto& u = ui(x);
    const Plist::Value* info = x.proc.infoPlist();
    const std::string bundle = x.proc.bundlePath();
    std::error_code ec;
    // 4-inch support is opted into with a Default-568h@2x launch image (or a launch storyboard / UILaunchImages).
    bool tall = fs::exists(fs::path(bundle) / "Default-568h@2x.png", ec);
    if (info) {
        if (info->get("UILaunchStoryboardName") || info->get("UILaunchImages")) tall = true;
        if (const Plist::Value* li = info->get("UILaunchImageFile")) {
            const std::string base = li->s;
            if (fs::exists(fs::path(bundle) / (fs::path(base).stem().string() + "-568h@2x.png"), ec)) tall = true;
        }
    }
    u.screenW = float(x.proc.config().screenWidth);
    u.screenH = tall ? std::max(568.0f, float(x.proc.config().screenHeight)) : 480.0f;
    if (!tall && x.proc.config().screenHeight < 568) u.screenH = float(x.proc.config().screenHeight);
    u.scale = x.proc.config().screenScale;
    x.proc.setScreenSize(uint32_t(u.screenW), uint32_t(u.screenH));
    if (info) {
        if (const Plist::Value* h = info->get("UIStatusBarHidden"); h && h->type == Plist::Value::Type::Bool) u.statusBarHidden = h->b;
        std::string o = info->getString("UIInterfaceOrientation");
        if (o.empty()) {
            const Plist::Value* sup = info->get("UISupportedInterfaceOrientations~iphone");
            if (!sup) sup = info->get("UISupportedInterfaceOrientations");
            if (sup && sup->type == Plist::Value::Type::Array && !sup->array.empty()) o = sup->array.front()->s;
        }
        u.initialOrientation = orientationFromString(o);
        u.statusBarOrientation = u.initialOrientation;
    }
    x.proc.logf(LogLevel::Info, "Screen: %gx%g @%gx, %s, status bar %s", u.screenW, u.screenH, u.scale,
                u.initialOrientation >= 3 ? "landscape" : "portrait", u.statusBarHidden ? "hidden" : "visible");
}

// ---------------------------------------------------------------- lifecycle ---
// Delivered on the guest's main thread when the host asks (Process::enterBackground /
// enterForeground / requestGracefulStop): this is when games save.
struct Lifecycle { bool active = true, background = false, legacy = false; };
// Apps built against an SDK before iOS 4 (or that set UIApplicationExitsOnSuspend)
// never get didEnterBackground/willEnterForeground on iOS: only resign/become active.
bool legacyLifecycle(Process& p) {
    const Plist::Value* info = p.infoPlist();
    if (!info) return false;
    if (const Plist::Value* e = info->get("UIApplicationExitsOnSuspend"); e && e->type == Plist::Value::Type::Bool && e->b) return true;
    const std::string sdk = info->getString("DTSDKName");   // e.g. "iphoneos3.1"
    size_t i = 0;
    while (i < sdk.size() && !std::isdigit(uint8_t(sdk[i]))) ++i;
    return i < sdk.size() && std::atoi(sdk.c_str() + i) < 4;
}
void deliverLifecycle(Ctx& x, const char* delegateSel, const char* note) {
    auto& u = ui(x);
    ObjC::Runtime& r = rt(x);
    const uint32_t pool = r.poolPush();
    const id del = u.appDelegate;
    if (del && r.respondsTo(del, r.sel(delegateSel))) r.send(x.cpu, del, delegateSel, {u.app});
    Foundation::postNotification(x, note, u.app);
    r.poolPop(x.cpu, pool);
}
void persistNow(Ctx& x) {
    Foundation::flushUserDefaults(x.proc);
    flushGuestStdio(x.proc);
}
void toBackground(Ctx& x, Lifecycle& st) {
    if (st.active) { deliverLifecycle(x, "applicationWillResignActive:", "UIApplicationWillResignActiveNotification"); st.active = false; }
    if (!st.background && !st.legacy) deliverLifecycle(x, "applicationDidEnterBackground:", "UIApplicationDidEnterBackgroundNotification");
    st.background = true;
    persistNow(x);
}
void installLifecycle(Process& p) {
    auto st = std::make_shared<Lifecycle>();
    st->legacy = legacyLifecycle(p);
    Process* pp = &p;
    p.setLifecycleHandler([pp, st](Process::Lifecycle what) {
        if (what == Process::Lifecycle::Foreground) { pp->setBackgrounded(false); return; }   // wakes the parked main thread
        Foundation::mainRunLoop(*pp).post([pp, st, what](CPU::Cpu& c) {
            Ctx x{*pp, c, pp->mem()};
            if (pp->stopping()) return;
            if (what == Process::Lifecycle::Terminate) {
                toBackground(x, *st);
                pp->beginTermination();
                deliverLifecycle(x, "applicationWillTerminate:", "UIApplicationWillTerminateNotification");
                persistNow(x);
                x.proc.log(LogLevel::Info, "The app saved and closed");
                pp->requestStop();
                return;
            }
            // Background: let the app save, then pause it until the host brings it back.
            toBackground(x, *st);
            pp->setBackgrounded(true);
            const uint32_t self = pp->currentThread().id;
            std::vector<GuestThread*> paused;
            for (uint32_t id : pp->liveThreadIds()) {
                if (id == self) continue;
                if (GuestThread* t = pp->findThreadById(id)) { pp->engine().suspend(t->cpu); paused.push_back(t); }
            }
            x.proc.log(LogLevel::Info, "The app is paused in the background");
            pp->waitWhileBackgrounded();
            for (GuestThread* t : paused) pp->engine().resume(t->cpu);
            pp->setBackgrounded(false);
            if (pp->stopping() || pp->gracefulStopRequested()) return;   // a queued Terminate follows
            if (!st->legacy) deliverLifecycle(x, "applicationWillEnterForeground:", "UIApplicationWillEnterForegroundNotification");
            st->background = false;
            deliverLifecycle(x, "applicationDidBecomeActive:", "UIApplicationDidBecomeActiveNotification");
            st->active = true;
            x.proc.log(LogLevel::Info, "The app resumed");
        });
    });
}

int32_t f_UIApplicationMain(Ctx& x, int32_t, GuestPtr, id principalName, id delegateName) {
    auto& u = ui(x);
    auto& r = rt(x);
    configureScreen(x);
    const Plist::Value* info = x.proc.infoPlist();
    const uint32_t pool = r.poolPush();

    std::string principal = principalName ? utf8(x, principalName) : (info ? info->getString("NSPrincipalClass") : "");
    Class appClass = principal.empty() ? u.application : r.getClass(principal);
    if (!appClass) { x.proc.logf(LogLevel::Warning, "UIApplicationMain: principal class %s not found", principal.c_str()); appClass = u.application; }
    u.app = id(r.send(x.cpu, id(r.send(x.cpu, appClass, r.selAlloc)), r.selInit));
    r.makeImmortal(u.app);

    if (delegateName) {
        const std::string dn = utf8(x, delegateName);
        const Class dc = r.getClass(dn);
        if (!dc) throw ObjC::ObjCError("UIApplicationMain: delegate class " + dn + " not found");
        const id del = id(r.send(x.cpu, id(r.send(x.cpu, dc, r.selAlloc)), r.selInit));
        r.send(x.cpu, u.app, "setDelegate:", {del});   // UIApplicationMain keeps this one alive
    }

    // Main nib / storyboard
    std::string mainNib = info ? info->getString("NSMainNibFile~iphone", info->getString("NSMainNibFile")) : "";
    if (!mainNib.empty()) {
        x.proc.logf(LogLevel::Info, "Loading main nib %s", mainNib.c_str());
        // UIKit keeps the main nib's top-level objects (the app delegate, the window) alive for the
        // app's lifetime; they're usually assign-outlets, so nothing else retains them.
        if (const id top = loadNib(x, mainNib, 0, u.app, 0))
            for (id o : Foundation::arrayItems(x, top)) r.retain(x.cpu, o);
    }
    const std::string storyboard = info ? info->getString("UIMainStoryboardFile~iphone", info->getString("UIMainStoryboardFile")) : "";
    if (!storyboard.empty()) {
        const Class sbCls = r.getClass("UIStoryboard");
        const id sb = sbCls ? id(r.send(x.cpu, sbCls, "storyboardWithName:bundle:", {makeString(x, storyboard), 0})) : 0;
        const id initVC = sb ? id(r.send(x.cpu, sb, "instantiateInitialViewController")) : 0;
        if (initVC) {
            id win = u.keyWindow;
            if (!win && !u.windows.empty()) win = u.windows.front();
            if (!win) {
                win = id(sendRect(x, id(r.send(x.cpu, u.window, r.selAlloc)), r.sel("initWithFrame:"), {0, 0, u.screenW, u.screenH}));
                if (u.appDelegate && r.respondsTo(u.appDelegate, r.sel("setWindow:")))
                    r.send(x.cpu, u.appDelegate, "setWindow:", {win});
            }
            r.send(x.cpu, win, "setRootViewController:", {initVC});
            r.send(x.cpu, win, "makeKeyAndVisible");
        } else {
            x.proc.reportMissing("UIMainStoryboardFile (storyboards: " + storyboard + ")");
        }
    }

    const id del = u.appDelegate;
    if (!del) x.proc.log(LogLevel::Warning, "UIApplicationMain: no application delegate");
    const id launchOptions = 0;
    if (del && r.respondsTo(del, r.sel("application:willFinishLaunchingWithOptions:")))
        r.send(x.cpu, del, "application:willFinishLaunchingWithOptions:", {u.app, launchOptions});
    if (del && r.respondsTo(del, r.sel("application:didFinishLaunchingWithOptions:")))
        r.send(x.cpu, del, "application:didFinishLaunchingWithOptions:", {u.app, launchOptions});
    else if (del && r.respondsTo(del, r.sel("applicationDidFinishLaunching:")))
        r.send(x.cpu, del, "applicationDidFinishLaunching:", {u.app});
    Foundation::postNotification(x, "UIApplicationDidFinishLaunchingNotification", u.app);
    if (del && r.respondsTo(del, r.sel("applicationDidBecomeActive:"))) r.send(x.cpu, del, "applicationDidBecomeActive:", {u.app});
    Foundation::postNotification(x, "UIApplicationDidBecomeActiveNotification", u.app);
    layoutPass(x);
    r.poolPop(x.cpu, pool);

    if (u.windows.empty()) x.proc.log(LogLevel::Warning, "The app finished launching without creating a window");
    else if (!u.keyWindow) {
        x.proc.log(LogLevel::Warning, "The app finished launching without a key window; making first window key");
        r.send(x.cpu, u.windows.front(), "makeKeyAndVisible");
    }

    // Frame loop: input, display links, layout, drawing, compositing — 60 Hz on the main run loop.
    Process* p = &x.proc;
    Foundation::mainRunLoop(x.proc).addFrameHook([p](CPU::Cpu& c, double now) {
        Ctx cx{*p, c, p->mem()};
        const uint32_t fp = rt(cx).poolPush();
        runFrame(cx, now);
        rt(cx).poolPop(c, fp);
    });
    installLifecycle(x.proc);
    for (;;) Foundation::mainRunLoop(x.proc).runUntil(x.cpu, 1e18);
}

} // namespace

void notifyHostKeyboard(Ctx& x) {
    auto& st=ui(x);auto& r=rt(x);KeyboardState state;
    const id responder=st.firstResponder;
    if(responder && r.respondsTo(responder,r.sel("insertText:")) &&
       r.respondsTo(responder,r.sel("deleteBackward")) && r.respondsTo(responder,r.sel("hasText"))) {
        state.responder=responder;state.active=true;state.hasText=sendBool(x,responder,"hasText");
        state.multiline=r.isKindOf(responder,r.getClass("UITextView"));
        const auto integer=[&](const char* name) {
            return r.respondsTo(responder,r.sel(name))?int32_t(r.send(x.cpu,responder,name)):0;
        };
        state.type=integer("keyboardType");state.returnKey=integer("returnKeyType");
        state.capitalization=integer("autocapitalizationType");state.correction=integer("autocorrectionType");
        state.secure=r.respondsTo(responder,r.sel("isSecureTextEntry")) && sendBool(x,responder,"isSecureTextEntry");
    }
    {
        std::lock_guard<std::mutex> lock(st.inputMu);
        state.revision=st.hostKeyboard.revision;
        state.focusToken=st.hostKeyboard.focusToken+(state.responder!=st.hostKeyboard.responder);
        if(state==st.hostKeyboard)return;
        ++state.revision;st.hostKeyboard=state;
    }
    if(x.proc.config().keyboard)x.proc.config().keyboard(state);
}

void registerUIApplication(Process& p, ObjC::Runtime& r) {
    auto& u = ui(p);
    ObjC::ClassBuilder touch = r.defineClass("UITouch", "NSObject");
    u.touch = touch.cls();
    touch.m<&t_locationInView>("locationInView:", kP + "@:@").m<&t_prevLocationInView>("previousLocationInView:", kP + "@:@")
         .m<&t_phase>("phase", "i@:").m<&t_tapCount>("tapCount", "I@:").m<&t_timestamp>("timestamp", "d@:")
         .m<&t_view>("view").m<&t_window>("window").m<&t_recognizers>("gestureRecognizers")
         .m<&t_majorRadius>("majorRadius", "f@:").m<&t_dealloc>("dealloc", "v@:");
    ObjC::ClassBuilder event = r.defineClass("UIEvent", "NSObject");
    u.event = event.cls();
    event.m<&e_allTouches>("allTouches").m<&e_touchesForView>("touchesForView:").m<&e_touchesForWindow>("touchesForWindow:")
         .m<&e_touchesForGesture>("touchesForGestureRecognizer:").m<&e_timestamp>("timestamp", "d@:").m<&e_type>("type", "i@:")
         .m<&e_subtype>("subtype", "i@:").m<&e_dealloc>("dealloc", "v@:");
    r.builder(u.window).m<&w_sendEvent>("sendEvent:", "v@:@");

    ObjC::ClassBuilder app = r.defineClass("UIApplication", "UIResponder");
    u.application = app.cls();
    app.cm<&app_shared>("sharedApplication");
    app.m<&app_delegate>("delegate").m<&app_setDelegate>("setDelegate:", "v@:@").m<&app_keyWindow>("keyWindow").m<&app_windows>("windows")
       .m<&app_statusBarHidden>("isStatusBarHidden", "c@:").m<&app_setStatusBarHidden>("setStatusBarHidden:", "v@:c")
       .m<&app_setStatusBarHiddenAnim>("setStatusBarHidden:animated:", "v@:cc").m<&app_setStatusBarHiddenAnim>("setStatusBarHidden:withAnimation:", "v@:ci")
       .m<&app_statusBarOrientation>("statusBarOrientation", "i@:").m<&app_setStatusBarOrientation>("setStatusBarOrientation:", "v@:i")
       .m<&app_setStatusBarOrientationAnim>("setStatusBarOrientation:animated:", "v@:ic")
       .m<&app_statusBarFrame>("statusBarFrame", "{CGRect={CGPoint=ff}{CGSize=ff}}@:").m<&app_statusBarStyle>("statusBarStyle", "i@:")
       .m<&app_ignoreInt>("setStatusBarStyle:", "v@:i").m<&app_ignoreIntBool>("setStatusBarStyle:animated:", "v@:ic")
       .m<&app_state>("applicationState", "i@:").m<&app_backgroundTime>("backgroundTimeRemaining", "d@:")
       .m<&app_openURL>("openURL:", "c@:@").m<&app_canOpenURL>("canOpenURL:", "c@:@").m<&app_openURLOptions>("openURL:options:completionHandler:", "v@:@@@?")
       .m<&app_idleDisabled>("isIdleTimerDisabled", "c@:").m<&app_setIdleDisabled>("setIdleTimerDisabled:", "v@:c")
       .m<&app_network>("isNetworkActivityIndicatorVisible", "c@:").m<&app_setNetwork>("setNetworkActivityIndicatorVisible:", "v@:c")
       .m<&app_badge>("applicationIconBadgeNumber", "i@:").m<&app_setBadge>("setApplicationIconBadgeNumber:", "v@:i")
       .m<&app_beginIgnoring>("beginIgnoringInteractionEvents", "v@:").m<&app_endIgnoring>("endIgnoringInteractionEvents", "v@:")
       .m<&app_isIgnoring>("isIgnoringInteractionEvents", "c@:").m<&app_ignoreInt>("registerForRemoteNotificationTypes:", "v@:I")
       .m<&app_noop>("registerForRemoteNotifications", "v@:").m<&app_noop>("unregisterForRemoteNotifications", "v@:")
       .m<&app_noopObj>("registerUserNotificationSettings:", "v@:@").m<&app_noopObj>("scheduleLocalNotification:", "v@:@")
       .m<&app_noopObj>("presentLocalNotificationNow:", "v@:@").m<&app_noop>("cancelAllLocalNotifications", "v@:")
       .m<&app_noopObj>("cancelLocalNotification:", "v@:@").m<&app_nil>("scheduledLocalNotifications")
       .m<&app_noop>("beginReceivingRemoteControlEvents", "v@:").m<&app_noop>("endReceivingRemoteControlEvents", "v@:")
       .m<&app_beginBackgroundTask>("beginBackgroundTaskWithExpirationHandler:", "I@:@?")
       .m<&app_beginBackgroundTaskNamed>("beginBackgroundTaskWithName:expirationHandler:", "I@:@@?")
       .m<&app_endBackgroundTask>("endBackgroundTask:", "v@:I").m<&app_false>("isProtectedDataAvailable", "c@:")
       .m<&app_ignoreBool>("setApplicationSupportsShakeToEdit:", "v@:c").m<&app_true>("applicationSupportsShakeToEdit", "c@:")
       .m<&app_sendEvent>("sendEvent:", "v@:@").m<&app_sendAction>("sendAction:to:from:forEvent:", "c@::@@@")
       .m<&app_nextResponder>("nextResponder").m<&app_nil>("preferredContentSizeCategory")
       .m<&app_layoutDirection>("userInterfaceLayoutDirection", "i@:")
       .m<&app_supportedOrientations>("supportedInterfaceOrientationsForWindow:", "I@:@");
    p.def<&f_UIApplicationMain>("_UIApplicationMain");
    p.mem().write<uint32_t>(p.defineDataBlock("_UIBackgroundTaskInvalid", 4), 0);
    p.mem().write<double>(p.defineDataBlock("_UIApplicationBackgroundFetchIntervalMinimum", 8), 0.0);
    p.mem().write<double>(p.defineDataBlock("_UIApplicationBackgroundFetchIntervalNever", 8), 1e9);
    for (const char* n : {"UIApplicationDidFinishLaunchingNotification", "UIApplicationDidBecomeActiveNotification",
                          "UIApplicationWillResignActiveNotification", "UIApplicationDidEnterBackgroundNotification",
                          "UIApplicationWillEnterForegroundNotification", "UIApplicationWillTerminateNotification",
                          "UIApplicationDidReceiveMemoryWarningNotification", "UIApplicationSignificantTimeChangeNotification",
                          "UIApplicationWillChangeStatusBarOrientationNotification", "UIApplicationDidChangeStatusBarOrientationNotification",
                          "UIApplicationWillChangeStatusBarFrameNotification", "UIApplicationDidChangeStatusBarFrameNotification",
                          "UIApplicationLaunchOptionsURLKey", "UIApplicationLaunchOptionsSourceApplicationKey",
                          "UIApplicationLaunchOptionsRemoteNotificationKey", "UIApplicationLaunchOptionsLocalNotificationKey",
                          "UIApplicationStatusBarOrientationUserInfoKey", "UIApplicationOpenSettingsURLString",
                          "UIDeviceOrientationDidChangeNotification", "UIDeviceBatteryLevelDidChangeNotification",
                          "UIDeviceBatteryStateDidChangeNotification", "UIDeviceProximityStateDidChangeNotification",
                          "UIContentSizeCategoryDidChangeNotification", "UITrackingRunLoopMode"}) {
        const uint32_t cell = p.defineDataBlock(std::string("_") + n, 4);
        if (!p.mem().read<uint32_t>(cell)) p.mem().write<uint32_t>(cell, Foundation::constString(p, n));
    }

    ObjC::ClassBuilder dev = r.defineClass("UIDevice", "NSObject");
    u.device = dev.cls();
    dev.cm<&dev_current>("currentDevice");
    dev.m<&dev_systemVersion>("systemVersion").m<&dev_systemName>("systemName").m<&dev_model>("model").m<&dev_model>("localizedModel")
       .m<&dev_name>("name").m<&dev_uniqueId>("uniqueIdentifier").m<&dev_vendorId>("identifierForVendor")
       .m<&dev_orientation>("orientation", "i@:").m<&dev_idiom>("userInterfaceIdiom", "i@:")
       .m<&dev_batteryLevel>("batteryLevel", "f@:").m<&dev_batteryState>("batteryState", "i@:")
       .m<&dev_batteryMonitoring>("isBatteryMonitoringEnabled", "c@:").m<&dev_setBatteryMonitoring>("setBatteryMonitoringEnabled:", "v@:c")
       .m<&dev_proximity>("isProximityMonitoringEnabled", "c@:").m<&dev_setProximity>("setProximityMonitoringEnabled:", "v@:c")
       .m<&app_false>("proximityState", "c@:").m<&app_true>("isMultitaskingSupported", "c@:")
       .m<&dev_beginOrientation>("beginGeneratingDeviceOrientationNotifications", "v@:")
       .m<&dev_endOrientation>("endGeneratingDeviceOrientationNotifications", "v@:")
       .m<&dev_generating>("isGeneratingDeviceOrientationNotifications", "c@:").m<&app_noop>("playInputClick", "v@:");
    u.currentDevice = r.createInstance(u.device);
    r.makeImmortal(u.currentDevice);

    auto& as = p.service<AccelState>();
    ObjC::ClassBuilder acc = r.defineClass("UIAccelerometer", "NSObject");
    as.accel = acc.cls();
    acc.cm<&acc_shared>("sharedAccelerometer");
    acc.m<&acc_delegate>("delegate").m<&acc_setDelegate>("setDelegate:", "v@:@").m<&acc_interval>("updateInterval", "d@:")
       .m<&acc_setInterval>("setUpdateInterval:", "v@:d");
    ObjC::ClassBuilder sample = r.defineClass("UIAcceleration", "NSObject", 36);
    as.acceleration = sample.cls();
    sample.m<&as_x>("x", "d@:").m<&as_y>("y", "d@:").m<&as_z>("z", "d@:").m<&as_t>("timestamp", "d@:");

    // Host input → main thread.
    p.setTouchHandler([&p](TouchPhase phase, float tx, float ty, uint32_t fid) {
        auto& st = ui(p);
        {
            std::lock_guard<std::mutex> lk(st.inputMu);
            st.pendingTouches.push_back({int(phase), tx, ty, fid, nowSeconds()});
        }
        Foundation::mainRunLoop(p).post([&p](CPU::Cpu& c) {
            Ctx cx{p, c, p.mem()};
            deliverPendingInput(cx);
        });
    });
    p.setAccelerationHandler([&p](float ax, float ay, float az) {
        auto& st = ui(p);
        st.accel[0] = ax; st.accel[1] = ay; st.accel[2] = az;
    });
    p.setTextHandler([&p](TextInputKind kind,std::string text) {
        if(text.size()>16384 || p.stopping())return;
        uint32_t target;uint64_t focus;
        {
            auto& st=ui(p);std::lock_guard<std::mutex> lock(st.inputMu);
            target=st.hostKeyboard.responder;focus=st.hostKeyboard.focusToken;
        }
        if(!target)return;
        Foundation::mainRunLoop(p).post([&p,kind,target,focus,text=std::move(text)](CPU::Cpu& cpu) {
            Ctx x{p,cpu,p.mem()};auto& st=ui(x);
            {
                std::lock_guard<std::mutex> lock(st.inputMu);
                if(st.hostKeyboard.responder!=target || st.hostKeyboard.focusToken!=focus)return;
            }
            if(st.firstResponder!=target)return;
            if(kind==TextInputKind::Submit)submitTextInput(x,target);
            else if(kind==TextInputKind::DeleteBackward)rt(x).send(cpu,target,"deleteBackward");
            else rt(x).send(cpu,target,"insertText:",{Foundation::makeString(x,text)});
        });
    });
}

} // namespace Hydrogen::Runtime::UIKit
