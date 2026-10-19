// HydrogenVM — UIKit graphics: UIColor, UIFont, UIImage, UIGraphics* image
// contexts, NSString drawing / measuring, geometry <-> string conversions and
// the NSValue geometry additions.
#include "UIKit.hpp"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace Hydrogen::Runtime::UIKit {

using Foundation::autoreleased;
using Foundation::makeString;
using Foundation::utf8;

namespace {

// ================================================================ UIColor ===

struct ColorObj : ObjC::HostObject {
    G::Color c{0, 0, 0, 1};
    G::ImagePtr pattern;
    float patternScale = 1;
    id cg = 0;          // retained CGColor
    Process* proc = nullptr;
};
ColorObj* co(Ctx& x, id c) { return c ? rt(x).hostIf<ColorObj>(c) : nullptr; }
ColorObj& coInit(Ctx& x, id self) {
    auto& h = rt(x).host<ColorObj>(self);
    h.proc = &x.proc;
    return h;
}
id newColor(Ctx& x, Class cls, G::Color c) {
    const id o = rt(x).createInstance(cls ? cls : ui(x).color);
    coInit(x, o).c = c;
    return o;
}
G::Color hsb(float h, float s, float v, float a) {
    h = h - std::floor(h);
    const float i = std::floor(h * 6), f = h * 6 - i, p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
    switch (int(i) % 6) {
        case 0: return {v, t, p, a};
        case 1: return {q, v, p, a};
        case 2: return {p, v, t, a};
        case 3: return {p, q, v, a};
        case 4: return {t, p, v, a};
        default: return {v, p, q, a};
    }
}
template <int R, int Gc, int B, int A> id cc_std(Ctx& x, id self, SEL) { return autoreleased(x, newColor(x, self, {R / 1000.0f, Gc / 1000.0f, B / 1000.0f, A / 1000.0f})); }
id cc_rgba(Ctx& x, id self, SEL, float r, float g, float b, float a) { return autoreleased(x, newColor(x, self, {r, g, b, a})); }
id cc_white(Ctx& x, id self, SEL, float w, float a) { return autoreleased(x, newColor(x, self, {w, w, w, a})); }
id cc_hsb(Ctx& x, id self, SEL, float h, float s, float v, float a) { return autoreleased(x, newColor(x, self, hsb(h, s, v, a))); }
id cc_cg(Ctx& x, id self, SEL, id cg) { return autoreleased(x, newColor(x, self, CG::colorOf(x, cg))); }
id cc_pattern(Ctx& x, id self, SEL, id image) {
    const id c = newColor(x, self, {0.5f, 0.5f, 0.5f, 1});
    if (ImgData* d = uiImage(x, image)) { co(x, c)->pattern = d->image; co(x, c)->patternScale = d->scale; }
    return autoreleased(x, c);
}
id ci_rgba(Ctx& x, id self, SEL, float r, float g, float b, float a) { coInit(x, self).c = {r, g, b, a}; return self; }
id ci_white(Ctx& x, id self, SEL, float w, float a) { coInit(x, self).c = {w, w, w, a}; return self; }
id ci_hsb(Ctx& x, id self, SEL, float h, float s, float v, float a) { coInit(x, self).c = hsb(h, s, v, a); return self; }
id ci_cg(Ctx& x, id self, SEL, id cg) { coInit(x, self).c = CG::colorOf(x, cg); return self; }
id ci_pattern(Ctx& x, id self, SEL, id image) {
    ColorObj& h = coInit(x, self);
    if (ImgData* d = uiImage(x, image)) { h.pattern = d->image; h.patternScale = d->scale; }
    return self;
}
id ci_coder(Ctx& x, id self, SEL, id coder) {
    auto& r = rt(x);
    auto has = [&](const char* k) { return (uint32_t(r.send(x.cpu, coder, "containsValueForKey:", {makeString(x, k)})) & 0xFF) != 0; };
    auto f = [&](const char* k) { return bitsf(uint32_t(r.send(x.cpu, coder, "decodeFloatForKey:", {makeString(x, k)}))); };
    ColorObj& h = coInit(x, self);
    if (has("UISystemColorName")) {
        const id name = id(r.send(x.cpu, coder, "decodeObjectForKey:", {makeString(x, "UISystemColorName")}));
        const id sys = name ? id(r.send(x.cpu, ui(x).color, r.sel(utf8(x, name)))) : 0;
        if (ColorObj* s = co(x, sys)) { h.c = s->c; return self; }
    }
    if (has("UIRed") || has("UIGreen") || has("UIBlue")) h.c = {f("UIRed"), f("UIGreen"), f("UIBlue"), has("UIAlpha") ? f("UIAlpha") : 1.0f};
    else if (has("UIWhite")) { const float w = f("UIWhite"); h.c = {w, w, w, has("UIAlpha") ? f("UIAlpha") : 1.0f}; }
    else if (has("NSRGB")) {   // AppKit-style "r g b a" ascii data
        const id d = id(r.send(x.cpu, coder, "decodeObjectForKey:", {makeString(x, "NSRGB")}));
        const auto b = Foundation::dataBytes(x, d);
        float c[4] = {0, 0, 0, 1};
        std::sscanf(std::string(b.begin(), b.end()).c_str(), "%f %f %f %f", &c[0], &c[1], &c[2], &c[3]);
        h.c = {c[0], c[1], c[2], c[3]};
    }
    return self;
}
id c_CGColor(Ctx& x, id self, SEL) {
    ColorObj* h = co(x, self);
    if (!h) return 0;
    if (!h->cg) h->cg = CG::createColor(x, h->c);
    return h->cg;
}
void c_set(Ctx& x, id self, SEL) {
    const id ctx = CG::currentContext(x);
    if (CG::ContextData* d = CG::context(x, ctx)) { d->gs().fill = uiColor(x, self); d->gs().stroke = uiColor(x, self); }
}
void c_setFill(Ctx& x, id self, SEL) { if (CG::ContextData* d = CG::context(x, CG::currentContext(x))) d->gs().fill = uiColor(x, self); }
void c_setStroke(Ctx& x, id self, SEL) { if (CG::ContextData* d = CG::context(x, CG::currentContext(x))) d->gs().stroke = uiColor(x, self); }
id c_withAlpha(Ctx& x, id self, SEL, float a) {
    G::Color c = uiColor(x, self);
    c.a = a;
    const id n = newColor(x, ui(x).color, c);
    if (ColorObj* s = co(x, self); s && s->pattern) { co(x, n)->pattern = s->pattern; co(x, n)->patternScale = s->patternScale; }
    return autoreleased(x, n);
}
bool c_getRGBA(Ctx& x, id self, SEL, GuestPtr r, GuestPtr g, GuestPtr b, GuestPtr a) {
    const G::Color c = uiColor(x, self);
    if (r) x.mem.write<float>(r, c.r);
    if (g) x.mem.write<float>(g, c.g);
    if (b) x.mem.write<float>(b, c.b);
    if (a) x.mem.write<float>(a, c.a);
    return true;
}
bool c_getWhite(Ctx& x, id self, SEL, GuestPtr w, GuestPtr a) {
    const G::Color c = uiColor(x, self);
    if (w) x.mem.write<float>(w, 0.299f * c.r + 0.587f * c.g + 0.114f * c.b);
    if (a) x.mem.write<float>(a, c.a);
    return true;
}
bool c_getHSB(Ctx& x, id self, SEL, GuestPtr hp, GuestPtr sp, GuestPtr bp, GuestPtr ap) {
    const G::Color c = uiColor(x, self);
    const float mx = std::max({c.r, c.g, c.b}), mn = std::min({c.r, c.g, c.b}), d = mx - mn;
    float h = 0;
    if (d > 0) {
        if (mx == c.r) h = std::fmod((c.g - c.b) / d, 6.0f);
        else if (mx == c.g) h = (c.b - c.r) / d + 2;
        else h = (c.r - c.g) / d + 4;
        h /= 6;
        if (h < 0) h += 1;
    }
    if (hp) x.mem.write<float>(hp, h);
    if (sp) x.mem.write<float>(sp, mx > 0 ? d / mx : 0);
    if (bp) x.mem.write<float>(bp, mx);
    if (ap) x.mem.write<float>(ap, c.a);
    return true;
}
bool c_isEqual(Ctx& x, id self, SEL, id o) {
    if (o == self) return true;
    if (!o || !co(x, o)) return false;
    const G::Color a = uiColor(x, self), b = uiColor(x, o);
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}
uint32_t c_hash(Ctx& x, id self, SEL) { const G::Color c = uiColor(x, self); return uint32_t(c.r * 255) << 24 | uint32_t(c.g * 255) << 16 | uint32_t(c.b * 255) << 8 | uint32_t(c.a * 255); }
id c_description(Ctx& x, id self, SEL) {
    const G::Color c = uiColor(x, self);
    char b[96];
    std::snprintf(b, sizeof b, "UIDeviceRGBColorSpace %g %g %g %g", c.r, c.g, c.b, c.a);
    return makeString(x, std::string(b));
}
id c_copy(Ctx& x, id self, SEL, uint32_t) { return rt(x).retain(x.cpu, self); }
void c_dealloc(Ctx& x, id self, SEL) {
    if (ColorObj* h = co(x, self)) { const id cg = h->cg; h->cg = 0; if (cg) rt(x).release(x.cpu, cg); }
    rt(x).disposeInstance(x.cpu, self);
}

// ================================================================= UIFont ===

struct FontObj : ObjC::HostObject { G::FontDesc f; };
FontObj* fo(Ctx& x, id f) { return f ? rt(x).hostIf<FontObj>(f) : nullptr; }
id newFont(Ctx& x, const G::FontDesc& d) {
    const id o = rt(x).createInstance(ui(x).font);
    auto h = std::make_unique<FontObj>();
    h->f = d;
    rt(x).setHost(o, std::move(h));
    return o;
}
G::FontDesc desc(const std::string& name, float size) {
    G::FontDesc d;
    d.name = name;
    d.size = size > 0 ? size : 17;
    d.bold = name.find("Bold") != std::string::npos || name.find("Heavy") != std::string::npos || name.find("Black") != std::string::npos;
    d.italic = name.find("Italic") != std::string::npos || name.find("Oblique") != std::string::npos;
    return d;
}
id fc_named(Ctx& x, id, SEL, id name, float size) { return autoreleased(x, newFont(x, desc(name ? utf8(x, name) : "Helvetica", size))); }
id fc_system(Ctx& x, id, SEL, float size) { return autoreleased(x, newFont(x, desc("Helvetica", size))); }
id fc_bold(Ctx& x, id, SEL, float size) { return autoreleased(x, newFont(x, desc("Helvetica-Bold", size))); }
id fc_italic(Ctx& x, id, SEL, float size) { return autoreleased(x, newFont(x, desc("Helvetica-Oblique", size))); }
id fc_systemWeight(Ctx& x, id, SEL, float size, float weight) { return autoreleased(x, newFont(x, desc(weight >= 0.3f ? "Helvetica-Bold" : "Helvetica", size))); }
float fc_labelSize(Ctx&, id, SEL) { return 17; }
float fc_buttonSize(Ctx&, id, SEL) { return 18; }
float fc_smallSize(Ctx&, id, SEL) { return 12; }
float fc_systemSize(Ctx&, id, SEL) { return 14; }
id fc_familyNames(Ctx& x, id, SEL) {
    std::vector<id> v;
    for (const char* n : {"Helvetica", "Helvetica Neue", "Arial", "Courier", "Courier New", "Georgia", "Marker Felt", "Times New Roman",
                          "Trebuchet MS", "Verdana", "American Typewriter", "Arial Rounded MT Bold", "Chalkboard SE", "Futura", "Noteworthy",
                          "Zapfino"})
        v.push_back(makeString(x, n));
    return Foundation::makeArray(x, v);
}
id fc_namesForFamily(Ctx& x, id, SEL, id fam) {
    const std::string f = fam ? utf8(x, fam) : "";
    std::string base = f;
    base.erase(std::remove(base.begin(), base.end(), ' '), base.end());
    return Foundation::makeArray(x, {makeString(x, base), makeString(x, base + "-Bold")});
}
id fc_preferred(Ctx& x, id, SEL, id) { return autoreleased(x, newFont(x, desc("Helvetica", 17))); }
float f_pointSize(Ctx& x, id self, SEL) { FontObj* h = fo(x, self); return h ? h->f.size : 17; }
id f_fontName(Ctx& x, id self, SEL) { FontObj* h = fo(x, self); return makeString(x, h ? h->f.name : "Helvetica"); }
id f_familyName(Ctx& x, id self, SEL) {
    FontObj* h = fo(x, self);
    std::string n = h ? h->f.name : "Helvetica";
    const size_t dash = n.find('-');
    return makeString(x, dash == std::string::npos ? n : n.substr(0, dash));
}
G::TextLayout metrics(Ctx& x, id self) { FontObj* h = fo(x, self); return G::textBackend().measure("Ag", h ? h->f : desc("Helvetica", 17)); }
float f_lineHeight(Ctx& x, id self, SEL) { return metrics(x, self).lineHeight; }
float f_ascender(Ctx& x, id self, SEL) { return metrics(x, self).ascent; }
float f_descender(Ctx& x, id self, SEL) { return -metrics(x, self).descent; }
float f_leading(Ctx& x, id self, SEL) { const auto m = metrics(x, self); return std::max(0.0f, m.lineHeight - m.ascent - m.descent); }
float f_capHeight(Ctx& x, id self, SEL) { return metrics(x, self).ascent * 0.7f; }
float f_xHeight(Ctx& x, id self, SEL) { return metrics(x, self).ascent * 0.5f; }
id f_withSize(Ctx& x, id self, SEL, float size) { FontObj* h = fo(x, self); G::FontDesc d = h ? h->f : desc("Helvetica", 17); d.size = size; return autoreleased(x, newFont(x, d)); }
id f_copy(Ctx& x, id self, SEL, uint32_t) { return rt(x).retain(x.cpu, self); }
id f_description(Ctx& x, id self, SEL) {
    FontObj* h = fo(x, self);
    char b[160];
    std::snprintf(b, sizeof b, "<UIFont: 0x%x> font-family: \"%s\"; font-size: %gpt", self, h ? h->f.name.c_str() : "?", h ? h->f.size : 0);
    return makeString(x, std::string(b));
}
id f_initWithCoder(Ctx& x, id self, SEL, id coder) {
    auto& r = rt(x);
    const id name = id(r.send(x.cpu, coder, "decodeObjectForKey:", {makeString(x, "UIFontName")}));
    float size = bitsf(uint32_t(r.send(x.cpu, coder, "decodeFloatForKey:", {makeString(x, "UIFontPointSize")})));
    if (size <= 0) size = 17;
    auto h = std::make_unique<FontObj>();
    h->f = desc(name ? utf8(x, name) : "Helvetica", size);
    if ((uint32_t(r.send(x.cpu, coder, "decodeBoolForKey:", {makeString(x, "UISystemFont")})) & 0xFF) && h->f.name.empty()) h->f.name = "Helvetica";
    r.setHost(self, std::move(h));
    return self;
}

// ================================================================ UIImage ===

std::vector<uint8_t> readFile(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}
G::ImagePtr loadFile(Ctx& x, const std::string& path) {
    const auto b = readFile(path);
    if (b.empty()) return nullptr;
    std::string err;
    auto img = G::decodeImage(b.data(), b.size(), &err);
    if (!img) x.proc.logf(LogLevel::Warning, "UIImage: can't decode %s: %s", path.c_str(), err.c_str());
    return img;
}
// Prefer name@2x on a retina screen (UIKit does this for imageNamed: and imageWithContentsOfFile:).
std::string retinaVariant(const std::string& path, float screenScale) {
    if (screenScale < 2) return path;
    const fs::path p(path);
    const std::string stem = p.stem().string();
    if (stem.find("@2x") != std::string::npos) return path;
    std::error_code ec;
    for (const std::string& suffix : {std::string("@2x~iphone"), std::string("@2x")}) {
        std::string s = stem;
        std::string dev;
        if (const size_t t = s.find("~iphone"); t != std::string::npos) { s.erase(t); }
        const fs::path cand = p.parent_path() / (s + suffix + p.extension().string());
        if (fs::exists(cand, ec)) return cand.string();
    }
    return path;
}
float scaleOfPath(const std::string& path) { return fs::path(path).stem().string().find("@2x") != std::string::npos ? 2.0f : 1.0f; }

id newImageObj(Ctx& x, Class cls, G::ImagePtr img, float scale) {
    const id o = rt(x).createInstance(cls ? cls : ui(x).image);
    auto h = std::make_unique<ImgData>();
    h->image = std::move(img);
    h->scale = scale > 0 ? scale : 1;
    rt(x).setHost(o, std::move(h));
    return o;
}
id imgc_named(Ctx& x, id self, SEL, id nameObj) {
    if (!nameObj) return 0;
    std::string name = utf8(x, nameObj);
    auto& u = ui(x);
    if (auto it = u.imageCache.find(name); it != u.imageCache.end()) return it->second;
    std::string ext = fs::path(name).extension().string();
    std::string base = name;
    if (!ext.empty() && ext.size() <= 5) base = name.substr(0, name.size() - ext.size()), ext = ext.substr(1);
    else ext = "png";
    std::string path;
    float scale = 1;
    if (u.scale >= 2) {
        path = Foundation::bundleResource(x, base + "@2x", ext);
        if (!path.empty()) scale = 2;
    }
    if (path.empty()) path = Foundation::bundleResource(x, base, ext);
    if (path.empty() && ext == "png") {
        for (const char* e : {"jpg", "jpeg", "gif"}) if (!(path = Foundation::bundleResource(x, base, e)).empty()) break;
    }
    if (path.empty()) return 0;
    if (scale == 1) scale = scaleOfPath(path);
    auto img = loadFile(x, path);
    if (!img) return 0;
    const id o = newImageObj(x, self, img, scale);
    rt(x).makeImmortal(o);
    u.imageCache[name] = o;
    return o;
}
id imgc_file(Ctx& x, id self, SEL, id pathObj) {
    if (!pathObj) return 0;
    const std::string path = retinaVariant(utf8(x, pathObj), ui(x).scale);
    auto img = loadFile(x, path);
    return img ? autoreleased(x, newImageObj(x, self, img, scaleOfPath(path))) : 0;
}
id imgc_data(Ctx& x, id self, SEL, id data) {
    if (!data) return 0;
    const auto b = Foundation::dataBytes(x, data);
    auto img = G::decodeImage(b.data(), b.size());
    return img ? autoreleased(x, newImageObj(x, self, img, 1)) : 0;
}
id imgc_dataScale(Ctx& x, id self, SEL s, id data, float scale) {
    const id i = imgc_data(x, self, s, data);
    if (ImgData* d = uiImage(x, i)) d->scale = scale > 0 ? scale : 1;
    return i;
}
id imgc_cg(Ctx& x, id self, SEL, id cg) {
    auto img = CG::imageOf(x, cg);
    if (!img) return 0;
    const id o = newImageObj(x, self, img, 1);
    uiImage(x, o)->cgImage = rt(x).retain(x.cpu, cg);
    return autoreleased(x, o);
}
id imgc_cgScale(Ctx& x, id self, SEL s, id cg, float scale, int32_t orient) {
    const id o = imgc_cg(x, self, s, cg);
    if (ImgData* d = uiImage(x, o)) { d->scale = scale > 0 ? scale : 1; d->orientation = orient; }
    return o;
}
id imgc_animated(Ctx& x, id self, SEL, id images, double duration) {
    const auto frames = Foundation::arrayItems(x, images);
    if (frames.empty()) return 0;
    ImgData* first = uiImage(x, frames[0]);
    const id o = newImageObj(x, self, first ? first->image : nullptr, first ? first->scale : 1);
    ImgData* d = uiImage(x, o);
    for (id f : frames) d->frames.push_back(rt(x).retain(x.cpu, f));
    d->duration = duration;
    return autoreleased(x, o);
}
// init variants: replace self (class clusters may do this)
id imgi_file(Ctx& x, id self, SEL s, id path) {
    const id o = imgc_file(x, rt(x).classOf(self), s, path);
    rt(x).disposeInstance(x.cpu, self);
    return o ? rt(x).retain(x.cpu, o) : 0;
}
id imgi_data(Ctx& x, id self, SEL s, id data) {
    const id o = imgc_data(x, rt(x).classOf(self), s, data);
    rt(x).disposeInstance(x.cpu, self);
    return o ? rt(x).retain(x.cpu, o) : 0;
}
id imgi_cg(Ctx& x, id self, SEL s, id cg) {
    const id o = imgc_cg(x, rt(x).classOf(self), s, cg);
    rt(x).disposeInstance(x.cpu, self);
    return o ? rt(x).retain(x.cpu, o) : 0;
}
id imgi_cgScale(Ctx& x, id self, SEL s, id cg, float scale, int32_t orient) {
    const id o = imgc_cgScale(x, rt(x).classOf(self), s, cg, scale, orient);
    rt(x).disposeInstance(x.cpu, self);
    return o ? rt(x).retain(x.cpu, o) : 0;
}
void img_size(Ctx& x, GuestPtr ret, id self, SEL) { const G::Size s = imageSize(x, self); CG::writeStruct(x, ret, CGSize{s.w, s.h}); }
float img_scale(Ctx& x, id self, SEL) { ImgData* d = uiImage(x, self); return d ? d->scale : 1; }
int32_t img_orientation(Ctx& x, id self, SEL) { ImgData* d = uiImage(x, self); return d ? d->orientation : 0; }
id img_CGImage(Ctx& x, id self, SEL) { return imageCGImage(x, self); }
void img_drawAtPoint(Ctx& x, id self, SEL, CGPoint p) {
    const G::Size s = imageSize(x, self);
    drawUIImage(x, CG::currentContext(x), self, {p.x, p.y, s.w, s.h}, 1);
}
void img_drawAtPointBlend(Ctx& x, id self, SEL, CGPoint p, int32_t mode, float alpha) {
    const G::Size s = imageSize(x, self);
    drawUIImage(x, CG::currentContext(x), self, {p.x, p.y, s.w, s.h}, alpha, mode == 0 ? G::BlendMode::Normal : mode == 1 ? G::BlendMode::Multiply : G::BlendMode::Normal);
}
void img_drawInRect(Ctx& x, id self, SEL, CGRect r) { drawUIImage(x, CG::currentContext(x), self, CG::toG(r), 1); }
void img_drawInRectBlend(Ctx& x, id self, SEL, CGRect r, int32_t mode, float alpha) {
    G::BlendMode m = G::BlendMode::Normal;
    switch (mode) { case 1: m = G::BlendMode::Multiply; break; case 2: m = G::BlendMode::Screen; break; case 17: m = G::BlendMode::Copy; break; default: break; }
    drawUIImage(x, CG::currentContext(x), self, CG::toG(r), alpha, m);
}
void img_drawPattern(Ctx& x, id self, SEL, CGRect r) {
    const G::Size s = imageSize(x, self);
    const id ctx = CG::currentContext(x);
    if (s.w <= 0 || s.h <= 0 || !ctx) return;
    CG::ContextData* d = CG::context(x, ctx);
    d->stack.push_back(d->gs());
    G::Path p;
    p.addRect(CG::toG(r));
    d->gs().clip = G::intersectClip(d->gs().clip, p.transformed(d->gs().ctm), false);
    for (float yy = r.origin.y; yy < r.origin.y + r.size.height; yy += s.h)
        for (float xx = r.origin.x; xx < r.origin.x + r.size.width; xx += s.w) drawUIImage(x, ctx, self, {xx, yy, s.w, s.h}, 1);
    d->stack.pop_back();
}
id img_stretchable(Ctx& x, id self, SEL, int32_t left, int32_t top) {
    ImgData* d = uiImage(x, self);
    if (!d) return self;
    const id o = newImageObj(x, ui(x).image, d->image, d->scale);
    ImgData* n = uiImage(x, o);
    const G::Size s = imageSize(x, self);
    // Apple: the middle is 1 point wide/high; 0 means "don't stretch that axis".
    n->caps = {float(top), float(left), top ? s.h - float(top) - 1 : 0, left ? s.w - float(left) - 1 : 0};
    n->resizable = left || top;
    return autoreleased(x, o);
}
id img_resizable(Ctx& x, id self, SEL, UIEdgeInsets caps) {
    ImgData* d = uiImage(x, self);
    if (!d) return self;
    const id o = newImageObj(x, ui(x).image, d->image, d->scale);
    ImgData* n = uiImage(x, o);
    n->caps = caps;
    n->resizable = true;
    return autoreleased(x, o);
}
id img_resizableMode(Ctx& x, id self, SEL s, UIEdgeInsets caps, int32_t) { return img_resizable(x, self, s, caps); }
void img_capInsets(Ctx& x, GuestPtr ret, id self, SEL) { ImgData* d = uiImage(x, self); CG::writeStruct(x, ret, d ? d->caps : UIEdgeInsets{0, 0, 0, 0}); }
int32_t img_leftCap(Ctx& x, id self, SEL) { ImgData* d = uiImage(x, self); return d ? int32_t(d->caps.left) : 0; }
int32_t img_topCap(Ctx& x, id self, SEL) { ImgData* d = uiImage(x, self); return d ? int32_t(d->caps.top) : 0; }
id img_withRendering(Ctx& x, id self, SEL, int32_t mode) {
    ImgData* d = uiImage(x, self);
    if (!d) return self;
    const id o = newImageObj(x, ui(x).image, d->image, d->scale);
    uiImage(x, o)->caps = d->caps;
    uiImage(x, o)->resizable = d->resizable;
    uiImage(x, o)->renderingMode = mode;
    return autoreleased(x, o);
}
id img_images(Ctx& x, id self, SEL) { ImgData* d = uiImage(x, self); return d && !d->frames.empty() ? Foundation::makeArray(x, d->frames) : 0; }
double img_duration(Ctx& x, id self, SEL) { ImgData* d = uiImage(x, self); return d ? d->duration : 0; }
id img_copy(Ctx& x, id self, SEL, uint32_t) { return rt(x).retain(x.cpu, self); }
id img_description(Ctx& x, id self, SEL) {
    const G::Size s = imageSize(x, self);
    char b[96];
    std::snprintf(b, sizeof b, "<UIImage: 0x%x> {%g, %g}", self, s.w, s.h);
    return makeString(x, std::string(b));
}
void img_dealloc(Ctx& x, id self, SEL) {
    if (ImgData* d = uiImage(x, self)) {
        const id cg = d->cgImage;
        d->cgImage = 0;
        if (cg) rt(x).release(x.cpu, cg);
        auto frames = std::move(d->frames);
        d->frames.clear();
        for (id f : frames) rt(x).release(x.cpu, f);
    }
    rt(x).disposeInstance(x.cpu, self);
}
id f_pngRep(Ctx& x, id img) {
    ImgData* d = uiImage(x, img);
    if (!d || !d->image) return 0;
    const auto png = G::encodePng(*d->image);
    return Foundation::makeData(x, png.data(), uint32_t(png.size()));
}
id f_jpegRep(Ctx& x, id img, float) {
    x.proc.reportMissing("UIImageJPEGRepresentation (writes PNG data instead)");
    return f_pngRep(x, img);
}
void f_writeToAlbum(Ctx& x, id, id, SEL, uint32_t) { x.proc.log(LogLevel::Info, "UIImageWriteToSavedPhotosAlbum: ignored"); }

// ============================================================ UIGraphics ===

struct ImageContext { id ctx; float scale; };
thread_local std::vector<ImageContext> t_imageContexts;

void ug_beginOptions(Ctx& x, CGSize size, bool opaque, float scale) {
    if (scale <= 0) scale = ui(x).scale;
    const int w = std::max(1, int(std::ceil(size.width * scale))), h = std::max(1, int(std::ceil(size.height * scale)));
    const id ctx = CG::createBitmapContext(x, 0, w, h, 0, opaque ? 0x2006u /* noneSkipFirst little */ : 0x2002u);
    if (!ctx) { x.proc.logf(LogLevel::Warning, "UIGraphicsBeginImageContext: bad size %gx%g", size.width, size.height); return; }
    CG::setUIKitCTM(x, ctx, size.height, scale);
    if (opaque) CG::fillRect(x, ctx, {0, 0, size.width, size.height}, {0, 0, 0, 1}, G::BlendMode::Copy);
    CG::pushContext(x, ctx);
    rt(x).release(x.cpu, ctx);   // the stack holds it
    t_imageContexts.push_back({ctx, scale});
}
void ug_begin(Ctx& x, CGSize size) { ug_beginOptions(x, size, false, 1); }
id ug_getImage(Ctx& x) {
    if (t_imageContexts.empty()) return 0;
    const ImageContext& ic = t_imageContexts.back();
    auto img = CG::contextImage(x, ic.ctx);
    return img ? autoreleased(x, newImageObj(x, ui(x).image, img, ic.scale)) : 0;
}
void ug_end(Ctx& x) {
    if (t_imageContexts.empty()) return;
    t_imageContexts.pop_back();
    CG::popContext(x);
}
uint32_t ug_current(Ctx& x) { return CG::currentContext(x); }
void ug_push(Ctx& x, id ctx) { CG::pushContext(x, ctx); }
void ug_pop(Ctx& x) { CG::popContext(x); }
void ug_rectFill(Ctx& x, CGRect r) {
    const id ctx = CG::currentContext(x);
    if (CG::ContextData* d = CG::context(x, ctx)) CG::fillRect(x, ctx, CG::toG(r), d->gs().fill, d->gs().blend);
}
void ug_rectFillBlend(Ctx& x, CGRect r, int32_t mode) {
    const id ctx = CG::currentContext(x);
    if (CG::ContextData* d = CG::context(x, ctx)) CG::fillRect(x, ctx, CG::toG(r), d->gs().fill, mode == 17 ? G::BlendMode::Copy : mode == 16 ? G::BlendMode::Clear : d->gs().blend);
}
void ug_rectFrame(Ctx& x, CGRect r) {
    const id ctx = CG::currentContext(x);
    CG::ContextData* d = CG::context(x, ctx);
    if (!d) return;
    const G::Rect g = CG::toG(r);
    const G::Color c = d->gs().fill;   // UIRectFrame uses the fill colour
    CG::fillRect(x, ctx, {g.x, g.y, g.w, 1}, c);
    CG::fillRect(x, ctx, {g.x, g.maxY() - 1, g.w, 1}, c);
    CG::fillRect(x, ctx, {g.x, g.y, 1, g.h}, c);
    CG::fillRect(x, ctx, {g.maxX() - 1, g.y, 1, g.h}, c);
}
void ug_rectClip(Ctx& x, CGRect r) {
    CG::ContextData* d = CG::context(x, CG::currentContext(x));
    if (!d) return;
    G::Path p;
    p.addRect(CG::toG(r));
    d->gs().clip = G::intersectClip(d->gs().clip, p.transformed(d->gs().ctm), false);
}

// ======================================================= NSString drawing ===

G::FontDesc fontArg(Ctx& x, id font) { return font ? uiFont(x, font) : uiFont(x, systemFont(x, 17)); }
G::Color currentFill(Ctx& x) { CG::ContextData* d = CG::context(x, CG::currentContext(x)); return d ? d->gs().fill : G::Color{0, 0, 0, 1}; }
void retSize(Ctx& x, GuestPtr ret, G::Size s) { CG::writeStruct(x, ret, CGSize{std::ceil(s.w), std::ceil(s.h)}); }

void s_sizeWithFont(Ctx& x, GuestPtr ret, id self, SEL, id font) { retSize(x, ret, measureText(x, textOf(x, self), fontArg(x, font), 0, 1, 2)); }
void s_sizeConstrained(Ctx& x, GuestPtr ret, id self, SEL, id font, CGSize c) {
    G::Size s = measureText(x, textOf(x, self), fontArg(x, font), c.width, 0, 0);
    s.h = std::min(s.h, c.height);
    retSize(x, ret, s);
}
void s_sizeConstrainedMode(Ctx& x, GuestPtr ret, id self, SEL, id font, CGSize c, int32_t mode) {
    G::Size s = measureText(x, textOf(x, self), fontArg(x, font), c.width, 0, mode);
    s.h = std::min(s.h, c.height);
    retSize(x, ret, s);
}
void s_sizeForWidth(Ctx& x, GuestPtr ret, id self, SEL, id font, float w, int32_t mode) {
    G::Size s = measureText(x, textOf(x, self), fontArg(x, font), w, 1, mode);
    retSize(x, ret, s);
}
void s_sizeMinFont(Ctx& x, GuestPtr ret, id self, SEL, id font, float, GuestPtr actual, float w, int32_t mode) {
    if (actual) x.mem.write<float>(actual, fontArg(x, font).size);
    retSize(x, ret, measureText(x, textOf(x, self), fontArg(x, font), w, 1, mode));
}
void s_drawAtPoint(Ctx& x, GuestPtr ret, id self, SEL, CGPoint p, id font) {
    const std::string t = textOf(x, self);
    const G::FontDesc f = fontArg(x, font);
    const G::Size s = measureText(x, t, f, 0, 1, 2);
    drawText(x, CG::currentContext(x), t, f, {p.x, p.y, s.w + 1, s.h}, currentFill(x), 2, 0, 1);
    retSize(x, ret, s);
}
void s_drawAtPointWidth(Ctx& x, GuestPtr ret, id self, SEL, CGPoint p, float w, id font, int32_t mode) {
    const std::string t = textOf(x, self);
    const G::FontDesc f = fontArg(x, font);
    const G::Size s = measureText(x, t, f, w, 1, mode);
    drawText(x, CG::currentContext(x), t, f, {p.x, p.y, w, s.h}, currentFill(x), mode, 0, 1);
    retSize(x, ret, s);
}
void s_drawAtPointWidthMin(Ctx& x, GuestPtr ret, id self, SEL s, CGPoint p, float w, id font, float, GuestPtr actual, int32_t mode, int32_t) {
    if (actual) x.mem.write<float>(actual, fontArg(x, font).size);
    s_drawAtPointWidth(x, ret, self, s, p, w, font, mode);
}
void s_drawInRectFull(Ctx& x, GuestPtr ret, id self, SEL, CGRect r, id font, int32_t mode, int32_t align) {
    const std::string t = textOf(x, self);
    retSize(x, ret, drawText(x, CG::currentContext(x), t, fontArg(x, font), CG::toG(r), currentFill(x), mode, align, 0));
}
void s_drawInRect(Ctx& x, GuestPtr ret, id self, SEL s, CGRect r, id font) { s_drawInRectFull(x, ret, self, s, r, font, 0, 0); }
void s_drawInRectMode(Ctx& x, GuestPtr ret, id self, SEL s, CGRect r, id font, int32_t mode) { s_drawInRectFull(x, ret, self, s, r, font, mode, 0); }
// iOS 7 attribute dictionaries
G::FontDesc attrFont(Ctx& x, id attrs) { const id f = attrs ? Foundation::dictGet(x, attrs, "NSFont") : 0; return fontArg(x, f); }
G::Color attrColor(Ctx& x, id attrs) { const id c = attrs ? Foundation::dictGet(x, attrs, "NSColor") : 0; return c ? uiColor(x, c) : G::Color{0, 0, 0, 1}; }
void s_sizeWithAttrs(Ctx& x, GuestPtr ret, id self, SEL, id attrs) { retSize(x, ret, measureText(x, textOf(x, self), attrFont(x, attrs), 0, 1, 2)); }
void s_drawAtPointAttrs(Ctx& x, id self, SEL, CGPoint p, id attrs) {
    const std::string t = textOf(x, self);
    const G::FontDesc f = attrFont(x, attrs);
    const G::Size s = measureText(x, t, f, 0, 1, 2);
    drawText(x, CG::currentContext(x), t, f, {p.x, p.y, s.w + 1, s.h}, attrColor(x, attrs), 2, 0, 1);
}
void s_drawInRectAttrs(Ctx& x, id self, SEL, CGRect r, id attrs) {
    drawText(x, CG::currentContext(x), textOf(x, self), attrFont(x, attrs), CG::toG(r), attrColor(x, attrs), 0, 0, 0);
}
void s_boundingRect(Ctx& x, GuestPtr ret, id self, SEL, CGSize size, uint32_t, id attrs, id) {
    const G::Size s = measureText(x, textOf(x, self), attrFont(x, attrs), size.width, 0, 0);
    CG::writeStruct(x, ret, CGRect{{0, 0}, {s.w, std::min(s.h, size.height)}});
}

// ================================================= geometry ↔ strings ===

std::string g(float v) { char b[32]; std::snprintf(b, sizeof b, "%g", v); return b; }
id f_strPoint(Ctx& x, CGPoint p) { return makeString(x, "{" + g(p.x) + ", " + g(p.y) + "}"); }
id f_strSize(Ctx& x, CGSize s) { return makeString(x, "{" + g(s.width) + ", " + g(s.height) + "}"); }
id f_strRect(Ctx& x, CGRect r) { return makeString(x, "{{" + g(r.origin.x) + ", " + g(r.origin.y) + "}, {" + g(r.size.width) + ", " + g(r.size.height) + "}}"); }
id f_strTransform(Ctx& x, CGAffineTransform t) { return makeString(x, "[" + g(t.a) + ", " + g(t.b) + ", " + g(t.c) + ", " + g(t.d) + ", " + g(t.tx) + ", " + g(t.ty) + "]"); }
id f_strInsets(Ctx& x, UIEdgeInsets i) { return makeString(x, "{" + g(i.top) + ", " + g(i.left) + ", " + g(i.bottom) + ", " + g(i.right) + "}"); }
std::vector<float> numbersIn(const std::string& s) {
    std::vector<float> v;
    const char* p = s.c_str();
    while (*p) {
        if ((*p >= '0' && *p <= '9') || *p == '-' || *p == '.' || *p == '+') {
            char* end;
            const float f = std::strtof(p, &end);
            if (end == p) { ++p; continue; }
            v.push_back(f);
            p = end;
        } else ++p;
    }
    return v;
}
void f_pointFrom(Ctx& x, GuestPtr ret, id s) { auto v = numbersIn(s ? utf8(x, s) : ""); v.resize(2, 0); CG::writeStruct(x, ret, CGPoint{v[0], v[1]}); }
void f_sizeFrom(Ctx& x, GuestPtr ret, id s) { auto v = numbersIn(s ? utf8(x, s) : ""); v.resize(2, 0); CG::writeStruct(x, ret, CGSize{v[0], v[1]}); }
void f_rectFrom(Ctx& x, GuestPtr ret, id s) { auto v = numbersIn(s ? utf8(x, s) : ""); v.resize(4, 0); CG::writeStruct(x, ret, CGRect{{v[0], v[1]}, {v[2], v[3]}}); }
void f_insetsFrom(Ctx& x, GuestPtr ret, id s) { auto v = numbersIn(s ? utf8(x, s) : ""); v.resize(4, 0); CG::writeStruct(x, ret, UIEdgeInsets{v[0], v[1], v[2], v[3]}); }
void f_transformFrom(Ctx& x, GuestPtr ret, id s) { auto v = numbersIn(s ? utf8(x, s) : ""); v.resize(6, 0); CG::writeStruct(x, ret, CGAffineTransform{v[0], v[1], v[2], v[3], v[4], v[5]}); }
void f_insetRect(Ctx& x, GuestPtr ret, CGRect r, UIEdgeInsets i) {
    CG::writeStruct(x, ret, CGRect{{r.origin.x + i.left, r.origin.y + i.top}, {r.size.width - i.left - i.right, r.size.height - i.top - i.bottom}});
}

// NSValue (UIGeometry) additions
template <typename T> id valueWith(Ctx& x, const T& v, const char* type) { return Foundation::makeValue(x, &v, sizeof v, type); }
id v_point(Ctx& x, id, SEL, CGPoint p) { return valueWith(x, p, "{CGPoint=ff}"); }
id v_size(Ctx& x, id, SEL, CGSize s) { return valueWith(x, s, "{CGSize=ff}"); }
id v_rect(Ctx& x, id, SEL, CGRect r) { return valueWith(x, r, "{CGRect={CGPoint=ff}{CGSize=ff}}"); }
id v_transform(Ctx& x, id, SEL, CGAffineTransform t) { return valueWith(x, t, "{CGAffineTransform=ffffff}"); }
id v_insets(Ctx& x, id, SEL, UIEdgeInsets i) { return valueWith(x, i, "{UIEdgeInsets=ffff}"); }
id v_t3d(Ctx& x, id, SEL, CATransform3D t) { return valueWith(x, t, "{CATransform3D=ffffffffffffffff}"); }
template <typename T> void valueGet(Ctx& x, GuestPtr ret, id self) {
    const uint32_t tmp = x.proc.heap().allocZeroed(64);
    rt(x).send(x.cpu, self, "getValue:", {tmp});
    std::memcpy(x.mem.host(ret), x.mem.host(tmp), sizeof(T));
    x.proc.heap().free(tmp);
}
void v_pointValue(Ctx& x, GuestPtr ret, id self, SEL) { valueGet<CGPoint>(x, ret, self); }
void v_sizeValue(Ctx& x, GuestPtr ret, id self, SEL) { valueGet<CGSize>(x, ret, self); }
void v_rectValue(Ctx& x, GuestPtr ret, id self, SEL) { valueGet<CGRect>(x, ret, self); }
void v_transformValue(Ctx& x, GuestPtr ret, id self, SEL) { valueGet<CGAffineTransform>(x, ret, self); }
void v_insetsValue(Ctx& x, GuestPtr ret, id self, SEL) { valueGet<UIEdgeInsets>(x, ret, self); }
void v_t3dValue(Ctx& x, GuestPtr ret, id self, SEL) { valueGet<CATransform3D>(x, ret, self); }

// NSCoder (UIGeometryKeyedCoding) additions — usable by both NSKeyedUnarchiver and nib decoders.
G::Rect rectFromCoderValue(Ctx& x, id v) {
    if (!v) return {};
    if (Foundation::isString(x, v)) { auto n = numbersIn(utf8(x, v)); n.resize(4, 0); return {n[0], n[1], n[2], n[3]}; }
    if (Foundation::isData(x, v)) {
        // UINibEncoder: a type byte (0x06 = float, 0x07 = double) followed by the components.
        const auto b = Foundation::dataBytes(x, v);
        float f[4] = {0, 0, 0, 0};
        if (!b.empty() && (b[0] == 6 || b[0] == 7) && (b.size() - 1) % (b[0] == 7 ? 8 : 4) == 0) {
            const size_t w = b[0] == 7 ? 8 : 4, n = std::min<size_t>(4, (b.size() - 1) / w);
            for (size_t i = 0; i < n; ++i) {
                if (w == 8) { double d; std::memcpy(&d, b.data() + 1 + i * 8, 8); f[i] = float(d); }
                else std::memcpy(&f[i], b.data() + 1 + i * 4, 4);
            }
        } else if (b.size() == 32 || b.size() == 16) {
            const size_t w = b.size() == 32 ? 8 : 4;
            for (size_t i = 0; i < 4; ++i) {
                if (w == 8) { double d; std::memcpy(&d, b.data() + i * 8, 8); f[i] = float(d); }
                else std::memcpy(&f[i], b.data() + i * 4, 4);
            }
        } else if (b.size() == 8) std::memcpy(f, b.data(), 8);
        return {f[0], f[1], f[2], f[3]};
    }
    if (rt(x).respondsTo(v, rt(x).sel("getValue:"))) {
        const uint32_t tmp = x.proc.heap().allocZeroed(64);
        rt(x).send(x.cpu, v, "getValue:", {tmp});
        const CGRect r = CG::readStruct<CGRect>(x, tmp);
        x.proc.heap().free(tmp);
        return CG::toG(r);
    }
    return {};
}
id coderObj(Ctx& x, id coder, id key) { return id(rt(x).send(x.cpu, coder, "decodeObjectForKey:", {key})); }
void cd_rect(Ctx& x, GuestPtr ret, id self, SEL, id key) { CG::writeStruct(x, ret, CG::fromG(rectFromCoderValue(x, coderObj(x, self, key)))); }
void cd_point(Ctx& x, GuestPtr ret, id self, SEL, id key) { const G::Rect r = rectFromCoderValue(x, coderObj(x, self, key)); CG::writeStruct(x, ret, CGPoint{r.x, r.y}); }
void cd_size(Ctx& x, GuestPtr ret, id self, SEL, id key) { const G::Rect r = rectFromCoderValue(x, coderObj(x, self, key)); CG::writeStruct(x, ret, CGSize{r.x, r.y}); }
void cd_insets(Ctx& x, GuestPtr ret, id self, SEL, id key) { const G::Rect r = rectFromCoderValue(x, coderObj(x, self, key)); CG::writeStruct(x, ret, UIEdgeInsets{r.x, r.y, r.w, r.h}); }
void cd_transform(Ctx& x, GuestPtr ret, id self, SEL, id key) {
    const id v = coderObj(x, self, key);
    auto n = v && Foundation::isString(x, v) ? numbersIn(utf8(x, v)) : std::vector<float>{1, 0, 0, 1, 0, 0};
    n.resize(6, 0);
    CG::writeStruct(x, ret, CGAffineTransform{n[0], n[1], n[2], n[3], n[4], n[5]});
}
void ce_rect(Ctx& x, id self, SEL, CGRect r, id key) { rt(x).send(x.cpu, self, "encodeObject:forKey:", {f_strRect(x, r), key}); }
void ce_point(Ctx& x, id self, SEL, CGPoint p, id key) { rt(x).send(x.cpu, self, "encodeObject:forKey:", {f_strPoint(x, p), key}); }
void ce_size(Ctx& x, id self, SEL, CGSize s, id key) { rt(x).send(x.cpu, self, "encodeObject:forKey:", {f_strSize(x, s), key}); }

} // namespace

// =============================================================== exports ===

G::Color uiColor(Ctx& x, id color) {
    if (!color) return {0, 0, 0, 0};
    if (ColorObj* h = co(x, color)) return h->c;
    return CG::colorOf(x, color);
}
G::ImagePtr uiColorPattern(Ctx& x, id color) { ColorObj* h = co(x, color); return h ? h->pattern : nullptr; }
float uiColorPatternScale(Ctx& x, id color) { ColorObj* h = co(x, color); return h ? h->patternScale : 1; }
id makeUIColor(Ctx& x, G::Color c) { return autoreleased(x, newColor(x, ui(x).color, c)); }
G::FontDesc uiFont(Ctx& x, id font) { FontObj* h = fo(x, font); return h ? h->f : desc("Helvetica", 17); }
id makeUIFont(Ctx& x, const G::FontDesc& f) { return autoreleased(x, newFont(x, f)); }
id systemFont(Ctx& x, float size, bool bold) { return autoreleased(x, newFont(x, desc(bold ? "Helvetica-Bold" : "Helvetica", size))); }

ImgData* uiImage(Ctx& x, id img) { return img ? rt(x).hostIf<ImgData>(img) : nullptr; }
id makeUIImage(Ctx& x, G::ImagePtr img, float scale) { return autoreleased(x, newImageObj(x, ui(x).image, std::move(img), scale)); }
id imageCGImage(Ctx& x, id uiimage) {
    ImgData* d = uiImage(x, uiimage);
    if (!d || !d->image) return 0;
    if (!d->cgImage) d->cgImage = CG::createImage(x, d->image);
    return d->cgImage;
}
G::Size imageSize(Ctx& x, id uiimage) {
    ImgData* d = uiImage(x, uiimage);
    if (!d || !d->image) return {0, 0};
    G::Size s{float(d->image->width) / d->scale, float(d->image->height) / d->scale};
    if (d->orientation >= 4) std::swap(s.w, s.h);   // Left/Right (mirrored too)
    return s;
}
void drawUIImage(Ctx& x, id ctx, id uiimage, const G::Rect& r, float alpha, G::BlendMode m) {
    ImgData* d = uiImage(x, uiimage);
    if (!d || !d->image || !ctx) return;
    if (!d->resizable) { CG::drawImageInRect(x, ctx, *d->image, r, true, alpha, m); return; }
    // 9-slice
    const G::Image& img = *d->image;
    const float s = d->scale;
    const UIEdgeInsets c = d->caps;
    const float sx[4] = {0, c.left * s, float(img.width) - c.right * s, float(img.width)};
    const float sy[4] = {0, c.top * s, float(img.height) - c.bottom * s, float(img.height)};
    const float dx[4] = {r.x, r.x + c.left, r.maxX() - c.right, r.maxX()};
    const float dy[4] = {r.y, r.y + c.top, r.maxY() - c.bottom, r.maxY()};
    CG::ContextData* cd = CG::context(x, ctx);
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i) {
            const float sw = sx[i + 1] - sx[i], sh = sy[j + 1] - sy[j], dw = dx[i + 1] - dx[i], dh = dy[j + 1] - dy[j];
            if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) continue;
            const G::Affine toUser = G::Affine::translate(-sx[i], -sy[j]).then(G::Affine::scale(dw / sw, dh / sh)).then(G::Affine::translate(dx[i], dy[j]));
            G::Path p;
            p.addRect({dx[i], dy[j], dw, dh});
            const G::Clip clip = G::intersectClip(cd->gs().clip, p.transformed(cd->gs().ctm), false);
            G::drawImage(cd->surface, img, toUser.then(cd->gs().ctm), clip, alpha * cd->gs().alpha, cd->gs().smoothImages, m);
        }
}
std::string textOf(Ctx& x, id str) { return str ? utf8(x, str) : std::string(); }

G::Size measureText(Ctx& x, const std::string& s, const G::FontDesc& f, float maxWidth, int maxLines, int lineBreak) {
    G::TextBackend& tb = G::textBackend();
    const float lh = tb.measure("Ag", f).lineHeight;
    if (s.empty()) return {0, lh};
    const bool wraps = lineBreak == 0 || lineBreak == 1;
    std::vector<std::string> lines;
    if (maxWidth > 0 && wraps) lines = G::wrapLines(s, f, maxWidth, maxLines, false);
    else if (maxLines == 1) lines = {s.substr(0, s.find('\n'))};
    else lines = G::wrapLines(s, f, 0, maxLines, false);
    float w = 0;
    for (auto& l : lines) w = std::max(w, tb.measure(l, f).width);
    if (maxWidth > 0) w = std::min(w, maxWidth);
    return {w, lh * float(std::max<size_t>(lines.size(), 1))};
}

G::Size drawText(Ctx& x, id ctx, const std::string& s, const G::FontDesc& f, const G::Rect& r, G::Color c, int lineBreak, int alignment, int maxLines) {
    if (!ctx || s.empty()) return {0, 0};
    G::TextBackend& tb = G::textBackend();
    const float lh = tb.measure("Ag", f).lineHeight;
    int fitLines = r.h > 0 ? std::max(1, int((r.h + 0.5f) / lh)) : 1;
    if (maxLines > 0) fitLines = std::min(fitLines, maxLines);
    const bool wraps = (lineBreak == 0 || lineBreak == 1) && fitLines != 1;
    std::vector<std::string> lines = wraps ? G::wrapLines(s, f, r.w, fitLines, true)
                                           : G::wrapLines(s.substr(0, maxLines == 1 || fitLines == 1 ? s.find('\n') : std::string::npos), f, 0, fitLines, false);
    if (!wraps && lineBreak >= 3 && !lines.empty()) {
        // truncate the (single) line to the width with an ellipsis
        std::string& l = lines.front();
        const std::string ell = "\xE2\x80\xA6";
        if (tb.measure(l, f).width > r.w) {
            while (!l.empty() && tb.measure(l + ell, f).width > r.w) { l.pop_back(); while (!l.empty() && (uint8_t(l.back()) & 0xC0) == 0x80) l.pop_back(); }
            l += ell;
        }
    }
    CG::ContextData* cd = CG::context(x, ctx);
    if (!cd) return {0, 0};
    // clip to the rect horizontally (UIKit clips text drawn with drawInRect:)
    cd->stack.push_back(cd->gs());
    if (r.w > 0) {
        G::Path p;
        p.addRect({r.x, r.y, r.w, std::max(r.h, lh * float(lines.size()))});
        cd->gs().clip = G::intersectClip(cd->gs().clip, p.transformed(cd->gs().ctm), false);
    }
    float y = r.y, maxW = 0;
    for (auto& l : lines) {
        const float w = tb.measure(l, f).width;
        float lx = r.x;
        if (alignment == 1) lx = r.x + (r.w - w) / 2;
        else if (alignment == 2) lx = r.x + r.w - w;
        CG::drawTextUIKit(x, ctx, l, f, {lx, y}, c);
        maxW = std::max(maxW, w);
        y += lh;
    }
    cd->stack.pop_back();
    return {std::min(maxW, r.w > 0 ? r.w : maxW), y - r.y};
}

void registerUIGraphics(Process& p, ObjC::Runtime& r) {
    auto& u = ui(p);
    // UIColor
    ObjC::ClassBuilder col = r.defineClass("UIColor", "NSObject");
    u.color = col.cls();
    col.cm<&cc_std<0, 0, 0, 1000>>("blackColor").cm<&cc_std<333, 333, 333, 1000>>("darkGrayColor")
       .cm<&cc_std<667, 667, 667, 1000>>("lightGrayColor").cm<&cc_std<1000, 1000, 1000, 1000>>("whiteColor")
       .cm<&cc_std<500, 500, 500, 1000>>("grayColor").cm<&cc_std<1000, 0, 0, 1000>>("redColor")
       .cm<&cc_std<0, 1000, 0, 1000>>("greenColor").cm<&cc_std<0, 0, 1000, 1000>>("blueColor")
       .cm<&cc_std<0, 1000, 1000, 1000>>("cyanColor").cm<&cc_std<1000, 1000, 0, 1000>>("yellowColor")
       .cm<&cc_std<1000, 0, 1000, 1000>>("magentaColor").cm<&cc_std<1000, 500, 0, 1000>>("orangeColor")
       .cm<&cc_std<500, 0, 500, 1000>>("purpleColor").cm<&cc_std<600, 400, 200, 1000>>("brownColor")
       .cm<&cc_std<0, 0, 0, 0>>("clearColor").cm<&cc_std<1000, 1000, 1000, 600>>("lightTextColor")
       .cm<&cc_std<0, 0, 0, 1000>>("darkTextColor").cm<&cc_std<773, 788, 816, 1000>>("groupTableViewBackgroundColor")
       .cm<&cc_std<122, 122, 129, 1000>>("viewFlipsideBackgroundColor").cm<&cc_std<435, 443, 471, 1000>>("scrollViewTexturedBackgroundColor")
       .cm<&cc_std<443, 459, 502, 1000>>("underPageBackgroundColor")
       .cm<&cc_rgba>("colorWithRed:green:blue:alpha:").cm<&cc_white>("colorWithWhite:alpha:")
       .cm<&cc_hsb>("colorWithHue:saturation:brightness:alpha:").cm<&cc_cg>("colorWithCGColor:").cm<&cc_pattern>("colorWithPatternImage:");
    col.m<&ci_rgba>("initWithRed:green:blue:alpha:").m<&ci_white>("initWithWhite:alpha:")
       .m<&ci_hsb>("initWithHue:saturation:brightness:alpha:").m<&ci_cg>("initWithCGColor:").m<&ci_pattern>("initWithPatternImage:")
       .m<&ci_coder>("initWithCoder:").m<&c_CGColor>("CGColor", "^{CGColor=}@:").m<&c_set>("set", "v@:").m<&c_setFill>("setFill", "v@:")
       .m<&c_setStroke>("setStroke", "v@:").m<&c_withAlpha>("colorWithAlphaComponent:")
       .m<&c_getRGBA>("getRed:green:blue:alpha:", "c@:^f^f^f^f").m<&c_getWhite>("getWhite:alpha:", "c@:^f^f")
       .m<&c_getHSB>("getHue:saturation:brightness:alpha:", "c@:^f^f^f^f").m<&c_isEqual>("isEqual:", "c@:@")
       .m<&c_hash>("hash", "I@:").m<&c_description>("description").m<&c_copy>("copyWithZone:").m<&c_dealloc>("dealloc", "v@:");

    // UIFont
    ObjC::ClassBuilder font = r.defineClass("UIFont", "NSObject");
    u.font = font.cls();
    font.cm<&fc_named>("fontWithName:size:").cm<&fc_system>("systemFontOfSize:").cm<&fc_bold>("boldSystemFontOfSize:")
        .cm<&fc_italic>("italicSystemFontOfSize:").cm<&fc_systemWeight>("systemFontOfSize:weight:")
        .cm<&fc_labelSize>("labelFontSize", "f@:").cm<&fc_buttonSize>("buttonFontSize", "f@:")
        .cm<&fc_smallSize>("smallSystemFontSize", "f@:").cm<&fc_systemSize>("systemFontSize", "f@:")
        .cm<&fc_familyNames>("familyNames").cm<&fc_namesForFamily>("fontNamesForFamilyName:")
        .cm<&fc_preferred>("preferredFontForTextStyle:");
    font.m<&f_pointSize>("pointSize", "f@:").m<&f_fontName>("fontName").m<&f_familyName>("familyName")
        .m<&f_lineHeight>("lineHeight", "f@:").m<&f_ascender>("ascender", "f@:").m<&f_descender>("descender", "f@:")
        .m<&f_leading>("leading", "f@:").m<&f_capHeight>("capHeight", "f@:").m<&f_xHeight>("xHeight", "f@:")
        .m<&f_withSize>("fontWithSize:").m<&f_copy>("copyWithZone:").m<&f_description>("description")
        .m<&f_initWithCoder>("initWithCoder:");

    // UIImage
    const std::string S = "{CGSize=ff}", R = "{CGRect={CGPoint=ff}{CGSize=ff}}", P = "{CGPoint=ff}", E = "{UIEdgeInsets=ffff}";
    ObjC::ClassBuilder img = r.defineClass("UIImage", "NSObject");
    u.image = img.cls();
    img.cm<&imgc_named>("imageNamed:").cm<&imgc_file>("imageWithContentsOfFile:").cm<&imgc_data>("imageWithData:")
       .cm<&imgc_dataScale>("imageWithData:scale:").cm<&imgc_cg>("imageWithCGImage:").cm<&imgc_cgScale>("imageWithCGImage:scale:orientation:")
       .cm<&imgc_animated>("animatedImageWithImages:duration:");
    img.m<&imgi_file>("initWithContentsOfFile:").m<&imgi_data>("initWithData:").m<&imgi_cg>("initWithCGImage:")
       .m<&imgi_cgScale>("initWithCGImage:scale:orientation:")
       .m<&img_size>("size", S + "@:").m<&img_scale>("scale", "f@:").m<&img_orientation>("imageOrientation", "i@:")
       .m<&img_CGImage>("CGImage", "^{CGImage=}@:").m<&img_drawAtPoint>("drawAtPoint:", "v@:" + P)
       .m<&img_drawAtPointBlend>("drawAtPoint:blendMode:alpha:", "v@:" + P + "if").m<&img_drawInRect>("drawInRect:", "v@:" + R)
       .m<&img_drawInRectBlend>("drawInRect:blendMode:alpha:", "v@:" + R + "if").m<&img_drawPattern>("drawAsPatternInRect:", "v@:" + R)
       .m<&img_stretchable>("stretchableImageWithLeftCapWidth:topCapHeight:").m<&img_resizable>("resizableImageWithCapInsets:")
       .m<&img_resizableMode>("resizableImageWithCapInsets:resizingMode:").m<&img_capInsets>("capInsets", E + "@:")
       .m<&img_leftCap>("leftCapWidth", "i@:").m<&img_topCap>("topCapHeight", "i@:").m<&img_withRendering>("imageWithRenderingMode:")
       .m<&img_images>("images").m<&img_duration>("duration", "d@:").m<&img_copy>("copyWithZone:")
       .m<&img_description>("description").m<&img_dealloc>("dealloc", "v@:");
    p.def<&f_pngRep>("_UIImagePNGRepresentation");
    p.def<&f_jpegRep>("_UIImageJPEGRepresentation");
    p.def<&f_writeToAlbum>("_UIImageWriteToSavedPhotosAlbum");

    // UIGraphics
    p.def<&ug_begin>("_UIGraphicsBeginImageContext");
    p.def<&ug_beginOptions>("_UIGraphicsBeginImageContextWithOptions");
    p.def<&ug_getImage>("_UIGraphicsGetImageFromCurrentImageContext");
    p.def<&ug_end>("_UIGraphicsEndImageContext");
    p.def<&ug_current>("_UIGraphicsGetCurrentContext");
    p.def<&ug_push>("_UIGraphicsPushContext");
    p.def<&ug_pop>("_UIGraphicsPopContext");
    p.def<&ug_rectFill>("_UIRectFill");
    p.def<&ug_rectFillBlend>("_UIRectFillUsingBlendMode");
    p.def<&ug_rectFrame>("_UIRectFrame");
    p.def<&ug_rectClip>("_UIRectClip");

    // NSString drawing
    ObjC::ClassBuilder str = r.builder(r.getClass("NSString"));
    str.m<&s_sizeWithFont>("sizeWithFont:", S + "@:@").m<&s_sizeConstrained>("sizeWithFont:constrainedToSize:", S + "@:@" + S)
       .m<&s_sizeConstrainedMode>("sizeWithFont:constrainedToSize:lineBreakMode:", S + "@:@" + S + "i")
       .m<&s_sizeForWidth>("sizeWithFont:forWidth:lineBreakMode:", S + "@:@fi")
       .m<&s_sizeMinFont>("sizeWithFont:minFontSize:actualFontSize:forWidth:lineBreakMode:", S + "@:@f^ffi")
       .m<&s_drawAtPoint>("drawAtPoint:withFont:", S + "@:" + P + "@")
       .m<&s_drawAtPointWidth>("drawAtPoint:forWidth:withFont:lineBreakMode:", S + "@:" + P + "f@i")
       .m<&s_drawAtPointWidthMin>("drawAtPoint:forWidth:withFont:minFontSize:actualFontSize:lineBreakMode:baselineAdjustment:", S + "@:" + P + "f@f^fii")
       .m<&s_drawInRect>("drawInRect:withFont:", S + "@:" + R + "@").m<&s_drawInRectMode>("drawInRect:withFont:lineBreakMode:", S + "@:" + R + "@i")
       .m<&s_drawInRectFull>("drawInRect:withFont:lineBreakMode:alignment:", S + "@:" + R + "@ii")
       .m<&s_sizeWithAttrs>("sizeWithAttributes:", S + "@:@").m<&s_drawAtPointAttrs>("drawAtPoint:withAttributes:", "v@:" + P + "@")
       .m<&s_drawInRectAttrs>("drawInRect:withAttributes:", "v@:" + R + "@")
       .m<&s_boundingRect>("boundingRectWithSize:options:attributes:context:", R + "@:" + S + "I@@");
    for (const char* n : {"NSFontAttributeName", "NSForegroundColorAttributeName", "NSBackgroundColorAttributeName",
                          "NSParagraphStyleAttributeName", "NSKernAttributeName", "NSShadowAttributeName", "UITextAttributeFont",
                          "UITextAttributeTextColor", "UITextAttributeTextShadowColor", "UITextAttributeTextShadowOffset"}) {
        std::string v = n;
        if (v == "NSFontAttributeName" || v == "UITextAttributeFont") v = "NSFont";
        else if (v == "NSForegroundColorAttributeName" || v == "UITextAttributeTextColor") v = "NSColor";
        else if (v == "NSBackgroundColorAttributeName") v = "NSBackgroundColor";
        else if (v == "NSParagraphStyleAttributeName") v = "NSParagraphStyle";
        else if (v == "NSKernAttributeName") v = "NSKern";
        else if (v == "NSShadowAttributeName") v = "NSShadow";
        const uint32_t cell = p.defineDataBlock(std::string("_") + n, 4);
        p.mem().write<uint32_t>(cell, Foundation::constString(p, v));
    }

    // geometry strings
    p.def<&f_strPoint>("_NSStringFromCGPoint");
    p.def<&f_strSize>("_NSStringFromCGSize");
    p.def<&f_strRect>("_NSStringFromCGRect");
    p.def<&f_strTransform>("_NSStringFromCGAffineTransform");
    p.def<&f_strInsets>("_NSStringFromUIEdgeInsets");
    p.def<&f_pointFrom>("_CGPointFromString");
    p.def<&f_sizeFrom>("_CGSizeFromString");
    p.def<&f_rectFrom>("_CGRectFromString");
    p.def<&f_insetsFrom>("_UIEdgeInsetsFromString");
    p.def<&f_transformFrom>("_CGAffineTransformFromString");
    p.def<&f_insetRect>("_UIEdgeInsetsInsetRect");
    p.defineDataBlock("_UIEdgeInsetsZero", 16);

    ObjC::ClassBuilder val = r.builder(r.getClass("NSValue"));
    val.cm<&v_point>("valueWithCGPoint:").cm<&v_size>("valueWithCGSize:").cm<&v_rect>("valueWithCGRect:")
       .cm<&v_transform>("valueWithCGAffineTransform:").cm<&v_insets>("valueWithUIEdgeInsets:").cm<&v_t3d>("valueWithCATransform3D:");
    val.m<&v_pointValue>("CGPointValue", P + "@:").m<&v_sizeValue>("CGSizeValue", S + "@:").m<&v_rectValue>("CGRectValue", R + "@:")
       .m<&v_transformValue>("CGAffineTransformValue", "{CGAffineTransform=ffffff}@:").m<&v_insetsValue>("UIEdgeInsetsValue", E + "@:")
       .m<&v_t3dValue>("CATransform3DValue", "{CATransform3D=ffffffffffffffff}@:");

    ObjC::ClassBuilder coder = r.builder(r.getClass("NSCoder"));
    coder.m<&cd_rect>("decodeCGRectForKey:", R + "@:@").m<&cd_point>("decodeCGPointForKey:", P + "@:@")
         .m<&cd_size>("decodeCGSizeForKey:", S + "@:@").m<&cd_insets>("decodeUIEdgeInsetsForKey:", E + "@:@")
         .m<&cd_transform>("decodeCGAffineTransformForKey:", "{CGAffineTransform=ffffff}@:@")
         .m<&ce_rect>("encodeCGRect:forKey:", "v@:" + R + "@").m<&ce_point>("encodeCGPoint:forKey:", "v@:" + P + "@")
         .m<&ce_size>("encodeCGSize:forKey:", "v@:" + S + "@");
}

} // namespace Hydrogen::Runtime::UIKit
