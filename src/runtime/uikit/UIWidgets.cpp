// HydrogenVM — Extended UIKit widgets, containers, and storyboards:
// UIScrollView, UITableView, UITableViewCell, UITableViewController,
// UIBarItem, UIBarButtonItem, UINavigationItem, UINavigationBar, UIToolbar,
// UINavigationController, UITabBarItem, UITabBar, UITabBarController,
// UITextField, UITextView, UISwitch, UISlider, UISegmentedControl,
// UIProgressView, UIPageControl, UIWebView, UIStoryboard, UIStoryboardSegue.
#include "UIKit.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#else
#include <unicode/ubrk.h>
#endif

namespace fs = std::filesystem;

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

id makeIndexPath(Ctx& x, int32_t row, int32_t section) {
    const Class cls = rt(x).getClass("NSIndexPath");
    return id(rt(x).send(x.cpu, cls, "indexPathForRow:inSection:", {uint32_t(row), uint32_t(section)}));
}
int32_t indexPathRow(Ctx& x, id ip) { return ip ? int32_t(rt(x).send(x.cpu, ip, "row")) : -1; }
int32_t indexPathSection(Ctx& x, id ip) { return ip ? int32_t(rt(x).send(x.cpu, ip, "section")) : -1; }

G::Point touchInView(Ctx& x, id touches, id view) {
    const id t = id(rt(x).send(x.cpu, touches, "anyObject"));
    if (!t) return {};
    const uint32_t tmp = x.proc.heap().allocZeroed(8);
    const IMP imp = rt(x).lookupForSend(rt(x).classOf(t), rt(x).sel("locationInView:"));
    x.proc.callGuest(x.cpu, imp, {tmp, t, rt(x).sel("locationInView:"), view});
    const CGPoint p = CG::readStruct<CGPoint>(x, tmp);
    x.proc.heap().free(tmp);
    return CG::toG(p);
}

// ============================================================ UIScrollView ===

struct ScrollData : ObjC::HostObject {
    G::Size contentSize{0, 0};
    UIEdgeInsets contentInset{0, 0, 0, 0};
    UIEdgeInsets indicatorInsets{0, 0, 0, 0};
    id delegate = 0;   // unretained
    bool scrollEnabled = true, pagingEnabled = false, bounces = true;
    bool alwaysBounceV = false, alwaysBounceH = false;
    bool showsH = true, showsV = true;
    bool delaysTouches = true, canCancelTouches = true, dirLock = false, scrollsToTop = true;
    bool tracking = false, dragging = false, decelerating = false, zooming = false;
    int32_t indicatorStyle = 0;
    float decelerationRate = 0.998f;
    float zoomScale = 1.0f, minZoom = 1.0f, maxZoom = 1.0f;
    G::Point dragStartScreen{}, dragStartOffset{};
};
ScrollData& SCR(Ctx& x, id s) { return rt(x).host<ScrollData>(s); }

id sv_initWithFrame(Ctx& x, id self, SEL s, CGRect f) {
    const id me = id(rt(x).sendSuper(x.cpu, self, ui(x).view, s, {fbits(f.origin.x), fbits(f.origin.y), fbits(f.size.width), fbits(f.size.height)}));
    if (me) {
        SCR(x, me);
        rt(x).send(x.cpu, me, "setClipsToBounds:", {1});
    }
    return me;
}
id sv_initWithCoder(Ctx& x, id self, SEL s, id coder) {
    const id me = id(rt(x).sendSuper(x.cpu, self, ui(x).view, s, {coder}));
    if (me) {
        ScrollData& d = SCR(x, me);
        rt(x).send(x.cpu, me, "setClipsToBounds:", {1});
        if (has(x, coder, "UIScrollDisabled")) d.scrollEnabled = !decodeBool(x, coder, "UIScrollDisabled");
        if (has(x, coder, "UIPagingEnabled")) d.pagingEnabled = decodeBool(x, coder, "UIPagingEnabled");
        if (has(x, coder, "UIBounces")) d.bounces = decodeBool(x, coder, "UIBounces");
        if (has(x, coder, "UIShowsHorizontalScrollIndicator")) d.showsH = decodeBool(x, coder, "UIShowsHorizontalScrollIndicator");
        if (has(x, coder, "UIShowsVerticalScrollIndicator")) d.showsV = decodeBool(x, coder, "UIShowsVerticalScrollIndicator");
    }
    return me;
}
void sv_contentOffset(Ctx& x, GuestPtr ret, id self, SEL) {
    const G::Rect b = boundsOf(x, self);
    CG::writeStruct(x, ret, CGPoint{b.x, b.y});
}
void sv_setContentOffset(Ctx& x, id self, SEL, CGPoint p) {
    LayerData& l = layerData(x, viewLayer(x, self));
    if (l.bounds.x == p.x && l.bounds.y == p.y) return;
    l.bounds.x = p.x;
    l.bounds.y = p.y;
    viewData(x, self).needsLayout = true;
    setNeedsComposite(x);
    const id del = SCR(x, self).delegate;
    if (del && rt(x).respondsTo(del, rt(x).sel("scrollViewDidScroll:")))
        rt(x).send(x.cpu, del, "scrollViewDidScroll:", {self});
}
void sv_setContentOffsetAnim(Ctx& x, id self, SEL s, CGPoint p, bool) { sv_setContentOffset(x, self, s, p); }
void sv_contentSize(Ctx& x, GuestPtr ret, id self, SEL) {
    const G::Size cs = SCR(x, self).contentSize;
    CG::writeStruct(x, ret, CGSize{cs.w, cs.h});
}
void sv_setContentSize(Ctx& x, id self, SEL, CGSize sz) {
    SCR(x, self).contentSize = {sz.width, sz.height};
    setNeedsComposite(x);
}
void sv_contentInset(Ctx& x, GuestPtr ret, id self, SEL) { CG::writeStruct(x, ret, SCR(x, self).contentInset); }
void sv_setContentInset(Ctx& x, id self, SEL, UIEdgeInsets in) { SCR(x, self).contentInset = in; }
void sv_indicatorInsets(Ctx& x, GuestPtr ret, id self, SEL) { CG::writeStruct(x, ret, SCR(x, self).indicatorInsets); }
void sv_setIndicatorInsets(Ctx& x, id self, SEL, UIEdgeInsets in) { SCR(x, self).indicatorInsets = in; }
id sv_delegate(Ctx& x, id self, SEL) { return SCR(x, self).delegate; }
void sv_setDelegate(Ctx& x, id self, SEL, id d) { SCR(x, self).delegate = d; }
bool sv_scrollEnabled(Ctx& x, id self, SEL) { return SCR(x, self).scrollEnabled; }
void sv_setScrollEnabled(Ctx& x, id self, SEL, bool b) { SCR(x, self).scrollEnabled = b; }
bool sv_pagingEnabled(Ctx& x, id self, SEL) { return SCR(x, self).pagingEnabled; }
void sv_setPagingEnabled(Ctx& x, id self, SEL, bool b) { SCR(x, self).pagingEnabled = b; }
bool sv_bounces(Ctx& x, id self, SEL) { return SCR(x, self).bounces; }
void sv_setBounces(Ctx& x, id self, SEL, bool b) { SCR(x, self).bounces = b; }
bool sv_alwaysV(Ctx& x, id self, SEL) { return SCR(x, self).alwaysBounceV; }
void sv_setAlwaysV(Ctx& x, id self, SEL, bool b) { SCR(x, self).alwaysBounceV = b; }
bool sv_alwaysH(Ctx& x, id self, SEL) { return SCR(x, self).alwaysBounceH; }
void sv_setAlwaysH(Ctx& x, id self, SEL, bool b) { SCR(x, self).alwaysBounceH = b; }
bool sv_showsH(Ctx& x, id self, SEL) { return SCR(x, self).showsH; }
void sv_setShowsH(Ctx& x, id self, SEL, bool b) { SCR(x, self).showsH = b; }
bool sv_showsV(Ctx& x, id self, SEL) { return SCR(x, self).showsV; }
void sv_setShowsV(Ctx& x, id self, SEL, bool b) { SCR(x, self).showsV = b; }
bool sv_tracking(Ctx& x, id self, SEL) { return SCR(x, self).tracking; }
bool sv_dragging(Ctx& x, id self, SEL) { return SCR(x, self).dragging; }
bool sv_decelerating(Ctx& x, id self, SEL) { return SCR(x, self).decelerating; }
bool sv_zooming(Ctx& x, id self, SEL) { return SCR(x, self).zooming; }
float sv_zoomScale(Ctx& x, id self, SEL) { return SCR(x, self).zoomScale; }
void sv_setZoomScale(Ctx& x, id self, SEL, float s) { SCR(x, self).zoomScale = s; }
void sv_setZoomScaleAnim(Ctx& x, id self, SEL s, float z, bool) { sv_setZoomScale(x, self, s, z); }
float sv_minZoom(Ctx& x, id self, SEL) { return SCR(x, self).minZoom; }
void sv_setMinZoom(Ctx& x, id self, SEL, float s) { SCR(x, self).minZoom = s; }
float sv_maxZoom(Ctx& x, id self, SEL) { return SCR(x, self).maxZoom; }
void sv_setMaxZoom(Ctx& x, id self, SEL, float s) { SCR(x, self).maxZoom = s; }
int32_t sv_indStyle(Ctx& x, id self, SEL) { return SCR(x, self).indicatorStyle; }
void sv_setIndStyle(Ctx& x, id self, SEL, int32_t s) { SCR(x, self).indicatorStyle = s; }
float sv_decelRate(Ctx& x, id self, SEL) { return SCR(x, self).decelerationRate; }
void sv_setDecelRate(Ctx& x, id self, SEL, float r) { SCR(x, self).decelerationRate = r; }
bool sv_delays(Ctx& x, id self, SEL) { return SCR(x, self).delaysTouches; }
void sv_setDelays(Ctx& x, id self, SEL, bool b) { SCR(x, self).delaysTouches = b; }
bool sv_canCancel(Ctx& x, id self, SEL) { return SCR(x, self).canCancelTouches; }
void sv_setCanCancel(Ctx& x, id self, SEL, bool b) { SCR(x, self).canCancelTouches = b; }
bool sv_dirLock(Ctx& x, id self, SEL) { return SCR(x, self).dirLock; }
void sv_setDirLock(Ctx& x, id self, SEL, bool b) { SCR(x, self).dirLock = b; }
bool sv_scrollsToTop(Ctx& x, id self, SEL) { return SCR(x, self).scrollsToTop; }
void sv_setScrollsToTop(Ctx& x, id self, SEL, bool b) { SCR(x, self).scrollsToTop = b; }
void sv_flash(Ctx&, id, SEL) {}
void sv_zoomToRect(Ctx&, id, SEL, CGRect, bool) {}

void sv_scrollRectToVisible(Ctx& x, id self, SEL, CGRect r, bool) {
    const G::Rect b = boundsOf(x, self);
    float ox = b.x, oy = b.y;
    if (r.origin.x < ox) ox = r.origin.x;
    else if (r.origin.x + r.size.width > ox + b.w) ox = r.origin.x + r.size.width - b.w;
    if (r.origin.y < oy) oy = r.origin.y;
    else if (r.origin.y + r.size.height > oy + b.h) oy = r.origin.y + r.size.height - b.h;
    sv_setContentOffset(x, self, 0, {ox, oy});
}

void sv_touchesBegan(Ctx& x, id self, SEL, id touches, id) {
    ScrollData& d = SCR(x, self);
    if (!d.scrollEnabled) return;
    d.tracking = true;
    d.dragging = false;
    d.dragStartScreen = touchInView(x, touches, 0);
    const G::Rect b = boundsOf(x, self);
    d.dragStartOffset = {b.x, b.y};
}
void sv_touchesMoved(Ctx& x, id self, SEL, id touches, id) {
    ScrollData& d = SCR(x, self);
    if (!d.scrollEnabled || !d.tracking) return;
    const G::Point cur = touchInView(x, touches, 0);
    const float dx = d.dragStartScreen.x - cur.x;
    const float dy = d.dragStartScreen.y - cur.y;
    if (!d.dragging && (std::abs(dx) > 4 || std::abs(dy) > 4)) {
        d.dragging = true;
        if (d.delegate && rt(x).respondsTo(d.delegate, rt(x).sel("scrollViewWillBeginDragging:")))
            rt(x).send(x.cpu, d.delegate, "scrollViewWillBeginDragging:", {self});
    }
    if (d.dragging) {
        const G::Rect b = boundsOf(x, self);
        const float maxX = std::max(0.0f, d.contentSize.w - b.w);
        const float maxY = std::max(0.0f, d.contentSize.h - b.h);
        const float nx = std::clamp(d.dragStartOffset.x + dx, 0.0f, maxX);
        const float ny = std::clamp(d.dragStartOffset.y + dy, 0.0f, maxY);
        sv_setContentOffset(x, self, 0, {nx, ny});
    }
}
void sv_touchesEnded(Ctx& x, id self, SEL, id, id) {
    ScrollData& d = SCR(x, self);
    const bool wasDragging = d.dragging;
    d.tracking = false;
    d.dragging = false;
    if (wasDragging && d.delegate && rt(x).respondsTo(d.delegate, rt(x).sel("scrollViewDidEndDragging:willDecelerate:")))
        rt(x).send(x.cpu, d.delegate, "scrollViewDidEndDragging:willDecelerate:", {self, 0});
}

// ========================================================= UITableViewCell ===

struct CellData : ObjC::HostObject {
    int32_t style = 0, selectionStyle = 1, accessoryType = 0, editingAccessoryType = 0, indentationLevel = 0;
    float indentationWidth = 10.0f;
    UIEdgeInsets separatorInset{0, 15, 0, 0};
    id reuseIdentifier = 0;   // retained NSString
    id contentView = 0, textLabel = 0, detailTextLabel = 0, imageView = 0;
    id backgroundView = 0, selectedBackgroundView = 0, accessoryView = 0;
    bool selected = false, highlighted = false, editing = false;
    id ownerTable = 0;        // unretained UITableView
    int32_t section = 0, row = 0;
};
CellData& CLD(Ctx& x, id c) { return rt(x).host<CellData>(c); }

void ensureCellSubviews(Ctx& x, id self) {
    CellData& d = CLD(x, self);
    if (!d.contentView) {
        const G::Rect b = boundsOf(x, self);
        d.contentView = allocFrame(x, ui(x).view, {0, 0, b.w > 0 ? b.w : 320, b.h > 0 ? b.h : 44});
        viewData(x, d.contentView).userInteraction = true;
        rt(x).send(x.cpu, d.contentView, "setBackgroundColor:", {makeUIColor(x, {0, 0, 0, 0})});
        rt(x).send(x.cpu, self, "addSubview:", {d.contentView});
    }
}

id tc_initWithStyle(Ctx& x, id self, SEL, int32_t style, id reuseId) {
    const id me = id(rt(x).sendSuper(x.cpu, self, ui(x).view, rt(x).sel("initWithFrame:"), {fbits(0), fbits(0), fbits(320), fbits(44)}));
    if (!me) return 0;
    CellData& d = CLD(x, me);
    d.style = style;
    setRetained(x, d.reuseIdentifier, reuseId ? autoreleased(x, id(rt(x).send(x.cpu, reuseId, "copy"))) : 0);
    rt(x).send(x.cpu, me, "setBackgroundColor:", {makeUIColor(x, {1, 1, 1, 1})});
    ensureCellSubviews(x, me);
    return me;
}
id tc_initWithFrameReuse(Ctx& x, id self, SEL, CGRect f, id reuseId) {
    const id me = tc_initWithStyle(x, self, 0, 0, reuseId);
    if (me) sendRect(x, me, rt(x).sel("setFrame:"), CG::toG(f));
    return me;
}
id tc_initWithFrame(Ctx& x, id self, SEL, CGRect f) { return tc_initWithFrameReuse(x, self, 0, f, 0); }
id tc_initWithCoder(Ctx& x, id self, SEL s, id coder) {
    const id me = id(rt(x).sendSuper(x.cpu, self, ui(x).view, s, {coder}));
    if (me) {
        CellData& d = CLD(x, me);
        setRetained(x, d.reuseIdentifier, decode(x, coder, "UIReuseIdentifier"));
        ensureCellSubviews(x, me);
    }
    return me;
}
void tc_dealloc(Ctx& x, id self, SEL s) {
    if (CellData* d = rt(x).hostIf<CellData>(self)) {
        for (id* o : {&d->reuseIdentifier, &d->contentView, &d->textLabel, &d->detailTextLabel, &d->imageView,
                      &d->backgroundView, &d->selectedBackgroundView, &d->accessoryView}) {
            if (*o) rt(x).release(x.cpu, *o);
            *o = 0;
        }
    }
    rt(x).sendSuper(x.cpu, self, ui(x).view, s);
}
id tc_contentView(Ctx& x, id self, SEL) { ensureCellSubviews(x, self); return CLD(x, self).contentView; }
id tc_textLabel(Ctx& x, id self, SEL) {
    ensureCellSubviews(x, self);
    CellData& d = CLD(x, self);
    if (!d.textLabel) {
        d.textLabel = allocFrame(x, ui(x).label, {15, 0, 270, 43});
        rt(x).send(x.cpu, d.textLabel, "setBackgroundColor:", {makeUIColor(x, {0, 0, 0, 0})});
        rt(x).send(x.cpu, d.textLabel, "setOpaque:", {0});
        rt(x).send(x.cpu, d.textLabel, "setFont:", {systemFont(x, 18, true)});
        rt(x).send(x.cpu, d.contentView, "addSubview:", {d.textLabel});
    }
    return d.textLabel;
}
id tc_detailLabel(Ctx& x, id self, SEL) {
    ensureCellSubviews(x, self);
    CellData& d = CLD(x, self);
    if (!d.detailTextLabel) {
        d.detailTextLabel = allocFrame(x, ui(x).label, {15, 24, 270, 18});
        rt(x).send(x.cpu, d.detailTextLabel, "setBackgroundColor:", {makeUIColor(x, {0, 0, 0, 0})});
        rt(x).send(x.cpu, d.detailTextLabel, "setOpaque:", {0});
        rt(x).send(x.cpu, d.detailTextLabel, "setFont:", {systemFont(x, 14, false)});
        rt(x).send(x.cpu, d.detailTextLabel, "setTextColor:", {makeUIColor(x, {0.5f, 0.5f, 0.55f, 1})});
        rt(x).send(x.cpu, d.contentView, "addSubview:", {d.detailTextLabel});
    }
    return d.detailTextLabel;
}
id tc_imageView(Ctx& x, id self, SEL) {
    ensureCellSubviews(x, self);
    CellData& d = CLD(x, self);
    if (!d.imageView) {
        d.imageView = allocFrame(x, ui(x).imageView, {8, 6, 32, 32});
        rt(x).send(x.cpu, d.contentView, "addSubview:", {d.imageView});
    }
    return d.imageView;
}
id tc_reuseId(Ctx& x, id self, SEL) { return CLD(x, self).reuseIdentifier; }
void tc_prepareForReuse(Ctx& x, id self, SEL) {
    CellData& d = CLD(x, self);
    d.selected = false;
    d.highlighted = false;
}
int32_t tc_selStyle(Ctx& x, id self, SEL) { return CLD(x, self).selectionStyle; }
void tc_setSelStyle(Ctx& x, id self, SEL, int32_t s) { CLD(x, self).selectionStyle = s; }
int32_t tc_accType(Ctx& x, id self, SEL) { return CLD(x, self).accessoryType; }
void tc_setAccType(Ctx& x, id self, SEL, int32_t t) { CLD(x, self).accessoryType = t; rt(x).send(x.cpu, self, "setNeedsLayout"); }
id tc_accView(Ctx& x, id self, SEL) { return CLD(x, self).accessoryView; }
void tc_setAccView(Ctx& x, id self, SEL, id v) {
    CellData& d = CLD(x, self);
    if (d.accessoryView) rt(x).send(x.cpu, d.accessoryView, "removeFromSuperview");
    setRetained(x, d.accessoryView, v);
    if (v) rt(x).send(x.cpu, self, "addSubview:", {v});
    rt(x).send(x.cpu, self, "setNeedsLayout");
}
id tc_bgView(Ctx& x, id self, SEL) { return CLD(x, self).backgroundView; }
void tc_setBgView(Ctx& x, id self, SEL, id v) { setRetained(x, CLD(x, self).backgroundView, v); }
id tc_selBgView(Ctx& x, id self, SEL) { return CLD(x, self).selectedBackgroundView; }
void tc_setSelBgView(Ctx& x, id self, SEL, id v) { setRetained(x, CLD(x, self).selectedBackgroundView, v); }
bool tc_selected(Ctx& x, id self, SEL) { return CLD(x, self).selected; }
void tc_setSelected(Ctx& x, id self, SEL, bool b) {
    CLD(x, self).selected = b;
    rt(x).send(x.cpu, self, "setBackgroundColor:", {makeUIColor(x, b ? G::Color{0.85f, 0.9f, 0.98f, 1} : G::Color{1, 1, 1, 1})});
}
void tc_setSelectedAnim(Ctx& x, id self, SEL s, bool b, bool) { tc_setSelected(x, self, s, b); }
bool tc_highlighted(Ctx& x, id self, SEL) { return CLD(x, self).highlighted; }
void tc_setHighlighted(Ctx& x, id self, SEL, bool b) { CLD(x, self).highlighted = b; }
void tc_setHighlightedAnim(Ctx& x, id self, SEL s, bool b, bool) { tc_setHighlighted(x, self, s, b); }
bool tc_editing(Ctx& x, id self, SEL) { return CLD(x, self).editing; }
void tc_setEditing(Ctx& x, id self, SEL, bool b) { CLD(x, self).editing = b; }
void tc_setEditingAnim(Ctx& x, id self, SEL s, bool b, bool) { tc_setEditing(x, self, s, b); }

void tc_layoutSubviews(Ctx& x, id self, SEL) {
    ensureCellSubviews(x, self);
    CellData& d = CLD(x, self);
    const G::Rect b = boundsOf(x, self);
    sendRect(x, d.contentView, rt(x).sel("setFrame:"), {0, 0, b.w, b.h});
    float left = 15.0f + float(d.indentationLevel) * d.indentationWidth;
    if (d.imageView && rt(x).send(x.cpu, d.imageView, "image")) {
        const float sz = std::min(32.0f, b.h - 8.0f);
        sendRect(x, d.imageView, rt(x).sel("setFrame:"), {8, (b.h - sz) / 2, sz, sz});
        left = 8 + sz + 10;
    }
    float right = b.w - 15.0f;
    if (d.accessoryView) {
        const G::Rect af = boundsOf(x, d.accessoryView);
        sendRect(x, d.accessoryView, rt(x).sel("setFrame:"), {b.w - af.w - 10, (b.h - af.h) / 2, af.w, af.h});
        right = b.w - af.w - 16;
    }
    const float w = std::max(0.0f, right - left);
    if (d.textLabel && d.detailTextLabel && d.style == 3 /* Subtitle */) {
        sendRect(x, d.textLabel, rt(x).sel("setFrame:"), {left, 4, w, b.h * 0.52f});
        sendRect(x, d.detailTextLabel, rt(x).sel("setFrame:"), {left, b.h * 0.52f, w, b.h * 0.42f});
    } else if (d.textLabel && d.detailTextLabel && (d.style == 1 || d.style == 2)) {
        sendRect(x, d.textLabel, rt(x).sel("setFrame:"), {left, 0, w * 0.6f, b.h - 1});
        sendRect(x, d.detailTextLabel, rt(x).sel("setFrame:"), {left + w * 0.6f, 0, w * 0.4f, b.h - 1});
    } else if (d.textLabel) {
        sendRect(x, d.textLabel, rt(x).sel("setFrame:"), {left, 0, w, b.h - 1});
    }
}

void tvSelectRowFromTap(Ctx& x, id table, int32_t section, int32_t row);

void tc_touchesEnded(Ctx& x, id self, SEL, id, id) {
    CellData& d = CLD(x, self);
    if (d.ownerTable) tvSelectRowFromTap(x, d.ownerTable, d.section, d.row);
}

// ============================================================= UITableView ===

struct RowRecord {
    int32_t section = 0, row = 0;
    float y = 0, h = 44;
    id cell = 0;   // retained while visible
};

struct TableData : ObjC::HostObject {
    int32_t style = 0, separatorStyle = 1;
    id dataSource = 0;   // unretained
    float rowHeight = 44.0f, sectionHeaderHeight = 22.0f, sectionFooterHeight = 22.0f;
    id separatorColor = 0, tableHeaderView = 0, tableFooterView = 0, backgroundView = 0;
    bool allowsSelection = true, allowsMultipleSelection = false, editing = false;
    bool needsReload = true;
    int32_t selectedSection = -1, selectedRow = -1;
    std::vector<int32_t> rowsPerSection;
    std::vector<RowRecord> rows;
    std::unordered_map<std::string, Class> registeredClasses;
    std::unordered_map<std::string, id> registeredNibs;
    std::unordered_map<std::string, std::vector<id>> reusePool;
};
TableData& TVD(Ctx& x, id t) { return rt(x).host<TableData>(t); }

void tv_reloadData(Ctx& x, id self, SEL);

id tv_initWithFrameStyle(Ctx& x, id self, SEL, CGRect f, int32_t style) {
    const id me = sv_initWithFrame(x, self, rt(x).sel("initWithFrame:"), f);
    if (me) {
        TableData& d = TVD(x, me);
        d.style = style;
        rt(x).send(x.cpu, me, "setBackgroundColor:", {makeUIColor(x, style == 1 ? G::Color{0.93f, 0.93f, 0.96f, 1} : G::Color{1, 1, 1, 1})});
    }
    return me;
}
id tv_initWithFrame(Ctx& x, id self, SEL s, CGRect f) { return tv_initWithFrameStyle(x, self, s, f, 0); }
id tv_initWithCoder(Ctx& x, id self, SEL s, id coder) {
    const id me = sv_initWithCoder(x, self, s, coder);
    if (me) {
        TableData& d = TVD(x, me);
        if (has(x, coder, "UIStyle")) d.style = decodeInt(x, coder, "UIStyle");
        if (has(x, coder, "UIRowHeight")) d.rowHeight = decodeFloat(x, coder, "UIRowHeight");
        setRetained(x, d.tableHeaderView, decode(x, coder, "UITableHeaderView"));
        setRetained(x, d.tableFooterView, decode(x, coder, "UITableFooterView"));
    }
    return me;
}
void tv_dealloc(Ctx& x, id self, SEL s) {
    if (TableData* d = rt(x).hostIf<TableData>(self)) {
        for (auto& r : d->rows) if (r.cell) rt(x).release(x.cpu, r.cell);
        d->rows.clear();
        for (auto& [k, vec] : d->reusePool) for (id c : vec) rt(x).release(x.cpu, c);
        d->reusePool.clear();
        for (auto& [k, nib] : d->registeredNibs) if (nib) rt(x).release(x.cpu, nib);
        d->registeredNibs.clear();
        for (id* o : {&d->separatorColor, &d->tableHeaderView, &d->tableFooterView, &d->backgroundView}) {
            if (*o) rt(x).release(x.cpu, *o);
            *o = 0;
        }
    }
    rt(x).sendSuper(x.cpu, self, rt(x).getClass("UIScrollView"), s);
}

int32_t tv_style(Ctx& x, id self, SEL) { return TVD(x, self).style; }
id tv_dataSource(Ctx& x, id self, SEL) { return TVD(x, self).dataSource; }
void tv_setDataSource(Ctx& x, id self, SEL, id ds) { TVD(x, self).dataSource = ds; TVD(x, self).needsReload = true; rt(x).send(x.cpu, self, "setNeedsLayout"); }
float tv_rowHeight(Ctx& x, id self, SEL) { return TVD(x, self).rowHeight; }
void tv_setRowHeight(Ctx& x, id self, SEL, float h) { TVD(x, self).rowHeight = h > 0 ? h : 44.0f; }
float tv_secHeaderH(Ctx& x, id self, SEL) { return TVD(x, self).sectionHeaderHeight; }
void tv_setSecHeaderH(Ctx& x, id self, SEL, float h) { TVD(x, self).sectionHeaderHeight = h; }
float tv_secFooterH(Ctx& x, id self, SEL) { return TVD(x, self).sectionFooterHeight; }
void tv_setSecFooterH(Ctx& x, id self, SEL, float h) { TVD(x, self).sectionFooterHeight = h; }
int32_t tv_sepStyle(Ctx& x, id self, SEL) { return TVD(x, self).separatorStyle; }
void tv_setSepStyle(Ctx& x, id self, SEL, int32_t s) { TVD(x, self).separatorStyle = s; }
id tv_sepColor(Ctx& x, id self, SEL) { return TVD(x, self).separatorColor; }
void tv_setSepColor(Ctx& x, id self, SEL, id c) { setRetained(x, TVD(x, self).separatorColor, c); }
id tv_headerView(Ctx& x, id self, SEL) { return TVD(x, self).tableHeaderView; }
void tv_setHeaderView(Ctx& x, id self, SEL, id v) {
    TableData& d = TVD(x, self);
    if (d.tableHeaderView) rt(x).send(x.cpu, d.tableHeaderView, "removeFromSuperview");
    setRetained(x, d.tableHeaderView, v);
    if (v) rt(x).send(x.cpu, self, "addSubview:", {v});
    d.needsReload = true;
    rt(x).send(x.cpu, self, "setNeedsLayout");
}
id tv_footerView(Ctx& x, id self, SEL) { return TVD(x, self).tableFooterView; }
void tv_setFooterView(Ctx& x, id self, SEL, id v) {
    TableData& d = TVD(x, self);
    if (d.tableFooterView) rt(x).send(x.cpu, d.tableFooterView, "removeFromSuperview");
    setRetained(x, d.tableFooterView, v);
    if (v) rt(x).send(x.cpu, self, "addSubview:", {v});
    d.needsReload = true;
    rt(x).send(x.cpu, self, "setNeedsLayout");
}
id tv_bgView(Ctx& x, id self, SEL) { return TVD(x, self).backgroundView; }
void tv_setBgView(Ctx& x, id self, SEL, id v) { setRetained(x, TVD(x, self).backgroundView, v); }
bool tv_allowsSel(Ctx& x, id self, SEL) { return TVD(x, self).allowsSelection; }
void tv_setAllowsSel(Ctx& x, id self, SEL, bool b) { TVD(x, self).allowsSelection = b; }
bool tv_allowsMulti(Ctx& x, id self, SEL) { return TVD(x, self).allowsMultipleSelection; }
void tv_setAllowsMulti(Ctx& x, id self, SEL, bool b) { TVD(x, self).allowsMultipleSelection = b; }
bool tv_editing(Ctx& x, id self, SEL) { return TVD(x, self).editing; }
void tv_setEditing(Ctx& x, id self, SEL, bool b) { TVD(x, self).editing = b; }
void tv_setEditingAnim(Ctx& x, id self, SEL s, bool b, bool) { tv_setEditing(x, self, s, b); }

void tv_registerClass(Ctx& x, id self, SEL, Class cls, id reuseId) {
    if (reuseId) TVD(x, self).registeredClasses[utf8(x, reuseId)] = cls;
}
void tv_registerNib(Ctx& x, id self, SEL, id nib, id reuseId) {
    if (!reuseId) return;
    const std::string k = utf8(x, reuseId);
    auto& m = TVD(x, self).registeredNibs;
    if (m[k]) rt(x).release(x.cpu, m[k]);
    m[k] = nib ? rt(x).retain(x.cpu, nib) : 0;
}
id tv_dequeue(Ctx& x, id self, SEL, id reuseId) {
    if (!reuseId) return 0;
    const std::string k = utf8(x, reuseId);
    TableData& d = TVD(x, self);
    auto it = d.reusePool.find(k);
    if (it != d.reusePool.end() && !it->second.empty()) {
        const id cell = it->second.back();
        it->second.pop_back();
        rt(x).send(x.cpu, cell, "prepareForReuse");
        return autoreleased(x, cell);
    }
    auto cit = d.registeredClasses.find(k);
    if (cit != d.registeredClasses.end() && cit->second) {
        const id cell = id(rt(x).send(x.cpu, id(rt(x).send(x.cpu, cit->second, rt(x).selAlloc)), "initWithStyle:reuseIdentifier:", {0, reuseId}));
        return autoreleased(x, cell);
    }
    auto nit = d.registeredNibs.find(k);
    if (nit != d.registeredNibs.end() && nit->second) {
        const id arr = id(rt(x).send(x.cpu, nit->second, "instantiateWithOwner:options:", {self, 0}));
        const auto items = Foundation::arrayItems(x, arr);
        if (!items.empty()) return items.front();
    }
    return 0;
}
id tv_dequeueForIndexPath(Ctx& x, id self, SEL s, id reuseId, id) {
    const id c = tv_dequeue(x, self, s, reuseId);
    if (c) return c;
    const Class cls = rt(x).getClass("UITableViewCell");
    return autoreleased(x, id(rt(x).send(x.cpu, id(rt(x).send(x.cpu, cls, rt(x).selAlloc)), "initWithStyle:reuseIdentifier:", {0, reuseId})));
}

void tv_reloadData(Ctx& x, id self, SEL) {
    TableData& d = TVD(x, self);
    d.needsReload = false;
    auto& r = rt(x);
    for (auto& row : d.rows) {
        if (row.cell) {
            const id rid = CLD(x, row.cell).reuseIdentifier;
            r.send(x.cpu, row.cell, "removeFromSuperview");
            if (rid) d.reusePool[utf8(x, rid)].push_back(row.cell);
            else r.release(x.cpu, row.cell);
            row.cell = 0;
        }
    }
    d.rows.clear();
    d.rowsPerSection.clear();

    if (!d.dataSource) return;
    int32_t sections = 1;
    if (r.respondsTo(d.dataSource, r.sel("numberOfSectionsInTableView:")))
        sections = std::max(0, int32_t(r.send(x.cpu, d.dataSource, "numberOfSectionsInTableView:", {self})));

    const G::Rect b = boundsOf(x, self);
    const float W = b.w > 0 ? b.w : 320.0f;
    float y = 0;
    if (d.tableHeaderView) {
        const G::Rect hf = boundsOf(x, d.tableHeaderView);
        sendRect(x, d.tableHeaderView, r.sel("setFrame:"), {0, 0, W, hf.h});
        y += hf.h;
    }

    const id del = SCR(x, self).delegate;
    const bool hasRowHeightDel = del && r.respondsTo(del, r.sel("tableView:heightForRowAtIndexPath:"));

    for (int32_t sec = 0; sec < sections; ++sec) {
        int32_t nrows = 0;
        if (r.respondsTo(d.dataSource, r.sel("tableView:numberOfRowsInSection:")))
            nrows = std::max(0, int32_t(r.send(x.cpu, d.dataSource, "tableView:numberOfRowsInSection:", {self, uint32_t(sec)})));
        d.rowsPerSection.push_back(nrows);

        for (int32_t row = 0; row < nrows; ++row) {
            const id ip = makeIndexPath(x, row, sec);
            float rh = d.rowHeight > 0 ? d.rowHeight : 44.0f;
            if (hasRowHeightDel) {
                rh = bitsf(uint32_t(r.send(x.cpu, del, "tableView:heightForRowAtIndexPath:", {self, ip})));
                if (rh <= 0) rh = 44.0f;
            }
            id cell = 0;
            if (r.respondsTo(d.dataSource, r.sel("tableView:cellForRowAtIndexPath:")))
                cell = id(r.send(x.cpu, d.dataSource, "tableView:cellForRowAtIndexPath:", {self, ip}));
            if (cell) {
                r.retain(x.cpu, cell);
                CellData& cd = CLD(x, cell);
                cd.ownerTable = self;
                cd.section = sec;
                cd.row = row;
                cd.selected = (sec == d.selectedSection && row == d.selectedRow);
                sendRect(x, cell, r.sel("setFrame:"), {0, y, W, rh});
                r.send(x.cpu, self, "addSubview:", {cell});
                if (del && r.respondsTo(del, r.sel("tableView:willDisplayCell:forRowAtIndexPath:")))
                    r.send(x.cpu, del, "tableView:willDisplayCell:forRowAtIndexPath:", {self, cell, ip});
            }
            d.rows.push_back({sec, row, y, rh, cell});
            y += rh;
        }
    }

    if (d.tableFooterView) {
        const G::Rect ff = boundsOf(x, d.tableFooterView);
        sendRect(x, d.tableFooterView, r.sel("setFrame:"), {0, y, W, ff.h});
        y += ff.h;
    }
    SCR(x, self).contentSize = {W, y};
    setNeedsComposite(x);
}

void tv_layoutSubviews(Ctx& x, id self, SEL) {
    TableData& d = TVD(x, self);
    if (d.needsReload) tv_reloadData(x, self, 0);
}

int32_t tv_numberOfSections(Ctx& x, id self, SEL) {
    TableData& d = TVD(x, self);
    if (d.needsReload) tv_reloadData(x, self, 0);
    return int32_t(d.rowsPerSection.size());
}
int32_t tv_numberOfRowsInSection(Ctx& x, id self, SEL, int32_t sec) {
    TableData& d = TVD(x, self);
    if (d.needsReload) tv_reloadData(x, self, 0);
    return (sec >= 0 && size_t(sec) < d.rowsPerSection.size()) ? d.rowsPerSection[size_t(sec)] : 0;
}
id tv_cellForRow(Ctx& x, id self, SEL, id ip) {
    TableData& d = TVD(x, self);
    if (d.needsReload) tv_reloadData(x, self, 0);
    const int32_t sec = indexPathSection(x, ip), row = indexPathRow(x, ip);
    for (const auto& r : d.rows) if (r.section == sec && r.row == row) return r.cell;
    return 0;
}
id tv_visibleCells(Ctx& x, id self, SEL) {
    TableData& d = TVD(x, self);
    if (d.needsReload) tv_reloadData(x, self, 0);
    std::vector<id> out;
    for (const auto& r : d.rows) if (r.cell) out.push_back(r.cell);
    return Foundation::makeArray(x, out);
}
id tv_indexPathsForVisibleRows(Ctx& x, id self, SEL) {
    TableData& d = TVD(x, self);
    if (d.needsReload) tv_reloadData(x, self, 0);
    std::vector<id> out;
    for (const auto& r : d.rows) out.push_back(makeIndexPath(x, r.row, r.section));
    return Foundation::makeArray(x, out);
}
id tv_indexPathForCell(Ctx& x, id self, SEL, id cell) {
    for (const auto& r : TVD(x, self).rows) if (r.cell == cell) return makeIndexPath(x, r.row, r.section);
    return 0;
}
id tv_indexPathForRowAtPoint(Ctx& x, id self, SEL, CGPoint p) {
    TableData& d = TVD(x, self);
    if (d.needsReload) tv_reloadData(x, self, 0);
    for (const auto& r : d.rows) if (p.y >= r.y && p.y < r.y + r.h) return makeIndexPath(x, r.row, r.section);
    return 0;
}
void tv_rectForRow(Ctx& x, GuestPtr ret, id self, SEL, id ip) {
    TableData& d = TVD(x, self);
    if (d.needsReload) tv_reloadData(x, self, 0);
    const int32_t sec = indexPathSection(x, ip), row = indexPathRow(x, ip);
    const float W = boundsOf(x, self).w > 0 ? boundsOf(x, self).w : 320.0f;
    for (const auto& r : d.rows) {
        if (r.section == sec && r.row == row) {
            CG::writeStruct(x, ret, CGRect{{0, r.y}, {W, r.h}});
            return;
        }
    }
    CG::writeStruct(x, ret, CGRect{{0, 0}, {0, 0}});
}
id tv_indexPathForSelectedRow(Ctx& x, id self, SEL) {
    const TableData& d = TVD(x, self);
    return (d.selectedSection >= 0 && d.selectedRow >= 0) ? makeIndexPath(x, d.selectedRow, d.selectedSection) : 0;
}
void tv_selectRow(Ctx& x, id self, SEL, id ip, bool, int32_t) {
    TableData& d = TVD(x, self);
    if (d.needsReload) tv_reloadData(x, self, 0);
    d.selectedSection = indexPathSection(x, ip);
    d.selectedRow = indexPathRow(x, ip);
    for (const auto& r : d.rows)
        if (r.cell) tc_setSelected(x, r.cell, 0, r.section == d.selectedSection && r.row == d.selectedRow);
}
void tv_deselectRow(Ctx& x, id self, SEL, id ip, bool) {
    TableData& d = TVD(x, self);
    const int32_t sec = indexPathSection(x, ip), row = indexPathRow(x, ip);
    if (d.selectedSection == sec && d.selectedRow == row) { d.selectedSection = -1; d.selectedRow = -1; }
    for (const auto& r : d.rows) if (r.section == sec && r.row == row && r.cell) tc_setSelected(x, r.cell, 0, false);
}
void tv_scrollToRow(Ctx& x, id self, SEL, id ip, int32_t, bool) {
    TableData& d = TVD(x, self);
    if (d.needsReload) tv_reloadData(x, self, 0);
    const int32_t sec = indexPathSection(x, ip), row = indexPathRow(x, ip);
    for (const auto& r : d.rows) {
        if (r.section == sec && r.row == row) {
            sv_scrollRectToVisible(x, self, 0, {{0, r.y}, {320, r.h}}, false);
            return;
        }
    }
}
void tv_beginUpdates(Ctx&, id, SEL) {}
void tv_endUpdates(Ctx& x, id self, SEL) { tv_reloadData(x, self, 0); }
void tv_mutateRows(Ctx& x, id self, SEL, id, int32_t) { tv_reloadData(x, self, 0); }

void tvSelectRowFromTap(Ctx& x, id table, int32_t section, int32_t row) {
    TableData& d = TVD(x, table);
    if (!d.allowsSelection) return;
    const id ip = makeIndexPath(x, row, section);
    tv_selectRow(x, table, 0, ip, false, 0);
    const id del = SCR(x, table).delegate;
    if (del && rt(x).respondsTo(del, rt(x).sel("tableView:didSelectRowAtIndexPath:")))
        rt(x).send(x.cpu, del, "tableView:didSelectRowAtIndexPath:", {table, ip});
}

// UITableViewController
struct TVCData : ObjC::HostObject { int32_t style = 0; bool clears = true; };
id tvc_initWithStyle(Ctx& x, id self, SEL, int32_t style) {
    const id me = id(rt(x).sendSuper(x.cpu, self, ui(x).viewController, rt(x).selInit));
    if (me) rt(x).host<TVCData>(me).style = style;
    return me;
}
void tvc_loadView(Ctx& x, id self, SEL) {
    const int32_t style = rt(x).host<TVCData>(self).style;
    const Class tvCls = rt(x).getClass("UITableView");
    const id tv = id(rt(x).send(x.cpu, id(rt(x).send(x.cpu, tvCls, rt(x).selAlloc)), "initWithFrame:style:",
                                {fbits(0), fbits(20), fbits(ui(x).screenW), fbits(ui(x).screenH - 20), uint32_t(style)}));
    rt(x).send(x.cpu, tv, "setDataSource:", {self});
    rt(x).send(x.cpu, tv, "setDelegate:", {self});
    rt(x).send(x.cpu, self, "setView:", {tv});
    rt(x).release(x.cpu, tv);
}
id tvc_tableView(Ctx& x, id self, SEL) { return id(rt(x).send(x.cpu, self, "view")); }
void tvc_setTableView(Ctx& x, id self, SEL, id tv) { rt(x).send(x.cpu, self, "setView:", {tv}); }

// ======================================= Navigation & Tab Bar Controllers ===

struct BarItemData : ObjC::HostObject {
    id title = 0, image = 0, selectedImage = 0, customView = 0, tintColor = 0, badgeValue = 0;
    id target = 0;
    SEL action = 0;
    int32_t style = 0, systemItem = -1, tag = 0;
    float width = 0;
    bool enabled = true;
};
BarItemData& BID(Ctx& x, id b) { return rt(x).host<BarItemData>(b); }

id bi_initWithTitle(Ctx& x, id self, SEL, id title, int32_t style, id target, SEL action) {
    BarItemData& d = BID(x, self);
    setRetained(x, d.title, title ? autoreleased(x, id(rt(x).send(x.cpu, title, "copy"))) : 0);
    d.style = style; d.target = target; d.action = action;
    return self;
}
id bi_initWithImage(Ctx& x, id self, SEL, id image, int32_t style, id target, SEL action) {
    BarItemData& d = BID(x, self);
    setRetained(x, d.image, image);
    d.style = style; d.target = target; d.action = action;
    return self;
}
id bi_initWithSystem(Ctx& x, id self, SEL, int32_t sys, id target, SEL action) {
    BarItemData& d = BID(x, self);
    d.systemItem = sys; d.target = target; d.action = action;
    static const char* const kSysTitles[] = {"Done", "Cancel", "Edit", "Save", "+", "", "", "Compose", "Reply", "Action", "Organize", "Bookmarks", "Search", "Refresh", "Stop", "Camera", "Trash", "Play", "Pause", "Rewind", "FastForward", "Undo", "Redo"};
    if (sys >= 0 && sys < 23 && kSysTitles[sys][0]) setRetained(x, d.title, makeString(x, kSysTitles[sys]));
    return self;
}
id bi_initWithCustomView(Ctx& x, id self, SEL, id view) {
    BarItemData& d = BID(x, self);
    setRetained(x, d.customView, view);
    return self;
}
id bi_initWithCoder(Ctx& x, id self, SEL, id coder) {
    BarItemData& d = BID(x, self);
    setRetained(x, d.title, decode(x, coder, "UITitle"));
    setRetained(x, d.image, decode(x, coder, "UIImage"));
    setRetained(x, d.customView, decode(x, coder, "UICustomView"));
    if (has(x, coder, "UITag")) d.tag = decodeInt(x, coder, "UITag");
    if (has(x, coder, "UIEnabled")) d.enabled = decodeBool(x, coder, "UIEnabled");
    return self;
}
id tbi_initWithTitleImageTag(Ctx& x, id self, SEL, id title, id image, int32_t tag) {
    BarItemData& d = BID(x, self);
    setRetained(x, d.title, title ? autoreleased(x, id(rt(x).send(x.cpu, title, "copy"))) : 0);
    setRetained(x, d.image, image);
    d.tag = tag;
    return self;
}
id tbi_initWithTitleImageSel(Ctx& x, id self, SEL, id title, id image, id selImage) {
    BarItemData& d = BID(x, self);
    setRetained(x, d.title, title ? autoreleased(x, id(rt(x).send(x.cpu, title, "copy"))) : 0);
    setRetained(x, d.image, image);
    setRetained(x, d.selectedImage, selImage);
    return self;
}
id tbi_initWithSystemTag(Ctx& x, id self, SEL, int32_t sys, int32_t tag) {
    BarItemData& d = BID(x, self);
    d.systemItem = sys; d.tag = tag;
    static const char* const kTabNames[] = {"More", "Favorites", "Featured", "Top Rated", "Recents", "Contacts", "History", "Bookmarks", "Search", "Downloads", "Most Recent", "Most Viewed"};
    if (sys >= 0 && sys < 12) setRetained(x, d.title, makeString(x, kTabNames[sys]));
    return self;
}
void bi_dealloc(Ctx& x, id self, SEL) {
    if (BarItemData* d = rt(x).hostIf<BarItemData>(self)) {
        for (id* o : {&d->title, &d->image, &d->selectedImage, &d->customView, &d->tintColor, &d->badgeValue}) {
            if (*o) rt(x).release(x.cpu, *o);
            *o = 0;
        }
    }
    rt(x).disposeInstance(x.cpu, self);
}
id bi_title(Ctx& x, id self, SEL) { return BID(x, self).title; }
void bi_setTitle(Ctx& x, id self, SEL, id t) { setRetained(x, BID(x, self).title, t ? autoreleased(x, id(rt(x).send(x.cpu, t, "copy"))) : 0); }
id bi_image(Ctx& x, id self, SEL) { return BID(x, self).image; }
void bi_setImage(Ctx& x, id self, SEL, id img) { setRetained(x, BID(x, self).image, img); }
id bi_customView(Ctx& x, id self, SEL) { return BID(x, self).customView; }
void bi_setCustomView(Ctx& x, id self, SEL, id v) {
    BarItemData& d = BID(x, self);
    setRetained(x, d.customView, v);
    if (v && d.target && d.action && rt(x).respondsTo(v, rt(x).sel("addTarget:action:forControlEvents:"))) {
        rt(x).send(x.cpu, v, "addTarget:action:forControlEvents:", {d.target, d.action, 1u << 6 /* UIControlEventTouchUpInside */});
    }
}
id bi_target(Ctx& x, id self, SEL) { return BID(x, self).target; }
void bi_setTarget(Ctx& x, id self, SEL, id t) { BID(x, self).target = t; }
SEL bi_action(Ctx& x, id self, SEL) { return BID(x, self).action; }
void bi_setAction(Ctx& x, id self, SEL, SEL a) { BID(x, self).action = a; }
void bi_addTarget(Ctx& x, id self, SEL, id target, SEL action, uint32_t events) {
    BarItemData& d = BID(x, self);
    d.target = target;
    d.action = action;
    if (d.customView && rt(x).respondsTo(d.customView, rt(x).sel("addTarget:action:forControlEvents:"))) {
        rt(x).send(x.cpu, d.customView, "addTarget:action:forControlEvents:", {target, action, events});
    }
}
void bi_removeTarget(Ctx& x, id self, SEL, id target, SEL action, uint32_t events) {
    BarItemData& d = BID(x, self);
    if (d.customView && rt(x).respondsTo(d.customView, rt(x).sel("removeTarget:action:forControlEvents:"))) {
        rt(x).send(x.cpu, d.customView, "removeTarget:action:forControlEvents:", {target, action, events});
    }
    if ((!target || d.target == target) && (!action || d.action == action)) {
        d.target = 0;
        d.action = 0;
    }
}
int32_t bi_style(Ctx& x, id self, SEL) { return BID(x, self).style; }
void bi_setStyle(Ctx& x, id self, SEL, int32_t s) { BID(x, self).style = s; }
float bi_width(Ctx& x, id self, SEL) { return BID(x, self).width; }
void bi_setWidth(Ctx& x, id self, SEL, float w) { BID(x, self).width = w; }
int32_t bi_tag(Ctx& x, id self, SEL) { return BID(x, self).tag; }
void bi_setTag(Ctx& x, id self, SEL, int32_t t) { BID(x, self).tag = t; }
bool bi_enabled(Ctx& x, id self, SEL) { return BID(x, self).enabled; }
void bi_setEnabled(Ctx& x, id self, SEL, bool b) { BID(x, self).enabled = b; }
id bi_tintColor(Ctx& x, id self, SEL) { return BID(x, self).tintColor; }
void bi_setTintColor(Ctx& x, id self, SEL, id c) { setRetained(x, BID(x, self).tintColor, c); }
id tbi_badge(Ctx& x, id self, SEL) { return BID(x, self).badgeValue; }
void tbi_setBadge(Ctx& x, id self, SEL, id b) { setRetained(x, BID(x, self).badgeValue, b ? autoreleased(x, id(rt(x).send(x.cpu, b, "copy"))) : 0); }

// UINavigationItem
struct NavItemData : ObjC::HostObject {
    id title = 0, prompt = 0, backItem = 0, leftItem = 0, rightItem = 0, titleView = 0;
    std::vector<id> leftItems, rightItems;
    bool hidesBack = false;
};
NavItemData& NID(Ctx& x, id n) { return rt(x).host<NavItemData>(n); }

id ni_initWithTitle(Ctx& x, id self, SEL, id title) {
    setRetained(x, NID(x, self).title, title ? autoreleased(x, id(rt(x).send(x.cpu, title, "copy"))) : 0);
    return self;
}
id ni_init(Ctx& x, id self, SEL s) { return ni_initWithTitle(x, self, s, 0); }
id ni_initWithCoder(Ctx& x, id self, SEL, id coder) {
    NavItemData& d = NID(x, self);
    setRetained(x, d.title, decode(x, coder, "UITitle"));
    setRetained(x, d.backItem, decode(x, coder, "UIBackBarButtonItem"));
    setRetained(x, d.leftItem, decode(x, coder, "UILeftBarButtonItem"));
    setRetained(x, d.rightItem, decode(x, coder, "UIRightBarButtonItem"));
    return self;
}
void ni_dealloc(Ctx& x, id self, SEL) {
    if (NavItemData* d = rt(x).hostIf<NavItemData>(self)) {
        for (id* o : {&d->title, &d->prompt, &d->backItem, &d->leftItem, &d->rightItem, &d->titleView}) {
            if (*o) rt(x).release(x.cpu, *o);
            *o = 0;
        }
        for (id o : d->leftItems) rt(x).release(x.cpu, o);
        for (id o : d->rightItems) rt(x).release(x.cpu, o);
    }
    rt(x).disposeInstance(x.cpu, self);
}
id ni_title(Ctx& x, id self, SEL) { return NID(x, self).title; }
void ni_setTitle(Ctx& x, id self, SEL, id t) { setRetained(x, NID(x, self).title, t ? autoreleased(x, id(rt(x).send(x.cpu, t, "copy"))) : 0); setNeedsComposite(x); }
id ni_prompt(Ctx& x, id self, SEL) { return NID(x, self).prompt; }
void ni_setPrompt(Ctx& x, id self, SEL, id p) { setRetained(x, NID(x, self).prompt, p ? autoreleased(x, id(rt(x).send(x.cpu, p, "copy"))) : 0); }
id ni_backItem(Ctx& x, id self, SEL) { return NID(x, self).backItem; }
void ni_setBackItem(Ctx& x, id self, SEL, id b) { setRetained(x, NID(x, self).backItem, b); }
id ni_leftItem(Ctx& x, id self, SEL) { return NID(x, self).leftItem; }
void ni_setLeftItem(Ctx& x, id self, SEL, id b) { setRetained(x, NID(x, self).leftItem, b); setNeedsComposite(x); }
void ni_setLeftItemAnim(Ctx& x, id self, SEL s, id b, bool) { ni_setLeftItem(x, self, s, b); }
id ni_rightItem(Ctx& x, id self, SEL) { return NID(x, self).rightItem; }
void ni_setRightItem(Ctx& x, id self, SEL, id b) { setRetained(x, NID(x, self).rightItem, b); setNeedsComposite(x); }
void ni_setRightItemAnim(Ctx& x, id self, SEL s, id b, bool) { ni_setRightItem(x, self, s, b); }
id ni_titleView(Ctx& x, id self, SEL) { return NID(x, self).titleView; }
void ni_setTitleView(Ctx& x, id self, SEL, id v) { setRetained(x, NID(x, self).titleView, v); setNeedsComposite(x); }
bool ni_hidesBack(Ctx& x, id self, SEL) { return NID(x, self).hidesBack; }
void ni_setHidesBack(Ctx& x, id self, SEL, bool b) { NID(x, self).hidesBack = b; setNeedsComposite(x); }
void ni_setHidesBackAnim(Ctx& x, id self, SEL s, bool b, bool) { ni_setHidesBack(x, self, s, b); }

// UINavigationBar & UINavigationController
struct NavBarData : ObjC::HostObject {
    int32_t barStyle = 0;
    bool translucent = false;
    id tintColor = 0, barTintColor = 0, delegate = 0, titleAttrs = 0;
    std::vector<id> items;   // retained UINavigationItems
    id navController = 0;    // unretained
};
NavBarData& NBD(Ctx& x, id b) { return rt(x).host<NavBarData>(b); }

id nb_initWithFrame(Ctx& x, id self, SEL s, CGRect f) {
    if (f.size.width <= 0) f.size.width = 320;
    if (f.size.height <= 0) f.size.height = 44;
    const id me = id(rt(x).sendSuper(x.cpu, self, ui(x).view, s, {fbits(f.origin.x), fbits(f.origin.y), fbits(f.size.width), fbits(f.size.height)}));
    if (me) {
        NBD(x, me);
        rt(x).send(x.cpu, me, "setBackgroundColor:", {makeUIColor(x, {0.95f, 0.95f, 0.97f, 1})});
    }
    return me;
}
void nb_dealloc(Ctx& x, id self, SEL s) {
    if (NavBarData* d = rt(x).hostIf<NavBarData>(self)) {
        for (id i : d->items) rt(x).release(x.cpu, i);
        d->items.clear();
        for (id* o : {&d->tintColor, &d->barTintColor, &d->titleAttrs}) { if (*o) rt(x).release(x.cpu, *o); *o = 0; }
    }
    rt(x).sendSuper(x.cpu, self, ui(x).view, s);
}
void nb_pushItem(Ctx& x, id self, SEL, id item, bool) {
    if (!item) return;
    NBD(x, self).items.push_back(rt(x).retain(x.cpu, item));
    rt(x).send(x.cpu, self, "setNeedsDisplay");
}
id nb_popItem(Ctx& x, id self, SEL, bool) {
    auto& v = NBD(x, self).items;
    if (v.empty()) return 0;
    const id top = v.back();
    v.pop_back();
    rt(x).send(x.cpu, self, "setNeedsDisplay");
    return autoreleased(x, top);
}
id nb_items(Ctx& x, id self, SEL) { return Foundation::makeArray(x, NBD(x, self).items); }
void nb_setItems(Ctx& x, id self, SEL, id arr, bool) {
    auto& v = NBD(x, self).items;
    for (id o : v) rt(x).release(x.cpu, o);
    v.clear();
    for (id o : Foundation::arrayItems(x, arr)) v.push_back(rt(x).retain(x.cpu, o));
    rt(x).send(x.cpu, self, "setNeedsDisplay");
}
void nb_setItemsSimple(Ctx& x, id self, SEL s, id arr) { nb_setItems(x, self, s, arr, false); }
id nb_topItem(Ctx& x, id self, SEL) { const auto& v = NBD(x, self).items; return v.empty() ? 0 : v.back(); }
id nb_backItem(Ctx& x, id self, SEL) { const auto& v = NBD(x, self).items; return v.size() < 2 ? 0 : v[v.size() - 2]; }
id nb_delegate(Ctx& x, id self, SEL) { return NBD(x, self).delegate; }
void nb_setDelegate(Ctx& x, id self, SEL, id d) { NBD(x, self).delegate = d; }
int32_t nb_barStyle(Ctx& x, id self, SEL) { return NBD(x, self).barStyle; }
void nb_setBarStyle(Ctx& x, id self, SEL, int32_t s) { NBD(x, self).barStyle = s; rt(x).send(x.cpu, self, "setNeedsDisplay"); }
bool nb_translucent(Ctx& x, id self, SEL) { return NBD(x, self).translucent; }
void nb_setTranslucent(Ctx& x, id self, SEL, bool b) { NBD(x, self).translucent = b; }
id nb_tintColor(Ctx& x, id self, SEL) { return NBD(x, self).tintColor; }
void nb_setTintColor(Ctx& x, id self, SEL, id c) { setRetained(x, NBD(x, self).tintColor, c); rt(x).send(x.cpu, self, "setNeedsDisplay"); }
id nb_barTintColor(Ctx& x, id self, SEL) { return NBD(x, self).barTintColor; }
void nb_setBarTintColor(Ctx& x, id self, SEL, id c) { setRetained(x, NBD(x, self).barTintColor, c); rt(x).send(x.cpu, self, "setBackgroundColor:", {c}); }

void nb_drawRect(Ctx& x, id self, SEL, CGRect) {
    const id ctx = CG::currentContext(x);
    if (!ctx) return;
    const NavBarData& d = NBD(x, self);
    const G::Rect b = boundsOf(x, self);
    CG::fillRect(x, ctx, {0, b.h - 1, b.w, 1}, {0.75f, 0.75f, 0.78f, 1});
    if (d.items.empty()) return;
    const NavItemData& top = NID(x, d.items.back());
    const std::string title = textOf(x, top.title);
    if (!title.empty()) {
        drawText(x, ctx, title, {"Helvetica-Bold", 17, true, false}, {60, 11, b.w - 120, 22},
                 d.barStyle == 1 ? G::Color{1, 1, 1, 1} : G::Color{0.1f, 0.1f, 0.12f, 1}, 4, 1 /* center */, 1);
    }
    if (top.leftItem) {
        const std::string lt = textOf(x, BID(x, top.leftItem).title);
        if (!lt.empty()) drawText(x, ctx, lt, {"Helvetica", 16, false, false}, {12, 12, 80, 20}, {0, 0.478f, 1, 1}, 4, 0, 1);
    } else if (d.items.size() >= 2 && !top.hidesBack) {
        std::string bt = "< Back";
        const NavItemData& prev = NID(x, d.items[d.items.size() - 2]);
        if (prev.title && !textOf(x, prev.title).empty()) bt = "< " + textOf(x, prev.title);
        drawText(x, ctx, bt, {"Helvetica", 16, false, false}, {10, 12, 90, 20}, {0, 0.478f, 1, 1}, 4, 0, 1);
    }
    if (top.rightItem) {
        const std::string rtTxt = textOf(x, BID(x, top.rightItem).title);
        if (!rtTxt.empty()) drawText(x, ctx, rtTxt, {"Helvetica-Bold", 16, true, false}, {b.w - 92, 12, 80, 20}, {0, 0.478f, 1, 1}, 4, 2 /* right */, 1);
    }
}

void nb_touchesEnded(Ctx& x, id self, SEL, id touches, id) {
    NavBarData& d = NBD(x, self);
    if (d.items.empty()) return;
    const G::Point p = touchInView(x, touches, self);
    const G::Rect b = boundsOf(x, self);
    const NavItemData& top = NID(x, d.items.back());
    if (p.x < 90) {
        if (top.leftItem) {
            const BarItemData& bi = BID(x, top.leftItem);
            if (bi.action) rt(x).send(x.cpu, ui(x).app, "sendAction:to:from:forEvent:", {bi.action, bi.target, top.leftItem, 0});
        } else if (d.items.size() >= 2 && !top.hidesBack && d.navController) {
            rt(x).send(x.cpu, d.navController, "popViewControllerAnimated:", {1});
        }
    } else if (p.x > b.w - 90 && top.rightItem) {
        const BarItemData& bi = BID(x, top.rightItem);
        if (bi.action) rt(x).send(x.cpu, ui(x).app, "sendAction:to:from:forEvent:", {bi.action, bi.target, top.rightItem, 0});
    }
}

// UINavigationController
struct NavControllerData : ObjC::HostObject {
    id navBar = 0, toolbar = 0, delegate = 0, currentContentView = 0;
    bool navBarHidden = false, toolbarHidden = true;
};
NavControllerData& NCD(Ctx& x, id n) { return rt(x).host<NavControllerData>(n); }

void syncNavControllerView(Ctx& x, id self) {
    NavControllerData& d = NCD(x, self);
    VCData& vd = rt(x).host<VCData>(self);
    if (!vd.view) return;
    const G::Rect b = boundsOf(x, vd.view);
    const float W = b.w > 0 ? b.w : ui(x).screenW;
    const float H = b.h > 0 ? b.h : (ui(x).screenH - (ui(x).statusBarHidden ? 0 : 20));
    const float topH = d.navBarHidden ? 0.0f : 44.0f;
    const float botH = d.toolbarHidden ? 0.0f : 44.0f;

    if (d.navBar) {
        rt(x).send(x.cpu, d.navBar, "setHidden:", {d.navBarHidden ? 1u : 0u});
        sendRect(x, d.navBar, rt(x).sel("setFrame:"), {0, 0, W, 44});
        std::vector<id> navItems;
        for (id vc : vd.children) {
            const id ni = id(rt(x).send(x.cpu, vc, "navigationItem"));
            if (ni) navItems.push_back(ni);
        }
        nb_setItems(x, d.navBar, 0, Foundation::makeArray(x, navItems), false);
    }
    if (d.toolbar) {
        rt(x).send(x.cpu, d.toolbar, "setHidden:", {d.toolbarHidden ? 1u : 0u});
        sendRect(x, d.toolbar, rt(x).sel("setFrame:"), {0, H - 44, W, 44});
    }

    const id topVC = vd.children.empty() ? 0 : vd.children.back();
    const id newView = topVC ? id(rt(x).send(x.cpu, topVC, "view")) : 0;
    if (d.currentContentView && d.currentContentView != newView) {
        rt(x).send(x.cpu, d.currentContentView, "removeFromSuperview");
        rt(x).release(x.cpu, d.currentContentView);
        d.currentContentView = 0;
    }
    if (newView) {
        sendRect(x, newView, rt(x).sel("setFrame:"), {0, topH, W, std::max(0.0f, H - topH - botH)});
        if (d.currentContentView != newView) {
            d.currentContentView = rt(x).retain(x.cpu, newView);
            rt(x).send(x.cpu, vd.view, "insertSubview:atIndex:", {newView, 0});
        }
        if (d.navBar) rt(x).send(x.cpu, vd.view, "bringSubviewToFront:", {d.navBar});
        if (d.toolbar) rt(x).send(x.cpu, vd.view, "bringSubviewToFront:", {d.toolbar});
    }
    setNeedsComposite(x);
}

void ensureNavBars(Ctx& x, id self) {
    NavControllerData& d = NCD(x, self);
    if (!d.navBar) {
        d.navBar = allocFrame(x, rt(x).getClass("UINavigationBar"), {0, 0, 320, 44});
        NBD(x, d.navBar).navController = self;
    }
    if (!d.toolbar) {
        d.toolbar = allocFrame(x, rt(x).getClass("UIToolbar"), {0, 416, 320, 44});
        rt(x).send(x.cpu, d.toolbar, "setHidden:", {1});
    }
}

id nc_initWithRoot(Ctx& x, id self, SEL, id rootVC) {
    const id me = id(rt(x).sendSuper(x.cpu, self, ui(x).viewController, rt(x).selInit));
    if (me) {
        ensureNavBars(x, me);
        if (rootVC) rt(x).send(x.cpu, me, "pushViewController:animated:", {rootVC, 0});
    }
    return me;
}
id nc_init(Ctx& x, id self, SEL s) { return nc_initWithRoot(x, self, s, 0); }
id nc_initWithCoder(Ctx& x, id self, SEL s, id coder) {
    const id me = id(rt(x).sendSuper(x.cpu, self, ui(x).viewController, s, {coder}));
    if (me) {
        ensureNavBars(x, me);
        if (const id vcs = decode(x, coder, "UIViewControllers"))
            for (id vc : Foundation::arrayItems(x, vcs)) rt(x).send(x.cpu, me, "pushViewController:animated:", {vc, 0});
    }
    return me;
}
void nc_dealloc(Ctx& x, id self, SEL s) {
    if (NavControllerData* d = rt(x).hostIf<NavControllerData>(self)) {
        for (id* o : {&d->navBar, &d->toolbar, &d->currentContentView}) { if (*o) rt(x).release(x.cpu, *o); *o = 0; }
    }
    rt(x).sendSuper(x.cpu, self, ui(x).viewController, s);
}
void nc_loadView(Ctx& x, id self, SEL) {
    ensureNavBars(x, self);
    NavControllerData& d = NCD(x, self);
    const float H = ui(x).screenH - (ui(x).statusBarHidden ? 0 : 20);
    const id v = allocFrame(x, ui(x).view, {0, ui(x).statusBarHidden ? 0.0f : 20.0f, ui(x).screenW, H});
    rt(x).send(x.cpu, v, "setBackgroundColor:", {makeUIColor(x, {1, 1, 1, 1})});
    rt(x).send(x.cpu, v, "addSubview:", {d.navBar});
    rt(x).send(x.cpu, v, "addSubview:", {d.toolbar});
    rt(x).send(x.cpu, self, "setView:", {v});
    rt(x).release(x.cpu, v);
    syncNavControllerView(x, self);
}
void nc_push(Ctx& x, id self, SEL, id vc, bool animated) {
    if (!vc) return;
    ensureNavBars(x, self);
    NavControllerData& d = NCD(x, self);
    auto& r = rt(x);
    if (d.delegate && r.respondsTo(d.delegate, r.sel("navigationController:willShowViewController:animated:")))
        r.send(x.cpu, d.delegate, "navigationController:willShowViewController:animated:", {self, vc, animated ? 1u : 0u});
    r.send(x.cpu, self, "addChildViewController:", {vc});
    r.send(x.cpu, vc, "didMoveToParentViewController:", {self});
    if (r.host<VCData>(self).view) {
        r.send(x.cpu, vc, "viewWillAppear:", {animated ? 1u : 0u});
        syncNavControllerView(x, self);
        r.send(x.cpu, vc, "viewDidAppear:", {animated ? 1u : 0u});
    }
    if (d.delegate && r.respondsTo(d.delegate, r.sel("navigationController:didShowViewController:animated:")))
        r.send(x.cpu, d.delegate, "navigationController:didShowViewController:animated:", {self, vc, animated ? 1u : 0u});
}
id nc_pop(Ctx& x, id self, SEL, bool animated) {
    VCData& vd = rt(x).host<VCData>(self);
    if (vd.children.size() <= 1) return 0;
    const id popped = rt(x).retain(x.cpu, vd.children.back());
    rt(x).send(x.cpu, popped, "viewWillDisappear:", {animated ? 1u : 0u});
    rt(x).send(x.cpu, popped, "removeFromParentViewController");
    syncNavControllerView(x, self);
    rt(x).send(x.cpu, popped, "viewDidDisappear:", {animated ? 1u : 0u});
    return autoreleased(x, popped);
}
id nc_popToRoot(Ctx& x, id self, SEL, bool animated) {
    VCData& vd = rt(x).host<VCData>(self);
    std::vector<id> popped;
    while (vd.children.size() > 1) popped.push_back(nc_pop(x, self, 0, animated));
    return Foundation::makeArray(x, popped);
}
id nc_popToVC(Ctx& x, id self, SEL, id target, bool animated) {
    VCData& vd = rt(x).host<VCData>(self);
    if (std::find(vd.children.begin(), vd.children.end(), target) == vd.children.end()) return 0;
    std::vector<id> popped;
    while (vd.children.size() > 1 && vd.children.back() != target) popped.push_back(nc_pop(x, self, 0, animated));
    return Foundation::makeArray(x, popped);
}
id nc_topVC(Ctx& x, id self, SEL) {
    const auto& c = rt(x).host<VCData>(self).children;
    return c.empty() ? 0 : c.back();
}
id nc_visibleVC(Ctx& x, id self, SEL s) {
    const id pres = rt(x).host<VCData>(self).presented;
    return pres ? pres : nc_topVC(x, self, s);
}
id nc_viewControllers(Ctx& x, id self, SEL) { return Foundation::makeArray(x, rt(x).host<VCData>(self).children); }
void nc_setViewControllersAnim(Ctx& x, id self, SEL, id arr, bool) {
    VCData& vd = rt(x).host<VCData>(self);
    while (!vd.children.empty()) rt(x).send(x.cpu, vd.children.back(), "removeFromParentViewController");
    for (id vc : Foundation::arrayItems(x, arr)) rt(x).send(x.cpu, self, "addChildViewController:", {vc});
    syncNavControllerView(x, self);
}
void nc_setViewControllers(Ctx& x, id self, SEL s, id arr) { nc_setViewControllersAnim(x, self, s, arr, false); }
id nc_navBar(Ctx& x, id self, SEL) { ensureNavBars(x, self); return NCD(x, self).navBar; }
id nc_toolbar(Ctx& x, id self, SEL) { ensureNavBars(x, self); return NCD(x, self).toolbar; }
bool nc_navBarHidden(Ctx& x, id self, SEL) { return NCD(x, self).navBarHidden; }
void nc_setNavBarHidden(Ctx& x, id self, SEL, bool b) { NCD(x, self).navBarHidden = b; syncNavControllerView(x, self); }
void nc_setNavBarHiddenAnim(Ctx& x, id self, SEL s, bool b, bool) { nc_setNavBarHidden(x, self, s, b); }
bool nc_toolbarHidden(Ctx& x, id self, SEL) { return NCD(x, self).toolbarHidden; }
void nc_setToolbarHidden(Ctx& x, id self, SEL, bool b) { NCD(x, self).toolbarHidden = b; syncNavControllerView(x, self); }
void nc_setToolbarHiddenAnim(Ctx& x, id self, SEL s, bool b, bool) { nc_setToolbarHidden(x, self, s, b); }
id nc_delegate(Ctx& x, id self, SEL) { return NCD(x, self).delegate; }
void nc_setDelegate(Ctx& x, id self, SEL, id d) { NCD(x, self).delegate = d; }

// UITabBar & UITabBarController
struct TabBarData : ObjC::HostObject {
    std::vector<id> items;
    id selectedItem = 0, delegate = 0, tintColor = 0, barTintColor = 0;
    id tabController = 0;
};
TabBarData& TBD(Ctx& x, id b) { return rt(x).host<TabBarData>(b); }

id tb_initWithFrame(Ctx& x, id self, SEL s, CGRect f) {
    if (f.size.width <= 0) f.size.width = 320;
    if (f.size.height <= 0) f.size.height = 49;
    const id me = id(rt(x).sendSuper(x.cpu, self, ui(x).view, s, {fbits(f.origin.x), fbits(f.origin.y), fbits(f.size.width), fbits(f.size.height)}));
    if (me) {
        TBD(x, me);
        rt(x).send(x.cpu, me, "setBackgroundColor:", {makeUIColor(x, {0.12f, 0.12f, 0.14f, 1})});
    }
    return me;
}
id tb_items(Ctx& x, id self, SEL) { return Foundation::makeArray(x, TBD(x, self).items); }
void tb_setItems(Ctx& x, id self, SEL, id arr, bool) {
    auto& v = TBD(x, self).items;
    for (id o : v) rt(x).release(x.cpu, o);
    v.clear();
    for (id o : Foundation::arrayItems(x, arr)) v.push_back(rt(x).retain(x.cpu, o));
    rt(x).send(x.cpu, self, "setNeedsDisplay");
}
void tb_setItemsSimple(Ctx& x, id self, SEL s, id arr) { tb_setItems(x, self, s, arr, false); }
id tb_selectedItem(Ctx& x, id self, SEL) { return TBD(x, self).selectedItem; }
void tb_setSelectedItem(Ctx& x, id self, SEL, id item) { TBD(x, self).selectedItem = item; rt(x).send(x.cpu, self, "setNeedsDisplay"); }
id tb_delegate(Ctx& x, id self, SEL) { return TBD(x, self).delegate; }
void tb_setDelegate(Ctx& x, id self, SEL, id d) { TBD(x, self).delegate = d; }

void tb_drawRect(Ctx& x, id self, SEL, CGRect) {
    const id ctx = CG::currentContext(x);
    if (!ctx) return;
    const TabBarData& d = TBD(x, self);
    const G::Rect b = boundsOf(x, self);
    CG::fillRect(x, ctx, {0, 0, b.w, 1}, {0.3f, 0.3f, 0.35f, 1});
    if (d.items.empty()) return;
    const float segW = b.w / float(d.items.size());
    for (size_t i = 0; i < d.items.size(); ++i) {
        const id item = d.items[i];
        const bool sel = (item == d.selectedItem);
        const std::string t = textOf(x, BID(x, item).title);
        if (!t.empty()) {
            drawText(x, ctx, t, {"Helvetica-Bold", 11, true, false}, {float(i) * segW, b.h - 18, segW, 14},
                     sel ? G::Color{0.2f, 0.65f, 1, 1} : G::Color{0.65f, 0.65f, 0.7f, 1}, 4, 1, 1);
        }
    }
}

struct TabControllerData : ObjC::HostObject {
    id tabBar = 0, delegate = 0, currentContentView = 0;
    uint32_t selectedIndex = 0;
};
TabControllerData& TCD(Ctx& x, id t) { return rt(x).host<TabControllerData>(t); }

void ensureTabBar(Ctx& x, id self) {
    TabControllerData& d = TCD(x, self);
    if (!d.tabBar) {
        d.tabBar = allocFrame(x, rt(x).getClass("UITabBar"), {0, 411, 320, 49});
        TBD(x, d.tabBar).tabController = self;
    }
}

void syncTabControllerView(Ctx& x, id self) {
    TabControllerData& d = TCD(x, self);
    VCData& vd = rt(x).host<VCData>(self);
    // Tab items are model state: keep them current even before the view is loaded.
    if (d.tabBar) {
        std::vector<id> items;
        for (id vc : vd.children) {
            const id ti = id(rt(x).send(x.cpu, vc, "tabBarItem"));
            if (ti) items.push_back(ti);
        }
        tb_setItems(x, d.tabBar, 0, Foundation::makeArray(x, items), false);
        if (d.selectedIndex < items.size()) tb_setSelectedItem(x, d.tabBar, 0, items[d.selectedIndex]);
    }
    if (!vd.view) return;
    const G::Rect b = boundsOf(x, vd.view);
    const float W = b.w > 0 ? b.w : ui(x).screenW;
    const float H = b.h > 0 ? b.h : (ui(x).screenH - (ui(x).statusBarHidden ? 0 : 20));
    if (d.tabBar) sendRect(x, d.tabBar, rt(x).sel("setFrame:"), {0, H - 49, W, 49});
    const id selVC = (d.selectedIndex < vd.children.size()) ? vd.children[d.selectedIndex] : 0;
    const id newView = selVC ? id(rt(x).send(x.cpu, selVC, "view")) : 0;
    if (d.currentContentView && d.currentContentView != newView) {
        rt(x).send(x.cpu, d.currentContentView, "removeFromSuperview");
        rt(x).release(x.cpu, d.currentContentView);
        d.currentContentView = 0;
    }
    if (newView) {
        sendRect(x, newView, rt(x).sel("setFrame:"), {0, 0, W, std::max(0.0f, H - 49)});
        if (d.currentContentView != newView) {
            d.currentContentView = rt(x).retain(x.cpu, newView);
            rt(x).send(x.cpu, vd.view, "insertSubview:atIndex:", {newView, 0});
        }
        if (d.tabBar) rt(x).send(x.cpu, vd.view, "bringSubviewToFront:", {d.tabBar});
    }
    setNeedsComposite(x);
}

void tb_touchesEnded(Ctx& x, id self, SEL, id touches, id) {
    TabBarData& d = TBD(x, self);
    if (d.items.empty()) return;
    const G::Point p = touchInView(x, touches, self);
    const G::Rect b = boundsOf(x, self);
    const size_t idx = std::min(d.items.size() - 1, size_t(std::max(0.0f, p.x) / (b.w / float(d.items.size()))));
    d.selectedItem = d.items[idx];
    rt(x).send(x.cpu, self, "setNeedsDisplay");
    if (d.tabController) rt(x).send(x.cpu, d.tabController, "setSelectedIndex:", {uint32_t(idx)});
    if (d.delegate && rt(x).respondsTo(d.delegate, rt(x).sel("tabBar:didSelectItem:")))
        rt(x).send(x.cpu, d.delegate, "tabBar:didSelectItem:", {self, d.selectedItem});
}

id tbc_init(Ctx& x, id self, SEL s) {
    const id me = id(rt(x).sendSuper(x.cpu, self, ui(x).viewController, s));
    if (me) ensureTabBar(x, me);
    return me;
}
void tbc_loadView(Ctx& x, id self, SEL) {
    ensureTabBar(x, self);
    const float H = ui(x).screenH - (ui(x).statusBarHidden ? 0 : 20);
    const id v = allocFrame(x, ui(x).view, {0, ui(x).statusBarHidden ? 0.0f : 20.0f, ui(x).screenW, H});
    rt(x).send(x.cpu, v, "setBackgroundColor:", {makeUIColor(x, {1, 1, 1, 1})});
    rt(x).send(x.cpu, v, "addSubview:", {TCD(x, self).tabBar});
    rt(x).send(x.cpu, self, "setView:", {v});
    rt(x).release(x.cpu, v);
    syncTabControllerView(x, self);
}
id tbc_viewControllers(Ctx& x, id self, SEL) { return Foundation::makeArray(x, rt(x).host<VCData>(self).children); }
void tbc_setViewControllersAnim(Ctx& x, id self, SEL, id arr, bool) {
    ensureTabBar(x, self);
    VCData& vd = rt(x).host<VCData>(self);
    while (!vd.children.empty()) rt(x).send(x.cpu, vd.children.back(), "removeFromParentViewController");
    for (id vc : Foundation::arrayItems(x, arr)) rt(x).send(x.cpu, self, "addChildViewController:", {vc});
    TCD(x, self).selectedIndex = 0;
    syncTabControllerView(x, self);
}
void tbc_setViewControllers(Ctx& x, id self, SEL s, id arr) { tbc_setViewControllersAnim(x, self, s, arr, false); }
uint32_t tbc_selectedIndex(Ctx& x, id self, SEL) { return TCD(x, self).selectedIndex; }
void tbc_setSelectedIndex(Ctx& x, id self, SEL, uint32_t idx) {
    VCData& vd = rt(x).host<VCData>(self);
    if (idx >= vd.children.size()) return;
    TCD(x, self).selectedIndex = idx;
    syncTabControllerView(x, self);
    const id del = TCD(x, self).delegate;
    if (del && rt(x).respondsTo(del, rt(x).sel("tabBarController:didSelectViewController:")))
        rt(x).send(x.cpu, del, "tabBarController:didSelectViewController:", {self, vd.children[idx]});
}
id tbc_selectedVC(Ctx& x, id self, SEL) {
    const auto& c = rt(x).host<VCData>(self).children;
    const uint32_t idx = TCD(x, self).selectedIndex;
    return idx < c.size() ? c[idx] : 0;
}
void tbc_setSelectedVC(Ctx& x, id self, SEL, id vc) {
    const auto& c = rt(x).host<VCData>(self).children;
    auto it = std::find(c.begin(), c.end(), vc);
    if (it != c.end()) tbc_setSelectedIndex(x, self, 0, uint32_t(it - c.begin()));
}
id tbc_tabBar(Ctx& x, id self, SEL) { ensureTabBar(x, self); return TCD(x, self).tabBar; }
id tbc_delegate(Ctx& x, id self, SEL) { return TCD(x, self).delegate; }
void tbc_setDelegate(Ctx& x, id self, SEL, id d) { TCD(x, self).delegate = d; }

// UIViewController tabBarItem getter/setter
id vc_tabBarItem(Ctx& x, id self, SEL) {
    VCData& d = rt(x).host<VCData>(self);
    if (!d.tabBarItem) {
        const Class cls = rt(x).getClass("UITabBarItem");
        if (cls) d.tabBarItem = id(rt(x).send(x.cpu, id(rt(x).send(x.cpu, cls, rt(x).selAlloc)), "initWithTitle:image:tag:", {d.title, 0, 0}));
    }
    return d.tabBarItem;
}
void vc_setTabBarItem(Ctx& x, id self, SEL, id item) { setRetained(x, rt(x).host<VCData>(self).tabBarItem, item); }

// ======================================= UITextField & UITextView ===

struct TextFieldData : ObjC::HostObject {
    id text = 0, placeholder = 0, font = 0, textColor = 0, delegate = 0;
    id background = 0, disabledBackground = 0, leftView = 0, rightView = 0, inputView = 0, inputAccessoryView = 0;
    int32_t alignment = 0, borderStyle = 0, clearButtonMode = 0, leftViewMode = 0, rightViewMode = 0;
    int32_t keyboardType = 0, keyboardAppearance = 0, returnKeyType = 0, autocap = 0, autocorrect = 0;
    bool clearsOnBegin = false, adjustsFont = false, secure = false, editing = false, editable = true, selectable = true;
    float minFontSize = 0;
};
TextFieldData& TFD(Ctx& x, id t) { return rt(x).host<TextFieldData>(t); }

id tf_initWithFrame(Ctx& x, id self, SEL s, CGRect f) {
    const id me = id(rt(x).sendSuper(x.cpu, self, ui(x).control, s, {fbits(f.origin.x), fbits(f.origin.y), fbits(f.size.width), fbits(f.size.height)}));
    if (me) {
        TextFieldData& d = TFD(x, me);
        d.text = rt(x).retain(x.cpu, makeString(x, ""));
        d.font = rt(x).retain(x.cpu, systemFont(x, 14, false));
        d.textColor = rt(x).retain(x.cpu, makeUIColor(x, {0, 0, 0, 1}));
        rt(x).send(x.cpu, me, "setBackgroundColor:", {makeUIColor(x, {0, 0, 0, 0})});
        rt(x).send(x.cpu, me, "setOpaque:", {0});
    }
    return me;
}
id tf_initWithCoder(Ctx& x, id self, SEL s, id coder) {
    const id me = id(rt(x).sendSuper(x.cpu, self, ui(x).control, s, {coder}));
    if (me) {
        TextFieldData& d = TFD(x, me);
        setRetained(x, d.text, decode(x, coder, "UIText") ? decode(x, coder, "UIText") : makeString(x, ""));
        setRetained(x, d.placeholder, decode(x, coder, "UIPlaceholder"));
        setRetained(x, d.font, decode(x, coder, "UIFont") ? decode(x, coder, "UIFont") : systemFont(x, 14, false));
        setRetained(x, d.textColor, decode(x, coder, "UITextColor") ? decode(x, coder, "UITextColor") : makeUIColor(x, {0, 0, 0, 1}));
        if (has(x, coder, "UIBorderStyle")) d.borderStyle = decodeInt(x, coder, "UIBorderStyle");
        if (has(x, coder, "UITextAlignment")) d.alignment = decodeInt(x, coder, "UITextAlignment");
    }
    return me;
}
void tf_dealloc(Ctx& x, id self, SEL s) {
    if (TextFieldData* d = rt(x).hostIf<TextFieldData>(self)) {
        for (id* o : {&d->text, &d->placeholder, &d->font, &d->textColor, &d->background, &d->disabledBackground,
                      &d->leftView, &d->rightView, &d->inputView, &d->inputAccessoryView}) {
            if (*o) rt(x).release(x.cpu, *o);
            *o = 0;
        }
    }
    rt(x).sendSuper(x.cpu, self, ui(x).control, s);
}
id tf_text(Ctx& x, id self, SEL) { return TFD(x, self).text ? TFD(x, self).text : makeString(x, ""); }
void tf_setText(Ctx& x, id self, SEL, id t) {
    setRetained(x, TFD(x, self).text, t ? autoreleased(x, id(rt(x).send(x.cpu, t, "copy"))) : makeString(x, ""));
    rt(x).send(x.cpu, self, "setNeedsDisplay");
}
id tf_placeholder(Ctx& x, id self, SEL) { return TFD(x, self).placeholder; }
void tf_setPlaceholder(Ctx& x, id self, SEL, id p) {
    setRetained(x, TFD(x, self).placeholder, p ? autoreleased(x, id(rt(x).send(x.cpu, p, "copy"))) : 0);
    rt(x).send(x.cpu, self, "setNeedsDisplay");
}
id tf_font(Ctx& x, id self, SEL) { return TFD(x, self).font; }
void tf_setFont(Ctx& x, id self, SEL, id f) { if (f) setRetained(x, TFD(x, self).font, f); rt(x).send(x.cpu, self, "setNeedsDisplay"); }
id tf_textColor(Ctx& x, id self, SEL) { return TFD(x, self).textColor; }
void tf_setTextColor(Ctx& x, id self, SEL, id c) { if (c) setRetained(x, TFD(x, self).textColor, c); rt(x).send(x.cpu, self, "setNeedsDisplay"); }
int32_t tf_alignment(Ctx& x, id self, SEL) { return TFD(x, self).alignment; }
void tf_setAlignment(Ctx& x, id self, SEL, int32_t a) { TFD(x, self).alignment = a; rt(x).send(x.cpu, self, "setNeedsDisplay"); }
int32_t tf_borderStyle(Ctx& x, id self, SEL) { return TFD(x, self).borderStyle; }
void tf_setBorderStyle(Ctx& x, id self, SEL, int32_t b) { TFD(x, self).borderStyle = b; rt(x).send(x.cpu, self, "setNeedsDisplay"); }
id tf_delegate(Ctx& x, id self, SEL) { return TFD(x, self).delegate; }
void tf_setDelegate(Ctx& x, id self, SEL, id d) { TFD(x, self).delegate = d; }
bool tf_editing(Ctx& x, id self, SEL) { return TFD(x, self).editing; }
bool tf_secure(Ctx& x, id self, SEL) { return TFD(x, self).secure; }
void tf_setSecure(Ctx& x, id self, SEL, bool b) { TFD(x, self).secure = b; rt(x).send(x.cpu, self, "setNeedsDisplay"); }
bool tf_isTextView(Ctx& x,id self) { return rt(x).isKindOf(self,rt(x).getClass("UITextView")); }
bool tf_hasText(Ctx& x,id self,SEL) { return !Foundation::stringValue(x,TFD(x,self).text).empty(); }
uint32_t previousTextBoundary(const std::u16string& text) {
    if(text.empty())return 0;
#if defined(__APPLE__)
    CFStringRef string=CFStringCreateWithCharacters(kCFAllocatorDefault,
        reinterpret_cast<const UniChar*>(text.data()),CFIndex(text.size()));
    if(!string)throw std::runtime_error("Text input: cannot segment text");
    const CFRange range=CFStringGetRangeOfComposedCharactersAtIndex(string,CFIndex(text.size()-1));
    CFRelease(string);return uint32_t(range.location);
#else
    UErrorCode error=U_ZERO_ERROR;
    UBreakIterator* iterator=ubrk_open(UBRK_CHARACTER,"",reinterpret_cast<const UChar*>(text.data()),int32_t(text.size()),&error);
    if(U_FAILURE(error) || !iterator)throw std::runtime_error("Text input: cannot segment text");
    const int32_t at=ubrk_preceding(iterator,int32_t(text.size()));ubrk_close(iterator);
    if(at<0)throw std::runtime_error("Text input: invalid character boundary");
    return uint32_t(at);
#endif
}
void tf_replaceText(Ctx& x,id self,uint32_t location,uint32_t length,id replacement) {
    auto& r=rt(x);auto& d=TFD(x,self);
    if(tf_isTextView(x,self) && !d.editable)return;
    const std::u16string before=Foundation::stringValue(x,d.text),insert=Foundation::stringValue(x,replacement);
    if(location>before.size() || length>before.size()-location)
        throw std::out_of_range("Text input: replacement range exceeds text");
    if(!length && insert.empty())return;
    // A delegate/action can detach or release the field. Keep it alive until
    // this event's autorelease pool drains, and respect delegate-owned edits.
    autoreleased(x,r.retain(x.cpu,self));
    const bool multiline=tf_isTextView(x,self);
    const char* selector=multiline?"textView:shouldChangeTextInRange:replacementText:":
                                  "textField:shouldChangeCharactersInRange:replacementString:";
    const id delegate=d.delegate;
    if(delegate)autoreleased(x,r.retain(x.cpu,delegate));
    if(delegate && r.respondsTo(delegate,r.sel(selector)) &&
       !sendBool(x,delegate,selector,{self,location,length,replacement}))return;
    if(Foundation::stringValue(x,d.text)!=before)return;
    std::u16string after=before;after.replace(location,length,insert);
    tf_setText(x,self,0,makeString(x,after));
    if(multiline) {
        if(d.delegate && r.respondsTo(d.delegate,r.sel("textViewDidChange:")))
            r.send(x.cpu,d.delegate,"textViewDidChange:",{self});
    } else r.send(x.cpu,self,"sendActionsForControlEvents:",{1u<<17});
    Foundation::postNotification(x,multiline?"UITextViewTextDidChangeNotification":"UITextFieldTextDidChangeNotification",self);
}
void tf_insertText(Ctx& x,id self,SEL,id text) {
    tf_replaceText(x,self,uint32_t(Foundation::stringValue(x,TFD(x,self).text).size()),0,text);
}
void tf_deleteBackward(Ctx& x,id self,SEL) {
    const auto text=Foundation::stringValue(x,TFD(x,self).text);
    if(text.empty())return;
    const uint32_t start=previousTextBoundary(text);
    tf_replaceText(x,self,start,uint32_t(text.size())-start,makeString(x,""));
}
int32_t tf_keyboardType(Ctx& x,id self,SEL) { return TFD(x,self).keyboardType; }
void tf_setKeyboardType(Ctx& x,id self,SEL,int32_t n) { TFD(x,self).keyboardType=n; }
int32_t tf_returnKeyType(Ctx& x,id self,SEL) { return TFD(x,self).returnKeyType; }
void tf_setReturnKeyType(Ctx& x,id self,SEL,int32_t n) { TFD(x,self).returnKeyType=n; }
int32_t tf_keyboardAppearance(Ctx& x,id self,SEL) { return TFD(x,self).keyboardAppearance; }
void tf_setKeyboardAppearance(Ctx& x,id self,SEL,int32_t n) { TFD(x,self).keyboardAppearance=n; }
int32_t tf_autocorrection(Ctx& x,id self,SEL) { return TFD(x,self).autocorrect; }
void tf_setAutocorrection(Ctx& x,id self,SEL,int32_t n) { TFD(x,self).autocorrect=n; }
int32_t tf_autocapitalization(Ctx& x,id self,SEL) { return TFD(x,self).autocap; }
void tf_setAutocapitalization(Ctx& x,id self,SEL,int32_t n) { TFD(x,self).autocap=n; }
bool tf_editable(Ctx& x,id self,SEL) { return TFD(x,self).editable; }
void tf_setEditable(Ctx& x,id self,SEL,bool n) { TFD(x,self).editable=n; }
bool tf_selectable(Ctx& x,id self,SEL) { return TFD(x,self).selectable; }
void tf_setSelectable(Ctx& x,id self,SEL,bool n) { TFD(x,self).selectable=n; }
bool tf_clearsOnBegin(Ctx& x,id self,SEL) { return TFD(x,self).clearsOnBegin; }
void tf_setClearsOnBegin(Ctx& x,id self,SEL,bool n) { TFD(x,self).clearsOnBegin=n; }
// Accessory views and unimplemented input-trait details do not implement
// insertion/deletion; those follow the real delegate/event path above.
void tf_ignoreObj(Ctx&, id, SEL, id) {}
void tf_ignoreInt(Ctx&, id, SEL, int32_t) {}
void tf_ignoreBool(Ctx&, id, SEL, bool) {}
id tf_nilObj(Ctx&, id, SEL) { return 0; }
int32_t tf_zeroInt(Ctx&, id, SEL) { return 0; }
bool tf_trueBool(Ctx&, id, SEL) { return true; }
bool tf_canBecome(Ctx& x, id self, SEL) {
    if(tf_isTextView(x,self) && !TFD(x,self).editable)return false;
    const id del = TFD(x, self).delegate;
    if (del && rt(x).respondsTo(del, rt(x).sel("textFieldShouldBeginEditing:")))
        return sendBool(x, del, "textFieldShouldBeginEditing:", {self});
    return true;
}
bool tf_become(Ctx& x, id self, SEL s) {
    if(ui(x).firstResponder==self && TFD(x,self).editing)return true;
    const bool ok = (uint32_t(rt(x).sendSuper(x.cpu, self, ui(x).responder, s)) & 0xFF) != 0;
    if (ok) {
        TextFieldData& d = TFD(x, self);
        d.editing = true;
        if (d.clearsOnBegin) tf_setText(x, self, 0, makeString(x, ""));
        if (d.delegate && rt(x).respondsTo(d.delegate, rt(x).sel("textFieldDidBeginEditing:")))
            rt(x).send(x.cpu, d.delegate, "textFieldDidBeginEditing:", {self});
        Foundation::postNotification(x, "UITextFieldTextDidBeginEditingNotification", self);
        Foundation::postNotification(x, "UIKeyboardWillShowNotification", 0);
        Foundation::postNotification(x, "UIKeyboardDidShowNotification", 0);
    }
    return ok;
}
bool tf_resign(Ctx& x, id self, SEL s) {
    TextFieldData& d = TFD(x, self);
    if (d.delegate && rt(x).respondsTo(d.delegate, rt(x).sel("textFieldShouldEndEditing:")))
        if (!sendBool(x, d.delegate, "textFieldShouldEndEditing:", {self})) return false;
    const bool ok = (uint32_t(rt(x).sendSuper(x.cpu, self, ui(x).responder, s)) & 0xFF) != 0;
    if (ok && d.editing) {
        d.editing = false;
        if (d.delegate && rt(x).respondsTo(d.delegate, rt(x).sel("textFieldDidEndEditing:")))
            rt(x).send(x.cpu, d.delegate, "textFieldDidEndEditing:", {self});
        Foundation::postNotification(x, "UITextFieldTextDidEndEditingNotification", self);
        Foundation::postNotification(x, "UIKeyboardWillHideNotification", 0);
        Foundation::postNotification(x, "UIKeyboardDidHideNotification", 0);
    }
    return ok;
}
void tf_drawRect(Ctx& x, id self, SEL, CGRect) {
    const id ctx = CG::currentContext(x);
    if (!ctx) return;
    const TextFieldData& d = TFD(x, self);
    const G::Rect b = boundsOf(x, self);
    if (d.borderStyle == 3 /* RoundedRect */ || d.borderStyle == 1 /* Line */ || d.borderStyle == 2 /* Bezel */) {
        CG::fillRect(x, ctx, b, {1, 1, 1, 1});
        CG::fillRect(x, ctx, {0, 0, b.w, 1}, {0.7f, 0.7f, 0.75f, 1});
        CG::fillRect(x, ctx, {0, b.h - 1, b.w, 1}, {0.7f, 0.7f, 0.75f, 1});
        CG::fillRect(x, ctx, {0, 0, 1, b.h}, {0.7f, 0.7f, 0.75f, 1});
        CG::fillRect(x, ctx, {b.w - 1, 0, 1, b.h}, {0.7f, 0.7f, 0.75f, 1});
    }
    std::string txt = textOf(x, d.text);
    const G::FontDesc fd = d.font ? uiFont(x, d.font) : G::FontDesc{"Helvetica", 14, false, false};
    if (!txt.empty()) {
        if (d.secure) txt.assign(txt.size(), '*');
        drawText(x, ctx, txt, fd, {6, (b.h - fd.size - 4) / 2, b.w - 12, fd.size + 4},
                 d.textColor ? uiColor(x, d.textColor) : G::Color{0, 0, 0, 1}, 4, d.alignment, 1);
    } else if (d.placeholder) {
        const std::string ph = textOf(x, d.placeholder);
        if (!ph.empty())
            drawText(x, ctx, ph, fd, {6, (b.h - fd.size - 4) / 2, b.w - 12, fd.size + 4}, {0.7f, 0.7f, 0.73f, 1}, 4, d.alignment, 1);
    }
}

// ======================================= UISwitch, UISlider, UISegmentedControl, UIProgressView, UIPageControl ===

struct SwitchData : ObjC::HostObject { bool on = false; id onTintColor = 0, tintColor = 0, thumbTintColor = 0; };
SwitchData& SWD(Ctx& x, id s) { return rt(x).host<SwitchData>(s); }
id sw_initWithFrame(Ctx& x, id self, SEL s, CGRect f) {
    f.size.width = 79; f.size.height = 27;
    const id me = id(rt(x).sendSuper(x.cpu, self, ui(x).control, s, {fbits(f.origin.x), fbits(f.origin.y), fbits(f.size.width), fbits(f.size.height)}));
    if (me) { SWD(x, me); rt(x).send(x.cpu, me, "setBackgroundColor:", {makeUIColor(x, {0, 0, 0, 0})}); rt(x).send(x.cpu, me, "setOpaque:", {0}); }
    return me;
}
bool sw_isOn(Ctx& x, id self, SEL) { return SWD(x, self).on; }
void sw_setOn(Ctx& x, id self, SEL, bool b) { SWD(x, self).on = b; rt(x).send(x.cpu, self, "setNeedsDisplay"); }
void sw_setOnAnim(Ctx& x, id self, SEL s, bool b, bool) { sw_setOn(x, self, s, b); }
void sw_touchesEnded(Ctx& x, id self, SEL, id, id) {
    SWD(x, self).on = !SWD(x, self).on;
    rt(x).send(x.cpu, self, "setNeedsDisplay");
    rt(x).send(x.cpu, self, "sendActionsForControlEvents:", {1u << 12 /* UIControlEventValueChanged */});
}
void sw_drawRect(Ctx& x, id self, SEL, CGRect) {
    const id ctx = CG::currentContext(x);
    if (!ctx) return;
    const bool on = SWD(x, self).on;
    const G::Rect b = boundsOf(x, self);
    CG::fillRect(x, ctx, b, on ? G::Color{0.3f, 0.85f, 0.39f, 1} : G::Color{0.85f, 0.85f, 0.88f, 1});
    const float r = b.h - 4;
    CG::fillRect(x, ctx, {on ? (b.w - r - 2) : 2.0f, 2.0f, r, r}, {1, 1, 1, 1});
}

struct SliderData : ObjC::HostObject {
    float val = 0, minVal = 0, maxVal = 1;
    bool continuous = true;
    id minTrackImg = 0, maxTrackImg = 0, thumbImg = 0;
    id minValImg = 0, maxValImg = 0;
    id minTrackTint = 0, maxTrackTint = 0, thumbTint = 0;
};
SliderData& SLD(Ctx& x, id s) { return rt(x).host<SliderData>(s); }
id sl_initWithFrame(Ctx& x, id self, SEL s, CGRect f) {
    if (f.size.height <= 0) f.size.height = 23;
    const id me = id(rt(x).sendSuper(x.cpu, self, ui(x).control, s, {fbits(f.origin.x), fbits(f.origin.y), fbits(f.size.width), fbits(f.size.height)}));
    if (me) { SLD(x, me); rt(x).send(x.cpu, me, "setBackgroundColor:", {makeUIColor(x, {0, 0, 0, 0})}); rt(x).send(x.cpu, me, "setOpaque:", {0}); }
    return me;
}
float sl_value(Ctx& x, id self, SEL) { return SLD(x, self).val; }
void sl_setValue(Ctx& x, id self, SEL, float v) {
    SliderData& d = SLD(x, self);
    d.val = std::clamp(v, d.minVal, d.maxVal);
    rt(x).send(x.cpu, self, "setNeedsDisplay");
}
void sl_setValueAnim(Ctx& x, id self, SEL s, float v, bool) { sl_setValue(x, self, s, v); }
float sl_min(Ctx& x, id self, SEL) { return SLD(x, self).minVal; }
void sl_setMin(Ctx& x, id self, SEL, float v) { SLD(x, self).minVal = v; }
float sl_max(Ctx& x, id self, SEL) { return SLD(x, self).maxVal; }
void sl_setMax(Ctx& x, id self, SEL, float v) { SLD(x, self).maxVal = v; }
bool sl_cont(Ctx& x, id self, SEL) { return SLD(x, self).continuous; }
void sl_setCont(Ctx& x, id self, SEL, bool b) { SLD(x, self).continuous = b; }
void sl_setMinTrackImg(Ctx& x, id self, SEL, id img, uint32_t) { setRetained(x, SLD(x, self).minTrackImg, img); }
void sl_setMaxTrackImg(Ctx& x, id self, SEL, id img, uint32_t) { setRetained(x, SLD(x, self).maxTrackImg, img); }
void sl_setThumbImg(Ctx& x, id self, SEL, id img, uint32_t) { setRetained(x, SLD(x, self).thumbImg, img); }
id sl_minTrackImg(Ctx& x, id self, SEL, uint32_t) { return SLD(x, self).minTrackImg; }
id sl_maxTrackImg(Ctx& x, id self, SEL, uint32_t) { return SLD(x, self).maxTrackImg; }
id sl_thumbImg(Ctx& x, id self, SEL, uint32_t) { return SLD(x, self).thumbImg; }
id sl_curMinTrackImg(Ctx& x, id self, SEL) { return SLD(x, self).minTrackImg; }
id sl_curMaxTrackImg(Ctx& x, id self, SEL) { return SLD(x, self).maxTrackImg; }
id sl_curThumbImg(Ctx& x, id self, SEL) { return SLD(x, self).thumbImg; }
id sl_minValImg(Ctx& x, id self, SEL) { return SLD(x, self).minValImg; }
void sl_setMinValImg(Ctx& x, id self, SEL, id img) { setRetained(x, SLD(x, self).minValImg, img); }
id sl_maxValImg(Ctx& x, id self, SEL) { return SLD(x, self).maxValImg; }
void sl_setMaxValImg(Ctx& x, id self, SEL, id img) { setRetained(x, SLD(x, self).maxValImg, img); }
id sl_minTrackTint(Ctx& x, id self, SEL) { return SLD(x, self).minTrackTint; }
void sl_setMinTrackTint(Ctx& x, id self, SEL, id c) { setRetained(x, SLD(x, self).minTrackTint, c); }
id sl_maxTrackTint(Ctx& x, id self, SEL) { return SLD(x, self).maxTrackTint; }
void sl_setMaxTrackTint(Ctx& x, id self, SEL, id c) { setRetained(x, SLD(x, self).maxTrackTint, c); }
id sl_thumbTint(Ctx& x, id self, SEL) { return SLD(x, self).thumbTint; }
void sl_setThumbTint(Ctx& x, id self, SEL, id c) { setRetained(x, SLD(x, self).thumbTint, c); }
void sl_drawRect(Ctx& x, id self, SEL, CGRect) {
    const id ctx = CG::currentContext(x);
    if (!ctx) return;
    const SliderData& d = SLD(x, self);
    const G::Rect b = boundsOf(x, self);
    const float t = (d.maxVal > d.minVal) ? std::clamp((d.val - d.minVal) / (d.maxVal - d.minVal), 0.0f, 1.0f) : 0.0f;
    const float tx = 10 + t * std::max(0.0f, b.w - 20);
    CG::fillRect(x, ctx, {10, b.h / 2 - 2, tx - 10, 4}, {0, 0.478f, 1, 1});
    CG::fillRect(x, ctx, {tx, b.h / 2 - 2, std::max(0.0f, b.w - 10 - tx), 4}, {0.75f, 0.75f, 0.78f, 1});
    CG::fillRect(x, ctx, {tx - 10, b.h / 2 - 10, 20, 20}, {1, 1, 1, 1});
}

struct SegControlData : ObjC::HostObject {
    struct Seg { id title = 0, image = 0; float width = 0; bool enabled = true; };
    std::vector<Seg> segs;
    int32_t selected = -1;
    bool momentary = false;
};
SegControlData& SGD(Ctx& x, id s) { return rt(x).host<SegControlData>(s); }

id sc_initWithItems(Ctx& x, id self, SEL, id items) {
    const id me = id(sendRect(x, self, rt(x).sel("initWithFrame:"), {0, 0, 200, 29}));
    if (me && items) {
        auto& d = SGD(x, me);
        for (id it : Foundation::arrayItems(x, items)) {
            SegControlData::Seg seg;
            if (Foundation::isString(x, it)) seg.title = rt(x).retain(x.cpu, it);
            else seg.image = rt(x).retain(x.cpu, it);
            d.segs.push_back(seg);
        }
    }
    return me;
}
uint32_t sc_numSegs(Ctx& x, id self, SEL) { return uint32_t(SGD(x, self).segs.size()); }
int32_t sc_selected(Ctx& x, id self, SEL) { return SGD(x, self).selected; }
void sc_setSelected(Ctx& x, id self, SEL, int32_t idx) { SGD(x, self).selected = idx; rt(x).send(x.cpu, self, "setNeedsDisplay"); }
void sc_insertTitle(Ctx& x, id self, SEL, id title, uint32_t idx, bool) {
    auto& s = SGD(x, self).segs;
    SegControlData::Seg seg;
    if (title) seg.title = rt(x).retain(x.cpu, title);
    s.insert(s.begin() + std::min(size_t(idx), s.size()), seg);
    rt(x).send(x.cpu, self, "setNeedsDisplay");
}
void sc_setTitle(Ctx& x, id self, SEL, id title, uint32_t idx) {
    auto& s = SGD(x, self).segs;
    if (idx < s.size()) { setRetained(x, s[idx].title, title); rt(x).send(x.cpu, self, "setNeedsDisplay"); }
}
id sc_titleFor(Ctx& x, id self, SEL, uint32_t idx) {
    auto& s = SGD(x, self).segs;
    return idx < s.size() ? s[idx].title : 0;
}
void sc_removeAll(Ctx& x, id self, SEL) {
    auto& s = SGD(x, self).segs;
    for (auto& g : s) { if (g.title) rt(x).release(x.cpu, g.title); if (g.image) rt(x).release(x.cpu, g.image); }
    s.clear();
    SGD(x, self).selected = -1;
}
void sc_touchesEnded(Ctx& x, id self, SEL, id touches, id) {
    auto& d = SGD(x, self);
    if (d.segs.empty()) return;
    const G::Point p = touchInView(x, touches, self);
    const G::Rect b = boundsOf(x, self);
    const int32_t idx = int32_t(std::clamp(p.x / (b.w / float(d.segs.size())), 0.0f, float(d.segs.size() - 1)));
    if (d.selected != idx) {
        d.selected = idx;
        rt(x).send(x.cpu, self, "setNeedsDisplay");
        rt(x).send(x.cpu, self, "sendActionsForControlEvents:", {1u << 12});
    }
}

struct ProgressData : ObjC::HostObject { float progress = 0; int32_t style = 0; };
ProgressData& PGD(Ctx& x, id p) { return rt(x).host<ProgressData>(p); }
id pv_initWithStyle(Ctx& x, id self, SEL, int32_t style) {
    const id me = id(sendRect(x, self, rt(x).sel("initWithFrame:"), {0, 0, 160, 9}));
    if (me) PGD(x, me).style = style;
    return me;
}
float pv_progress(Ctx& x, id self, SEL) { return PGD(x, self).progress; }
void pv_setProgress(Ctx& x, id self, SEL, float p) { PGD(x, self).progress = std::clamp(p, 0.0f, 1.0f); rt(x).send(x.cpu, self, "setNeedsDisplay"); }
void pv_setProgressAnim(Ctx& x, id self, SEL s, float p, bool) { pv_setProgress(x, self, s, p); }

struct PageControlData : ObjC::HostObject { int32_t pages = 0, current = 0; bool hidesSingle = false; };
PageControlData& PCD(Ctx& x, id p) { return rt(x).host<PageControlData>(p); }
int32_t pc_pages(Ctx& x, id self, SEL) { return PCD(x, self).pages; }
void pc_setPages(Ctx& x, id self, SEL, int32_t n) { PCD(x, self).pages = std::max(0, n); rt(x).send(x.cpu, self, "setNeedsDisplay"); }
int32_t pc_current(Ctx& x, id self, SEL) { return PCD(x, self).current; }
void pc_setCurrent(Ctx& x, id self, SEL, int32_t c) { PCD(x, self).current = std::clamp(c, 0, std::max(0, PCD(x, self).pages - 1)); rt(x).send(x.cpu, self, "setNeedsDisplay"); }

// ============================================================ UISearchBar ===

struct SearchBarData : ObjC::HostObject {
    id text = 0, placeholder = 0, prompt = 0, delegate = 0;
    id scopeButtonTitles = 0, tintColor = 0, barTintColor = 0;
    int32_t barStyle = 0, selectedScopeButtonIndex = 0;
    bool showsCancelButton = false, showsBookmarkButton = false, showsScopeBar = false, translucent = false;
};
SearchBarData& SRBD(Ctx& x, id sb) { return rt(x).host<SearchBarData>(sb); }

id sb_initWithFrame(Ctx& x, id self, SEL s, CGRect f) {
    if (f.size.height <= 0) f.size.height = 44;
    const id me = id(rt(x).sendSuper(x.cpu, self, ui(x).view, s, {fbits(f.origin.x), fbits(f.origin.y), fbits(f.size.width), fbits(f.size.height)}));
    if (me) {
        SearchBarData& d = SRBD(x, me);
        d.text = rt(x).retain(x.cpu, makeString(x, ""));
        rt(x).send(x.cpu, me, "setBackgroundColor:", {makeUIColor(x, {0.8f, 0.82f, 0.85f, 1})});
    }
    return me;
}
id sb_initWithCoder(Ctx& x, id self, SEL s, id coder) {
    const id me = id(rt(x).sendSuper(x.cpu, self, ui(x).view, s, {coder}));
    if (me) {
        SearchBarData& d = SRBD(x, me);
        setRetained(x, d.text, decode(x, coder, "UIText") ? decode(x, coder, "UIText") : makeString(x, ""));
        setRetained(x, d.placeholder, decode(x, coder, "UIPlaceholder"));
        setRetained(x, d.prompt, decode(x, coder, "UIPrompt"));
        if (has(x, coder, "UIShowsCancelButton")) d.showsCancelButton = decodeBool(x, coder, "UIShowsCancelButton");
        if (has(x, coder, "UIBarStyle")) d.barStyle = decodeInt(x, coder, "UIBarStyle");
    }
    return me;
}
void sb_dealloc(Ctx& x, id self, SEL s) {
    if (SearchBarData* d = rt(x).hostIf<SearchBarData>(self)) {
        for (id* o : {&d->text, &d->placeholder, &d->prompt, &d->scopeButtonTitles, &d->tintColor, &d->barTintColor}) {
            if (*o) rt(x).release(x.cpu, *o);
            *o = 0;
        }
    }
    rt(x).sendSuper(x.cpu, self, ui(x).view, s);
}
id sb_text(Ctx& x, id self, SEL) { return SRBD(x, self).text ? SRBD(x, self).text : makeString(x, ""); }
void sb_setText(Ctx& x, id self, SEL, id t) {
    setRetained(x, SRBD(x, self).text, t ? autoreleased(x, id(rt(x).send(x.cpu, t, "copy"))) : makeString(x, ""));
    rt(x).send(x.cpu, self, "setNeedsDisplay");
}
id sb_placeholder(Ctx& x, id self, SEL) { return SRBD(x, self).placeholder; }
void sb_setPlaceholder(Ctx& x, id self, SEL, id p) {
    setRetained(x, SRBD(x, self).placeholder, p ? autoreleased(x, id(rt(x).send(x.cpu, p, "copy"))) : 0);
    rt(x).send(x.cpu, self, "setNeedsDisplay");
}
id sb_prompt(Ctx& x, id self, SEL) { return SRBD(x, self).prompt; }
void sb_setPrompt(Ctx& x, id self, SEL, id p) {
    setRetained(x, SRBD(x, self).prompt, p ? autoreleased(x, id(rt(x).send(x.cpu, p, "copy"))) : 0);
    rt(x).send(x.cpu, self, "setNeedsDisplay");
}
id sb_delegate(Ctx& x, id self, SEL) { return SRBD(x, self).delegate; }
void sb_setDelegate(Ctx& x, id self, SEL, id d) { SRBD(x, self).delegate = d; }
bool sb_showsCancel(Ctx& x, id self, SEL) { return SRBD(x, self).showsCancelButton; }
void sb_setShowsCancel(Ctx& x, id self, SEL, bool b) { SRBD(x, self).showsCancelButton = b; rt(x).send(x.cpu, self, "setNeedsDisplay"); }
void sb_setShowsCancelAnim(Ctx& x, id self, SEL s, bool b, bool) { sb_setShowsCancel(x, self, s, b); }
bool sb_showsBookmark(Ctx& x, id self, SEL) { return SRBD(x, self).showsBookmarkButton; }
void sb_setShowsBookmark(Ctx& x, id self, SEL, bool b) { SRBD(x, self).showsBookmarkButton = b; }
int32_t sb_barStyle(Ctx& x, id self, SEL) { return SRBD(x, self).barStyle; }
void sb_setBarStyle(Ctx& x, id self, SEL, int32_t s) { SRBD(x, self).barStyle = s; }
bool sb_translucent(Ctx& x, id self, SEL) { return SRBD(x, self).translucent; }
void sb_setTranslucent(Ctx& x, id self, SEL, bool b) { SRBD(x, self).translucent = b; }
id sb_tintColor(Ctx& x, id self, SEL) { return SRBD(x, self).tintColor; }
void sb_setTintColor(Ctx& x, id self, SEL, id c) { setRetained(x, SRBD(x, self).tintColor, c); }
id sb_barTintColor(Ctx& x, id self, SEL) { return SRBD(x, self).barTintColor; }
void sb_setBarTintColor(Ctx& x, id self, SEL, id c) { setRetained(x, SRBD(x, self).barTintColor, c); rt(x).send(x.cpu, self, "setBackgroundColor:", {c}); }
id sb_scopeTitles(Ctx& x, id self, SEL) { return SRBD(x, self).scopeButtonTitles; }
void sb_setScopeTitles(Ctx& x, id self, SEL, id arr) { setRetained(x, SRBD(x, self).scopeButtonTitles, arr ? autoreleased(x, id(rt(x).send(x.cpu, arr, "copy"))) : 0); }
int32_t sb_selectedScope(Ctx& x, id self, SEL) { return SRBD(x, self).selectedScopeButtonIndex; }
void sb_setSelectedScope(Ctx& x, id self, SEL, int32_t idx) { SRBD(x, self).selectedScopeButtonIndex = idx; }
bool sb_showsScopeBar(Ctx& x, id self, SEL) { return SRBD(x, self).showsScopeBar; }
void sb_setShowsScopeBar(Ctx& x, id self, SEL, bool b) { SRBD(x, self).showsScopeBar = b; }

void sb_drawRect(Ctx& x, id self, SEL, CGRect) {
    const id ctx = CG::currentContext(x);
    if (!ctx) return;
    const SearchBarData& d = SRBD(x, self);
    const G::Rect b = boundsOf(x, self);
    CG::fillRect(x, ctx, b, d.barTintColor ? uiColor(x, d.barTintColor) : G::Color{0.8f, 0.82f, 0.85f, 1});
    const float fieldW = d.showsCancelButton ? b.w - 70 : b.w - 12;
    CG::fillRect(x, ctx, {6, 6, fieldW, b.h - 12}, {1, 1, 1, 1});
    const std::string txt = textOf(x, d.text);
    const G::FontDesc fd{"Helvetica", 14, false, false};
    if (!txt.empty()) {
        drawText(x, ctx, txt, fd, {12, (b.h - fd.size) / 2, fieldW - 12, fd.size + 2}, {0, 0, 0, 1}, 4, 0, 1);
    } else if (d.placeholder) {
        const std::string ph = textOf(x, d.placeholder);
        if (!ph.empty()) drawText(x, ctx, ph, fd, {12, (b.h - fd.size) / 2, fieldW - 12, fd.size + 2}, {0.6f, 0.6f, 0.65f, 1}, 4, 0, 1);
    }
    if (d.showsCancelButton) drawText(x, ctx, "Cancel", {"Helvetica", 15, false, false}, {b.w - 58, (b.h - 18) / 2, 52, 18}, {0, 0.478f, 1, 1}, 4, 1, 1);
}

// ======================================= UIStoryboard & UIStoryboardSegue ===

struct StoryboardData : ObjC::HostObject {
    std::string name;
    id bundle = 0;
    std::string dirPath;
    std::string initialNib;
    std::unordered_map<std::string, std::string> idToNib;
};
StoryboardData& SBD(Ctx& x, id s) { return rt(x).host<StoryboardData>(s); }

id sb_withName(Ctx& x, id self, SEL, id name, id bundle) {
    if (!name) return 0;
    const std::string nm = utf8(x, name);
    const id sb = rt(x).createInstance(self);
    StoryboardData& d = SBD(x, sb);
    d.name = nm;
    d.bundle = bundle ? rt(x).retain(x.cpu, bundle) : 0;
    const std::string p = Foundation::bundleResource(x, nm, "storyboardc");
    if (!p.empty()) {
        d.dirPath = p;
        const fs::path infoPath = fs::path(p) / "Info.plist";
        std::error_code ec;
        if (fs::exists(infoPath, ec)) {
            auto plist = Plist::parseFile(infoPath.string());
            if (plist) {
                d.initialNib = plist->getString("UIStoryboardDesignatedEntryPointIdentifier");
                if (const Plist::Value* map = plist->get("UIViewControllerIdentifiersToNibNames"); map && map->type == Plist::Value::Type::Dict) {
                    for (auto& [k, v] : map->dict) d.idToNib[k] = v->s;
                }
            }
        }
    }
    return autoreleased(x, sb);
}

id sb_instantiateWithId(Ctx& x, id self, SEL, id ident) {
    if (!ident) return 0;
    StoryboardData& d = SBD(x, self);
    const std::string idStr = utf8(x, ident);
    std::string nibName = idStr;
    auto it = d.idToNib.find(idStr);
    if (it != d.idToNib.end()) nibName = it->second;

    if (!d.dirPath.empty()) {
        const std::string rel = d.name + ".storyboardc/" + nibName;
        const id arr = loadNib(x, rel, d.bundle, 0, 0);
        const auto items = Foundation::arrayItems(x, arr);
        for (id o : items) if (rt(x).isKindOf(o, ui(x).viewController)) return o;
        if (!items.empty()) return items.front();
    }
    if (nibExists(x, nibName)) {
        const id arr = loadNib(x, nibName, d.bundle, 0, 0);
        const auto items = Foundation::arrayItems(x, arr);
        for (id o : items) if (rt(x).isKindOf(o, ui(x).viewController)) return o;
    }
    const Class cls = rt(x).getClass(idStr);
    if (cls && rt(x).isSubclass(cls, ui(x).viewController)) return autoreleased(x, allocInit(x, cls));
    return 0;
}

id sb_instantiateInitial(Ctx& x, id self, SEL s) {
    StoryboardData& d = SBD(x, self);
    if (!d.initialNib.empty()) return sb_instantiateWithId(x, self, s, makeString(x, d.initialNib));
    if (!d.idToNib.empty()) return sb_instantiateWithId(x, self, s, makeString(x, d.idToNib.begin()->first));
    return 0;
}

struct SegueData : ObjC::HostObject {
    id identifier = 0, source = 0, destination = 0;
    uint32_t handlerBlock = 0;
};
SegueData& SEGD(Ctx& x, id s) { return rt(x).host<SegueData>(s); }

id seg_init(Ctx& x, id self, SEL, id ident, id src, id dst) {
    SegueData& d = SEGD(x, self);
    setRetained(x, d.identifier, ident ? autoreleased(x, id(rt(x).send(x.cpu, ident, "copy"))) : 0);
    setRetained(x, d.source, src);
    setRetained(x, d.destination, dst);
    return self;
}
id seg_withHandler(Ctx& x, id self, SEL, id ident, id src, id dst, uint32_t block) {
    const id s = id(rt(x).send(x.cpu, id(rt(x).send(x.cpu, self, rt(x).selAlloc)), "initWithIdentifier:source:destination:", {ident, src, dst}));
    if (s && block) SEGD(x, s).handlerBlock = ObjC::blockCopy(x, block);
    return autoreleased(x, s);
}
id seg_identifier(Ctx& x, id self, SEL) { return SEGD(x, self).identifier; }
id seg_source(Ctx& x, id self, SEL) { return SEGD(x, self).source; }
id seg_dest(Ctx& x, id self, SEL) { return SEGD(x, self).destination; }
void seg_perform(Ctx& x, id self, SEL) {
    SegueData& d = SEGD(x, self);
    if (d.handlerBlock) {
        ObjC::callBlock(x, d.handlerBlock);
        return;
    }
    if (!d.source || !d.destination) return;
    const id nav = id(rt(x).send(x.cpu, d.source, "navigationController"));
    if (nav) rt(x).send(x.cpu, nav, "pushViewController:animated:", {d.destination, 1});
    else rt(x).send(x.cpu, d.source, "presentViewController:animated:completion:", {d.destination, 1, 0});
}

} // namespace

void registerUIWidgets(Process& p, ObjC::Runtime& r) {
    auto& u = ui(p);
    const std::string R = kR, P = kP, S = kS, E = kE;

    // UIScrollView
    ObjC::ClassBuilder sv = r.defineClass("UIScrollView", "UIView");
    sv.m<&sv_initWithFrame>("initWithFrame:", "@@:" + R)
      .m<&sv_initWithCoder>("initWithCoder:")
      .m<&sv_contentOffset>("contentOffset", P + "@:")
      .m<&sv_setContentOffset>("setContentOffset:", "v@:" + P)
      .m<&sv_setContentOffsetAnim>("setContentOffset:animated:", "v@:" + P + "c")
      .m<&sv_contentSize>("contentSize", S + "@:")
      .m<&sv_setContentSize>("setContentSize:", "v@:" + S)
      .m<&sv_contentInset>("contentInset", E + "@:")
      .m<&sv_setContentInset>("setContentInset:", "v@:" + E)
      .m<&sv_indicatorInsets>("scrollIndicatorInsets", E + "@:")
      .m<&sv_setIndicatorInsets>("setScrollIndicatorInsets:", "v@:" + E)
      .m<&sv_delegate>("delegate")
      .m<&sv_setDelegate>("setDelegate:", "v@:@")
      .m<&sv_scrollEnabled>("isScrollEnabled", "c@:")
      .m<&sv_setScrollEnabled>("setScrollEnabled:", "v@:c")
      .m<&sv_pagingEnabled>("isPagingEnabled", "c@:")
      .m<&sv_setPagingEnabled>("setPagingEnabled:", "v@:c")
      .m<&sv_bounces>("bounces", "c@:")
      .m<&sv_setBounces>("setBounces:", "v@:c")
      .m<&sv_alwaysV>("alwaysBounceVertical", "c@:")
      .m<&sv_setAlwaysV>("setAlwaysBounceVertical:", "v@:c")
      .m<&sv_alwaysH>("alwaysBounceHorizontal", "c@:")
      .m<&sv_setAlwaysH>("setAlwaysBounceHorizontal:", "v@:c")
      .m<&sv_showsH>("showsHorizontalScrollIndicator", "c@:")
      .m<&sv_setShowsH>("setShowsHorizontalScrollIndicator:", "v@:c")
      .m<&sv_showsV>("showsVerticalScrollIndicator", "c@:")
      .m<&sv_setShowsV>("setShowsVerticalScrollIndicator:", "v@:c")
      .m<&sv_tracking>("isTracking", "c@:")
      .m<&sv_dragging>("isDragging", "c@:")
      .m<&sv_decelerating>("isDecelerating", "c@:")
      .m<&sv_zooming>("isZooming", "c@:")
      .m<&sv_zoomScale>("zoomScale", "f@:")
      .m<&sv_setZoomScale>("setZoomScale:", "v@:f")
      .m<&sv_setZoomScaleAnim>("setZoomScale:animated:", "v@:fc")
      .m<&sv_minZoom>("minimumZoomScale", "f@:")
      .m<&sv_setMinZoom>("setMinimumZoomScale:", "v@:f")
      .m<&sv_maxZoom>("maximumZoomScale", "f@:")
      .m<&sv_setMaxZoom>("setMaximumZoomScale:", "v@:f")
      .m<&sv_indStyle>("indicatorStyle", "i@:")
      .m<&sv_setIndStyle>("setIndicatorStyle:", "v@:i")
      .m<&sv_decelRate>("decelerationRate", "f@:")
      .m<&sv_setDecelRate>("setDecelerationRate:", "v@:f")
      .m<&sv_delays>("delaysContentTouches", "c@:")
      .m<&sv_setDelays>("setDelaysContentTouches:", "v@:c")
      .m<&sv_canCancel>("canCancelContentTouches", "c@:")
      .m<&sv_setCanCancel>("setCanCancelContentTouches:", "v@:c")
      .m<&sv_dirLock>("isDirectionalLockEnabled", "c@:")
      .m<&sv_setDirLock>("setDirectionalLockEnabled:", "v@:c")
      .m<&sv_scrollsToTop>("scrollsToTop", "c@:")
      .m<&sv_setScrollsToTop>("setScrollsToTop:", "v@:c")
      .m<&sv_flash>("flashScrollIndicators", "v@:")
      .m<&sv_zoomToRect>("zoomToRect:animated:", "v@:" + R + "c")
      .m<&sv_scrollRectToVisible>("scrollRectToVisible:animated:", "v@:" + R + "c")
      .m<&sv_touchesBegan>("touchesBegan:withEvent:", "v@:@@")
      .m<&sv_touchesMoved>("touchesMoved:withEvent:", "v@:@@")
      .m<&sv_touchesEnded>("touchesEnded:withEvent:", "v@:@@")
      .m<&sv_touchesEnded>("touchesCancelled:withEvent:", "v@:@@");

    // UITableViewCell
    ObjC::ClassBuilder tc = r.defineClass("UITableViewCell", "UIView");
    tc.m<&tc_initWithStyle>("initWithStyle:reuseIdentifier:", "@@:i@")
      .m<&tc_initWithFrameReuse>("initWithFrame:reuseIdentifier:", "@@:" + R + "@")
      .m<&tc_initWithFrame>("initWithFrame:", "@@:" + R)
      .m<&tc_initWithCoder>("initWithCoder:")
      .m<&tc_dealloc>("dealloc", "v@:")
      .m<&tc_contentView>("contentView")
      .m<&tc_textLabel>("textLabel")
      .m<&tc_detailLabel>("detailTextLabel")
      .m<&tc_imageView>("imageView")
      .m<&tc_reuseId>("reuseIdentifier")
      .m<&tc_prepareForReuse>("prepareForReuse", "v@:")
      .m<&tc_selStyle>("selectionStyle", "i@:")
      .m<&tc_setSelStyle>("setSelectionStyle:", "v@:i")
      .m<&tc_accType>("accessoryType", "i@:")
      .m<&tc_setAccType>("setAccessoryType:", "v@:i")
      .m<&tc_accView>("accessoryView")
      .m<&tc_setAccView>("setAccessoryView:", "v@:@")
      .m<&tc_bgView>("backgroundView")
      .m<&tc_setBgView>("setBackgroundView:", "v@:@")
      .m<&tc_selBgView>("selectedBackgroundView")
      .m<&tc_setSelBgView>("setSelectedBackgroundView:", "v@:@")
      .m<&tc_selected>("isSelected", "c@:")
      .m<&tc_setSelected>("setSelected:", "v@:c")
      .m<&tc_setSelectedAnim>("setSelected:animated:", "v@:cc")
      .m<&tc_highlighted>("isHighlighted", "c@:")
      .m<&tc_setHighlighted>("setHighlighted:", "v@:c")
      .m<&tc_setHighlightedAnim>("setHighlighted:animated:", "v@:cc")
      .m<&tc_editing>("isEditing", "c@:")
      .m<&tc_setEditing>("setEditing:", "v@:c")
      .m<&tc_setEditingAnim>("setEditing:animated:", "v@:cc")
      .m<&tc_layoutSubviews>("layoutSubviews", "v@:")
      .m<&tc_touchesEnded>("touchesEnded:withEvent:", "v@:@@");

    // UITableView & UITableViewController
    ObjC::ClassBuilder tv = r.defineClass("UITableView", "UIScrollView");
    tv.m<&tv_initWithFrameStyle>("initWithFrame:style:", "@@:" + R + "i")
      .m<&tv_initWithFrame>("initWithFrame:", "@@:" + R)
      .m<&tv_initWithCoder>("initWithCoder:")
      .m<&tv_dealloc>("dealloc", "v@:")
      .m<&tv_style>("style", "i@:")
      .m<&tv_dataSource>("dataSource")
      .m<&tv_setDataSource>("setDataSource:", "v@:@")
      .m<&tv_rowHeight>("rowHeight", "f@:")
      .m<&tv_setRowHeight>("setRowHeight:", "v@:f")
      .m<&tv_secHeaderH>("sectionHeaderHeight", "f@:")
      .m<&tv_setSecHeaderH>("setSectionHeaderHeight:", "v@:f")
      .m<&tv_secFooterH>("sectionFooterHeight", "f@:")
      .m<&tv_setSecFooterH>("setSectionFooterHeight:", "v@:f")
      .m<&tv_sepStyle>("separatorStyle", "i@:")
      .m<&tv_setSepStyle>("setSeparatorStyle:", "v@:i")
      .m<&tv_sepColor>("separatorColor")
      .m<&tv_setSepColor>("setSeparatorColor:", "v@:@")
      .m<&tv_headerView>("tableHeaderView")
      .m<&tv_setHeaderView>("setTableHeaderView:", "v@:@")
      .m<&tv_footerView>("tableFooterView")
      .m<&tv_setFooterView>("setTableFooterView:", "v@:@")
      .m<&tv_bgView>("backgroundView")
      .m<&tv_setBgView>("setBackgroundView:", "v@:@")
      .m<&tv_allowsSel>("allowsSelection", "c@:")
      .m<&tv_setAllowsSel>("setAllowsSelection:", "v@:c")
      .m<&tv_allowsMulti>("allowsMultipleSelection", "c@:")
      .m<&tv_setAllowsMulti>("setAllowsMultipleSelection:", "v@:c")
      .m<&tv_editing>("isEditing", "c@:")
      .m<&tv_setEditing>("setEditing:", "v@:c")
      .m<&tv_setEditingAnim>("setEditing:animated:", "v@:cc")
      .m<&tv_registerClass>("registerClass:forCellReuseIdentifier:", "v@:#@")
      .m<&tv_registerNib>("registerNib:forCellReuseIdentifier:", "v@:@@")
      .m<&tv_dequeue>("dequeueReusableCellWithIdentifier:")
      .m<&tv_dequeueForIndexPath>("dequeueReusableCellWithIdentifier:forIndexPath:")
      .m<&tv_reloadData>("reloadData", "v@:")
      .m<&tv_layoutSubviews>("layoutSubviews", "v@:")
      .m<&tv_numberOfSections>("numberOfSections", "i@:")
      .m<&tv_numberOfRowsInSection>("numberOfRowsInSection:", "i@:i")
      .m<&tv_cellForRow>("cellForRowAtIndexPath:")
      .m<&tv_visibleCells>("visibleCells")
      .m<&tv_indexPathsForVisibleRows>("indexPathsForVisibleRows")
      .m<&tv_indexPathForCell>("indexPathForCell:")
      .m<&tv_indexPathForRowAtPoint>("indexPathForRowAtPoint:", "@@:" + P)
      .m<&tv_rectForRow>("rectForRowAtIndexPath:", R + "@:@")
      .m<&tv_indexPathForSelectedRow>("indexPathForSelectedRow")
      .m<&tv_selectRow>("selectRowAtIndexPath:animated:scrollPosition:", "v@:@ci")
      .m<&tv_deselectRow>("deselectRowAtIndexPath:animated:", "v@:@c")
      .m<&tv_scrollToRow>("scrollToRowAtIndexPath:atScrollPosition:animated:", "v@:@ic")
      .m<&tv_beginUpdates>("beginUpdates", "v@:")
      .m<&tv_endUpdates>("endUpdates", "v@:")
      .m<&tv_mutateRows>("insertRowsAtIndexPaths:withRowAnimation:", "v@:@i")
      .m<&tv_mutateRows>("deleteRowsAtIndexPaths:withRowAnimation:", "v@:@i")
      .m<&tv_mutateRows>("reloadRowsAtIndexPaths:withRowAnimation:", "v@:@i")
      .m<&tv_mutateRows>("reloadSections:withRowAnimation:", "v@:@i");

    ObjC::ClassBuilder tvc = r.defineClass("UITableViewController", "UIViewController");
    tvc.m<&tvc_initWithStyle>("initWithStyle:", "@@:i")
       .m<&tvc_loadView>("loadView", "v@:")
       .m<&tvc_tableView>("tableView")
       .m<&tvc_setTableView>("setTableView:", "v@:@");

    // UIBarItem, UIBarButtonItem, UITabBarItem
    ObjC::ClassBuilder bi = r.defineClass("UIBarItem", "NSObject");
    bi.m<&bi_initWithCoder>("initWithCoder:")
      .m<&bi_dealloc>("dealloc", "v@:")
      .m<&bi_title>("title").m<&bi_setTitle>("setTitle:", "v@:@")
      .m<&bi_image>("image").m<&bi_setImage>("setImage:", "v@:@")
      .m<&bi_tag>("tag", "i@:").m<&bi_setTag>("setTag:", "v@:i")
      .m<&bi_enabled>("isEnabled", "c@:").m<&bi_setEnabled>("setEnabled:", "v@:c");

    ObjC::ClassBuilder bbi = r.defineClass("UIBarButtonItem", "UIBarItem");
    bbi.m<&bi_initWithTitle>("initWithTitle:style:target:action:", "@@:@i@:")
       .m<&bi_initWithImage>("initWithImage:style:target:action:", "@@:@i@:")
       .m<&bi_initWithSystem>("initWithBarButtonSystemItem:target:action:", "@@:i@:")
       .m<&bi_initWithCustomView>("initWithCustomView:")
       .m<&bi_customView>("customView").m<&bi_setCustomView>("setCustomView:", "v@:@")
       .m<&bi_target>("target").m<&bi_setTarget>("setTarget:", "v@:@")
       .m<&bi_action>("action", ":@:").m<&bi_setAction>("setAction:", "v@::")
       .m<&bi_addTarget>("addTarget:action:forControlEvents:", "v@:@:I")
       .m<&bi_removeTarget>("removeTarget:action:forControlEvents:", "v@:@:I")
       .m<&bi_style>("style", "i@:").m<&bi_setStyle>("setStyle:", "v@:i")
       .m<&bi_width>("width", "f@:").m<&bi_setWidth>("setWidth:", "v@:f")
       .m<&bi_tintColor>("tintColor").m<&bi_setTintColor>("setTintColor:", "v@:@");

    ObjC::ClassBuilder tbi = r.defineClass("UITabBarItem", "UIBarItem");
    tbi.m<&tbi_initWithTitleImageTag>("initWithTitle:image:tag:", "@@:@@i")
       .m<&tbi_initWithTitleImageSel>("initWithTitle:image:selectedImage:")
       .m<&tbi_initWithSystemTag>("initWithTabBarSystemItem:tag:", "@@:ii")
       .m<&tbi_badge>("badgeValue").m<&tbi_setBadge>("setBadgeValue:", "v@:@");

    // UINavigationItem, UINavigationBar, UIToolbar, UINavigationController
    ObjC::ClassBuilder ni = r.defineClass("UINavigationItem", "NSObject");
    ni.m<&ni_initWithTitle>("initWithTitle:").m<&ni_init>("init").m<&ni_initWithCoder>("initWithCoder:")
      .m<&ni_dealloc>("dealloc", "v@:").m<&ni_title>("title").m<&ni_setTitle>("setTitle:", "v@:@")
      .m<&ni_prompt>("prompt").m<&ni_setPrompt>("setPrompt:", "v@:@")
      .m<&ni_backItem>("backBarButtonItem").m<&ni_setBackItem>("setBackBarButtonItem:", "v@:@")
      .m<&ni_leftItem>("leftBarButtonItem").m<&ni_setLeftItem>("setLeftBarButtonItem:", "v@:@")
      .m<&ni_setLeftItemAnim>("setLeftBarButtonItem:animated:", "v@:@c")
      .m<&ni_rightItem>("rightBarButtonItem").m<&ni_setRightItem>("setRightBarButtonItem:", "v@:@")
      .m<&ni_setRightItemAnim>("setRightBarButtonItem:animated:", "v@:@c")
      .m<&ni_titleView>("titleView").m<&ni_setTitleView>("setTitleView:", "v@:@")
      .m<&ni_hidesBack>("hidesBackButton", "c@:").m<&ni_setHidesBack>("setHidesBackButton:", "v@:c")
      .m<&ni_setHidesBackAnim>("setHidesBackButton:animated:", "v@:cc");

    ObjC::ClassBuilder nb = r.defineClass("UINavigationBar", "UIView");
    nb.m<&nb_initWithFrame>("initWithFrame:", "@@:" + R).m<&nb_dealloc>("dealloc", "v@:")
      .m<&nb_pushItem>("pushNavigationItem:animated:", "v@:@c")
      .m<&nb_popItem>("popNavigationItemAnimated:", "@@:c")
      .m<&nb_items>("items").m<&nb_setItemsSimple>("setItems:", "v@:@").m<&nb_setItems>("setItems:animated:", "v@:@c")
      .m<&nb_topItem>("topItem").m<&nb_backItem>("backItem")
      .m<&nb_delegate>("delegate").m<&nb_setDelegate>("setDelegate:", "v@:@")
      .m<&nb_barStyle>("barStyle", "i@:").m<&nb_setBarStyle>("setBarStyle:", "v@:i")
      .m<&nb_translucent>("isTranslucent", "c@:").m<&nb_setTranslucent>("setTranslucent:", "v@:c")
      .m<&nb_tintColor>("tintColor").m<&nb_setTintColor>("setTintColor:", "v@:@")
      .m<&nb_barTintColor>("barTintColor").m<&nb_setBarTintColor>("setBarTintColor:", "v@:@")
      .m<&nb_drawRect>("drawRect:", "v@:" + R)
      .m<&nb_touchesEnded>("touchesEnded:withEvent:", "v@:@@");

    ObjC::ClassBuilder tb = r.defineClass("UIToolbar", "UIView");
    tb.m<&nb_initWithFrame>("initWithFrame:", "@@:" + R).m<&nb_dealloc>("dealloc", "v@:")
      .m<&nb_items>("items").m<&nb_setItemsSimple>("setItems:", "v@:@").m<&nb_setItems>("setItems:animated:", "v@:@c")
      .m<&nb_barStyle>("barStyle", "i@:").m<&nb_setBarStyle>("setBarStyle:", "v@:i")
      .m<&nb_translucent>("isTranslucent", "c@:").m<&nb_setTranslucent>("setTranslucent:", "v@:c")
      .m<&nb_tintColor>("tintColor").m<&nb_setTintColor>("setTintColor:", "v@:@")
      .m<&nb_barTintColor>("barTintColor").m<&nb_setBarTintColor>("setBarTintColor:", "v@:@");

    ObjC::ClassBuilder nc = r.defineClass("UINavigationController", "UIViewController");
    nc.m<&nc_initWithRoot>("initWithRootViewController:").m<&nc_init>("init").m<&nc_initWithCoder>("initWithCoder:")
      .m<&nc_dealloc>("dealloc", "v@:").m<&nc_loadView>("loadView", "v@:")
      .m<&nc_push>("pushViewController:animated:", "v@:@c")
      .m<&nc_pop>("popViewControllerAnimated:", "@@:c")
      .m<&nc_popToRoot>("popToRootViewControllerAnimated:", "@@:c")
      .m<&nc_popToVC>("popToViewController:animated:", "@@:@c")
      .m<&nc_topVC>("topViewController").m<&nc_visibleVC>("visibleViewController")
      .m<&nc_viewControllers>("viewControllers")
      .m<&nc_setViewControllers>("setViewControllers:", "v@:@")
      .m<&nc_setViewControllersAnim>("setViewControllers:animated:", "v@:@c")
      .m<&nc_navBar>("navigationBar").m<&nc_toolbar>("toolbar")
      .m<&nc_navBarHidden>("isNavigationBarHidden", "c@:").m<&nc_setNavBarHidden>("setNavigationBarHidden:", "v@:c")
      .m<&nc_setNavBarHiddenAnim>("setNavigationBarHidden:animated:", "v@:cc")
      .m<&nc_toolbarHidden>("isToolbarHidden", "c@:").m<&nc_setToolbarHidden>("setToolbarHidden:", "v@:c")
      .m<&nc_setToolbarHiddenAnim>("setToolbarHidden:animated:", "v@:cc")
      .m<&nc_delegate>("delegate").m<&nc_setDelegate>("setDelegate:", "v@:@");

    // UITabBar & UITabBarController
    ObjC::ClassBuilder tabBar = r.defineClass("UITabBar", "UIView");
    tabBar.m<&tb_initWithFrame>("initWithFrame:", "@@:" + R)
          .m<&tb_items>("items").m<&tb_setItemsSimple>("setItems:", "v@:@").m<&tb_setItems>("setItems:animated:", "v@:@c")
          .m<&tb_selectedItem>("selectedItem").m<&tb_setSelectedItem>("setSelectedItem:", "v@:@")
          .m<&tb_delegate>("delegate").m<&tb_setDelegate>("setDelegate:", "v@:@")
          .m<&tb_drawRect>("drawRect:", "v@:" + R)
          .m<&tb_touchesEnded>("touchesEnded:withEvent:", "v@:@@");

    ObjC::ClassBuilder tbc = r.defineClass("UITabBarController", "UIViewController");
    tbc.m<&tbc_init>("init").m<&tbc_loadView>("loadView", "v@:")
       .m<&tbc_viewControllers>("viewControllers")
       .m<&tbc_setViewControllers>("setViewControllers:", "v@:@")
       .m<&tbc_setViewControllersAnim>("setViewControllers:animated:", "v@:@c")
       .m<&tbc_selectedIndex>("selectedIndex", "I@:").m<&tbc_setSelectedIndex>("setSelectedIndex:", "v@:I")
       .m<&tbc_selectedVC>("selectedViewController").m<&tbc_setSelectedVC>("setSelectedViewController:", "v@:@")
       .m<&tbc_tabBar>("tabBar").m<&tbc_delegate>("delegate").m<&tbc_setDelegate>("setDelegate:", "v@:@");

    r.builder(u.viewController).m<&vc_tabBarItem>("tabBarItem").m<&vc_setTabBarItem>("setTabBarItem:", "v@:@");

    // UITextField & UITextView
    ObjC::ClassBuilder tf = r.defineClass("UITextField", "UIControl");
    tf.m<&tf_initWithFrame>("initWithFrame:", "@@:" + R).m<&tf_initWithCoder>("initWithCoder:").m<&tf_dealloc>("dealloc", "v@:")
      .m<&tf_text>("text").m<&tf_setText>("setText:", "v@:@")
      .m<&tf_placeholder>("placeholder").m<&tf_setPlaceholder>("setPlaceholder:", "v@:@")
      .m<&tf_font>("font").m<&tf_setFont>("setFont:", "v@:@")
      .m<&tf_textColor>("textColor").m<&tf_setTextColor>("setTextColor:", "v@:@")
      .m<&tf_alignment>("textAlignment", "i@:").m<&tf_setAlignment>("setTextAlignment:", "v@:i")
      .m<&tf_borderStyle>("borderStyle", "i@:").m<&tf_setBorderStyle>("setBorderStyle:", "v@:i")
      .m<&tf_delegate>("delegate").m<&tf_setDelegate>("setDelegate:", "v@:@")
      .m<&tf_editing>("isEditing", "c@:")
      .m<&tf_insertText>("insertText:","v@:@").m<&tf_deleteBackward>("deleteBackward","v@:").m<&tf_hasText>("hasText","c@:")
      .m<&tf_secure>("isSecureTextEntry", "c@:").m<&tf_setSecure>("setSecureTextEntry:", "v@:c")
      .m<&tf_canBecome>("canBecomeFirstResponder", "c@:")
      .m<&tf_become>("becomeFirstResponder", "c@:")
      .m<&tf_resign>("resignFirstResponder", "c@:")
      .m<&tf_setKeyboardType>("setKeyboardType:", "v@:i").m<&tf_keyboardType>("keyboardType", "i@:")
      .m<&tf_setReturnKeyType>("setReturnKeyType:", "v@:i").m<&tf_returnKeyType>("returnKeyType", "i@:")
      .m<&tf_setKeyboardAppearance>("setKeyboardAppearance:", "v@:i").m<&tf_keyboardAppearance>("keyboardAppearance","i@:")
      .m<&tf_setAutocorrection>("setAutocorrectionType:", "v@:i").m<&tf_autocorrection>("autocorrectionType","i@:")
      .m<&tf_setAutocapitalization>("setAutocapitalizationType:", "v@:i").m<&tf_autocapitalization>("autocapitalizationType","i@:")
      .m<&tf_ignoreInt>("setSpellCheckingType:", "v@:i").m<&tf_ignoreBool>("setEnablesReturnKeyAutomatically:", "v@:c")
      .m<&tf_ignoreInt>("setClearButtonMode:", "v@:i").m<&tf_setClearsOnBegin>("setClearsOnBeginEditing:", "v@:c").m<&tf_clearsOnBegin>("clearsOnBeginEditing","c@:")
      .m<&tf_ignoreInt>("setContentVerticalAlignment:", "v@:i").m<&tf_ignoreInt>("setContentHorizontalAlignment:", "v@:i")
      .m<&tf_ignoreObj>("setInputView:", "v@:@").m<&tf_ignoreObj>("setInputAccessoryView:", "v@:@")
      .m<&tf_nilObj>("inputView").m<&tf_nilObj>("inputAccessoryView")
      .m<&tf_ignoreObj>("setTypingAttributes:", "v@:@").m<&tf_ignoreBool>("setAdjustsFontSizeToFitWidth:", "v@:c")
      .m<&tf_ignoreObj>("setKeyboardLanguage:", "v@:@")
      .m<&tf_drawRect>("drawRect:", "v@:" + R);

    ObjC::ClassBuilder tvText = r.defineClass("UITextView", "UIScrollView");
    tvText.m<&tf_initWithFrame>("initWithFrame:", "@@:" + R).m<&tf_initWithCoder>("initWithCoder:")
          .m<&tf_text>("text").m<&tf_setText>("setText:", "v@:@")
          .m<&tf_insertText>("insertText:","v@:@").m<&tf_deleteBackward>("deleteBackward","v@:").m<&tf_hasText>("hasText","c@:")
          .m<&tf_font>("font").m<&tf_setFont>("setFont:", "v@:@")
          .m<&tf_textColor>("textColor").m<&tf_setTextColor>("setTextColor:", "v@:@")
          .m<&tf_alignment>("textAlignment", "i@:").m<&tf_setAlignment>("setTextAlignment:", "v@:i")
          .m<&tf_delegate>("delegate").m<&tf_setDelegate>("setDelegate:", "v@:@")
          .m<&tf_canBecome>("canBecomeFirstResponder", "c@:")
          .m<&tf_become>("becomeFirstResponder", "c@:")
          .m<&tf_resign>("resignFirstResponder", "c@:")
          .m<&tf_ignoreInt>("setKeyboardType:", "v@:i").m<&tf_zeroInt>("keyboardType", "i@:")
          .m<&tf_ignoreInt>("setReturnKeyType:", "v@:i").m<&tf_zeroInt>("returnKeyType", "i@:")
          .m<&tf_ignoreInt>("setKeyboardAppearance:", "v@:i").m<&tf_ignoreInt>("setAutocorrectionType:", "v@:i")
          .m<&tf_ignoreInt>("setAutocapitalizationType:", "v@:i").m<&tf_ignoreInt>("setSpellCheckingType:", "v@:i")
          .m<&tf_ignoreBool>("setEnablesReturnKeyAutomatically:", "v@:c").m<&tf_ignoreBool>("setSecureTextEntry:", "v@:c")
          .m<&tf_ignoreObj>("setInputView:", "v@:@").m<&tf_ignoreObj>("setInputAccessoryView:", "v@:@")
          .m<&tf_nilObj>("inputView").m<&tf_nilObj>("inputAccessoryView")
          .m<&tf_setEditable>("setEditable:", "v@:c").m<&tf_editable>("isEditable", "c@:")
          .m<&tf_setSelectable>("setSelectable:", "v@:c").m<&tf_selectable>("isSelectable", "c@:")
          .m<&tf_ignoreInt>("setDataDetectorTypes:", "v@:i").m<&tf_ignoreObj>("setLinkTextAttributes:", "v@:@")
          .m<&tf_ignoreBool>("setAllowsEditingTextAttributes:", "v@:c")
          .m<&tf_drawRect>("drawRect:", "v@:" + R);

    for (const char* n : {"UITextFieldTextDidBeginEditingNotification", "UITextFieldTextDidChangeNotification",
                          "UITextFieldTextDidEndEditingNotification", "UITextViewTextDidBeginEditingNotification",
                          "UITextViewTextDidChangeNotification", "UITextViewTextDidEndEditingNotification"}) {
        const uint32_t cell = p.defineDataBlock(std::string("_") + n, 4);
        p.mem().write<uint32_t>(cell, Foundation::constString(p, n));
    }

    // UISearchBar
    ObjC::ClassBuilder searchBar = r.defineClass("UISearchBar", "UIView");
    searchBar.m<&sb_initWithFrame>("initWithFrame:", "@@:" + R).m<&sb_initWithCoder>("initWithCoder:").m<&sb_dealloc>("dealloc", "v@:")
             .m<&sb_text>("text").m<&sb_setText>("setText:", "v@:@")
             .m<&sb_placeholder>("placeholder").m<&sb_setPlaceholder>("setPlaceholder:", "v@:@")
             .m<&sb_prompt>("prompt").m<&sb_setPrompt>("setPrompt:", "v@:@")
             .m<&sb_delegate>("delegate").m<&sb_setDelegate>("setDelegate:", "v@:@")
             .m<&sb_showsCancel>("showsCancelButton", "c@:").m<&sb_setShowsCancel>("setShowsCancelButton:", "v@:c")
             .m<&sb_setShowsCancelAnim>("setShowsCancelButton:animated:", "v@:cc")
             .m<&sb_showsBookmark>("showsBookmarkButton", "c@:").m<&sb_setShowsBookmark>("setShowsBookmarkButton:", "v@:c")
             .m<&sb_barStyle>("barStyle", "i@:").m<&sb_setBarStyle>("setBarStyle:", "v@:i")
             .m<&sb_translucent>("isTranslucent", "c@:").m<&sb_setTranslucent>("setTranslucent:", "v@:c")
             .m<&sb_tintColor>("tintColor").m<&sb_setTintColor>("setTintColor:", "v@:@")
             .m<&sb_barTintColor>("barTintColor").m<&sb_setBarTintColor>("setBarTintColor:", "v@:@")
             .m<&sb_scopeTitles>("scopeButtonTitles").m<&sb_setScopeTitles>("setScopeButtonTitles:", "v@:@")
             .m<&sb_selectedScope>("selectedScopeButtonIndex", "i@:").m<&sb_setSelectedScope>("setSelectedScopeButtonIndex:", "v@:i")
             .m<&sb_showsScopeBar>("showsScopeBar", "c@:").m<&sb_setShowsScopeBar>("setShowsScopeBar:", "v@:c")
             .m<&tf_ignoreInt>("setAutocapitalizationType:", "v@:i").m<&tf_ignoreInt>("setAutocorrectionType:", "v@:i")
             .m<&tf_ignoreInt>("setSpellCheckingType:", "v@:i").m<&tf_ignoreInt>("setKeyboardType:", "v@:i")
             .m<&sb_drawRect>("drawRect:", "v@:" + R);

    // UISwitch, UISlider, UISegmentedControl, UIProgressView, UIPageControl
    ObjC::ClassBuilder sw = r.defineClass("UISwitch", "UIControl");
    sw.m<&sw_initWithFrame>("initWithFrame:", "@@:" + R)
      .m<&sw_isOn>("isOn", "c@:").m<&sw_setOn>("setOn:", "v@:c").m<&sw_setOnAnim>("setOn:animated:", "v@:cc")
      .m<&sw_touchesEnded>("touchesEnded:withEvent:", "v@:@@").m<&sw_drawRect>("drawRect:", "v@:" + R);

    ObjC::ClassBuilder sl = r.defineClass("UISlider", "UIControl");
    sl.m<&sl_initWithFrame>("initWithFrame:", "@@:" + R)
      .m<&sl_value>("value", "f@:").m<&sl_setValue>("setValue:", "v@:f").m<&sl_setValueAnim>("setValue:animated:", "v@:fc")
      .m<&sl_min>("minimumValue", "f@:").m<&sl_setMin>("setMinimumValue:", "v@:f")
      .m<&sl_max>("maximumValue", "f@:").m<&sl_setMax>("setMaximumValue:", "v@:f")
      .m<&sl_cont>("isContinuous", "c@:").m<&sl_setCont>("setContinuous:", "v@:c")
      .m<&sl_setMinTrackImg>("setMinimumTrackImage:forState:", "v@:@I")
      .m<&sl_setMaxTrackImg>("setMaximumTrackImage:forState:", "v@:@I")
      .m<&sl_setThumbImg>("setThumbImage:forState:", "v@:@I")
      .m<&sl_minTrackImg>("minimumTrackImageForState:", "@@:I")
      .m<&sl_maxTrackImg>("maximumTrackImageForState:", "@@:I")
      .m<&sl_thumbImg>("thumbImageForState:", "@@:I")
      .m<&sl_curMinTrackImg>("currentMinimumTrackImage")
      .m<&sl_curMaxTrackImg>("currentMaximumTrackImage")
      .m<&sl_curThumbImg>("currentThumbImage")
      .m<&sl_minValImg>("minimumValueImage").m<&sl_setMinValImg>("setMinimumValueImage:", "v@:@")
      .m<&sl_maxValImg>("maximumValueImage").m<&sl_setMaxValImg>("setMaximumValueImage:", "v@:@")
      .m<&sl_minTrackTint>("minimumTrackTintColor").m<&sl_setMinTrackTint>("setMinimumTrackTintColor:", "v@:@")
      .m<&sl_maxTrackTint>("maximumTrackTintColor").m<&sl_setMaxTrackTint>("setMaximumTrackTintColor:", "v@:@")
      .m<&sl_thumbTint>("thumbTintColor").m<&sl_setThumbTint>("setThumbTintColor:", "v@:@")
      .m<&sl_drawRect>("drawRect:", "v@:" + R);

    ObjC::ClassBuilder seg = r.defineClass("UISegmentedControl", "UIControl");
    seg.m<&sc_initWithItems>("initWithItems:")
       .m<&sc_numSegs>("numberOfSegments", "I@:")
       .m<&sc_selected>("selectedSegmentIndex", "i@:").m<&sc_setSelected>("setSelectedSegmentIndex:", "v@:i")
       .m<&sc_insertTitle>("insertSegmentWithTitle:atIndex:animated:", "v@:@Ic")
       .m<&sc_setTitle>("setTitle:forSegmentAtIndex:", "v@:@I")
       .m<&sc_titleFor>("titleForSegmentAtIndex:")
       .m<&sc_removeAll>("removeAllSegments", "v@:")
       .m<&sc_touchesEnded>("touchesEnded:withEvent:", "v@:@@");

    ObjC::ClassBuilder pv = r.defineClass("UIProgressView", "UIView");
    pv.m<&pv_initWithStyle>("initWithProgressViewStyle:", "@@:i")
      .m<&pv_progress>("progress", "f@:").m<&pv_setProgress>("setProgress:", "v@:f")
      .m<&pv_setProgressAnim>("setProgress:animated:", "v@:fc");

    ObjC::ClassBuilder pc = r.defineClass("UIPageControl", "UIControl");
    pc.m<&pc_pages>("numberOfPages", "i@:").m<&pc_setPages>("setNumberOfPages:", "v@:i")
      .m<&pc_current>("currentPage", "i@:").m<&pc_setCurrent>("setCurrentPage:", "v@:i");

    // UIStoryboard & UIStoryboardSegue
    ObjC::ClassBuilder sb = r.defineClass("UIStoryboard", "NSObject");
    sb.cm<&sb_withName>("storyboardWithName:bundle:")
      .m<&sb_instantiateInitial>("instantiateInitialViewController")
      .m<&sb_instantiateWithId>("instantiateViewControllerWithIdentifier:");

    ObjC::ClassBuilder segue = r.defineClass("UIStoryboardSegue", "NSObject");
    segue.cm<&seg_withHandler>("segueWithIdentifier:source:destination:performHandler:")
         .m<&seg_init>("initWithIdentifier:source:destination:")
         .m<&seg_identifier>("identifier")
         .m<&seg_source>("sourceViewController")
         .m<&seg_dest>("destinationViewController")
         .m<&seg_perform>("perform", "v@:");

    // UIPasteboard
    struct PasteboardData : ObjC::HostObject {
        id name = 0;
        id itemsDict = 0;
        int32_t changeCount = 1;
        bool persistent = true;
    };
    auto getPbData = [](Ctx& x, id self) -> PasteboardData& {
        auto& d = rt(x).host<PasteboardData>(self);
        if (!d.itemsDict) d.itemsDict = Foundation::makeDict(x, {}, true, true);
        return d;
    };
    ObjC::ClassBuilder pb = r.defineClass("UIPasteboard", "NSObject");
    pb.classMethod("pasteboardWithName:create:", [getPbData](Ctx& x) {
        const id cls = x.cpu.r[0];
        const id nameObj = x.cpu.r[2];
        const std::string name = nameObj ? utf8(x, nameObj) : "com.apple.UIKit.pboard.general";
        static const char mapKey = 0;
        uint32_t& dictSlot = x.proc.processSlot(&mapKey);
        if (!dictSlot) dictSlot = Foundation::makeDict(x, {}, true, true);
        const id k = makeString(x, name);
        id inst = Foundation::dictGet(x, dictSlot, k);
        if (!inst) {
            inst = rt(x).createInstance(cls);
            rt(x).makeImmortal(inst);
            auto& d = getPbData(x, inst);
            d.name = Foundation::makeString(x, name, true);
            rt(x).send(x.cpu, dictSlot, "setObject:forKey:", {inst, k});
        }
        x.cpu.r[0] = inst;
    }, "@@:@c");
    pb.classMethod("generalPasteboard", [](Ctx& x) {
        const id cls = x.cpu.r[0];
        x.cpu.r[0] = id(rt(x).send(x.cpu, cls, "pasteboardWithName:create:", {makeString(x, "com.apple.UIKit.pboard.general"), 1}));
    }, "@@:");
    pb.classMethod("pasteboardWithUniqueName", [](Ctx& x) {
        const id cls = x.cpu.r[0];
        static int seq = 1;
        x.cpu.r[0] = id(rt(x).send(x.cpu, cls, "pasteboardWithName:create:", {makeString(x, "com.apple.UIKit.pboard.unique." + std::to_string(seq++)), 1}));
    }, "@@:");
    pb.classMethod("removePasteboardWithName:", [](Ctx&) {}, "v@:@");
    pb.method("name", [getPbData](Ctx& x) { x.cpu.r[0] = getPbData(x, x.cpu.r[0]).name; }, "@@:");
    pb.method("isPersistent", [getPbData](Ctx& x) { x.cpu.r[0] = getPbData(x, x.cpu.r[0]).persistent ? 1 : 0; }, "c@:");
    pb.method("setPersistent:", [getPbData](Ctx& x) { getPbData(x, x.cpu.r[0]).persistent = (x.cpu.r[2] & 0xFF) != 0; }, "v@:c");
    pb.method("changeCount", [getPbData](Ctx& x) { x.cpu.r[0] = uint32_t(getPbData(x, x.cpu.r[0]).changeCount); }, "i@:");
    pb.method("string", [getPbData](Ctx& x) {
        auto& d = getPbData(x, x.cpu.r[0]);
        x.cpu.r[0] = Foundation::dictGet(x, d.itemsDict, "public.utf8-plain-text");
    }, "@@:");
    pb.method("setString:", [getPbData](Ctx& x) {
        auto& d = getPbData(x, x.cpu.r[0]);
        d.changeCount++;
        const id val = x.cpu.r[2];
        if (val) rt(x).send(x.cpu, d.itemsDict, "setObject:forKey:", {val, makeString(x, "public.utf8-plain-text")});
        else rt(x).send(x.cpu, d.itemsDict, "removeObjectForKey:", {makeString(x, "public.utf8-plain-text")});
    }, "v@:@");
    pb.method("strings", [getPbData](Ctx& x) {
        auto& d = getPbData(x, x.cpu.r[0]);
        const id s = Foundation::dictGet(x, d.itemsDict, "public.utf8-plain-text");
        x.cpu.r[0] = s ? Foundation::makeArray(x, {s}) : Foundation::makeArray(x, {});
    }, "@@:");
    pb.method("setStrings:", [getPbData](Ctx& x) {
        auto& d = getPbData(x, x.cpu.r[0]);
        d.changeCount++;
        const auto items = Foundation::arrayItems(x, x.cpu.r[2]);
        if (!items.empty()) rt(x).send(x.cpu, d.itemsDict, "setObject:forKey:", {items[0], makeString(x, "public.utf8-plain-text")});
    }, "v@:@");
    pb.method("URL", [getPbData](Ctx& x) { x.cpu.r[0] = Foundation::dictGet(x, getPbData(x, x.cpu.r[0]).itemsDict, "public.url"); }, "@@:");
    pb.method("setURL:", [getPbData](Ctx& x) {
        auto& d = getPbData(x, x.cpu.r[0]);
        d.changeCount++;
        if (x.cpu.r[2]) rt(x).send(x.cpu, d.itemsDict, "setObject:forKey:", {x.cpu.r[2], makeString(x, "public.url")});
    }, "v@:@");
    pb.method("image", [getPbData](Ctx& x) { x.cpu.r[0] = Foundation::dictGet(x, getPbData(x, x.cpu.r[0]).itemsDict, "public.image"); }, "@@:");
    pb.method("setImage:", [getPbData](Ctx& x) {
        auto& d = getPbData(x, x.cpu.r[0]);
        d.changeCount++;
        if (x.cpu.r[2]) rt(x).send(x.cpu, d.itemsDict, "setObject:forKey:", {x.cpu.r[2], makeString(x, "public.image")});
    }, "v@:@");
    pb.method("items", [getPbData](Ctx& x) {
        auto& d = getPbData(x, x.cpu.r[0]);
        x.cpu.r[0] = Foundation::makeArray(x, {d.itemsDict});
    }, "@@:");
    pb.method("setItems:", [getPbData](Ctx& x) {
        auto& d = getPbData(x, x.cpu.r[0]);
        d.changeCount++;
        const auto arr = Foundation::arrayItems(x, x.cpu.r[2]);
        if (!arr.empty() && Foundation::isDict(x, arr[0])) {
            for (auto& [k, v] : Foundation::dictEntries(x, arr[0]))
                rt(x).send(x.cpu, d.itemsDict, "setObject:forKey:", {v, k});
        }
    }, "v@:@");
    pb.method("valueForPasteboardType:", [getPbData](Ctx& x) {
        auto& d = getPbData(x, x.cpu.r[0]);
        x.cpu.r[0] = x.cpu.r[2] ? Foundation::dictGet(x, d.itemsDict, x.cpu.r[2]) : 0;
    }, "@@:@");
    pb.method("dataForPasteboardType:", [getPbData](Ctx& x) {
        auto& d = getPbData(x, x.cpu.r[0]);
        const id v = x.cpu.r[2] ? Foundation::dictGet(x, d.itemsDict, x.cpu.r[2]) : 0;
        if (!v) { x.cpu.r[0] = 0; return; }
        if (Foundation::isData(x, v)) { x.cpu.r[0] = v; return; }
        if (Foundation::isString(x, v)) {
            const std::string s = utf8(x, v);
            x.cpu.r[0] = Foundation::makeData(x, s.data(), uint32_t(s.size()));
            return;
        }
        x.cpu.r[0] = 0;
    }, "@@:@");
    pb.method("setValue:forPasteboardType:", [getPbData](Ctx& x) {
        auto& d = getPbData(x, x.cpu.r[0]);
        d.changeCount++;
        if (x.cpu.r[2] && x.cpu.r[3]) rt(x).send(x.cpu, d.itemsDict, "setObject:forKey:", {x.cpu.r[2], x.cpu.r[3]});
    }, "v@:@@");
    pb.method("setData:forPasteboardType:", [getPbData](Ctx& x) {
        auto& d = getPbData(x, x.cpu.r[0]);
        d.changeCount++;
        if (x.cpu.r[2] && x.cpu.r[3]) rt(x).send(x.cpu, d.itemsDict, "setObject:forKey:", {x.cpu.r[2], x.cpu.r[3]});
    }, "v@:@@");
    pb.method("pasteboardTypes", [getPbData](Ctx& x) {
        auto& d = getPbData(x, x.cpu.r[0]);
        x.cpu.r[0] = id(rt(x).send(x.cpu, d.itemsDict, "allKeys"));
    }, "@@:");
    pb.method("containsPasteboardTypes:", [getPbData](Ctx& x) {
        auto& d = getPbData(x, x.cpu.r[0]);
        const id types = x.cpu.r[2];
        if (!types) { x.cpu.r[0] = 0; return; }
        const auto want = Foundation::arrayItems(x, types);
        for (id t : want) {
            if (Foundation::dictGet(x, d.itemsDict, t)) { x.cpu.r[0] = 1; return; }
        }
        x.cpu.r[0] = 0;
    }, "c@:@");
    pb.method("numberOfItems", [getPbData](Ctx& x) {
        auto& d = getPbData(x, x.cpu.r[0]);
        const auto entries = Foundation::dictEntries(x, d.itemsDict);
        x.cpu.r[0] = entries.empty() ? 0 : 1;
    }, "i@:");
    for (const char* n : {"UIPasteboardNameGeneral", "UIPasteboardNameFind", "UIPasteboardChangedNotification", "UIPasteboardRemovedNotification",
                          "UIPasteboardTypeListString", "UIPasteboardTypeListURL", "UIPasteboardTypeListImage", "UIPasteboardTypeListColor"}) {
        const uint32_t cell = p.defineDataBlock(std::string("_") + n, 4);
        p.mem().write<uint32_t>(cell, Foundation::constString(p, n));
    }
}

} // namespace Hydrogen::Runtime::UIKit
