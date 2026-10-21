// HydrogenVM — PNG decoding (all standard colour types and bit depths, Adam7
// interlacing, tRNS, and Apple's "CgBI" Xcode-crushed variant found in most
// IPAs) and encoding (for screenshots), plus the image-decoder dispatcher.
#include "Graphics.hpp"
#include <cstring>
#include <mutex>
#include <zlib.h>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#endif
#define STBI_ONLY_JPEG
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "thirdparty/stb/stb_image.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace Hydrogen::Graphics {

namespace {

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

bool inflateAll(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, bool raw, size_t expected) {
    z_stream zs{};
    if (inflateInit2(&zs, raw ? -15 : 15) != Z_OK) return false;
    out.resize(expected ? expected : in.size() * 4 + 1024);
    zs.next_in = const_cast<Bytef*>(in.data());
    zs.avail_in = uInt(in.size());
    size_t written = 0;
    int rc;
    do {
        if (written == out.size()) out.resize(out.size() * 2);
        zs.next_out = out.data() + written;
        zs.avail_out = uInt(out.size() - written);
        rc = inflate(&zs, Z_NO_FLUSH);
        written = out.size() - zs.avail_out;
        if (rc == Z_BUF_ERROR && zs.avail_in == 0) break;   // truncated stream: keep what we have
    } while (rc == Z_OK || (rc == Z_BUF_ERROR && zs.avail_out == 0));
    inflateEnd(&zs);
    out.resize(written);
    return rc == Z_STREAM_END || rc == Z_BUF_ERROR || written > 0;
}

uint8_t paeth(int a, int b, int c) {
    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return uint8_t(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

bool unfilter(uint8_t* data, size_t rowBytes, size_t rows, size_t bpp, size_t available) {
    if ((rowBytes + 1) * rows > available) return false;
    std::vector<uint8_t> prev(rowBytes, 0);
    uint8_t* out = data;   // in-place: rows are compacted (filter byte removed)
    const uint8_t* in = data;
    for (size_t y = 0; y < rows; ++y) {
        const uint8_t ft = in[0];
        const uint8_t* src = in + 1;
        std::vector<uint8_t> cur(src, src + rowBytes);
        for (size_t i = 0; i < rowBytes; ++i) {
            const int a = i >= bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0;
            switch (ft) {
                case 0: break;
                case 1: cur[i] = uint8_t(cur[i] + a); break;
                case 2: cur[i] = uint8_t(cur[i] + b); break;
                case 3: cur[i] = uint8_t(cur[i] + ((a + b) >> 1)); break;
                case 4: cur[i] = uint8_t(cur[i] + paeth(a, b, c)); break;
                default: return false;
            }
        }
        std::memcpy(out, cur.data(), rowBytes);
        prev = std::move(cur);
        out += rowBytes;
        in += rowBytes + 1;
    }
    return true;
}

} // namespace

ImagePtr decodePng(const uint8_t* d, size_t n, std::string* error) {
    auto fail = [&](const char* why) -> ImagePtr { if (error) *error = why; return nullptr; };
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (n < 8 || std::memcmp(d, sig, 8) != 0) return fail("not a PNG");
    size_t p = 8;
    uint32_t w = 0, h = 0;
    int depth = 0, ctype = 0, interlace = 0;
    bool cgbi = false;
    std::vector<uint8_t> idat, palette, trns;
    while (p + 8 <= n) {
        const uint32_t len = be32(d + p);
        const uint8_t* type = d + p + 4;
        const uint8_t* body = d + p + 8;
        if (p + 12 + size_t(len) > n) break;
        if (!std::memcmp(type, "CgBI", 4)) cgbi = true;
        else if (!std::memcmp(type, "IHDR", 4) && len >= 13) {
            w = be32(body); h = be32(body + 4); depth = body[8]; ctype = body[9]; interlace = body[12];
        } else if (!std::memcmp(type, "PLTE", 4)) palette.assign(body, body + len);
        else if (!std::memcmp(type, "tRNS", 4)) trns.assign(body, body + len);
        else if (!std::memcmp(type, "IDAT", 4)) idat.insert(idat.end(), body, body + len);
        else if (!std::memcmp(type, "IEND", 4)) break;
        p += 12 + size_t(len);
    }
    if (!w || !h || w > 16384 || h > 16384) return fail("bad PNG dimensions");
    const int channels = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : ctype == 6 ? 4 : 0;
    if (!channels || !(depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16)) return fail("unsupported PNG format");
    const size_t bitsPerPixel = size_t(channels) * size_t(depth);
    const size_t bpp = std::max<size_t>(1, bitsPerPixel / 8);

    auto rowBytesFor = [&](uint32_t pw) { return (size_t(pw) * bitsPerPixel + 7) / 8; };
    size_t expected = 0;
    if (!interlace) expected = (rowBytesFor(w) + 1) * h;
    std::vector<uint8_t> raw;
    // CgBI streams are raw deflate (no zlib header); fall back to the other kind if needed.
    if (!inflateAll(idat, raw, cgbi, expected) || raw.empty()) {
        if (!inflateAll(idat, raw, !cgbi, expected) || raw.empty()) return fail("PNG inflate failed");
    }

    auto img = std::make_shared<Image>();
    img->width = int(w);
    img->height = int(h);
    img->rgba.assign(size_t(w) * h * 4, 0);

    auto sample = [&](const uint8_t* row, uint32_t x, int ch) -> uint32_t {   // returns 8-bit value (or 16-bit for tRNS compare)
        if (depth == 8) return row[x * size_t(channels) + size_t(ch)];
        if (depth == 16) return uint32_t(row[(x * size_t(channels) + size_t(ch)) * 2]) << 8 | row[(x * size_t(channels) + size_t(ch)) * 2 + 1];
        const size_t bit = size_t(x) * size_t(depth);
        const uint32_t v = (row[bit / 8] >> (8 - depth - int(bit % 8))) & ((1u << depth) - 1);
        return v;
    };
    auto to8 = [&](uint32_t v) -> uint8_t {
        if (depth == 16) return uint8_t(v >> 8);
        if (depth == 8) return uint8_t(v);
        return uint8_t(v * 255 / ((1u << depth) - 1));
    };
    auto put = [&](const uint8_t* row, uint32_t px, uint32_t x, uint32_t y) {
        uint8_t* o = &img->rgba[(size_t(y) * w + x) * 4];
        uint8_t r, g, b, a = 255;
        switch (ctype) {
            case 0: {
                const uint32_t v = sample(row, px, 0);
                r = g = b = to8(v);
                if (trns.size() >= 2 && v == (uint32_t(trns[0]) << 8 | trns[1])) a = 0;
                break;
            }
            case 2: {
                const uint32_t vr = sample(row, px, 0), vg = sample(row, px, 1), vb = sample(row, px, 2);
                r = to8(vr); g = to8(vg); b = to8(vb);
                if (trns.size() >= 6) {
                    const uint32_t tr = uint32_t(trns[0]) << 8 | trns[1], tg = uint32_t(trns[2]) << 8 | trns[3], tb = uint32_t(trns[4]) << 8 | trns[5];
                    if (vr == tr && vg == tg && vb == tb) a = 0;
                }
                break;
            }
            case 3: {
                const uint32_t i = sample(row, px, 0);
                if (size_t(i) * 3 + 2 < palette.size()) { r = palette[i * 3]; g = palette[i * 3 + 1]; b = palette[i * 3 + 2]; }
                else r = g = b = 0;
                if (i < trns.size()) a = trns[i];
                break;
            }
            case 4: r = g = b = to8(sample(row, px, 0)); a = to8(sample(row, px, 1)); break;
            default: r = to8(sample(row, px, 0)); g = to8(sample(row, px, 1)); b = to8(sample(row, px, 2)); a = to8(sample(row, px, 3)); break;
        }
        if (cgbi && channels == 4) {
            // CgBI stores BGRA, already premultiplied.
            o[0] = b; o[1] = g; o[2] = r; o[3] = a;
        } else if (cgbi && channels == 3) {
            o[0] = b; o[1] = g; o[2] = r; o[3] = a;
        } else {
            o[0] = uint8_t((r * a + 127) / 255); o[1] = uint8_t((g * a + 127) / 255); o[2] = uint8_t((b * a + 127) / 255); o[3] = a;
        }
    };

    if (!interlace) {
        const size_t rb = rowBytesFor(w);
        if (!unfilter(raw.data(), rb, h, bpp, raw.size())) return fail("PNG data corrupt");
        for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) put(raw.data() + size_t(y) * rb, x, x, y);
    } else {
        static const int sx[7] = {0, 4, 0, 2, 0, 1, 0}, sy[7] = {0, 0, 4, 0, 2, 0, 1}, dx[7] = {8, 8, 4, 4, 2, 2, 1}, dy[7] = {8, 8, 8, 4, 4, 2, 2};
        size_t off = 0;
        for (int pass = 0; pass < 7; ++pass) {
            const uint32_t pw = (w - uint32_t(sx[pass]) + uint32_t(dx[pass]) - 1) / uint32_t(dx[pass]);
            const uint32_t ph = (h - uint32_t(sy[pass]) + uint32_t(dy[pass]) - 1) / uint32_t(dy[pass]);
            if (w <= uint32_t(sx[pass]) || h <= uint32_t(sy[pass]) || !pw || !ph) continue;
            const size_t rb = rowBytesFor(pw);
            if (off >= raw.size() || !unfilter(raw.data() + off, rb, ph, bpp, raw.size() - off)) return fail("PNG interlaced data corrupt");
            for (uint32_t y = 0; y < ph; ++y)
                for (uint32_t x = 0; x < pw; ++x) put(raw.data() + off + size_t(y) * rb, x, uint32_t(sx[pass]) + x * uint32_t(dx[pass]), uint32_t(sy[pass]) + y * uint32_t(dy[pass]));
            off += (rb + 1) * ph;
        }
    }
    bool opaque = true;
    for (size_t i = 3; i < img->rgba.size(); i += 4) if (img->rgba[i] != 255) { opaque = false; break; }
    img->opaque = opaque;
    return img;
}

std::vector<uint8_t> encodePng(const Image& img) {
    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    auto chunk = [&](const char* type, const std::vector<uint8_t>& body) {
        const uint32_t len = uint32_t(body.size());
        for (int i = 3; i >= 0; --i) out.push_back(uint8_t(len >> (i * 8)));
        const size_t start = out.size();
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), body.begin(), body.end());
        const uint32_t crc = uint32_t(crc32(0, out.data() + start, uInt(out.size() - start)));
        for (int i = 3; i >= 0; --i) out.push_back(uint8_t(crc >> (i * 8)));
    };
    std::vector<uint8_t> ihdr(13);
    for (int i = 0; i < 4; ++i) { ihdr[size_t(i)] = uint8_t(uint32_t(img.width) >> ((3 - i) * 8)); ihdr[size_t(4 + i)] = uint8_t(uint32_t(img.height) >> ((3 - i) * 8)); }
    ihdr[8] = 8; ihdr[9] = 6;
    chunk("IHDR", ihdr);
    std::vector<uint8_t> raw;
    raw.reserve(size_t(img.width * 4 + 1) * size_t(img.height));
    for (int y = 0; y < img.height; ++y) {
        raw.push_back(0);
        const uint8_t* row = &img.rgba[size_t(y) * size_t(img.width) * 4];
        for (int x = 0; x < img.width; ++x) {
            const uint8_t a = row[x * 4 + 3];
            for (int c = 0; c < 3; ++c) raw.push_back(a ? uint8_t(std::min(255, (row[x * 4 + c] * 255 + a / 2) / a)) : 0);
            raw.push_back(a);
        }
    }
    uLongf clen = compressBound(uLong(raw.size()));
    std::vector<uint8_t> comp(clen);
    compress2(comp.data(), &clen, raw.data(), uLong(raw.size()), 6);
    comp.resize(clen);
    chunk("IDAT", comp);
    chunk("IEND", {});
    return out;
}

namespace {
std::mutex g_decMu;
std::function<ImagePtr(const uint8_t*, size_t)> g_platformDecoder;
}
void setPlatformImageDecoder(std::function<ImagePtr(const uint8_t*, size_t)> fn) {
    std::lock_guard<std::mutex> lk(g_decMu);
    g_platformDecoder = std::move(fn);
}
ImagePtr decodeImage(const uint8_t* data, size_t n, std::string* error) {
    if (n >= 8 && data[0] == 0x89 && data[1] == 'P') {
        std::string e;
        if (ImagePtr img = decodePng(data, n, &e)) return img;
        if (error) *error = e;
    }
    std::function<ImagePtr(const uint8_t*, size_t)> dec;
    {
        std::lock_guard<std::mutex> lk(g_decMu);
        dec = g_platformDecoder;
    }
    if (dec) if (ImagePtr img = dec(data, n)) return img;
    if (n >= 3 && data[0] == 0xFF && data[1] == 0xD8) {
        int w = 0, h = 0, channels = 0;
        stbi_uc* pixels = stbi_load_from_memory(data, (int)n, &w, &h, &channels, 4);
        if (pixels) {
            auto img = std::make_shared<Image>();
            img->width = w;
            img->height = h;
            img->rgba.assign(pixels, pixels + (size_t(w) * size_t(h) * 4));
            img->opaque = true;
            stbi_image_free(pixels);
            return img;
        }
    }
    if (error && error->empty()) *error = n >= 3 && data[0] == 0xFF && data[1] == 0xD8 ? "corrupt JPEG data" : "unknown image format";
    return nullptr;
}

} // namespace Hydrogen::Graphics
