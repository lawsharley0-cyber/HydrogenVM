#include "TrueType.hpp"
#include <map>
#include <set>
#define STB_TRUETYPE_IMPLEMENTATION
#include "thirdparty/stb/stb_truetype.h"

namespace Hydrogen::Graphics {
struct TrueTypeFont::Impl {
    std::vector<uint8_t> bytes;
    stbtt_fontinfo info{};
    int units = 0, asc = 0, desc = 0, gap = 0;
    uint32_t traits = 0;
    int underline = 0;
    float scale(float size) const { return size / float(units); }
};
TrueTypeFont::TrueTypeFont() : impl(std::make_unique<Impl>()) {}
TrueTypeFont::~TrueTypeFont() = default;
std::shared_ptr<TrueTypeFont> TrueTypeFont::load(std::vector<uint8_t> bytes) {
    // Support standalone TrueType outlines. Reject truncated tables before
    // passing data to stb; CFF/TTC and other formats remain unsupported.
    auto u16 = [&](size_t p) { return (uint32_t(bytes[p]) << 8) | bytes[p + 1]; };
    auto u32 = [&](size_t p) { return (u16(p) << 16) | u16(p + 2); };
    if (bytes.size() < 12 || u32(0) != 0x10000) return {};
    const uint32_t n = u16(4);
    if (!n || 12ull + n * 16ull > bytes.size()) return {};
    uint32_t head = 0, maxp = 0, post = 0;
    std::map<uint32_t, std::pair<uint32_t,uint32_t>> tables;
    for (uint32_t i = 0; i < n; ++i) {
        const size_t p = 12 + i * 16;
        const uint32_t offset = u32(p + 8), size = u32(p + 12);
        if (uint64_t(offset) + size > bytes.size()) return {};
        if (!tables.emplace(u32(p), std::pair{offset,size}).second) return {};
        if (u32(p) == 0x68656164) { if (size < 54) return {}; head = offset; }
        if (u32(p) == 0x6D617870) { if (size < 6) return {}; maxp = offset; }
        if (u32(p) == 0x706F7374) { if (size < 16) return {}; post = offset; }
    }
    if (!head || !maxp || u16(head + 18) == 0 || u16(maxp + 4) == 0) return {};
    const auto [hhea,hheaSize] = tables[0x68686561];
    const auto [hmtx,hmtxSize] = tables[0x686D7478];
    const auto [loca,locaSize] = tables[0x6C6F6361];
    const auto [glyf,glyfSize] = tables[0x676C7966];
    const auto [cmap,cmapSize] = tables[0x636D6170];
    const uint32_t glyphs = u16(maxp + 4), format = u16(head + 50);
    if (hheaSize < 36 || !hmtx || !loca || !glyf || cmapSize < 4 || format > 1) return {};
    const uint32_t metrics = u16(hhea + 34);
    if (!metrics || metrics > glyphs || hmtxSize < metrics*4 + (glyphs-metrics)*2 || locaSize < (glyphs+1)*(format ? 4u : 2u)) return {};
    auto glyphOffset = [&](uint32_t g) { return format ? u32(loca+g*4) : u16(loca+g*2)*2; };
    std::vector<std::vector<uint32_t>> children(glyphs);
    for (uint32_t g=0; g<glyphs; ++g) {
        const uint32_t begin = glyphOffset(g), end = glyphOffset(g+1);
        if (begin > end || end > glyfSize) return {};
        if (begin == end) continue;
        if (end-begin < 10) return {};
        size_t p = glyf+begin+10, limit = glyf+end;
        const int16_t contours = int16_t(u16(glyf+begin));
        if (contours >= 0) {
            if (p+uint32_t(contours)*2+2 > limit) return {};
            uint32_t points = 0;
            for (int i=0; i<contours; ++i) {
                const uint32_t count = u16(p)+1; p+=2;
                if (count <= points) return {};
                points = count;
            }
            const uint32_t instructions = u16(p); p+=2;
            if (p+instructions > limit) return {};
            p += instructions;
            uint32_t coordinates = 0;
            for (uint32_t i=0; i<points;) {
                if (p >= limit) return {};
                const uint8_t flags = bytes[p++];
                uint32_t repeat = 1;
                if (flags & 8) { if (p >= limit) return {}; repeat += bytes[p++]; }
                if (repeat > points-i) return {};
                coordinates += repeat * (((flags & 2) ? 1 : (flags & 16) ? 0 : 2) + ((flags & 4) ? 1 : (flags & 32) ? 0 : 2));
                i += repeat;
            }
            if (p+coordinates > limit) return {};
        } else if (contours == -1) {
            uint32_t flags;
            do {
                if (p+4 > limit) return {};
                flags = u16(p); const uint32_t child = u16(p+2); p+=4;
                if (child >= glyphs || !(flags & 2)) return {}; // point-matching composites unsupported by stb
                children[g].push_back(child);
                p += (flags & 1) ? 4 : 2;
                p += (flags & 8) ? 2 : (flags & 64) ? 4 : (flags & 128) ? 8 : 0;
                if (p > limit) return {};
            } while (flags & 32);
            if (flags & 256) { if (p+2 > limit || p+2+u16(p) > limit) return {}; }
        } else return {};
    }
    // Prevent recursive/cyclic compound outlines in the upstream decoder.
    std::vector<int> depths(glyphs, -1);
    std::set<uint32_t> active;
    std::function<int(uint32_t)> depth = [&](uint32_t g) {
        if (active.count(g) || active.size() >= 16) return 100;
        if (depths[g] >= 0) return depths[g];
        active.insert(g); int d = 0;
        for (uint32_t c : children[g]) d = std::max(d, 1+depth(c));
        active.erase(g); return depths[g] = d;
    };
    for (uint32_t g=0; g<glyphs; ++g) if (depth(g) >= 16) return {};
    const uint32_t maps = u16(cmap+2);
    if (4ull+maps*8ull > cmapSize) return {};
    for (uint32_t i=0; i<maps; ++i) {
        const uint32_t offset = u32(cmap+4+i*8+4);
        if (uint64_t(offset)+4 > cmapSize) return {};
        const size_t p = cmap+offset;
        const uint32_t kind = u16(p);
        const uint32_t platform = u16(cmap+4+i*8), encoding = u16(cmap+6+i*8);
        if ((platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10))) &&
            kind != 0 && kind != 4 && kind != 6 && kind != 12 && kind != 13) return {};
        uint32_t size = u16(p+2);
        if (kind == 12 || kind == 13) { if (uint64_t(offset)+16 > cmapSize) return {}; size=u32(p+4); }
        if (uint64_t(offset)+size > cmapSize) return {};
        if (kind == 0 && size < 262) return {};
        if (kind == 6 && (size < 10 || 10ull+u16(p+8)*2ull > size)) return {};
        if (kind == 4) {
            if (size < 16) return {};
            const uint32_t segments = u16(p+6)/2;
            if (!segments || 16ull+segments*8ull > size) return {};
            uint32_t power=1, selector=0;
            while (power*2 <= segments) { power*=2; ++selector; }
            if (u16(p+6) != segments*2 || u16(p+8) != power*2 || u16(p+10) != selector ||
                u16(p+12) != segments*2-power*2 || u16(p+14+(segments-1)*2) != 0xFFFF) return {};
            for (uint32_t j=0; j<segments; ++j) {
                const uint32_t last = u16(p+14+j*2), first = u16(p+16+segments*2+j*2);
                const size_t range = p+16+segments*6+j*2;
                if (first > last) return {};
                if (j && first <= u16(p+14+(j-1)*2)) return {};
                const uint32_t shift = u16(range);
                if (shift && range+shift+(last-first)*2ull+2 > p+size) return {};
            }
        }
        if ((kind == 12 || kind == 13) && (size < 16 || 16ull+u32(p+12)*12ull > size)) return {};
    }
    if (auto [name,size] = tables[0x6E616D65]; name) {
        if (size < 6 || 6ull+u16(name+2)*12ull > size) return {};
        for (uint32_t i=0; i<u16(name+2); ++i) {
            const size_t p=name+6+i*12;
            if (uint64_t(u16(name+4))+u16(p+10)+u16(p+8) > size) return {};
        }
    }
    auto f = std::shared_ptr<TrueTypeFont>(new TrueTypeFont);
    f->impl->units = int(u16(head + 18));
    const uint32_t style = u16(head + 44);
    f->impl->traits = ((style & 1) ? 2u : 0u) | ((style & 2) ? 1u : 0u) | ((post && u32(post + 12)) ? 1024u : 0u);
    f->impl->underline = post ? int(int16_t(u16(post + 10))) : 0;
    f->impl->bytes = std::move(bytes);
    if (!stbtt_InitFont(&f->impl->info, f->impl->bytes.data(), 0)) return {};
    stbtt_GetFontVMetrics(&f->impl->info, &f->impl->asc, &f->impl->desc, &f->impl->gap);
    return f;
}
std::string TrueTypeFont::name(bool family) const {
    int length = 0;
    const char* p = stbtt_GetFontNameString(&impl->info, &length, 3, 1, 0x409, family ? 1 : 6);
    std::string out;
    if (p) for (int i = 0; i + 1 < length; i += 2) {
        const uint32_t c = (uint32_t(uint8_t(p[i])) << 8) | uint8_t(p[i + 1]);
        if (c < 128) out += char(c);
        else if (c < 2048) { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 63)); }
        else { out += char(0xE0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 63)); out += char(0x80 | (c & 63)); }
    }
    return out.empty() ? "CustomTrueType" : out;
}
int TrueTypeFont::glyph(uint32_t c) const { return stbtt_FindGlyphIndex(&impl->info, int(c)); }
int TrueTypeFont::glyphCount() const { return impl->info.numGlyphs; }
uint32_t TrueTypeFont::traits() const { return impl->traits; }
float TrueTypeFont::underlineThickness(float s) const { return impl->underline * impl->scale(s); }
float TrueTypeFont::ascent(float s) const { return impl->asc * impl->scale(s); }
float TrueTypeFont::descent(float s) const { return -impl->desc * impl->scale(s); }
float TrueTypeFont::leading(float s) const { return impl->gap * impl->scale(s); }
float TrueTypeFont::advance(int g, float s) const {
    if (g < 0 || g >= glyphCount()) return 0;
    int a, b; stbtt_GetGlyphHMetrics(&impl->info, g, &a, &b);
    return a * impl->scale(s);
}
Rect TrueTypeFont::bounds(int g, float s) const {
    int x0, y0, x1, y1;
    if (g < 0 || g >= glyphCount() || !stbtt_GetGlyphBox(&impl->info, g, &x0, &y0, &x1, &y1)) return {};
    const float k = impl->scale(s);
    return {x0*k, y0*k, (x1-x0)*k, (y1-y0)*k};
}
Path TrueTypeFont::outline(int g, float s) const {
    Path path;
    if (g < 0 || g >= glyphCount()) return path;
    stbtt_vertex* v = nullptr;
    const int n = stbtt_GetGlyphShape(&impl->info, g, &v);
    const float k = impl->scale(s);
    for (int i = 0; i < n; ++i) {
        const Point p{v[i].x*k, v[i].y*k};
        switch (v[i].type) {
            case STBTT_vmove: if (path.hasCurrent) path.close(); path.moveTo(p); break;
            case STBTT_vline: path.lineTo(p); break;
            case STBTT_vcurve: path.quadTo({v[i].cx*k, v[i].cy*k}, p); break;
            case STBTT_vcubic: path.cubicTo({v[i].cx*k, v[i].cy*k}, {v[i].cx1*k, v[i].cy1*k}, p); break;
        }
    }
    if (path.hasCurrent) path.close();
    stbtt_FreeShape(&impl->info, v);
    return path;
}
}
