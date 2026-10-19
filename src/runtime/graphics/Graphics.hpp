// HydrogenVM — portable 2D graphics core used by the CoreGraphics, QuartzCore
// and UIKit HLE: pixel surfaces in guest or host memory, an anti-aliased
// polygon rasterizer, image drawing with affine transforms, clipping, image
// decoding (PNG incl. Apple's CgBI variant; other formats via the host
// platform), PNG encoding (screenshots) and text rendering through a
// host-provided font backend (with a built-in bitmap fallback).
#pragma once
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Hydrogen::Graphics {

struct Point { float x = 0, y = 0; };
struct Size { float w = 0, h = 0; };
struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    float maxX() const { return x + w; }
    float maxY() const { return y + h; }
    bool empty() const { return !(w > 0) || !(h > 0); }
    Rect standardized() const { Rect r = *this; if (r.w < 0) { r.x += r.w; r.w = -r.w; } if (r.h < 0) { r.y += r.h; r.h = -r.h; } return r; }
    Rect intersect(const Rect& o) const {
        const Rect a = standardized(), b = o.standardized();
        const float x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y), x1 = std::min(a.maxX(), b.maxX()), y1 = std::min(a.maxY(), b.maxY());
        if (x1 <= x0 || y1 <= y0) return {};
        return {x0, y0, x1 - x0, y1 - y0};
    }
    Rect unite(const Rect& o) const {
        if (empty()) return o;
        if (o.empty()) return *this;
        const Rect a = standardized(), b = o.standardized();
        const float x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y), x1 = std::max(a.maxX(), b.maxX()), y1 = std::max(a.maxY(), b.maxY());
        return {x0, y0, x1 - x0, y1 - y0};
    }
    bool contains(Point p) const { const Rect a = standardized(); return p.x >= a.x && p.x < a.maxX() && p.y >= a.y && p.y < a.maxY(); }
};

// Same layout and semantics as CGAffineTransform: x' = a*x + c*y + tx, y' = b*x + d*y + ty.
struct Affine {
    float a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
    static Affine translate(float x, float y) { return {1, 0, 0, 1, x, y}; }
    static Affine scale(float sx, float sy) { return {sx, 0, 0, sy, 0, 0}; }
    static Affine rotate(float r) { const float s = std::sin(r), c = std::cos(r); return {c, s, -s, c, 0, 0}; }
    Point apply(Point p) const { return {a * p.x + c * p.y + tx, b * p.x + d * p.y + ty}; }
    Point applyVector(Point p) const { return {a * p.x + c * p.y, b * p.x + d * p.y}; }
    // this then o (CGAffineTransformConcat(this, o)).
    Affine then(const Affine& o) const {
        return {a * o.a + b * o.c, a * o.b + b * o.d, c * o.a + d * o.c, c * o.b + d * o.d, tx * o.a + ty * o.c + o.tx, tx * o.b + ty * o.d + o.ty};
    }
    float det() const { return a * d - b * c; }
    Affine inverted() const {
        const float k = det();
        if (k == 0) return *this;
        const float ia = d / k, ib = -b / k, ic = -c / k, id = a / k;
        return {ia, ib, ic, id, -(ia * tx + ic * ty), -(ib * tx + id * ty)};
    }
    bool isIdentity() const { return a == 1 && b == 0 && c == 0 && d == 1 && tx == 0 && ty == 0; }
    bool axisAligned() const { return b == 0 && c == 0; }
    Rect applyRect(const Rect& r) const;
};

struct Color { float r = 0, g = 0, b = 0, a = 0; };   // straight (non-premultiplied) components 0..1

// Pixel layout: 4 bytes per pixel, channel byte offsets given explicitly,
// premultiplied alpha (or no alpha). A8 is a single alpha byte per pixel.
enum class PixelFormat { RGBA, BGRA, ARGB, ABGR, A8, Gray8 };
struct Surface {
    uint8_t* data = nullptr;
    int width = 0, height = 0;
    int stride = 0;              // bytes per row
    PixelFormat format = PixelFormat::RGBA;
    bool hasAlpha = true;        // false: alpha byte is ignored ("skip") and treated as opaque
    int bytesPerPixel() const { return format == PixelFormat::A8 || format == PixelFormat::Gray8 ? 1 : 4; }
};
// Byte offsets of R, G, B, A within a pixel.
void channelOffsets(PixelFormat f, int& r, int& g, int& b, int& a);

// Owned premultiplied RGBA image.
struct Image {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;   // premultiplied, width*height*4
    bool opaque = false;
    Surface surface() { return {rgba.data(), width, height, width * 4, PixelFormat::RGBA, true}; }
};
using ImagePtr = std::shared_ptr<Image>;

// Coverage mask in device pixels (clip). Empty mask with `rect` only = rectangular clip.
struct Clip {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;          // device bounds (exclusive max)
    std::shared_ptr<std::vector<uint8_t>> mask;  // (x1-x0)*(y1-y0) coverage, or null for a plain rect
    uint8_t at(int x, int y) const {
        if (x < x0 || y < y0 || x >= x1 || y >= y1) return 0;
        return mask ? (*mask)[size_t(y - y0) * size_t(x1 - x0) + size_t(x - x0)] : 255;
    }
};

enum class BlendMode { Normal, Copy, Clear, Multiply, Screen, Overlay, Darken, Lighten, PlusLighter, SourceIn, DestinationOut, DestinationIn };

// A path: closed or open subpaths of points (curves are flattened on the way in).
struct Path {
    std::vector<std::vector<Point>> subpaths;
    std::vector<bool> closed;
    Point current{}, start{};
    bool hasCurrent = false;
    void moveTo(Point p) { subpaths.push_back({p}); closed.push_back(false); current = start = p; hasCurrent = true; }
    void lineTo(Point p) { if (!hasCurrent) { moveTo(p); return; } subpaths.back().push_back(p); current = p; }
    void quadTo(Point c, Point p);
    void cubicTo(Point c1, Point c2, Point p);
    void arc(Point center, float radius, float a0, float a1, bool clockwise);   // CG semantics (y-up angles in user space)
    void arcTo(Point p1, Point p2, float radius);
    void close() { if (hasCurrent && !subpaths.empty()) { closed.back() = true; current = start; } }
    void addRect(const Rect& r) { moveTo({r.x, r.y}); lineTo({r.maxX(), r.y}); lineTo({r.maxX(), r.maxY()}); lineTo({r.x, r.maxY()}); close(); }
    void addEllipse(const Rect& r);
    void addRoundedRect(const Rect& r, float rx, float ry);
    bool empty() const { return subpaths.empty(); }
    Rect bounds() const;
    Path transformed(const Affine& t) const;
    bool contains(Point p, bool evenOdd) const;
};

// --------------------------------------------------------------- raster ---

// Rasterize a filled path (already in device space) into a coverage buffer
// over `bounds` (device, integer). Anti-aliased (4 sub-scanlines, exact x coverage).
void rasterize(const Path& devicePath, bool evenOdd, int bx0, int by0, int bx1, int by1, std::vector<uint8_t>& coverage);
// Stroke outline as a fillable polygon set (device space input, nonzero).
Path strokeToPath(const Path& p, float width, int cap, int join, float miterLimit, const std::vector<float>& dash, float dashPhase);

// Intersect a clip with a device-space path (rectangles on the pixel grid stay mask-free).
Clip intersectClip(const Clip& c, const Path& devicePath, bool evenOdd);
// Fill device-space path with a solid colour.
void fillPath(Surface& dst, const Path& devicePath, bool evenOdd, Color c, const Clip& clip, BlendMode mode = BlendMode::Normal, float alpha = 1);
void fillRect(Surface& dst, const Rect& deviceRect, Color c, const Clip& clip, BlendMode mode = BlendMode::Normal, float alpha = 1);
// Draw `img` so that its unit square maps through `imageToDevice` (image pixel space (0..w,0..h) → device).
void drawImage(Surface& dst, const Image& img, const Affine& imageToDevice, const Clip& clip, float alpha, bool smooth,
               BlendMode mode = BlendMode::Normal, const Color* tint = nullptr /* use image alpha as a mask filled with tint */);
// Composite a whole surface (premultiplied RGBA source) with a transform.
void clear(Surface& dst, Color c);
// Read back a surface as premultiplied RGBA.
ImagePtr snapshot(const Surface& s);

// ---------------------------------------------------------------- images ---

// PNG (all colour types/depths, interlaced, CgBI), plus whatever the platform
// decoder supports (JPEG, GIF, … — see setPlatformImageDecoder).
ImagePtr decodeImage(const uint8_t* data, size_t n, std::string* error = nullptr);
ImagePtr decodePng(const uint8_t* data, size_t n, std::string* error = nullptr);
std::vector<uint8_t> encodePng(const Image& img);
void setPlatformImageDecoder(std::function<ImagePtr(const uint8_t*, size_t)> fn);

// ------------------------------------------------------------------ text ---

struct FontDesc {
    std::string name;            // PostScript-ish name ("Helvetica-Bold", "Marker Felt", ...)
    float size = 17;
    bool bold = false, italic = false;
};
struct TextLayout {
    float width = 0, height = 0, ascent = 0, descent = 0, lineHeight = 0;
};
// Host text backend: measure and draw UTF-8 text. `draw` renders into an A8
// coverage image (origin top-left, baseline at `ascent`).
struct TextBackend {
    virtual ~TextBackend() = default;
    virtual TextLayout measure(const std::string& utf8, const FontDesc& f) = 0;
    virtual std::shared_ptr<Image> render(const std::string& utf8, const FontDesc& f, float scale, TextLayout* layout) = 0;   // RGBA mask (alpha = coverage)
};
void setTextBackend(std::shared_ptr<TextBackend> backend);
TextBackend& textBackend();   // the host's, or the built-in bitmap font

// Line wrapping helper (word wrap by measuring with the backend).
std::vector<std::string> wrapLines(const std::string& utf8, const FontDesc& f, float maxWidth, int maxLines, bool truncateTail);

} // namespace Hydrogen::Graphics
