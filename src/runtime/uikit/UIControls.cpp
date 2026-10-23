// HydrogenVM — UIControl (target/action, touch tracking), UIButton, UILabel,
// UIImageView, UIActivityIndicatorView, and UIAlertView / UIActionSheet
// (drawn with our own views in an alert-level window, so they are tappable).
#include "UIKit.hpp"

namespace Hydrogen::Runtime::UIKit {

using Foundation::autoreleased;
using Foundation::makeString;
using Foundation::utf8;

namespace {

const std::string kR = "{CGRect={CGPoint=ff}{CGSize=ff}}", kP = "{CGPoint=ff}", kS = "{CGSize=ff}", kE = "{UIEdgeInsets=ffff}";

G::Rect boundsOf(Ctx& x, id v) { return layerData(x, viewLayer(x, v)).bounds; }
id allocInit(Ctx& x, Class cls) { return id(rt(x).send(x.cpu, id(rt(x).send(x.cpu, cls, rt(x).selAlloc)), rt(x).selInit)); }
id allocFrame(Ctx& x, Class cls, const G::Rect& f) { return id(sendRect(x, id(rt(x).send(x.cpu, cls, rt(x).selAlloc)), rt(x).sel("initWithFrame:"), f)); }
id decode(Ctx& x, id coder, const char* key) { return id(rt(x).send(x.cpu, coder, "decodeObjectForKey:", {makeString(x, key)})); }
bool has(Ctx& x, id coder, const char* key) { return sendBool(x, coder, "containsValueForKey:", {makeString(x, key)}); }
int32_t decodeInt(Ctx& x, id coder, const char* key) { return int32_t(rt(x).send(x.cpu, coder, "decodeIntForKey:", {makeString(x, key)})); }
float decodeFloat(Ctx& x, id coder, const char* key) { return bitsf(uint32_t(rt(x).send(x.cpu, coder, "decodeFloatForKey:", {makeString(x, key)}))); }
bool decodeBool(Ctx& x, id coder, const char* key) { return sendBool(x, coder, "decodeBoolForKey:", {makeString(x, key)}); }
void setRetained(Ctx& x, id& slot, id v) { if (v) rt(x).retain(x.cpu, v); const id o = slot; slot = v; if (o) rt(x).release(x.cpu, o); }

// =============================================================== UIControl ===

struct Target { id target; SEL action; uint32_t events; };
struct ControlData : ObjC::HostObject {
    std::vector<Target> targets;
    bool enabled = true, selected = false, highlighted = false, tracking = false, touchInside = false;
    int32_t hAlign = 0, vAlign = 0;
};
ControlData& CD(Ctx& x, id c) { return rt(x).host<ControlData>(c); }

enum : uint32_t {
    kTouchDown = 1u << 0, kTouchDownRepeat = 1u << 1, kTouchDragInside = 1u << 2, kTouchDragOutside = 1u << 3, kTouchDragEnter = 1u << 4,
    kTouchDragExit = 1u << 5, kTouchUpInside = 1u << 6, kTouchUpOutside = 1u << 7, kTouchCancel = 1u << 8, kValueChanged = 1u << 12,
};

void stateChanged(Ctx& x, id self) {
    rt(x).send(x.cpu, self, "setNeedsLayout");
    rt(x).send(x.cpu, self, "setNeedsDisplay");
}
void c_addTarget(Ctx& x, id self, SEL, id target, SEL action, uint32_t events) {
    auto& t = CD(x, self).targets;
    for (auto& e : t) if (e.target == target && e.action == action) { e.events |= events; return; }
    t.push_back({target, action, events});
}
void c_removeTarget(Ctx& x, id self, SEL, id target, SEL action, uint32_t events) {
    auto& t = CD(x, self).targets;
    for (auto& e : t) if ((!target || e.target == target) && (!action || e.action == action)) e.events &= ~events;
    t.erase(std::remove_if(t.begin(), t.end(), [](const Target& e) { return e.events == 0; }), t.end());
}
id c_allTargets(Ctx& x, id self, SEL) {
    std::vector<id> v;
    for (auto& e : CD(x, self).targets) if (e.target) v.push_back(e.target);
    return Foundation::makeSet(x, v);
}
uint32_t c_allEvents(Ctx& x, id self, SEL) { uint32_t m = 0; for (auto& e : CD(x, self).targets) m |= e.events; return m; }
id c_actionsFor(Ctx& x, id self, SEL, id target, uint32_t event) {
    std::vector<id> v;
    for (auto& e : CD(x, self).targets) if (e.target == target && (e.events & event)) v.push_back(makeString(x, rt(x).selName(e.action)));
    return v.empty() ? 0 : Foundation::makeArray(x, v);
}
void c_sendAction(Ctx& x, id self, SEL, SEL action, id target, id event) {
    rt(x).send(x.cpu, ui(x).app, "sendAction:to:from:forEvent:", {action, target, self, event});
}
void sendActions(Ctx& x, id self, uint32_t events, id event) {
    rt(x).retain(x.cpu, self);
    const std::vector<Target> t = CD(x, self).targets;
    for (const auto& e : t)
        if (e.events & events) rt(x).send(x.cpu, self, "sendAction:to:forEvent:", {e.action, e.target, event});
    rt(x).release(x.cpu, self);
}
void c_sendActionsForEvents(Ctx& x, id self, SEL, uint32_t events) { sendActions(x, self, events, 0); }
bool c_enabled(Ctx& x, id self, SEL) { return CD(x, self).enabled; }
void c_setEnabled(Ctx& x, id self, SEL, bool b) { CD(x, self).enabled = b; stateChanged(x, self); }
bool c_selected(Ctx& x, id self, SEL) { return CD(x, self).selected; }
void c_setSelected(Ctx& x, id self, SEL, bool b) { CD(x, self).selected = b; stateChanged(x, self); }
bool c_highlighted(Ctx& x, id self, SEL) { return CD(x, self).highlighted; }
void c_setHighlighted(Ctx& x, id self, SEL, bool b) { if (CD(x, self).highlighted != b) { CD(x, self).highlighted = b; stateChanged(x, self); } }
uint32_t c_state(Ctx& x, id self, SEL) {
    ControlData& d = CD(x, self);
    return (d.highlighted ? 1u : 0u) | (d.enabled ? 0u : 2u) | (d.selected ? 4u : 0u);
}
bool c_tracking(Ctx& x, id self, SEL) { return CD(x, self).tracking; }
bool c_touchInside(Ctx& x, id self, SEL) { return CD(x, self).touchInside; }
int32_t c_hAlign(Ctx& x, id self, SEL) { return CD(x, self).hAlign; }
void c_setHAlign(Ctx& x, id self, SEL, int32_t a) { CD(x, self).hAlign = a; stateChanged(x, self); }
int32_t c_vAlign(Ctx& x, id self, SEL) { return CD(x, self).vAlign; }
void c_setVAlign(Ctx& x, id self, SEL, int32_t a) { CD(x, self).vAlign = a; stateChanged(x, self); }
bool c_beginTracking(Ctx&, id, SEL, id, id) { return true; }
bool c_continueTracking(Ctx&, id, SEL, id, id) { return true; }
void c_endTracking(Ctx&, id, SEL, id, id) {}
void c_cancelTracking(Ctx&, id, SEL, id) {}

id anyTouch(Ctx& x, id touches) { return id(rt(x).send(x.cpu, touches, "anyObject")); }
G::Point touchPoint(Ctx& x, id touch, id view) {
    const uint32_t tmp = x.proc.heap().allocZeroed(8);
    const IMP imp = rt(x).lookupForSend(rt(x).classOf(touch), rt(x).sel("locationInView:"));
    x.proc.callGuest(x.cpu, imp, {tmp, touch, rt(x).sel("locationInView:"), view});
    const CGPoint p = CG::readStruct<CGPoint>(x, tmp);
    x.proc.heap().free(tmp);
    return CG::toG(p);
}
bool insideWithSlop(Ctx& x, id self, G::Point p) {
    G::Rect b = boundsOf(x, self);
    b = {b.x - 70, b.y - 70, b.w + 140, b.h + 140};   // UIKit's generous touch-up slop
    return b.contains(p);
}
void c_touchesBegan(Ctx& x, id self, SEL, id touches, id event) {
    ControlData& d = CD(x, self);
    if (!d.enabled) return;
    const id t = anyTouch(x, touches);
    if (!sendBool(x, self, "beginTrackingWithTouch:withEvent:", {t, event})) return;
    d.tracking = true;
    d.touchInside = true;
    rt(x).send(x.cpu, self, "setHighlighted:", {1});
    const int32_t taps = int32_t(rt(x).send(x.cpu, t, "tapCount"));
    sendActions(x, self, taps > 1 ? (kTouchDown | kTouchDownRepeat) : kTouchDown, event);
}
void c_touchesMoved(Ctx& x, id self, SEL, id touches, id event) {
    ControlData& d = CD(x, self);
    if (!d.tracking) return;
    const id t = anyTouch(x, touches);
    const bool inside = insideWithSlop(x, self, touchPoint(x, t, self));
    if (inside != d.touchInside) sendActions(x, self, inside ? kTouchDragEnter : kTouchDragExit, event);
    d.touchInside = inside;
    rt(x).send(x.cpu, self, "setHighlighted:", {inside ? 1u : 0u});
    if (!sendBool(x, self, "continueTrackingWithTouch:withEvent:", {t, event})) { d.tracking = false; return; }
    sendActions(x, self, inside ? kTouchDragInside : kTouchDragOutside, event);
}
void c_touchesEnded(Ctx& x, id self, SEL, id touches, id event) {
    rt(x).retain(x.cpu, self);
    ControlData& d = CD(x, self);
    if (!d.tracking) { rt(x).release(x.cpu, self); return; }
    const id t = anyTouch(x, touches);
    const bool inside = insideWithSlop(x, self, touchPoint(x, t, self));
    d.tracking = false;
    d.touchInside = inside;
    rt(x).send(x.cpu, self, "setHighlighted:", {0});
    rt(x).send(x.cpu, self, "endTrackingWithTouch:withEvent:", {t, event});
    sendActions(x, self, inside ? kTouchUpInside : kTouchUpOutside, event);
    rt(x).release(x.cpu, self);
}
void c_touchesCancelled(Ctx& x, id self, SEL, id, id event) {
    ControlData& d = CD(x, self);
    if (!d.tracking) return;
    d.tracking = false;
    rt(x).send(x.cpu, self, "setHighlighted:", {0});
    rt(x).send(x.cpu, self, "cancelTrackingWithEvent:", {event});
    sendActions(x, self, kTouchCancel, event);
}
id c_initWithCoder(Ctx& x, id self, SEL s, id coder) {
    const id me = id(rt(x).sendSuper(x.cpu, self, ui(x).view, s, {coder}));
    if (!me) return 0;
    ControlData& d = CD(x, me);
    if (has(x, coder, "UIDisabled")) d.enabled = !decodeBool(x, coder, "UIDisabled");
    if (has(x, coder, "UISelected")) d.selected = decodeBool(x, coder, "UISelected");
    if (has(x, coder, "UIHighlighted")) d.highlighted = decodeBool(x, coder, "UIHighlighted");
    if (has(x, coder, "UIContentHorizontalAlignment")) d.hAlign = decodeInt(x, coder, "UIContentHorizontalAlignment");
    if (has(x, coder, "UIContentVerticalAlignment")) d.vAlign = decodeInt(x, coder, "UIContentVerticalAlignment");
    return me;
}

// ================================================================= UILabel ===

struct LabelData : ObjC::HostObject {
    id text = 0, font = 0, textColor = 0, shadowColor = 0, highlightedColor = 0;   // retained
    int32_t alignment = 0, lines = 1, lineBreak = 4, baseline = 0;
    CGSize shadowOffset{0, -1};
    bool highlighted = false, enabled = true, adjusts = false;
    float minFontSize = 0, minScale = 0, preferredWidth = 0;
};
LabelData& LD(Ctx& x, id l) { return rt(x).host<LabelData>(l); }
void initLabelDefaults(Ctx& x, id self) {
    LabelData& d = LD(x, self);
    if (!d.font) d.font = rt(x).retain(x.cpu, systemFont(x, 17));
    if (!d.textColor) d.textColor = rt(x).retain(x.cpu, makeUIColor(x, {0, 0, 0, 1}));
    viewData(x, self).userInteraction = false;
    layerData(x, viewLayer(x, self)).needsDisplay = true;
}
id l_initWithFrame(Ctx& x, id self, SEL s, CGRect f) {
    const id me = id(rt(x).sendSuper(x.cpu, self, ui(x).view, s, {fbits(f.origin.x), fbits(f.origin.y), fbits(f.size.width), fbits(f.size.height)}));
    if (!me) return 0;
    initLabelDefaults(x, me);
    // iOS 6 labels are opaque white by default
    rt(x).send(x.cpu, me, "setBackgroundColor:", {makeUIColor(x, {1, 1, 1, 1})});
    rt(x).send(x.cpu, me, "setOpaque:", {1});
    return me;
}
id l_initWithCoder(Ctx& x, id self, SEL s, id coder) {
    const id me = id(rt(x).sendSuper(x.cpu, self, ui(x).view, s, {coder}));
    if (!me) return 0;
    LabelData& d = LD(x, me);
    setRetained(x, d.text, decode(x, coder, "UIText"));
    setRetained(x, d.font, decode(x, coder, "UIFont"));
    setRetained(x, d.textColor, decode(x, coder, "UITextColor"));
    setRetained(x, d.shadowColor, decode(x, coder, "UIShadowColor"));
    setRetained(x, d.highlightedColor, decode(x, coder, "UIHighlightedColor"));
    if (has(x, coder, "UITextAlignment")) d.alignment = decodeInt(x, coder, "UITextAlignment");
    if (has(x, coder, "UINumberOfLines")) d.lines = decodeInt(x, coder, "UINumberOfLines");
    if (has(x, coder, "UILineBreakMode")) d.lineBreak = decodeInt(x, coder, "UILineBreakMode");
    if (has(x, coder, "UIAdjustsFontSizeToFit")) d.adjusts = decodeBool(x, coder, "UIAdjustsFontSizeToFit");
    if (has(x, coder, "UIMinimumFontSize")) d.minFontSize = decodeFloat(x, coder, "UIMinimumFontSize");
    if (has(x, coder, "UIEnabled")) d.enabled = decodeBool(x, coder, "UIEnabled");
    if (has(x, coder, "UIShadowOffset")) {
        const G::Size so = getSize(x, coder, rt(x).sel("decodeCGSizeForKey:"), {makeString(x, "UIShadowOffset")});
        d.shadowOffset = {so.w, so.h};
    }
    initLabelDefaults(x, me);
    return me;
}
void l_dealloc(Ctx& x, id self, SEL s) {
    if (LabelData* d = rt(x).hostIf<LabelData>(self))
        for (id* o : {&d->text, &d->font, &d->textColor, &d->shadowColor, &d->highlightedColor}) { if (*o) rt(x).release(x.cpu, *o); *o = 0; }
    rt(x).sendSuper(x.cpu, self, ui(x).view, s);
}
void relabel(Ctx& x, id self) { rt(x).send(x.cpu, self, "setNeedsDisplay"); }
id l_text(Ctx& x, id self, SEL) { return LD(x, self).text; }
void l_setText(Ctx& x, id self, SEL, id t) {
    LabelData& d = LD(x, self);
    const id n = t ? id(rt(x).send(x.cpu, t, "copy")) : 0;
    const id o = d.text;
    d.text = n;
    if (o) rt(x).release(x.cpu, o);
    relabel(x, self);
}
id l_attributedText(Ctx& x, id self, SEL) { return LD(x, self).text; }
void l_setAttributedText(Ctx& x, id self, SEL s, id t) {
    const id str = t && rt(x).respondsTo(t, rt(x).sel("string")) ? id(rt(x).send(x.cpu, t, "string")) : t;
    l_setText(x, self, s, str);
}
id l_font(Ctx& x, id self, SEL) { return LD(x, self).font; }
void l_setFont(Ctx& x, id self, SEL, id f) { setRetained(x, LD(x, self).font, f ? f : systemFont(x, 17)); relabel(x, self); }
id l_textColor(Ctx& x, id self, SEL) { return LD(x, self).textColor; }
void l_setTextColor(Ctx& x, id self, SEL, id c) { setRetained(x, LD(x, self).textColor, c); relabel(x, self); }
id l_shadowColor(Ctx& x, id self, SEL) { return LD(x, self).shadowColor; }
void l_setShadowColor(Ctx& x, id self, SEL, id c) { setRetained(x, LD(x, self).shadowColor, c); relabel(x, self); }
void l_shadowOffset(Ctx& x, GuestPtr ret, id self, SEL) { CG::writeStruct(x, ret, LD(x, self).shadowOffset); }
void l_setShadowOffset(Ctx& x, id self, SEL, CGSize s) { LD(x, self).shadowOffset = s; relabel(x, self); }
id l_hlColor(Ctx& x, id self, SEL) { return LD(x, self).highlightedColor; }
void l_setHlColor(Ctx& x, id self, SEL, id c) { setRetained(x, LD(x, self).highlightedColor, c); relabel(x, self); }
int32_t l_alignment(Ctx& x, id self, SEL) { return LD(x, self).alignment; }
void l_setAlignment(Ctx& x, id self, SEL, int32_t a) { LD(x, self).alignment = a; relabel(x, self); }
int32_t l_lines(Ctx& x, id self, SEL) { return LD(x, self).lines; }
void l_setLines(Ctx& x, id self, SEL, int32_t n) { LD(x, self).lines = n; relabel(x, self); }
int32_t l_lineBreak(Ctx& x, id self, SEL) { return LD(x, self).lineBreak; }
void l_setLineBreak(Ctx& x, id self, SEL, int32_t m) { LD(x, self).lineBreak = m; relabel(x, self); }
bool l_highlighted(Ctx& x, id self, SEL) { return LD(x, self).highlighted; }
void l_setHighlighted(Ctx& x, id self, SEL, bool b) { LD(x, self).highlighted = b; relabel(x, self); }
bool l_enabled(Ctx& x, id self, SEL) { return LD(x, self).enabled; }
void l_setEnabled(Ctx& x, id self, SEL, bool b) { LD(x, self).enabled = b; relabel(x, self); }
bool l_adjusts(Ctx& x, id self, SEL) { return LD(x, self).adjusts; }
void l_setAdjusts(Ctx& x, id self, SEL, bool b) { LD(x, self).adjusts = b; relabel(x, self); }
float l_minFont(Ctx& x, id self, SEL) { return LD(x, self).minFontSize; }
void l_setMinFont(Ctx& x, id self, SEL, float f) { LD(x, self).minFontSize = f; }
float l_minScale(Ctx& x, id self, SEL) { return LD(x, self).minScale; }
void l_setMinScale(Ctx& x, id self, SEL, float f) { LD(x, self).minScale = f; }
int32_t l_baseline(Ctx& x, id self, SEL) { return LD(x, self).baseline; }
void l_setBaseline(Ctx& x, id self, SEL, int32_t b) { LD(x, self).baseline = b; }
float l_preferredWidth(Ctx& x, id self, SEL) { return LD(x, self).preferredWidth; }
void l_setPreferredWidth(Ctx& x, id self, SEL, float w) { LD(x, self).preferredWidth = w; }

G::FontDesc labelFont(Ctx& x, id self, const std::string& text, float width) {
    LabelData& d = LD(x, self);
    G::FontDesc f = uiFont(x, d.font);
    if (d.adjusts && d.lines == 1 && width > 0) {
        const float minSize = d.minScale > 0 ? f.size * d.minScale : (d.minFontSize > 0 ? d.minFontSize : f.size * 0.5f);
        while (f.size > minSize && G::textBackend().measure(text, f).width > width) f.size = std::max(minSize, f.size - 0.5f);
    }
    return f;
}
void l_textRect(Ctx& x, GuestPtr ret, id self, SEL, CGRect bounds, int32_t lines) {
    const std::string t = textOf(x, LD(x, self).text);
    const G::FontDesc f = labelFont(x, self, t, bounds.size.width);
    G::Size s = measureText(x, t, f, bounds.size.width, lines, LD(x, self).lineBreak);
    s.h = std::min(s.h, bounds.size.height);
    CG::writeStruct(x, ret, CGRect{bounds.origin, {std::min(s.w, bounds.size.width), s.h}});
}
void l_drawTextInRect(Ctx& x, id self, SEL, CGRect r) {
    LabelData& d = LD(x, self);
    const std::string t = textOf(x, d.text);
    if (t.empty()) return;
    const id ctx = CG::currentContext(x);
    const G::FontDesc f = labelFont(x, self, t, r.size.width);
    const G::Size s = measureText(x, t, f, r.size.width, d.lines, d.lineBreak);
    const float h = std::min(s.h, r.size.height);
    G::Rect tr{r.origin.x, r.origin.y + (r.size.height - h) / 2, r.size.width, h};   // vertically centred
    G::Color c = d.highlighted && d.highlightedColor ? uiColor(x, d.highlightedColor) : uiColor(x, d.textColor);
    if (!d.enabled) c.a *= 0.44f;
    if (d.shadowColor && (d.shadowOffset.width != 0 || d.shadowOffset.height != 0)) {
        const G::Rect sr{tr.x + d.shadowOffset.width, tr.y + d.shadowOffset.height, tr.w, tr.h};
        drawText(x, ctx, t, f, sr, uiColor(x, d.shadowColor), d.lineBreak, d.alignment, d.lines);
    }
    drawText(x, ctx, t, f, tr, c, d.lineBreak, d.alignment, d.lines);
}
void l_drawRect(Ctx& x, id self, SEL, CGRect) {
    const G::Rect b = boundsOf(x, self);
    sendRect(x, self, rt(x).sel("drawTextInRect:"), b);
}
void l_sizeThatFits(Ctx& x, GuestPtr ret, id self, SEL, CGSize size) {
    LabelData& d = LD(x, self);
    const std::string t = textOf(x, d.text);
    const float maxW = d.lines == 1 ? 0 : (size.width > 0 ? size.width : 1e6f);
    G::Size s = measureText(x, t, uiFont(x, d.font), maxW, d.lines, d.lines == 1 ? 2 : d.lineBreak);
    CG::writeStruct(x, ret, CGSize{std::ceil(s.w), std::ceil(s.h)});
}
void l_intrinsic(Ctx& x, GuestPtr ret, id self, SEL s) { l_sizeThatFits(x, ret, self, s, {0, 0}); }

// ============================================================= UIImageView ===

struct ImageViewData : ObjC::HostObject {
    id image = 0, highlightedImage = 0, animationImages = 0;   // retained
    bool highlighted = false, animating = false;
    double duration = 0;
    int32_t repeatCount = 0;
    std::shared_ptr<Foundation::RunLoop::Timer> timer;
    size_t frame = 0;
};
ImageViewData& IV(Ctx& x, id v) { return rt(x).host<ImageViewData>(v); }
void showImage(Ctx& x, id self, id img) {
    const id layer = viewLayer(x, self);
    LayerData& l = layerData(x, layer);
    ImgData* d = uiImage(x, img);
    const id cg = img ? imageCGImage(x, img) : 0;
    rt(x).send(x.cpu, layer, "setContents:", {cg});
    l.contentsScale = d ? d->scale : 1;
    l.contentsCenter = {0, 0, 1, 1};
    if (d && d->resizable && d->image) {
        const float w = float(d->image->width) / d->scale, h = float(d->image->height) / d->scale;
        if (w > 0 && h > 0) {
            const float cx = d->caps.left / w, cy = d->caps.top / h;
            const float cw = std::max(0.0f, 1 - (d->caps.left + d->caps.right) / w), ch = std::max(0.0f, 1 - (d->caps.top + d->caps.bottom) / h);
            l.contentsCenter = {cx, cy, cw, ch};
        }
    }
    setNeedsComposite(x);
}
void refreshImage(Ctx& x, id self) {
    ImageViewData& d = IV(x, self);
    if (d.animating) return;
    showImage(x, self, d.highlighted && d.highlightedImage ? d.highlightedImage : d.image);
}
id iv_initWithFrame(Ctx& x, id self, SEL s, CGRect f) {
    const id me = id(rt(x).sendSuper(x.cpu, self, ui(x).view, s, {fbits(f.origin.x), fbits(f.origin.y), fbits(f.size.width), fbits(f.size.height)}));
    if (me) viewData(x, me).userInteraction = false;
    return me;
}
id iv_initWithImage(Ctx& x, id self, SEL, id img) {
    const G::Size s = imageSize(x, img);
    const id me = id(sendRect(x, self, rt(x).sel("initWithFrame:"), {0, 0, s.w, s.h}));
    if (me) rt(x).send(x.cpu, me, "setImage:", {img});
    return me;
}
id iv_initWithImages(Ctx& x, id self, SEL s, id img, id hl) {
    const id me = iv_initWithImage(x, self, s, img);
    if (me) rt(x).send(x.cpu, me, "setHighlightedImage:", {hl});
    return me;
}
id iv_initWithCoder(Ctx& x, id self, SEL s, id coder) {
    const id me = id(rt(x).sendSuper(x.cpu, self, ui(x).view, s, {coder}));
    if (!me) return 0;
    if (!has(x, coder, "UIUserInteractionDisabled")) viewData(x, me).userInteraction = false;
    if (const id img = decode(x, coder, "UIImage")) rt(x).send(x.cpu, me, "setImage:", {img});
    if (const id hl = decode(x, coder, "UIHighlightedImage")) rt(x).send(x.cpu, me, "setHighlightedImage:", {hl});
    if (const id anim = decode(x, coder, "UIAnimationImages")) rt(x).send(x.cpu, me, "setAnimationImages:", {anim});
    if (has(x, coder, "UIAnimationDuration")) IV(x, me).duration = double(decodeFloat(x, coder, "UIAnimationDuration"));
    return me;
}
void iv_stop(Ctx& x, id self, SEL);
void iv_dealloc(Ctx& x, id self, SEL s) {
    if (ImageViewData* d = rt(x).hostIf<ImageViewData>(self)) {
        if (d->timer) d->timer->valid = false;
        for (id* o : {&d->image, &d->highlightedImage, &d->animationImages}) { if (*o) rt(x).release(x.cpu, *o); *o = 0; }
    }
    rt(x).sendSuper(x.cpu, self, ui(x).view, s);
}
id iv_image(Ctx& x, id self, SEL) { return IV(x, self).image; }
void iv_setImage(Ctx& x, id self, SEL, id img) { setRetained(x, IV(x, self).image, img); refreshImage(x, self); }
id iv_hlImage(Ctx& x, id self, SEL) { return IV(x, self).highlightedImage; }
void iv_setHlImage(Ctx& x, id self, SEL, id img) { setRetained(x, IV(x, self).highlightedImage, img); refreshImage(x, self); }
bool iv_highlighted(Ctx& x, id self, SEL) { return IV(x, self).highlighted; }
void iv_setHighlighted(Ctx& x, id self, SEL, bool b) { IV(x, self).highlighted = b; refreshImage(x, self); }
id iv_animImages(Ctx& x, id self, SEL) { return IV(x, self).animationImages; }
void iv_setAnimImages(Ctx& x, id self, SEL, id a) { setRetained(x, IV(x, self).animationImages, a ? id(rt(x).send(x.cpu, a, "copy")) : 0); if (a) rt(x).release(x.cpu, IV(x, self).animationImages); }
double iv_duration(Ctx& x, id self, SEL) { return IV(x, self).duration; }
void iv_setDuration(Ctx& x, id self, SEL, double d) { IV(x, self).duration = d; }
int32_t iv_repeat(Ctx& x, id self, SEL) { return IV(x, self).repeatCount; }
void iv_setRepeat(Ctx& x, id self, SEL, int32_t n) { IV(x, self).repeatCount = n; }
void iv_start(Ctx& x, id self, SEL) {
    ImageViewData& d = IV(x, self);
    const auto frames = d.animationImages ? Foundation::arrayItems(x, d.animationImages) : std::vector<id>();
    if (frames.empty()) return;
    if (d.timer) d.timer->valid = false;
    d.animating = true;
    d.frame = 0;
    const double per = (d.duration > 0 ? d.duration : frames.size() / 30.0) / double(frames.size());
    Process* p = &x.proc;
    rt(x).retain(x.cpu, self);
    auto loops = std::make_shared<int32_t>(0);
    d.timer = Foundation::mainRunLoop(x.proc).addTimer(0, per, true, [p, self, loops](CPU::Cpu& c) {
        Ctx cx{*p, c, p->mem()};
        ImageViewData& dd = IV(cx, self);
        const auto fr = dd.animationImages ? Foundation::arrayItems(cx, dd.animationImages) : std::vector<id>();
        if (!dd.animating || fr.empty()) { if (dd.timer) dd.timer->valid = false; dd.animating = false; rt(cx).release(c, self); return; }
        showImage(cx, self, fr[dd.frame % fr.size()]);
        if (++dd.frame % fr.size() == 0 && dd.repeatCount > 0 && ++*loops >= dd.repeatCount) {
            dd.animating = false;
            if (dd.timer) dd.timer->valid = false;
            refreshImage(cx, self);
            rt(cx).release(c, self);
        }
    });
}
void iv_stop(Ctx& x, id self, SEL) {
    ImageViewData& d = IV(x, self);
    if (!d.animating) return;
    d.animating = false;
    if (d.timer) { d.timer->valid = false; d.timer.reset(); rt(x).release(x.cpu, self); }
    refreshImage(x, self);
}
bool iv_isAnimating(Ctx& x, id self, SEL) { return IV(x, self).animating; }
void iv_sizeThatFits(Ctx& x, GuestPtr ret, id self, SEL, CGSize) { const G::Size s = imageSize(x, IV(x, self).image); CG::writeStruct(x, ret, CGSize{s.w, s.h}); }

// ================================================================ UIButton ===

struct StateContent { id title = 0, titleColor = 0, shadowColor = 0, image = 0, background = 0; };
struct ButtonData : ObjC::HostObject {
    int32_t type = 0;
    std::unordered_map<uint32_t, StateContent> content;   // by UIControlState
    id titleLabel = 0, imageView = 0;                     // retained subviews
    UIEdgeInsets contentInsets{0, 0, 0, 0}, titleInsets{0, 0, 0, 0}, imageInsets{0, 0, 0, 0};
    bool adjustsImage = true, adjustsDisabled = true, showsTouch = false;
};
ButtonData& BD(Ctx& x, id b) { return rt(x).host<ButtonData>(b); }
uint32_t stateOf(Ctx& x, id self) { return uint32_t(rt(x).send(x.cpu, self, "state")); }
id contentFor(Ctx& x, id self, id StateContent::*field, uint32_t state) {
    ButtonData& d = BD(x, self);
    auto get = [&](uint32_t s) -> id { auto it = d.content.find(s); return it == d.content.end() ? 0 : it->second.*field; };
    if (const id v = get(state)) return v;
    if (state & 4) if (const id v = get(4)) return v;   // selected
    if (state & 1) if (const id v = get(1)) return v;   // highlighted
    return get(0);
}
id ensureTitleLabel(Ctx& x, id self) {
    ButtonData& d = BD(x, self);
    if (!d.titleLabel) {
        d.titleLabel = allocFrame(x, ui(x).label, {0, 0, 0, 0});
        rt(x).send(x.cpu, d.titleLabel, "setBackgroundColor:", {makeUIColor(x, {0, 0, 0, 0})});
        rt(x).send(x.cpu, d.titleLabel, "setOpaque:", {0});
        rt(x).send(x.cpu, d.titleLabel, "setFont:", {systemFont(x, 15, true)});
        rt(x).send(x.cpu, d.titleLabel, "setLineBreakMode:", {5});   // middle truncation
        rt(x).send(x.cpu, self, "addSubview:", {d.titleLabel});
    }
    return d.titleLabel;
}
id ensureImageView(Ctx& x, id self) {
    ButtonData& d = BD(x, self);
    if (!d.imageView) {
        d.imageView = allocFrame(x, ui(x).imageView, {0, 0, 0, 0});
        rt(x).send(x.cpu, d.imageView, "setContentMode:", {4});   // center
        rt(x).send(x.cpu, self, "addSubview:", {d.imageView});
    }
    return d.imageView;
}
void defaultLook(Ctx& x, id self, int32_t type) {
    BD(x, self).type = type;
    LayerData& l = layerData(x, viewLayer(x, self));
    if (type == 1) {   // UIButtonTypeRoundedRect (iOS 6 look)
        l.background = {1, 1, 1, 1};
        l.cornerRadius = 9;
        l.borderWidth = 1;
        l.border = {0.55f, 0.55f, 0.6f, 1};
    }
}
id b_withType(Ctx& x, id self, SEL, int32_t type) {
    const G::Rect f = type >= 2 && type <= 5 ? G::Rect{0, 0, 22, 22} : G::Rect{0, 0, 0, 0};
    const id b = allocFrame(x, self, f);
    defaultLook(x, b, type);
    return autoreleased(x, b);
}
id b_initWithFrame(Ctx& x, id self, SEL s, CGRect f) {
    const id me = id(rt(x).sendSuper(x.cpu, self, ui(x).control, s, {fbits(f.origin.x), fbits(f.origin.y), fbits(f.size.width), fbits(f.size.height)}));
    if (me) { BD(x, me); CD(x, me).hAlign = 0; viewData(x, me).needsLayout = true; }
    return me;
}
id b_initWithCoder(Ctx& x, id self, SEL s, id coder) {
    const id me = id(rt(x).sendSuper(x.cpu, self, ui(x).control, s, {coder}));
    if (!me) return 0;
    ButtonData& d = BD(x, me);
    if (has(x, coder, "UIButtonType")) defaultLook(x, me, decodeInt(x, coder, "UIButtonType"));
    if (const id dict = decode(x, coder, "UIButtonStatefulContent")) {
        for (auto& [k, v] : Foundation::dictEntries(x, dict)) {
            const uint32_t state = uint32_t(Foundation::numberInt(x, k));
            auto get = [&](const char* key) { return id(rt(x).send(x.cpu, v, "_hvmValueForKey:", {makeString(x, key)})); };
            StateContent& c = d.content[state];
            setRetained(x, c.title, get("UITitle"));
            setRetained(x, c.titleColor, get("UITitleColor"));
            setRetained(x, c.shadowColor, get("UIShadowColor"));
            setRetained(x, c.image, get("UIImage"));
            setRetained(x, c.background, get("UIBackgroundImage"));
        }
    }
    if (const id font = decode(x, coder, "UIFont")) rt(x).send(x.cpu, ensureTitleLabel(x, me), "setFont:", {font});
    if (has(x, coder, "UIAdjustsImageWhenHighlighted")) d.adjustsImage = decodeBool(x, coder, "UIAdjustsImageWhenHighlighted");
    if (has(x, coder, "UIShowsTouchWhenHighlighted")) d.showsTouch = decodeBool(x, coder, "UIShowsTouchWhenHighlighted");
    auto insets = [&](const char* key, UIEdgeInsets& out) {
        if (!has(x, coder, key)) return;
        const uint32_t tmp = x.proc.heap().allocZeroed(16);
        const SEL sel = rt(x).sel("decodeUIEdgeInsetsForKey:");
        x.proc.callGuest(x.cpu, rt(x).lookupForSend(rt(x).classOf(coder), sel), {tmp, coder, sel, makeString(x, key)});
        out = CG::readStruct<UIEdgeInsets>(x, tmp);
        x.proc.heap().free(tmp);
    };
    insets("UIContentEdgeInsets", d.contentInsets);
    insets("UITitleEdgeInsets", d.titleInsets);
    insets("UIImageEdgeInsets", d.imageInsets);
    viewData(x, me).needsLayout = true;
    return me;
}
void b_dealloc(Ctx& x, id self, SEL s) {
    if (ButtonData* d = rt(x).hostIf<ButtonData>(self)) {
        for (auto& [k, c] : d->content)
            for (id* o : {&c.title, &c.titleColor, &c.shadowColor, &c.image, &c.background}) { if (*o) rt(x).release(x.cpu, *o); *o = 0; }
        for (id* o : {&d->titleLabel, &d->imageView}) { if (*o) rt(x).release(x.cpu, *o); *o = 0; }
    }
    rt(x).sendSuper(x.cpu, self, ui(x).control, s);
}
void setContent(Ctx& x, id self, id StateContent::*field, id v, uint32_t state, bool copy) {
    StateContent& c = BD(x, self).content[state];
    setRetained(x, c.*field, v && copy ? autoreleased(x, id(rt(x).send(x.cpu, v, "copy"))) : v);
    viewData(x, self).needsLayout = true;
    setNeedsComposite(x);
}
void b_setTitle(Ctx& x, id self, SEL, id t, uint32_t st) { setContent(x, self, &StateContent::title, t, st, true); }
void b_setTitleColor(Ctx& x, id self, SEL, id c, uint32_t st) { setContent(x, self, &StateContent::titleColor, c, st, false); }
void b_setShadowColor(Ctx& x, id self, SEL, id c, uint32_t st) { setContent(x, self, &StateContent::shadowColor, c, st, false); }
void b_setImage(Ctx& x, id self, SEL, id i, uint32_t st) { setContent(x, self, &StateContent::image, i, st, false); }
void b_setBackground(Ctx& x, id self, SEL, id i, uint32_t st) { setContent(x, self, &StateContent::background, i, st, false); }
id b_titleFor(Ctx& x, id self, SEL, uint32_t st) { return contentFor(x, self, &StateContent::title, st); }
id b_titleColorFor(Ctx& x, id self, SEL, uint32_t st) { return contentFor(x, self, &StateContent::titleColor, st); }
id b_imageFor(Ctx& x, id self, SEL, uint32_t st) { return contentFor(x, self, &StateContent::image, st); }
id b_backgroundFor(Ctx& x, id self, SEL, uint32_t st) { return contentFor(x, self, &StateContent::background, st); }
id b_currentTitle(Ctx& x, id self, SEL) { return contentFor(x, self, &StateContent::title, stateOf(x, self)); }
id b_currentImage(Ctx& x, id self, SEL) { return contentFor(x, self, &StateContent::image, stateOf(x, self)); }
id b_currentBackground(Ctx& x, id self, SEL) { return contentFor(x, self, &StateContent::background, stateOf(x, self)); }
id b_currentTitleColor(Ctx& x, id self, SEL) {
    const id c = contentFor(x, self, &StateContent::titleColor, stateOf(x, self));
    if (c) return c;
    const int32_t type = BD(x, self).type;
    const bool hl = stateOf(x, self) & 1;
    if (type == 1) return makeUIColor(x, hl ? G::Color{1, 1, 1, 1} : G::Color{0.22f, 0.33f, 0.53f, 1});
    return makeUIColor(x, {1, 1, 1, 1});
}
id b_titleLabel(Ctx& x, id self, SEL) { return ensureTitleLabel(x, self); }
id b_imageView(Ctx& x, id self, SEL) { return ensureImageView(x, self); }
int32_t b_buttonType(Ctx& x, id self, SEL) { return BD(x, self).type; }
void b_setFont(Ctx& x, id self, SEL, id f) { rt(x).send(x.cpu, ensureTitleLabel(x, self), "setFont:", {f}); viewData(x, self).needsLayout = true; }
id b_font(Ctx& x, id self, SEL) { return id(rt(x).send(x.cpu, ensureTitleLabel(x, self), "font")); }
void b_contentInsets(Ctx& x, GuestPtr ret, id self, SEL) { CG::writeStruct(x, ret, BD(x, self).contentInsets); }
void b_setContentInsets(Ctx& x, id self, SEL, UIEdgeInsets i) { BD(x, self).contentInsets = i; viewData(x, self).needsLayout = true; }
void b_titleInsets(Ctx& x, GuestPtr ret, id self, SEL) { CG::writeStruct(x, ret, BD(x, self).titleInsets); }
void b_setTitleInsets(Ctx& x, id self, SEL, UIEdgeInsets i) { BD(x, self).titleInsets = i; viewData(x, self).needsLayout = true; }
void b_imageInsets(Ctx& x, GuestPtr ret, id self, SEL) { CG::writeStruct(x, ret, BD(x, self).imageInsets); }
void b_setImageInsets(Ctx& x, id self, SEL, UIEdgeInsets i) { BD(x, self).imageInsets = i; viewData(x, self).needsLayout = true; }
bool b_adjustsImage(Ctx& x, id self, SEL) { return BD(x, self).adjustsImage; }
void b_setAdjustsImage(Ctx& x, id self, SEL, bool b) { BD(x, self).adjustsImage = b; }
bool b_showsTouch(Ctx& x, id self, SEL) { return BD(x, self).showsTouch; }
void b_setShowsTouch(Ctx& x, id self, SEL, bool b) { BD(x, self).showsTouch = b; }
void b_ignoreBool(Ctx&, id, SEL, bool) {}
G::Size contentSize(Ctx& x, id self, G::Size& titleSize, G::Size& imageSize_) {
    const id title = b_currentTitle(x, self, 0);
    const id image = b_currentImage(x, self, 0);
    titleSize = {0, 0};
    if (title) {
        const id label = ensureTitleLabel(x, self);
        titleSize = measureText(x, textOf(x, title), uiFont(x, id(rt(x).send(x.cpu, label, "font"))), 0, 1, 2);
    }
    imageSize_ = image ? imageSize(x, image) : G::Size{0, 0};
    return {titleSize.w + imageSize_.w, std::max(titleSize.h, imageSize_.h)};
}
void b_layoutSubviews(Ctx& x, id self, SEL s) {
    rt(x).sendSuper(x.cpu, self, ui(x).control, s);
    ButtonData& d = BD(x, self);
    const uint32_t state = stateOf(x, self);
    const G::Rect b = boundsOf(x, self);
    // background image drawn by our own layer
    const id bg = contentFor(x, self, &StateContent::background, state);
    LayerData& l = layerData(x, viewLayer(x, self));
    if (bg) {
        rt(x).send(x.cpu, viewLayer(x, self), "setContents:", {imageCGImage(x, bg)});
        ImgData* bd = uiImage(x, bg);
        l.contentsScale = bd ? bd->scale : 1;
        l.contentsCenter = {0, 0, 1, 1};
        if (bd && bd->resizable && bd->image) {
            const float w = float(bd->image->width) / bd->scale, h = float(bd->image->height) / bd->scale;
            l.contentsCenter = {d.contentInsets.left * 0 + bd->caps.left / w, bd->caps.top / h,
                                std::max(0.0f, 1 - (bd->caps.left + bd->caps.right) / w), std::max(0.0f, 1 - (bd->caps.top + bd->caps.bottom) / h)};
        }
        l.gravity = kGravityResize;
    } else if (l.contents) {
        rt(x).send(x.cpu, viewLayer(x, self), "setContents:", {0});
    }
    if (d.type == 1) l.background = (state & 1) ? G::Color{0.2f, 0.45f, 0.95f, 1} : G::Color{1, 1, 1, 1};

    G::Size ts, is;
    const G::Size cs = contentSize(x, self, ts, is);
    const G::Rect inner{b.x + d.contentInsets.left, b.y + d.contentInsets.top, b.w - d.contentInsets.left - d.contentInsets.right,
                        b.h - d.contentInsets.top - d.contentInsets.bottom};
    const ControlData& c = CD(x, self);
    float x0;
    switch (c.hAlign) {
        case 1: x0 = inner.x; break;                               // left
        case 2: x0 = inner.maxX() - cs.w; break;                   // right
        default: x0 = inner.x + (inner.w - cs.w) / 2; break;       // center / fill
    }
    auto yFor = [&](float h) {
        switch (c.vAlign) { case 1: return inner.y; case 2: return inner.maxY() - h; default: return inner.y + (inner.h - h) / 2; }
    };
    const id image = b_currentImage(x, self, 0);
    if (image || d.imageView) {
        const id iv = ensureImageView(x, self);
        rt(x).send(x.cpu, iv, "setImage:", {image});
        const G::Rect ir{x0 + d.imageInsets.left - d.imageInsets.right, yFor(is.h) + d.imageInsets.top - d.imageInsets.bottom, is.w, is.h};
        sendRect(x, iv, rt(x).sel("setFrame:"), ir);
        rt(x).send(x.cpu, iv, "setHidden:", {image ? 0u : 1u});
        layerData(x, viewLayer(x, iv)).opacity = (state & 1) && d.adjustsImage && !contentFor(x, self, &StateContent::image, 1) ? 0.6f : 1.0f;
    }
    const id title = b_currentTitle(x, self, 0);
    if (title || d.titleLabel) {
        const id label = ensureTitleLabel(x, self);
        rt(x).send(x.cpu, label, "setText:", {title});
        rt(x).send(x.cpu, label, "setTextColor:", {b_currentTitleColor(x, self, 0)});
        if (const id sc = contentFor(x, self, &StateContent::shadowColor, state)) rt(x).send(x.cpu, label, "setShadowColor:", {sc});
        const float avail = std::max(0.0f, inner.w - is.w);
        const float tw = std::min(ts.w, avail);
        const G::Rect tr{x0 + is.w + d.titleInsets.left - d.titleInsets.right, yFor(ts.h) + d.titleInsets.top - d.titleInsets.bottom, tw, ts.h};
        sendRect(x, label, rt(x).sel("setFrame:"), tr);
        rt(x).send(x.cpu, label, "setHidden:", {title ? 0u : 1u});
        rt(x).send(x.cpu, label, "setEnabled:", {(state & 2) ? 0u : 1u});
    }
}
void b_setHighlighted(Ctx& x, id self, SEL s, bool h) {
    rt(x).sendSuper(x.cpu, self, ui(x).control, s, {h ? 1u : 0u});
    viewData(x, self).needsLayout = true;
    layoutIfNeeded(x, self);
}
void b_sizeThatFits(Ctx& x, GuestPtr ret, id self, SEL, CGSize) {
    ButtonData& d = BD(x, self);
    G::Size ts, is;
    G::Size cs = contentSize(x, self, ts, is);
    if (const id bg = contentFor(x, self, &StateContent::background, stateOf(x, self))) {
        const G::Size bs = imageSize(x, bg);
        if (!uiImage(x, bg) || !uiImage(x, bg)->resizable) { cs.w = std::max(cs.w, bs.w); cs.h = std::max(cs.h, bs.h); }
    }
    cs.w += d.contentInsets.left + d.contentInsets.right;
    cs.h += d.contentInsets.top + d.contentInsets.bottom;
    if (d.type == 1) { cs.w += 24; cs.h = std::max(cs.h + 12, 37.0f); }
    CG::writeStruct(x, ret, CGSize{std::ceil(cs.w), std::ceil(cs.h)});
}
// Nib helper: UIButtonContent is a dictionary-like value object.
struct BCData : ObjC::HostObject { std::unordered_map<std::string, id> v; };
id bc_initWithCoder(Ctx& x, id self, SEL, id coder) {
    BCData& d = rt(x).host<BCData>(self);
    for (const char* k : {"UITitle", "UITitleColor", "UIShadowColor", "UIImage", "UIBackgroundImage"}) {
        const id o = decode(x, coder, k);
        if (o) d.v[k] = rt(x).retain(x.cpu, o);
    }
    return self;
}
id bc_valueForKey(Ctx& x, id self, SEL, id key) {
    BCData& d = rt(x).host<BCData>(self);
    auto it = d.v.find(utf8(x, key));
    return it == d.v.end() ? 0 : it->second;
}

// ================================================== UIActivityIndicatorView ===

struct SpinnerData : ObjC::HostObject { bool animating = false, hides = true; int32_t style = 2; id color = 0; };
SpinnerData& SD(Ctx& x, id v) { return rt(x).host<SpinnerData>(v); }
id ai_initWithStyle(Ctx& x, id self, SEL, int32_t style) {
    const float sz = style == 0 ? 37 : 20;
    const id me = id(sendRect(x, self, rt(x).sel("initWithFrame:"), {0, 0, sz, sz}));
    if (me) { SD(x, me).style = style; rt(x).send(x.cpu, me, "setHidden:", {1}); rt(x).send(x.cpu, me, "setBackgroundColor:", {0}); }
    return me;
}
id ai_initWithFrame(Ctx& x, id self, SEL s, CGRect f) {
    const id me = id(rt(x).sendSuper(x.cpu, self, ui(x).view, s, {fbits(f.origin.x), fbits(f.origin.y), fbits(f.size.width), fbits(f.size.height)}));
    if (me) { SD(x, me); viewData(x, me).userInteraction = false; }
    return me;
}
void ai_start(Ctx& x, id self, SEL) { SD(x, self).animating = true; rt(x).send(x.cpu, self, "setHidden:", {0}); rt(x).send(x.cpu, self, "setNeedsDisplay"); }
void ai_stop(Ctx& x, id self, SEL) { SD(x, self).animating = false; if (SD(x, self).hides) rt(x).send(x.cpu, self, "setHidden:", {1}); }
bool ai_isAnimating(Ctx& x, id self, SEL) { return SD(x, self).animating; }
bool ai_hides(Ctx& x, id self, SEL) { return SD(x, self).hides; }
void ai_setHides(Ctx& x, id self, SEL, bool b) { SD(x, self).hides = b; if (b && !SD(x, self).animating) rt(x).send(x.cpu, self, "setHidden:", {1}); }
int32_t ai_style(Ctx& x, id self, SEL) { return SD(x, self).style; }
void ai_setStyle(Ctx& x, id self, SEL, int32_t s) { SD(x, self).style = s; rt(x).send(x.cpu, self, "setNeedsDisplay"); }
id ai_color(Ctx& x, id self, SEL) { return SD(x, self).color; }
void ai_setColor(Ctx& x, id self, SEL, id c) { setRetained(x, SD(x, self).color, c); rt(x).send(x.cpu, self, "setNeedsDisplay"); }
void ai_drawRect(Ctx& x, id self, SEL, CGRect) {
    const id ctx = CG::currentContext(x);
    CG::ContextData* cd = CG::context(x, ctx);
    if (!cd) return;
    const SpinnerData& d = SD(x, self);
    const G::Rect b = boundsOf(x, self);
    const G::Color base = d.color ? uiColor(x, d.color) : d.style == 2 ? G::Color{0.5f, 0.5f, 0.5f, 1} : G::Color{1, 1, 1, 1};
    const G::Point c{b.x + b.w / 2, b.y + b.h / 2};
    const float r0 = std::min(b.w, b.h) * 0.22f, r1 = std::min(b.w, b.h) * 0.46f;
    for (int i = 0; i < 12; ++i) {
        const float a = float(i) * 3.14159265f / 6;
        G::Path p;
        p.moveTo({c.x + r0 * std::cos(a), c.y + r0 * std::sin(a)});
        p.lineTo({c.x + r1 * std::cos(a), c.y + r1 * std::sin(a)});
        G::Color k = base;
        k.a = 0.25f + 0.75f * float(i) / 11;
        G::fillPath(cd->surface, G::strokeToPath(p.transformed(cd->gs().ctm), std::min(b.w, b.h) * 0.09f * cd->gs().ctm.a, 1, 1, 10, {}, 0), false, k, cd->gs().clip);
    }
}

// ===================================================== UIAlertView / sheets ===

struct AlertData : ObjC::HostObject {
    id title = 0, message = 0, delegate = 0;   // title/message retained
    std::vector<std::string> buttons;
    int32_t cancelIndex = -1, firstOther = -1, style = 0;
    id window = 0;                             // retained while shown
    bool visible = false, sheet = false;
    int32_t destructiveIndex = -1;
};
AlertData& AL(Ctx& x, id a) { return rt(x).host<AlertData>(a); }
struct AlertState { explicit AlertState(Process&) {} Class handler = 0; id handlerObj = 0; std::vector<id> shown; };
AlertState& ALS(Ctx& x) { return x.proc.service<AlertState>(); }

id av_initFull(Ctx& x, id self, SEL, id title, id message, id delegate, id cancel, VarArgs va) {
    const id me = id(sendRect(x, self, rt(x).sel("initWithFrame:"), {0, 0, 284, 140}));
    if (!me) return 0;
    AlertData& d = AL(x, me);
    setRetained(x, d.title, title ? autoreleased(x, id(rt(x).send(x.cpu, title, "copy"))) : 0);
    setRetained(x, d.message, message ? autoreleased(x, id(rt(x).send(x.cpu, message, "copy"))) : 0);
    d.delegate = delegate;
    if (cancel) { d.buttons.push_back(utf8(x, cancel)); d.cancelIndex = 0; }
    for (int guard = 0; guard < 32; ++guard) {
        const id b = va.args.word();
        if (!b) break;
        if (d.firstOther < 0) d.firstOther = int32_t(d.buttons.size());
        d.buttons.push_back(utf8(x, b));
    }
    return me;
}
id av_initSheet(Ctx& x, id self, SEL, id title, id delegate, id cancel, id destructive, VarArgs va) {
    const id me = id(sendRect(x, self, rt(x).sel("initWithFrame:"), {0, 0, 320, 200}));
    if (!me) return 0;
    AlertData& d = AL(x, me);
    d.sheet = true;
    setRetained(x, d.title, title ? autoreleased(x, id(rt(x).send(x.cpu, title, "copy"))) : 0);
    d.delegate = delegate;
    if (destructive) { d.destructiveIndex = int32_t(d.buttons.size()); d.buttons.push_back(utf8(x, destructive)); }
    for (int guard = 0; guard < 32; ++guard) {
        const id b = va.args.word();
        if (!b) break;
        if (d.firstOther < 0) d.firstOther = int32_t(d.buttons.size());
        d.buttons.push_back(utf8(x, b));
    }
    if (cancel) { d.cancelIndex = int32_t(d.buttons.size()); d.buttons.push_back(utf8(x, cancel)); }
    return me;
}
void av_dealloc(Ctx& x, id self, SEL s) {
    if (AlertData* d = rt(x).hostIf<AlertData>(self)) for (id* o : {&d->title, &d->message, &d->window}) { if (*o) rt(x).release(x.cpu, *o); *o = 0; }
    rt(x).sendSuper(x.cpu, self, ui(x).view, s);
}
int32_t av_addButton(Ctx& x, id self, SEL, id title) {
    AlertData& d = AL(x, self);
    if (d.firstOther < 0 && d.cancelIndex != int32_t(d.buttons.size())) d.firstOther = int32_t(d.buttons.size());
    d.buttons.push_back(title ? utf8(x, title) : "");
    return int32_t(d.buttons.size() - 1);
}
int32_t av_count(Ctx& x, id self, SEL) { return int32_t(AL(x, self).buttons.size()); }
id av_buttonTitle(Ctx& x, id self, SEL, int32_t i) { AlertData& d = AL(x, self); return i >= 0 && size_t(i) < d.buttons.size() ? makeString(x, d.buttons[size_t(i)]) : 0; }
int32_t av_cancelIndex(Ctx& x, id self, SEL) { return AL(x, self).cancelIndex; }
void av_setCancelIndex(Ctx& x, id self, SEL, int32_t i) { AL(x, self).cancelIndex = i; }
int32_t av_firstOther(Ctx& x, id self, SEL) { return AL(x, self).firstOther; }
int32_t av_destructive(Ctx& x, id self, SEL) { return AL(x, self).destructiveIndex; }
void av_setDestructive(Ctx& x, id self, SEL, int32_t i) { AL(x, self).destructiveIndex = i; }
id av_title(Ctx& x, id self, SEL) { return AL(x, self).title; }
void av_setTitle(Ctx& x, id self, SEL, id t) { setRetained(x, AL(x, self).title, t); }
id av_message(Ctx& x, id self, SEL) { return AL(x, self).message; }
void av_setMessage(Ctx& x, id self, SEL, id m) { setRetained(x, AL(x, self).message, m); }
id av_delegate(Ctx& x, id self, SEL) { return AL(x, self).delegate; }
void av_setDelegate(Ctx& x, id self, SEL, id d) { AL(x, self).delegate = d; }
bool av_visible(Ctx& x, id self, SEL) { return AL(x, self).visible; }
int32_t av_style(Ctx& x, id self, SEL) { return AL(x, self).style; }
void av_setStyle(Ctx& x, id self, SEL, int32_t s) { AL(x, self).style = s; }
id av_textField(Ctx& x, id, SEL, int32_t) { x.proc.reportMissing("UIAlertView text fields"); return 0; }

struct AlertMap { explicit AlertMap(Process&) {} std::unordered_map<id, id> m; };
std::unordered_map<id, id>& objectAlertMap(Ctx& x) { return x.proc.service<AlertMap>().m; }

void buildAlertWindow(Ctx& x, id self) {
    AlertData& d = AL(x, self);
    auto& u = ui(x);
    auto& r = rt(x);
    const id win = allocFrame(x, u.window, {0, 0, u.screenW, u.screenH});
    r.send(x.cpu, win, "setWindowLevel:", {fbits(2000)});
    r.send(x.cpu, win, "setBackgroundColor:", {makeUIColor(x, {0, 0, 0, 0.45f})});
    // content box
    const bool landscape = u.statusBarOrientation >= 3;
    const float W = landscape ? u.screenH : u.screenW, H = landscape ? u.screenW : u.screenH;
    const float boxW = d.sheet ? W : 284;
    const G::FontDesc titleFont{"Helvetica-Bold", 17, true, false}, msgFont{"Helvetica", 15, false, false};
    const std::string title = textOf(x, d.title), msg = textOf(x, d.message);
    const G::Size ts = title.empty() ? G::Size{0, 0} : measureText(x, title, titleFont, boxW - 32, 0, 0);
    const G::Size ms = msg.empty() ? G::Size{0, 0} : measureText(x, msg, msgFont, boxW - 32, 0, 0);
    const bool sideBySide = !d.sheet && d.buttons.size() == 2;
    const float btnH = 43, gap = 8;
    const size_t rows = sideBySide ? 1 : d.buttons.size();
    const float boxH = 16 + ts.h + (ms.h > 0 ? 8 + ms.h : 0) + 16 + float(rows) * (btnH + gap);
    const id root = allocFrame(x, u.view, {0, 0, W, H});
    r.send(x.cpu, root, "setBackgroundColor:", {makeUIColor(x, {0, 0, 0, 0})});
    if (landscape) {
        LayerData& l = layerData(x, viewLayer(x, root));
        l.transform = from2D(G::Affine::rotate(u.statusBarOrientation == 4 ? 3.14159265f / 2 : -3.14159265f / 2));
        l.position = {u.screenW / 2, u.screenH / 2};
    }
    r.send(x.cpu, win, "addSubview:", {root});
    const float boxY = d.sheet ? H - boxH : (H - boxH) / 2;
    const id box = allocFrame(x, u.view, {(W - boxW) / 2, boxY, boxW, boxH});
    r.send(x.cpu, box, "setBackgroundColor:", {makeUIColor(x, d.sheet ? G::Color{0.12f, 0.12f, 0.14f, 0.95f} : G::Color{0.09f, 0.16f, 0.38f, 0.94f})});
    LayerData& bl = layerData(x, viewLayer(x, box));
    if (!d.sheet) { bl.cornerRadius = 10; bl.borderWidth = 2; bl.border = {1, 1, 1, 0.9f}; }
    r.send(x.cpu, root, "addSubview:", {box});
    float y = 16;
    auto addLabel = [&](const std::string& text, const G::FontDesc& f, G::Size s) {
        const id l = allocFrame(x, u.label, {16, y, boxW - 32, s.h});
        r.send(x.cpu, l, "setText:", {makeString(x, text)});
        r.send(x.cpu, l, "setFont:", {makeUIFont(x, f)});
        r.send(x.cpu, l, "setTextColor:", {makeUIColor(x, {1, 1, 1, 1})});
        r.send(x.cpu, l, "setBackgroundColor:", {makeUIColor(x, {0, 0, 0, 0})});
        r.send(x.cpu, l, "setOpaque:", {0});
        r.send(x.cpu, l, "setTextAlignment:", {1});
        r.send(x.cpu, l, "setNumberOfLines:", {0});
        r.send(x.cpu, box, "addSubview:", {l});
        r.release(x.cpu, l);
        y += s.h + 8;
    };
    if (!title.empty()) addLabel(title, titleFont, ts);
    if (!msg.empty()) addLabel(msg, msgFont, ms);
    y += 8;
    auto& as = ALS(x);
    if (!as.handlerObj) { as.handlerObj = r.createInstance(as.handler); r.makeImmortal(as.handlerObj); }
    for (size_t i = 0; i < d.buttons.size(); ++i) {
        G::Rect f;
        if (sideBySide) f = {16 + float(i) * ((boxW - 40) / 2 + 8), y, (boxW - 40) / 2, btnH};
        else { f = {16, y, boxW - 32, btnH}; y += btnH + gap; }
        const id b = id(r.send(x.cpu, u.button, "buttonWithType:", {0}));
        sendRect(x, b, r.sel("setFrame:"), f);
        const bool cancel = int32_t(i) == d.cancelIndex, destructive = int32_t(i) == d.destructiveIndex;
        r.send(x.cpu, b, "setBackgroundColor:", {makeUIColor(x, destructive ? G::Color{0.75f, 0.12f, 0.1f, 1} : cancel ? G::Color{0.25f, 0.3f, 0.45f, 1} : G::Color{0.35f, 0.42f, 0.62f, 1})});
        LayerData& btl = layerData(x, viewLayer(x, b));
        btl.cornerRadius = 6;
        btl.borderWidth = 1;
        btl.border = {1, 1, 1, 0.5f};
        r.send(x.cpu, b, "setTitle:forState:", {makeString(x, d.buttons[i]), 0});
        r.send(x.cpu, b, "setTag:", {uint32_t(i)});
        r.send(x.cpu, b, "addTarget:action:forControlEvents:", {as.handlerObj, r.sel("_hvmAlertButton:"), kTouchUpInside});
        r.send(x.cpu, box, "addSubview:", {b});
        objectAlertMap(x)[b] = self;
    }
    r.release(x.cpu, box);
    r.release(x.cpu, root);
    setRetained(x, d.window, win);
    r.release(x.cpu, win);
}
void av_show(Ctx& x, id self, SEL) {
    AlertData& d = AL(x, self);
    if (d.visible) return;
    auto& r = rt(x);
    std::string log = std::string(d.sheet ? "[ActionSheet] " : "[Alert] ") + textOf(x, d.title);
    if (d.message) log += " — " + textOf(x, d.message);
    for (auto& b : d.buttons) log += " [" + b + "]";
    x.proc.log(LogLevel::Info, log);
    r.retain(x.cpu, self);   // shown alerts keep themselves alive
    if (d.delegate && r.respondsTo(d.delegate, r.sel(d.sheet ? "willPresentActionSheet:" : "willPresentAlertView:")))
        r.send(x.cpu, d.delegate, d.sheet ? "willPresentActionSheet:" : "willPresentAlertView:", {self});
    buildAlertWindow(x, self);
    d.visible = true;
    r.send(x.cpu, d.window, "setHidden:", {0});
    viewData(x, d.window).visible = true;
    auto& u = ui(x);
    if (std::find(u.windows.begin(), u.windows.end(), d.window) == u.windows.end()) u.windows.push_back(d.window);
    setNeedsComposite(x);
    if (d.delegate && r.respondsTo(d.delegate, r.sel(d.sheet ? "didPresentActionSheet:" : "didPresentAlertView:")))
        r.send(x.cpu, d.delegate, d.sheet ? "didPresentActionSheet:" : "didPresentAlertView:", {self});
}
void av_showIn(Ctx& x, id self, SEL s, id) { av_show(x, self, s); }
void av_showFromRect(Ctx& x, id self, SEL s, CGRect, id, bool) { av_show(x, self, s); }
void dismissAlert(Ctx& x, id self, int32_t index, bool clicked) {
    AlertData& d = AL(x, self);
    if (!d.visible) return;
    auto& r = rt(x);
    const char* clickedSel = d.sheet ? "actionSheet:clickedButtonAtIndex:" : "alertView:clickedButtonAtIndex:";
    const char* willSel = d.sheet ? "actionSheet:willDismissWithButtonIndex:" : "alertView:willDismissWithButtonIndex:";
    const char* didSel = d.sheet ? "actionSheet:didDismissWithButtonIndex:" : "alertView:didDismissWithButtonIndex:";
    if (clicked && d.delegate && r.respondsTo(d.delegate, r.sel(clickedSel))) r.send(x.cpu, d.delegate, clickedSel, {self, uint32_t(index)});
    if (d.delegate && r.respondsTo(d.delegate, r.sel(willSel))) r.send(x.cpu, d.delegate, willSel, {self, uint32_t(index)});
    d.visible = false;
    if (d.window) {
        viewData(x, d.window).visible = false;
        auto& u = ui(x);
        u.windows.erase(std::remove(u.windows.begin(), u.windows.end(), d.window), u.windows.end());
        auto& m = objectAlertMap(x);
        for (auto it = m.begin(); it != m.end();) { if (it->second == self) it = m.erase(it); else ++it; }
        const id w = d.window;
        d.window = 0;
        autoreleased(x, w);
    }
    setNeedsComposite(x);
    if (d.delegate && r.respondsTo(d.delegate, r.sel(didSel))) r.send(x.cpu, d.delegate, didSel, {self, uint32_t(index)});
    r.release(x.cpu, self);
}
void av_dismiss(Ctx& x, id self, SEL, int32_t index, bool) { dismissAlert(x, self, index, false); }
void handler_button(Ctx& x, id, SEL, id button) {
    auto& m = objectAlertMap(x);
    auto it = m.find(button);
    if (it == m.end()) return;
    const id alert = it->second;
    const int32_t index = int32_t(rt(x).send(x.cpu, button, "tag"));
    dismissAlert(x, alert, index, true);
}
} // namespace

void registerUIControls(Process& p, ObjC::Runtime& r) {
    auto& u = ui(p);
    const std::string R = kR, S = kS, E = kE;
    ObjC::ClassBuilder ctl = r.defineClass("UIControl", "UIView");
    u.control = ctl.cls();
    ctl.m<&c_initWithCoder>("initWithCoder:")
       .m<&c_addTarget>("addTarget:action:forControlEvents:", "v@:@:I").m<&c_removeTarget>("removeTarget:action:forControlEvents:", "v@:@:I")
       .m<&c_allTargets>("allTargets").m<&c_allEvents>("allControlEvents", "I@:").m<&c_actionsFor>("actionsForTarget:forControlEvent:")
       .m<&c_sendAction>("sendAction:to:forEvent:", "v@::@@").m<&c_sendActionsForEvents>("sendActionsForControlEvents:", "v@:I")
       .m<&c_enabled>("isEnabled", "c@:").m<&c_setEnabled>("setEnabled:", "v@:c").m<&c_selected>("isSelected", "c@:")
       .m<&c_setSelected>("setSelected:", "v@:c").m<&c_highlighted>("isHighlighted", "c@:").m<&c_setHighlighted>("setHighlighted:", "v@:c")
       .m<&c_state>("state", "I@:").m<&c_tracking>("isTracking", "c@:").m<&c_touchInside>("isTouchInside", "c@:")
       .m<&c_hAlign>("contentHorizontalAlignment", "i@:").m<&c_setHAlign>("setContentHorizontalAlignment:", "v@:i")
       .m<&c_vAlign>("contentVerticalAlignment", "i@:").m<&c_setVAlign>("setContentVerticalAlignment:", "v@:i")
       .m<&c_beginTracking>("beginTrackingWithTouch:withEvent:", "c@:@@").m<&c_continueTracking>("continueTrackingWithTouch:withEvent:", "c@:@@")
       .m<&c_endTracking>("endTrackingWithTouch:withEvent:", "v@:@@").m<&c_cancelTracking>("cancelTrackingWithEvent:", "v@:@")
       .m<&c_touchesBegan>("touchesBegan:withEvent:", "v@:@@").m<&c_touchesMoved>("touchesMoved:withEvent:", "v@:@@")
       .m<&c_touchesEnded>("touchesEnded:withEvent:", "v@:@@").m<&c_touchesCancelled>("touchesCancelled:withEvent:", "v@:@@");

    ObjC::ClassBuilder label = r.defineClass("UILabel", "UIView");
    u.label = label.cls();
    label.m<&l_initWithFrame>("initWithFrame:", "@@:" + R).m<&l_initWithCoder>("initWithCoder:").m<&l_dealloc>("dealloc", "v@:")
         .m<&l_text>("text").m<&l_setText>("setText:", "v@:@").m<&l_attributedText>("attributedText").m<&l_setAttributedText>("setAttributedText:", "v@:@")
         .m<&l_font>("font").m<&l_setFont>("setFont:", "v@:@").m<&l_textColor>("textColor").m<&l_setTextColor>("setTextColor:", "v@:@")
         .m<&l_shadowColor>("shadowColor").m<&l_setShadowColor>("setShadowColor:", "v@:@").m<&l_shadowOffset>("shadowOffset", S + "@:")
         .m<&l_setShadowOffset>("setShadowOffset:", "v@:" + S).m<&l_hlColor>("highlightedTextColor").m<&l_setHlColor>("setHighlightedTextColor:", "v@:@")
         .m<&l_alignment>("textAlignment", "i@:").m<&l_setAlignment>("setTextAlignment:", "v@:i").m<&l_lines>("numberOfLines", "i@:")
         .m<&l_setLines>("setNumberOfLines:", "v@:i").m<&l_lineBreak>("lineBreakMode", "i@:").m<&l_setLineBreak>("setLineBreakMode:", "v@:i")
         .m<&l_highlighted>("isHighlighted", "c@:").m<&l_setHighlighted>("setHighlighted:", "v@:c").m<&l_enabled>("isEnabled", "c@:")
         .m<&l_setEnabled>("setEnabled:", "v@:c").m<&l_adjusts>("adjustsFontSizeToFitWidth", "c@:").m<&l_setAdjusts>("setAdjustsFontSizeToFitWidth:", "v@:c")
         .m<&l_minFont>("minimumFontSize", "f@:").m<&l_setMinFont>("setMinimumFontSize:", "v@:f").m<&l_minScale>("minimumScaleFactor", "f@:")
         .m<&l_setMinScale>("setMinimumScaleFactor:", "v@:f").m<&l_baseline>("baselineAdjustment", "i@:").m<&l_setBaseline>("setBaselineAdjustment:", "v@:i")
         .m<&l_preferredWidth>("preferredMaxLayoutWidth", "f@:").m<&l_setPreferredWidth>("setPreferredMaxLayoutWidth:", "v@:f")
         .m<&l_textRect>("textRectForBounds:limitedToNumberOfLines:", R + "@:" + R + "i").m<&l_drawTextInRect>("drawTextInRect:", "v@:" + R)
         .m<&l_drawRect>("drawRect:", "v@:" + R).m<&l_sizeThatFits>("sizeThatFits:", S + "@:" + S).m<&l_intrinsic>("intrinsicContentSize", S + "@:");

    ObjC::ClassBuilder iv = r.defineClass("UIImageView", "UIView");
    u.imageView = iv.cls();
    iv.m<&iv_initWithFrame>("initWithFrame:", "@@:" + R).m<&iv_initWithImage>("initWithImage:").m<&iv_initWithImages>("initWithImage:highlightedImage:")
      .m<&iv_initWithCoder>("initWithCoder:").m<&iv_dealloc>("dealloc", "v@:").m<&iv_image>("image").m<&iv_setImage>("setImage:", "v@:@")
      .m<&iv_hlImage>("highlightedImage").m<&iv_setHlImage>("setHighlightedImage:", "v@:@").m<&iv_highlighted>("isHighlighted", "c@:")
      .m<&iv_setHighlighted>("setHighlighted:", "v@:c").m<&iv_animImages>("animationImages").m<&iv_setAnimImages>("setAnimationImages:", "v@:@")
      .m<&iv_duration>("animationDuration", "d@:").m<&iv_setDuration>("setAnimationDuration:", "v@:d").m<&iv_repeat>("animationRepeatCount", "i@:")
      .m<&iv_setRepeat>("setAnimationRepeatCount:", "v@:i").m<&iv_start>("startAnimating", "v@:").m<&iv_stop>("stopAnimating", "v@:")
      .m<&iv_isAnimating>("isAnimating", "c@:").m<&iv_sizeThatFits>("sizeThatFits:", S + "@:" + S);

    ObjC::ClassBuilder btn = r.defineClass("UIButton", "UIControl");
    u.button = btn.cls();
    btn.cm<&b_withType>("buttonWithType:");
    btn.m<&b_initWithFrame>("initWithFrame:", "@@:" + R).m<&b_initWithCoder>("initWithCoder:").m<&b_dealloc>("dealloc", "v@:")
       .m<&b_setTitle>("setTitle:forState:", "v@:@I").m<&b_setTitleColor>("setTitleColor:forState:", "v@:@I")
       .m<&b_setShadowColor>("setTitleShadowColor:forState:", "v@:@I").m<&b_setImage>("setImage:forState:", "v@:@I")
       .m<&b_setBackground>("setBackgroundImage:forState:", "v@:@I").m<&b_titleFor>("titleForState:").m<&b_titleColorFor>("titleColorForState:")
       .m<&b_imageFor>("imageForState:").m<&b_backgroundFor>("backgroundImageForState:").m<&b_currentTitle>("currentTitle")
       .m<&b_currentImage>("currentImage").m<&b_currentBackground>("currentBackgroundImage").m<&b_currentTitleColor>("currentTitleColor")
       .m<&b_titleLabel>("titleLabel").m<&b_imageView>("imageView").m<&b_buttonType>("buttonType", "i@:").m<&b_setFont>("setFont:", "v@:@")
       .m<&b_font>("font").m<&b_contentInsets>("contentEdgeInsets", E + "@:").m<&b_setContentInsets>("setContentEdgeInsets:", "v@:" + E)
       .m<&b_titleInsets>("titleEdgeInsets", E + "@:").m<&b_setTitleInsets>("setTitleEdgeInsets:", "v@:" + E)
       .m<&b_imageInsets>("imageEdgeInsets", E + "@:").m<&b_setImageInsets>("setImageEdgeInsets:", "v@:" + E)
       .m<&b_adjustsImage>("adjustsImageWhenHighlighted", "c@:").m<&b_setAdjustsImage>("setAdjustsImageWhenHighlighted:", "v@:c")
       .m<&b_ignoreBool>("setAdjustsImageWhenDisabled:", "v@:c").m<&b_showsTouch>("showsTouchWhenHighlighted", "c@:")
       .m<&b_setShowsTouch>("setShowsTouchWhenHighlighted:", "v@:c").m<&b_ignoreBool>("setReversesTitleShadowWhenHighlighted:", "v@:c")
       .m<&b_layoutSubviews>("layoutSubviews", "v@:").m<&b_setHighlighted>("setHighlighted:", "v@:c")
       .m<&b_sizeThatFits>("sizeThatFits:", S + "@:" + S);
    ObjC::ClassBuilder bc = r.defineClass("UIButtonContent", "NSObject");
    bc.m<&bc_initWithCoder>("initWithCoder:").m<&bc_valueForKey>("_hvmValueForKey:");

    ObjC::ClassBuilder ai = r.defineClass("UIActivityIndicatorView", "UIView");
    ai.m<&ai_initWithStyle>("initWithActivityIndicatorStyle:").m<&ai_initWithFrame>("initWithFrame:", "@@:" + R)
      .m<&ai_start>("startAnimating", "v@:").m<&ai_stop>("stopAnimating", "v@:").m<&ai_isAnimating>("isAnimating", "c@:")
      .m<&ai_hides>("hidesWhenStopped", "c@:").m<&ai_setHides>("setHidesWhenStopped:", "v@:c").m<&ai_style>("activityIndicatorViewStyle", "i@:")
      .m<&ai_setStyle>("setActivityIndicatorViewStyle:", "v@:i").m<&ai_color>("color").m<&ai_setColor>("setColor:", "v@:@")
      .m<&ai_drawRect>("drawRect:", "v@:" + R);

    auto& as = p.service<AlertState>();
    ObjC::ClassBuilder handler = r.defineClass("__HVMAlertHandler", "NSObject");
    as.handler = handler.cls();
    handler.m<&handler_button>("_hvmAlertButton:", "v@:@");
    for (const char* name : {"UIAlertView", "UIActionSheet"}) {
        ObjC::ClassBuilder av = r.defineClass(name, "UIView");
        if (std::string(name) == "UIAlertView") av.m<&av_initFull>("initWithTitle:message:delegate:cancelButtonTitle:otherButtonTitles:");
        else av.m<&av_initSheet>("initWithTitle:delegate:cancelButtonTitle:destructiveButtonTitle:otherButtonTitles:");
        av.m<&av_dealloc>("dealloc", "v@:").m<&av_addButton>("addButtonWithTitle:", "i@:@").m<&av_count>("numberOfButtons", "i@:")
          .m<&av_buttonTitle>("buttonTitleAtIndex:").m<&av_cancelIndex>("cancelButtonIndex", "i@:").m<&av_setCancelIndex>("setCancelButtonIndex:", "v@:i")
          .m<&av_firstOther>("firstOtherButtonIndex", "i@:").m<&av_destructive>("destructiveButtonIndex", "i@:")
          .m<&av_setDestructive>("setDestructiveButtonIndex:", "v@:i").m<&av_title>("title").m<&av_setTitle>("setTitle:", "v@:@")
          .m<&av_message>("message").m<&av_setMessage>("setMessage:", "v@:@").m<&av_delegate>("delegate").m<&av_setDelegate>("setDelegate:", "v@:@")
          .m<&av_visible>("isVisible", "c@:").m<&av_style>("alertViewStyle", "i@:").m<&av_setStyle>("setAlertViewStyle:", "v@:i")
          .m<&av_textField>("textFieldAtIndex:").m<&av_show>("show", "v@:").m<&av_showIn>("showInView:", "v@:@")
          .m<&av_showIn>("showFromTabBar:", "v@:@").m<&av_showIn>("showFromToolbar:", "v@:@")
          .m<&av_showFromRect>("showFromRect:inView:animated:", "v@:" + R + "@c")
          .m<&av_dismiss>("dismissWithClickedButtonIndex:animated:", "v@:ic");
    }
}

} // namespace Hydrogen::Runtime::UIKit
