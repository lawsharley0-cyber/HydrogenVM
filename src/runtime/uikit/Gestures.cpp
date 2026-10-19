// HydrogenVM — gesture recognizers: tap, long press, pan, swipe, pinch,
// rotation. Recognizers on the touched view and its ancestors see each touch
// before the view does; once one begins/recognizes (and cancelsTouchesInView),
// the view gets -touchesCancelled:withEvent:.
#include "UIKit.hpp"
#include <chrono>

namespace Hydrogen::Runtime::UIKit {

using Foundation::autoreleased;

namespace {

enum State : int32_t { Possible = 0, Began = 1, Changed = 2, Ended = 3, Cancelled = 4, Failed = 5 };
enum Kind { KBase, KTap, KLongPress, KPan, KSwipe, KPinch, KRotation };

struct Target { id target; SEL action; };
struct GestureData : ObjC::HostObject {
    Kind kind = KBase;
    std::vector<Target> targets;
    id view = 0, delegate = 0;
    int32_t state = Possible;
    bool enabled = true, cancelsTouches = true, delaysBegan = false, delaysEnded = true;
    std::vector<id> touches;              // tracked touches
    G::Point start, last, prev;           // window coordinates (centroid)
    double startTime = 0, lastTime = 0, prevTime = 0;
    // parameters
    int32_t tapsRequired = 1, touchesRequired = 1, minTouches = 1, maxTouches = 1000;
    uint32_t direction = 1;               // swipe: right
    double minPressDuration = 0.5;
    float allowableMovement = 10;
    std::shared_ptr<Foundation::RunLoop::Timer> timer;
    // pan
    G::Point translationOrigin;
    // pinch / rotation
    float startDist = 0, startAngle = 0, scale = 1, rotation = 0;
};
GestureData& GD(Ctx& x, id g) { return rt(x).host<GestureData>(g); }

double nowSec() { using namespace std::chrono; return duration<double>(steady_clock::now().time_since_epoch()).count(); }

G::Point touchWindowPoint(Ctx& x, id touch) {
    const uint32_t tmp = x.proc.heap().allocZeroed(8);
    const SEL s = rt(x).sel("locationInView:");
    x.proc.callGuest(x.cpu, rt(x).lookupForSend(rt(x).classOf(touch), s), {tmp, touch, s, 0});
    const CGPoint p = CG::readStruct<CGPoint>(x, tmp);
    x.proc.heap().free(tmp);
    // locationInView:nil gives screen coordinates; recognizers work in window coordinates
    const id w = id(rt(x).send(x.cpu, touch, "window"));
    if (!w) return CG::toG(p);
    return layerToSuper(layerData(x, viewLayer(x, w))).inverted().apply(CG::toG(p));
}
G::Point centroid(Ctx& x, const std::vector<id>& ts) {
    G::Point c{0, 0};
    if (ts.empty()) return c;
    for (id t : ts) { const G::Point p = touchWindowPoint(x, t); c.x += p.x; c.y += p.y; }
    return {c.x / float(ts.size()), c.y / float(ts.size())};
}

void fire(Ctx& x, id g) {
    const std::vector<Target> t = GD(x, g).targets;
    for (auto& e : t) if (e.target) rt(x).send(x.cpu, e.target, e.action, {g});
}
bool shouldBegin(Ctx& x, id g) {
    GestureData& d = GD(x, g);
    auto& r = rt(x);
    if (d.delegate && r.respondsTo(d.delegate, r.sel("gestureRecognizerShouldBegin:"))) return sendBool(x, d.delegate, "gestureRecognizerShouldBegin:", {g});
    if (d.view && r.respondsTo(d.view, r.sel("gestureRecognizerShouldBegin:"))) return sendBool(x, d.view, "gestureRecognizerShouldBegin:", {g});
    return true;
}
void setState(Ctx& x, id g, int32_t s) {
    GestureData& d = GD(x, g);
    if ((s == Began || s == Ended) && d.state == Possible && !shouldBegin(x, g)) { d.state = Failed; return; }
    d.state = s;
    if (s == Began || s == Changed || s == Ended || s == Cancelled) fire(x, g);
}
void reset(Ctx& x, id g) {
    GestureData& d = GD(x, g);
    d.state = Possible;
    d.touches.clear();
    if (d.timer) { d.timer->valid = false; d.timer.reset(); }
    d.scale = 1;
    d.rotation = 0;
}

float distOf(Ctx& x, const std::vector<id>& ts) {
    if (ts.size() < 2) return 0;
    const G::Point a = touchWindowPoint(x, ts[0]), b = touchWindowPoint(x, ts[1]);
    return std::hypot(b.x - a.x, b.y - a.y);
}
float angleOf(Ctx& x, const std::vector<id>& ts) {
    if (ts.size() < 2) return 0;
    const G::Point a = touchWindowPoint(x, ts[0]), b = touchWindowPoint(x, ts[1]);
    return std::atan2(b.y - a.y, b.x - a.x);
}

// Feed one phase of touches to a recognizer.
void feed(Ctx& x, id g, const std::vector<id>& touches, int phase) {
    GestureData& d = GD(x, g);
    if (!d.enabled || d.state == Failed || d.state == Cancelled) return;
    auto& r = rt(x);
    const double t = nowSec();
    if (phase == 0) {
        for (id tc : touches) {
            if (d.delegate && r.respondsTo(d.delegate, r.sel("gestureRecognizer:shouldReceiveTouch:")) &&
                !sendBool(x, d.delegate, "gestureRecognizer:shouldReceiveTouch:", {g, tc}))
                continue;
            if (std::find(d.touches.begin(), d.touches.end(), tc) == d.touches.end()) d.touches.push_back(tc);
        }
        if (d.touches.empty()) return;
        d.start = d.last = d.prev = centroid(x, d.touches);
        d.startTime = d.lastTime = d.prevTime = t;
        d.translationOrigin = d.start;
        if (d.touches.size() >= 2) { d.startDist = distOf(x, d.touches); d.startAngle = angleOf(x, d.touches); }
        if (d.kind == KLongPress && int32_t(d.touches.size()) >= d.touchesRequired) {
            Process* p = &x.proc;
            r.retain(x.cpu, g);
            d.timer = Foundation::mainRunLoop(x.proc).addTimer(d.minPressDuration, 0, false, [p, g](CPU::Cpu& c) {
                Ctx cx{*p, c, p->mem()};
                GestureData& gd = GD(cx, g);
                if (gd.state == Possible && !gd.touches.empty()) setState(cx, g, Began);
                rt(cx).release(c, g);
            });
        }
        return;
    }
    // only touches we track
    bool ours = false;
    for (id tc : touches) if (std::find(d.touches.begin(), d.touches.end(), tc) != d.touches.end()) ours = true;
    if (!ours) return;
    const G::Point c = centroid(x, d.touches);
    d.prev = d.last; d.prevTime = d.lastTime;
    d.last = c; d.lastTime = t;
    const float moved = std::hypot(c.x - d.start.x, c.y - d.start.y);
    const bool ending = phase == 3 || phase == 4;
    switch (d.kind) {
        case KTap:
            if (phase == 1 && moved > 10) d.state = Failed;
            if (phase == 3 && d.state == Possible) {
                const int32_t taps = int32_t(r.send(x.cpu, touches[0], "tapCount"));
                if (taps >= d.tapsRequired && int32_t(d.touches.size()) >= d.touchesRequired && t - d.startTime < 1.0) setState(x, g, Ended);
            }
            break;
        case KLongPress:
            if (d.state == Possible && moved > d.allowableMovement) d.state = Failed;
            else if (phase == 1 && (d.state == Began || d.state == Changed)) setState(x, g, Changed);
            if (ending && (d.state == Began || d.state == Changed)) setState(x, g, phase == 3 ? Ended : Cancelled);
            break;
        case KPan:
            if (phase == 1) {
                if (d.state == Possible && moved > 10 && int32_t(d.touches.size()) >= d.minTouches && int32_t(d.touches.size()) <= d.maxTouches) {
                    d.translationOrigin = d.start;
                    setState(x, g, Began);
                } else if (d.state == Began || d.state == Changed) setState(x, g, Changed);
            }
            if (ending && (d.state == Began || d.state == Changed)) setState(x, g, phase == 3 ? Ended : Cancelled);
            break;
        case KSwipe: {
            if (d.state != Possible) break;
            const float dx = c.x - d.start.x, dy = c.y - d.start.y;
            const bool fast = t - d.startTime < 0.6;
            uint32_t dir = 0;
            if (std::fabs(dx) > 50 && std::fabs(dx) > 2 * std::fabs(dy)) dir = dx > 0 ? 1 : 2;
            else if (std::fabs(dy) > 50 && std::fabs(dy) > 2 * std::fabs(dx)) dir = dy > 0 ? 8 : 4;
            if (dir && fast && (d.direction & dir) && int32_t(d.touches.size()) >= d.touchesRequired) setState(x, g, Ended);
            else if (!fast) d.state = Failed;
            break;
        }
        case KPinch: case KRotation:
            if (d.touches.size() >= 2 && phase == 1) {
                if (d.startDist <= 0) { d.startDist = distOf(x, d.touches); d.startAngle = angleOf(x, d.touches); }
                d.scale = d.startDist > 0 ? distOf(x, d.touches) / d.startDist : 1;
                d.rotation = angleOf(x, d.touches) - d.startAngle;
                const bool started = d.kind == KPinch ? std::fabs(d.scale - 1) > 0.05f : std::fabs(d.rotation) > 0.1f;
                if (d.state == Possible && started) setState(x, g, Began);
                else if (d.state == Began || d.state == Changed) setState(x, g, Changed);
            }
            if (ending && (d.state == Began || d.state == Changed)) setState(x, g, phase == 3 ? Ended : Cancelled);
            break;
        default: break;
    }
    if (ending) {
        for (id tc : touches) d.touches.erase(std::remove(d.touches.begin(), d.touches.end(), tc), d.touches.end());
        if (d.touches.empty()) {
            if (d.kind == KTap || d.kind == KSwipe || d.kind == KLongPress) { /* Failed or Ended already set */ }
        }
    }
}

// ------------------------------------------------------------ the class ---

id g_initWithTarget(Ctx& x, id self, SEL, id target, SEL action) {
    GestureData& d = GD(x, self);
    const std::string cls = rt(x).objectClassName(self);
    auto isA = [&](const char* n) { const Class c = rt(x).getClass(n); return c && rt(x).isKindOf(self, c); };
    if (isA("UITapGestureRecognizer")) d.kind = KTap;
    else if (isA("UILongPressGestureRecognizer")) d.kind = KLongPress;
    else if (isA("UISwipeGestureRecognizer")) d.kind = KSwipe;
    else if (isA("UIPinchGestureRecognizer")) d.kind = KPinch;
    else if (isA("UIRotationGestureRecognizer")) d.kind = KRotation;
    else if (isA("UIPanGestureRecognizer")) d.kind = KPan;
    if (target && action) d.targets.push_back({target, action});
    return self;
}
id g_init(Ctx& x, id self, SEL s) { return g_initWithTarget(x, self, s, 0, 0); }
void g_addTarget(Ctx& x, id self, SEL, id t, SEL a) { GD(x, self).targets.push_back({t, a}); }
void g_removeTarget(Ctx& x, id self, SEL, id t, SEL a) {
    auto& v = GD(x, self).targets;
    v.erase(std::remove_if(v.begin(), v.end(), [&](const Target& e) { return (!t || e.target == t) && (!a || e.action == a); }), v.end());
}
int32_t g_state(Ctx& x, id self, SEL) { return GD(x, self).state; }
void g_setState(Ctx& x, id self, SEL, int32_t s) { setState(x, self, s); }
id g_view(Ctx& x, id self, SEL) { return GD(x, self).view; }
void g_setView(Ctx& x, id self, SEL, id v) { GD(x, self).view = v; }
bool g_enabled(Ctx& x, id self, SEL) { return GD(x, self).enabled; }
void g_setEnabled(Ctx& x, id self, SEL, bool b) { GD(x, self).enabled = b; if (!b) reset(x, self); }
bool g_cancels(Ctx& x, id self, SEL) { return GD(x, self).cancelsTouches; }
void g_setCancels(Ctx& x, id self, SEL, bool b) { GD(x, self).cancelsTouches = b; }
bool g_delaysBegan(Ctx& x, id self, SEL) { return GD(x, self).delaysBegan; }
void g_setDelaysBegan(Ctx& x, id self, SEL, bool b) { GD(x, self).delaysBegan = b; }
void g_setDelaysEnded(Ctx& x, id self, SEL, bool b) { GD(x, self).delaysEnded = b; }
id g_delegate(Ctx& x, id self, SEL) { return GD(x, self).delegate; }
void g_setDelegate(Ctx& x, id self, SEL, id d) { GD(x, self).delegate = d; }
void g_require(Ctx&, id, SEL, id) {}
uint32_t g_numberOfTouches(Ctx& x, id self, SEL) { return uint32_t(GD(x, self).touches.size()); }
void g_locationInView(Ctx& x, GuestPtr ret, id self, SEL, id view) {
    GestureData& d = GD(x, self);
    G::Point p = d.touches.empty() ? d.last : centroid(x, d.touches);
    const id w = d.view ? viewWindow(x, d.view) : 0;
    if (view) p = convertPoint(x, p, w, view);
    CG::writeStruct(x, ret, CG::fromG(p));
}
void g_locationOfTouch(Ctx& x, GuestPtr ret, id self, SEL, uint32_t i, id view) {
    GestureData& d = GD(x, self);
    G::Point p = i < d.touches.size() ? touchWindowPoint(x, d.touches[i]) : d.last;
    const id w = d.view ? viewWindow(x, d.view) : 0;
    if (view) p = convertPoint(x, p, w, view);
    CG::writeStruct(x, ret, CG::fromG(p));
}
void g_reset(Ctx& x, id self, SEL) { reset(x, self); }
void g_dealloc(Ctx& x, id self, SEL) {
    if (GestureData* d = rt(x).hostIf<GestureData>(self)) if (d->timer) d->timer->valid = false;
    rt(x).disposeInstance(x.cpu, self);
}
// touches* overrides on custom recognizer subclasses are not called (we drive recognition ourselves).
void g_ignoreTouches(Ctx&, id, SEL, id, id) {}

// tap
uint32_t tap_taps(Ctx& x, id self, SEL) { return uint32_t(GD(x, self).tapsRequired); }
void tap_setTaps(Ctx& x, id self, SEL, uint32_t n) { GD(x, self).tapsRequired = int32_t(std::max(1u, n)); }
uint32_t g_touchesReq(Ctx& x, id self, SEL) { return uint32_t(GD(x, self).touchesRequired); }
void g_setTouchesReq(Ctx& x, id self, SEL, uint32_t n) { GD(x, self).touchesRequired = int32_t(std::max(1u, n)); }
// long press
double lp_duration(Ctx& x, id self, SEL) { return GD(x, self).minPressDuration; }
void lp_setDuration(Ctx& x, id self, SEL, double d) { GD(x, self).minPressDuration = d; }
float lp_movement(Ctx& x, id self, SEL) { return GD(x, self).allowableMovement; }
void lp_setMovement(Ctx& x, id self, SEL, float m) { GD(x, self).allowableMovement = m; }
// pan
G::Point toViewVector(Ctx& x, id self, G::Point v, id view) {
    const id w = GD(x, self).view ? viewWindow(x, GD(x, self).view) : 0;
    if (!view) return v;
    const G::Point a = convertPoint(x, {0, 0}, w, view), b = convertPoint(x, v, w, view);
    return {b.x - a.x, b.y - a.y};
}
void pan_translation(Ctx& x, GuestPtr ret, id self, SEL, id view) {
    GestureData& d = GD(x, self);
    CG::writeStruct(x, ret, CG::fromG(toViewVector(x, self, {d.last.x - d.translationOrigin.x, d.last.y - d.translationOrigin.y}, view)));
}
void pan_setTranslation(Ctx& x, id self, SEL, CGPoint t, id view) {
    GestureData& d = GD(x, self);
    const G::Point tw = toViewVector(x, self, {1, 1}, view);   // view → window scale (approximate inverse)
    const float sx = tw.x != 0 ? 1 / tw.x : 1, sy = tw.y != 0 ? 1 / tw.y : 1;
    d.translationOrigin = {d.last.x - t.x * sx, d.last.y - t.y * sy};
}
void pan_velocity(Ctx& x, GuestPtr ret, id self, SEL, id view) {
    GestureData& d = GD(x, self);
    const double dt = std::max(d.lastTime - d.prevTime, 1e-3);
    CG::writeStruct(x, ret, CG::fromG(toViewVector(x, self, {float((d.last.x - d.prev.x) / dt), float((d.last.y - d.prev.y) / dt)}, view)));
}
uint32_t pan_min(Ctx& x, id self, SEL) { return uint32_t(GD(x, self).minTouches); }
void pan_setMin(Ctx& x, id self, SEL, uint32_t n) { GD(x, self).minTouches = int32_t(n); }
uint32_t pan_max(Ctx& x, id self, SEL) { return uint32_t(GD(x, self).maxTouches); }
void pan_setMax(Ctx& x, id self, SEL, uint32_t n) { GD(x, self).maxTouches = int32_t(n); }
// swipe
uint32_t sw_direction(Ctx& x, id self, SEL) { return GD(x, self).direction; }
void sw_setDirection(Ctx& x, id self, SEL, uint32_t d) { GD(x, self).direction = d; }
// pinch / rotation
float pinch_scale(Ctx& x, id self, SEL) { return GD(x, self).scale; }
void pinch_setScale(Ctx& x, id self, SEL, float s) {
    GestureData& d = GD(x, self);
    d.scale = s;
    d.startDist = s > 0 ? distOf(x, d.touches) / s : d.startDist;
}
float pinch_velocity(Ctx&, id, SEL) { return 0; }
float rot_rotation(Ctx& x, id self, SEL) { return GD(x, self).rotation; }
void rot_setRotation(Ctx& x, id self, SEL, float r) { GestureData& d = GD(x, self); d.rotation = r; d.startAngle = angleOf(x, d.touches) - r; }

} // namespace

bool gestureFilterTouches(Ctx& x, id view, const std::vector<id>& touches, int phase, id event) {
    (void)event;
    // recognizers on the view and its ancestors
    std::vector<id> recognizers;
    for (id v = view; v; v = viewData(x, v).superview)
        for (id g : viewData(x, v).gestureRecognizers) recognizers.push_back(g);
    if (recognizers.empty()) return false;
    auto& r = rt(x);
    for (id g : recognizers) r.retain(x.cpu, g);
    bool cancel = false;
    for (id g : recognizers) {
        feed(x, g, touches, phase);
        GestureData& d = GD(x, g);
        if ((d.state == Began || d.state == Changed || d.state == Ended) && d.cancelsTouches) cancel = true;
    }
    // reset recognizers whose sequence ended
    for (id g : recognizers) {
        GestureData& d = GD(x, g);
        if (d.touches.empty() && (phase == 3 || phase == 4)) reset(x, g);
        else if (d.state == Ended && d.kind != KPan && d.kind != KLongPress && d.kind != KPinch && d.kind != KRotation) reset(x, g);
    }
    for (id g : recognizers) r.release(x.cpu, g);
    return cancel;
}

void registerGestures(Process& p, ObjC::Runtime& r) {
    auto& u = ui(p);
    const std::string P = "{CGPoint=ff}";
    ObjC::ClassBuilder g = r.defineClass("UIGestureRecognizer", "NSObject");
    u.gesture = g.cls();
    g.m<&g_initWithTarget>("initWithTarget:action:").m<&g_init>("init").m<&g_addTarget>("addTarget:action:", "v@:@:")
     .m<&g_removeTarget>("removeTarget:action:", "v@:@:").m<&g_state>("state", "i@:").m<&g_setState>("setState:", "v@:i")
     .m<&g_view>("view").m<&g_setView>("_hvmSetView:", "v@:@").m<&g_enabled>("isEnabled", "c@:").m<&g_setEnabled>("setEnabled:", "v@:c")
     .m<&g_cancels>("cancelsTouchesInView", "c@:").m<&g_setCancels>("setCancelsTouchesInView:", "v@:c")
     .m<&g_delaysBegan>("delaysTouchesBegan", "c@:").m<&g_setDelaysBegan>("setDelaysTouchesBegan:", "v@:c")
     .m<&g_setDelaysEnded>("setDelaysTouchesEnded:", "v@:c").m<&g_delegate>("delegate").m<&g_setDelegate>("setDelegate:", "v@:@")
     .m<&g_require>("requireGestureRecognizerToFail:", "v@:@").m<&g_numberOfTouches>("numberOfTouches", "I@:")
     .m<&g_locationInView>("locationInView:", P + "@:@").m<&g_locationOfTouch>("locationOfTouch:inView:", P + "@:I@")
     .m<&g_reset>("reset", "v@:").m<&g_dealloc>("dealloc", "v@:")
     .m<&g_ignoreTouches>("touchesBegan:withEvent:", "v@:@@").m<&g_ignoreTouches>("touchesMoved:withEvent:", "v@:@@")
     .m<&g_ignoreTouches>("touchesEnded:withEvent:", "v@:@@").m<&g_ignoreTouches>("touchesCancelled:withEvent:", "v@:@@");
    r.defineClass("UITapGestureRecognizer", "UIGestureRecognizer")
        .m<&tap_taps>("numberOfTapsRequired", "I@:").m<&tap_setTaps>("setNumberOfTapsRequired:", "v@:I")
        .m<&g_touchesReq>("numberOfTouchesRequired", "I@:").m<&g_setTouchesReq>("setNumberOfTouchesRequired:", "v@:I");
    r.defineClass("UILongPressGestureRecognizer", "UIGestureRecognizer")
        .m<&lp_duration>("minimumPressDuration", "d@:").m<&lp_setDuration>("setMinimumPressDuration:", "v@:d")
        .m<&lp_movement>("allowableMovement", "f@:").m<&lp_setMovement>("setAllowableMovement:", "v@:f")
        .m<&g_touchesReq>("numberOfTouchesRequired", "I@:").m<&g_setTouchesReq>("setNumberOfTouchesRequired:", "v@:I")
        .m<&tap_taps>("numberOfTapsRequired", "I@:").m<&tap_setTaps>("setNumberOfTapsRequired:", "v@:I");
    r.defineClass("UIPanGestureRecognizer", "UIGestureRecognizer")
        .m<&pan_translation>("translationInView:", P + "@:@").m<&pan_setTranslation>("setTranslation:inView:", "v@:" + P + "@")
        .m<&pan_velocity>("velocityInView:", P + "@:@").m<&pan_min>("minimumNumberOfTouches", "I@:")
        .m<&pan_setMin>("setMinimumNumberOfTouches:", "v@:I").m<&pan_max>("maximumNumberOfTouches", "I@:")
        .m<&pan_setMax>("setMaximumNumberOfTouches:", "v@:I");
    r.defineClass("UISwipeGestureRecognizer", "UIGestureRecognizer")
        .m<&sw_direction>("direction", "I@:").m<&sw_setDirection>("setDirection:", "v@:I")
        .m<&g_touchesReq>("numberOfTouchesRequired", "I@:").m<&g_setTouchesReq>("setNumberOfTouchesRequired:", "v@:I");
    r.defineClass("UIPinchGestureRecognizer", "UIGestureRecognizer")
        .m<&pinch_scale>("scale", "f@:").m<&pinch_setScale>("setScale:", "v@:f").m<&pinch_velocity>("velocity", "f@:");
    r.defineClass("UIRotationGestureRecognizer", "UIGestureRecognizer")
        .m<&rot_rotation>("rotation", "f@:").m<&rot_setRotation>("setRotation:", "v@:f").m<&pinch_velocity>("velocity", "f@:");
}

} // namespace Hydrogen::Runtime::UIKit
