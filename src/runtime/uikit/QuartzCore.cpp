// HydrogenVM — QuartzCore HLE: CALayer, CATransform3D, CATransaction,
// CADisplayLink, CAAnimation (accepted; completion callbacks fire, no
// interpolation yet) and the compositor that turns the layer trees of all
// visible windows into the framebuffer.
#include "UIKit.hpp"
#include <chrono>

namespace Hydrogen::Runtime::UIKit {

using Foundation::autoreleased;
using Foundation::makeString;
using Foundation::utf8;

namespace {

void dirty(Ctx& x) { ui(x).needsComposite = true; }
LayerData& L(Ctx& x, id l) { return layerData(x, l); }

// ---------------------------------------------------------------- CALayer ---

id lc_layer(Ctx& x, id self, SEL) { return autoreleased(x, id(rt(x).send(x.cpu, id(rt(x).send(x.cpu, self, rt(x).selAlloc)), rt(x).selInit))); }
id l_init(Ctx& x, id self, SEL) { L(x, self); return self; }
id l_initWithLayer(Ctx& x, id self, SEL, id other) {
    LayerData& d = L(x, self);
    if (other) {
        const LayerData& o = L(x, other);
        d.bounds = o.bounds; d.position = o.position; d.anchor = o.anchor; d.transform = o.transform; d.opacity = o.opacity;
        d.hidden = o.hidden; d.background = o.background; d.contentsScale = o.contentsScale;
    }
    return self;
}
void l_dealloc(Ctx& x, id self, SEL) {
    if (LayerData* d = rt(x).hostIf<LayerData>(self)) {
        auto subs = std::move(d->sublayers);
        d->sublayers.clear();
        for (id s : subs) { if (LayerData* sd = rt(x).hostIf<LayerData>(s)) sd->superlayer = 0; rt(x).release(x.cpu, s); }
        const id c = d->contents, m = d->mask, n = d->name;
        d->contents = d->mask = d->name = 0;
        for (id o : {c, m, n}) if (o) rt(x).release(x.cpu, o);
    }
    dirty(x);
    rt(x).disposeInstance(x.cpu, self);
}

void l_bounds(Ctx& x, GuestPtr ret, id self, SEL) { CG::writeStruct(x, ret, CG::fromG(L(x, self).bounds)); }
void l_setBounds(Ctx& x, id self, SEL, CGRect r) {
    LayerData& d = L(x, self);
    const G::Rect nr = CG::toG(r);
    const bool sizeChanged = nr.w != d.bounds.w || nr.h != d.bounds.h;
    d.bounds = nr;
    if (sizeChanged && d.needsDisplayOnBoundsChange) d.needsDisplay = true;
    dirty(x);
}
void l_position(Ctx& x, GuestPtr ret, id self, SEL) { CG::writeStruct(x, ret, CG::fromG(L(x, self).position)); }
void l_setPosition(Ctx& x, id self, SEL, CGPoint p) { L(x, self).position = CG::toG(p); dirty(x); }
void l_anchor(Ctx& x, GuestPtr ret, id self, SEL) { CG::writeStruct(x, ret, CG::fromG(L(x, self).anchor)); }
void l_setAnchor(Ctx& x, id self, SEL, CGPoint p) { L(x, self).anchor = CG::toG(p); dirty(x); }
void l_frame(Ctx& x, GuestPtr ret, id self, SEL) { CG::writeStruct(x, ret, CG::fromG(layerFrame(L(x, self)))); }
void l_setFrame(Ctx& x, id self, SEL, CGRect r) { layerSetFrame(x, self, CG::toG(r)); }
float l_zPosition(Ctx& x, id self, SEL) { return L(x, self).zPosition; }
void l_setZPosition(Ctx& x, id self, SEL, float z) { L(x, self).zPosition = z; dirty(x); }
void l_transform(Ctx& x, GuestPtr ret, id self, SEL) { CG::writeStruct(x, ret, L(x, self).transform); }
void l_setTransform(Ctx& x, id self, SEL, CATransform3D t) { L(x, self).transform = t; dirty(x); }
void l_sublayerTransform(Ctx& x, GuestPtr ret, id self, SEL) { CG::writeStruct(x, ret, L(x, self).sublayerTransform); }
void l_setSublayerTransform(Ctx& x, id self, SEL, CATransform3D t) { L(x, self).sublayerTransform = t; dirty(x); }
void l_affine(Ctx& x, GuestPtr ret, id self, SEL) { CG::writeStruct(x, ret, CG::fromG(affineOf(L(x, self).transform))); }
void l_setAffine(Ctx& x, id self, SEL, CGAffineTransform t) { L(x, self).transform = from2D(CG::toG(t)); dirty(x); }
bool l_hidden(Ctx& x, id self, SEL) { return L(x, self).hidden; }
void l_setHidden(Ctx& x, id self, SEL, bool v) { L(x, self).hidden = v; dirty(x); }
float l_opacity(Ctx& x, id self, SEL) { return L(x, self).opacity; }
void l_setOpacity(Ctx& x, id self, SEL, float v) { L(x, self).opacity = v; dirty(x); }
bool l_masks(Ctx& x, id self, SEL) { return L(x, self).masksToBounds; }
void l_setMasks(Ctx& x, id self, SEL, bool v) { L(x, self).masksToBounds = v; dirty(x); }
bool l_opaque(Ctx& x, id self, SEL) { return L(x, self).opaque; }
void l_setOpaque(Ctx& x, id self, SEL, bool v) { L(x, self).opaque = v; }
float l_cornerRadius(Ctx& x, id self, SEL) { return L(x, self).cornerRadius; }
void l_setCornerRadius(Ctx& x, id self, SEL, float v) { L(x, self).cornerRadius = v; dirty(x); }
float l_borderWidth(Ctx& x, id self, SEL) { return L(x, self).borderWidth; }
void l_setBorderWidth(Ctx& x, id self, SEL, float v) { L(x, self).borderWidth = v; dirty(x); }
id l_borderColor(Ctx& x, id self, SEL) { return autoreleased(x, CG::createColor(x, L(x, self).border)); }
void l_setBorderColor(Ctx& x, id self, SEL, id c) { L(x, self).border = c ? CG::colorOf(x, c) : G::Color{0, 0, 0, 1}; dirty(x); }
id l_backgroundColor(Ctx& x, id self, SEL) {
    const G::Color c = L(x, self).background;
    return c.a > 0 ? autoreleased(x, CG::createColor(x, c)) : 0;
}
void l_setBackgroundColor(Ctx& x, id self, SEL, id c) { L(x, self).background = c ? CG::colorOf(x, c) : G::Color{}; dirty(x); }
id l_contents(Ctx& x, id self, SEL) { return L(x, self).contents; }
void l_setContents(Ctx& x, id self, SEL, id c) {
    LayerData& d = L(x, self);
    if (c == d.contents) { dirty(x); return; }
    const id old = d.contents;
    d.contents = c ? rt(x).retain(x.cpu, c) : 0;
    if (old) rt(x).release(x.cpu, old);
    d.backing.reset();
    dirty(x);
}
void l_contentsRect(Ctx& x, GuestPtr ret, id self, SEL) { CG::writeStruct(x, ret, CG::fromG(L(x, self).contentsRect)); }
void l_setContentsRect(Ctx& x, id self, SEL, CGRect r) { L(x, self).contentsRect = CG::toG(r); dirty(x); }
void l_contentsCenter(Ctx& x, GuestPtr ret, id self, SEL) { CG::writeStruct(x, ret, CG::fromG(L(x, self).contentsCenter)); }
void l_setContentsCenter(Ctx& x, id self, SEL, CGRect r) { L(x, self).contentsCenter = CG::toG(r); dirty(x); }
const char* const kGravityNames[] = {"resize", "resizeAspect", "resizeAspectFill", "center", "top", "bottom", "left", "right",
                                     "topLeft", "topRight", "bottomLeft", "bottomRight"};
id l_gravity(Ctx& x, id self, SEL) { return makeString(x, std::string(kGravityNames[L(x, self).gravity])); }
void l_setGravity(Ctx& x, id self, SEL, id g) {
    const std::string s = g ? utf8(x, g) : "resize";
    for (int i = 0; i < 12; ++i) if (s == kGravityNames[i]) L(x, self).gravity = i;
    dirty(x);
}
float l_contentsScale(Ctx& x, id self, SEL) { return L(x, self).contentsScale; }
void l_setContentsScale(Ctx& x, id self, SEL, float s) { L(x, self).contentsScale = s > 0 ? s : 1; }
void l_setFilter(Ctx& x, id self, SEL, id f) { L(x, self).magnifyNearest = f && utf8(x, f) == "nearest"; }
id l_filter(Ctx& x, id self, SEL) { return makeString(x, L(x, self).magnifyNearest ? "nearest" : "linear"); }
id l_delegate(Ctx& x, id self, SEL) { return L(x, self).delegate; }
void l_setDelegate(Ctx& x, id self, SEL, id d) { L(x, self).delegate = d; }
id l_superlayer(Ctx& x, id self, SEL) { return L(x, self).superlayer; }
id l_sublayers(Ctx& x, id self, SEL) {
    const auto& s = L(x, self).sublayers;
    return s.empty() ? 0 : Foundation::makeArray(x, s);
}
void l_setSublayers(Ctx& x, id self, SEL, id arr) {
    for (id s : std::vector<id>(L(x, self).sublayers)) layerRemoveFromSuperlayer(x, s);
    for (id s : Foundation::arrayItems(x, arr)) layerAddSublayer(x, self, s);
}
void l_addSublayer(Ctx& x, id self, SEL, id s) { layerAddSublayer(x, self, s); }
void l_insertAt(Ctx& x, id self, SEL, id s, uint32_t i) { layerAddSublayer(x, self, s, long(i)); }
long indexOfLayer(Ctx& x, id parent, id child) {
    const auto& v = L(x, parent).sublayers;
    auto it = std::find(v.begin(), v.end(), child);
    return it == v.end() ? -1 : long(it - v.begin());
}
void l_insertAbove(Ctx& x, id self, SEL, id s, id sib) {
    const long i = sib ? indexOfLayer(x, self, sib) : -1;
    layerAddSublayer(x, self, s, i < 0 ? -1 : i + 1);
}
void l_insertBelow(Ctx& x, id self, SEL, id s, id sib) {
    const long i = sib ? indexOfLayer(x, self, sib) : -1;
    layerAddSublayer(x, self, s, i < 0 ? 0 : i);
}
void l_replace(Ctx& x, id self, SEL, id old, id with) {
    const long i = indexOfLayer(x, self, old);
    if (i < 0) return;
    layerRemoveFromSuperlayer(x, old);
    layerAddSublayer(x, self, with, i);
}
void l_removeFromSuperlayer(Ctx& x, id self, SEL) { layerRemoveFromSuperlayer(x, self); }
void l_setNeedsDisplay(Ctx& x, id self, SEL) { L(x, self).needsDisplay = true; dirty(x); }
void l_setNeedsDisplayInRect(Ctx& x, id self, SEL, CGRect) { L(x, self).needsDisplay = true; dirty(x); }
bool l_needsDisplay(Ctx& x, id self, SEL) { return L(x, self).needsDisplay; }
bool l_needsDisplayOnBounds(Ctx& x, id self, SEL) { return L(x, self).needsDisplayOnBoundsChange; }
void l_setNeedsDisplayOnBounds(Ctx& x, id self, SEL, bool v) { L(x, self).needsDisplayOnBoundsChange = v; }

// -display: delegate's displayLayer:, else draw into a backing store via drawInContext:.
void l_display(Ctx& x, id self, SEL) {
    LayerData& d = L(x, self);
    d.needsDisplay = false;
    auto& r = rt(x);
    if (d.delegate && r.respondsTo(d.delegate, r.sel("displayLayer:"))) { r.send(x.cpu, d.delegate, "displayLayer:", {self}); return; }
    const float s = d.contentsScale;
    const int w = int(std::ceil(d.bounds.w * s)), h = int(std::ceil(d.bounds.h * s));
    if (w <= 0 || h <= 0) { d.backing.reset(); return; }
    const id ctx = CG::createBitmapContext(x, 0, w, h, 0, 0x2002 /* premultiplied first, 32 little: BGRA */);
    if (!ctx) return;
    if (d.opaque && d.background.a > 0) CG::fillRect(x, ctx, {0, 0, float(w), float(h)}, d.background, G::BlendMode::Copy);
    CG::setUIKitCTM(x, ctx, d.bounds.h, s);
    r.send(x.cpu, ctx, "self");   // (keeps ctx alive across the call in traces)
    CG::ContextData* cd = CG::context(x, ctx);
    cd->gs().ctm = G::Affine::translate(-d.bounds.x, -d.bounds.y).then(cd->gs().ctm);
    CG::pushContext(x, ctx);   // so UIGraphicsGetCurrentContext works inside -drawInContext: overrides that call UIKit
    try {
        r.send(x.cpu, self, "drawInContext:", {ctx});
    } catch (...) { CG::popContext(x); r.release(x.cpu, ctx); throw; }
    CG::popContext(x);
    d.backing = CG::contextImage(x, ctx);
    r.release(x.cpu, ctx);
    dirty(x);
}
void l_displayIfNeeded(Ctx& x, id self, SEL) { if (L(x, self).needsDisplay) rt(x).send(x.cpu, self, "display"); }
void l_drawInContext(Ctx& x, id self, SEL, id ctx) {
    LayerData& d = L(x, self);
    auto& r = rt(x);
    if (d.delegate && r.respondsTo(d.delegate, r.sel("drawLayer:inContext:"))) r.send(x.cpu, d.delegate, "drawLayer:inContext:", {self, ctx});
}
void l_setNeedsLayout(Ctx& x, id self, SEL) { dirty(x); }
void l_layoutIfNeeded(Ctx& x, id self, SEL) { rt(x).send(x.cpu, self, "layoutSublayers"); }
void l_layoutSublayers(Ctx& x, id self, SEL) {
    LayerData& d = L(x, self);
    auto& r = rt(x);
    if (d.delegate && r.respondsTo(d.delegate, r.sel("layoutSublayersOfLayer:"))) r.send(x.cpu, d.delegate, "layoutSublayersOfLayer:", {self});
}
G::Affine layerToRoot(Ctx& x, id layer, id* root = nullptr) {
    G::Affine t;
    id cur = layer;
    while (cur) {
        const LayerData& d = L(x, cur);
        if (!d.superlayer) break;
        t = t.then(layerToSuper(d));
        cur = d.superlayer;
    }
    if (root) *root = cur;
    return t;
}
G::Point convertLayerPoint(Ctx& x, G::Point p, id from, id to) {
    const G::Affine a = from ? layerToRoot(x, from) : G::Affine{};
    const G::Affine b = to ? layerToRoot(x, to) : G::Affine{};
    return b.inverted().apply(a.apply(p));
}
void l_convertPointTo(Ctx& x, GuestPtr ret, id self, SEL, CGPoint p, id to) { CG::writeStruct(x, ret, CG::fromG(convertLayerPoint(x, CG::toG(p), self, to))); }
void l_convertPointFrom(Ctx& x, GuestPtr ret, id self, SEL, CGPoint p, id from) { CG::writeStruct(x, ret, CG::fromG(convertLayerPoint(x, CG::toG(p), from, self))); }
void l_convertRectTo(Ctx& x, GuestPtr ret, id self, SEL, CGRect r, id to) {
    const G::Affine a = layerToRoot(x, self), b = to ? layerToRoot(x, to) : G::Affine{};
    CG::writeStruct(x, ret, CG::fromG(a.then(b.inverted()).applyRect(CG::toG(r))));
}
void l_convertRectFrom(Ctx& x, GuestPtr ret, id self, SEL, CGRect r, id from) {
    const G::Affine a = from ? layerToRoot(x, from) : G::Affine{}, b = layerToRoot(x, self);
    CG::writeStruct(x, ret, CG::fromG(a.then(b.inverted()).applyRect(CG::toG(r))));
}
bool l_containsPoint(Ctx& x, id self, SEL, CGPoint p) { return L(x, self).bounds.contains(CG::toG(p)); }
id hitLayer(Ctx& x, id layer, G::Point pSuper) {
    const LayerData& d = L(x, layer);
    if (d.hidden) return 0;
    const G::Point p = layerToSuper(d).inverted().apply(pSuper);
    if (d.masksToBounds && !d.bounds.contains(p)) return 0;
    for (auto it = d.sublayers.rbegin(); it != d.sublayers.rend(); ++it) if (const id h = hitLayer(x, *it, p)) return h;
    return d.bounds.contains(p) ? layer : 0;
}
id l_hitTest(Ctx& x, id self, SEL, CGPoint p) { return hitLayer(x, self, CG::toG(p)); }
id l_name(Ctx& x, id self, SEL) { return L(x, self).name; }
void l_setName(Ctx& x, id self, SEL, id n) { LayerData& d = L(x, self); const id o = d.name; d.name = n ? id(rt(x).send(x.cpu, n, "copy")) : 0; if (o) rt(x).release(x.cpu, o); }
id l_mask(Ctx& x, id self, SEL) { return L(x, self).mask; }
void l_setMask(Ctx& x, id self, SEL, id m) {
    LayerData& d = L(x, self);
    const id o = d.mask;
    d.mask = m ? rt(x).retain(x.cpu, m) : 0;
    if (o) rt(x).release(x.cpu, o);
    if (m) x.proc.reportMissing("CALayer mask (masks are ignored when compositing)");
}
id l_self(Ctx&, id self, SEL) { return self; }
void l_ignoreObj(Ctx&, id, SEL, id) {}
void l_ignoreFloat(Ctx&, id, SEL, float) {}
void l_ignoreBool(Ctx&, id, SEL, bool) {}
void l_ignoreSize(Ctx&, id, SEL, CGSize) {}
bool l_false(Ctx&, id, SEL) { return false; }
float l_zero(Ctx&, id, SEL) { return 0; }
bool l_doubleSided(Ctx& x, id self, SEL) { return L(x, self).doubleSided; }
void l_setDoubleSided(Ctx& x, id self, SEL, bool v) { L(x, self).doubleSided = v; }
bool l_flipped(Ctx& x, id self, SEL) { return L(x, self).geometryFlipped; }
void l_setFlipped(Ctx& x, id self, SEL, bool v) { L(x, self).geometryFlipped = v; }

// ---- animations attached to layers: remember them, fire delegate callbacks.
struct AnimData : ObjC::HostObject {
    std::unordered_map<std::string, uint64_t> values;   // property bag (raw bits)
    std::unordered_map<std::string, id> objects;        // retained object properties
};
void l_addAnimation(Ctx& x, id self, SEL, id anim, id key) {
    if (!anim) return;
    auto& r = rt(x);
    AnimData& a = r.host<AnimData>(anim);
    double dur = 0.25;
    if (auto it = a.values.find("duration"); it != a.values.end()) { std::memcpy(&dur, &it->second, 8); if (dur <= 0) dur = 0.25; }
    float repeat = 0;
    if (auto it = a.values.find("repeatCount"); it != a.values.end()) { const uint32_t b = uint32_t(it->second); std::memcpy(&repeat, &b, 4); }
    const id delegate = a.objects.count("delegate") ? a.objects["delegate"] : 0;
    // Apply the end state of basic animations whose model value won't be set otherwise.
    if (delegate && r.respondsTo(delegate, r.sel("animationDidStart:"))) r.send(x.cpu, delegate, "animationDidStart:", {anim});
    if (std::isinf(repeat) || repeat > 1e6f) return;   // runs forever
    const double total = dur * std::max(1.0f, repeat);
    r.retain(x.cpu, anim);
    Process* p = &x.proc;
    Foundation::mainRunLoop(x.proc).addTimer(total, 0, false, [p, anim](CPU::Cpu& c) {
        Ctx cx{*p, c, p->mem()};
        auto& rr = rt(cx);
        AnimData& ad = rr.host<AnimData>(anim);
        const id del = ad.objects.count("delegate") ? ad.objects["delegate"] : 0;
        if (del && rr.respondsTo(del, rr.sel("animationDidStop:finished:"))) rr.send(c, del, "animationDidStop:finished:", {anim, 1});
        rr.release(c, anim);
    });
    (void)key;
}
void l_removeAnimation(Ctx&, id, SEL, id) {}
void l_removeAll(Ctx&, id, SEL) {}
id l_animationForKey(Ctx&, id, SEL, id) { return 0; }
id l_animationKeys(Ctx&, id, SEL) { return 0; }
void l_renderInContext(Ctx& x, id self, SEL, id ctx);

// ---------------------------------------------------- generic CAAnimation ---

// Animation objects are property bags: every setter stores, every getter returns.
struct PropSpec { const char* name; char type; };
const PropSpec kAnimProps[] = {
    {"duration", 'd'}, {"beginTime", 'd'}, {"timeOffset", 'd'}, {"repeatCount", 'f'}, {"repeatDuration", 'd'}, {"speed", 'f'},
    {"autoreverses", 'c'}, {"removedOnCompletion", 'c'}, {"fillMode", '@'}, {"timingFunction", '@'}, {"delegate", '@'},
    {"keyPath", '@'}, {"fromValue", '@'}, {"toValue", '@'}, {"byValue", '@'}, {"values", '@'}, {"keyTimes", '@'},
    {"timingFunctions", '@'}, {"calculationMode", '@'}, {"path", '@'}, {"rotationMode", '@'}, {"animations", '@'},
    {"type", '@'}, {"subtype", '@'}, {"startProgress", 'f'}, {"endProgress", 'f'}, {"cumulative", 'c'}, {"additive", 'c'},
};
std::string setterName(const char* p) { std::string s = "set"; s += char(std::toupper(uint8_t(p[0]))); s += p + 1; return s + ":"; }
void defineBag(ObjC::ClassBuilder& b) {
    for (const PropSpec& ps : kAnimProps) {
        const std::string name = ps.name;
        const char t = ps.type;
        b.method(name, [name, t](Ctx& x) {
            AnimData& a = rt(x).host<AnimData>(x.cpu.r[0]);
            if (t == '@') { auto it = a.objects.find(name); x.cpu.r[0] = it == a.objects.end() ? 0 : it->second; return; }
            auto it = a.values.find(name);
            uint64_t v = it == a.values.end() ? 0 : it->second;
            if (it == a.values.end() && (name == "speed" || name == "removedOnCompletion")) {
                if (name == "speed") { const float one = 1; uint32_t bb; std::memcpy(&bb, &one, 4); v = bb; } else v = 1;
            }
            x.cpu.r[0] = uint32_t(v);
            x.cpu.r[1] = uint32_t(v >> 32);
        }, std::string(1, t == 'c' ? 'c' : t) + "@:");
        b.method(setterName(ps.name), [name, t](Ctx& x) {
            AnimData& a = rt(x).host<AnimData>(x.cpu.r[0]);
            if (t == '@') {
                const id nv = x.cpu.r[2];
                // delegates are retained by CAAnimation (documented exception)
                auto it = a.objects.find(name);
                const id old = it == a.objects.end() ? 0 : it->second;
                a.objects[name] = nv ? rt(x).retain(x.cpu, nv) : 0;
                if (old) rt(x).release(x.cpu, old);
            } else if (t == 'd') a.values[name] = uint64_t(x.cpu.r[2]) | uint64_t(x.cpu.r[3]) << 32;
            else if (t == 'c') a.values[name] = x.cpu.r[2] & 0xFF;
            else a.values[name] = x.cpu.r[2];
        }, std::string("v@:") + t);
    }
}
id anim_withKeyPath(Ctx& x, id self, SEL, id path) {
    const id a = autoreleased(x, id(rt(x).send(x.cpu, id(rt(x).send(x.cpu, self, rt(x).selAlloc)), rt(x).selInit)));
    rt(x).send(x.cpu, a, "setKeyPath:", {path});
    return a;
}
id anim_animation(Ctx& x, id self, SEL) { return autoreleased(x, id(rt(x).send(x.cpu, id(rt(x).send(x.cpu, self, rt(x).selAlloc)), rt(x).selInit))); }
id anim_copy(Ctx& x, id self, SEL, uint32_t) {
    const id c = rt(x).createInstance(rt(x).classOf(self));
    AnimData& src = rt(x).host<AnimData>(self);
    AnimData& dst = rt(x).host<AnimData>(c);
    dst.values = src.values;
    for (auto& [k, v] : src.objects) dst.objects[k] = v ? rt(x).retain(x.cpu, v) : 0;
    return c;
}
void anim_dealloc(Ctx& x, id self, SEL) {
    if (AnimData* a = rt(x).hostIf<AnimData>(self)) {
        auto objs = std::move(a->objects);
        a->objects.clear();
        for (auto& [k, v] : objs) if (v) rt(x).release(x.cpu, v);
    }
    rt(x).disposeInstance(x.cpu, self);
}
id tf_withName(Ctx& x, id self, SEL, id) { return autoreleased(x, rt(x).createInstance(self)); }
id tf_withControlPoints(Ctx& x, id self, SEL, float, float, float, float) { return autoreleased(x, rt(x).createInstance(self)); }

// ------------------------------------------------------------ CATransaction ---

void tx_noop(Ctx&, id, SEL) {}
void tx_setDisable(Ctx&, id, SEL, bool) {}
void tx_setDuration(Ctx&, id, SEL, double) {}
bool tx_disable(Ctx&, id, SEL) { return false; }
double tx_duration(Ctx&, id, SEL) { return 0.25; }
void tx_setCompletion(Ctx& x, id, SEL, uint32_t block) {
    if (!block) return;
    const uint32_t b = ObjC::blockCopy(x, block);
    Process* p = &x.proc;
    Foundation::mainRunLoop(x.proc).post([p, b](CPU::Cpu& c) { Ctx cx{*p, c, p->mem()}; ObjC::callBlock(cx, b); ObjC::blockRelease(cx, b); });
}
void tx_setValue(Ctx&, id, SEL, id, id) {}
id tx_value(Ctx&, id, SEL, id) { return 0; }
void tx_setTimingFunction(Ctx&, id, SEL, id) {}

// ----------------------------------------------------------- CATransform3D ---

void t3_makeTranslation(Ctx& x, GuestPtr ret, float tx, float ty, float tz) { CATransform3D t = identity3D(); t.m41 = tx; t.m42 = ty; t.m43 = tz; CG::writeStruct(x, ret, t); }
void t3_makeScale(Ctx& x, GuestPtr ret, float sx, float sy, float sz) { CATransform3D t = identity3D(); t.m11 = sx; t.m22 = sy; t.m33 = sz; CG::writeStruct(x, ret, t); }
CATransform3D rotation(float a, float rx, float ry, float rz) {
    const float len = std::sqrt(rx * rx + ry * ry + rz * rz);
    if (len == 0) return identity3D();
    rx /= len; ry /= len; rz /= len;
    const float c = std::cos(a), s = std::sin(a), t = 1 - c;
    CATransform3D m = identity3D();
    m.m11 = t * rx * rx + c;      m.m12 = t * rx * ry + s * rz; m.m13 = t * rx * rz - s * ry;
    m.m21 = t * rx * ry - s * rz; m.m22 = t * ry * ry + c;      m.m23 = t * ry * rz + s * rx;
    m.m31 = t * rx * rz + s * ry; m.m32 = t * ry * rz - s * rx; m.m33 = t * rz * rz + c;
    return m;
}
CATransform3D concat(const CATransform3D& a, const CATransform3D& b) {
    const float* A = &a.m11;
    const float* B = &b.m11;
    CATransform3D r;
    float* R = &r.m11;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += A[i * 4 + k] * B[k * 4 + j];
            R[i * 4 + j] = s;
        }
    return r;
}
void t3_makeRotation(Ctx& x, GuestPtr ret, float a, float rx, float ry, float rz) { CG::writeStruct(x, ret, rotation(a, rx, ry, rz)); }
void t3_translate(Ctx& x, GuestPtr ret, CATransform3D t, float tx, float ty, float tz) {
    CATransform3D m = identity3D(); m.m41 = tx; m.m42 = ty; m.m43 = tz;
    CG::writeStruct(x, ret, concat(m, t));
}
void t3_scale(Ctx& x, GuestPtr ret, CATransform3D t, float sx, float sy, float sz) {
    CATransform3D m = identity3D(); m.m11 = sx; m.m22 = sy; m.m33 = sz;
    CG::writeStruct(x, ret, concat(m, t));
}
void t3_rotate(Ctx& x, GuestPtr ret, CATransform3D t, float a, float rx, float ry, float rz) { CG::writeStruct(x, ret, concat(rotation(a, rx, ry, rz), t)); }
void t3_concat(Ctx& x, GuestPtr ret, CATransform3D a, CATransform3D b) { CG::writeStruct(x, ret, concat(a, b)); }
void t3_invert(Ctx& x, GuestPtr ret, CATransform3D t) {
    // affine part only (sufficient for 2D layer transforms)
    CG::writeStruct(x, ret, from2D(affineOf(t).inverted()));
}
bool t3_isIdentity(Ctx&, CATransform3D t) { const CATransform3D i = identity3D(); return std::memcmp(&t, &i, sizeof t) == 0; }
bool t3_equal(Ctx&, CATransform3D a, CATransform3D b) { return std::memcmp(&a, &b, sizeof a) == 0; }
void t3_makeAffine(Ctx& x, GuestPtr ret, CGAffineTransform a) { CG::writeStruct(x, ret, from2D(CG::toG(a))); }
void t3_getAffine(Ctx& x, GuestPtr ret, CATransform3D t) { CG::writeStruct(x, ret, CG::fromG(affineOf(t))); }
bool t3_isAffine(Ctx&, CATransform3D t) { return t.m13 == 0 && t.m14 == 0 && t.m23 == 0 && t.m24 == 0 && t.m31 == 0 && t.m32 == 0 && t.m33 == 1 && t.m34 == 0 && t.m43 == 0 && t.m44 == 1; }
double ca_mediaTime(Ctx&) {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// ------------------------------------------------------------ CADisplayLink ---

struct LinkData : ObjC::HostObject { id target = 0; SEL sel = 0; int interval = 1; bool paused = false, added = false; double timestamp = 0; };
LinkData& LD(Ctx& x, id l) { return rt(x).host<LinkData>(l); }
id dl_create(Ctx& x, id self, SEL, id target, SEL sel) {
    const id l = rt(x).createInstance(self);
    LinkData& d = LD(x, l);
    d.target = rt(x).retain(x.cpu, target);
    d.sel = sel;
    return autoreleased(x, l);
}
void dl_add(Ctx& x, id self, SEL, id, id) {
    LinkData& d = LD(x, self);
    if (d.added) return;
    d.added = true;
    rt(x).retain(x.cpu, self);   // the run loop retains the link
    ui(x).links.push_back({self, d.target, d.sel, d.interval, d.paused, 0});
}
void dl_invalidate(Ctx& x, id self, SEL) {
    LinkData& d = LD(x, self);
    auto& links = ui(x).links;
    for (auto& l : links) if (l.obj == self) l.obj = 0;   // removed after the frame loop (safe during iteration)
    const id t = d.target;
    d.target = 0;
    if (t) rt(x).release(x.cpu, t);
    if (d.added) { d.added = false; rt(x).release(x.cpu, self); }
}
void dl_remove(Ctx& x, id self, SEL s, id, id) { dl_invalidate(x, self, s); }
void dl_setInterval(Ctx& x, id self, SEL, int32_t n) {
    LD(x, self).interval = std::max(1, n);
    for (auto& l : ui(x).links) if (l.obj == self) l.interval = std::max(1, n);
}
int32_t dl_interval(Ctx& x, id self, SEL) { return LD(x, self).interval; }
void dl_setPaused(Ctx& x, id self, SEL, bool p) {
    LD(x, self).paused = p;
    for (auto& l : ui(x).links) if (l.obj == self) l.paused = p;
}
bool dl_paused(Ctx& x, id self, SEL) { return LD(x, self).paused; }
double dl_timestamp(Ctx& x, id self, SEL) { return LD(x, self).timestamp; }
double dl_duration(Ctx&, id, SEL) { return 1.0 / 60.0; }
void dl_setPreferredFPS(Ctx& x, id self, SEL s, int32_t fps) { dl_setInterval(x, self, s, fps > 0 ? std::max(1, 60 / fps) : 1); }
void dl_dealloc(Ctx& x, id self, SEL) {
    if (LinkData* d = rt(x).hostIf<LinkData>(self)) { const id t = d->target; d->target = 0; if (t) rt(x).release(x.cpu, t); }
    rt(x).disposeInstance(x.cpu, self);
}

// ================================================================ compositor ===

struct Compositor {
    Ctx& x;
    G::Surface& dst;

    // 9-slice / gravity aware contents drawing (layer bounds space → device via m)
    void drawContents(const LayerData& d, const G::Image& img, float imgScale, const G::Affine& m, const G::Clip& clip, float alpha) {
        const G::Rect b = d.bounds;
        const G::Rect cr = d.contentsRect;
        const float iw = float(img.width) * cr.w, ih = float(img.height) * cr.h;
        if (iw <= 0 || ih <= 0 || b.w <= 0 || b.h <= 0) return;
        const bool smooth = !d.magnifyNearest;
        const G::Rect cc = d.contentsCenter;
        const bool nine = d.gravity == kGravityResize && !(cc.x == 0 && cc.y == 0 && cc.w == 1 && cc.h == 1);
        if (nine) {
            // Source slices in pixels, destination slices in points.
            const float sx[4] = {0, cc.x * iw, (cc.x + cc.w) * iw, iw};
            const float sy[4] = {0, cc.y * ih, (cc.y + cc.h) * ih, ih};
            const float capL = sx[1] / imgScale, capR = (iw - sx[2]) / imgScale, capT = sy[1] / imgScale, capB = (ih - sy[2]) / imgScale;
            const float dx[4] = {b.x, b.x + capL, b.maxX() - capR, b.maxX()};
            const float dy[4] = {b.y, b.y + capT, b.maxY() - capB, b.maxY()};
            for (int j = 0; j < 3; ++j)
                for (int i = 0; i < 3; ++i) {
                    const float sw = sx[i + 1] - sx[i], sh = sy[j + 1] - sy[j], dw = dx[i + 1] - dx[i], dh = dy[j + 1] - dy[j];
                    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) continue;
                    const G::Affine im = G::Affine::translate(-(cr.x * float(img.width) + sx[i]), -(cr.y * float(img.height) + sy[j]))
                                             .then(G::Affine::scale(dw / sw, dh / sh)).then(G::Affine::translate(dx[i], dy[j])).then(m);
                    G::Path p;
                    p.addRect({dx[i], dy[j], dw, dh});
                    const G::Clip c2 = G::intersectClip(clip, p.transformed(m), false);
                    G::drawImage(dst, img, im, c2, alpha, smooth);
                }
            return;
        }
        // gravity → destination rect (points)
        const float pw = iw / imgScale, ph = ih / imgScale;
        G::Rect r = b;
        switch (d.gravity) {
            case kGravityResize: break;
            case kGravityAspect: case kGravityAspectFill: {
                const float s = d.gravity == kGravityAspect ? std::min(b.w / pw, b.h / ph) : std::max(b.w / pw, b.h / ph);
                r = {b.x + (b.w - pw * s) / 2, b.y + (b.h - ph * s) / 2, pw * s, ph * s};
                break;
            }
            default: {
                float rx = b.x + (b.w - pw) / 2, ry = b.y + (b.h - ph) / 2;
                // CA gravities are in the (unflipped) layer space where "top" is max Y on macOS; on iOS
                // UIKit maps contentMode top → kCAGravityBottom. We store UIKit's meaning directly.
                switch (d.gravity) {
                    case kGravityTop: ry = b.y; break;
                    case kGravityBottom: ry = b.maxY() - ph; break;
                    case kGravityLeft: rx = b.x; break;
                    case kGravityRight: rx = b.maxX() - pw; break;
                    case kGravityTopLeft: rx = b.x; ry = b.y; break;
                    case kGravityTopRight: rx = b.maxX() - pw; ry = b.y; break;
                    case kGravityBottomLeft: rx = b.x; ry = b.maxY() - ph; break;
                    case kGravityBottomRight: rx = b.maxX() - pw; ry = b.maxY() - ph; break;
                    default: break;
                }
                r = {rx, ry, pw, ph};
            }
        }
        const G::Affine im = G::Affine::translate(-cr.x * float(img.width), -cr.y * float(img.height))
                                 .then(G::Affine::scale(r.w / iw, r.h / ih)).then(G::Affine::translate(r.x, r.y)).then(m);
        const bool needClip = d.gravity == kGravityAspectFill || r.w > b.w || r.h > b.h || !(cr.x == 0 && cr.y == 0 && cr.w == 1 && cr.h == 1);
        if (needClip) {
            G::Path p;
            p.addRect(r.intersect(b));
            G::drawImage(dst, img, im, G::intersectClip(clip, p.transformed(m), false), alpha, smooth);
        } else {
            G::drawImage(dst, img, im, clip, alpha, smooth);
        }
    }

    void render(id layer, const G::Affine& parentToDevice, const G::Clip& clip, float parentOpacity) {
        const LayerData& d = L(x, layer);
        if (d.hidden || d.opacity <= 0.001f) return;
        if (clip.x1 <= clip.x0 || clip.y1 <= clip.y0) return;
        const float alpha = parentOpacity * std::clamp(d.opacity, 0.0f, 1.0f);
        const G::Affine m = layerToSuper(d).then(parentToDevice);
        G::Path shape;
        if (d.cornerRadius > 0) shape.addRoundedRect(d.bounds, d.cornerRadius, d.cornerRadius);
        else shape.addRect(d.bounds);
        const G::Path devShape = shape.transformed(m);

        // background
        if (d.backgroundPattern) {
            const G::Clip c2 = G::intersectClip(clip, devShape, false);
            const G::Image& pat = *d.backgroundPattern;
            const float pw = float(pat.width) / std::max(d.contentsScale, 1.0f), ph = float(pat.height) / std::max(d.contentsScale, 1.0f);
            if (pw > 0 && ph > 0) {
                int guard = 0;
                for (float yy = d.bounds.y; yy < d.bounds.maxY() && guard < 20000; yy += ph)
                    for (float xx = d.bounds.x; xx < d.bounds.maxX() && guard < 20000; xx += pw, ++guard)
                        G::drawImage(dst, pat, G::Affine::scale(pw / float(pat.width), ph / float(pat.height)).then(G::Affine::translate(xx, yy)).then(m), c2, alpha, true);
            }
        } else if (d.background.a > 0) {
            if (d.cornerRadius <= 0 && m.axisAligned()) G::fillRect(dst, m.applyRect(d.bounds), d.background, clip, G::BlendMode::Normal, alpha);
            else G::fillPath(dst, devShape, false, d.background, clip, G::BlendMode::Normal, alpha);
        }

        // contents
        G::ImagePtr img;
        float imgScale = d.contentsScale;
        if (d.externalContents) img = d.externalContents();
        else if (d.contents) { img = CG::imageOf(x, d.contents); }
        else if (d.backing) img = d.backing;
        if (img && img->width > 0) {
            const G::Clip c2 = d.cornerRadius > 0 && d.masksToBounds ? G::intersectClip(clip, devShape, false) : clip;
            drawContents(d, *img, imgScale > 0 ? imgScale : 1, m, c2, alpha);
        }

        // border (inside the bounds)
        if (d.borderWidth > 0 && d.border.a > 0) {
            G::Path ring = shape;
            const float bw = d.borderWidth;
            G::Rect inner{d.bounds.x + bw, d.bounds.y + bw, d.bounds.w - 2 * bw, d.bounds.h - 2 * bw};
            if (inner.w > 0 && inner.h > 0) {
                G::Path in;
                if (d.cornerRadius > bw) in.addRoundedRect(inner, d.cornerRadius - bw, d.cornerRadius - bw);
                else in.addRect(inner);
                ring.subpaths.insert(ring.subpaths.end(), in.subpaths.begin(), in.subpaths.end());
                ring.closed.insert(ring.closed.end(), in.closed.begin(), in.closed.end());
            }
            G::fillPath(dst, ring.transformed(m), true, d.border, clip, G::BlendMode::Normal, alpha);
        }

        if (d.sublayers.empty()) return;
        const G::Clip childClip = d.masksToBounds ? G::intersectClip(clip, devShape, false) : clip;
        G::Affine childBase = m;
        if (std::memcmp(&d.sublayerTransform, &kIdentity, sizeof kIdentity) != 0) {
            const G::Point ap{d.bounds.x + d.anchor.x * d.bounds.w, d.bounds.y + d.anchor.y * d.bounds.h};
            childBase = G::Affine::translate(-ap.x, -ap.y).then(affineOf(d.sublayerTransform)).then(G::Affine::translate(ap.x, ap.y)).then(m);
        }
        // sort by zPosition (stable)
        std::vector<id> order = d.sublayers;
        std::stable_sort(order.begin(), order.end(), [&](id a, id b) { return L(x, a).zPosition < L(x, b).zPosition; });
        for (id s : order) render(s, childBase, childClip, alpha);
    }
    static inline const CATransform3D kIdentity = identity3D();
};

void drawStatusBar(Ctx& x, G::Surface& s) {
    auto& u = ui(x);
    const float h = 20 * u.scale;
    G::Clip all; all.x0 = 0; all.y0 = 0; all.x1 = s.width; all.y1 = s.height;
    G::Rect band;
    switch (u.statusBarOrientation) {
        case 3: band = {0, 0, h, float(s.height)}; break;                    // UIInterfaceOrientationLandscapeLeft: UI top is the device's left edge
        case 4: band = {float(s.width) - h, 0, h, float(s.height)}; break;   // LandscapeRight: UI top is the device's right edge
        case 2: band = {0, float(s.height) - h, float(s.width), h}; break;
        default: band = {0, 0, float(s.width), h}; break;
    }
    G::fillRect(s, band, {0, 0, 0, 1}, all);
}

} // namespace

// =============================================================== exports ===

LayerData& layerData(Ctx& x, id layer) { return rt(x).host<LayerData>(layer); }

id newLayer(Ctx& x, Class cls) {
    if (!cls) cls = ui(x).layer;
    const id l = id(rt(x).send(x.cpu, id(rt(x).send(x.cpu, cls, rt(x).selAlloc)), rt(x).selInit));
    return l;
}

void layerAddSublayer(Ctx& x, id parent, id child, long index) {
    if (!parent || !child || parent == child) return;
    rt(x).retain(x.cpu, child);
    layerRemoveFromSuperlayer(x, child);
    LayerData& p = L(x, parent);
    if (index < 0 || size_t(index) > p.sublayers.size()) p.sublayers.push_back(child);
    else p.sublayers.insert(p.sublayers.begin() + index, child);
    L(x, child).superlayer = parent;
    dirty(x);
}
void layerRemoveFromSuperlayer(Ctx& x, id layer) {
    LayerData& d = L(x, layer);
    if (!d.superlayer) return;
    auto& v = L(x, d.superlayer).sublayers;
    auto it = std::find(v.begin(), v.end(), layer);
    d.superlayer = 0;
    if (it != v.end()) { v.erase(it); rt(x).release(x.cpu, layer); }
    dirty(x);
}
G::Affine layerToSuper(const LayerData& d) {
    const G::Point ap{d.bounds.x + d.anchor.x * d.bounds.w, d.bounds.y + d.anchor.y * d.bounds.h};
    const G::Affine t = affineOf(d.transform);
    if (t.isIdentity()) return G::Affine::translate(d.position.x - ap.x, d.position.y - ap.y);
    return G::Affine::translate(-ap.x, -ap.y).then(t).then(G::Affine::translate(d.position.x, d.position.y));
}
G::Rect layerFrame(const LayerData& d) {
    G::Rect local{d.bounds.x, d.bounds.y, d.bounds.w, d.bounds.h};
    return layerToSuper(d).applyRect(local);
}
void layerSetFrame(Ctx& x, id layer, const G::Rect& f0) {
    LayerData& d = L(x, layer);
    const G::Rect f = f0.standardized();
    const G::Affine t = affineOf(d.transform);
    float w = f.w, h = f.h;
    if (!t.isIdentity()) {
        // Like CA: setting the frame of a transformed layer sets bounds from the inverse-transformed size (approx.)
        const G::Rect r = t.inverted().applyRect({0, 0, f.w, f.h});
        w = r.w; h = r.h;
    }
    const bool sizeChanged = w != d.bounds.w || h != d.bounds.h;
    d.bounds.w = w;
    d.bounds.h = h;
    d.position = {f.x + d.anchor.x * f.w, f.y + d.anchor.y * f.h};
    if (sizeChanged && d.needsDisplayOnBoundsChange) d.needsDisplay = true;
    dirty(x);
}
void setNeedsComposite(Ctx& x) { dirty(x); }

bool compositeFrame(Ctx& x) {
    auto& u = ui(x);
    const int W = int(u.screenW * u.scale), H = int(u.screenH * u.scale);
    G::Image& fb = u.framebuffer;
    if (fb.width != W || fb.height != H) { fb.width = W; fb.height = H; fb.rgba.assign(size_t(W) * size_t(H) * 4, 0); }
    G::Surface s = fb.surface();
    G::clear(s, {0, 0, 0, 1});
    G::Clip all; all.x0 = 0; all.y0 = 0; all.x1 = W; all.y1 = H;
    Compositor c{x, s};
    // windows: by level, then order of appearance
    std::vector<id> wins;
    for (id w : u.windows) if (viewData(x, w).visible) wins.push_back(w);
    std::stable_sort(wins.begin(), wins.end(), [&](id a, id b) { return viewData(x, a).windowLevel < viewData(x, b).windowLevel; });
    const G::Affine screen = G::Affine::scale(u.scale, u.scale);
    for (id w : wins) c.render(viewLayer(x, w), screen, all, 1);
    if (!u.statusBarHidden) drawStatusBar(x, s);
    u.needsComposite = false;
    ++u.frames;
    return true;
}

namespace {
// Run -display on layers that need it (depth first), honouring UIView's
// "only views that override drawRect: get a backing store" rule.
void displayPass(Ctx& x, id layer, IMP baseDrawRect) {
    LayerData& d = L(x, layer);
    if (d.needsDisplay) {
        auto& r = rt(x);
        bool should = true;
        if (d.delegate && r.isKindOf(d.delegate, ui(x).view)) {
            const IMP imp = r.lookup(r.classOf(d.delegate), r.sel("drawRect:"));
            should = imp && imp != baseDrawRect;
            if (!should) d.needsDisplay = false;
        }
        if (should) r.send(x.cpu, layer, "display");
    }
    const std::vector<id> subs = d.sublayers;
    for (id s : subs) displayPass(x, s, baseDrawRect);
}
}

void runFrame(Ctx& x, double now) {
    auto& u = ui(x);
    deliverPendingInput(x);
    // display links
    if (!u.links.empty()) {
        const size_t n = u.links.size();
        for (size_t i = 0; i < n && i < u.links.size(); ++i) {
            UIState::Link l = u.links[i];
            if (!l.obj || l.paused) continue;
            if (++u.links[i].counter < l.interval) continue;
            u.links[i].counter = 0;
            if (LinkData* ld = rt(x).hostIf<LinkData>(l.obj)) {
                ld->timestamp = ca_mediaTime(x);
                if (ld->target) rt(x).send(x.cpu, ld->target, ld->sel, {l.obj});
            }
        }
        u.links.erase(std::remove_if(u.links.begin(), u.links.end(), [](const UIState::Link& l) { return !l.obj; }), u.links.end());
    }
    layoutPass(x);
    const IMP baseDrawRect = rt(x).lookup(u.view, rt(x).sel("drawRect:"));
    for (id w : std::vector<id>(u.windows)) if (viewData(x, w).visible) displayPass(x, viewLayer(x, w), baseDrawRect);
    bool external = false;
    (void)now;
    if (u.needsComposite || external) {
        compositeFrame(x);
        if (x.proc.config().present) x.proc.config().present(u.framebuffer.rgba.data(), u.framebuffer.width, u.framebuffer.height);
    }
}

namespace {
void l_renderInContextImpl(Ctx& x, id self, id ctx) {
    CG::ContextData* cd = CG::context(x, ctx);
    if (!cd) return;
    Compositor c{x, cd->surface};
    // Render with the layer's own bounds at the context's current CTM (layer space = user space).
    LayerData& d = L(x, self);
    const G::Affine toUser = layerToSuper(d).inverted();
    c.render(self, toUser.then(cd->gs().ctm), cd->gs().clip, cd->gs().alpha);
}
}
namespace { void l_renderInContext(Ctx& x, id self, SEL, id ctx) { l_renderInContextImpl(x, self, ctx); } }

void registerQuartzCore(Process& p, ObjC::Runtime& r) {
    auto& u = ui(p);
    ObjC::ClassBuilder layer = r.defineClass("CALayer", "NSObject");
    u.layer = layer.cls();
    const std::string R = "{CGRect={CGPoint=ff}{CGSize=ff}}", P = "{CGPoint=ff}", T3 = "{CATransform3D=ffffffffffffffff}";
    layer.cm<&lc_layer>("layer");
    layer.m<&l_init>("init").m<&l_initWithLayer>("initWithLayer:").m<&l_dealloc>("dealloc", "v@:")
         .m<&l_bounds>("bounds", R + "@:").m<&l_setBounds>("setBounds:", "v@:" + R)
         .m<&l_position>("position", P + "@:").m<&l_setPosition>("setPosition:", "v@:" + P)
         .m<&l_anchor>("anchorPoint", P + "@:").m<&l_setAnchor>("setAnchorPoint:", "v@:" + P)
         .m<&l_frame>("frame", R + "@:").m<&l_setFrame>("setFrame:", "v@:" + R)
         .m<&l_zPosition>("zPosition", "f@:").m<&l_setZPosition>("setZPosition:", "v@:f")
         .m<&l_transform>("transform", T3 + "@:").m<&l_setTransform>("setTransform:", "v@:" + T3)
         .m<&l_sublayerTransform>("sublayerTransform", T3 + "@:").m<&l_setSublayerTransform>("setSublayerTransform:", "v@:" + T3)
         .m<&l_affine>("affineTransform", "{CGAffineTransform=ffffff}@:").m<&l_setAffine>("setAffineTransform:", "v@:{CGAffineTransform=ffffff}")
         .m<&l_hidden>("isHidden", "c@:").m<&l_setHidden>("setHidden:", "v@:c")
         .m<&l_opacity>("opacity", "f@:").m<&l_setOpacity>("setOpacity:", "v@:f")
         .m<&l_masks>("masksToBounds", "c@:").m<&l_setMasks>("setMasksToBounds:", "v@:c")
         .m<&l_opaque>("isOpaque", "c@:").m<&l_setOpaque>("setOpaque:", "v@:c")
         .m<&l_cornerRadius>("cornerRadius", "f@:").m<&l_setCornerRadius>("setCornerRadius:", "v@:f")
         .m<&l_borderWidth>("borderWidth", "f@:").m<&l_setBorderWidth>("setBorderWidth:", "v@:f")
         .m<&l_borderColor>("borderColor", "^{CGColor=}@:").m<&l_setBorderColor>("setBorderColor:", "v@:^{CGColor=}")
         .m<&l_backgroundColor>("backgroundColor", "^{CGColor=}@:").m<&l_setBackgroundColor>("setBackgroundColor:", "v@:^{CGColor=}")
         .m<&l_contents>("contents").m<&l_setContents>("setContents:", "v@:@")
         .m<&l_contentsRect>("contentsRect", R + "@:").m<&l_setContentsRect>("setContentsRect:", "v@:" + R)
         .m<&l_contentsCenter>("contentsCenter", R + "@:").m<&l_setContentsCenter>("setContentsCenter:", "v@:" + R)
         .m<&l_gravity>("contentsGravity").m<&l_setGravity>("setContentsGravity:", "v@:@")
         .m<&l_contentsScale>("contentsScale", "f@:").m<&l_setContentsScale>("setContentsScale:", "v@:f")
         .m<&l_contentsScale>("rasterizationScale", "f@:").m<&l_ignoreFloat>("setRasterizationScale:", "v@:f")
         .m<&l_filter>("magnificationFilter").m<&l_setFilter>("setMagnificationFilter:", "v@:@")
         .m<&l_filter>("minificationFilter").m<&l_ignoreObj>("setMinificationFilter:", "v@:@")
         .m<&l_delegate>("delegate").m<&l_setDelegate>("setDelegate:", "v@:@")
         .m<&l_superlayer>("superlayer").m<&l_sublayers>("sublayers").m<&l_setSublayers>("setSublayers:", "v@:@")
         .m<&l_addSublayer>("addSublayer:", "v@:@").m<&l_insertAt>("insertSublayer:atIndex:", "v@:@I")
         .m<&l_insertAbove>("insertSublayer:above:", "v@:@@").m<&l_insertBelow>("insertSublayer:below:", "v@:@@")
         .m<&l_replace>("replaceSublayer:with:", "v@:@@").m<&l_removeFromSuperlayer>("removeFromSuperlayer", "v@:")
         .m<&l_setNeedsDisplay>("setNeedsDisplay", "v@:").m<&l_setNeedsDisplayInRect>("setNeedsDisplayInRect:", "v@:" + R)
         .m<&l_needsDisplay>("needsDisplay", "c@:").m<&l_display>("display", "v@:").m<&l_displayIfNeeded>("displayIfNeeded", "v@:")
         .m<&l_needsDisplayOnBounds>("needsDisplayOnBoundsChange", "c@:").m<&l_setNeedsDisplayOnBounds>("setNeedsDisplayOnBoundsChange:", "v@:c")
         .m<&l_drawInContext>("drawInContext:", "v@:^{CGContext=}").m<&l_renderInContext>("renderInContext:", "v@:^{CGContext=}")
         .m<&l_setNeedsLayout>("setNeedsLayout", "v@:").m<&l_layoutIfNeeded>("layoutIfNeeded", "v@:")
         .m<&l_layoutSublayers>("layoutSublayers", "v@:")
         .m<&l_convertPointTo>("convertPoint:toLayer:", P + "@:" + P + "@").m<&l_convertPointFrom>("convertPoint:fromLayer:", P + "@:" + P + "@")
         .m<&l_convertRectTo>("convertRect:toLayer:", R + "@:" + R + "@").m<&l_convertRectFrom>("convertRect:fromLayer:", R + "@:" + R + "@")
         .m<&l_containsPoint>("containsPoint:", "c@:" + P).m<&l_hitTest>("hitTest:", "@@:" + P)
         .m<&l_name>("name").m<&l_setName>("setName:", "v@:@").m<&l_mask>("mask").m<&l_setMask>("setMask:", "v@:@")
         .m<&l_self>("presentationLayer").m<&l_self>("modelLayer")
         .m<&l_addAnimation>("addAnimation:forKey:", "v@:@@").m<&l_removeAnimation>("removeAnimationForKey:", "v@:@")
         .m<&l_removeAll>("removeAllAnimations", "v@:").m<&l_animationForKey>("animationForKey:").m<&l_animationKeys>("animationKeys")
         .m<&l_ignoreObj>("setShadowColor:", "v@:^{CGColor=}").m<&l_ignoreFloat>("setShadowOpacity:", "v@:f")
         .m<&l_ignoreFloat>("setShadowRadius:", "v@:f").m<&l_ignoreSize>("setShadowOffset:", "v@:{CGSize=ff}")
         .m<&l_ignoreObj>("setShadowPath:", "v@:^{CGPath=}").m<&l_ignoreBool>("setShouldRasterize:", "v@:c")
         .m<&l_false>("shouldRasterize", "c@:").m<&l_zero>("shadowOpacity", "f@:").m<&l_ignoreObj>("setActions:", "v@:@")
         .m<&l_ignoreObj>("setStyle:", "v@:@").m<&l_ignoreBool>("setAllowsEdgeAntialiasing:", "v@:c")
         .m<&l_ignoreBool>("setAllowsGroupOpacity:", "v@:c").m<&l_ignoreObj>("setCompositingFilter:", "v@:@")
         .m<&l_doubleSided>("isDoubleSided", "c@:").m<&l_setDoubleSided>("setDoubleSided:", "v@:c")
         .m<&l_flipped>("isGeometryFlipped", "c@:").m<&l_setFlipped>("setGeometryFlipped:", "v@:c");

    for (const char* n : {"CAAnimation", "CAPropertyAnimation", "CABasicAnimation", "CAKeyframeAnimation", "CAAnimationGroup", "CATransition"}) {
        const std::string name = n;
        const char* super = name == "CAAnimation" ? "NSObject" : (name == "CABasicAnimation" || name == "CAKeyframeAnimation") ? "CAPropertyAnimation" : "CAAnimation";
        ObjC::ClassBuilder b = r.defineClass(n, super);
        if (name == "CAAnimation") {
            defineBag(b);
            b.cm<&anim_animation>("animation").m<&anim_copy>("copyWithZone:").m<&anim_dealloc>("dealloc", "v@:");
        }
        if (name == "CAPropertyAnimation") b.cm<&anim_withKeyPath>("animationWithKeyPath:");
    }
    ObjC::ClassBuilder tf = r.defineClass("CAMediaTimingFunction", "NSObject");
    tf.cm<&tf_withName>("functionWithName:").cm<&tf_withControlPoints>("functionWithControlPoints::::");
    for (const char* n : {"kCAMediaTimingFunctionLinear", "kCAMediaTimingFunctionEaseIn", "kCAMediaTimingFunctionEaseOut",
                          "kCAMediaTimingFunctionEaseInEaseOut", "kCAMediaTimingFunctionDefault", "kCAFillModeForwards",
                          "kCAFillModeBackwards", "kCAFillModeBoth", "kCAFillModeRemoved", "kCATransitionFade", "kCATransitionPush",
                          "kCATransitionMoveIn", "kCATransitionReveal", "kCATransitionFromLeft", "kCATransitionFromRight",
                          "kCATransitionFromTop", "kCATransitionFromBottom", "kCAAnimationLinear", "kCAAnimationDiscrete",
                          "kCAAnimationPaced", "kCAAnimationCubic", "kCAAnimationRotateAuto", "kCAFilterLinear", "kCAFilterNearest",
                          "kCAFilterTrilinear", "kCAOnOrderIn", "kCAOnOrderOut", "kCATransition", "kCAGravityResize",
                          "kCAGravityResizeAspect", "kCAGravityResizeAspectFill", "kCAGravityCenter", "kCAGravityTop",
                          "kCAGravityBottom", "kCAGravityLeft", "kCAGravityRight", "kCAGravityTopLeft", "kCAGravityTopRight",
                          "kCAGravityBottomLeft", "kCAGravityBottomRight"}) {
        std::string v = n + 1;   // "CA…"; values match CA's strings for the ones that matter
        static const std::pair<const char*, const char*> vals[] = {
            {"kCAFilterLinear", "linear"}, {"kCAFilterNearest", "nearest"}, {"kCAFilterTrilinear", "trilinear"},
            {"kCAGravityResize", "resize"}, {"kCAGravityResizeAspect", "resizeAspect"}, {"kCAGravityResizeAspectFill", "resizeAspectFill"},
            {"kCAGravityCenter", "center"}, {"kCAGravityTop", "top"}, {"kCAGravityBottom", "bottom"}, {"kCAGravityLeft", "left"},
            {"kCAGravityRight", "right"}, {"kCAGravityTopLeft", "topLeft"}, {"kCAGravityTopRight", "topRight"},
            {"kCAGravityBottomLeft", "bottomLeft"}, {"kCAGravityBottomRight", "bottomRight"}, {"kCAFillModeForwards", "forwards"},
            {"kCAFillModeBackwards", "backwards"}, {"kCAFillModeBoth", "both"}, {"kCAFillModeRemoved", "removed"},
            {"kCATransitionFade", "fade"}, {"kCATransitionPush", "push"}, {"kCATransitionMoveIn", "moveIn"}, {"kCATransitionReveal", "reveal"},
            {"kCATransitionFromLeft", "fromLeft"}, {"kCATransitionFromRight", "fromRight"}, {"kCATransitionFromTop", "fromTop"},
            {"kCATransitionFromBottom", "fromBottom"}, {"kCAMediaTimingFunctionLinear", "linear"}, {"kCAMediaTimingFunctionEaseIn", "easeIn"},
            {"kCAMediaTimingFunctionEaseOut", "easeOut"}, {"kCAMediaTimingFunctionEaseInEaseOut", "easeInEaseOut"},
            {"kCAMediaTimingFunctionDefault", "default"},
        };
        for (auto& [k, val] : vals) if (std::string(n) == k) v = val;
        const uint32_t cell = p.defineDataBlock(std::string("_") + n, 4);
        p.mem().write<uint32_t>(cell, Foundation::constString(p, v));
    }

    ObjC::ClassBuilder tx = r.defineClass("CATransaction", "NSObject");
    tx.cm<&tx_noop>("begin", "v@:").cm<&tx_noop>("commit", "v@:").cm<&tx_noop>("flush", "v@:").cm<&tx_noop>("lock", "v@:")
      .cm<&tx_noop>("unlock", "v@:").cm<&tx_setDisable>("setDisableActions:", "v@:c").cm<&tx_disable>("disableActions", "c@:")
      .cm<&tx_setDuration>("setAnimationDuration:", "v@:d").cm<&tx_duration>("animationDuration", "d@:")
      .cm<&tx_setCompletion>("setCompletionBlock:", "v@:@?").cm<&tx_setValue>("setValue:forKey:", "v@:@@")
      .cm<&tx_value>("valueForKey:").cm<&tx_setTimingFunction>("setAnimationTimingFunction:", "v@:@");

    ObjC::ClassBuilder dl = r.defineClass("CADisplayLink", "NSObject");
    u.displayLink = dl.cls();
    dl.cm<&dl_create>("displayLinkWithTarget:selector:");
    dl.m<&dl_add>("addToRunLoop:forMode:", "v@:@@").m<&dl_remove>("removeFromRunLoop:forMode:", "v@:@@")
      .m<&dl_invalidate>("invalidate", "v@:").m<&dl_setInterval>("setFrameInterval:", "v@:i").m<&dl_interval>("frameInterval", "i@:")
      .m<&dl_setPaused>("setPaused:", "v@:c").m<&dl_paused>("isPaused", "c@:").m<&dl_timestamp>("timestamp", "d@:")
      .m<&dl_duration>("duration", "d@:").m<&dl_setPreferredFPS>("setPreferredFramesPerSecond:", "v@:i")
      .m<&dl_dealloc>("dealloc", "v@:");

    p.def<&t3_makeTranslation>("_CATransform3DMakeTranslation");
    p.def<&t3_makeScale>("_CATransform3DMakeScale");
    p.def<&t3_makeRotation>("_CATransform3DMakeRotation");
    p.def<&t3_translate>("_CATransform3DTranslate");
    p.def<&t3_scale>("_CATransform3DScale");
    p.def<&t3_rotate>("_CATransform3DRotate");
    p.def<&t3_concat>("_CATransform3DConcat");
    p.def<&t3_invert>("_CATransform3DInvert");
    p.def<&t3_isIdentity>("_CATransform3DIsIdentity");
    p.def<&t3_equal>("_CATransform3DEqualToTransform");
    p.def<&t3_makeAffine>("_CATransform3DMakeAffineTransform");
    p.def<&t3_getAffine>("_CATransform3DGetAffineTransform");
    p.def<&t3_isAffine>("_CATransform3DIsAffine");
    p.def<&ca_mediaTime>("_CACurrentMediaTime");
    {
        const uint32_t idt = p.defineDataBlock("_CATransform3DIdentity", 64);
        const CATransform3D i = identity3D();
        std::memcpy(p.mem().host(idt), &i, sizeof i);
    }
}

} // namespace Hydrogen::Runtime::UIKit
