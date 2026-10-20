// HydrogenVM — PVRTC1 decompression.
//
// The image is stored as 64-bit blocks (4x4 pixels at 4 bpp, 8x4 at 2 bpp) in
// Morton ("twiddled") order. Each block holds two low-resolution colours, A and
// B, and per-pixel modulation weights. The decoder upsamples the A and B images
// bilinearly (block centres are the sample points, wrapping at the edges) and
// blends A and B per pixel with the modulation weight.
//
// Block layout (little endian): word 0 = modulation bits, word 1 = colour data:
//   bit 31     colour B opaque           bits 30..16 colour B: RGB555 or ARGB3444
//   bit 15     colour A opaque           bits 14..1  colour A: RGB554 or ARGB3443
//   bit 0      modulation mode
#include "Pvrtc.hpp"
#include <algorithm>

namespace Hydrogen::Graphics {
namespace {

struct Colour { int r, g, b, a; };   // r, g, b: 5 bits; a: 4 bits

Colour colourA(uint32_t c) {
    if (c & 0x8000) return {int((c >> 10) & 0x1F), int((c >> 5) & 0x1F), int((c & 0x1E) | ((c & 0x1E) >> 4)), 0xF};
    return {int(((c >> 7) & 0x1E) | ((c >> 11) & 1)), int(((c >> 3) & 0x1E) | ((c >> 7) & 1)),
            int(((c << 1) & 0x1C) | ((c >> 2) & 3)), int((c >> 11) & 0xE)};
}
Colour colourB(uint32_t c) {
    if (c & 0x80000000u) return {int((c >> 26) & 0x1F), int((c >> 21) & 0x1F), int((c >> 16) & 0x1F), 0xF};
    return {int(((c >> 23) & 0x1E) | ((c >> 27) & 1)), int(((c >> 19) & 0x1E) | ((c >> 23) & 1)),
            int(((c >> 15) & 0x1E) | ((c >> 19) & 1)), int((c >> 27) & 0xE)};
}

// Morton index of block (x, y) in a grid of bw x bh blocks (powers of two).
uint32_t twiddle(uint32_t x, uint32_t y, uint32_t bw, uint32_t bh) {
    uint32_t idx = 0, shift = 0, mask = 1;
    const uint32_t minDim = std::min(bw, bh);
    for (; mask < minDim; mask <<= 1) {
        idx |= ((y & mask) << shift) | ((x & mask) << (shift + 1));
        ++shift;
    }
    // The longer axis continues with its remaining bits.
    const uint32_t rest = bw > bh ? (x >> shift) : (y >> shift);
    return idx | (rest << (2 * shift));
}

uint8_t expand5(int v) { return uint8_t((v << 3) | (v >> 2)); }
uint8_t expand4(int v) { return uint8_t((v << 4) | v); }

} // namespace

bool decodePvrtc(const uint8_t* data, size_t n, int width, int height, bool twoBpp, bool withAlpha, std::vector<uint8_t>& rgba) {
    if (width <= 0 || height <= 0) return false;
    const int blockW = twoBpp ? 8 : 4, blockH = 4;
    // The block grid is at least 2x2 blocks.
    const int bw = std::max(2, (width + blockW - 1) / blockW), bh = std::max(2, (height + blockH - 1) / blockH);
    if (n < size_t(bw) * size_t(bh) * 8) return false;
    // Unpack colours, modulation mode and bits for every block.
    std::vector<Colour> ca(size_t(bw) * bh), cb(size_t(bw) * bh);
    std::vector<uint32_t> mod(size_t(bw) * bh), mode(size_t(bw) * bh);
    for (int y = 0; y < bh; ++y) {
        for (int x = 0; x < bw; ++x) {
            const uint8_t* p = data + size_t(twiddle(uint32_t(x), uint32_t(y), uint32_t(bw), uint32_t(bh))) * 8;
            const uint32_t m = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
            const uint32_t c = uint32_t(p[4]) | (uint32_t(p[5]) << 8) | (uint32_t(p[6]) << 16) | (uint32_t(p[7]) << 24);
            const size_t i = size_t(y) * bw + x;
            ca[i] = colourA(c);
            cb[i] = colourB(c);
            mod[i] = m;
            mode[i] = c & 1;
        }
    }
    auto wrapX = [&](int v) { return ((v % bw) + bw) % bw; };
    auto wrapY = [&](int v) { return ((v % bh) + bh) % bh; };
    // 2 bpp, modulation mode 1: only checkerboard pixels carry a 2-bit value.
    auto mod2bpp = [&](int px, int py) -> int {
        const int bx = wrapX(px >= 0 ? px / 8 : (px - 7) / 8), by = wrapY(py >= 0 ? py / 4 : (py - 3) / 4);
        const size_t i = size_t(by) * bw + bx;
        const int lx = ((px % 8) + 8) % 8, ly = ((py % 4) + 4) % 4;
        if (!mode[i]) return (mod[i] >> (ly * 8 + lx)) & 1 ? 8 : 0;
        const int bit = (ly * 8 + lx) & ~1;   // two pixels share one 2-bit value
        static const int kWeights[4] = {0, 3, 5, 8};
        return kWeights[(mod[i] >> bit) & 3];
    };

    rgba.assign(size_t(width) * height * 4, 0);
    for (int y = 0; y < height; ++y) {
        // Sample points are block centres: pixel y sits between rows y0 and y0 + 1.
        const int fy = y - blockH / 2;
        const int y0 = (fy >= 0 ? fy / blockH : (fy - blockH + 1) / blockH);
        const int wy = fy - y0 * blockH;   // 0 .. blockH-1
        for (int x = 0; x < width; ++x) {
            const int fx = x - blockW / 2;
            const int x0 = (fx >= 0 ? fx / blockW : (fx - blockW + 1) / blockW);
            const int wx = fx - x0 * blockW;
            const size_t i00 = size_t(wrapY(y0)) * bw + wrapX(x0), i10 = size_t(wrapY(y0)) * bw + wrapX(x0 + 1);
            const size_t i01 = size_t(wrapY(y0 + 1)) * bw + wrapX(x0), i11 = size_t(wrapY(y0 + 1)) * bw + wrapX(x0 + 1);
            const int w00 = (blockW - wx) * (blockH - wy), w10 = wx * (blockH - wy), w01 = (blockW - wx) * wy, w11 = wx * wy;
            const int total = blockW * blockH;
            auto lerp = [&](const std::vector<Colour>& c, int Colour::*ch) {
                return float(c[i00].*ch * w00 + c[i10].*ch * w10 + c[i01].*ch * w01 + c[i11].*ch * w11) / float(total);
            };
            const float ar = lerp(ca, &Colour::r), ag = lerp(ca, &Colour::g), ab = lerp(ca, &Colour::b), aa = lerp(ca, &Colour::a);
            const float br = lerp(cb, &Colour::r), bg = lerp(cb, &Colour::g), bb = lerp(cb, &Colour::b), ba = lerp(cb, &Colour::a);
            // Modulation weight (0..8) of this pixel, from its own block.
            const int bx = x / blockW, by = y / blockH;
            const size_t bi = size_t(by) * bw + bx;
            int weight;
            bool punchThrough = false;
            if (!twoBpp) {
                const int bits = int((mod[bi] >> (2 * ((y % 4) * 4 + (x % 4)))) & 3);
                if (mode[bi]) {
                    static const int kPunch[4] = {0, 4, 4, 8};
                    weight = kPunch[bits];
                    punchThrough = bits == 2;
                } else {
                    static const int kStd[4] = {0, 3, 5, 8};
                    weight = kStd[bits];
                }
            } else if (mode[bi] && (((x % 8) + (y % 4)) & 1)) {
                // Interpolated pixel: average of the stored neighbours.
                weight = (mod2bpp(x - 1, y) + mod2bpp(x + 1, y) + mod2bpp(x, y - 1) + mod2bpp(x, y + 1) + 2) / 4;
            } else {
                weight = mod2bpp(x, y);
            }
            const float t = float(weight) / 8.0f;
            auto mix = [&](float a, float b) { return a + (b - a) * t; };
            uint8_t* out = &rgba[(size_t(y) * width + x) * 4];
            out[0] = expand5(std::clamp(int(mix(ar, br) + 0.5f), 0, 31));
            out[1] = expand5(std::clamp(int(mix(ag, bg) + 0.5f), 0, 31));
            out[2] = expand5(std::clamp(int(mix(ab, bb) + 0.5f), 0, 31));
            out[3] = !withAlpha ? 255 : punchThrough ? 0 : expand4(std::clamp(int(mix(aa, ba) + 0.5f), 0, 15));
        }
    }
    return true;
}

} // namespace Hydrogen::Graphics
