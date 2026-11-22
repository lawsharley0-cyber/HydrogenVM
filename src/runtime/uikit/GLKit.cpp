// HydrogenVM — GLKit's view and view controller (iOS 5).
//
// GLKView is a UIView over a CAEAGLLayer that owns its framebuffer: -display makes the
// view's EAGLContext current, binds (and on a size change rebuilds) a framebuffer with
// a colour renderbuffer stored from the layer plus the depth/stencil buffers the
// drawable formats ask for, sets the viewport, calls -drawRect: (by default the
// delegate's glkView:drawInRect:) and presents the colour renderbuffer.
//
// GLKViewController drives it: once its view appears, a CADisplayLink at the preferred
// frame rate calls the delegate's glkViewControllerUpdate: (or the controller's own
// -update) and then -[GLKView display]. With no other delegate the controller is its
// view's delegate, as in GLKit, so a subclass's glkView:drawInRect: draws.
//
// GL calls go through the guest's own GL entry points, so the GPU and software
// renderers both serve them. Not provided: multisampling (the drawable is single-sampled),
// -snapshot (nil), GLKBaseEffect, GLKTextureLoader. GLKMath is inline in the SDK headers.
#include "UIKit.hpp"
#include "runtime/Support.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>

namespace Hydrogen::Runtime::UIKit {
namespace {

constexpr uint32_t kFramebuffer = 0x8D40, kRenderbuffer = 0x8D41, kColor0 = 0x8CE0, kDepthAtt = 0x8D00, kStencilAtt = 0x8D20;
constexpr uint32_t kDepth16 = 0x81A5, kDepth24 = 0x81A6, kDepth24Stencil8 = 0x88F0, kStencil8 = 0x8D48;
constexpr uint32_t kRbWidth = 0x8D42, kRbHeight = 0x8D43;

struct ViewGL : ObjC::HostObject {
    id context = 0;    // retained
    id delegate = 0;   // weak, as in GLKit
    int32_t colorFormat = 0, depthFormat = 0, stencilFormat = 0, multisample = 0;
    uint32_t fbo = 0, color = 0, depth = 0, stencil = 0;
    int width = 0, height = 0;
    bool enableSetNeedsDisplay = true;
};
ViewGL& VG(Ctx& x, id self) { return rt(x).host<ViewGL>(self); }

struct ControllerGL : ObjC::HostObject {
    id link = 0;       // retained CADisplayLink
    id delegate = 0;   // weak
    int32_t preferredFps = 30;
    bool paused = false, pauseOnResign = true, resumeOnActive = true;
    int64_t framesDisplayed = 0;
    double firstResume = -1, lastResume = -1, lastUpdate = -1, lastDraw = -1;
};
ControllerGL& CG_(Ctx& x, id self) { return rt(x).host<ControllerGL>(self); }

double nowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The current context's API picks core (ES 2) or OES (ES 1) framebuffer entry points.
uint32_t gl(Ctx& x, const std::string& name, bool oes, std::initializer_list<uint32_t> args) {
    const auto sym = x.proc.lookupSymbol("_" + name + (oes ? "OES" : ""));
    if (!sym) return 0;
    return uint32_t(x.proc.callGuest(x.cpu, *sym, args));
}
bool usesOES(Ctx& x, id context) { return context && uint32_t(rt(x).send(x.cpu, context, "API")) == 1; }

void deleteDrawable(Ctx& x, id self) {
    ViewGL& v = VG(x, self);
    if (!v.fbo && !v.color) return;
    const bool oes = usesOES(x, v.context);
    const uint32_t tmp = x.proc.heap().allocZeroed(16);
    for (uint32_t* rb : {&v.color, &v.depth, &v.stencil}) {
        if (!*rb) continue;
        x.mem.write<uint32_t>(tmp, *rb);
        gl(x, "glDeleteRenderbuffers", oes, {1, tmp});
        *rb = 0;
    }
    if (v.fbo) { x.mem.write<uint32_t>(tmp, v.fbo); gl(x, "glDeleteFramebuffers", oes, {1, tmp}); v.fbo = 0; }
    x.proc.heap().free(tmp);
    v.width = v.height = 0;
}

// Builds the framebuffer for the layer's current size (bounds x contentScaleFactor).
bool ensureDrawable(Ctx& x, id self) {
    ViewGL& v = VG(x, self);
    if (!v.context) return false;
    auto& r = rt(x);
    const G::Rect b = getRect(x, self, "bounds");
    uint32_t sb = uint32_t(r.send(x.cpu, self, "contentScaleFactor"));
    float scale; std::memcpy(&scale, &sb, 4);
    if (!(scale > 0)) scale = ui(x).scale;
    const int w = std::max(1, int(std::lround(b.w * scale))), h = std::max(1, int(std::lround(b.h * scale)));
    if (v.fbo && v.width == w && v.height == h) return true;
    deleteDrawable(x, self);
    const bool oes = usesOES(x, v.context);
    const uint32_t tmp = x.proc.heap().allocZeroed(16);
    gl(x, "glGenFramebuffers", oes, {1, tmp});
    v.fbo = x.mem.read<uint32_t>(tmp);
    gl(x, "glBindFramebuffer", oes, {kFramebuffer, v.fbo});
    gl(x, "glGenRenderbuffers", oes, {1, tmp});
    v.color = x.mem.read<uint32_t>(tmp);
    gl(x, "glBindRenderbuffer", oes, {kRenderbuffer, v.color});
    const id layer = id(r.send(x.cpu, self, "layer"));
    r.send(x.cpu, v.context, "renderbufferStorage:fromDrawable:", {kRenderbuffer, layer});
    gl(x, "glFramebufferRenderbuffer", oes, {kFramebuffer, kColor0, kRenderbuffer, v.color});
    gl(x, "glGetRenderbufferParameteriv", oes, {kRenderbuffer, kRbWidth, tmp});
    gl(x, "glGetRenderbufferParameteriv", oes, {kRenderbuffer, kRbHeight, tmp + 4});
    v.width = int(x.mem.read<int32_t>(tmp));
    v.height = int(x.mem.read<int32_t>(tmp + 4));
    if (v.width <= 0 || v.height <= 0) { v.width = w; v.height = h; }
    // GLKViewDrawableDepthFormat: 0 none, 1 16-bit, 2 24-bit. With an 8-bit stencil a 24-bit
    // depth shares one packed buffer, as GLKit does.
    const bool packed = v.depthFormat == 2 && v.stencilFormat == 1;
    if (v.depthFormat) {
        gl(x, "glGenRenderbuffers", oes, {1, tmp});
        v.depth = x.mem.read<uint32_t>(tmp);
        gl(x, "glBindRenderbuffer", oes, {kRenderbuffer, v.depth});
        gl(x, "glRenderbufferStorage", oes, {kRenderbuffer, packed ? kDepth24Stencil8 : v.depthFormat == 2 ? kDepth24 : kDepth16,
                                             uint32_t(v.width), uint32_t(v.height)});
        gl(x, "glFramebufferRenderbuffer", oes, {kFramebuffer, kDepthAtt, kRenderbuffer, v.depth});
        if (packed) gl(x, "glFramebufferRenderbuffer", oes, {kFramebuffer, kStencilAtt, kRenderbuffer, v.depth});
    }
    if (v.stencilFormat && !packed) {
        gl(x, "glGenRenderbuffers", oes, {1, tmp});
        v.stencil = x.mem.read<uint32_t>(tmp);
        gl(x, "glBindRenderbuffer", oes, {kRenderbuffer, v.stencil});
        gl(x, "glRenderbufferStorage", oes, {kRenderbuffer, kStencil8, uint32_t(v.width), uint32_t(v.height)});
        gl(x, "glFramebufferRenderbuffer", oes, {kFramebuffer, kStencilAtt, kRenderbuffer, v.stencil});
    }
    gl(x, "glBindRenderbuffer", oes, {kRenderbuffer, v.color});
    x.proc.heap().free(tmp);
    return true;
}

// ---- GLKView
Class gv_layerClass(Ctx& x, id, SEL) { return rt(x).getClass("CAEAGLLayer"); }
void gv_setup(Ctx& x, id self) {
    VG(x, self);
    auto& r = rt(x);
    const float s = ui(x).scale;
    uint32_t bits; std::memcpy(&bits, &s, 4);
    r.send(x.cpu, self, "setContentScaleFactor:", {bits});
    r.send(x.cpu, id(r.send(x.cpu, self, "layer")), "setOpaque:", {1});
}
id gv_initWithFrame(Ctx& x, id self, SEL s, CGRect f) {
    const id me = id(rt(x).sendSuper(x.cpu, self, ui(x).view, s, {fbits(f.origin.x), fbits(f.origin.y), fbits(f.size.width), fbits(f.size.height)}));
    if (me) gv_setup(x, me);
    return me;
}
id gv_initWithFrameContext(Ctx& x, id self, SEL, CGRect f, id context) {
    const id me = gv_initWithFrame(x, self, rt(x).sel("initWithFrame:"), f);
    if (me) rt(x).send(x.cpu, me, "setContext:", {context});
    return me;
}
id gv_initWithCoder(Ctx& x, id self, SEL s, id coder) {
    const id me = id(rt(x).sendSuper(x.cpu, self, ui(x).view, s, {coder}));
    if (me) gv_setup(x, me);
    return me;
}
void gv_dealloc(Ctx& x, id self, SEL s) {
    if (ViewGL* v = rt(x).hostIf<ViewGL>(self)) {
        if (v->context) { deleteDrawable(x, self); rt(x).release(x.cpu, v->context); v->context = 0; }
    }
    rt(x).sendSuper(x.cpu, self, ui(x).view, s, {});
}
id gv_context(Ctx& x, id self, SEL) { return VG(x, self).context; }
void gv_setContext(Ctx& x, id self, SEL, id c) {
    ViewGL& v = VG(x, self);
    if (v.context == c) return;
    if (v.context) {
        rt(x).send(x.cpu, rt(x).getClass("EAGLContext"), "setCurrentContext:", {v.context});
        deleteDrawable(x, self);
        rt(x).release(x.cpu, v.context);
    }
    v.context = c ? rt(x).retain(x.cpu, c) : 0;
}
id gv_delegate(Ctx& x, id self, SEL) { return VG(x, self).delegate; }
void gv_setDelegate(Ctx& x, id self, SEL, id d) { VG(x, self).delegate = d; }
template <int32_t ViewGL::*F> int32_t gv_getFmt(Ctx& x, id self, SEL) { return VG(x, self).*F; }
template <int32_t ViewGL::*F> void gv_setFmt(Ctx& x, id self, SEL, int32_t v) {
    if (VG(x, self).*F == v) return;
    VG(x, self).*F = v;
    if (VG(x, self).context) {
        rt(x).send(x.cpu, rt(x).getClass("EAGLContext"), "setCurrentContext:", {VG(x, self).context});
        deleteDrawable(x, self);   // rebuilt with the new format at the next bind
    }
}
int32_t gv_drawableWidth(Ctx& x, id self, SEL) {
    if (VG(x, self).width) return VG(x, self).width;
    const G::Rect b = getRect(x, self, "bounds");
    return int32_t(std::lround(b.w * ui(x).scale));
}
int32_t gv_drawableHeight(Ctx& x, id self, SEL) {
    if (VG(x, self).height) return VG(x, self).height;
    const G::Rect b = getRect(x, self, "bounds");
    return int32_t(std::lround(b.h * ui(x).scale));
}
void gv_bindDrawable(Ctx& x, id self, SEL) {
    ViewGL& v = VG(x, self);
    if (!ensureDrawable(x, self)) return;
    const bool oes = usesOES(x, v.context);
    gl(x, "glBindFramebuffer", oes, {kFramebuffer, v.fbo});
    gl(x, "glViewport", false, {0, 0, uint32_t(v.width), uint32_t(v.height)});
}
void gv_deleteDrawable(Ctx& x, id self, SEL) { deleteDrawable(x, self); }
void gv_display(Ctx& x, id self, SEL) {
    ViewGL& v = VG(x, self);
    if (!v.context) return;
    auto& r = rt(x);
    r.send(x.cpu, r.getClass("EAGLContext"), "setCurrentContext:", {v.context});
    gv_bindDrawable(x, self, 0);
    if (!v.fbo) return;
    sendRect(x, self, r.sel("drawRect:"), getRect(x, self, "bounds"));
    ViewGL& after = VG(x, self);
    if (!after.color || !after.context) return;
    gl(x, "glBindRenderbuffer", usesOES(x, after.context), {kRenderbuffer, after.color});
    r.send(x.cpu, after.context, "presentRenderbuffer:", {kRenderbuffer});
}
// -drawRect: by default asks the delegate; a GLKView subclass overrides it.
void gv_drawRect(Ctx& x, id self, SEL, CGRect rect) {
    const id d = VG(x, self).delegate;
    auto& r = rt(x);
    if (d && r.respondsTo(d, r.sel("glkView:drawInRect:")))
        r.send(x.cpu, d, "glkView:drawInRect:", {self, fbits(rect.origin.x), fbits(rect.origin.y), fbits(rect.size.width), fbits(rect.size.height)});
}
// The display pass reaches a GLKView through its layer: draw with GL, never into a bitmap.
void gv_displayLayer(Ctx& x, id self, SEL, id) { gv_display(x, self, 0); }
bool gv_enableSetNeedsDisplay(Ctx& x, id self, SEL) { return VG(x, self).enableSetNeedsDisplay; }
void gv_setEnableSetNeedsDisplay(Ctx& x, id self, SEL, bool b) { VG(x, self).enableSetNeedsDisplay = b; }

// ---- GLKViewController
void gvc_loadView(Ctx& x, id self, SEL s) {
    auto& r = rt(x);
    r.sendSuper(x.cpu, self, ui(x).viewController, s, {});   // a nib's view, or a plain UIView
    const id view = id(r.send(x.cpu, self, "view"));
    if (view && r.isKindOf(view, r.getClass("GLKView"))) return;
    // No GLKView from a nib: GLKit makes one with the screen's bounds.
    const G::Rect frame = view ? getRect(x, view, "frame") : G::Rect{0, 0, ui(x).screenW, ui(x).screenH};
    const id gv = id(sendRect(x, id(r.send(x.cpu, r.getClass("GLKView"), r.selAlloc)), r.sel("initWithFrame:"), frame));
    r.send(x.cpu, gv, "setAutoresizingMask:", {2 | 16});
    r.send(x.cpu, self, "setView:", {gv});
    r.release(x.cpu, gv);
}
void gvc_tick(Ctx& x, id self, SEL, id) {
    ControllerGL& c = CG_(x, self);
    if (c.paused) return;
    auto& r = rt(x);
    const double now = nowSeconds();
    if (c.delegate && r.respondsTo(c.delegate, r.sel("glkViewControllerUpdate:")))
        r.send(x.cpu, c.delegate, "glkViewControllerUpdate:", {self});
    else if (r.respondsTo(self, r.sel("update")))
        r.send(x.cpu, self, "update");
    CG_(x, self).lastUpdate = now;
    const id view = id(r.send(x.cpu, self, "view"));
    if (view && r.isKindOf(view, r.getClass("GLKView"))) {
        if (!VG(x, view).delegate) VG(x, view).delegate = self;
        r.send(x.cpu, view, "display");
    }
    ControllerGL& after = CG_(x, self);
    after.lastDraw = now;
    ++after.framesDisplayed;
}
void gvc_start(Ctx& x, id self) {
    ControllerGL& c = CG_(x, self);
    const double now = nowSeconds();
    if (c.firstResume < 0) c.firstResume = now;
    c.lastResume = now;
    if (c.link) return;
    auto& r = rt(x);
    const id link = id(r.send(x.cpu, r.getClass("CADisplayLink"), "displayLinkWithTarget:selector:", {self, r.sel("_hvmGLKTick:")}));
    if (!link) return;
    CG_(x, self).link = r.retain(x.cpu, link);
    const int fps = std::clamp(CG_(x, self).preferredFps, 1, 60);
    r.send(x.cpu, link, "setFrameInterval:", {uint32_t(std::max(1, 60 / fps))});
    const id loop = id(r.send(x.cpu, r.getClass("NSRunLoop"), "mainRunLoop"));
    r.send(x.cpu, link, "addToRunLoop:forMode:", {loop, Foundation::makeString(x, "kCFRunLoopDefaultMode")});
}
void gvc_viewWillAppear(Ctx& x, id self, SEL s, bool animated) {
    rt(x).sendSuper(x.cpu, self, ui(x).viewController, s, {uint32_t(animated)});
    const id view = id(rt(x).send(x.cpu, self, "view"));
    if (view && rt(x).isKindOf(view, rt(x).getClass("GLKView")) && !VG(x, view).delegate) VG(x, view).delegate = self;
    gvc_start(x, self);
}
void gvc_viewDidDisappear(Ctx& x, id self, SEL s, bool animated) {
    rt(x).sendSuper(x.cpu, self, ui(x).viewController, s, {uint32_t(animated)});
    ControllerGL& c = CG_(x, self);
    if (c.link) { const id l = c.link; c.link = 0; rt(x).send(x.cpu, l, "invalidate"); rt(x).release(x.cpu, l); }
}
void gvc_dealloc(Ctx& x, id self, SEL s) {
    if (ControllerGL* c = rt(x).hostIf<ControllerGL>(self); c && c->link) {
        const id l = c->link; c->link = 0;
        rt(x).send(x.cpu, l, "invalidate");
        rt(x).release(x.cpu, l);
    }
    rt(x).sendSuper(x.cpu, self, ui(x).viewController, s, {});
}
id gvc_delegate(Ctx& x, id self, SEL) { return CG_(x, self).delegate; }
void gvc_setDelegate(Ctx& x, id self, SEL, id d) { CG_(x, self).delegate = d; }
int32_t gvc_preferredFps(Ctx& x, id self, SEL) { return CG_(x, self).preferredFps; }
void gvc_setPreferredFps(Ctx& x, id self, SEL, int32_t fps) {
    ControllerGL& c = CG_(x, self);
    c.preferredFps = fps > 0 ? fps : 30;
    if (c.link) rt(x).send(x.cpu, c.link, "setFrameInterval:", {uint32_t(std::max(1, 60 / std::clamp(c.preferredFps, 1, 60)))});
}
// framesPerSecond: what the display can give at the preferred rate (60 divided evenly).
int32_t gvc_fps(Ctx& x, id self, SEL) { return 60 / std::max(1, 60 / std::clamp(CG_(x, self).preferredFps, 1, 60)); }
bool gvc_paused(Ctx& x, id self, SEL) { return CG_(x, self).paused; }
void gvc_setPaused(Ctx& x, id self, SEL, bool p) {
    ControllerGL& c = CG_(x, self);
    if (c.paused && !p) c.lastResume = nowSeconds();
    c.paused = p;
}
bool gvc_pauseOnResign(Ctx& x, id self, SEL) { return CG_(x, self).pauseOnResign; }
void gvc_setPauseOnResign(Ctx& x, id self, SEL, bool b) { CG_(x, self).pauseOnResign = b; }
bool gvc_resumeOnActive(Ctx& x, id self, SEL) { return CG_(x, self).resumeOnActive; }
void gvc_setResumeOnActive(Ctx& x, id self, SEL, bool b) { CG_(x, self).resumeOnActive = b; }
int32_t gvc_framesDisplayed(Ctx& x, id self, SEL) { return int32_t(CG_(x, self).framesDisplayed); }
double since(double t) { return t < 0 ? 0 : nowSeconds() - t; }
double gvc_sinceFirstResume(Ctx& x, id self, SEL) { return since(CG_(x, self).firstResume); }
double gvc_sinceLastResume(Ctx& x, id self, SEL) { return since(CG_(x, self).lastResume); }
double gvc_sinceLastUpdate(Ctx& x, id self, SEL) { return since(CG_(x, self).lastUpdate); }
double gvc_sinceLastDraw(Ctx& x, id self, SEL) { return since(CG_(x, self).lastDraw); }

} // namespace

void registerGLKit(Process& p, ObjC::Runtime& r) {
    const std::string R = "{CGRect={CGPoint=ff}{CGSize=ff}}";
    ObjC::ClassBuilder view = r.defineClass("GLKView", "UIView");
    view.cm<&gv_layerClass>("layerClass", "#@:");
    view.m<&gv_initWithFrame>("initWithFrame:", "@@:" + R)
        .m<&gv_initWithFrameContext>("initWithFrame:context:", "@@:" + R + "@")
        .m<&gv_initWithCoder>("initWithCoder:")
        .m<&gv_dealloc>("dealloc", "v@:")
        .m<&gv_context>("context").m<&gv_setContext>("setContext:", "v@:@")
        .m<&gv_delegate>("delegate").m<&gv_setDelegate>("setDelegate:", "v@:@")
        .m<&gv_getFmt<&ViewGL::colorFormat>>("drawableColorFormat", "i@:").m<&gv_setFmt<&ViewGL::colorFormat>>("setDrawableColorFormat:", "v@:i")
        .m<&gv_getFmt<&ViewGL::depthFormat>>("drawableDepthFormat", "i@:").m<&gv_setFmt<&ViewGL::depthFormat>>("setDrawableDepthFormat:", "v@:i")
        .m<&gv_getFmt<&ViewGL::stencilFormat>>("drawableStencilFormat", "i@:").m<&gv_setFmt<&ViewGL::stencilFormat>>("setDrawableStencilFormat:", "v@:i")
        .m<&gv_getFmt<&ViewGL::multisample>>("drawableMultisample", "i@:").m<&gv_setFmt<&ViewGL::multisample>>("setDrawableMultisample:", "v@:i")
        .m<&gv_drawableWidth>("drawableWidth", "i@:").m<&gv_drawableHeight>("drawableHeight", "i@:")
        .m<&gv_bindDrawable>("bindDrawable", "v@:").m<&gv_deleteDrawable>("deleteDrawable", "v@:")
        .m<&gv_display>("display", "v@:").m<&gv_drawRect>("drawRect:", "v@:" + R)
        .m<&gv_displayLayer>("displayLayer:", "v@:@")
        .m<&gv_enableSetNeedsDisplay>("enableSetNeedsDisplay", "c@:").m<&gv_setEnableSetNeedsDisplay>("setEnableSetNeedsDisplay:", "v@:c");
    supportMethod(p, view, "GLKView", "snapshot", [](Ctx& x) { x.cpu.r[0] = 0; }, SupportStatus::Stub,
                  "nil: the drawable is not read back into a UIImage", "@@:");

    ObjC::ClassBuilder vc = r.defineClass("GLKViewController", "UIViewController");
    vc.m<&gvc_loadView>("loadView", "v@:")
      .m<&gvc_viewWillAppear>("viewWillAppear:", "v@:c").m<&gvc_viewDidDisappear>("viewDidDisappear:", "v@:c")
      .m<&gvc_dealloc>("dealloc", "v@:")
      .m<&gvc_tick>("_hvmGLKTick:", "v@:@")
      .m<&gvc_delegate>("delegate").m<&gvc_setDelegate>("setDelegate:", "v@:@")
      .m<&gvc_preferredFps>("preferredFramesPerSecond", "i@:").m<&gvc_setPreferredFps>("setPreferredFramesPerSecond:", "v@:i")
      .m<&gvc_fps>("framesPerSecond", "i@:")
      .m<&gvc_paused>("isPaused", "c@:").m<&gvc_paused>("paused", "c@:").m<&gvc_setPaused>("setPaused:", "v@:c")
      .m<&gvc_pauseOnResign>("pauseOnWillResignActive", "c@:").m<&gvc_setPauseOnResign>("setPauseOnWillResignActive:", "v@:c")
      .m<&gvc_resumeOnActive>("resumeOnDidBecomeActive", "c@:").m<&gvc_setResumeOnActive>("setResumeOnDidBecomeActive:", "v@:c")
      .m<&gvc_framesDisplayed>("framesDisplayed", "i@:")
      .m<&gvc_sinceFirstResume>("timeSinceFirstResume", "d@:").m<&gvc_sinceLastResume>("timeSinceLastResume", "d@:")
      .m<&gvc_sinceLastUpdate>("timeSinceLastUpdate", "d@:").m<&gvc_sinceLastDraw>("timeSinceLastDraw", "d@:");
}

} // namespace Hydrogen::Runtime::UIKit
