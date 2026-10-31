// HydrogenVM — CoreGraphics HLE: geometry, affine transforms, colour spaces,
// colours, data providers, images, paths, gradients and bitmap contexts
// (drawing into guest-visible pixel memory with the software rasterizer).
//
// CG objects are Objective-C instances of private host classes so CFRetain /
// CFRelease / CFGetTypeID work on them like on iOS.
#include "CoreGraphics.hpp"
#include "runtime/graphics/TrueType.hpp"
#include <cmath>
#include <fstream>
#include <limits>

namespace Hydrogen::Runtime::CG {

namespace {

using ObjC::rt;

struct CGState {
    explicit CGState(Process&) {}
    Class context = 0, image = 0, colorSpace = 0, color = 0, provider = 0, path = 0, gradient = 0, font = 0, ctfont = 0, descriptor = 0, text = 0;
    id rgbSpace = 0, graySpace = 0;
};
CGState& S(Ctx& x) { return x.proc.service<CGState>(); }

thread_local std::vector<id> t_contextStack;

constexpr float kPi = 3.14159265358979f;

id newObject(Ctx& x, Class cls, std::unique_ptr<ObjC::HostObject> h) {
    const id o = rt(x).createInstance(cls);
    rt(x).setHost(o, std::move(h));
    return o;
}
template <typename T> T* hd(Ctx& x, id o) { return o ? rt(x).hostIf<T>(o) : nullptr; }
void obj_dealloc(Ctx& x, id self, SEL) { rt(x).disposeInstance(x.cpu, self); }
uint32_t cf_retain(Ctx& x, id o) { return o ? rt(x).retain(x.cpu, o) : 0; }
void cf_release(Ctx& x, id o) { if (o) rt(x).release(x.cpu, o); }

// ============================================================== geometry ===

constexpr float kInf = std::numeric_limits<float>::infinity();
CGRect nullRect() { return {{kInf, kInf}, {0, 0}}; }
bool isNull(const CGRect& r) { return std::isinf(r.origin.x) || std::isinf(r.origin.y); }
CGRect standardize(CGRect r) {
    if (r.size.width < 0) { r.origin.x += r.size.width; r.size.width = -r.size.width; }
    if (r.size.height < 0) { r.origin.y += r.size.height; r.size.height = -r.size.height; }
    return r;
}

float r_minX(Ctx&, CGRect r) { r = standardize(r); return r.origin.x; }
float r_minY(Ctx&, CGRect r) { r = standardize(r); return r.origin.y; }
float r_maxX(Ctx&, CGRect r) { r = standardize(r); return r.origin.x + r.size.width; }
float r_maxY(Ctx&, CGRect r) { r = standardize(r); return r.origin.y + r.size.height; }
float r_midX(Ctx&, CGRect r) { r = standardize(r); return r.origin.x + r.size.width / 2; }
float r_midY(Ctx&, CGRect r) { r = standardize(r); return r.origin.y + r.size.height / 2; }
float r_width(Ctx&, CGRect r) { return std::fabs(r.size.width); }
float r_height(Ctx&, CGRect r) { return std::fabs(r.size.height); }
bool r_isEmpty(Ctx&, CGRect r) { return isNull(r) || r.size.width == 0 || r.size.height == 0; }
bool r_isNull(Ctx&, CGRect r) { return isNull(r); }
bool r_isInfinite(Ctx&, CGRect r) { return r.size.width >= 1e30f && r.size.height >= 1e30f; }
bool r_containsPoint(Ctx&, CGRect r, CGPoint p) {
    if (isNull(r)) return false;
    r = standardize(r);
    return p.x >= r.origin.x && p.x < r.origin.x + r.size.width && p.y >= r.origin.y && p.y < r.origin.y + r.size.height;
}
bool r_containsRect(Ctx&, CGRect a, CGRect b) {
    if (isNull(a) || isNull(b)) return false;
    a = standardize(a); b = standardize(b);
    return b.origin.x >= a.origin.x && b.origin.y >= a.origin.y && b.origin.x + b.size.width <= a.origin.x + a.size.width &&
           b.origin.y + b.size.height <= a.origin.y + a.size.height;
}
bool r_intersects(Ctx&, CGRect a, CGRect b) {
    if (isNull(a) || isNull(b)) return false;
    return !toG(a).intersect(toG(b)).empty();
}
bool r_equal(Ctx&, CGRect a, CGRect b) {
    a = standardize(a); b = standardize(b);
    return a.origin.x == b.origin.x && a.origin.y == b.origin.y && a.size.width == b.size.width && a.size.height == b.size.height;
}
bool p_equal(Ctx&, CGPoint a, CGPoint b) { return a.x == b.x && a.y == b.y; }
bool s_equal(Ctx&, CGSize a, CGSize b) { return a.width == b.width && a.height == b.height; }
void r_intersection(Ctx& x, GuestPtr ret, CGRect a, CGRect b) {
    if (isNull(a) || isNull(b)) { writeStruct(x, ret, nullRect()); return; }
    const G::Rect r = toG(a).intersect(toG(b));
    writeStruct(x, ret, r.empty() ? nullRect() : fromG(r));
}
void r_union(Ctx& x, GuestPtr ret, CGRect a, CGRect b) {
    if (isNull(a)) { writeStruct(x, ret, b); return; }
    if (isNull(b)) { writeStruct(x, ret, a); return; }
    writeStruct(x, ret, fromG(toG(standardize(a)).unite(toG(standardize(b)))));
}
void r_inset(Ctx& x, GuestPtr ret, CGRect r, float dx, float dy) {
    if (isNull(r)) { writeStruct(x, ret, r); return; }
    r = standardize(r);
    r.origin.x += dx; r.origin.y += dy; r.size.width -= 2 * dx; r.size.height -= 2 * dy;
    writeStruct(x, ret, r.size.width < 0 || r.size.height < 0 ? nullRect() : r);
}
void r_offset(Ctx& x, GuestPtr ret, CGRect r, float dx, float dy) {
    if (!isNull(r)) { r.origin.x += dx; r.origin.y += dy; }
    writeStruct(x, ret, r);
}
void r_integral(Ctx& x, GuestPtr ret, CGRect r) {
    if (isNull(r)) { writeStruct(x, ret, r); return; }
    r = standardize(r);
    const float x0 = std::floor(r.origin.x), y0 = std::floor(r.origin.y);
    const float x1 = std::ceil(r.origin.x + r.size.width), y1 = std::ceil(r.origin.y + r.size.height);
    writeStruct(x, ret, CGRect{{x0, y0}, {x1 - x0, y1 - y0}});
}
void r_standardize(Ctx& x, GuestPtr ret, CGRect r) { writeStruct(x, ret, standardize(r)); }
void r_divide(Ctx& x, CGRect r, GuestPtr slice, GuestPtr rem, float amount, uint32_t edge) {
    r = standardize(r);
    CGRect s = r, m = r;
    switch (edge) {
        case 0: amount = std::min(amount, r.size.width); s.size.width = amount; m.origin.x += amount; m.size.width -= amount; break;   // MinX
        case 1: amount = std::min(amount, r.size.height); s.size.height = amount; m.origin.y += amount; m.size.height -= amount; break; // MinY
        case 2: amount = std::min(amount, r.size.width); s.origin.x = r.origin.x + r.size.width - amount; s.size.width = amount; m.size.width -= amount; break;
        default: amount = std::min(amount, r.size.height); s.origin.y = r.origin.y + r.size.height - amount; s.size.height = amount; m.size.height -= amount; break;
    }
    if (slice) writeStruct(x, slice, s);
    if (rem) writeStruct(x, rem, m);
}
void p_apply(Ctx& x, GuestPtr ret, CGPoint p, CGAffineTransform t) { writeStruct(x, ret, fromG(toG(t).apply(toG(p)))); }
void s_apply(Ctx& x, GuestPtr ret, CGSize s, CGAffineTransform t) {
    const G::Point v = toG(t).applyVector({s.width, s.height});
    writeStruct(x, ret, CGSize{v.x, v.y});
}
void r_apply(Ctx& x, GuestPtr ret, CGRect r, CGAffineTransform t) {
    if (isNull(r)) { writeStruct(x, ret, r); return; }
    writeStruct(x, ret, fromG(toG(t).applyRect(toG(standardize(r)))));
}

void t_make(Ctx& x, GuestPtr ret, float a, float b, float c, float d, float tx, float ty) { writeStruct(x, ret, CGAffineTransform{a, b, c, d, tx, ty}); }
void t_makeTranslation(Ctx& x, GuestPtr ret, float tx, float ty) { writeStruct(x, ret, fromG(G::Affine::translate(tx, ty))); }
void t_makeScale(Ctx& x, GuestPtr ret, float sx, float sy) { writeStruct(x, ret, fromG(G::Affine::scale(sx, sy))); }
void t_makeRotation(Ctx& x, GuestPtr ret, float a) { writeStruct(x, ret, fromG(G::Affine::rotate(a))); }
void t_translate(Ctx& x, GuestPtr ret, CGAffineTransform t, float tx, float ty) { writeStruct(x, ret, fromG(G::Affine::translate(tx, ty).then(toG(t)))); }
void t_scale(Ctx& x, GuestPtr ret, CGAffineTransform t, float sx, float sy) { writeStruct(x, ret, fromG(G::Affine::scale(sx, sy).then(toG(t)))); }
void t_rotate(Ctx& x, GuestPtr ret, CGAffineTransform t, float a) { writeStruct(x, ret, fromG(G::Affine::rotate(a).then(toG(t)))); }
void t_concat(Ctx& x, GuestPtr ret, CGAffineTransform a, CGAffineTransform b) { writeStruct(x, ret, fromG(toG(a).then(toG(b)))); }
void t_invert(Ctx& x, GuestPtr ret, CGAffineTransform t) { writeStruct(x, ret, fromG(toG(t).inverted())); }
bool t_isIdentity(Ctx&, CGAffineTransform t) { return toG(t).isIdentity(); }
bool t_equal(Ctx&, CGAffineTransform a, CGAffineTransform b) { return std::memcmp(&a, &b, sizeof a) == 0; }

// ========================================================== colour spaces ===

struct SpaceData : ObjC::HostObject { int model = 1; };   // 0 gray, 1 RGB
id cs_rgb(Ctx& x) {
    auto& s = S(x);
    if (!s.rgbSpace) { auto h = std::make_unique<SpaceData>(); h->model = 1; s.rgbSpace = newObject(x, s.colorSpace, std::move(h)); rt(x).makeImmortal(s.rgbSpace); }
    return s.rgbSpace;
}
id cs_gray(Ctx& x) {
    auto& s = S(x);
    if (!s.graySpace) { auto h = std::make_unique<SpaceData>(); h->model = 0; s.graySpace = newObject(x, s.colorSpace, std::move(h)); rt(x).makeImmortal(s.graySpace); }
    return s.graySpace;
}
int spaceModel(Ctx& x, id cs) { SpaceData* d = hd<SpaceData>(x, cs); return d ? d->model : 1; }
uint32_t cs_createWithName(Ctx& x, id name) {
    const std::string n = name ? Foundation::utf8(x, name) : "";
    return n.find("Gray") != std::string::npos ? cs_gray(x) : cs_rgb(x);
}
int32_t cs_components(Ctx& x, id cs) { return spaceModel(x, cs) == 0 ? 1 : 3; }
int32_t cs_model(Ctx& x, id cs) { return spaceModel(x, cs); }

// ================================================================ colours ===

struct ColorData : ObjC::HostObject {
    Process* proc = nullptr;
    G::Color c;
    id space = 0;
    uint32_t components = 0;   // guest float[4] (or float[2] for gray)
    ~ColorData() override { if (components && proc) proc->heap().free(components); }
};
id makeColor(Ctx& x, G::Color c, id space) {
    auto h = std::make_unique<ColorData>();
    h->proc = &x.proc;
    h->c = c;
    h->space = space ? space : cs_rgb(x);
    const bool gray = spaceModel(x, h->space) == 0;
    h->components = x.proc.heap().alloc(16);
    if (gray) { x.mem.write<float>(h->components, c.r); x.mem.write<float>(h->components + 4, c.a); }
    else { x.mem.write<float>(h->components, c.r); x.mem.write<float>(h->components + 4, c.g); x.mem.write<float>(h->components + 8, c.b); x.mem.write<float>(h->components + 12, c.a); }
    return newObject(x, S(x).color, std::move(h));
}
uint32_t col_create(Ctx& x, id space, GuestPtr comps) {
    if (!comps) return 0;
    if (spaceModel(x, space) == 0) { const float v = x.mem.read<float>(comps); return makeColor(x, {v, v, v, x.mem.read<float>(comps + 4)}, space); }
    return makeColor(x, {x.mem.read<float>(comps), x.mem.read<float>(comps + 4), x.mem.read<float>(comps + 8), x.mem.read<float>(comps + 12)}, space);
}
uint32_t col_createGenericRGB(Ctx& x, float r, float g, float b, float a) { return makeColor(x, {r, g, b, a}, cs_rgb(x)); }
uint32_t col_createGenericGray(Ctx& x, float v, float a) { return makeColor(x, {v, v, v, a}, cs_gray(x)); }
uint32_t col_copyWithAlpha(Ctx& x, id c, float a) {
    ColorData* d = hd<ColorData>(x, c);
    if (!d) return 0;
    G::Color k = d->c;
    k.a = a;
    return makeColor(x, k, d->space);
}
uint32_t col_copy(Ctx& x, id c) { ColorData* d = hd<ColorData>(x, c); return d ? makeColor(x, d->c, d->space) : 0; }
uint32_t col_components(Ctx& x, id c) { ColorData* d = hd<ColorData>(x, c); return d ? d->components : 0; }
int32_t col_numComponents(Ctx& x, id c) { ColorData* d = hd<ColorData>(x, c); return d && spaceModel(x, d->space) == 0 ? 2 : 4; }
float col_alpha(Ctx& x, id c) { ColorData* d = hd<ColorData>(x, c); return d ? d->c.a : 1; }
uint32_t col_space(Ctx& x, id c) { ColorData* d = hd<ColorData>(x, c); return d ? d->space : 0; }
bool col_equal(Ctx& x, id a, id b) {
    if (a == b) return true;
    ColorData *p = hd<ColorData>(x, a), *q = hd<ColorData>(x, b);
    return p && q && p->c.r == q->c.r && p->c.g == q->c.g && p->c.b == q->c.b && p->c.a == q->c.a;
}
uint32_t col_getConstant(Ctx& x, id name) {
    const std::string n = name ? Foundation::utf8(x, name) : "";
    if (n == "kCGColorWhite") return makeColor(x, {1, 1, 1, 1}, cs_gray(x));
    if (n == "kCGColorClear") return makeColor(x, {0, 0, 0, 0}, cs_gray(x));
    return makeColor(x, {0, 0, 0, 1}, cs_gray(x));
}

// ========================================================= data providers ===

struct ProviderData : ObjC::HostObject {
    Process* proc = nullptr;
    std::vector<uint8_t> bytes;      // host copy (files, CFData)
    uint32_t guestData = 0, guestSize = 0, info = 0, releaseFn = 0;
    id cfdata = 0;
    std::vector<uint8_t> contents(Ctx& x) const {
        if (cfdata) return Foundation::dataBytes(x, cfdata);
        if (guestData) return std::vector<uint8_t>(x.mem.host(guestData), x.mem.host(guestData) + guestSize);
        return bytes;
    }
};
id newProvider(Ctx& x, std::unique_ptr<ProviderData> h) { h->proc = &x.proc; return newObject(x, S(x).provider, std::move(h)); }
uint32_t dp_createWithData(Ctx& x, uint32_t info, GuestPtr data, uint32_t size, uint32_t releaseFn) {
    auto h = std::make_unique<ProviderData>();
    h->guestData = data; h->guestSize = size; h->info = info; h->releaseFn = releaseFn;
    return newProvider(x, std::move(h));
}
uint32_t dp_createWithCFData(Ctx& x, id data) {
    auto h = std::make_unique<ProviderData>();
    h->cfdata = rt(x).retain(x.cpu, data);
    return newProvider(x, std::move(h));
}
uint32_t dp_createWithFilename(Ctx& x, GuestPtr name) {
    std::ifstream in(x.proc.readCString(name), std::ios::binary);
    if (!in) return 0;
    auto h = std::make_unique<ProviderData>();
    h->bytes.assign(std::istreambuf_iterator<char>(in), {});
    return newProvider(x, std::move(h));
}
uint32_t dp_createWithURL(Ctx& x, id url) {
    std::ifstream in(Foundation::urlFilePath(x, url), std::ios::binary);
    if (!in) return 0;
    auto h = std::make_unique<ProviderData>();
    h->bytes.assign(std::istreambuf_iterator<char>(in), {});
    return newProvider(x, std::move(h));
}
uint32_t dp_copyData(Ctx& x, id p) {
    ProviderData* d = hd<ProviderData>(x, p);
    if (!d) return 0;
    const auto b = d->contents(x);
    return Foundation::makeData(x, b.data(), uint32_t(b.size()), true);
}
void dp_dealloc(Ctx& x, id self, SEL) {
    if (ProviderData* d = hd<ProviderData>(x, self)) {
        if (d->releaseFn) x.proc.callGuest(x.cpu, d->releaseFn, {d->info, d->guestData, d->guestSize});
        const id cf = d->cfdata;
        d->cfdata = 0;
        if (cf) rt(x).release(x.cpu, cf);
    }
    rt(x).disposeInstance(x.cpu, self);
}

// ================================================================= images ===

// Convert raw pixels described by CGImageCreate parameters to premultiplied RGBA.
G::ImagePtr convertPixels(const uint8_t* src, size_t avail, int w, int h, int bpc, int bpp, int bpr, uint32_t bitmapInfo, int model, bool mask) {
    if (w <= 0 || h <= 0 || size_t(bpr) * size_t(h - 1) + size_t((w * bpp + 7) / 8) > avail) return nullptr;
    auto img = std::make_shared<G::Image>();
    img->width = w; img->height = h;
    img->rgba.resize(size_t(w) * size_t(h) * 4);
    const uint32_t alphaInfo = bitmapInfo & 0x1F, order = bitmapInfo & 0x7000;
    const bool premul = alphaInfo == 1 || alphaInfo == 2;
    const bool hasAlpha = alphaInfo >= 1 && alphaInfo <= 4;
    const bool alphaFirst = alphaInfo == 2 || alphaInfo == 4 || alphaInfo == 6;
    const bool little = order == 0x2000 || order == 0x1000;
    bool opaque = true;
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = src + size_t(y) * size_t(bpr);
        uint8_t* o = &img->rgba[size_t(y) * size_t(w) * 4];
        for (int x = 0; x < w; ++x, o += 4) {
            uint32_t r = 0, g = 0, b = 0, a = 255;
            if (mask || alphaInfo == 7) {   // alpha-only / image mask
                const uint8_t v = bpc == 8 ? row[x] : uint8_t(((row[x * bpc / 8] >> (8 - bpc - (x * bpc) % 8)) & ((1 << bpc) - 1)) * 255 / ((1 << bpc) - 1));
                a = mask ? 255 - v : v;   // image masks: 0 paints
                r = g = b = 0;
            } else if (bpc == 8 && bpp == 32) {
                uint8_t p[4];
                std::memcpy(p, row + x * 4, 4);
                if (little) std::swap(p[0], p[3]), std::swap(p[1], p[2]);
                if (alphaFirst) { a = p[0]; r = p[1]; g = p[2]; b = p[3]; } else { r = p[0]; g = p[1]; b = p[2]; a = p[3]; }
                if (!hasAlpha) a = 255;
            } else if (bpc == 8 && bpp == 24) {
                r = row[x * 3]; g = row[x * 3 + 1]; b = row[x * 3 + 2];
            } else if (bpc == 8 && bpp == 16 && model == 0) {   // gray + alpha
                r = g = b = row[x * 2]; a = row[x * 2 + 1];
                if (alphaFirst) { a = row[x * 2]; r = g = b = row[x * 2 + 1]; }
            } else if (bpc == 8 && bpp == 8) {
                r = g = b = row[x];
            } else if (bpp == 16) {
                uint16_t v = uint16_t(row[x * 2] | row[x * 2 + 1] << 8);
                if (!little) v = uint16_t(v >> 8 | v << 8);
                if (bpc == 5) {   // 5551
                    if (alphaFirst) { a = (v >> 15) ? 255 : (hasAlpha ? 0 : 255); r = (v >> 10) & 31; g = (v >> 5) & 31; b = v & 31; }
                    else { r = v >> 11; g = (v >> 6) & 31; b = (v >> 1) & 31; a = (v & 1) || !hasAlpha ? 255 : 0; }
                    r = r * 255 / 31; g = g * 255 / 31; b = b * 255 / 31;
                } else if (bpc == 4) {   // 4444
                    if (alphaFirst) { a = v >> 12; r = (v >> 8) & 15; g = (v >> 4) & 15; b = v & 15; }
                    else { r = v >> 12; g = (v >> 8) & 15; b = (v >> 4) & 15; a = v & 15; }
                    r *= 17; g *= 17; b *= 17; a = hasAlpha ? a * 17 : 255;
                }
            }
            if (!premul && a != 255) { r = r * a / 255; g = g * a / 255; b = b * a / 255; }
            if (premul) { r = std::min(r, a); g = std::min(g, a); b = std::min(b, a); }
            o[0] = uint8_t(r); o[1] = uint8_t(g); o[2] = uint8_t(b); o[3] = uint8_t(a);
            if (a != 255) opaque = false;
        }
    }
    img->opaque = opaque;
    return img;
}

id newImage(Ctx& x, G::ImagePtr img, bool mask = false) {
    auto h = std::make_unique<ImageData>();
    h->image = std::move(img);
    h->isMask = mask;
    h->bitmapInfo = h->image && h->image->opaque ? 5 /* NoneSkipLast */ : 1;
    return newObject(x, S(x).image, std::move(h));
}
uint32_t img_create(Ctx& x, uint32_t w, uint32_t h, uint32_t bpc, uint32_t bpp, uint32_t bpr, id space, uint32_t info, id provider,
                    GuestPtr, bool, uint32_t) {
    ProviderData* p = hd<ProviderData>(x, provider);
    if (!p) return 0;
    const auto bytes = p->contents(x);
    auto img = convertPixels(bytes.data(), bytes.size(), int(w), int(h), int(bpc), int(bpp), int(bpr), info, spaceModel(x, space), false);
    if (!img) { x.proc.logf(LogLevel::Warning, "CGImageCreate: unsupported or short pixel data (%ux%u bpc %u bpp %u)", w, h, bpc, bpp); return 0; }
    return newImage(x, img);
}
uint32_t img_maskCreate(Ctx& x, uint32_t w, uint32_t h, uint32_t bpc, uint32_t bpp, uint32_t bpr, id provider, GuestPtr, bool) {
    ProviderData* p = hd<ProviderData>(x, provider);
    if (!p) return 0;
    const auto bytes = p->contents(x);
    auto img = convertPixels(bytes.data(), bytes.size(), int(w), int(h), int(bpc), int(bpp), int(bpr), 0, 0, true);
    return img ? newImage(x, img, true) : 0;
}
uint32_t img_fromProvider(Ctx& x, id provider, GuestPtr, bool, uint32_t) {
    ProviderData* p = hd<ProviderData>(x, provider);
    if (!p) return 0;
    const auto bytes = p->contents(x);
    std::string err;
    auto img = G::decodeImage(bytes.data(), bytes.size(), &err);
    if (!img) { x.proc.logf(LogLevel::Warning, "CGImage: can't decode image data: %s", err.c_str()); return 0; }
    return newImage(x, img);
}
uint32_t img_copy(Ctx& x, id img) { ImageData* d = hd<ImageData>(x, img); return d ? newImage(x, d->image, d->isMask) : 0; }
uint32_t img_inRect(Ctx& x, id img, CGRect r) {
    ImageData* d = hd<ImageData>(x, img);
    if (!d || !d->image) return 0;
    const G::Rect rr = toG(standardize(r)).intersect({0, 0, float(d->image->width), float(d->image->height)});
    const int x0 = int(std::floor(rr.x)), y0 = int(std::floor(rr.y)), x1 = int(std::ceil(rr.maxX())), y1 = int(std::ceil(rr.maxY()));
    if (x1 <= x0 || y1 <= y0) return 0;
    auto out = std::make_shared<G::Image>();
    out->width = x1 - x0; out->height = y1 - y0;
    out->rgba.resize(size_t(out->width) * size_t(out->height) * 4);
    for (int y = y0; y < y1; ++y)
        std::memcpy(&out->rgba[size_t(y - y0) * size_t(out->width) * 4], &d->image->rgba[(size_t(y) * size_t(d->image->width) + size_t(x0)) * 4], size_t(out->width) * 4);
    out->opaque = d->image->opaque;
    return newImage(x, out, d->isMask);
}
uint32_t img_withMask(Ctx& x, id img, id mask) {
    ImageData *d = hd<ImageData>(x, img), *m = hd<ImageData>(x, mask);
    if (!d || !m || !d->image || !m->image) return img_copy(x, img);
    auto out = std::make_shared<G::Image>(*d->image);
    const G::Image& mi = *m->image;
    for (int y = 0; y < out->height; ++y)
        for (int xx = 0; xx < out->width; ++xx) {
            const int mx = xx * mi.width / out->width, my = y * mi.height / out->height;
            const uint8_t* mp = &mi.rgba[(size_t(my) * size_t(mi.width) + size_t(mx)) * 4];
            // masks: alpha channel (image masks were stored inverted) or luminance for gray images
            const uint32_t k = m->isMask ? mp[3] : (mp[3] == 255 ? (mp[0] * 299u + mp[1] * 587u + mp[2] * 114u) / 1000u : mp[3]);
            uint8_t* o = &out->rgba[(size_t(y) * size_t(out->width) + size_t(xx)) * 4];
            for (int c = 0; c < 4; ++c) o[c] = uint8_t(o[c] * k / 255);
        }
    out->opaque = false;
    return newImage(x, out);
}
uint32_t img_width(Ctx& x, id img) { ImageData* d = hd<ImageData>(x, img); return d && d->image ? uint32_t(d->image->width) : 0; }
uint32_t img_height(Ctx& x, id img) { ImageData* d = hd<ImageData>(x, img); return d && d->image ? uint32_t(d->image->height) : 0; }
uint32_t img_bpc(Ctx&, id) { return 8; }
uint32_t img_bpp(Ctx&, id) { return 32; }
uint32_t img_bpr(Ctx& x, id img) { return img_width(x, img) * 4; }
uint32_t img_bitmapInfo(Ctx& x, id img) { ImageData* d = hd<ImageData>(x, img); return d ? d->bitmapInfo : 0; }
uint32_t img_alphaInfo(Ctx& x, id img) { return img_bitmapInfo(x, img) & 0x1F; }
uint32_t img_space(Ctx& x, id) { return cs_rgb(x); }
bool img_isMask(Ctx& x, id img) { ImageData* d = hd<ImageData>(x, img); return d && d->isMask; }
uint32_t img_provider(Ctx& x, id img) {
    ImageData* d = hd<ImageData>(x, img);
    if (!d || !d->image) return 0;
    auto h = std::make_unique<ProviderData>();
    h->bytes = d->image->rgba;
    if (d->bitmapInfo == 5) for (size_t i = 3; i < h->bytes.size(); i += 4) h->bytes[i] = 255;
    // Owned by the image (returned at +0): autorelease it.
    return rt(x).autorelease(x.cpu, newProvider(x, std::move(h)));
}

// ================================================================== paths ===

struct PathData : ObjC::HostObject { G::Path path; };
G::Path* pathOf(Ctx& x, id p) { PathData* d = hd<PathData>(x, p); return d ? &d->path : nullptr; }
G::Affine optTransform(Ctx& x, GuestPtr m) { return m ? toG(readStruct<CGAffineTransform>(x, m)) : G::Affine{}; }

// Build in a scratch user space, then append (transformed) to `dst`.
void appendBuilt(G::Path& dst, const G::Affine& t, const std::function<void(G::Path&)>& build) {
    G::Path tmp;
    const bool cont = dst.hasCurrent && !dst.subpaths.empty() && !dst.closed.back();
    if (dst.hasCurrent) tmp.moveTo(t.inverted().apply(dst.current));
    build(tmp);
    G::Path dev = tmp.transformed(t);
    for (size_t i = 0; i < dev.subpaths.size(); ++i) {
        auto& sp = dev.subpaths[i];
        if (i == 0 && dst.hasCurrent) {
            if (cont) dst.subpaths.back().insert(dst.subpaths.back().end(), sp.begin() + 1, sp.end());
            else if (sp.size() > 1) { dst.subpaths.push_back(sp); dst.closed.push_back(dev.closed[i]); continue; }
            if (dev.closed[i]) dst.closed.back() = true;
            continue;
        }
        dst.subpaths.push_back(sp);
        dst.closed.push_back(dev.closed[i]);
    }
    if (!dev.subpaths.empty()) {
        dst.current = dev.current;
        dst.start = dev.start;
        dst.hasCurrent = dev.hasCurrent;
    }
}
id newPath(Ctx& x, G::Path p = {}) { auto h = std::make_unique<PathData>(); h->path = std::move(p); return newObject(x, S(x).path, std::move(h)); }
uint32_t path_create(Ctx& x) { return newPath(x); }
uint32_t path_copy(Ctx& x, id p) { G::Path* s = pathOf(x, p); return s ? newPath(x, *s) : 0; }
uint32_t path_copyTransform(Ctx& x, id p, GuestPtr m) { G::Path* s = pathOf(x, p); return s ? newPath(x, s->transformed(optTransform(x, m))) : 0; }
uint32_t path_withRect(Ctx& x, CGRect r, GuestPtr m) { G::Path p; p.addRect(toG(r)); return newPath(x, p.transformed(optTransform(x, m))); }
uint32_t path_withEllipse(Ctx& x, CGRect r, GuestPtr m) { G::Path p; p.addEllipse(toG(r)); return newPath(x, p.transformed(optTransform(x, m))); }
uint32_t path_withRounded(Ctx& x, CGRect r, float rw, float rh, GuestPtr m) { G::Path p; p.addRoundedRect(toG(r), rw, rh); return newPath(x, p.transformed(optTransform(x, m))); }
void path_moveTo(Ctx& x, id p, GuestPtr m, float px, float py) { if (G::Path* s = pathOf(x, p)) s->moveTo(optTransform(x, m).apply({px, py})); }
void path_lineTo(Ctx& x, id p, GuestPtr m, float px, float py) { if (G::Path* s = pathOf(x, p)) s->lineTo(optTransform(x, m).apply({px, py})); }
void path_curveTo(Ctx& x, id p, GuestPtr m, float c1x, float c1y, float c2x, float c2y, float px, float py) {
    if (G::Path* s = pathOf(x, p)) { const G::Affine t = optTransform(x, m); s->cubicTo(t.apply({c1x, c1y}), t.apply({c2x, c2y}), t.apply({px, py})); }
}
void path_quadTo(Ctx& x, id p, GuestPtr m, float cx, float cy, float px, float py) {
    if (G::Path* s = pathOf(x, p)) { const G::Affine t = optTransform(x, m); s->quadTo(t.apply({cx, cy}), t.apply({px, py})); }
}
void path_arc(Ctx& x, id p, GuestPtr m, float cx, float cy, float r, float a0, float a1, bool cw) {
    if (G::Path* s = pathOf(x, p)) appendBuilt(*s, optTransform(x, m), [&](G::Path& t) { t.arc({cx, cy}, r, a0, a1, cw); });
}
void path_relativeArc(Ctx& x, id p, GuestPtr m, float cx, float cy, float r, float a0, float delta) {
    if (G::Path* s = pathOf(x, p)) appendBuilt(*s, optTransform(x, m), [&](G::Path& t) { t.arc({cx, cy}, r, a0, a0 + delta, delta < 0); });
}
void path_arcTo(Ctx& x, id p, GuestPtr m, float x1, float y1, float x2, float y2, float r) {
    if (G::Path* s = pathOf(x, p)) appendBuilt(*s, optTransform(x, m), [&](G::Path& t) { t.arcTo({x1, y1}, {x2, y2}, r); });
}
void path_addRect(Ctx& x, id p, GuestPtr m, CGRect r) { if (G::Path* s = pathOf(x, p)) { G::Path t; t.addRect(toG(r)); for (auto& sp : t.transformed(optTransform(x, m)).subpaths) { s->subpaths.push_back(sp); s->closed.push_back(true); } s->hasCurrent = true; s->current = s->start = s->subpaths.back().front(); } }
void path_addRects(Ctx& x, id p, GuestPtr m, GuestPtr rects, uint32_t n) { for (uint32_t i = 0; i < n; ++i) path_addRect(x, p, m, readStruct<CGRect>(x, rects + i * 16)); }
void path_addEllipse(Ctx& x, id p, GuestPtr m, CGRect r) { if (G::Path* s = pathOf(x, p)) { G::Path t; t.addEllipse(toG(r)); for (auto& sp : t.transformed(optTransform(x, m)).subpaths) { s->subpaths.push_back(sp); s->closed.push_back(true); } s->hasCurrent = true; s->current = s->start = s->subpaths.back().front(); } }
void path_addRounded(Ctx& x, id p, GuestPtr m, CGRect r, float rw, float rh) { if (G::Path* s = pathOf(x, p)) { G::Path t; t.addRoundedRect(toG(r), rw, rh); for (auto& sp : t.transformed(optTransform(x, m)).subpaths) { s->subpaths.push_back(sp); s->closed.push_back(true); } s->hasCurrent = true; s->current = s->start = s->subpaths.back().front(); } }
void path_addLines(Ctx& x, id p, GuestPtr m, GuestPtr pts, uint32_t n) {
    G::Path* s = pathOf(x, p);
    if (!s) return;
    const G::Affine t = optTransform(x, m);
    for (uint32_t i = 0; i < n; ++i) {
        const G::Point q = t.apply(toG(readStruct<CGPoint>(x, pts + i * 8)));
        if (i == 0) s->moveTo(q); else s->lineTo(q);
    }
}
void path_addPath(Ctx& x, id p, GuestPtr m, id other) {
    G::Path *s = pathOf(x, p), *o = pathOf(x, other);
    if (!s || !o) return;
    G::Path t = o->transformed(optTransform(x, m));
    s->subpaths.insert(s->subpaths.end(), t.subpaths.begin(), t.subpaths.end());
    s->closed.insert(s->closed.end(), t.closed.begin(), t.closed.end());
    if (t.hasCurrent) { s->hasCurrent = true; s->current = t.current; s->start = t.start; }
}
void path_close(Ctx& x, id p) { if (G::Path* s = pathOf(x, p)) s->close(); }
bool path_isEmpty(Ctx& x, id p) { G::Path* s = pathOf(x, p); return !s || s->empty(); }
bool path_isRect(Ctx& x, id p, GuestPtr out) {
    G::Path* s = pathOf(x, p);
    if (!s || s->subpaths.size() != 1 || s->subpaths[0].size() != 4) return false;
    const auto& q = s->subpaths[0];
    const bool axis = (q[0].y == q[1].y && q[1].x == q[2].x && q[2].y == q[3].y && q[3].x == q[0].x) ||
                      (q[0].x == q[1].x && q[1].y == q[2].y && q[2].x == q[3].x && q[3].y == q[0].y);
    if (axis && out) writeStruct(x, out, fromG(s->bounds()));
    return axis;
}
void path_bbox(Ctx& x, GuestPtr ret, id p) { G::Path* s = pathOf(x, p); writeStruct(x, ret, s && !s->empty() ? fromG(s->bounds()) : nullRect()); }
void path_current(Ctx& x, GuestPtr ret, id p) { G::Path* s = pathOf(x, p); writeStruct(x, ret, s ? fromG(s->current) : CGPoint{0, 0}); }
bool path_contains(Ctx& x, id p, GuestPtr m, CGPoint pt, bool eo) {
    G::Path* s = pathOf(x, p);
    if (!s) return false;
    return s->transformed(optTransform(x, m)).contains(toG(pt), eo);
}
bool path_equal(Ctx& x, id a, id b) {
    G::Path *p = pathOf(x, a), *q = pathOf(x, b);
    if (!p || !q || p->subpaths.size() != q->subpaths.size()) return false;
    for (size_t i = 0; i < p->subpaths.size(); ++i) {
        if (p->subpaths[i].size() != q->subpaths[i].size()) return false;
        for (size_t j = 0; j < p->subpaths[i].size(); ++j) if (p->subpaths[i][j].x != q->subpaths[i][j].x || p->subpaths[i][j].y != q->subpaths[i][j].y) return false;
    }
    return true;
}
uint32_t path_strokeCopy(Ctx& x, id p, GuestPtr m, float width, int32_t cap, int32_t join, float miter) {
    G::Path* s = pathOf(x, p);
    if (!s) return 0;
    return newPath(x, G::strokeToPath(s->transformed(optTransform(x, m)), width, cap, join, miter, {}, 0));
}
void path_apply(Ctx& x, id p, uint32_t info, uint32_t fn) {
    // CGPathApply(path, info, function(info, const CGPathElement*)): element { type; CGPoint *points }
    G::Path* s = pathOf(x, p);
    if (!s || !fn) return;
    const uint32_t elem = x.proc.heap().alloc(8), pts = x.proc.heap().alloc(24);
    x.mem.write<uint32_t>(elem + 4, pts);
    for (size_t i = 0; i < s->subpaths.size(); ++i) {
        const auto& sp = s->subpaths[i];
        for (size_t j = 0; j < sp.size(); ++j) {
            x.mem.write<uint32_t>(elem, j == 0 ? 0 : 1);   // moveTo / addLineTo
            writeStruct(x, pts, fromG(sp[j]));
            x.proc.callGuest(x.cpu, fn, {info, elem});
        }
        if (s->closed[i]) { x.mem.write<uint32_t>(elem, 4); x.proc.callGuest(x.cpu, fn, {info, elem}); }
    }
    x.proc.heap().free(elem);
    x.proc.heap().free(pts);
}

// ============================================================== gradients ===

struct GradientData : ObjC::HostObject { std::vector<std::pair<float, G::Color>> stops; };
G::Color sampleGradient(const GradientData& g, float t) {
    if (g.stops.empty()) return {};
    if (t <= g.stops.front().first) return g.stops.front().second;
    if (t >= g.stops.back().first) return g.stops.back().second;
    for (size_t i = 1; i < g.stops.size(); ++i) {
        if (t <= g.stops[i].first) {
            const auto& [t0, c0] = g.stops[i - 1];
            const auto& [t1, c1] = g.stops[i];
            const float k = t1 > t0 ? (t - t0) / (t1 - t0) : 0;
            return {c0.r + (c1.r - c0.r) * k, c0.g + (c1.g - c0.g) * k, c0.b + (c1.b - c0.b) * k, c0.a + (c1.a - c0.a) * k};
        }
    }
    return g.stops.back().second;
}
id newGradient(Ctx& x, std::vector<std::pair<float, G::Color>> stops) {
    std::sort(stops.begin(), stops.end(), [](auto& a, auto& b) { return a.first < b.first; });
    auto h = std::make_unique<GradientData>();
    h->stops = std::move(stops);
    return newObject(x, S(x).gradient, std::move(h));
}
uint32_t grad_withComponents(Ctx& x, id space, GuestPtr comps, GuestPtr locs, uint32_t n) {
    const bool gray = spaceModel(x, space) == 0;
    std::vector<std::pair<float, G::Color>> stops;
    for (uint32_t i = 0; i < n; ++i) {
        const float t = locs ? x.mem.read<float>(locs + i * 4) : (n > 1 ? float(i) / float(n - 1) : 0);
        G::Color c;
        if (gray) { const float v = x.mem.read<float>(comps + i * 8); c = {v, v, v, x.mem.read<float>(comps + i * 8 + 4)}; }
        else c = {x.mem.read<float>(comps + i * 16), x.mem.read<float>(comps + i * 16 + 4), x.mem.read<float>(comps + i * 16 + 8), x.mem.read<float>(comps + i * 16 + 12)};
        stops.emplace_back(t, c);
    }
    return newGradient(x, std::move(stops));
}
uint32_t grad_withColors(Ctx& x, id, id colors, GuestPtr locs) {
    const auto items = Foundation::arrayItems(x, colors);
    std::vector<std::pair<float, G::Color>> stops;
    for (size_t i = 0; i < items.size(); ++i) {
        const float t = locs ? x.mem.read<float>(locs + uint32_t(i) * 4) : (items.size() > 1 ? float(i) / float(items.size() - 1) : 0);
        stops.emplace_back(t, colorOf(x, items[i]));
    }
    return newGradient(x, std::move(stops));
}

// =============================================================== contexts ===

G::Clip fullClip(const G::Surface& s) { G::Clip c; c.x0 = 0; c.y0 = 0; c.x1 = s.width; c.y1 = s.height; return c; }

float ctmScale(const G::Affine& t) { return std::sqrt(std::fabs(t.det())); }

ContextData* cx(Ctx& x, id c) { return hd<ContextData>(x, c); }

void ctx_save(Ctx& x, id c) { if (ContextData* d = cx(x, c)) d->stack.push_back(d->gs()); }
void ctx_restore(Ctx& x, id c) { if (ContextData* d = cx(x, c); d && d->stack.size() > 1) d->stack.pop_back(); }
void ctx_translate(Ctx& x, id c, float tx, float ty) { if (ContextData* d = cx(x, c)) d->gs().ctm = G::Affine::translate(tx, ty).then(d->gs().ctm); }
void ctx_scale(Ctx& x, id c, float sx, float sy) { if (ContextData* d = cx(x, c)) d->gs().ctm = G::Affine::scale(sx, sy).then(d->gs().ctm); }
void ctx_rotate(Ctx& x, id c, float a) { if (ContextData* d = cx(x, c)) d->gs().ctm = G::Affine::rotate(a).then(d->gs().ctm); }
void ctx_concat(Ctx& x, id c, CGAffineTransform t) { if (ContextData* d = cx(x, c)) d->gs().ctm = toG(t).then(d->gs().ctm); }
G::Affine baseCTM(const ContextData& d) { return {1, 0, 0, -1, 0, float(d.surface.height)}; }
void ctx_getCTM(Ctx& x, GuestPtr ret, id c) {
    ContextData* d = cx(x, c);
    // The CTM reported to apps excludes the device flip, like CG (user → default user space).
    writeStruct(x, ret, d ? fromG(d->gs().ctm.then(baseCTM(*d).inverted())) : CGAffineTransform{1, 0, 0, 1, 0, 0});
}
void ctx_getUserToDevice(Ctx& x, GuestPtr ret, id c) { ContextData* d = cx(x, c); writeStruct(x, ret, d ? fromG(d->gs().ctm) : CGAffineTransform{1, 0, 0, 1, 0, 0}); }

void ctx_setRGBFill(Ctx& x, id c, float r, float g, float b, float a) { if (ContextData* d = cx(x, c)) d->gs().fill = {r, g, b, a}; }
void ctx_setRGBStroke(Ctx& x, id c, float r, float g, float b, float a) { if (ContextData* d = cx(x, c)) d->gs().stroke = {r, g, b, a}; }
void ctx_setGrayFill(Ctx& x, id c, float v, float a) { if (ContextData* d = cx(x, c)) d->gs().fill = {v, v, v, a}; }
void ctx_setGrayStroke(Ctx& x, id c, float v, float a) { if (ContextData* d = cx(x, c)) d->gs().stroke = {v, v, v, a}; }
void ctx_setFillColorWithColor(Ctx& x, id c, id col) { if (ContextData* d = cx(x, c)) d->gs().fill = colorOf(x, col); }
void ctx_setStrokeColorWithColor(Ctx& x, id c, id col) { if (ContextData* d = cx(x, c)) d->gs().stroke = colorOf(x, col); }
G::Color readComponents(Ctx& x, GuestPtr p, bool gray) {
    if (!p) return {0, 0, 0, 1};
    if (gray) { const float v = x.mem.read<float>(p); return {v, v, v, x.mem.read<float>(p + 4)}; }
    return {x.mem.read<float>(p), x.mem.read<float>(p + 4), x.mem.read<float>(p + 8), x.mem.read<float>(p + 12)};
}
void ctx_setFillColor(Ctx& x, id c, GuestPtr comps) { if (ContextData* d = cx(x, c)) d->gs().fill = readComponents(x, comps, false); }
void ctx_setStrokeColor(Ctx& x, id c, GuestPtr comps) { if (ContextData* d = cx(x, c)) d->gs().stroke = readComponents(x, comps, false); }
void ctx_setCMYKFill(Ctx& x, id c, float cy, float m, float y, float k, float a) { if (ContextData* d = cx(x, c)) d->gs().fill = {(1 - cy) * (1 - k), (1 - m) * (1 - k), (1 - y) * (1 - k), a}; }
void ctx_setAlpha(Ctx& x, id c, float a) { if (ContextData* d = cx(x, c)) d->gs().alpha = std::clamp(a, 0.0f, 1.0f); }
void ctx_setLineWidth(Ctx& x, id c, float w) { if (ContextData* d = cx(x, c)) d->gs().lineWidth = w; }
void ctx_setLineCap(Ctx& x, id c, int32_t v) { if (ContextData* d = cx(x, c)) d->gs().lineCap = v; }
void ctx_setLineJoin(Ctx& x, id c, int32_t v) { if (ContextData* d = cx(x, c)) d->gs().lineJoin = v; }
void ctx_setMiter(Ctx& x, id c, float v) { if (ContextData* d = cx(x, c)) d->gs().miterLimit = v; }
void ctx_setFlatness(Ctx& x, id c, float v) { if (ContextData* d = cx(x, c)) d->gs().flatness = v; }
void ctx_setLineDash(Ctx& x, id c, float phase, GuestPtr lengths, uint32_t n) {
    ContextData* d = cx(x, c);
    if (!d) return;
    d->gs().dash.clear();
    for (uint32_t i = 0; i < n && lengths; ++i) d->gs().dash.push_back(x.mem.read<float>(lengths + i * 4));
    d->gs().dashPhase = phase;
}
G::BlendMode blendMode(int32_t m) {
    switch (m) {
        case 1: return G::BlendMode::Multiply;
        case 2: return G::BlendMode::Screen;
        case 3: return G::BlendMode::Overlay;
        case 4: return G::BlendMode::Darken;
        case 5: return G::BlendMode::Lighten;
        case 16: return G::BlendMode::Clear;
        case 17: return G::BlendMode::Copy;
        case 18: return G::BlendMode::SourceIn;
        case 22: return G::BlendMode::DestinationIn;
        case 23: return G::BlendMode::DestinationOut;
        case 27: return G::BlendMode::PlusLighter;
        default: return G::BlendMode::Normal;
    }
}
void ctx_setBlend(Ctx& x, id c, int32_t m) { if (ContextData* d = cx(x, c)) d->gs().blend = blendMode(m); }
void ctx_setAA(Ctx& x, id c, bool v) { if (ContextData* d = cx(x, c)) d->gs().antialias = v; }
void ctx_setInterp(Ctx& x, id c, int32_t q) { if (ContextData* d = cx(x, c)) d->gs().smoothImages = q != 1; }
int32_t ctx_getInterp(Ctx& x, id c) { ContextData* d = cx(x, c); return d && !d->gs().smoothImages ? 1 : 0; }
void ctx_ignore1(Ctx&, id, uint32_t) {}
void ctx_setShadow(Ctx& x, id, CGSize, float) { x.proc.reportMissing("CGContextSetShadow (shadows are not drawn)"); }
void ctx_setShadowColor(Ctx& x, id, CGSize, float, id) { x.proc.reportMissing("CGContextSetShadowWithColor (shadows are not drawn)"); }

// ---- path construction (device space)
void ctx_beginPath(Ctx& x, id c) { if (ContextData* d = cx(x, c)) d->path = {}; }
void ctx_moveTo(Ctx& x, id c, float px, float py) { if (ContextData* d = cx(x, c)) d->path.moveTo(d->gs().ctm.apply({px, py})); }
void ctx_lineTo(Ctx& x, id c, float px, float py) { if (ContextData* d = cx(x, c)) d->path.lineTo(d->gs().ctm.apply({px, py})); }
void ctx_curveTo(Ctx& x, id c, float c1x, float c1y, float c2x, float c2y, float px, float py) {
    if (ContextData* d = cx(x, c)) { const G::Affine& t = d->gs().ctm; d->path.cubicTo(t.apply({c1x, c1y}), t.apply({c2x, c2y}), t.apply({px, py})); }
}
void ctx_quadTo(Ctx& x, id c, float cx_, float cy, float px, float py) {
    if (ContextData* d = cx(x, c)) { const G::Affine& t = d->gs().ctm; d->path.quadTo(t.apply({cx_, cy}), t.apply({px, py})); }
}
void ctx_arc(Ctx& x, id c, float px, float py, float r, float a0, float a1, int32_t cw) {
    if (ContextData* d = cx(x, c)) appendBuilt(d->path, d->gs().ctm, [&](G::Path& t) { t.arc({px, py}, r, a0, a1, cw != 0); });
}
void ctx_arcTo(Ctx& x, id c, float x1, float y1, float x2, float y2, float r) {
    if (ContextData* d = cx(x, c)) appendBuilt(d->path, d->gs().ctm, [&](G::Path& t) { t.arcTo({x1, y1}, {x2, y2}, r); });
}
void addUserPath(ContextData& d, const G::Path& user) {
    G::Path dev = user.transformed(d.gs().ctm);
    d.path.subpaths.insert(d.path.subpaths.end(), dev.subpaths.begin(), dev.subpaths.end());
    d.path.closed.insert(d.path.closed.end(), dev.closed.begin(), dev.closed.end());
    if (dev.hasCurrent) { d.path.hasCurrent = true; d.path.current = dev.current; d.path.start = dev.start; }
}
void ctx_addRect(Ctx& x, id c, CGRect r) { if (ContextData* d = cx(x, c)) { G::Path p; p.addRect(toG(r)); addUserPath(*d, p); } }
void ctx_addRects(Ctx& x, id c, GuestPtr rects, uint32_t n) { for (uint32_t i = 0; i < n; ++i) ctx_addRect(x, c, readStruct<CGRect>(x, rects + i * 16)); }
void ctx_addEllipse(Ctx& x, id c, CGRect r) { if (ContextData* d = cx(x, c)) { G::Path p; p.addEllipse(toG(r)); addUserPath(*d, p); } }
void ctx_addLines(Ctx& x, id c, GuestPtr pts, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        const CGPoint p = readStruct<CGPoint>(x, pts + i * 8);
        if (i == 0) ctx_moveTo(x, c, p.x, p.y); else ctx_lineTo(x, c, p.x, p.y);
    }
}
void ctx_addPath(Ctx& x, id c, id path) { ContextData* d = cx(x, c); G::Path* p = pathOf(x, path); if (d && p) addUserPath(*d, *p); }
void ctx_closePath(Ctx& x, id c) { if (ContextData* d = cx(x, c)) d->path.close(); }
bool ctx_isPathEmpty(Ctx& x, id c) { ContextData* d = cx(x, c); return !d || d->path.empty(); }
void ctx_pathCurrent(Ctx& x, GuestPtr ret, id c) {
    ContextData* d = cx(x, c);
    writeStruct(x, ret, d && d->path.hasCurrent ? fromG(d->gs().ctm.inverted().apply(d->path.current)) : CGPoint{0, 0});
}
void ctx_pathBBox(Ctx& x, GuestPtr ret, id c) {
    ContextData* d = cx(x, c);
    if (!d || d->path.empty()) { writeStruct(x, ret, nullRect()); return; }
    writeStruct(x, ret, fromG(d->gs().ctm.inverted().applyRect(d->path.bounds())));
}
bool ctx_pathContains(Ctx& x, id c, CGPoint p, int32_t mode) {
    ContextData* d = cx(x, c);
    return d && d->path.contains(d->gs().ctm.apply(toG(p)), mode == 1 || mode == 4);
}

G::Path devStroke(ContextData& d, const G::Path& devPath) {
    const GState& g = d.gs();
    const float s = ctmScale(g.ctm);
    std::vector<float> dash = g.dash;
    for (float& v : dash) v *= s;
    return G::strokeToPath(devPath, std::max(g.lineWidth * s, 0.35f), g.lineCap, g.lineJoin, g.miterLimit, dash, g.dashPhase * s);
}
void doFill(ContextData& d, const G::Path& p, bool eo) { G::fillPath(d.surface, p, eo, d.gs().fill, d.gs().clip, d.gs().blend, d.gs().alpha); }
void doStroke(ContextData& d, const G::Path& p) { G::fillPath(d.surface, devStroke(d, p), false, d.gs().stroke, d.gs().clip, d.gs().blend, d.gs().alpha); }

void ctx_fillPath(Ctx& x, id c) { if (ContextData* d = cx(x, c)) { doFill(*d, d->path, false); d->path = {}; } }
void ctx_eoFillPath(Ctx& x, id c) { if (ContextData* d = cx(x, c)) { doFill(*d, d->path, true); d->path = {}; } }
void ctx_strokePath(Ctx& x, id c) { if (ContextData* d = cx(x, c)) { doStroke(*d, d->path); d->path = {}; } }
void ctx_drawPath(Ctx& x, id c, int32_t mode) {
    ContextData* d = cx(x, c);
    if (!d) return;
    // kCGPathFill, EOFill, Stroke, FillStroke, EOFillStroke
    if (mode == 0 || mode == 3) doFill(*d, d->path, false);
    if (mode == 1 || mode == 4) doFill(*d, d->path, true);
    if (mode >= 2) doStroke(*d, d->path);
    d->path = {};
}
void ctx_replaceWithStroked(Ctx& x, id c) { if (ContextData* d = cx(x, c)) d->path = devStroke(*d, d->path); }
void ctx_fillRect(Ctx& x, id c, CGRect r) {
    ContextData* d = cx(x, c);
    if (!d) return;
    G::Path p;
    p.addRect(toG(r));
    G::Path dev = p.transformed(d->gs().ctm);
    if (d->gs().ctm.axisAligned()) G::fillRect(d->surface, dev.bounds(), d->gs().fill, d->gs().clip, d->gs().blend, d->gs().alpha);
    else doFill(*d, dev, false);
}
void ctx_fillRects(Ctx& x, id c, GuestPtr rects, uint32_t n) { for (uint32_t i = 0; i < n; ++i) ctx_fillRect(x, c, readStruct<CGRect>(x, rects + i * 16)); }
void ctx_strokeRectWidth(Ctx& x, id c, CGRect r, float w) {
    ContextData* d = cx(x, c);
    if (!d) return;
    const float old = d->gs().lineWidth;
    d->gs().lineWidth = w;
    G::Path p;
    p.addRect(toG(r));
    doStroke(*d, p.transformed(d->gs().ctm));
    d->gs().lineWidth = old;
}
void ctx_strokeRect(Ctx& x, id c, CGRect r) { ContextData* d = cx(x, c); if (d) ctx_strokeRectWidth(x, c, r, d->gs().lineWidth); }
void ctx_clearRect(Ctx& x, id c, CGRect r) {
    ContextData* d = cx(x, c);
    if (!d) return;
    G::Path p;
    p.addRect(toG(r));
    G::fillPath(d->surface, p.transformed(d->gs().ctm), false, {0, 0, 0, 0}, d->gs().clip, G::BlendMode::Copy, 1);
}
void ctx_fillEllipse(Ctx& x, id c, CGRect r) { if (ContextData* d = cx(x, c)) { G::Path p; p.addEllipse(toG(r)); doFill(*d, p.transformed(d->gs().ctm), false); } }
void ctx_strokeEllipse(Ctx& x, id c, CGRect r) { if (ContextData* d = cx(x, c)) { G::Path p; p.addEllipse(toG(r)); doStroke(*d, p.transformed(d->gs().ctm)); } }
void ctx_strokeSegments(Ctx& x, id c, GuestPtr pts, uint32_t n) {
    ContextData* d = cx(x, c);
    if (!d) return;
    G::Path p;
    for (uint32_t i = 0; i + 1 < n; i += 2) {
        p.moveTo(d->gs().ctm.apply(toG(readStruct<CGPoint>(x, pts + i * 8))));
        p.lineTo(d->gs().ctm.apply(toG(readStruct<CGPoint>(x, pts + (i + 1) * 8))));
    }
    doStroke(*d, p);
}

// ---- clipping
void ctx_clip(Ctx& x, id c) { if (ContextData* d = cx(x, c)) { d->gs().clip = G::intersectClip(d->gs().clip, d->path, false); d->path = {}; } }
void ctx_eoClip(Ctx& x, id c) { if (ContextData* d = cx(x, c)) { d->gs().clip = G::intersectClip(d->gs().clip, d->path, true); d->path = {}; } }
void ctx_clipToRect(Ctx& x, id c, CGRect r) {
    ContextData* d = cx(x, c);
    if (!d) return;
    G::Path p;
    p.addRect(toG(standardize(r)));
    d->gs().clip = G::intersectClip(d->gs().clip, p.transformed(d->gs().ctm), false);
}
void ctx_clipToRects(Ctx& x, id c, GuestPtr rects, uint32_t n) {
    ContextData* d = cx(x, c);
    if (!d) return;
    G::Path p;
    for (uint32_t i = 0; i < n; ++i) p.addRect(toG(standardize(readStruct<CGRect>(x, rects + i * 16))));
    d->gs().clip = G::intersectClip(d->gs().clip, p.transformed(d->gs().ctm), false);
}
void ctx_clipToMask(Ctx& x, id c, CGRect r, id mask) {
    ContextData* d = cx(x, c);
    ImageData* m = hd<ImageData>(x, mask);
    if (!d || !m || !m->image) return;
    // Rasterize the mask image into a coverage buffer over the clip bounds.
    G::Clip& cl = d->gs().clip;
    G::Image cov;
    cov.width = cl.x1 - cl.x0; cov.height = cl.y1 - cl.y0;
    if (cov.width <= 0 || cov.height <= 0) return;
    cov.rgba.assign(size_t(cov.width) * size_t(cov.height) * 4, 0);
    G::Surface s = cov.surface();
    const G::Rect rr = toG(r);
    G::Affine m2{rr.w / float(m->image->width), 0, 0, -rr.h / float(m->image->height), rr.x, rr.maxY()};
    G::Affine toCov = m2.then(d->gs().ctm).then(G::Affine::translate(float(-cl.x0), float(-cl.y0)));
    G::Image src = *m->image;
    if (!m->isMask) {   // a normal image: use its luminance (CG uses the image's gray values as alpha)
        for (size_t i = 0; i < src.rgba.size(); i += 4) {
            const uint32_t lum = (src.rgba[i] * 299u + src.rgba[i + 1] * 587u + src.rgba[i + 2] * 114u) / 1000u;
            src.rgba[i + 3] = uint8_t(lum); src.rgba[i] = src.rgba[i + 1] = src.rgba[i + 2] = 0;
        }
    }
    G::Clip all; all.x0 = 0; all.y0 = 0; all.x1 = cov.width; all.y1 = cov.height;
    G::drawImage(s, src, toCov, all, 1, true, G::BlendMode::Copy);
    auto mk = std::make_shared<std::vector<uint8_t>>(size_t(cov.width) * size_t(cov.height));
    for (int y = 0; y < cov.height; ++y)
        for (int xx = 0; xx < cov.width; ++xx)
            (*mk)[size_t(y) * size_t(cov.width) + size_t(xx)] = uint8_t(cov.rgba[(size_t(y) * size_t(cov.width) + size_t(xx)) * 4 + 3] * cl.at(xx + cl.x0, y + cl.y0) / 255);
    G::Clip out = cl;
    out.mask = mk;
    cl = out;
}
void ctx_clipBBox(Ctx& x, GuestPtr ret, id c) {
    ContextData* d = cx(x, c);
    if (!d) { writeStruct(x, ret, nullRect()); return; }
    const G::Clip& cl = d->gs().clip;
    writeStruct(x, ret, fromG(d->gs().ctm.inverted().applyRect({float(cl.x0), float(cl.y0), float(cl.x1 - cl.x0), float(cl.y1 - cl.y0)})));
}

// ---- images
void drawImageUser(ContextData& d, const G::Image& img, const G::Rect& r, bool yDown, float alpha, G::BlendMode m, const G::Color* tint = nullptr) {
    if (img.width <= 0 || img.height <= 0) return;
    const G::Affine toUser = yDown ? G::Affine{r.w / float(img.width), 0, 0, r.h / float(img.height), r.x, r.y}
                                   : G::Affine{r.w / float(img.width), 0, 0, -r.h / float(img.height), r.x, r.maxY()};
    G::drawImage(d.surface, img, toUser.then(d.gs().ctm), d.gs().clip, alpha * d.gs().alpha, d.gs().smoothImages, m, tint);
}
void ctx_drawImage(Ctx& x, id c, CGRect r, id image) {
    ContextData* d = cx(x, c);
    ImageData* i = hd<ImageData>(x, image);
    if (!d || !i || !i->image) return;
    if (i->isMask) { const G::Color f = d->gs().fill; drawImageUser(*d, *i->image, toG(r), false, 1, d->gs().blend, &f); return; }
    drawImageUser(*d, *i->image, toG(r), false, 1, d->gs().blend);
}
void ctx_drawTiled(Ctx& x, id c, CGRect r, id image) {
    ContextData* d = cx(x, c);
    ImageData* i = hd<ImageData>(x, image);
    if (!d || !i || !i->image || r.size.width <= 0 || r.size.height <= 0) return;
    // Tile over the clip bounds (in user space).
    const G::Clip& cl = d->gs().clip;
    const G::Rect ub = d->gs().ctm.inverted().applyRect({float(cl.x0), float(cl.y0), float(cl.x1 - cl.x0), float(cl.y1 - cl.y0)});
    const float tw = r.size.width, th = r.size.height;
    const float sx = r.origin.x + std::floor((ub.x - r.origin.x) / tw) * tw, sy = r.origin.y + std::floor((ub.y - r.origin.y) / th) * th;
    int guard = 0;
    for (float yy = sy; yy < ub.maxY() && guard < 100000; yy += th)
        for (float xx = sx; xx < ub.maxX() && guard < 100000; xx += tw, ++guard) drawImageUser(*d, *i->image, {xx, yy, tw, th}, false, 1, d->gs().blend);
}

// ---- gradients
void drawGradient(ContextData& d, const GradientData& g, uint32_t options, const std::function<bool(G::Point, float&)>& tAt) {
    const G::Clip& cl = d.gs().clip;
    const G::Affine inv = d.gs().ctm.inverted();
    G::Image tmp;
    tmp.width = cl.x1 - cl.x0; tmp.height = cl.y1 - cl.y0;
    if (tmp.width <= 0 || tmp.height <= 0) return;
    tmp.rgba.assign(size_t(tmp.width) * size_t(tmp.height) * 4, 0);
    for (int y = 0; y < tmp.height; ++y)
        for (int xx = 0; xx < tmp.width; ++xx) {
            const G::Point u = inv.apply({float(cl.x0 + xx) + 0.5f, float(cl.y0 + y) + 0.5f});
            float t;
            if (!tAt(u, t)) continue;
            if (t < 0 && !(options & 1)) continue;
            if (t > 1 && !(options & 2)) continue;
            const G::Color c = sampleGradient(g, std::clamp(t, 0.0f, 1.0f));
            uint8_t* o = &tmp.rgba[(size_t(y) * size_t(tmp.width) + size_t(xx)) * 4];
            o[0] = uint8_t(c.r * c.a * 255); o[1] = uint8_t(c.g * c.a * 255); o[2] = uint8_t(c.b * c.a * 255); o[3] = uint8_t(c.a * 255);
        }
    G::drawImage(d.surface, tmp, G::Affine::translate(float(cl.x0), float(cl.y0)), cl, d.gs().alpha, false, d.gs().blend);
}
void ctx_linearGradient(Ctx& x, id c, id grad, CGPoint s, CGPoint e, uint32_t options) {
    ContextData* d = cx(x, c);
    GradientData* g = hd<GradientData>(x, grad);
    if (!d || !g) return;
    const float dx = e.x - s.x, dy = e.y - s.y, len2 = dx * dx + dy * dy;
    drawGradient(*d, *g, options, [&](G::Point u, float& t) {
        t = len2 > 0 ? ((u.x - s.x) * dx + (u.y - s.y) * dy) / len2 : 0;
        return true;
    });
}
void ctx_radialGradient(Ctx& x, id c, id grad, CGPoint c0, float r0, CGPoint c1, float r1, uint32_t options) {
    ContextData* d = cx(x, c);
    GradientData* g = hd<GradientData>(x, grad);
    if (!d || !g) return;
    const float cdx = c1.x - c0.x, cdy = c1.y - c0.y, dr = r1 - r0;
    const float a = cdx * cdx + cdy * cdy - dr * dr;
    drawGradient(*d, *g, options, [&](G::Point u, float& t) {
        const float pdx = u.x - c0.x, pdy = u.y - c0.y;
        const float b = pdx * cdx + pdy * cdy + r0 * dr;
        const float cc = pdx * pdx + pdy * pdy - r0 * r0;
        if (std::fabs(a) < 1e-6f) {
            if (std::fabs(b) < 1e-6f) return false;
            t = cc / (2 * b);
        } else {
            const float disc = b * b - a * cc;
            if (disc < 0) return false;
            const float sq = std::sqrt(disc);
            t = (b + sq) / a;
            if (r0 + t * dr < 0) t = (b - sq) / a;
        }
        return r0 + t * dr >= 0;
    });
}

// ---- text (legacy CGContextShowText family)
void showText(Ctx& x, ContextData& d, const std::string& s, G::Point pos) {
    const GState& g = d.gs();
    G::FontDesc f;
    f.name = g.fontName;
    f.size = g.fontSize;
    f.bold = g.fontName.find("Bold") != std::string::npos;
    const G::Affine textToDevice = g.textMatrix.then(G::Affine::translate(pos.x, pos.y)).then(g.ctm);
    const float scale = std::max(ctmScale(textToDevice), 0.01f);
    G::TextLayout lay;
    auto mask = G::textBackend().render(s, f, scale, &lay);
    if (mask && g.textMode != 3) {   // kCGTextInvisible
        // image pixels (y down) → text space (y up, baseline at 0)
        const G::Affine imgToText{1 / scale, 0, 0, -1 / scale, 0, lay.ascent};
        const G::Color fill = g.fill;
        G::drawImage(d.surface, *mask, imgToText.then(textToDevice), g.clip, g.alpha, true, g.blend, &fill);
    }
    d.textPos = {pos.x + lay.width + g.charSpacing * float(s.size()), pos.y};
}
void ctx_selectFont(Ctx& x, id c, GuestPtr name, float size, uint32_t) {
    if (ContextData* d = cx(x, c)) { d->gs().fontName = x.proc.readCString(name); d->gs().fontSize = size; }
}
void ctx_setFontSize(Ctx& x, id c, float size) { if (ContextData* d = cx(x, c)) d->gs().fontSize = size; }
struct FontData : ObjC::HostObject {
    std::string name;
    std::shared_ptr<G::TrueTypeFont> face;
    float size = 12;
    G::Affine matrix;
};
void ctx_setFont(Ctx& x, id c, id font) { if (ContextData* d = cx(x, c)) if (FontData* f = hd<FontData>(x, font)) d->gs().fontName = f->name; }
void ctx_setCharSpacing(Ctx& x, id c, float v) { if (ContextData* d = cx(x, c)) d->gs().charSpacing = v; }
void ctx_setTextMode(Ctx& x, id c, int32_t m) { if (ContextData* d = cx(x, c)) d->gs().textMode = m; }
void ctx_setTextMatrix(Ctx& x, id c, CGAffineTransform t) { if (ContextData* d = cx(x, c)) d->gs().textMatrix = toG(t); }
void ctx_getTextMatrix(Ctx& x, GuestPtr ret, id c) { ContextData* d = cx(x, c); writeStruct(x, ret, d ? fromG(d->gs().textMatrix) : CGAffineTransform{1, 0, 0, 1, 0, 0}); }
void ctx_setTextPos(Ctx& x, id c, float px, float py) { if (ContextData* d = cx(x, c)) d->textPos = {px, py}; }
void ctx_getTextPos(Ctx& x, GuestPtr ret, id c) { ContextData* d = cx(x, c); writeStruct(x, ret, d ? fromG(d->textPos) : CGPoint{0, 0}); }
void ctx_showTextAt(Ctx& x, id c, float px, float py, GuestPtr str, uint32_t len) {
    ContextData* d = cx(x, c);
    if (!d || !str) return;
    showText(x, *d, std::string(reinterpret_cast<const char*>(x.mem.host(str)), len), {px, py});
}
void ctx_showText(Ctx& x, id c, GuestPtr str, uint32_t len) { ContextData* d = cx(x, c); if (d) ctx_showTextAt(x, c, d->textPos.x, d->textPos.y, str, len); }
void ctx_showGlyphs(Ctx& x, id, GuestPtr, uint32_t) { x.proc.reportMissing("CGContextShowGlyphs (glyph IDs need font tables)"); }
uint32_t font_createWithName(Ctx& x, id name) {
    auto h = std::make_unique<FontData>();
    h->name = name ? Foundation::utf8(x, name) : "Helvetica";
    return newObject(x, S(x).font, std::move(h));
}
uint32_t font_createWithProvider(Ctx& x, id provider) {
    auto* data = hd<ProviderData>(x, provider);
    if (!data) return 0;
    auto face = G::TrueTypeFont::load(data->contents(x));
    if (!face) return 0;
    auto h = std::make_unique<FontData>();
    h->name = face->name();
    h->face = std::move(face);
    return newObject(x, S(x).font, std::move(h));
}
uint32_t font_copyName(Ctx& x, id f) { FontData* d = hd<FontData>(x, f); return Foundation::makeString(x, d ? d->name : "Helvetica", true); }

// CGFont design-unit metrics (B-109).
// All return int32 in design units; the CGFont contract is "font units", not points.
// When the face is absent (name-only font), fall back to sensible defaults scaled
// to a notional 2048-upem grid so callers that divide by unitsPerEm get ~1.0.
int32_t font_getUnitsPerEm(Ctx& x, id f) {
    FontData* d = hd<FontData>(x, f);
    if (d && d->face) return d->face->unitsPerEm();
    return 2048;
}
int32_t font_getAscent(Ctx& x, id f) {
    FontData* d = hd<FontData>(x, f);
    if (d && d->face) return d->face->ascentDesign();
    return 1638; // ~0.8 × 2048
}
int32_t font_getDescent(Ctx& x, id f) {
    FontData* d = hd<FontData>(x, f);
    if (d && d->face) return d->face->descentDesign(); // negative in TrueType
    return -410;  // ~-0.2 × 2048
}
int32_t font_getCapHeight(Ctx& x, id f) {
    FontData* d = hd<FontData>(x, f);
    if (d && d->face) {
        const int v = d->face->capHeightDesign();
        if (v != 0) return v;
        return d->face->ascentDesign(); // OS/2 absent: use ascent
    }
    return 1434; // ~0.7 × 2048
}
int32_t font_getXHeight(Ctx& x, id f) {
    FontData* d = hd<FontData>(x, f);
    if (d && d->face) {
        const int v = d->face->xHeightDesign();
        if (v != 0) return v;
        return d->face->ascentDesign() / 2; // OS/2 absent: half-ascent heuristic
    }
    return 1024;
}
// CGFontGetGlyphAdvances(font, glyphs, count, advances)
// glyphs: uint16[] CGGlyph array; advances: int32[] output.  Returns true if all
// glyphs were found.
bool font_getGlyphAdvances(Ctx& x, id f, GuestPtr glyphs, int32_t count, GuestPtr advances) {
    FontData* d = hd<FontData>(x, f);
    if (!d || !d->face || count <= 0 || !glyphs || !advances) return false;
    bool all = true;
    for (int32_t i = 0; i < count; ++i) {
        const int g = int(x.mem.read<uint16_t>(glyphs + uint32_t(i) * 2));
        const int a = d->face->glyphAdvanceDesign(g);
        x.mem.write<int32_t>(advances + uint32_t(i) * 4, int32_t(a));
        if (g == 0) all = false;
    }
    return all;
}
// CGFontCopyTableForTag(font, tag) → CFDataRef (or NULL when absent).
// tag is the four-byte big-endian OpenType tag passed as a uint32_t.
uint32_t font_copyTableForTag(Ctx& x, id f, uint32_t tag) {
    FontData* d = hd<FontData>(x, f);
    if (!d || !d->face) return 0;
    const std::vector<uint8_t> data = d->face->rawTable(tag);
    if (data.empty()) return 0;
    // Wrap in an NSData/CFData object (owned — caller is responsible for release).
    return Foundation::makeData(x, data.data(), uint32_t(data.size()), true);
}

uint32_t ct_createGraphics(Ctx& x, id font, float size, GuestPtr matrix, id) {
    auto* f = hd<FontData>(x, font);
    if (!f || !std::isfinite(size) || size < 0) return 0;
    auto h = std::make_unique<FontData>(*f);
    h->size = size ? size : 12;
    if (matrix) h->matrix = toG(readStruct<CGAffineTransform>(x, matrix));
    return newObject(x, S(x).ctfont, std::move(h));
}
uint32_t ct_createName(Ctx& x, id name, float size, GuestPtr matrix) {
    const id cg = font_createWithName(x, name);
    const id ct = ct_createGraphics(x, cg, size, matrix, 0);
    rt(x).release(x.cpu, cg);
    return ct;
}
uint32_t ct_copyDescriptor(Ctx& x, id font) {
    auto* f = hd<FontData>(x, font);
    return f ? newObject(x, S(x).descriptor, std::make_unique<FontData>(*f)) : 0;
}
uint32_t ct_createDescriptor(Ctx& x, id desc, float size, GuestPtr matrix) {
    auto* f = hd<FontData>(x, desc);
    return f ? ct_createGraphics(x, desc, size ? size : f->size, matrix, 0) : 0;
}
uint32_t ct_family(Ctx& x, id font) {
    auto* f = hd<FontData>(x, font);
    return f ? Foundation::makeString(x, f->face ? f->face->name(true) : f->name, true) : 0;
}
uint32_t ct_traits(Ctx& x, id font) {
    auto* f = hd<FontData>(x, font);
    return !f ? 0 : f->face ? f->face->traits() :
        (f->name.find("Bold") != std::string::npos ? 2u : 0u) | (f->name.find("Italic") != std::string::npos ? 1u : 0u);
}
float ct_underline(Ctx& x, id font) {
    auto* f = hd<FontData>(x, font);
    if (f && f->face) return f->face->underlineThickness(f->size);
    x.proc.reportMissing("CTFontGetUnderlineThickness system font metrics"); return 0;
}
template<int Metric> float ct_metric(Ctx& x, id font) {
    auto* f = hd<FontData>(x, font);
    if (!f) return 0;
    if (f->face) return Metric == 0 ? f->face->ascent(f->size) : Metric == 1 ? f->face->descent(f->size) : f->face->leading(f->size);
    const auto m = G::textBackend().measure("M", {f->name, f->size});
    return Metric == 0 ? m.ascent : Metric == 1 ? m.descent : m.lineHeight - m.ascent - m.descent;
}
bool ct_characters(Ctx& x, id font, GuestPtr chars, GuestPtr glyphs, int32_t count) {
    auto* f = hd<FontData>(x, font);
    if (!f || !f->face) { x.proc.reportMissing("CTFontGetGlyphsForCharacters system font mapping"); return false; }
    bool all = true;
    for (int32_t i = 0; i < count; ++i) {
        const int g = f->face->glyph(x.mem.read<uint16_t>(chars + uint32_t(i)*2));
        x.mem.write<uint16_t>(glyphs + uint32_t(i)*2, uint16_t(g)); all &= g != 0;
    }
    return all;
}
double ct_advances(Ctx& x, id font, uint32_t orientation, GuestPtr glyphs, GuestPtr advances, int32_t count) {
    auto* f = hd<FontData>(x, font);
    if (!f || !f->face || orientation == 2) { x.proc.reportMissing("CTFontGetAdvancesForGlyphs unsupported font/orientation"); return 0; }
    double total = 0;
    for (int32_t i = 0; i < count; ++i) {
        const float a = f->face->advance(x.mem.read<uint16_t>(glyphs + uint32_t(i)*2), f->size);
        const auto v = f->matrix.applyVector({a, 0});
        if (advances) writeStruct(x, advances + uint32_t(i)*8, CGSize{v.x, v.y});
        total += v.x;
    }
    return total;
}
void ct_bounds(Ctx& x, GuestPtr ret, id font, uint32_t orientation, GuestPtr glyphs, GuestPtr rects, int32_t count) {
    auto* f = hd<FontData>(x, font);
    G::Rect total{};
    if (!f || !f->face || orientation == 2) x.proc.reportMissing("CTFontGetBoundingRectsForGlyphs unsupported font/orientation");
    else for (int32_t i = 0; i < count; ++i) {
        const auto b = f->matrix.applyRect(f->face->bounds(x.mem.read<uint16_t>(glyphs + uint32_t(i)*2), f->size));
        if (rects) writeStruct(x, rects + uint32_t(i)*16, fromG(b));
        total = i == 0 ? b : total.unite(b);
    }
    writeStruct(x, ret, fromG(total));
}
void ct_draw(Ctx& x, id font, GuestPtr glyphs, GuestPtr positions, uint32_t count, id ctx) {
    auto* f = hd<FontData>(x, font);
    auto* d = cx(x, ctx);
    if (!f || !d) return;
    if (!f->face) { x.proc.reportMissing("CTFontDrawGlyphs system font outlines"); return; }
    const auto& g = d->gs();
    for (uint32_t i = 0; i < count; ++i) {
        auto p = readStruct<CGPoint>(x, positions + i*8);
        if (!d->allowsFontSubpixelPositioning || !g.subpixelPositionFonts) { p.x = std::round(p.x); p.y = std::round(p.y); }
        else if (d->allowsFontSubpixelQuantization && g.subpixelQuantizeFonts) { p.x = std::round(p.x*4)/4; p.y = std::round(p.y*4)/4; }
        const auto transform = f->matrix.then(g.textMatrix).then(G::Affine::translate(p.x, p.y)).then(g.ctm);
        const auto path = f->face->outline(x.mem.read<uint16_t>(glyphs + i*2), f->size).transformed(transform);
        if (g.textMode == 0 || g.textMode == 2) G::fillPath(d->surface, path, false, g.fill, g.clip, g.blend, g.alpha);
        else if (g.textMode != 3) x.proc.reportMissing("CTFontDrawGlyphs text drawing mode");
    }
}
void ctx_allowsFontPosition(Ctx& x, id c, bool b) { if (auto* d = cx(x, c)) d->allowsFontSubpixelPositioning = b; }
void ctx_allowsFontQuantize(Ctx& x, id c, bool b) { if (auto* d = cx(x, c)) d->allowsFontSubpixelQuantization = b; }
void ctx_fontPosition(Ctx& x, id c, bool b) { if (auto* d = cx(x, c)) d->gs().subpixelPositionFonts = b; }
void ctx_fontQuantize(Ctx& x, id c, bool b) { if (auto* d = cx(x, c)) d->gs().subpixelQuantizeFonts = b; }

// Uniform-attribute, horizontal TrueType text. Complex-script shaping and
// multi-run attributed strings remain explicit gaps rather than fake glyphs.
struct TextRange { int32_t location, length; static constexpr bool kGuestStruct = true; };
struct TextData : ObjC::HostObject {
    std::u16string string;
    id attributes = 0, runArray = 0;
    int32_t start = 0;
    std::vector<uint16_t> glyphs;
    std::vector<G::Point> positions;
    std::vector<float> advances;
    float width = 0, ascent = 0, descent = 0, leading = 0;
};
void text_dealloc(Ctx& x, id self, SEL) {
    if (auto* d = hd<TextData>(x, self)) {
        const id a = d->attributes, runs = d->runArray; d->attributes = d->runArray = 0;
        if (runs) rt(x).release(x.cpu, runs);
        if (a) rt(x).release(x.cpu, a);
    }
    rt(x).disposeInstance(x.cpu, self);
}
id text_font(Ctx& x, const TextData& d) {
    for (auto [key, value] : Foundation::dictEntries(x, d.attributes))
        if (Foundation::utf8(x, key) == "kCTFontAttributeName") return value;
    return 0;
}
uint32_t attributed_create(Ctx& x, uint32_t, id string, id attributes) {
    if (!string) return 0;
    auto h = std::make_unique<TextData>();
    h->string = Foundation::stringValue(x, string);
    h->attributes = attributes ? Foundation::makeDict(x, Foundation::dictEntries(x, attributes), true) : 0;
    return newObject(x, S(x).text, std::move(h));
}
uint32_t text_retain(Ctx& x, id text) { return hd<TextData>(x, text) ? rt(x).retain(x.cpu, text) : 0; }
uint32_t text_line(Ctx& x, id text, TextRange range) {
    auto* src = hd<TextData>(x, text);
    if (!src) return 0;
    const int32_t length = range.length ? range.length : int32_t(src->string.size()) - range.location;
    if (range.location < 0 || length < 0 || uint64_t(range.location) + length > src->string.size())
        throw std::out_of_range("CoreText line range outside attributed string");
    auto* font = hd<FontData>(x, text_font(x, *src));
    if (!font || !font->face) { x.proc.reportMissing("CoreText layout requires a custom TrueType font"); return 0; }
    auto h = std::make_unique<TextData>();
    h->start = range.location;
    h->string = src->string.substr(size_t(range.location), size_t(length));
    h->attributes = src->attributes ? rt(x).retain(x.cpu, src->attributes) : 0;
    h->ascent = font->face->ascent(font->size); h->descent = font->face->descent(font->size); h->leading = font->face->leading(font->size);
    for (char16_t ch : h->string) {
        if ((ch >= 0x590 && ch <= 0xDFF) || (ch >= 0xD800 && ch <= 0xDFFF)) {
            x.proc.reportMissing("CoreText complex-script/surrogate shaping");
            if (h->attributes) rt(x).release(x.cpu, h->attributes);
            return 0;
        }
        const int glyph = font->face->glyph(ch);
        const float advance = (ch == '\n' || ch == '\r') ? 0 : font->face->advance(glyph, font->size);
        h->glyphs.push_back(uint16_t(glyph)); h->positions.push_back({h->width, 0}); h->advances.push_back(advance); h->width += advance;
    }
    return newObject(x, S(x).text, std::move(h));
}
uint32_t text_wholeLine(Ctx& x, id text) { return text_line(x, text, {0,0}); }
int32_t text_break(Ctx& x, id text, int32_t start, double width) {
    auto* d = hd<TextData>(x, text);
    if (!d || start < 0 || size_t(start) >= d->string.size()) return 0;
    auto* f = hd<FontData>(x, text_font(x, *d));
    if (!f || !f->face) { x.proc.reportMissing("CoreText line breaking requires custom TrueType font"); return 0; }
    double used = 0; int32_t lastSpace = -1, i = start;
    for (; size_t(i) < d->string.size(); ++i) {
        const auto ch = d->string[size_t(i)];
        if (ch == '\n' || ch == '\r') return i - start + 1;
        const float a = f->face->advance(f->face->glyph(ch), f->size);
        if (used + a > width) break;
        used += a;
        if (ch == ' ' || ch == '\t') lastSpace = i;
    }
    if (size_t(i) == d->string.size()) return i - start;
    return lastSpace >= start ? lastSpace - start + 1 : std::max(i - start, 1);
}
void text_range(Ctx& x, GuestPtr ret, id text) {
    auto* d = hd<TextData>(x, text);
    writeStruct(x, ret, TextRange{d ? d->start : 0, d ? int32_t(d->string.size()) : 0});
}
double text_bounds(Ctx& x, id text, GuestPtr ascent, GuestPtr descent, GuestPtr leading) {
    auto* d = hd<TextData>(x, text);
    if (ascent) x.mem.write<float>(ascent, d ? d->ascent : 0);
    if (descent) x.mem.write<float>(descent, d ? d->descent : 0);
    if (leading) x.mem.write<float>(leading, d ? d->leading : 0);
    return d ? d->width : 0;
}
uint32_t text_runs(Ctx& x, id text) {
    auto* d = hd<TextData>(x, text);
    if (!d) return 0;
    if (!d->runArray) {
        auto h = std::make_unique<TextData>(*d);
        if (h->attributes) rt(x).retain(x.cpu, h->attributes);
        h->runArray = 0;
        const id run = newObject(x, S(x).text, std::move(h));
        d->runArray = Foundation::makeArray(x, {run}, true);
        rt(x).release(x.cpu, run);
    }
    return d->runArray; // Get API: borrowed, owned by the line; no self-retain cycle.
}
int32_t run_count(Ctx& x, id run) { auto* d = hd<TextData>(x, run); return d ? int32_t(d->glyphs.size()) : 0; }
uint32_t run_attrs(Ctx& x, id run) { auto* d = hd<TextData>(x, run); return d ? d->attributes : 0; }
template<int What> void run_copy(Ctx& x, id run, TextRange range, GuestPtr out) {
    auto* d = hd<TextData>(x, run);
    if (!d) return;
    const int32_t n = range.length ? range.length : int32_t(d->glyphs.size()) - range.location;
    if (range.location < 0 || n < 0 || uint64_t(range.location) + n > d->glyphs.size()) throw std::out_of_range("CTRun range outside glyphs");
    for (int32_t i = 0; i < n; ++i) {
        const auto j = size_t(range.location + i);
        if constexpr (What == 0) x.mem.write<uint16_t>(out + uint32_t(i)*2, d->glyphs[j]);
        if constexpr (What == 1) writeStruct(x, out + uint32_t(i)*8, fromG(d->positions[j]));
        if constexpr (What == 2) writeStruct(x, out + uint32_t(i)*8, CGSize{d->advances[j],0});
        if constexpr (What == 3) x.mem.write<int32_t>(out + uint32_t(i)*4, d->start + int32_t(j));
    }
}
uint32_t run_status(Ctx&, id) { return 0; } // These runs are horizontal, monotonic, identity matrix.
double run_bounds(Ctx& x, id run, TextRange range, GuestPtr ascent, GuestPtr descent, GuestPtr leading) {
    auto* d = hd<TextData>(x, run);
    text_bounds(x, run, ascent, descent, leading);
    if (!d) return 0;
    const int32_t n = range.length ? range.length : int32_t(d->advances.size()) - range.location;
    if (range.location < 0 || n < 0 || uint64_t(range.location)+n > d->advances.size()) throw std::out_of_range("CTRun bounds range");
    double width = 0;
    for (int32_t i=0; i<n; ++i) width += d->advances[size_t(range.location+i)];
    return width;
}
void text_draw(Ctx& x, id text, id ctx) {
    auto* d = hd<TextData>(x, text); auto* c = cx(x, ctx);
    if (!d || !c || d->glyphs.empty()) return;
    const uint32_t glyphs = x.proc.heap().alloc(uint32_t(d->glyphs.size())*2);
    const uint32_t positions = x.proc.heap().alloc(uint32_t(d->positions.size())*8);
    x.mem.copyIn(glyphs, d->glyphs.data(), d->glyphs.size()*2);
    for (size_t i=0; i<d->positions.size(); ++i) writeStruct(x, positions+uint32_t(i)*8, CGPoint{d->positions[i].x+c->textPos.x, d->positions[i].y+c->textPos.y});
    ct_draw(x, text_font(x,*d), glyphs, positions, uint32_t(d->glyphs.size()), ctx);
    x.proc.heap().free(glyphs); x.proc.heap().free(positions);
}

// ---- bitmap context API
uint32_t bm_create(Ctx& x, GuestPtr data, uint32_t w, uint32_t h, uint32_t bpc, uint32_t bpr, id space, uint32_t info) {
    if (bpc != 8) { x.proc.logf(LogLevel::Warning, "CGBitmapContextCreate: %u bits per component isn't supported", bpc); return 0; }
    const uint32_t alpha = info & 0x1F;
    const bool alphaOnly = alpha == 7 && !space;
    const bool gray = space && spaceModel(x, space) == 0;
    if (gray && alpha != 0) { x.proc.log(LogLevel::Warning, "CGBitmapContextCreate: gray contexts with alpha aren't supported"); }
    const id c = createBitmapContext(x, data, int(w), int(h), bpr, info, gray, alphaOnly || (alpha == 7));
    if (ContextData* d = cx(x, c)) { d->colorSpace = space ? rt(x).retain(x.cpu, space) : 0; }
    return c;
}
uint32_t bm_createWithCallback(Ctx& x, GuestPtr data, uint32_t w, uint32_t h, uint32_t bpc, uint32_t bpr, id space, uint32_t info, uint32_t, uint32_t) {
    return bm_create(x, data, w, h, bpc, bpr, space, info);
}
uint32_t bm_data(Ctx& x, id c) { ContextData* d = cx(x, c); return d ? d->guestData : 0; }
uint32_t bm_width(Ctx& x, id c) { ContextData* d = cx(x, c); return d ? uint32_t(d->surface.width) : 0; }
uint32_t bm_height(Ctx& x, id c) { ContextData* d = cx(x, c); return d ? uint32_t(d->surface.height) : 0; }
uint32_t bm_bpr(Ctx& x, id c) { ContextData* d = cx(x, c); return d ? d->bytesPerRow : 0; }
uint32_t bm_bpc(Ctx& x, id c) { ContextData* d = cx(x, c); return d ? d->bitsPerComponent : 0; }
uint32_t bm_bpp(Ctx& x, id c) { ContextData* d = cx(x, c); return d ? d->bitsPerPixel : 0; }
uint32_t bm_info(Ctx& x, id c) { ContextData* d = cx(x, c); return d ? d->bitmapInfo : 0; }
uint32_t bm_alphaInfo(Ctx& x, id c) { return bm_info(x, c) & 0x1F; }
uint32_t bm_space(Ctx& x, id c) { ContextData* d = cx(x, c); return d && d->colorSpace ? d->colorSpace : cs_rgb(x); }
uint32_t bm_createImage(Ctx& x, id c) { ContextData* d = cx(x, c); return d ? newImage(x, G::snapshot(d->surface)) : 0; }
void ctx_noop(Ctx&, id) {}
void ctx_dealloc(Ctx& x, id self, SEL) {
    if (ContextData* d = cx(x, self)) { const id cs = d->colorSpace; d->colorSpace = 0; if (cs) rt(x).release(x.cpu, cs); }
    rt(x).disposeInstance(x.cpu, self);
}
void ctx_convertPointToDevice(Ctx& x, GuestPtr ret, id c, CGPoint p) {
    ContextData* d = cx(x, c);
    writeStruct(x, ret, d ? fromG(d->gs().ctm.then(baseCTM(*d).inverted()).apply(toG(p))) : p);
}
void ctx_convertPointToUser(Ctx& x, GuestPtr ret, id c, CGPoint p) {
    ContextData* d = cx(x, c);
    writeStruct(x, ret, d ? fromG(d->gs().ctm.then(baseCTM(*d).inverted()).inverted().apply(toG(p))) : p);
}
void ctx_convertRectToDevice(Ctx& x, GuestPtr ret, id c, CGRect r) {
    ContextData* d = cx(x, c);
    writeStruct(x, ret, d ? fromG(d->gs().ctm.then(baseCTM(*d).inverted()).applyRect(toG(r))) : r);
}

} // namespace

// ================================================================ exports ===

ContextData::~ContextData() { if (ownsData && guestData && proc) proc->heap().free(guestData); }

ContextData* context(Ctx& x, id ctx) { return ctx ? rt(x).hostIf<ContextData>(ctx) : nullptr; }

id createBitmapContext(Ctx& x, uint32_t data, int w, int h, uint32_t bpr, uint32_t info, bool gray, bool alphaOnly) {
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return 0;
    auto d = std::make_unique<ContextData>();
    d->proc = &x.proc;
    const int bppBytes = gray || alphaOnly ? 1 : 4;
    if (!bpr) bpr = uint32_t(w * bppBytes);
    if (bpr < uint32_t(w * bppBytes)) return 0;
    if (!data) {
        data = x.proc.heap().allocZeroed(bpr * uint32_t(h));
        if (!data) return 0;
        d->ownsData = true;
    }
    d->guestData = data;
    d->bytesPerRow = bpr;
    d->bitsPerComponent = 8;
    d->bitsPerPixel = uint32_t(bppBytes * 8);
    d->bitmapInfo = info;
    G::Surface& s = d->surface;
    s.data = x.mem.host(data);
    s.width = w; s.height = h; s.stride = int(bpr);
    const uint32_t alpha = info & 0x1F, order = info & 0x7000;
    if (alphaOnly) s.format = G::PixelFormat::A8;
    else if (gray) { s.format = G::PixelFormat::Gray8; s.hasAlpha = false; }
    else {
        const bool first = alpha == 2 || alpha == 4 || alpha == 6;
        const bool little = order == 0x2000;
        s.format = first ? (little ? G::PixelFormat::BGRA : G::PixelFormat::ARGB) : (little ? G::PixelFormat::ABGR : G::PixelFormat::RGBA);
        s.hasAlpha = alpha >= 1 && alpha <= 4;
    }
    d->gs().ctm = baseCTM(*d);
    d->gs().clip = fullClip(s);
    return newObject(x, S(x).context, std::move(d));
}

G::ImagePtr imageOf(Ctx& x, id img) { ImageData* d = img ? rt(x).hostIf<ImageData>(img) : nullptr; return d ? d->image : nullptr; }
id createImage(Ctx& x, G::ImagePtr img) { return img ? newImage(x, std::move(img)) : 0; }
G::Color colorOf(Ctx& x, id c) {
    if (!c) return {0, 0, 0, 0};
    if (ColorData* d = rt(x).hostIf<ColorData>(c)) return d->c;
    // A UIColor passed where a CGColor is expected (or vice versa): ask it.
    if (rt(x).respondsTo(c, rt(x).sel("CGColor"))) {
        const id cg = id(rt(x).send(x.cpu, c, "CGColor"));
        if (ColorData* d = cg ? rt(x).hostIf<ColorData>(cg) : nullptr) return d->c;
    }
    return {0, 0, 0, 1};
}
id createColor(Ctx& x, G::Color c) { return makeColor(x, c, cs_rgb(x)); }
G::ImagePtr contextImage(Ctx& x, id c) { ContextData* d = context(x, c); return d ? G::snapshot(d->surface) : nullptr; }

void pushContext(Ctx& x, id c) { t_contextStack.push_back(rt(x).retain(x.cpu, c)); }
void popContext(Ctx& x) {
    if (t_contextStack.empty()) return;
    const id c = t_contextStack.back();
    t_contextStack.pop_back();
    rt(x).release(x.cpu, c);
}
id currentContext(Ctx&) { return t_contextStack.empty() ? 0 : t_contextStack.back(); }

void fillRect(Ctx& x, id ctx, const G::Rect& r, G::Color c, G::BlendMode m) {
    ContextData* d = context(x, ctx);
    if (!d) return;
    G::Path p;
    p.addRect(r);
    const G::Path dev = p.transformed(d->gs().ctm);
    if (d->gs().ctm.axisAligned()) G::fillRect(d->surface, dev.bounds(), c, d->gs().clip, m, d->gs().alpha);
    else G::fillPath(d->surface, dev, false, c, d->gs().clip, m, d->gs().alpha);
}
void drawImageInRect(Ctx& x, id ctx, const G::Image& img, const G::Rect& r, bool yDown, float alpha, G::BlendMode m) {
    if (ContextData* d = context(x, ctx)) drawImageUser(*d, img, r, yDown, alpha, m);
}
G::Size drawTextUIKit(Ctx& x, id ctx, const std::string& s, const G::FontDesc& f, G::Point p, G::Color c) {
    ContextData* d = context(x, ctx);
    G::TextLayout lay = G::textBackend().measure(s, f);
    if (!d || s.empty()) return {lay.width, lay.height};
    const G::Affine toDevice = G::Affine::translate(p.x, p.y).then(d->gs().ctm);
    const float scale = std::max(ctmScale(toDevice), 0.01f);
    auto mask = G::textBackend().render(s, f, scale, &lay);
    if (mask) G::drawImage(d->surface, *mask, G::Affine::scale(1 / scale, 1 / scale).then(toDevice), d->gs().clip, d->gs().alpha, true, d->gs().blend, &c);
    return {lay.width, lay.height};
}
void setUIKitCTM(Ctx& x, id ctx, float pointHeight, float scale) {
    ContextData* d = context(x, ctx);
    if (!d) return;
    (void)pointHeight;
    d->gs().ctm = G::Affine::scale(scale, scale);   // y down, origin top-left: device space scaled
}

void registerCoreGraphics(Process& p, ObjC::Runtime& r) {
    auto& s = p.service<CGState>();
    auto cls = [&](const char* name, void (*dealloc)(Ctx&, id, SEL)) {
        ObjC::ClassBuilder b = r.defineClass(name, "NSObject");
        if (dealloc) b.method("dealloc", [dealloc](Ctx& x) { dealloc(x, x.cpu.r[0], x.cpu.r[1]); }, "v@:");
        return b.cls();
    };
    s.context = cls("__HVMCGContext", &ctx_dealloc);
    s.image = cls("__HVMCGImage", nullptr);
    s.colorSpace = cls("__HVMCGColorSpace", nullptr);
    s.color = cls("__HVMCGColor", nullptr);
    s.provider = cls("__HVMCGDataProvider", &dp_dealloc);
    s.path = cls("__HVMCGPath", nullptr);
    s.gradient = cls("__HVMCGGradient", nullptr);
    s.font = cls("__HVMCGFont", nullptr);
    s.ctfont = cls("__HVMCTFont", nullptr);
    s.descriptor = cls("__HVMCTFontDescriptor", nullptr);
    s.text = cls("__HVMCTText", &text_dealloc);

    // geometry
    p.def<&r_minX>("_CGRectGetMinX"); p.def<&r_minY>("_CGRectGetMinY"); p.def<&r_maxX>("_CGRectGetMaxX"); p.def<&r_maxY>("_CGRectGetMaxY");
    p.def<&r_midX>("_CGRectGetMidX"); p.def<&r_midY>("_CGRectGetMidY"); p.def<&r_width>("_CGRectGetWidth"); p.def<&r_height>("_CGRectGetHeight");
    p.def<&r_isEmpty>("_CGRectIsEmpty"); p.def<&r_isNull>("_CGRectIsNull"); p.def<&r_isInfinite>("_CGRectIsInfinite");
    p.def<&r_containsPoint>("_CGRectContainsPoint"); p.def<&r_containsRect>("_CGRectContainsRect"); p.def<&r_intersects>("_CGRectIntersectsRect");
    p.def<&r_equal>("_CGRectEqualToRect"); p.def<&p_equal>("_CGPointEqualToPoint"); p.def<&s_equal>("_CGSizeEqualToSize");
    p.def<&r_intersection>("_CGRectIntersection"); p.def<&r_union>("_CGRectUnion"); p.def<&r_inset>("_CGRectInset");
    p.def<&r_offset>("_CGRectOffset"); p.def<&r_integral>("_CGRectIntegral"); p.def<&r_standardize>("_CGRectStandardize");
    p.def<&r_divide>("_CGRectDivide"); p.def<&p_apply>("_CGPointApplyAffineTransform"); p.def<&s_apply>("_CGSizeApplyAffineTransform");
    p.def<&r_apply>("_CGRectApplyAffineTransform");
    p.def<&t_make>("_CGAffineTransformMake"); p.def<&t_makeTranslation>("_CGAffineTransformMakeTranslation");
    p.def<&t_makeScale>("_CGAffineTransformMakeScale"); p.def<&t_makeRotation>("_CGAffineTransformMakeRotation");
    p.def<&t_translate>("_CGAffineTransformTranslate"); p.def<&t_scale>("_CGAffineTransformScale"); p.def<&t_rotate>("_CGAffineTransformRotate");
    p.def<&t_concat>("_CGAffineTransformConcat"); p.def<&t_invert>("_CGAffineTransformInvert"); p.def<&t_isIdentity>("_CGAffineTransformIsIdentity");
    p.def<&t_equal>("_CGAffineTransformEqualToTransform");
    {
        const uint32_t id_ = p.defineDataBlock("_CGAffineTransformIdentity", 24);
        p.mem().write<float>(id_, 1); p.mem().write<float>(id_ + 12, 1);
        p.defineDataBlock("_CGRectZero", 16);
        p.defineDataBlock("_CGPointZero", 8);
        p.defineDataBlock("_CGSizeZero", 8);
        const uint32_t nul = p.defineDataBlock("_CGRectNull", 16);
        p.mem().write<float>(nul, kInf); p.mem().write<float>(nul + 4, kInf);
        const uint32_t inf = p.defineDataBlock("_CGRectInfinite", 16);
        p.mem().write<float>(inf, -8.5e37f); p.mem().write<float>(inf + 4, -8.5e37f);
        p.mem().write<float>(inf + 8, 1.7e38f); p.mem().write<float>(inf + 12, 1.7e38f);
    }

    // colour spaces & colours
    p.def<&cs_rgb>("_CGColorSpaceCreateDeviceRGB");
    p.def<&cs_gray>("_CGColorSpaceCreateDeviceGray");
    p.def<&cs_createWithName>("_CGColorSpaceCreateWithName");
    p.def<&cs_components>("_CGColorSpaceGetNumberOfComponents");
    p.def<&cs_model>("_CGColorSpaceGetModel");
    for (const char* n : {"_CGColorSpaceRelease", "_CGColorRelease", "_CGImageRelease", "_CGDataProviderRelease", "_CGPathRelease",
                          "_CGGradientRelease", "_CGFontRelease", "_CGContextRelease"})
        p.def<&cf_release>(n);
    for (const char* n : {"_CGColorSpaceRetain", "_CGColorRetain", "_CGImageRetain", "_CGDataProviderRetain", "_CGPathRetain",
                          "_CGGradientRetain", "_CGFontRetain", "_CGContextRetain"})
        p.def<&cf_retain>(n);
    for (const char* n : {"_kCGColorSpaceGenericRGB", "_kCGColorSpaceGenericGray", "_kCGColorSpaceSRGB", "_kCGColorSpaceDeviceRGB",
                          "_kCGColorWhite", "_kCGColorBlack", "_kCGColorClear"}) {
        const uint32_t cell = p.defineDataBlock(n, 4);
        p.mem().write<uint32_t>(cell, Foundation::constString(p, n + 1));
    }
    p.def<&col_create>("_CGColorCreate");
    p.def<&col_createGenericRGB>("_CGColorCreateGenericRGB");
    p.def<&col_createGenericGray>("_CGColorCreateGenericGray");
    p.def<&col_copyWithAlpha>("_CGColorCreateCopyWithAlpha");
    p.def<&col_copy>("_CGColorCreateCopy");
    p.def<&col_components>("_CGColorGetComponents");
    p.def<&col_numComponents>("_CGColorGetNumberOfComponents");
    p.def<&col_alpha>("_CGColorGetAlpha");
    p.def<&col_space>("_CGColorGetColorSpace");
    p.def<&col_equal>("_CGColorEqualToColor");
    p.def<&col_getConstant>("_CGColorGetConstantColor");

    // data providers & images
    p.def<&dp_createWithData>("_CGDataProviderCreateWithData");
    p.def<&dp_createWithCFData>("_CGDataProviderCreateWithCFData");
    p.def<&dp_createWithFilename>("_CGDataProviderCreateWithFilename");
    p.def<&dp_createWithURL>("_CGDataProviderCreateWithURL");
    p.def<&dp_copyData>("_CGDataProviderCopyData");
    p.def<&img_create>("_CGImageCreate");
    p.def<&img_maskCreate>("_CGImageMaskCreate");
    p.def<&img_fromProvider>("_CGImageCreateWithPNGDataProvider");
    p.def<&img_fromProvider>("_CGImageCreateWithJPEGDataProvider");
    p.def<&img_copy>("_CGImageCreateCopy");
    p.def<&img_inRect>("_CGImageCreateWithImageInRect");
    p.def<&img_withMask>("_CGImageCreateWithMask");
    p.def<&img_width>("_CGImageGetWidth");
    p.def<&img_height>("_CGImageGetHeight");
    p.def<&img_bpc>("_CGImageGetBitsPerComponent");
    p.def<&img_bpp>("_CGImageGetBitsPerPixel");
    p.def<&img_bpr>("_CGImageGetBytesPerRow");
    p.def<&img_bitmapInfo>("_CGImageGetBitmapInfo");
    p.def<&img_alphaInfo>("_CGImageGetAlphaInfo");
    p.def<&img_space>("_CGImageGetColorSpace");
    p.def<&img_isMask>("_CGImageIsMask");
    p.def<&img_provider>("_CGImageGetDataProvider");

    // paths
    p.def<&path_create>("_CGPathCreateMutable");
    p.def<&path_copy>("_CGPathCreateCopy");
    p.def<&path_copy>("_CGPathCreateMutableCopy");
    p.def<&path_copyTransform>("_CGPathCreateCopyByTransformingPath");
    p.def<&path_copyTransform>("_CGPathCreateMutableCopyByTransformingPath");
    p.def<&path_withRect>("_CGPathCreateWithRect");
    p.def<&path_withEllipse>("_CGPathCreateWithEllipseInRect");
    p.def<&path_withRounded>("_CGPathCreateWithRoundedRect");
    p.def<&path_moveTo>("_CGPathMoveToPoint");
    p.def<&path_lineTo>("_CGPathAddLineToPoint");
    p.def<&path_curveTo>("_CGPathAddCurveToPoint");
    p.def<&path_quadTo>("_CGPathAddQuadCurveToPoint");
    p.def<&path_arc>("_CGPathAddArc");
    p.def<&path_relativeArc>("_CGPathAddRelativeArc");
    p.def<&path_arcTo>("_CGPathAddArcToPoint");
    p.def<&path_addRect>("_CGPathAddRect");
    p.def<&path_addRects>("_CGPathAddRects");
    p.def<&path_addEllipse>("_CGPathAddEllipseInRect");
    p.def<&path_addRounded>("_CGPathAddRoundedRect");
    p.def<&path_addLines>("_CGPathAddLines");
    p.def<&path_addPath>("_CGPathAddPath");
    p.def<&path_close>("_CGPathCloseSubpath");
    p.def<&path_isEmpty>("_CGPathIsEmpty");
    p.def<&path_isRect>("_CGPathIsRect");
    p.def<&path_bbox>("_CGPathGetBoundingBox");
    p.def<&path_bbox>("_CGPathGetPathBoundingBox");
    p.def<&path_current>("_CGPathGetCurrentPoint");
    p.def<&path_contains>("_CGPathContainsPoint");
    p.def<&path_equal>("_CGPathEqualToPath");
    p.def<&path_strokeCopy>("_CGPathCreateCopyByStrokingPath");
    p.def<&path_apply>("_CGPathApply");

    // gradients
    p.def<&grad_withComponents>("_CGGradientCreateWithColorComponents");
    p.def<&grad_withColors>("_CGGradientCreateWithColors");

    // contexts
    p.def<&ctx_save>("_CGContextSaveGState");
    p.def<&ctx_restore>("_CGContextRestoreGState");
    p.def<&ctx_translate>("_CGContextTranslateCTM");
    p.def<&ctx_scale>("_CGContextScaleCTM");
    p.def<&ctx_rotate>("_CGContextRotateCTM");
    p.def<&ctx_concat>("_CGContextConcatCTM");
    p.def<&ctx_getCTM>("_CGContextGetCTM");
    p.def<&ctx_getUserToDevice>("_CGContextGetUserSpaceToDeviceSpaceTransform");
    p.def<&ctx_setRGBFill>("_CGContextSetRGBFillColor");
    p.def<&ctx_setRGBStroke>("_CGContextSetRGBStrokeColor");
    p.def<&ctx_setGrayFill>("_CGContextSetGrayFillColor");
    p.def<&ctx_setGrayStroke>("_CGContextSetGrayStrokeColor");
    p.def<&ctx_setFillColorWithColor>("_CGContextSetFillColorWithColor");
    p.def<&ctx_setStrokeColorWithColor>("_CGContextSetStrokeColorWithColor");
    p.def<&ctx_setFillColor>("_CGContextSetFillColor");
    p.def<&ctx_setStrokeColor>("_CGContextSetStrokeColor");
    p.def<&ctx_setCMYKFill>("_CGContextSetCMYKFillColor");
    p.def<&ctx_ignore1>("_CGContextSetFillColorSpace");
    p.def<&ctx_ignore1>("_CGContextSetStrokeColorSpace");
    p.def<&ctx_ignore1>("_CGContextSetRenderingIntent");
    p.def<&ctx_ignore1>("_CGContextSetShouldSmoothFonts");
    p.def<&ctx_ignore1>("_CGContextSetAllowsFontSmoothing");
    p.def<&ctx_fontPosition>("_CGContextSetShouldSubpixelPositionFonts");
    p.def<&ctx_fontQuantize>("_CGContextSetShouldSubpixelQuantizeFonts");
    p.def<&ctx_allowsFontPosition>("_CGContextSetAllowsFontSubpixelPositioning");
    p.def<&ctx_allowsFontQuantize>("_CGContextSetAllowsFontSubpixelQuantization");
    p.def<&ctx_setAlpha>("_CGContextSetAlpha");
    p.def<&ctx_setLineWidth>("_CGContextSetLineWidth");
    p.def<&ctx_setLineCap>("_CGContextSetLineCap");
    p.def<&ctx_setLineJoin>("_CGContextSetLineJoin");
    p.def<&ctx_setMiter>("_CGContextSetMiterLimit");
    p.def<&ctx_setFlatness>("_CGContextSetFlatness");
    p.def<&ctx_setLineDash>("_CGContextSetLineDash");
    p.def<&ctx_setBlend>("_CGContextSetBlendMode");
    p.def<&ctx_setAA>("_CGContextSetShouldAntialias");
    p.def<&ctx_setAA>("_CGContextSetAllowsAntialiasing");
    p.def<&ctx_setInterp>("_CGContextSetInterpolationQuality");
    p.def<&ctx_getInterp>("_CGContextGetInterpolationQuality");
    p.def<&ctx_setShadow>("_CGContextSetShadow");
    p.def<&ctx_setShadowColor>("_CGContextSetShadowWithColor");
    p.def<&ctx_beginPath>("_CGContextBeginPath");
    p.def<&ctx_moveTo>("_CGContextMoveToPoint");
    p.def<&ctx_lineTo>("_CGContextAddLineToPoint");
    p.def<&ctx_curveTo>("_CGContextAddCurveToPoint");
    p.def<&ctx_quadTo>("_CGContextAddQuadCurveToPoint");
    p.def<&ctx_arc>("_CGContextAddArc");
    p.def<&ctx_arcTo>("_CGContextAddArcToPoint");
    p.def<&ctx_addRect>("_CGContextAddRect");
    p.def<&ctx_addRects>("_CGContextAddRects");
    p.def<&ctx_addEllipse>("_CGContextAddEllipseInRect");
    p.def<&ctx_addLines>("_CGContextAddLines");
    p.def<&ctx_addPath>("_CGContextAddPath");
    p.def<&ctx_closePath>("_CGContextClosePath");
    p.def<&ctx_isPathEmpty>("_CGContextIsPathEmpty");
    p.def<&ctx_pathCurrent>("_CGContextGetPathCurrentPoint");
    p.def<&ctx_pathBBox>("_CGContextGetPathBoundingBox");
    p.def<&ctx_pathContains>("_CGContextPathContainsPoint");
    p.def<&ctx_fillPath>("_CGContextFillPath");
    p.def<&ctx_eoFillPath>("_CGContextEOFillPath");
    p.def<&ctx_strokePath>("_CGContextStrokePath");
    p.def<&ctx_drawPath>("_CGContextDrawPath");
    p.def<&ctx_replaceWithStroked>("_CGContextReplacePathWithStrokedPath");
    p.def<&ctx_fillRect>("_CGContextFillRect");
    p.def<&ctx_fillRects>("_CGContextFillRects");
    p.def<&ctx_strokeRect>("_CGContextStrokeRect");
    p.def<&ctx_strokeRectWidth>("_CGContextStrokeRectWithWidth");
    p.def<&ctx_clearRect>("_CGContextClearRect");
    p.def<&ctx_fillEllipse>("_CGContextFillEllipseInRect");
    p.def<&ctx_strokeEllipse>("_CGContextStrokeEllipseInRect");
    p.def<&ctx_strokeSegments>("_CGContextStrokeLineSegments");
    p.def<&ctx_clip>("_CGContextClip");
    p.def<&ctx_eoClip>("_CGContextEOClip");
    p.def<&ctx_clipToRect>("_CGContextClipToRect");
    p.def<&ctx_clipToRects>("_CGContextClipToRects");
    p.def<&ctx_clipToMask>("_CGContextClipToMask");
    p.def<&ctx_clipBBox>("_CGContextGetClipBoundingBox");
    p.def<&ctx_drawImage>("_CGContextDrawImage");
    p.def<&ctx_drawTiled>("_CGContextDrawTiledImage");
    p.def<&ctx_linearGradient>("_CGContextDrawLinearGradient");
    p.def<&ctx_radialGradient>("_CGContextDrawRadialGradient");
    p.def<&ctx_selectFont>("_CGContextSelectFont");
    p.def<&ctx_setFontSize>("_CGContextSetFontSize");
    p.def<&ctx_setFont>("_CGContextSetFont");
    p.def<&ctx_setCharSpacing>("_CGContextSetCharacterSpacing");
    p.def<&ctx_setTextMode>("_CGContextSetTextDrawingMode");
    p.def<&ctx_setTextMatrix>("_CGContextSetTextMatrix");
    p.def<&ctx_getTextMatrix>("_CGContextGetTextMatrix");
    p.def<&ctx_setTextPos>("_CGContextSetTextPosition");
    p.def<&ctx_getTextPos>("_CGContextGetTextPosition");
    p.def<&ctx_showTextAt>("_CGContextShowTextAtPoint");
    p.def<&ctx_showText>("_CGContextShowText");
    p.def<&ctx_showGlyphs>("_CGContextShowGlyphs");
    p.def<&ctx_showGlyphs>("_CGContextShowGlyphsAtPoint");
    p.def<&font_createWithName>("_CGFontCreateWithFontName");
    p.def<&font_createWithProvider>("_CGFontCreateWithDataProvider");
    p.def<&font_copyName>("_CGFontCopyFullName");
    p.def<&font_copyName>("_CGFontCopyPostScriptName");
    p.def<&font_getUnitsPerEm>("_CGFontGetUnitsPerEm");
    p.def<&font_getAscent>("_CGFontGetAscent");
    p.def<&font_getDescent>("_CGFontGetDescent");
    p.def<&font_getCapHeight>("_CGFontGetCapHeight");
    p.def<&font_getXHeight>("_CGFontGetXHeight");
    p.def<&font_getGlyphAdvances>("_CGFontGetGlyphAdvances");
    p.def<&font_copyTableForTag>("_CGFontCopyTableForTag");
    p.def<&ct_createGraphics>("_CTFontCreateWithGraphicsFont");
    p.def<&ct_createName>("_CTFontCreateWithName");
    p.def<&ct_copyDescriptor>("_CTFontCopyFontDescriptor");
    p.def<&ct_createDescriptor>("_CTFontCreateWithFontDescriptor");
    p.def<&ct_family>("_CTFontCopyFamilyName");
    p.def<&ct_traits>("_CTFontGetSymbolicTraits");
    p.def<&ct_underline>("_CTFontGetUnderlineThickness");
    p.def<&ct_metric<0>>("_CTFontGetAscent");
    p.def<&ct_metric<1>>("_CTFontGetDescent");
    p.def<&ct_metric<2>>("_CTFontGetLeading");
    p.def<&ct_characters>("_CTFontGetGlyphsForCharacters");
    p.def<&ct_advances>("_CTFontGetAdvancesForGlyphs");
    p.def<&ct_bounds>("_CTFontGetBoundingRectsForGlyphs");
    p.def<&ct_draw>("_CTFontDrawGlyphs");
    p.def<&attributed_create>("_CFAttributedStringCreate");
    p.def<&text_retain>("_CTTypesetterCreateWithAttributedString");
    p.def<&text_break>("_CTTypesetterSuggestLineBreak");
    p.def<&text_line>("_CTTypesetterCreateLine");
    p.def<&text_wholeLine>("_CTLineCreateWithAttributedString");
    p.def<&text_bounds>("_CTLineGetTypographicBounds");
    p.def<&text_range>("_CTLineGetStringRange");
    p.def<&text_runs>("_CTLineGetGlyphRuns");
    p.def<&text_draw>("_CTLineDraw");
    p.def<&run_count>("_CTRunGetGlyphCount");
    p.def<&run_attrs>("_CTRunGetAttributes");
    p.def<&run_copy<0>>("_CTRunGetGlyphs");
    p.def<&run_copy<1>>("_CTRunGetPositions");
    p.def<&run_copy<2>>("_CTRunGetAdvances");
    p.def<&run_copy<3>>("_CTRunGetStringIndices");
    p.def<&run_status>("_CTRunGetStatus");
    p.def<&text_range>("_CTRunGetStringRange");
    p.def<&run_bounds>("_CTRunGetTypographicBounds");
    p.def<&ctx_noop>("_CGContextFlush");
    p.def<&ctx_noop>("_CGContextSynchronize");
    p.def<&ctx_noop>("_CGContextBeginTransparencyLayer");
    p.def<&ctx_noop>("_CGContextEndTransparencyLayer");
    p.def<&ctx_convertPointToDevice>("_CGContextConvertPointToDeviceSpace");
    p.def<&ctx_convertPointToUser>("_CGContextConvertPointToUserSpace");
    p.def<&ctx_convertRectToDevice>("_CGContextConvertRectToDeviceSpace");
    p.def<&bm_create>("_CGBitmapContextCreate");
    p.def<&bm_createWithCallback>("_CGBitmapContextCreateWithData");
    p.def<&bm_data>("_CGBitmapContextGetData");
    p.def<&bm_width>("_CGBitmapContextGetWidth");
    p.def<&bm_height>("_CGBitmapContextGetHeight");
    p.def<&bm_bpr>("_CGBitmapContextGetBytesPerRow");
    p.def<&bm_bpc>("_CGBitmapContextGetBitsPerComponent");
    p.def<&bm_bpp>("_CGBitmapContextGetBitsPerPixel");
    p.def<&bm_info>("_CGBitmapContextGetBitmapInfo");
    p.def<&bm_alphaInfo>("_CGBitmapContextGetAlphaInfo");
    p.def<&bm_space>("_CGBitmapContextGetColorSpace");
    p.def<&bm_createImage>("_CGBitmapContextCreateImage");
}

} // namespace Hydrogen::Runtime::CG
