// HydrogenVM — UIResponder, UIView, UIWindow, UIScreen, UIViewController.
#include "UIKit.hpp"

namespace Hydrogen::Runtime::UIKit {

using Foundation::autoreleased;
using Foundation::makeString;
using Foundation::utf8;

namespace {

constexpr float kPi = 3.14159265358979f;
const std::string kR = "{CGRect={CGPoint=ff}{CGSize=ff}}", kP = "{CGPoint=ff}", kS = "{CGSize=ff}", kT = "{CGAffineTransform=ffffff}";

ViewData& V(Ctx& x, id v) { return viewData(x, v); }
VCData& VC(Ctx& x, id vc) { return rt(x).host<VCData>(vc); }
bool isView(Ctx& x, id o) { return o && rt(x).isKindOf(o, ui(x).view); }
bool isWindowObj(Ctx& x, id o) { return o && rt(x).isKindOf(o, ui(x).window); }
void sendRectArg(Ctx& x, id self, const char* sel, const G::Rect& r) { sendRect(x, self, rt(x).sel(sel), r); }

// ============================================================= UIResponder ===

id r_nextResponder(Ctx&, id, SEL) { return 0; }
bool r_false(Ctx&, id, SEL) { return false; }
bool r_true(Ctx&, id, SEL) { return true; }
bool r_isFirst(Ctx& x, id self, SEL) { return ui(x).firstResponder == self; }
bool r_become(Ctx& x, id self, SEL) {
    if (!sendBool(x, self, "canBecomeFirstResponder")) return false;
    auto& u = ui(x);
    if (u.firstResponder && u.firstResponder != self &&
        !sendBool(x, u.firstResponder, "resignFirstResponder")) return false;
    u.firstResponder = self;
    notifyHostKeyboard(x);
    return true;
}
bool r_resign(Ctx& x, id self, SEL) {
    if (ui(x).firstResponder == self) { ui(x).firstResponder=0;notifyHostKeyboard(x); }
    return true;
}
void forwardTouches(Ctx& x, id self, SEL s, id touches, id event) {
    const id next = id(rt(x).send(x.cpu, self, "nextResponder"));
    if (next) rt(x).send(x.cpu, next, s, {touches, event});
}
void forwardMotion(Ctx& x, id self, SEL s, int32_t motion, id event) {
    const id next = id(rt(x).send(x.cpu, self, "nextResponder"));
    if (next) rt(x).send(x.cpu, next, s, {uint32_t(motion), event});
}
void r_remoteControl(Ctx&, id, SEL, id) {}
id r_nil(Ctx&, id, SEL) { return 0; }

// ================================================================== UIView ===

uint32_t gravityForMode(int32_t mode) { return mode <= 2 ? uint32_t(mode) : mode == 3 ? 0u : uint32_t(mode - 1); }

id vc_layerClass(Ctx& x, id, SEL) { return ui(x).layer; }

void ensureLayer(Ctx& x, id self) {
    ViewData& d = V(x, self);
    if (d.layer) return;
    const Class lc = Class(rt(x).send(x.cpu, rt(x).classOf(self), "layerClass"));
    d.layer = newLayer(x, lc ? lc : ui(x).layer);
    LayerData& l = layerData(x, d.layer);
    l.delegate = self;
    l.contentsScale = ui(x).scale;
    // Views backed by a CAEAGLLayer start at scale 1.0, as on iOS: a GL app
    // opts into Retina by setting contentScaleFactor before creating its
    // renderbuffer; otherwise it draws at 320x480 and is scaled up.
    const Class eagl = rt(x).getClass("CAEAGLLayer");
    for (Class c = lc; eagl && c; c = rt(x).superclass(c))
        if (c == eagl) { l.contentsScale = 1; break; }
    l.needsDisplay = true;
}

id v_init(Ctx& x, id self, SEL) { return id(sendRect(x, self, rt(x).sel("initWithFrame:"), {0, 0, 0, 0})); }
id v_initWithFrame(Ctx& x, id self, SEL, CGRect f) {
    ensureLayer(x, self);
    layerSetFrame(x, V(x, self).layer, CG::toG(f));
    return self;
}
void v_dealloc(Ctx& x, id self, SEL) {
    if (ViewData* d = rt(x).hostIf<ViewData>(self)) {
        auto subs = std::move(d->subviews);
        d->subviews.clear();
        for (id s : subs) { if (ViewData* sd = rt(x).hostIf<ViewData>(s)) sd->superview = 0; rt(x).release(x.cpu, s); }
        auto grs = std::move(d->gestureRecognizers);
        d->gestureRecognizers.clear();
        for (id g : grs) rt(x).release(x.cpu, g);
        const id layer = d->layer, bg = d->backgroundColor, root = d->rootViewController;
        d->layer = d->backgroundColor = d->rootViewController = 0;
        if (layer) { layerData(x, layer).delegate = 0; layerRemoveFromSuperlayer(x, layer); rt(x).release(x.cpu, layer); }
        if (bg) rt(x).release(x.cpu, bg);
        if (root) rt(x).release(x.cpu, root);
        auto& u = ui(x);
        u.windows.erase(std::remove(u.windows.begin(), u.windows.end(), self), u.windows.end());
        if (u.keyWindow == self) u.keyWindow = 0;
        if (u.firstResponder == self) { u.firstResponder=0;notifyHostKeyboard(x); }
        for (auto& t : u.activeTouches) if (t.view == self) t.view = 0;
    }
    setNeedsComposite(x);
    rt(x).disposeInstance(x.cpu, self);
}
id v_layer(Ctx& x, id self, SEL) { return viewLayer(x, self); }
// -[UIView recursiveDescription] (UIKit's debugging aid): one line per view, indented.
void describeTree(Ctx& x, id v, int depth, std::string& out) {
    if (!v || depth > 64) return;
    const ViewData& d = V(x, v);
    const LayerData& l = layerData(x, viewLayer(x, v));
    char line[320];
    std::snprintf(line, sizeof line, "%*s<%s: 0x%x; frame = (%g %g; %g %g)%s%s; alpha = %g; bg = (%.2f %.2f %.2f %.2f)%s>\n",
                  depth * 3, "", rt(x).objectClassName(v).c_str(), v, l.position.x - l.bounds.w / 2, l.position.y - l.bounds.h / 2,
                  l.bounds.w, l.bounds.h, l.hidden ? "; hidden" : "", l.opaque ? "; opaque" : "", l.opacity,
                  l.background.r, l.background.g, l.background.b, l.background.a, l.contents ? "; contents" : "");
    out += line;
    for (id sub : d.subviews) describeTree(x, sub, depth + 1, out);
}
id v_recursiveDescription(Ctx& x, id self, SEL) {
    std::string out;
    describeTree(x, self, 0, out);
    return makeString(x, out);
}

// ---- geometry
void autoresize(Ctx& x, id view, G::Size oldSuper, G::Size newSuper) {
    ViewData& d = V(x, view);
    const uint32_t m = d.autoresizingMask;
    if (!m) return;
    G::Rect f = layerFrame(layerData(x, viewLayer(x, view)));
    auto axis = [](float& pos, float& size, float oldTotal, float newTotal, bool flexLo, bool flexSize, bool flexHi) {
        const float delta = newTotal - oldTotal;
        if (delta == 0 || !(flexLo || flexSize || flexHi)) return;
        const float lo = pos, hi = oldTotal - pos - size;
        float sum = (flexLo ? lo : 0) + (flexSize ? size : 0) + (flexHi ? hi : 0);
        const int n = int(flexLo) + int(flexSize) + int(flexHi);
        auto share = [&](float v) { return sum > 0 ? delta * v / sum : delta / float(n); };
        if (flexLo) pos += share(lo);
        if (flexSize) size += share(size);
    };
    axis(f.x, f.w, oldSuper.w, newSuper.w, m & 1, m & 2, m & 4);
    axis(f.y, f.h, oldSuper.h, newSuper.h, m & 8, m & 16, m & 32);
    sendRectArg(x, view, "setFrame:", f);
}
void boundsSizeChanged(Ctx& x, id self, G::Size oldSize, G::Size newSize) {
    if (oldSize.w == newSize.w && oldSize.h == newSize.h) return;
    ViewData& d = V(x, self);
    d.needsLayout = true;
    if (d.autoresizesSubviews) for (id s : std::vector<id>(d.subviews)) autoresize(x, s, oldSize, newSize);
    if (d.contentMode == 3) layerData(x, viewLayer(x, self)).needsDisplay = true;
    else if (newSize.w > 0 && newSize.h > 0 && !layerData(x, viewLayer(x, self)).backing) layerData(x, viewLayer(x, self)).needsDisplay = true;
}
void v_frame(Ctx& x, GuestPtr ret, id self, SEL) { CG::writeStruct(x, ret, CG::fromG(layerFrame(layerData(x, viewLayer(x, self))))); }
void v_setFrame(Ctx& x, id self, SEL, CGRect f) {
    LayerData& l = layerData(x, viewLayer(x, self));
    const G::Size old{l.bounds.w, l.bounds.h};
    layerSetFrame(x, viewLayer(x, self), CG::toG(f));
    boundsSizeChanged(x, self, old, {l.bounds.w, l.bounds.h});
}
void v_bounds(Ctx& x, GuestPtr ret, id self, SEL) { CG::writeStruct(x, ret, CG::fromG(layerData(x, viewLayer(x, self)).bounds)); }
void v_setBounds(Ctx& x, id self, SEL, CGRect b) {
    LayerData& l = layerData(x, viewLayer(x, self));
    const G::Size old{l.bounds.w, l.bounds.h};
    l.bounds = CG::toG(b).standardized();
    setNeedsComposite(x);
    boundsSizeChanged(x, self, old, {l.bounds.w, l.bounds.h});
}
void v_center(Ctx& x, GuestPtr ret, id self, SEL) {
    const LayerData& l = layerData(x, viewLayer(x, self));
    // center is the position when the anchor is (0.5, 0.5); UIKit keeps it that way
    CG::writeStruct(x, ret, CG::fromG(l.position));
}
void v_setCenter(Ctx& x, id self, SEL, CGPoint c) { layerData(x, viewLayer(x, self)).position = CG::toG(c); setNeedsComposite(x); }
void v_transform(Ctx& x, GuestPtr ret, id self, SEL) { CG::writeStruct(x, ret, CG::fromG(affineOf(layerData(x, viewLayer(x, self)).transform))); }
void v_setTransform(Ctx& x, id self, SEL, CGAffineTransform t) { layerData(x, viewLayer(x, self)).transform = from2D(CG::toG(t)); setNeedsComposite(x); }

// ---- appearance
id v_backgroundColor(Ctx& x, id self, SEL) { return V(x, self).backgroundColor; }
void v_setBackgroundColor(Ctx& x, id self, SEL, id c) {
    ViewData& d = V(x, self);
    if (c) rt(x).retain(x.cpu, c);
    const id old = d.backgroundColor;
    d.backgroundColor = c;
    if (old) rt(x).release(x.cpu, old);
    LayerData& l = layerData(x, viewLayer(x, self));
    l.background = c ? uiColor(x, c) : G::Color{};
    l.backgroundPattern = c ? uiColorPattern(x, c) : nullptr;
    if (l.backgroundPattern) { l.background = {}; }
    setNeedsComposite(x);
}
float v_alpha(Ctx& x, id self, SEL) { return layerData(x, viewLayer(x, self)).opacity; }
void v_setAlpha(Ctx& x, id self, SEL, float a) { layerData(x, viewLayer(x, self)).opacity = std::clamp(a, 0.0f, 1.0f); setNeedsComposite(x); }
bool v_hidden(Ctx& x, id self, SEL) { return layerData(x, viewLayer(x, self)).hidden; }
void v_setHidden(Ctx& x, id self, SEL, bool h) { layerData(x, viewLayer(x, self)).hidden = h; setNeedsComposite(x); }
bool v_opaque(Ctx& x, id self, SEL) { return layerData(x, viewLayer(x, self)).opaque; }
void v_setOpaque(Ctx& x, id self, SEL, bool o) { layerData(x, viewLayer(x, self)).opaque = o; }
bool v_clips(Ctx& x, id self, SEL) { return layerData(x, viewLayer(x, self)).masksToBounds; }
void v_setClips(Ctx& x, id self, SEL, bool c) { layerData(x, viewLayer(x, self)).masksToBounds = c; setNeedsComposite(x); }
int32_t v_contentMode(Ctx& x, id self, SEL) { return V(x, self).contentMode; }
void v_setContentMode(Ctx& x, id self, SEL, int32_t m) { V(x, self).contentMode = m; layerData(x, viewLayer(x, self)).gravity = int(gravityForMode(m)); setNeedsComposite(x); }
float v_contentScale(Ctx& x, id self, SEL) { return layerData(x, viewLayer(x, self)).contentsScale; }
void v_setContentScale(Ctx& x, id self, SEL, float s) { LayerData& l = layerData(x, viewLayer(x, self)); l.contentsScale = s > 0 ? s : 1; l.needsDisplay = true; }
int32_t v_tag(Ctx& x, id self, SEL) { return V(x, self).tag; }
void v_setTag(Ctx& x, id self, SEL, int32_t t) { V(x, self).tag = t; }
bool v_userInteraction(Ctx& x, id self, SEL) { return V(x, self).userInteraction; }
void v_setUserInteraction(Ctx& x, id self, SEL, bool b) { V(x, self).userInteraction = b; }
bool v_multiTouch(Ctx& x, id self, SEL) { return V(x, self).multipleTouch; }
void v_setMultiTouch(Ctx& x, id self, SEL, bool b) { V(x, self).multipleTouch = b; }
bool v_exclusiveTouch(Ctx& x, id self, SEL) { return V(x, self).exclusiveTouch; }
void v_setExclusiveTouch(Ctx& x, id self, SEL, bool b) { V(x, self).exclusiveTouch = b; }
uint32_t v_autoresizingMask(Ctx& x, id self, SEL) { return V(x, self).autoresizingMask; }
void v_setAutoresizingMask(Ctx& x, id self, SEL, uint32_t m) { V(x, self).autoresizingMask = m; }
bool v_autoresizesSubviews(Ctx& x, id self, SEL) { return V(x, self).autoresizesSubviews; }
void v_setAutoresizesSubviews(Ctx& x, id self, SEL, bool b) { V(x, self).autoresizesSubviews = b; }
bool v_clearsContext(Ctx& x, id self, SEL) { return V(x, self).clearsContextBeforeDrawing; }
void v_setClearsContext(Ctx& x, id self, SEL, bool b) { V(x, self).clearsContextBeforeDrawing = b; }
id v_tintColor(Ctx& x, id, SEL) { return makeUIColor(x, {0, 0.478f, 1, 1}); }
void v_ignoreObj(Ctx&, id, SEL, id) {}
void v_ignoreBool(Ctx&, id, SEL, bool) {}

// ---- hierarchy
id v_superview(Ctx& x, id self, SEL) { return V(x, self).superview; }
id v_subviews(Ctx& x, id self, SEL) { return Foundation::makeArray(x, V(x, self).subviews); }
id v_window(Ctx& x, id self, SEL) { return viewWindow(x, self); }

void syncLayerOrder(Ctx& x, id self) {
    // Keep subview layers in subview order, preserving non-view sublayers' relative positions at the front.
    ViewData& d = V(x, self);
    LayerData& l = layerData(x, viewLayer(x, self));
    std::vector<id> viewLayers;
    for (id s : d.subviews) viewLayers.push_back(viewLayer(x, s));
    std::vector<id> others;
    for (id sl : l.sublayers) if (std::find(viewLayers.begin(), viewLayers.end(), sl) == viewLayers.end()) others.push_back(sl);
    std::vector<id> merged;
    // non-view layers stay where they were relative to the view layers that preceded them
    size_t vi = 0;
    for (id sl : l.sublayers) {
        if (std::find(others.begin(), others.end(), sl) != others.end()) merged.push_back(sl);
        else if (vi < viewLayers.size()) merged.push_back(viewLayers[vi++]);
    }
    while (vi < viewLayers.size()) merged.push_back(viewLayers[vi++]);
    l.sublayers = merged;
    setNeedsComposite(x);
}

void notifyMoveToWindow(Ctx& x, id view, id newWindow, bool will) {
    rt(x).send(x.cpu, view, will ? "willMoveToWindow:" : "didMoveToWindow", will ? std::initializer_list<uint32_t>{newWindow} : std::initializer_list<uint32_t>{});
    for (id s : std::vector<id>(V(x, view).subviews)) notifyMoveToWindow(x, s, newWindow, will);
}
void vcAppearance(Ctx& x, id view, bool appearing) {
    const id vc = V(x, view).viewController;
    if (vc) {
        VCData& c = VC(x, vc);
        if (appearing && !c.appeared) {
            c.appeared = true;
            rt(x).send(x.cpu, vc, "viewWillAppear:", {0});
            rt(x).send(x.cpu, vc, "viewDidAppear:", {0});
        } else if (!appearing && c.appeared) {
            c.appeared = false;
            rt(x).send(x.cpu, vc, "viewWillDisappear:", {0});
            rt(x).send(x.cpu, vc, "viewDidDisappear:", {0});
        }
    }
    for (id s : std::vector<id>(V(x, view).subviews)) vcAppearance(x, s, appearing);
}

void insertSubview(Ctx& x, id self, id view, long index) {
    if (!view || view == self) return;
    auto& r = rt(x);
    r.retain(x.cpu, view);
    const id oldSuper = V(x, view).superview;
    const id oldWindow = viewWindow(x, view);
    const id newWindow = isWindowObj(x, self) ? self : viewWindow(x, self);
    if (oldSuper == self) {
        // reordering within the same superview: no move notifications
        auto& v = V(x, self).subviews;
        auto it = std::find(v.begin(), v.end(), view);
        if (it != v.end()) { v.erase(it); r.release(x.cpu, view); }
        if (index < 0 || size_t(index) > v.size()) v.push_back(view); else v.insert(v.begin() + index, view);
        layerAddSublayer(x, viewLayer(x, self), viewLayer(x, view));
        syncLayerOrder(x, self);
        return;
    }
    r.send(x.cpu, view, "willMoveToSuperview:", {self});
    if (oldWindow != newWindow) notifyMoveToWindow(x, view, newWindow, true);
    if (oldSuper) {
        r.send(x.cpu, oldSuper, "willRemoveSubview:", {view});
        auto& v = V(x, oldSuper).subviews;
        auto it = std::find(v.begin(), v.end(), view);
        if (it != v.end()) { v.erase(it); r.release(x.cpu, view); }
    }
    auto& v = V(x, self).subviews;
    if (index < 0 || size_t(index) > v.size()) v.push_back(view); else v.insert(v.begin() + index, view);
    V(x, view).superview = self;
    layerAddSublayer(x, viewLayer(x, self), viewLayer(x, view));
    syncLayerOrder(x, self);
    r.send(x.cpu, view, "didMoveToSuperview");
    r.send(x.cpu, self, "didAddSubview:", {view});
    if (oldWindow != newWindow) {
        notifyMoveToWindow(x, view, newWindow, false);
        if (newWindow && V(x, newWindow).visible) vcAppearance(x, view, true);
        else if (oldWindow) vcAppearance(x, view, false);
    }
    V(x, self).needsLayout = true;
}
void v_addSubview(Ctx& x, id self, SEL, id view) { insertSubview(x, self, view, -1); }
void v_insertAt(Ctx& x, id self, SEL, id view, int32_t i) { insertSubview(x, self, view, i); }
long indexOf(Ctx& x, id self, id sub) {
    auto& v = V(x, self).subviews;
    auto it = std::find(v.begin(), v.end(), sub);
    return it == v.end() ? -1 : long(it - v.begin());
}
void v_insertAbove(Ctx& x, id self, SEL, id view, id sib) {
    long i = indexOf(x, self, sib);
    if (V(x, view).superview == self && indexOf(x, self, view) < i) --i;
    insertSubview(x, self, view, i < 0 ? -1 : i + 1);
}
void v_insertBelow(Ctx& x, id self, SEL, id view, id sib) {
    long i = indexOf(x, self, sib);
    if (V(x, view).superview == self && indexOf(x, self, view) < i) --i;
    insertSubview(x, self, view, i < 0 ? 0 : i);
}
void v_bringToFront(Ctx& x, id self, SEL, id view) { if (V(x, view).superview == self) insertSubview(x, self, view, -1); }
void v_sendToBack(Ctx& x, id self, SEL, id view) { if (V(x, view).superview == self) insertSubview(x, self, view, 0); }
void v_exchange(Ctx& x, id self, SEL, int32_t a, int32_t b) {
    auto& v = V(x, self).subviews;
    if (a < 0 || b < 0 || size_t(a) >= v.size() || size_t(b) >= v.size()) return;
    std::swap(v[size_t(a)], v[size_t(b)]);
    syncLayerOrder(x, self);
}
void v_removeFromSuperview(Ctx& x, id self, SEL) {
    const id sup = V(x, self).superview;
    if (!sup) return;
    auto& r = rt(x);
    r.retain(x.cpu, self);
    const id oldWindow = viewWindow(x, self);
    r.send(x.cpu, self, "willMoveToSuperview:", {0});
    if (oldWindow) { vcAppearance(x, self, false); notifyMoveToWindow(x, self, 0, true); }
    r.send(x.cpu, sup, "willRemoveSubview:", {self});
    auto& v = V(x, sup).subviews;
    auto it = std::find(v.begin(), v.end(), self);
    if (it != v.end()) { v.erase(it); r.release(x.cpu, self); }
    V(x, self).superview = 0;
    layerRemoveFromSuperlayer(x, viewLayer(x, self));
    r.send(x.cpu, self, "didMoveToSuperview");
    if (oldWindow) notifyMoveToWindow(x, self, 0, false);
    // first responder inside the removed subtree resigns
    auto& u = ui(x);
    for (id fr = u.firstResponder; fr && isView(x, fr); fr = V(x, fr).superview)
        if (fr == self) { u.firstResponder=0;notifyHostKeyboard(x);break; }
    autoreleased(x, self);
}
void v_hookObj(Ctx&, id, SEL, id) {}
void v_hook(Ctx&, id, SEL) {}
bool v_isDescendant(Ctx& x, id self, SEL, id other) {
    for (id v = self; v; v = V(x, v).superview) if (v == other) return true;
    return false;
}
id findTag(Ctx& x, id view, int32_t tag) {
    if (V(x, view).tag == tag) return view;
    for (id s : V(x, view).subviews) if (const id f = findTag(x, s, tag)) return f;
    return 0;
}
id v_viewWithTag(Ctx& x, id self, SEL, int32_t tag) { return findTag(x, self, tag); }
id v_nextResponder(Ctx& x, id self, SEL) {
    ViewData& d = V(x, self);
    if (d.viewController) return d.viewController;
    if (d.isWindow) return ui(x).app;
    return d.superview;
}

// ---- layout & drawing
void v_setNeedsLayout(Ctx& x, id self, SEL) { V(x, self).needsLayout = true; }
void v_layoutIfNeeded(Ctx& x, id self, SEL) { layoutIfNeeded(x, self); }
void v_layoutSubviews(Ctx& x, id self, SEL) {
    const id vc = V(x, self).viewController;
    if (vc) {
        rt(x).send(x.cpu, vc, "viewWillLayoutSubviews");
        rt(x).send(x.cpu, vc, "viewDidLayoutSubviews");
    }
}
void v_setNeedsDisplay(Ctx& x, id self, SEL) { layerData(x, viewLayer(x, self)).needsDisplay = true; setNeedsComposite(x); }
void v_setNeedsDisplayInRect(Ctx& x, id self, SEL, CGRect) { layerData(x, viewLayer(x, self)).needsDisplay = true; setNeedsComposite(x); }
void v_drawRect(Ctx&, id, SEL, CGRect) {}
void v_drawLayerInContext(Ctx& x, id self, SEL, id layer, id ctx) {
    CG::pushContext(x, ctx);
    const G::Rect b = layerData(x, layer).bounds;
    try { sendRect(x, self, rt(x).sel("drawRect:"), b); } catch (...) { CG::popContext(x); throw; }
    CG::popContext(x);
}
void v_sizeThatFits(Ctx& x, GuestPtr ret, id self, SEL, CGSize) {
    const G::Rect b = layerData(x, viewLayer(x, self)).bounds;
    CG::writeStruct(x, ret, CGSize{b.w, b.h});
}
void v_sizeToFit(Ctx& x, id self, SEL) {
    const G::Rect b = layerData(x, viewLayer(x, self)).bounds;
    const G::Size s = getSize(x, self, rt(x).sel("sizeThatFits:"), {fbits(b.w), fbits(b.h)});
    G::Rect f = layerFrame(layerData(x, viewLayer(x, self)));
    f.w = s.w; f.h = s.h;
    sendRectArg(x, self, "setFrame:", f);
}

// ---- hit testing & conversion
bool v_pointInside(Ctx& x, id self, SEL, CGPoint p, id) { return layerData(x, viewLayer(x, self)).bounds.contains(CG::toG(p)); }
id v_hitTest(Ctx& x, id self, SEL, CGPoint p, id event) {
    ViewData& d = V(x, self);
    const LayerData& l = layerData(x, d.layer ? d.layer : viewLayer(x, self));
    if (l.hidden || !d.userInteraction || l.opacity < 0.01f) return 0;
    if (!sendBool(x, self, "pointInside:withEvent:", {fbits(p.x), fbits(p.y), event})) return 0;
    const std::vector<id> subs = d.subviews;
    for (auto it = subs.rbegin(); it != subs.rend(); ++it) {
        const LayerData& sl = layerData(x, viewLayer(x, *it));
        const G::Point sp = layerToSuper(sl).inverted().apply(CG::toG(p));
        const id h = id(rt(x).send(x.cpu, *it, "hitTest:withEvent:", {fbits(sp.x), fbits(sp.y), event}));
        if (h) return h;
    }
    return self;
}
void v_convertPointTo(Ctx& x, GuestPtr ret, id self, SEL, CGPoint p, id to) { CG::writeStruct(x, ret, CG::fromG(convertPoint(x, CG::toG(p), self, to))); }
void v_convertPointFrom(Ctx& x, GuestPtr ret, id self, SEL, CGPoint p, id from) { CG::writeStruct(x, ret, CG::fromG(convertPoint(x, CG::toG(p), from, self))); }
G::Affine viewToScreen(Ctx& x, id v) {
    if (!v) return {};
    const id w = isWindowObj(x, v) ? v : viewWindow(x, v);
    G::Affine t = viewToWindow(x, v);
    if (w) t = t.then(layerToSuper(layerData(x, viewLayer(x, w))));
    return t;
}
void v_convertRectTo(Ctx& x, GuestPtr ret, id self, SEL, CGRect r, id to) {
    const G::Affine a = viewToScreen(x, self);
    const G::Affine b = to ? viewToScreen(x, to) : (viewWindow(x, self) ? viewToScreen(x, viewWindow(x, self)) : G::Affine{});
    CG::writeStruct(x, ret, CG::fromG(a.then(b.inverted()).applyRect(CG::toG(r))));
}
void v_convertRectFrom(Ctx& x, GuestPtr ret, id self, SEL, CGRect r, id from) {
    const G::Affine a = from ? viewToScreen(x, from) : (viewWindow(x, self) ? viewToScreen(x, viewWindow(x, self)) : G::Affine{});
    const G::Affine b = viewToScreen(x, self);
    CG::writeStruct(x, ret, CG::fromG(a.then(b.inverted()).applyRect(CG::toG(r))));
}
bool v_endEditing(Ctx& x, id, SEL, bool) { if (ui(x).firstResponder) rt(x).send(x.cpu, ui(x).firstResponder, "resignFirstResponder"); return true; }

// ---- gesture recognizers (state lives in Gestures.cpp; the view only keeps the list)
id v_gestures(Ctx& x, id self, SEL) { auto& g = V(x, self).gestureRecognizers; return g.empty() ? 0 : Foundation::makeArray(x, g); }
void v_addGesture(Ctx& x, id self, SEL, id g) {
    if (!g) return;
    auto& v = V(x, self).gestureRecognizers;
    if (std::find(v.begin(), v.end(), g) != v.end()) return;
    v.push_back(rt(x).retain(x.cpu, g));
    rt(x).send(x.cpu, g, "_hvmSetView:", {self});
}
void v_removeGesture(Ctx& x, id self, SEL, id g) {
    auto& v = V(x, self).gestureRecognizers;
    auto it = std::find(v.begin(), v.end(), g);
    if (it == v.end()) return;
    v.erase(it);
    rt(x).send(x.cpu, g, "_hvmSetView:", {0});
    rt(x).release(x.cpu, g);
}

// ---- animations: applied immediately, completions fire after the duration.
struct AnimState {
    explicit AnimState(Process&) {}
    struct Block { double duration = 0.2, delay = 0; id delegate = 0; SEL didStop = 0, willStart = 0; id animID = 0; uint32_t context = 0; };
    std::vector<Block> stack;
    bool enabled = true;
};
AnimState& AS(Ctx& x) { return x.proc.service<AnimState>(); }
void scheduleCompletion(Ctx& x, double after, uint32_t block) {
    if (!block) return;
    const uint32_t b = ObjC::blockCopy(x, block);
    Process* p = &x.proc;
    Foundation::mainRunLoop(x.proc).addTimer(std::max(0.0, after), 0, false, [p, b](CPU::Cpu& c) {
        Ctx cx{*p, c, p->mem()};
        ObjC::callBlock(cx, b, {1});   // finished = YES
        ObjC::blockRelease(cx, b);
    });
}
void runAnimations(Ctx& x, uint32_t animations) { if (animations) ObjC::callBlock(x, animations); setNeedsComposite(x); }
void vc_animate(Ctx& x, id, SEL, double d, uint32_t anim) { runAnimations(x, anim); }
void vc_animateCompletion(Ctx& x, id, SEL, double d, uint32_t anim, uint32_t comp) { runAnimations(x, anim); scheduleCompletion(x, d, comp); }
void vc_animateFull(Ctx& x, id, SEL, double d, double delay, uint32_t, uint32_t anim, uint32_t comp) {
    if (delay > 0 && anim) {
        // keep the ordering apps expect: changes land after the delay
        const uint32_t a = ObjC::blockCopy(x, anim), c = comp ? ObjC::blockCopy(x, comp) : 0;
        Process* p = &x.proc;
        Foundation::mainRunLoop(x.proc).addTimer(delay, 0, false, [p, a, c, d](CPU::Cpu& cpu) {
            Ctx cx{*p, cpu, p->mem()};
            runAnimations(cx, a);
            ObjC::blockRelease(cx, a);
            if (c) { scheduleCompletion(cx, d, c); ObjC::blockRelease(cx, c); }
        });
        return;
    }
    runAnimations(x, anim);
    scheduleCompletion(x, d + delay, comp);
}
void vc_animateSpring(Ctx& x, id s, SEL c, double d, double delay, float, float, uint32_t opts, uint32_t anim, uint32_t comp) {
    vc_animateFull(x, s, c, d, delay, opts, anim, comp);
}
void vc_transition(Ctx& x, id, SEL, id, double d, uint32_t, uint32_t anim, uint32_t comp) { runAnimations(x, anim); scheduleCompletion(x, d, comp); }
void vc_transitionFromTo(Ctx& x, id, SEL, id from, id to, double d, uint32_t opts, uint32_t comp) {
    // UIViewAnimationOptionShowHideTransitionViews (1<<8): hide/show instead of remove/add
    if (opts & (1u << 8)) { rt(x).send(x.cpu, from, "setHidden:", {1}); rt(x).send(x.cpu, to, "setHidden:", {0}); }
    else if (from && to) {
        const id sup = V(x, from).superview;
        if (sup) { insertSubview(x, sup, to, indexOf(x, sup, from)); rt(x).send(x.cpu, from, "removeFromSuperview"); }
    }
    scheduleCompletion(x, d, comp);
}
void vc_begin(Ctx& x, id, SEL, id animID, uint32_t ctx) {
    AnimState::Block b;
    b.animID = animID ? rt(x).retain(x.cpu, animID) : 0;
    b.context = ctx;
    AS(x).stack.push_back(b);
}
void vc_commit(Ctx& x, id, SEL) {
    auto& st = AS(x).stack;
    if (st.empty()) return;
    AnimState::Block b = st.back();
    st.pop_back();
    setNeedsComposite(x);
    auto& r = rt(x);
    if (b.delegate && b.willStart) r.send(x.cpu, b.delegate, b.willStart, {b.animID, b.context});
    if (b.delegate && b.didStop) {
        r.retain(x.cpu, b.delegate);
        Process* p = &x.proc;
        Foundation::mainRunLoop(x.proc).addTimer(b.duration + b.delay, 0, false, [p, b](CPU::Cpu& c) {
            Ctx cx{*p, c, p->mem()};
            rt(cx).send(c, b.delegate, b.didStop, {b.animID, Foundation::makeBool(cx, true), b.context});
            rt(cx).release(c, b.delegate);
            if (b.animID) rt(cx).release(c, b.animID);
        });
    } else if (b.animID) r.release(x.cpu, b.animID);
}
AnimState::Block* top(Ctx& x) { auto& st = AS(x).stack; return st.empty() ? nullptr : &st.back(); }
void vc_setDuration(Ctx& x, id, SEL, double d) { if (auto* b = top(x)) b->duration = d; }
void vc_setDelay(Ctx& x, id, SEL, double d) { if (auto* b = top(x)) b->delay = d; }
void vc_setDelegate(Ctx& x, id, SEL, id d) { if (auto* b = top(x)) b->delegate = d; }
void vc_setDidStop(Ctx& x, id, SEL, SEL s) { if (auto* b = top(x)) b->didStop = s; }
void vc_setWillStart(Ctx& x, id, SEL, SEL s) { if (auto* b = top(x)) b->willStart = s; }
void vc_ignoreInt(Ctx&, id, SEL, int32_t) {}
void vc_ignoreFloat(Ctx&, id, SEL, float) {}
void vc_ignoreBool(Ctx&, id, SEL, bool) {}
void vc_setTransition(Ctx&, id, SEL, int32_t, id, bool) {}
void vc_setEnabled(Ctx& x, id, SEL, bool e) { AS(x).enabled = e; }
bool vc_enabled(Ctx& x, id, SEL) { return AS(x).enabled; }
void vc_withoutAnimation(Ctx& x, id, SEL, uint32_t block) { if (block) ObjC::callBlock(x, block); }

// ================================================================ UIWindow ===

id w_initWithFrame(Ctx& x, id self, SEL s, CGRect f) {
    v_initWithFrame(x, self, s, f);
    V(x, self).isWindow = true;
    auto& u = ui(x);
    if (std::find(u.windows.begin(), u.windows.end(), self) == u.windows.end()) u.windows.push_back(self);
    return self;
}
void setVisible(Ctx& x, id self, bool vis) {
    ViewData& d = V(x, self);
    if (d.visible == vis) return;
    d.visible = vis;
    auto& u = ui(x);
    if (vis) {
        if (std::find(u.windows.begin(), u.windows.end(), self) == u.windows.end()) u.windows.push_back(self);
        rt(x).retain(x.cpu, self);   // visible windows are kept alive by the app
        layerData(x, viewLayer(x, self)).hidden = false;
        vcAppearance(x, self, true);
        if (d.isWindow) Foundation::postNotification(x, "UIWindowDidBecomeVisibleNotification", self);
    } else {
        vcAppearance(x, self, false);
        if (d.isWindow) Foundation::postNotification(x, "UIWindowDidBecomeHiddenNotification", self);
        rt(x).release(x.cpu, self);
    }
    setNeedsComposite(x);
}
void w_makeKey(Ctx& x, id self, SEL) {
    auto& u = ui(x);
    if (u.keyWindow == self) return;
    const id old = u.keyWindow;
    if (old) { V(x, old).isKey = false; rt(x).send(x.cpu, old, "resignKeyWindow"); }
    u.keyWindow = self;
    V(x, self).isKey = true;
    rt(x).send(x.cpu, self, "becomeKeyWindow");
    Foundation::postNotification(x, "UIWindowDidBecomeKeyNotification", self);
}
void w_makeKeyAndVisible(Ctx& x, id self, SEL s) {
    setVisible(x, self, true);
    w_makeKey(x, self, s);
}
void w_setHidden(Ctx& x, id self, SEL s, bool h) { v_setHidden(x, self, s, h); setVisible(x, self, !h); }
bool w_isKey(Ctx& x, id self, SEL) { return V(x, self).isKey; }
float w_level(Ctx& x, id self, SEL) { return V(x, self).windowLevel; }
void w_setLevel(Ctx& x, id self, SEL, float l) { V(x, self).windowLevel = l; setNeedsComposite(x); }
id w_screen(Ctx& x, id, SEL) { return ui(x).mainScreen; }
id w_root(Ctx& x, id self, SEL) { return V(x, self).rootViewController; }

G::Rect applicationFrame(Ctx& x) {
    auto& u = ui(x);
    if (u.statusBarHidden) return {0, 0, u.screenW, u.screenH};
    switch (u.statusBarOrientation) {
        case 3: return {20, 0, u.screenW - 20, u.screenH};
        case 4: return {0, 0, u.screenW - 20, u.screenH};
        case 2: return {0, 0, u.screenW, u.screenH - 20};
        default: return {0, 20, u.screenW, u.screenH - 20};
    }
}
// Lay out a root view controller's view for the current interface orientation (iOS 6 model:
// the window stays portrait; the root view is rotated).
void layoutRootView(Ctx& x, id window, id vc) {
    auto& u = ui(x);
    const id view = id(rt(x).send(x.cpu, vc, "view"));
    if (!view) return;
    const bool full = VC(x, vc).wantsFullScreen;
    G::Rect af = full ? G::Rect{0, 0, u.screenW, u.screenH} : applicationFrame(x);
    LayerData& l = layerData(x, viewLayer(x, view));
    const int o = u.statusBarOrientation;
    const G::Size old{l.bounds.w, l.bounds.h};
    if (o == 3 || o == 4) {
        l.transform = from2D(G::Affine::rotate(o == 4 ? kPi / 2 : -kPi / 2));
        l.bounds = {0, 0, af.h, af.w};
        l.position = {af.x + af.w / 2, af.y + af.h / 2};
    } else if (o == 2) {
        l.transform = from2D(G::Affine::rotate(kPi));
        l.bounds = {0, 0, af.w, af.h};
        l.position = {af.x + af.w / 2, af.y + af.h / 2};
    } else {
        l.transform = identity3D();
        l.bounds = {0, 0, af.w, af.h};
        l.position = {af.x + af.w / 2, af.y + af.h / 2};
    }
    boundsSizeChanged(x, view, old, {l.bounds.w, l.bounds.h});
    (void)window;
    setNeedsComposite(x);
}
void w_setRoot(Ctx& x, id self, SEL, id vc) {
    ViewData& d = V(x, self);
    if (d.rootViewController == vc) return;
    const id old = d.rootViewController;
    if (old) {
        const id oldView = VC(x, old).view;
        if (oldView && V(x, oldView).superview == self) rt(x).send(x.cpu, oldView, "removeFromSuperview");
    }
    d.rootViewController = vc ? rt(x).retain(x.cpu, vc) : 0;
    if (old) rt(x).release(x.cpu, old);
    if (!vc) return;
    // Choose the interface orientation this controller supports (prefer the app's initial one).
    auto& u = ui(x);
    const int want = u.initialOrientation;
    auto supports = [&](int o) {
        auto& r = rt(x);
        if (r.respondsTo(vc, r.sel("shouldAutorotateToInterfaceOrientation:")) &&
            r.lookup(r.classOf(vc), r.sel("shouldAutorotateToInterfaceOrientation:")) != r.lookup(u.viewController, r.sel("shouldAutorotateToInterfaceOrientation:")))
            return sendBool(x, vc, "shouldAutorotateToInterfaceOrientation:", {uint32_t(o)});
        const uint32_t mask = uint32_t(r.send(x.cpu, vc, "supportedInterfaceOrientations"));
        return (mask & (1u << o)) != 0;
    };
    if (supports(want)) u.statusBarOrientation = want;
    else for (int o : {1, 3, 4, 2}) if (supports(o)) { u.statusBarOrientation = o; break; }
    VC(x, vc).modalStyle = 0;
    const id view = id(rt(x).send(x.cpu, vc, "view"));
    layoutRootView(x, self, vc);
    if (view) insertSubview(x, self, view, -1);
}
void w_becomeKey(Ctx& x, id self, SEL sel) {
    if (sel == rt(x).sel("becomeKeyWindow")) Foundation::postNotification(x, "UIWindowDidBecomeKeyNotification", self);
    else Foundation::postNotification(x, "UIWindowDidResignKeyNotification", self);
}
id w_keyWindowClass(Ctx& x, id, SEL) { return ui(x).keyWindow; }
void w_convertPointToWindow(Ctx& x, GuestPtr ret, id self, SEL, CGPoint p, id to) {
    const G::Point s = layerToSuper(layerData(x, viewLayer(x, self))).apply(CG::toG(p));
    const G::Point r = to ? layerToSuper(layerData(x, viewLayer(x, to))).inverted().apply(s) : s;
    CG::writeStruct(x, ret, CG::fromG(r));
}
void w_convertPointFromWindow(Ctx& x, GuestPtr ret, id self, SEL, CGPoint p, id from) {
    const G::Point s = from ? layerToSuper(layerData(x, viewLayer(x, from))).apply(CG::toG(p)) : CG::toG(p);
    CG::writeStruct(x, ret, CG::fromG(layerToSuper(layerData(x, viewLayer(x, self))).inverted().apply(s)));
}

// ================================================================ UIScreen ===

id sc_main(Ctx& x, id, SEL) { return ui(x).mainScreen; }
id sc_screens(Ctx& x, id, SEL) { return Foundation::makeArray(x, {ui(x).mainScreen}); }
void sc_bounds(Ctx& x, GuestPtr ret, id, SEL) { CG::writeStruct(x, ret, CGRect{{0, 0}, {ui(x).screenW, ui(x).screenH}}); }
void sc_nativeBounds(Ctx& x, GuestPtr ret, id, SEL) { auto& u = ui(x); CG::writeStruct(x, ret, CGRect{{0, 0}, {u.screenW * u.scale, u.screenH * u.scale}}); }
void sc_appFrame(Ctx& x, GuestPtr ret, id, SEL) { CG::writeStruct(x, ret, CG::fromG(applicationFrame(x))); }
float sc_scale(Ctx& x, id, SEL) { return ui(x).scale; }
float sc_brightness(Ctx&, id, SEL) { return 0.8f; }
void sc_setBrightness(Ctx&, id, SEL, float) {}
void sc_setMode(Ctx&, id, SEL, id) {}
// UIAccessibility informal protocol (an NSObject category in UIKit): nothing reads it here.
void ax_set(Ctx&, id, SEL, id) {}
id ax_nil(Ctx&, id, SEL) { return 0; }
void ax_noop(Ctx&, id, SEL) {}
struct ModeData : ObjC::HostObject {};
id sc_currentMode(Ctx& x, id, SEL) { return autoreleased(x, rt(x).createInstance(rt(x).getClass("UIScreenMode"))); }
// -[UIScreen displayLinkWithTarget:selector:] forwards to CADisplayLink.
id sc_displayLink(Ctx& x, id, SEL, id target, SEL sel) {
    return id(rt(x).send(x.cpu, ui(x).displayLink, "displayLinkWithTarget:selector:", {target, sel}));
}
id sc_modes(Ctx& x, id s, SEL c) { return Foundation::makeArray(x, {sc_currentMode(x, s, c)}); }
void sm_size(Ctx& x, GuestPtr ret, id, SEL) { auto& u = ui(x); CG::writeStruct(x, ret, CGSize{u.screenW * u.scale, u.screenH * u.scale}); }
float sm_aspect(Ctx&, id, SEL) { return 1; }
id sc_nil(Ctx&, id, SEL) { return 0; }
int32_t sc_overscan(Ctx&, id, SEL) { return 0; }
float sc_zeroFloat(Ctx&, id, SEL) { return 0.0f; }

// ======================================================= UIViewController ===

id vc_init(Ctx& x, id self, SEL) { return id(rt(x).send(x.cpu, self, "initWithNibName:bundle:", {0, 0})); }
id vc_initWithNib(Ctx& x, id self, SEL, id nib, id bundle) {
    VCData& d = VC(x, self);
    if (nib) d.nibName = id(rt(x).send(x.cpu, nib, "copy"));
    if (bundle) d.nibBundle = rt(x).retain(x.cpu, bundle);
    return self;
}
id vc_initWithCoder(Ctx& x, id self, SEL, id coder) {
    auto& r = rt(x);
    VCData& d = VC(x, self);
    const id nib = id(r.send(x.cpu, coder, "decodeObjectForKey:", {makeString(x, "UINibName")}));
    if (nib) d.nibName = r.retain(x.cpu, nib);
    const id title = id(r.send(x.cpu, coder, "decodeObjectForKey:", {makeString(x, "UITitle")}));
    if (title) d.title = r.retain(x.cpu, title);
    d.wantsFullScreen = (uint32_t(r.send(x.cpu, coder, "decodeBoolForKey:", {makeString(x, "UIWantsFullScreenLayout")})) & 0xFF) != 0;
    const id view = id(r.send(x.cpu, coder, "decodeObjectForKey:", {makeString(x, "UIView")}));
    if (view) r.send(x.cpu, self, "setView:", {view});
    return self;
}
void vc_dealloc(Ctx& x, id self, SEL) {
    if (VCData* d = rt(x).hostIf<VCData>(self)) {
        if (d->view && rt(x).hostIf<ViewData>(d->view)) V(x, d->view).viewController = 0;
        auto children = std::move(d->children);
        d->children.clear();
        for (id c : children) rt(x).release(x.cpu, c);
        for (id o : {d->view, d->nibName, d->nibBundle, d->title, d->presented, d->navigationItem, d->tabBarItem}) if (o) rt(x).release(x.cpu, o);
        d->view = d->nibName = d->nibBundle = d->title = d->presented = d->navigationItem = d->tabBarItem = 0;
    }
    rt(x).disposeInstance(x.cpu, self);
}
std::string nibForClass(Ctx& x, id self) {
    std::string cls = rt(x).objectClassName(self);
    const std::string suffix = "Controller";
    if (cls.size() > suffix.size() && cls.compare(cls.size() - suffix.size(), suffix.size(), suffix) == 0) {
        const std::string trimmed = cls.substr(0, cls.size() - suffix.size());
        if (nibExists(x, trimmed)) return trimmed;
    }
    return nibExists(x, cls) ? cls : std::string();
}
void vc_loadView(Ctx& x, id self, SEL) {
    VCData& d = VC(x, self);
    std::string nib = d.nibName ? utf8(x, d.nibName) : nibForClass(x, self);
    if (!nib.empty()) {
        loadNib(x, nib, d.nibBundle, self, 0);
        if (VC(x, self).view) return;
        x.proc.logf(LogLevel::Warning, "-[%s loadView] loaded the \"%s\" nib but the view outlet was not set", rt(x).objectClassName(self).c_str(), nib.c_str());
    }
    const G::Rect af = applicationFrame(x);
    const id view = id(sendRect(x, id(rt(x).send(x.cpu, ui(x).view, rt(x).selAlloc)), rt(x).sel("initWithFrame:"), af));
    rt(x).send(x.cpu, view, "setAutoresizingMask:", {2 | 16});
    rt(x).send(x.cpu, self, "setView:", {view});
    rt(x).release(x.cpu, view);
}
id vc_view(Ctx& x, id self, SEL) {
    if (!VC(x, self).view) {
        rt(x).send(x.cpu, self, "loadView");
        if (VC(x, self).view) rt(x).send(x.cpu, self, "viewDidLoad");
    }
    return VC(x, self).view;
}
void vc_setView(Ctx& x, id self, SEL, id view) {
    VCData& d = VC(x, self);
    if (d.view == view) return;
    if (view) rt(x).retain(x.cpu, view);
    const id old = d.view;
    d.view = view;
    if (old) { V(x, old).viewController = 0; rt(x).release(x.cpu, old); }
    if (view) V(x, view).viewController = self;
}
bool vc_isViewLoaded(Ctx& x, id self, SEL) { return VC(x, self).view != 0; }
void vc_hook(Ctx&, id, SEL) {}
void vc_hookBool(Ctx&, id, SEL, bool) {}
id vc_title(Ctx& x, id self, SEL) { return VC(x, self).title; }
void vc_setTitle(Ctx& x, id self, SEL, id t) { VCData& d = VC(x, self); const id o = d.title; d.title = t ? id(rt(x).send(x.cpu, t, "copy")) : 0; if (o) rt(x).release(x.cpu, o); }
id vc_nibName(Ctx& x, id self, SEL) { return VC(x, self).nibName; }
id vc_nibBundle(Ctx& x, id self, SEL) { return VC(x, self).nibBundle; }
id vc_parent(Ctx& x, id self, SEL) { return VC(x, self).parent; }
id vc_presented(Ctx& x, id self, SEL) { return VC(x, self).presented; }
id vc_presenting(Ctx& x, id self, SEL) { return VC(x, self).presenting; }
bool vc_shouldAutorotate(Ctx&, id, SEL, int32_t o) { return o == 1; }
bool vc_yes(Ctx&, id, SEL) { return true; }
uint32_t vc_supported(Ctx&, id, SEL) { return (1u << 1) | (1u << 3) | (1u << 4); }   // all but upside down
int32_t vc_preferredOrientation(Ctx& x, id, SEL) { return ui(x).statusBarOrientation; }
int32_t vc_interfaceOrientation(Ctx& x, id, SEL) { return ui(x).statusBarOrientation; }
bool vc_wantsFull(Ctx& x, id self, SEL) { return VC(x, self).wantsFullScreen; }
void vc_setWantsFull(Ctx& x, id self, SEL, bool b) { VC(x, self).wantsFullScreen = b; }
id vc_childVCs(Ctx& x, id self, SEL) { return Foundation::makeArray(x, VC(x, self).children); }
void vc_addChild(Ctx& x, id self, SEL, id child) {
    if (!child) return;
    rt(x).send(x.cpu, child, "willMoveToParentViewController:", {self});
    VC(x, self).children.push_back(rt(x).retain(x.cpu, child));
    VC(x, child).parent = self;
}
void vc_removeFromParent(Ctx& x, id self, SEL) {
    const id p = VC(x, self).parent;
    if (!p) return;
    auto& v = VC(x, p).children;
    auto it = std::find(v.begin(), v.end(), self);
    VC(x, self).parent = 0;
    rt(x).send(x.cpu, self, "didMoveToParentViewController:", {0});
    if (it != v.end()) { v.erase(it); rt(x).release(x.cpu, self); }
}
void vc_hookObj(Ctx&, id, SEL, id) {}
void vc_present(Ctx& x, id self, SEL, id vc, bool, uint32_t completion) {
    if (!vc) return;
    VCData& d = VC(x, self);
    if (d.presented) { x.proc.log(LogLevel::Warning, "presentViewController: already presenting"); return; }
    d.presented = rt(x).retain(x.cpu, vc);
    VC(x, vc).presenting = self;
    // Show it full screen in the presenter's window.
    id window = 0;
    if (d.view) window = viewWindow(x, d.view);
    if (!window) window = ui(x).keyWindow;
    const id view = id(rt(x).send(x.cpu, vc, "view"));
    if (window && view) {
        layoutRootView(x, window, vc);
        insertSubview(x, window, view, -1);
    }
    if (completion) {
        const uint32_t b = ObjC::blockCopy(x, completion);
        Process* p = &x.proc;
        Foundation::mainRunLoop(x.proc).post([p, b](CPU::Cpu& c) { Ctx cx{*p, c, p->mem()}; ObjC::callBlock(cx, b); ObjC::blockRelease(cx, b); });
    }
}
void vc_presentModal(Ctx& x, id self, SEL s, id vc, bool animated) { vc_present(x, self, s, vc, animated, 0); }
void vc_dismiss(Ctx& x, id self, SEL, bool, uint32_t completion) {
    // Dismissing on the presented controller dismisses itself (forwarded to its presenter).
    id presenter = self;
    if (!VC(x, self).presented && VC(x, self).presenting) presenter = VC(x, self).presenting;
    VCData& d = VC(x, presenter);
    const id vc = d.presented;
    if (vc) {
        d.presented = 0;
        VC(x, vc).presenting = 0;
        if (const id view = VC(x, vc).view) rt(x).send(x.cpu, view, "removeFromSuperview");
        autoreleased(x, vc);
    }
    if (completion) {
        const uint32_t b = ObjC::blockCopy(x, completion);
        Process* p = &x.proc;
        Foundation::mainRunLoop(x.proc).post([p, b](CPU::Cpu& c) { Ctx cx{*p, c, p->mem()}; ObjC::callBlock(cx, b); ObjC::blockRelease(cx, b); });
    }
}
void vc_dismissModal(Ctx& x, id self, SEL s, bool animated) { vc_dismiss(x, self, s, animated, 0); }
id vc_nextResponder(Ctx& x, id self, SEL) {
    const id view = VC(x, self).view;
    if (view && V(x, view).superview) return V(x, view).superview;
    if (VC(x, self).parent) return VC(x, self).parent;
    return 0;
}
id vc_ancestor(Ctx& x, id self, const char* clsName) {
    const Class cls = rt(x).getClass(clsName);
    if (!cls) return 0;
    for (id p = VC(x, self).parent; p; p = VC(x, p).parent) if (rt(x).isKindOf(p, cls)) return p;
    return 0;
}
id vc_navigationController(Ctx& x, id self, SEL) { return vc_ancestor(x, self, "UINavigationController"); }
id vc_tabBarController(Ctx& x, id self, SEL) { return vc_ancestor(x, self, "UITabBarController"); }
id vc_navigationItem(Ctx& x, id self, SEL) {
    VCData& d = VC(x, self);
    if (!d.navigationItem) {
        const Class cls = rt(x).getClass("UINavigationItem");
        if (cls) d.navigationItem = id(rt(x).send(x.cpu, id(rt(x).send(x.cpu, cls, rt(x).selAlloc)), "initWithTitle:", {d.title}));
    }
    return d.navigationItem;
}
bool vc_editing(Ctx&, id, SEL) { return false; }
void vc_setEditing(Ctx&, id, SEL, bool, bool) {}
int32_t vc_zero(Ctx&, id, SEL) { return 0; }
void vc_setInt(Ctx& x, id self, SEL, int32_t v) { VC(x, self).modalStyle = v; }
void vc_contentSize(Ctx& x, GuestPtr ret, id self, SEL) { CG::writeStruct(x, ret, CGSize{320, 460}); }

} // namespace

// ================================================================ exports ===

ViewData& viewData(Ctx& x, id view) { return rt(x).host<ViewData>(view); }
id viewLayer(Ctx& x, id view) {
    ViewData& d = viewData(x, view);
    if (!d.layer) ensureLayer(x, view);
    return viewData(x, view).layer;
}
// Whether a layer is part of what the screen shows: its chain of superlayers ends
// at a visible window's layer, and nothing on the way is hidden or transparent.
bool layerOnScreen(Ctx& x, id layer) {
    id root = 0;
    for (id l = layer; l; l = layerData(x, l).superlayer) {
        const LayerData& d = layerData(x, l);
        if (d.hidden || d.opacity <= 0.001f) return false;
        root = l;
    }
    for (id w : ui(x).windows) {
        const ViewData& wd = viewData(x, w);
        if (wd.visible && wd.layer == root) return true;
    }
    return false;
}
id viewWindow(Ctx& x, id view) {
    for (id v = view; v; v = viewData(x, v).superview) if (viewData(x, v).isWindow) return v;
    return 0;
}
G::Affine viewToWindow(Ctx& x, id view) {
    G::Affine t;
    for (id v = view; v; v = viewData(x, v).superview) {
        if (viewData(x, v).isWindow) break;
        t = t.then(layerToSuper(layerData(x, viewLayer(x, v))));
    }
    return t;
}
G::Point convertPoint(Ctx& x, G::Point p, id from, id to) {
    auto toScreen = [&](id v) -> G::Affine {
        if (!v) return {};
        const id w = viewData(x, v).isWindow ? v : viewWindow(x, v);
        G::Affine t = viewData(x, v).isWindow ? G::Affine{} : viewToWindow(x, v);
        if (w) t = t.then(layerToSuper(layerData(x, viewLayer(x, w))));
        return t;
    };
    // nil means "the window" of the other view
    const id fromW = from ? from : (to ? viewWindow(x, to) : 0);
    const id toW = to ? to : (from ? viewWindow(x, from) : 0);
    const G::Point s = toScreen(fromW).apply(p);
    return toScreen(toW).inverted().apply(s);
}
id hitTest(Ctx& x, id view, G::Point p, id event) { return id(rt(x).send(x.cpu, view, "hitTest:withEvent:", {fbits(p.x), fbits(p.y), event})); }
void layoutIfNeeded(Ctx& x, id view) {
    ViewData& d = viewData(x, view);
    if (d.needsLayout) {
        d.needsLayout = false;
        rt(x).send(x.cpu, view, "layoutSubviews");
    }
    for (id s : std::vector<id>(viewData(x, view).subviews)) layoutIfNeeded(x, s);
}
void layoutPass(Ctx& x) { for (id w : std::vector<id>(ui(x).windows)) if (viewData(x, w).visible) layoutIfNeeded(x, w); }
id viewControllerOfView(Ctx& x, id view) { return viewData(x, view).viewController; }

uint64_t sendRect(Ctx& x, id self, SEL s, const G::Rect& r) { return rt(x).send(x.cpu, self, s, {fbits(r.x), fbits(r.y), fbits(r.w), fbits(r.h)}); }
uint64_t sendPoint(Ctx& x, id self, SEL s, const G::Point& p) { return rt(x).send(x.cpu, self, s, {fbits(p.x), fbits(p.y)}); }
G::Rect getRect(Ctx& x, id self, const char* sel) {
    if (!self) return {};
    auto& r = rt(x);
    const SEL s = r.sel(sel);
    IMP imp = r.lookupForSend(r.classOf(self), s);
    if (!imp) imp = r.unrecognized(self, s);
    const uint32_t tmp = x.proc.heap().allocZeroed(16);
    x.proc.callGuest(x.cpu, imp, {tmp, self, s});
    const CGRect out = CG::readStruct<CGRect>(x, tmp);
    x.proc.heap().free(tmp);
    return CG::toG(out);
}
G::Size getSize(Ctx& x, id self, SEL s, std::initializer_list<uint32_t> args) {
    if (!self) return {};
    auto& r = rt(x);
    IMP imp = r.lookupForSend(r.classOf(self), s);
    if (!imp) imp = r.unrecognized(self, s);
    const uint32_t tmp = x.proc.heap().allocZeroed(16);
    std::vector<uint32_t> a{tmp, self, s};
    a.insert(a.end(), args.begin(), args.end());
    x.proc.callGuest(x.cpu, imp, a);
    const CGSize out = CG::readStruct<CGSize>(x, tmp);
    x.proc.heap().free(tmp);
    return {out.width, out.height};
}
bool sendBool(Ctx& x, id self, const char* sel, std::initializer_list<uint32_t> args) {
    return (uint32_t(rt(x).send(x.cpu, self, sel, args)) & 0xFF) != 0;
}

void registerUIView(Process& p, ObjC::Runtime& r) {
    // HVM_DUMP_VIEWS=1: print the key window's view tree when the app stops.
    if (const char* e = std::getenv("HVM_DUMP_VIEWS"); e && *e == '1') {
        Process* pp = &p;
        p.onExit([pp] {
            CPU::Cpu cpu{};
            Ctx x{*pp, cpu, pp->mem()};
            std::string out;
            for (id w : {ui(x).keyWindow}) describeTree(x, w, 0, out);
            std::fprintf(stderr, "[hvm] view tree:\n%s", out.c_str());
        });
    }
    auto& u = ui(p);
    const std::string R = kR, P = kP, S = kS, T = kT;
    ObjC::ClassBuilder resp = r.defineClass("UIResponder", "NSObject");
    u.responder = resp.cls();
    resp.m<&r_nextResponder>("nextResponder").m<&r_isFirst>("isFirstResponder", "c@:").m<&r_false>("canBecomeFirstResponder", "c@:")
        .m<&r_become>("becomeFirstResponder", "c@:").m<&r_true>("canResignFirstResponder", "c@:").m<&r_resign>("resignFirstResponder", "c@:")
        .method("touchesBegan:withEvent:", [](Ctx& x) { forwardTouches(x, x.cpu.r[0], x.cpu.r[1], x.cpu.r[2], x.cpu.r[3]); }, "v@:@@")
        .method("touchesMoved:withEvent:", [](Ctx& x) { forwardTouches(x, x.cpu.r[0], x.cpu.r[1], x.cpu.r[2], x.cpu.r[3]); }, "v@:@@")
        .method("touchesEnded:withEvent:", [](Ctx& x) { forwardTouches(x, x.cpu.r[0], x.cpu.r[1], x.cpu.r[2], x.cpu.r[3]); }, "v@:@@")
        .method("touchesCancelled:withEvent:", [](Ctx& x) { forwardTouches(x, x.cpu.r[0], x.cpu.r[1], x.cpu.r[2], x.cpu.r[3]); }, "v@:@@")
        .method("motionBegan:withEvent:", [](Ctx& x) { forwardMotion(x, x.cpu.r[0], x.cpu.r[1], int32_t(x.cpu.r[2]), x.cpu.r[3]); }, "v@:i@")
        .method("motionEnded:withEvent:", [](Ctx& x) { forwardMotion(x, x.cpu.r[0], x.cpu.r[1], int32_t(x.cpu.r[2]), x.cpu.r[3]); }, "v@:i@")
        .method("motionCancelled:withEvent:", [](Ctx& x) { forwardMotion(x, x.cpu.r[0], x.cpu.r[1], int32_t(x.cpu.r[2]), x.cpu.r[3]); }, "v@:i@")
        .m<&r_remoteControl>("remoteControlReceivedWithEvent:", "v@:@").m<&r_nil>("inputView").m<&r_nil>("inputAccessoryView")
        .m<&r_nil>("undoManager");

    {
        ObjC::ClassBuilder ax = r.builder(r.getClass("NSObject"));
        for (const char* k : {"IsAccessibilityElement", "AccessibilityLabel", "AccessibilityHint", "AccessibilityValue",
                              "AccessibilityTraits", "AccessibilityIdentifier", "AccessibilityElementsHidden",
                              "AccessibilityViewIsModal", "AccessibilityLanguage", "ShouldGroupAccessibilityChildren",
                              "AccessibilityElements", "AccessibilityCustomActions"}) {
            std::string g = k;
            g[0] = char(std::tolower(g[0]));
            if (g.rfind("is", 0) == 0) g = "isAccessibilityElement";
            ax.m<&ax_set>("set" + std::string(k) + ":", "v@:@").m<&ax_nil>(g);
        }
        // UINibLoadingAdditions (NSObject category): -awakeFromNib does nothing by default.
        ax.m<&ax_noop>("awakeFromNib", "v@:").m<&ax_noop>("prepareForInterfaceBuilder", "v@:");
    }
    ObjC::ClassBuilder view = r.defineClass("UIView", "UIResponder");
    u.view = view.cls();
    view.cm<&vc_layerClass>("layerClass", "#@:")
        .cm<&vc_animate>("animateWithDuration:animations:", "v@:d@?").cm<&vc_animateCompletion>("animateWithDuration:animations:completion:", "v@:d@?@?")
        .cm<&vc_animateFull>("animateWithDuration:delay:options:animations:completion:", "v@:ddI@?@?")
        .cm<&vc_animateSpring>("animateWithDuration:delay:usingSpringWithDamping:initialSpringVelocity:options:animations:completion:", "v@:ddffI@?@?")
        .cm<&vc_transition>("transitionWithView:duration:options:animations:completion:", "v@:@dI@?@?")
        .cm<&vc_transitionFromTo>("transitionFromView:toView:duration:options:completion:", "v@:@@dI@?")
        .cm<&vc_begin>("beginAnimations:context:", "v@:@^v").cm<&vc_commit>("commitAnimations", "v@:")
        .cm<&vc_setDuration>("setAnimationDuration:", "v@:d").cm<&vc_setDelay>("setAnimationDelay:", "v@:d")
        .cm<&vc_setDelegate>("setAnimationDelegate:", "v@:@").cm<&vc_setDidStop>("setAnimationDidStopSelector:", "v@::")
        .cm<&vc_setWillStart>("setAnimationWillStartSelector:", "v@::").cm<&vc_ignoreInt>("setAnimationCurve:", "v@:i")
        .cm<&vc_ignoreFloat>("setAnimationRepeatCount:", "v@:f").cm<&vc_ignoreBool>("setAnimationRepeatAutoreverses:", "v@:c")
        .cm<&vc_ignoreBool>("setAnimationBeginsFromCurrentState:", "v@:c").cm<&vc_setTransition>("setAnimationTransition:forView:cache:", "v@:i@c")
        .cm<&vc_setEnabled>("setAnimationsEnabled:", "v@:c").cm<&vc_enabled>("areAnimationsEnabled", "c@:")
        .cm<&vc_withoutAnimation>("performWithoutAnimation:", "v@:@?");
    view.m<&v_init>("init").m<&v_initWithFrame>("initWithFrame:", "@@:" + R).m<&v_dealloc>("dealloc", "v@:").m<&v_layer>("layer").m<&v_recursiveDescription>("recursiveDescription")
        .m<&v_frame>("frame", R + "@:").m<&v_setFrame>("setFrame:", "v@:" + R).m<&v_bounds>("bounds", R + "@:")
        .m<&v_setBounds>("setBounds:", "v@:" + R).m<&v_center>("center", P + "@:").m<&v_setCenter>("setCenter:", "v@:" + P)
        .m<&v_transform>("transform", T + "@:").m<&v_setTransform>("setTransform:", "v@:" + T)
        .m<&v_backgroundColor>("backgroundColor").m<&v_setBackgroundColor>("setBackgroundColor:", "v@:@")
        .m<&v_alpha>("alpha", "f@:").m<&v_setAlpha>("setAlpha:", "v@:f").m<&v_hidden>("isHidden", "c@:").m<&v_setHidden>("setHidden:", "v@:c")
        .m<&v_opaque>("isOpaque", "c@:").m<&v_setOpaque>("setOpaque:", "v@:c").m<&v_clips>("clipsToBounds", "c@:")
        .m<&v_setClips>("setClipsToBounds:", "v@:c").m<&v_contentMode>("contentMode", "i@:").m<&v_setContentMode>("setContentMode:", "v@:i")
        .m<&v_contentScale>("contentScaleFactor", "f@:").m<&v_setContentScale>("setContentScaleFactor:", "v@:f")
        .m<&v_tag>("tag", "i@:").m<&v_setTag>("setTag:", "v@:i").m<&v_userInteraction>("isUserInteractionEnabled", "c@:")
        .m<&v_setUserInteraction>("setUserInteractionEnabled:", "v@:c").m<&v_multiTouch>("isMultipleTouchEnabled", "c@:")
        .m<&v_setMultiTouch>("setMultipleTouchEnabled:", "v@:c").m<&v_exclusiveTouch>("isExclusiveTouch", "c@:")
        .m<&v_setExclusiveTouch>("setExclusiveTouch:", "v@:c").m<&v_autoresizingMask>("autoresizingMask", "I@:")
        .m<&v_setAutoresizingMask>("setAutoresizingMask:", "v@:I").m<&v_autoresizesSubviews>("autoresizesSubviews", "c@:")
        .m<&v_setAutoresizesSubviews>("setAutoresizesSubviews:", "v@:c").m<&v_clearsContext>("clearsContextBeforeDrawing", "c@:")
        .m<&v_setClearsContext>("setClearsContextBeforeDrawing:", "v@:c").m<&v_tintColor>("tintColor").m<&v_ignoreObj>("setTintColor:", "v@:@")
        .m<&v_ignoreBool>("setTranslatesAutoresizingMaskIntoConstraints:", "v@:c")
        .m<&v_superview>("superview").m<&v_subviews>("subviews").m<&v_window>("window")
        .m<&v_addSubview>("addSubview:", "v@:@").m<&v_insertAt>("insertSubview:atIndex:", "v@:@i")
        .m<&v_insertAbove>("insertSubview:aboveSubview:", "v@:@@").m<&v_insertBelow>("insertSubview:belowSubview:", "v@:@@")
        .m<&v_bringToFront>("bringSubviewToFront:", "v@:@").m<&v_sendToBack>("sendSubviewToBack:", "v@:@")
        .m<&v_exchange>("exchangeSubviewAtIndex:withSubviewAtIndex:", "v@:ii").m<&v_removeFromSuperview>("removeFromSuperview", "v@:")
        .m<&v_hookObj>("willMoveToSuperview:", "v@:@").m<&v_hook>("didMoveToSuperview", "v@:").m<&v_hookObj>("willMoveToWindow:", "v@:@")
        .m<&v_hook>("didMoveToWindow", "v@:").m<&v_hookObj>("didAddSubview:", "v@:@").m<&v_hookObj>("willRemoveSubview:", "v@:@")
        .m<&v_isDescendant>("isDescendantOfView:", "c@:@").m<&v_viewWithTag>("viewWithTag:").m<&v_nextResponder>("nextResponder")
        .m<&v_setNeedsLayout>("setNeedsLayout", "v@:").m<&v_layoutIfNeeded>("layoutIfNeeded", "v@:").m<&v_layoutSubviews>("layoutSubviews", "v@:")
        .m<&v_setNeedsDisplay>("setNeedsDisplay", "v@:").m<&v_setNeedsDisplayInRect>("setNeedsDisplayInRect:", "v@:" + R)
        .m<&v_drawRect>("drawRect:", "v@:" + R).m<&v_drawLayerInContext>("drawLayer:inContext:", "v@:@^{CGContext=}")
        .m<&v_sizeThatFits>("sizeThatFits:", S + "@:" + S).m<&v_sizeToFit>("sizeToFit", "v@:")
        .m<&v_pointInside>("pointInside:withEvent:", "c@:" + P + "@").m<&v_hitTest>("hitTest:withEvent:", "@@:" + P + "@")
        .m<&v_convertPointTo>("convertPoint:toView:", P + "@:" + P + "@").m<&v_convertPointFrom>("convertPoint:fromView:", P + "@:" + P + "@")
        .m<&v_convertRectTo>("convertRect:toView:", R + "@:" + R + "@").m<&v_convertRectFrom>("convertRect:fromView:", R + "@:" + R + "@")
        .m<&v_endEditing>("endEditing:", "c@:c").m<&v_gestures>("gestureRecognizers")
        .m<&v_addGesture>("addGestureRecognizer:", "v@:@").m<&v_removeGesture>("removeGestureRecognizer:", "v@:@");

    ObjC::ClassBuilder win = r.defineClass("UIWindow", "UIView");
    u.window = win.cls();
    win.cm<&w_keyWindowClass>("keyWindow");
    win.m<&w_initWithFrame>("initWithFrame:", "@@:" + R).m<&w_makeKeyAndVisible>("makeKeyAndVisible", "v@:")
       .m<&w_makeKey>("makeKeyWindow", "v@:").m<&w_isKey>("isKeyWindow", "c@:").m<&w_becomeKey>("becomeKeyWindow", "v@:")
       .m<&w_becomeKey>("resignKeyWindow", "v@:").m<&w_setHidden>("setHidden:", "v@:c").m<&w_level>("windowLevel", "f@:")
       .m<&w_setLevel>("setWindowLevel:", "v@:f").m<&w_screen>("screen").m<&v_ignoreObj>("setScreen:", "v@:@")
       .m<&w_root>("rootViewController").m<&w_setRoot>("setRootViewController:", "v@:@")
       .m<&w_convertPointToWindow>("convertPoint:toWindow:", P + "@:" + P + "@")
       .m<&w_convertPointFromWindow>("convertPoint:fromWindow:", P + "@:" + P + "@");
    for (const char* n : {"UIWindowDidBecomeKeyNotification", "UIWindowDidResignKeyNotification", "UIWindowDidBecomeVisibleNotification",
                          "UIWindowDidBecomeHiddenNotification", "UIKeyboardWillShowNotification", "UIKeyboardDidShowNotification",
                          "UIKeyboardWillHideNotification", "UIKeyboardDidHideNotification", "UIKeyboardFrameEndUserInfoKey",
                          "UIKeyboardFrameBeginUserInfoKey", "UIKeyboardAnimationDurationUserInfoKey", "UIKeyboardBoundsUserInfoKey"}) {
        const uint32_t cell = p.defineDataBlock(std::string("_") + n, 4);
        p.mem().write<uint32_t>(cell, Foundation::constString(p, n));
    }
    for (auto [n, v] : std::initializer_list<std::pair<const char*, float>>{{"_UIWindowLevelNormal", 0.0f}, {"_UIWindowLevelAlert", 2000.0f}, {"_UIWindowLevelStatusBar", 1000.0f}}) {
        const uint32_t cell = p.defineDataBlock(n, 4);
        p.mem().write<float>(cell, v);
    }

    ObjC::ClassBuilder scr = r.defineClass("UIScreen", "NSObject");
    u.screen = scr.cls();
    scr.cm<&sc_main>("mainScreen").cm<&sc_screens>("screens");
    scr.m<&sc_bounds>("bounds", R + "@:").m<&sc_nativeBounds>("nativeBounds", R + "@:").m<&sc_appFrame>("applicationFrame", R + "@:")
       .m<&sc_scale>("scale", "f@:").m<&sc_scale>("nativeScale", "f@:").m<&sc_brightness>("brightness", "f@:")
       .m<&sc_setBrightness>("setBrightness:", "v@:f").m<&sc_currentMode>("currentMode").m<&sc_currentMode>("preferredMode")
       .m<&sc_modes>("availableModes").m<&sc_setMode>("setCurrentMode:").m<&sc_setMode>("setOverscanCompensation:")
       .m<&sc_overscan>("overscanCompensation", "i@:").m<&sc_zeroFloat>("overscanCompensationPixelsPerInch", "f@:")
       .m<&sc_nil>("mirroredScreen")
       .m<&sc_displayLink>("displayLinkWithTarget:selector:", "@@:@:");
    ObjC::ClassBuilder mode = r.defineClass("UIScreenMode", "NSObject");
    mode.m<&sm_size>("size", S + "@:").m<&sm_aspect>("pixelAspectRatio", "f@:");
    u.mainScreen = r.createInstance(u.screen);
    r.makeImmortal(u.mainScreen);
    for (const char* n : {"UIScreenDidConnectNotification", "UIScreenDidDisconnectNotification", "UIScreenModeDidChangeNotification"}) {
        const uint32_t cell = p.defineDataBlock(std::string("_") + n, 4);
        p.mem().write<uint32_t>(cell, Foundation::constString(p, n));
    }

    ObjC::ClassBuilder vc = r.defineClass("UIViewController", "UIResponder");
    u.viewController = vc.cls();
    vc.m<&vc_init>("init").m<&vc_initWithNib>("initWithNibName:bundle:").m<&vc_initWithCoder>("initWithCoder:")
      .m<&vc_dealloc>("dealloc", "v@:").m<&vc_loadView>("loadView", "v@:").m<&vc_view>("view").m<&vc_setView>("setView:", "v@:@")
      .m<&vc_isViewLoaded>("isViewLoaded", "c@:").m<&vc_hook>("viewDidLoad", "v@:").m<&vc_hook>("viewDidUnload", "v@:")
      .m<&vc_hookBool>("viewWillAppear:", "v@:c").m<&vc_hookBool>("viewDidAppear:", "v@:c").m<&vc_hookBool>("viewWillDisappear:", "v@:c")
      .m<&vc_hookBool>("viewDidDisappear:", "v@:c").m<&vc_hook>("viewWillLayoutSubviews", "v@:").m<&vc_hook>("viewDidLayoutSubviews", "v@:")
      .m<&vc_hook>("didReceiveMemoryWarning", "v@:").m<&vc_hook>("awakeFromNib", "v@:").m<&vc_title>("title").m<&vc_setTitle>("setTitle:", "v@:@")
      .m<&vc_nibName>("nibName").m<&vc_nibBundle>("nibBundle").m<&vc_parent>("parentViewController")
      .m<&vc_presented>("presentedViewController").m<&vc_presented>("modalViewController").m<&vc_presenting>("presentingViewController")
      .m<&vc_shouldAutorotate>("shouldAutorotateToInterfaceOrientation:", "c@:i").m<&vc_yes>("shouldAutorotate", "c@:")
      .m<&vc_supported>("supportedInterfaceOrientations", "I@:").m<&vc_preferredOrientation>("preferredInterfaceOrientationForPresentation", "i@:")
      .m<&vc_interfaceOrientation>("interfaceOrientation", "i@:").m<&vc_wantsFull>("wantsFullScreenLayout", "c@:")
      .m<&vc_setWantsFull>("setWantsFullScreenLayout:", "v@:c").m<&vc_childVCs>("childViewControllers")
      .m<&vc_addChild>("addChildViewController:", "v@:@").m<&vc_removeFromParent>("removeFromParentViewController", "v@:")
      .m<&vc_hookObj>("willMoveToParentViewController:", "v@:@").m<&vc_hookObj>("didMoveToParentViewController:", "v@:@")
      .m<&vc_present>("presentViewController:animated:completion:", "v@:@c@?").m<&vc_presentModal>("presentModalViewController:animated:", "v@:@c")
      .m<&vc_dismiss>("dismissViewControllerAnimated:completion:", "v@:c@?").m<&vc_dismissModal>("dismissModalViewControllerAnimated:", "v@:c")
      .m<&vc_nextResponder>("nextResponder").m<&vc_navigationController>("navigationController")
      .m<&vc_tabBarController>("tabBarController").m<&vc_navigationItem>("navigationItem")
      .m<&vc_editing>("isEditing", "c@:").m<&vc_setEditing>("setEditing:animated:", "v@:cc")
      .m<&vc_zero>("modalTransitionStyle", "i@:").m<&vc_setInt>("setModalTransitionStyle:", "v@:i")
      .m<&vc_zero>("modalPresentationStyle", "i@:").m<&vc_setInt>("setModalPresentationStyle:", "v@:i")
      .m<&vc_zero>("preferredStatusBarStyle", "i@:").m<&vc_editing>("prefersStatusBarHidden", "c@:")
      .m<&vc_hook>("setNeedsStatusBarAppearanceUpdate", "v@:").m<&vc_contentSize>("contentSizeForViewInPopover", S + "@:")
      .m<&vc_hookObj>("setHidesBottomBarWhenPushed:", "v@:c");
}

} // namespace Hydrogen::Runtime::UIKit
