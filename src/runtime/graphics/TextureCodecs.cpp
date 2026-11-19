// ETC2 mode bit extraction was adapted from AssetStudio/Texture2DDecoderNative.
// https://github.com/Perfare/AssetStudio/blob/master/Texture2DDecoderNative/etc.cpp
// MIT License
//
// Copyright (c) 2016 Radu
// Copyright (c) 2016-2020 Perfare
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "runtime/graphics/TextureCodecs.hpp"
#include "runtime/android/Etc1.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>

namespace Hydrogen::Graphics {
namespace {

enum class Codec { Eac, Etc2, Etc2A1, Etc2A8, Dxt, Atc, Pvrtc };
enum class Alpha { Opaque, PunchThrough, Explicit, Interpolated, Full };
struct Format {
    Codec codec;
    int blockWidth = 4, blockBytes = 8;
    Alpha alpha = Alpha::Opaque;
    bool srgb = false;
    bool signedEac = false;
};

std::optional<Format> describe(uint32_t token) {
    switch (token) {
        case 0x9270: return Format{Codec::Eac};                                    // R11
        case 0x9271: return Format{Codec::Eac, 4, 8, Alpha::Opaque, false, true};  // signed R11
        case 0x9272: return Format{Codec::Eac, 4, 16};                            // RG11
        case 0x9273: return Format{Codec::Eac, 4, 16, Alpha::Opaque, false, true}; // signed RG11
        case 0x9274: case 0x9275: return Format{Codec::Etc2, 4, 8, Alpha::Opaque, token == 0x9275};
        case 0x9276: case 0x9277: return Format{Codec::Etc2A1, 4, 8, Alpha::PunchThrough, token == 0x9277};
        case 0x9278: case 0x9279: return Format{Codec::Etc2A8, 4, 16, Alpha::Full, token == 0x9279};
        case 0x83F0: case 0x8C4C: return Format{Codec::Dxt, 4, 8, Alpha::Opaque, token == 0x8C4C};
        case 0x83F1: case 0x8C4D: return Format{Codec::Dxt, 4, 8, Alpha::PunchThrough, token == 0x8C4D};
        case 0x83F2: case 0x8C4E: return Format{Codec::Dxt, 4, 16, Alpha::Explicit, token == 0x8C4E};
        case 0x83F3: case 0x8C4F: return Format{Codec::Dxt, 4, 16, Alpha::Interpolated, token == 0x8C4F};
        case 0x8C92: return Format{Codec::Atc};
        case 0x8C93: return Format{Codec::Atc, 4, 16, Alpha::Explicit};
        case 0x87EE: return Format{Codec::Atc, 4, 16, Alpha::Interpolated};
        case 0x8C00: return Format{Codec::Pvrtc};
        case 0x8C01: return Format{Codec::Pvrtc, 8};
        case 0x8C02: return Format{Codec::Pvrtc, 4, 8, Alpha::Full};
        case 0x8C03: return Format{Codec::Pvrtc, 8, 8, Alpha::Full};
        default: return std::nullopt;
    }
}

struct BlockGrid { int columns, rows; };
std::optional<BlockGrid> blockGrid(const Format& f, int w, int h) {
    if (w <= 0 || h <= 0) return std::nullopt;
    const int columns = (w - 1) / f.blockWidth + 1, rows = (h - 1) / 4 + 1;
    if (f.codec != Codec::Pvrtc) return BlockGrid{columns, rows};
    // IMG_texture_compression_pvrtc allows sub-block mip levels but requires a 2×2
    // stored word footprint and rejects NPOT texture dimensions.
    if (!std::has_single_bit(unsigned(w)) || !std::has_single_bit(unsigned(h))) return std::nullopt;
    return BlockGrid{std::max(2, columns), std::max(2, rows)};
}

uint32_t little(const uint8_t* data, int bytes) {
    uint32_t value = 0;
    for (int i = 0; i < bytes; ++i) value |= uint32_t(data[i]) << (8 * i);
    return value;
}

uint8_t clamp8(int value) { return uint8_t(std::clamp(value, 0, 255)); }
uint8_t expand(int value, int bits) { return uint8_t((value << (8 - bits)) | (value >> (2 * bits - 8))); }

uint32_t pack(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return uint32_t(r) | uint32_t(g) << 8 | uint32_t(b) << 16 | uint32_t(a) << 24;
}

constexpr int kEtcDistances[8] = {3, 6, 11, 16, 23, 32, 41, 64};
constexpr int kEacModifiers[16][8] = {
  {-3, -6, -9, -15, 2, 5, 8, 14}, {-3, -7, -10, -13, 2, 6, 9, 12}, {-2, -5, -8, -13, 1, 4, 7, 12},
  {-2, -4, -6, -13, 1, 3, 5, 12}, {-3, -6, -8, -12, 2, 5, 7, 11},  {-3, -7, -9, -11, 2, 6, 8, 10},
  {-4, -7, -8, -11, 3, 6, 7, 10}, {-3, -5, -8, -11, 2, 4, 7, 10},  {-2, -6, -8, -10, 1, 5, 7, 9},
  {-2, -5, -8, -10, 1, 4, 7, 9},  {-2, -4, -8, -10, 1, 3, 7, 9},   {-2, -5, -7, -10, 1, 4, 6, 9},
  {-3, -4, -7, -10, 2, 3, 6, 9},  {-1, -2, -3, -10, 0, 1, 2, 9},   {-4, -6, -8, -9, 3, 5, 7, 8},
  {-3, -5, -7, -9, 2, 4, 6, 8}};

constexpr int kEtcPunchIntensity[] = {8, 17, 29, 42, 60, 80, 106, 183};

unsigned etcPaletteIndex(uint32_t indices, int x, int y) {
    const int bit = x * 4 + y;
    return ((indices >> (bit + 15)) & 2) | ((indices >> bit) & 1);
}

void copyPalette(uint32_t* out, uint32_t indices, const uint32_t (&palette)[4], bool transparent) {
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            const auto index = etcPaletteIndex(indices, x, y);
            out[y * 4 + x] = transparent && index == 2 ? 0 : palette[index];
        }
}

uint32_t offsetColor(const uint8_t (&c)[3], int offset) {
    return pack(clamp8(c[0] + offset), clamp8(c[1] + offset), clamp8(c[2] + offset));
}

int etcDifferential(uint8_t encoded) {
    const int delta = encoded & 7;
    return (encoded >> 3) + (delta < 4 ? delta : delta - 8);
}

void decodeEtc2(const uint8_t* b, uint32_t* out, bool punchThrough) {
    const bool differential = punchThrough || (b[3] & 2);
    const int red = etcDifferential(b[0]), green = etcDifferential(b[1]), blue = etcDifferential(b[2]);
    const auto overflow = [](int channel) { return channel < 0 || channel > 31; };
    const bool redOverflow = differential && overflow(red);
    const bool greenOverflow = differential && overflow(green);
    const bool blueOverflow = differential && overflow(blue);
    const bool transparent = punchThrough && !(b[3] & 2);
    const uint32_t indices = uint32_t(b[4]) << 24 | uint32_t(b[5]) << 16 | uint32_t(b[6]) << 8 | b[7];
    if (!redOverflow && !greenOverflow && !blueOverflow) {
        if (!transparent) {
            uint8_t rgb[4][4][3];
            Runtime::Android::decodeEtc1Block(b, rgb);
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x)
                    out[y * 4 + x] = pack(rgb[y][x][0], rgb[y][x][1], rgb[y][x][2]);
            return;
        }
        const uint8_t base[2][3] = {
            {expand(b[0] >> 3, 5), expand(b[1] >> 3, 5), expand(b[2] >> 3, 5)},
            {expand(red, 5), expand(green, 5), expand(blue, 5)},
        };
        const int table[2] = {b[3] >> 5, (b[3] >> 2) & 7};
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) {
                const int sub = (b[3] & 1) ? y / 2 : x / 2;
                const auto index = etcPaletteIndex(indices, x, y);
                const int modifier = index & 1 ? kEtcPunchIntensity[table[sub]] : 0;
                out[y * 4 + x] = index == 2 ? 0 : offsetColor(base[sub], index & 2 ? -modifier : modifier);
            }
        return;
    }
    uint8_t c[3][3]{};
    if (redOverflow) {
        // T
        c[0][0] = (b[0] << 3 & 0xc0) | (b[0] << 4 & 0x30) | (b[0] >> 1 & 0xc) | (b[0] & 3);
        c[0][1] = (b[1] & 0xf0) | b[1] >> 4;
        c[0][2] = (b[1] & 0x0f) | b[1] << 4;
        c[1][0] = (b[2] & 0xf0) | b[2] >> 4;
        c[1][1] = (b[2] & 0x0f) | b[2] << 4;
        c[1][2] = (b[3] & 0xf0) | b[3] >> 4;
        const uint8_t d = kEtcDistances[(b[3] >> 1 & 6) | (b[3] & 1)];
        uint32_t palette[4] = {offsetColor(c[0], 0), offsetColor(c[1], d),
                                      offsetColor(c[1], 0), offsetColor(c[1], -d)};
        copyPalette(out, indices, palette, transparent);
    } else if (greenOverflow) {
        // H
        c[0][0] = (b[0] << 1 & 0xf0) | (b[0] >> 3 & 0xf);
        c[0][1] = (b[0] << 5 & 0xe0) | (b[1] & 0x10);
        c[0][1] |= c[0][1] >> 4;
        c[0][2] = (b[1] & 8) | (b[1] << 1 & 6) | b[2] >> 7;
        c[0][2] |= c[0][2] << 4;
        c[1][0] = (b[2] << 1 & 0xf0) | (b[2] >> 3 & 0xf);
        c[1][1] = (b[2] << 5 & 0xe0) | (b[3] >> 3 & 0x10);
        c[1][1] |= c[1][1] >> 4;
        c[1][2] = (b[3] << 1 & 0xf0) | (b[3] >> 3 & 0xf);
        uint8_t d = (b[3] & 4) | (b[3] << 1 & 2);
        if (c[0][0] > c[1][0] ||
            (c[0][0] == c[1][0] && (c[0][1] > c[1][1] || (c[0][1] == c[1][1] && c[0][2] >= c[1][2]))))
            ++d;
        d = kEtcDistances[d];
        uint32_t palette[4] = {offsetColor(c[0], d), offsetColor(c[0], -d), offsetColor(c[1], d),
                                      offsetColor(c[1], -d)};
        copyPalette(out, indices, palette, transparent);
    } else {
        // planar
        c[0][0] = (b[0] << 1 & 0xfc) | (b[0] >> 5 & 3);
        c[0][1] = (b[0] << 7 & 0x80) | (b[1] & 0x7e) | (b[0] & 1);
        c[0][2] = (b[1] << 7 & 0x80) | (b[2] << 2 & 0x60) | (b[2] << 3 & 0x18) | (b[3] >> 5 & 4);
        c[0][2] |= c[0][2] >> 6;
        c[1][0] = (b[3] << 1 & 0xf8) | (b[3] << 2 & 4) | (b[3] >> 5 & 3);
        c[1][1] = (b[4] & 0xfe) | b[4] >> 7;
        c[1][2] = (b[4] << 7 & 0x80) | (b[5] >> 1 & 0x7c);
        c[1][2] |= c[1][2] >> 6;
        c[2][0] = (b[5] << 5 & 0xe0) | (b[6] >> 3 & 0x1c) | (b[5] >> 1 & 3);
        c[2][1] = (b[6] << 3 & 0xf8) | (b[7] >> 5 & 0x6) | (b[6] >> 4 & 1);
        c[2][2] = b[7] << 2 | (b[7] >> 4 & 3);
        for (int y = 0, i = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++, i++) {
                uint8_t r = clamp8((x * (c[1][0] - c[0][0]) + y * (c[2][0] - c[0][0]) + 4 * c[0][0] + 2) >> 2);
                uint8_t g = clamp8((x * (c[1][1] - c[0][1]) + y * (c[2][1] - c[0][1]) + 4 * c[0][1] + 2) >> 2);
                uint8_t blue = clamp8((x * (c[1][2] - c[0][2]) + y * (c[2][2] - c[0][2]) + 4 * c[0][2] + 2) >> 2);
                out[i] = pack(r, g, blue);
            }
        }
    }
}

int eacModifier(const uint8_t* data, uint64_t selectors, int x, int y) {
    return kEacModifiers[data[1] & 15][(selectors >> (45 - 3 * (x * 4 + y))) & 7];
}

uint64_t eacSelectors(const uint8_t* data) {
    uint64_t value = 0;
    for (int i = 2; i < 8; ++i) value = (value << 8) | data[i];
    return value;
}

void decodeEacAlpha(const uint8_t* data, uint32_t* out) {
    const uint64_t selectors = eacSelectors(data);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            const auto alpha = clamp8(data[0] + (data[1] >> 4) * eacModifier(data, selectors, x, y));
            auto& pixel = out[y * 4 + x];
            pixel = (pixel & 0xFFFFFF) | (uint32_t(alpha) << 24);
        }
}

void decodeEacChannel(const uint8_t* data, float* out, int components, bool signedChannel) {
    int base = data[0];
    if (signedChannel && base >= 128) base = std::max(-127, base - 256);
    const int multiplier = std::max(1, (data[1] >> 4) * 8);
    const uint64_t selectors = eacSelectors(data);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            const int value = base * 8 + multiplier * eacModifier(data, selectors, x, y);
            out[(y * 4 + x) * components] = signedChannel
                ? float(std::clamp(value, -1023, 1023)) / 1023.0f
                : float(std::clamp(value + 4, 0, 2047)) / 2047.0f;
        }
}

using RGB = std::array<uint8_t, 3>;
RGB rgb565(uint32_t value) {
    return {expand(value >> 11, 5), expand((value >> 5) & 63, 6), expand(value & 31, 5)};
}
RGB rgb555(uint32_t value) {
    return {expand((value >> 10) & 31, 5), expand((value >> 5) & 31, 5), expand(value & 31, 5)};
}
RGB blend(RGB a, RGB b, int weightA, int weightB) {
    RGB result{};
    for (size_t i = 0; i < result.size(); ++i)
        result[i] = uint8_t((a[i] * weightA + b[i] * weightB) / (weightA + weightB));
    return result;
}

void decodeDxtColors(const uint8_t* data, uint32_t* out, const Format& format) {
    const uint32_t first = little(data, 2), second = little(data + 2, 2);
    const auto a = rgb565(first), b = rgb565(second);
    const bool threeColors = format.blockBytes == 8 && first <= second;
    const RGB palette[] = {a, b, threeColors ? blend(a, b, 1, 1) : blend(a, b, 2, 1),
                           threeColors ? RGB{} : blend(a, b, 1, 2)};
    uint32_t selectors = little(data + 4, 4);
    for (int i = 0; i < 16; ++i, selectors >>= 2) {
        const auto index = selectors & 3;
        const auto& pixel = palette[index];
        const uint8_t alpha = index == 3 && threeColors && format.alpha == Alpha::PunchThrough ? 0 : 255;
        out[i] = pack(pixel[0], pixel[1], pixel[2], alpha);
    }
}

void decodeAtcColors(const uint8_t* data, uint32_t* out) {
    const uint32_t first = little(data, 2);
    const auto a = rgb555(first), b = rgb565(little(data + 2, 2));
    RGB palette[] = {a, blend(a, b, 5, 3), blend(a, b, 3, 5), b};
    if (first & 0x8000) {
        palette[0] = {};
        palette[2] = a;
        for (size_t i = 0; i < a.size(); ++i) palette[1][i] = clamp8(a[i] - b[i] / 4);
    }
    uint32_t selectors = little(data + 4, 4);
    for (int i = 0; i < 16; ++i, selectors >>= 2) {
        const auto& rgb = palette[selectors & 3];
        out[i] = pack(rgb[0], rgb[1], rgb[2]);
    }
}

void decodeSeparateAlpha(const uint8_t* data, uint32_t* out, Alpha mode) {
    if (mode == Alpha::Explicit) {
        for (int i = 0; i < 16; ++i) {
            const uint32_t nibble = (data[i / 2] >> ((i % 2) * 4)) & 15;
            out[i] = (out[i] & 0xFFFFFF) | ((nibble * 17) << 24);
        }
        return;
    }
    const unsigned a = data[0], b = data[1];
    unsigned palette[8]{a, b};
    const unsigned denominator = a > b ? 7 : 5;
    for (unsigned i = 1; i < denominator; ++i)
        palette[i + 1] = ((denominator - i) * a + i * b + denominator / 2) / denominator;
    if (a <= b) palette[7] = 255;
    uint64_t selectors = little(data + 2, 4) | (uint64_t(little(data + 6, 2)) << 32);
    for (int i = 0; i < 16; ++i, selectors >>= 3)
        out[i] = (out[i] & 0xFFFFFF) | (uint32_t(palette[selectors & 7]) << 24);
}

// PVRTC1 uses two endpoint planes, wrapped bilinear interpolation between
// word centers, and modulation sampled on the original pixel lattice.
using PvColor = std::array<int, 4>;
struct PvWord {
    PvColor a, b;
    uint32_t modulation;
    bool alternate;
};

PvWord pvWord(const uint8_t* bytes) {
    const auto first = little(bytes + 4, 2), second = little(bytes + 6, 2);
    PvColor a, b;
    if (first & 0x8000) {
        a = {int((first >> 10) & 31), int((first >> 5) & 31),
             int((first & 30) | ((first >> 4) & 1)), 15};
    } else {
        a = {int(((first >> 7) & 30) | ((first >> 11) & 1)),
             int(((first >> 3) & 30) | ((first >> 7) & 1)),
             int(((first << 1) & 28) | ((first >> 2) & 3)), int((first >> 11) & 14)};
    }
    if (second & 0x8000) {
        b = {int((second >> 10) & 31), int((second >> 5) & 31), int(second & 31), 15};
    } else {
        b = {int(((second >> 7) & 30) | ((second >> 11) & 1)),
             int(((second >> 3) & 30) | ((second >> 7) & 1)),
             int(((second << 1) & 30) | ((second >> 3) & 1)), int((second >> 11) & 14)};
    }
    return {a, b, little(bytes, 4), bool(first & 1)};
}

size_t pvMorton(unsigned x, unsigned y, BlockGrid grid) {
    size_t index = 0;
    unsigned bit = 0;
    const unsigned minor = std::min(grid.columns, grid.rows);
    for (unsigned mask = 1; mask < minor; mask <<= 1, ++bit)
        index |= (size_t(y & mask) | size_t(x & mask) << 1) << bit;
    return index | (size_t((x | y) >> bit) << (2 * bit));
}

int pvStoredWeight(const PvWord& word, int x, int y) {
    if (!word.alternate) return (word.modulation >> (y * 8 + x)) & 1 ? 8 : 0;
    const int slot = (y * 8 + x) & ~1;
    const unsigned selector = (word.modulation >> slot) & 3;
    if (slot == 0 || ((word.modulation & 1) && slot == 20)) return selector & 2 ? 8 : 0;
    constexpr int weights[] = {0, 3, 5, 8};
    return weights[selector];
}

void storeColor(uint8_t* out, uint32_t color) {
    for (int i = 0; i < 4; ++i) out[i] = uint8_t(color >> (8 * i));
}

void decodePvrtc1(const uint8_t* data, int w, int h, BlockGrid grid, const Format& f, uint8_t* out) {
    const bool twoBpp = f.blockWidth == 8;
    const int paddedWidth = grid.columns * f.blockWidth, paddedHeight = grid.rows * 4;
    const auto wrap = [](int position, int dimension) { return position & (dimension - 1); };
    std::vector<PvWord> words(size_t(grid.columns) * grid.rows);
    // Untwiddle once, then use constant-time neighbors for each output texel.
    for (int y = 0; y < grid.rows; ++y)
        for (int x = 0; x < grid.columns; ++x)
            words[size_t(y) * grid.columns + x] = pvWord(data + pvMorton(x, y, grid) * 8);
    auto wordAt = [&](int x, int y) -> const PvWord& {
        const int column = wrap(x, paddedWidth) / f.blockWidth, row = wrap(y, paddedHeight) / 4;
        return words[size_t(row) * grid.columns + column];
    };
    auto storedWeight = [&](int x, int y) {
        return pvStoredWeight(wordAt(x, y), wrap(x, 8), wrap(y, 4));
    };
    const int blockWidth = f.blockWidth;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const auto& word = wordAt(x, y);
            bool punchThrough = false;
            int weight;
            if (!twoBpp) {
                constexpr int normal[] = {0, 3, 5, 8}, punch[] = {0, 4, 4, 8};
                const unsigned index = (word.modulation >> (2 * ((y % 4) * 4 + x % 4))) & 3;
                punchThrough = word.alternate && index == 2;
                weight = word.alternate ? punch[index] : normal[index];
            } else if (!word.alternate || ((x ^ y) & 1) == 0) {
                weight = storedWeight(x, y);
            } else {
                const bool directional = word.modulation & 1;
                const bool vertical = word.modulation & (1u << 20);
                const int horizontalPair = storedWeight(x - 1, y) + storedWeight(x + 1, y);
                const int verticalPair = storedWeight(x, y - 1) + storedWeight(x, y + 1);
                weight = directional ? ((vertical ? verticalPair : horizontalPair) + 1) / 2
                                     : (horizontalPair + verticalPair + 2) / 4;
            }

            const int fx = x - blockWidth / 2, fy = y - 2;
            const int dx = wrap(fx, blockWidth), dy = wrap(fy, 4);
            const int x0 = fx - dx, y0 = fy - dy;
            const auto& nw = wordAt(x0, y0);
            const auto& ne = wordAt(x0 + blockWidth, y0);
            const auto& sw = wordAt(x0, y0 + 4);
            const auto& se = wordAt(x0 + blockWidth, y0 + 4);
            const auto lerp = [&](PvColor PvWord::* endpoint, int channel) {
                const int north = (nw.*endpoint)[channel] * (blockWidth - dx) + (ne.*endpoint)[channel] * dx;
                const int south = (sw.*endpoint)[channel] * (blockWidth - dx) + (se.*endpoint)[channel] * dx;
                const int sum = north * (4 - dy) + south * dy;
                // Expand interpolated 5-bit color or 4-bit alpha before
                // blending the endpoint planes, retaining fractional bits.
                if (channel == 3) return (sum >> (twoBpp ? 1 : 0)) + (sum >> (twoBpp ? 5 : 4));
                return (sum >> (twoBpp ? 2 : 1)) + (sum >> (twoBpp ? 7 : 6));
            };
            auto* pixel = out + (size_t(y) * w + x) * 4;
            for (int c = 0; c < 4; ++c)
                pixel[c] = uint8_t((lerp(&PvWord::a, c) * (8 - weight) + lerp(&PvWord::b, c) * weight) / 8);
            if (f.alpha == Alpha::Opaque) pixel[3] = 255;
            else if (punchThrough) pixel[3] = 0;
        }
}

} // namespace

std::optional<size_t> compressedTextureSize(uint32_t format, int width, int height) {
    const auto f = describe(format);
    if (!f) return std::nullopt;
    const auto grid = blockGrid(*f, width, height);
    if (!grid || size_t(grid->columns) > std::numeric_limits<size_t>::max() / grid->rows / f->blockBytes)
        return std::nullopt;
    return size_t(grid->columns) * grid->rows * f->blockBytes;
}

std::optional<DecodedTexture> decodeCompressedTexture(uint32_t format, int width, int height,
                                                      std::span<const uint8_t> data) {
    const auto f = describe(format);
    const auto bytes = compressedTextureSize(format, width, height);
    if (!f || !bytes || data.size() != *bytes) return std::nullopt;
    const auto grid = *blockGrid(*f, width, height);
    DecodedTexture decoded;
    decoded.srgb = f->srgb;
    const bool eac = f->codec == Codec::Eac;
    if (eac) decoded.components = f->blockBytes / 8;
    const size_t components = decoded.components;
    const size_t max = eac ? decoded.redGreen.max_size() : decoded.rgba.max_size();
    if (size_t(width) > max / height / components) return std::nullopt;
    const size_t samples = size_t(width) * height * components;
    if (eac) decoded.redGreen.resize(samples);
    else decoded.rgba.resize(samples);

    if (f->codec == Codec::Pvrtc) {
        decodePvrtc1(data.data(), width, height, grid, *f, decoded.rgba.data());
        return decoded;
    }
    const uint8_t* block = data.data();
    for (int by = 0; by < grid.rows; ++by)
        for (int bx = 0; bx < grid.columns; ++bx, block += f->blockBytes) {
            uint32_t rgba[16];
            float redGreen[32];
            switch (f->codec) {
                case Codec::Eac:
                    for (int channel = 0; channel < decoded.components; ++channel)
                        decodeEacChannel(block + channel * 8, redGreen + channel, decoded.components, f->signedEac);
                    break;
                case Codec::Etc2:
                case Codec::Etc2A1:
                    decodeEtc2(block, rgba, f->codec == Codec::Etc2A1);
                    break;
                case Codec::Etc2A8:
                    decodeEtc2(block + 8, rgba, false);
                    decodeEacAlpha(block, rgba);
                    break;
                case Codec::Dxt:
                case Codec::Atc: {
                    const auto* colors = block + f->blockBytes - 8;
                    if (f->codec == Codec::Dxt) decodeDxtColors(colors, rgba, *f);
                    else decodeAtcColors(colors, rgba);
                    if (f->blockBytes == 16) decodeSeparateAlpha(block, rgba, f->alpha);
                    break;
                }
                case Codec::Pvrtc: break;
            }
            for (int y = 0; y < std::min(4, height - by * 4); ++y)
                for (int x = 0; x < std::min(4, width - bx * 4); ++x) {
                    const size_t index = (size_t(by * 4 + y) * width + bx * 4 + x) * components;
                    if (eac) std::copy_n(redGreen + (y * 4 + x) * components, components,
                                         decoded.redGreen.data() + index);
                    else storeColor(decoded.rgba.data() + index, rgba[y * 4 + x]);
                }
        }
    return decoded;
}

} // namespace Hydrogen::Graphics
