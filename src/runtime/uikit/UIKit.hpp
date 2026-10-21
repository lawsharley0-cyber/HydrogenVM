// HydrogenVM — UIKit / QuartzCore HLE: shared state and helpers.
//
// Model: every UIView owns a CALayer; the layer tree is the source of truth
// for geometry and appearance. Each frame (60 Hz, on the main run loop) the
// compositor redraws dirty layers (drawRect: / drawInContext:) into backing
// images and composites all visible windows into an RGBA framebuffer that is
// handed to the host app (or saved as a screenshot by the test runner).
#pragma once
#include "CoreGraphics.hpp"
#include <array>
#include <deque>

namespace Hydrogen::Runtime::UIKit {

using Foundation::id;
using Foundation::SEL;
using Foundation::Class;
using Foundation::IMP;
using ObjC::rt;
namespace G = Graphics;
using CG::CGPoint;
using CG::CGSize;
using CG::CGRect;
using CG::CGAffineTransform;
using CG::UIEdgeInsets;

struct CATransform3D {
    float m11, m12, m13, m14, m21, m22, m23, m24, m31, m32, m33, m34, m41, m42, m43, m44;
    static constexpr bool kGuestStruct = true;
};
inline CATransform3D identity3D() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }
inline G::Affine affineOf(const CATransform3D& t) { return {t.m11, t.m12, t.m21, t.m22, t.m41, t.m42}; }
inline CATransform3D from2D(const G::Affine& a) {
    CATransform3D t = identity3D();
    t.m11 = a.a; t.m12 = a.b; t.m21 = a.c; t.m22 = a.d; t.m41 = a.tx; t.m42 = a.ty;
    return t;
}

enum Gravity { kGravityResize, kGravityAspect, kGravityAspectFill, kGravityCenter, kGravityTop, kGravityBottom, kGravityLeft, kGravityRight,
               kGravityTopLeft, kGravityTopRight, kGravityBottomLeft, kGravityBottomRight };

struct LayerData : ObjC::HostObject {
    G::Rect bounds;
    G::Point position;
    G::Point anchor{0.5f, 0.5f};
    float zPosition = 0;
    CATransform3D transform = identity3D();
    CATransform3D sublayerTransform = identity3D();
    bool hidden = false, masksToBounds = false, opaque = false, needsDisplay = false, needsDisplayOnBoundsChange = false;
    bool geometryFlipped = false, doubleSided = true;
    float opacity = 1, cornerRadius = 0, borderWidth = 0, contentsScale = 1;
    G::Color background{0, 0, 0, 0}, border{0, 0, 0, 1};
    G::ImagePtr backgroundPattern;
    id contents = 0;                 // retained (CGImage)
    G::Rect contentsRect{0, 0, 1, 1}, contentsCenter{0, 0, 1, 1};
    int gravity = kGravityResize;
    bool magnifyNearest = false;
    id delegate = 0;                 // not retained
    id superlayer = 0;               // not retained
    id mask = 0;                     // retained (ignored when compositing, for now)
    std::vector<id> sublayers;       // retained
    G::ImagePtr backing;             // result of -display
    id name = 0;                     // retained
    std::function<G::ImagePtr()> externalContents;   // CAEAGLLayer etc.: provides the current image
};

struct ViewData : ObjC::HostObject {
    id layer = 0;                    // retained
    id superview = 0;                // not retained
    std::vector<id> subviews;        // retained
    id viewController = 0;           // not retained: VC whose view this is
    id backgroundColor = 0;          // retained UIColor
    int32_t tag = 0;
    bool userInteraction = true, multipleTouch = false, exclusiveTouch = false, autoresizesSubviews = true, needsLayout = true;
    bool clearsContextBeforeDrawing = true;
    uint32_t autoresizingMask = 0;
    int32_t contentMode = 0;
    std::vector<id> gestureRecognizers;   // retained
    // window only
    bool isWindow = false, isKey = false, visible = false;
    float windowLevel = 0;
    id rootViewController = 0;       // retained
};

struct VCData : ObjC::HostObject {
    id view = 0;                     // retained
    id nibName = 0, nibBundle = 0, title = 0;   // retained
    id parent = 0, presented = 0, presenting = 0;   // presented: retained
    std::vector<id> children;        // retained
    id navigationItem = 0, tabBarItem = 0;     // retained
    bool appeared = false, wantsFullScreen = false;
    int32_t modalStyle = 0;
};

// Per-process UIKit state.
struct UIState {
    explicit UIState(Process& p) : proc(p) {}
    Process& proc;
    // classes
    Class responder = 0, view = 0, window = 0, viewController = 0, application = 0, screen = 0, device = 0, layer = 0,
          color = 0, font = 0, image = 0, touch = 0, event = 0, label = 0, imageView = 0, control = 0, button = 0,
          displayLink = 0, nib = 0, nibDecoder = 0, proxy = 0, outletConn = 0, eventConn = 0, gesture = 0;
    id app = 0, appDelegate = 0, mainScreen = 0, currentDevice = 0;
    std::vector<id> windows;         // not retained (windows retain themselves while visible)
    id keyWindow = 0;
    // screen (points)
    float screenW = 320, screenH = 480, scale = 2;
    bool statusBarHidden = false;
    int32_t statusBarOrientation = 1;   // UIInterfaceOrientationPortrait
    int32_t initialOrientation = 1;
    // framebuffer
    G::Image framebuffer;
    double lastFrame = 0;
    uint64_t frames = 0;
    bool needsComposite = true;
    // display links
    struct Link { id obj; id target; SEL sel; int interval; bool paused; int counter; };
    std::vector<Link> links;
    // touches
    struct TouchRec { id touch; id view; uint32_t hostId; };
    std::vector<TouchRec> activeTouches;
    std::mutex inputMu;
    struct HostTouch { int phase; float x, y; uint32_t id; double time; };
    std::deque<HostTouch> pendingTouches;
    // accelerometer
    float accel[3] = {0, 0, -1};
    id firstResponder = 0;
    std::unordered_map<std::string, id> imageCache;   // imageNamed: (retained)
    id ignoringEvents = 0;
    int ignoreCount = 0;
};
UIState& ui(Process& p);
inline UIState& ui(Ctx& x) { return ui(x.proc); }

// ---- layers (QuartzCore.cpp)
LayerData& layerData(Ctx& x, id layer);
id newLayer(Ctx& x, Class cls = 0);                           // +1
void layerAddSublayer(Ctx& x, id parent, id child, long index = -1);
void layerRemoveFromSuperlayer(Ctx& x, id layer);
G::Rect layerFrame(const LayerData& l);
void layerSetFrame(Ctx& x, id layer, const G::Rect& f);
// layer-space → superlayer-space transform
G::Affine layerToSuper(const LayerData& l);
void setNeedsComposite(Ctx& x);
// Draw everything; returns true if the framebuffer changed.
bool compositeFrame(Ctx& x);
void runFrame(Ctx& x, double now);                              // display links + layout + display + composite + present

// ---- views (UIView.cpp)
ViewData& viewData(Ctx& x, id view);
id viewLayer(Ctx& x, id view);
id viewWindow(Ctx& x, id view);
G::Affine viewToWindow(Ctx& x, id view);                         // view bounds space → window space
G::Point convertPoint(Ctx& x, G::Point p, id from, id to);       // nil = window (or screen for windows)
id hitTest(Ctx& x, id view, G::Point p, id event);
void layoutIfNeeded(Ctx& x, id view);
void layoutPass(Ctx& x);                                         // all windows
void viewDidMoveToWindowRecursive(Ctx& x, id view, id window);
id viewControllerOfView(Ctx& x, id view);

// ---- colours / fonts / images (Graphics.cpp)
G::Color uiColor(Ctx& x, id color);
G::ImagePtr uiColorPattern(Ctx& x, id color);                    // colorWithPatternImage:
float uiColorPatternScale(Ctx& x, id color);
id makeUIColor(Ctx& x, G::Color c);                              // autoreleased
G::FontDesc uiFont(Ctx& x, id font);
id makeUIFont(Ctx& x, const G::FontDesc& f);                     // autoreleased
id systemFont(Ctx& x, float size, bool bold = false);
struct ImgData : ObjC::HostObject {
    G::ImagePtr image;
    float scale = 1;
    int32_t orientation = 0;
    id cgImage = 0;                  // retained, lazily created
    UIEdgeInsets caps{0, 0, 0, 0};
    bool resizable = false;
    int32_t renderingMode = 0;
    std::vector<id> frames;          // animated images (retained)
    double duration = 0;
};
ImgData* uiImage(Ctx& x, id img);
id makeUIImage(Ctx& x, G::ImagePtr img, float scale);            // autoreleased
id imageCGImage(Ctx& x, id uiimage);
G::Size imageSize(Ctx& x, id uiimage);                           // points
void drawUIImage(Ctx& x, id ctx, id uiimage, const G::Rect& r, float alpha, G::BlendMode m = G::BlendMode::Normal);
std::string textOf(Ctx& x, id str);
G::Size measureText(Ctx& x, const std::string& s, const G::FontDesc& f, float maxWidth, int maxLines, int lineBreak);
G::Size drawText(Ctx& x, id ctx, const std::string& s, const G::FontDesc& f, const G::Rect& r, G::Color c, int lineBreak, int alignment, int maxLines);

// ---- events (UIApplication.cpp)
void deliverPendingInput(Ctx& x);

// ---- message helpers
uint64_t sendRect(Ctx& x, id self, SEL s, const G::Rect& r);
uint64_t sendPoint(Ctx& x, id self, SEL s, const G::Point& p);
G::Rect getRect(Ctx& x, id self, const char* sel);               // calls a CGRect-returning method (stret)
G::Size getSize(Ctx& x, id self, SEL s, std::initializer_list<uint32_t> args = {});
inline uint32_t fbits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }
inline float bitsf(uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; }
bool sendBool(Ctx& x, id self, const char* sel, std::initializer_list<uint32_t> args = {});

void registerQuartzCore(Process& p, ObjC::Runtime& r);
void registerUIGraphics(Process& p, ObjC::Runtime& r);
void registerUIView(Process& p, ObjC::Runtime& r);
void registerUIControls(Process& p, ObjC::Runtime& r);
void registerUIApplication(Process& p, ObjC::Runtime& r);
void registerNib(Process& p, ObjC::Runtime& r);
void registerGestures(Process& p, ObjC::Runtime& r);
void registerPlatformStubs(Process& p, ObjC::Runtime& r);
void registerOpenGLES(Process& p, ObjC::Runtime& r);
void registerUIWidgets(Process& p, ObjC::Runtime& r);
void registerGameKit(Process& p, ObjC::Runtime& r);
void registerMiscFrameworks(Process& p, ObjC::Runtime& r);

// NIB loading (Nib.cpp): returns the top-level objects (autoreleased array) or 0.
id loadNib(Ctx& x, const std::string& name, id bundle, id owner, id externalObjects);
bool nibExists(Ctx& x, const std::string& name);

} // namespace Hydrogen::Runtime::UIKit
