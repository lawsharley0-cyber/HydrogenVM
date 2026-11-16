// HydrogenVM — ETC1 decoding (Khronos OES_compressed_ETC1_RGB8_texture). See Etc1.hpp.
#include "runtime/android/Etc1.hpp"
#include <algorithm>

namespace Hydrogen::Runtime::Android {
namespace {

// Intensity modifier tables (codeword 0..7), for pixel index values 0..3 as stored
// (MSB:LSB = 00 small +, 01 large +, 10 small -, 11 large -).
constexpr int kModifiers[8][4] = {
    {2, 8, -2, -8},       {5, 17, -5, -17},     {9, 29, -9, -29},     {13, 42, -13, -42},
    {18, 60, -18, -60},   {24, 80, -24, -80},   {33, 106, -33, -106}, {47, 183, -47, -183},
};

inline uint8_t clamp255(int v) { return uint8_t(std::clamp(v, 0, 255)); }
inline int extend4(int v) { return (v << 4) | v; }
inline int extend5(int v) { return (v << 3) | (v >> 2); }

void decodeBlock(const uint8_t* b, uint8_t out[4][4][3]) {
    const uint32_t hi = uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3];
    const uint32_t lo = uint32_t(b[4]) << 24 | uint32_t(b[5]) << 16 | uint32_t(b[6]) << 8 | b[7];
    const bool diff = hi & 2, flip = hi & 1;
    int base[2][3];
    if (diff) {
        // 5-bit base colour plus a signed 3-bit delta for the second sub-block.
        const int r = (hi >> 27) & 31, g = (hi >> 19) & 31, bl = (hi >> 11) & 31;
        auto delta = [](uint32_t v) { return int(v & 4 ? int(v) - 8 : int(v)); };
        const int dr = delta((hi >> 24) & 7), dg = delta((hi >> 16) & 7), db = delta((hi >> 8) & 7);
        base[0][0] = extend5(r); base[0][1] = extend5(g); base[0][2] = extend5(bl);
        base[1][0] = extend5((r + dr) & 31); base[1][1] = extend5((g + dg) & 31); base[1][2] = extend5((bl + db) & 31);
    } else {
        base[0][0] = extend4((hi >> 28) & 15); base[1][0] = extend4((hi >> 24) & 15);
        base[0][1] = extend4((hi >> 20) & 15); base[1][1] = extend4((hi >> 16) & 15);
        base[0][2] = extend4((hi >> 12) & 15); base[1][2] = extend4((hi >> 8) & 15);
    }
    const int table[2] = {int((hi >> 5) & 7), int((hi >> 2) & 7)};
    for (int x = 0; x < 4; ++x)
        for (int y = 0; y < 4; ++y) {
            const int sub = flip ? (y >= 2) : (x >= 2);
            const int bit = x * 4 + y;   // pixels are stored column by column
            const int idx = int(((lo >> (bit + 16)) & 1) << 1 | ((lo >> bit) & 1));
            const int m = kModifiers[table[sub]][idx];
            for (int c = 0; c < 3; ++c) out[y][x][c] = clamp255(base[sub][c] + m);
        }
}

} // namespace

std::vector<uint8_t> decodeEtc1(const uint8_t* data, int w, int h) {
    std::vector<uint8_t> rgb(size_t(std::max(w, 0)) * size_t(std::max(h, 0)) * 3);
    const int bw = (w + 3) / 4, bh = (h + 3) / 4;
    uint8_t px[4][4][3];
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            decodeBlock(data + (size_t(by) * size_t(bw) + size_t(bx)) * 8, px);
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x) {
                    const int X = bx * 4 + x, Y = by * 4 + y;
                    if (X >= w || Y >= h) continue;
                    uint8_t* o = &rgb[(size_t(Y) * size_t(w) + size_t(X)) * 3];
                    o[0] = px[y][x][0]; o[1] = px[y][x][1]; o[2] = px[y][x][2];
                }
        }
    return rgb;
}

} // namespace Hydrogen::Runtime::Android
