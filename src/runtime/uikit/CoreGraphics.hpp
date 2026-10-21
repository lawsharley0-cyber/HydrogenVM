// HydrogenVM — CoreGraphics HLE shared with QuartzCore / UIKit.
#pragma once
#include "runtime/foundation/Foundation.hpp"
#include "runtime/graphics/Graphics.hpp"

namespace Hydrogen::Runtime::CG {

using Foundation::id;
using Foundation::SEL;
using Foundation::Class;
namespace G = Graphics;

// By-value guest structs (32-bit: CGFloat is float).
struct CGPoint { float x, y; static constexpr bool kGuestStruct = true; };
struct CGSize { float width, height; static constexpr bool kGuestStruct = true; };
struct CGRect { CGPoint origin; CGSize size; static constexpr bool kGuestStruct = true; };
struct CGAffineTransform { float a, b, c, d, tx, ty; static constexpr bool kGuestStruct = true; };
struct UIEdgeInsets { float top, left, bottom, right; static constexpr bool kGuestStruct = true; };

inline G::Rect toG(const CGRect& r) { return {r.origin.x, r.origin.y, r.size.width, r.size.height}; }
inline CGRect fromG(const G::Rect& r) { return {{r.x, r.y}, {r.w, r.h}}; }
inline G::Point toG(const CGPoint& p) { return {p.x, p.y}; }
inline CGPoint fromG(const G::Point& p) { return {p.x, p.y}; }
inline G::Affine toG(const CGAffineTransform& t) { return {t.a, t.b, t.c, t.d, t.tx, t.ty}; }
inline CGAffineTransform fromG(const G::Affine& t) { return {t.a, t.b, t.c, t.d, t.tx, t.ty}; }

template <typename T> void writeStruct(Ctx& x, GuestPtr p, const T& v) { std::memcpy(x.mem.host(p), &v, sizeof v); }
template <typename T> T readStruct(Ctx& x, GuestPtr p) { T v; std::memcpy(&v, x.mem.host(p), sizeof v); return v; }

struct GState {
    G::Affine ctm;
    G::Clip clip;
    G::Color fill{0, 0, 0, 1}, stroke{0, 0, 0, 1};
    float alpha = 1, lineWidth = 1, miterLimit = 10, flatness = 0.5f;
    int lineCap = 0, lineJoin = 0;   // butt / miter
    std::vector<float> dash;
    float dashPhase = 0;
    G::BlendMode blend = G::BlendMode::Normal;
    bool antialias = true, smoothImages = true;
    // text
    std::string fontName = "Helvetica";
    float fontSize = 12;
    G::Affine textMatrix;
    float charSpacing = 0;
    int textMode = 0;   // kCGTextFill
    bool subpixelPositionFonts = true, subpixelQuantizeFonts = true;
};

struct ContextData : ObjC::HostObject {
    Process* proc = nullptr;
    G::Surface surface;
    uint32_t guestData = 0;          // pixels live in guest memory at this address
    bool ownsData = false;
    uint32_t bitsPerComponent = 8, bitsPerPixel = 32, bytesPerRow = 0, bitmapInfo = 0;
    id colorSpace = 0;
    bool allowsFontSubpixelPositioning = true, allowsFontSubpixelQuantization = true;
    std::vector<GState> stack{GState{}};
    G::Path path;                    // device space
    G::Point textPos{};              // text space
    GState& gs() { return stack.back(); }
    ~ContextData() override;
};

struct ImageData : ObjC::HostObject {
    G::ImagePtr image;
    uint32_t bitmapInfo = 1;          // kCGImageAlphaPremultipliedLast
    bool isMask = false;
    id colorSpace = 0;
};

// ---- objects
ContextData* context(Ctx& x, id ctx);
id createBitmapContext(Ctx& x, uint32_t data, int w, int h, uint32_t bytesPerRow, uint32_t bitmapInfo, bool gray = false,
                       bool alphaOnly = false);    // +1; data 0 = allocate (zeroed)
G::ImagePtr imageOf(Ctx& x, id cgimage);
id createImage(Ctx& x, G::ImagePtr img);           // +1
G::Color colorOf(Ctx& x, id cgcolor);
id createColor(Ctx& x, G::Color c);                // +1
G::ImagePtr contextImage(Ctx& x, id ctx);          // snapshot of a bitmap context's pixels

// ---- UIKit graphics context stack (per thread)
void pushContext(Ctx& x, id ctx);
void popContext(Ctx& x);
id currentContext(Ctx& x);

// ---- drawing helpers for UIKit
void fillRect(Ctx& x, id ctx, const G::Rect& userRect, G::Color c, G::BlendMode m = G::BlendMode::Normal);
void drawImageInRect(Ctx& x, id ctx, const G::Image& img, const G::Rect& userRect, bool flippedUser /* UIKit y-down */, float alpha = 1,
                     G::BlendMode m = G::BlendMode::Normal);
// Draw UTF-8 text with its top-left at `p` in a y-down (UIKit) user space; returns the drawn size.
G::Size drawTextUIKit(Ctx& x, id ctx, const std::string& utf8, const G::FontDesc& f, G::Point p, G::Color c);
// Set the user->device transform so (0,0) is top-left, y down, in points at `scale`.
void setUIKitCTM(Ctx& x, id ctx, float pointHeight, float scale);

void registerCoreGraphics(Process& p, ObjC::Runtime& r);

} // namespace Hydrogen::Runtime::CG
