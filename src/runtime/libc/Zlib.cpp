// HydrogenVM — libz for guest apps, backed by the host's zlib.
//
// The guest's z_stream (32-bit layout, 14 words) is mirrored by a host
// z_stream that does the work: each call copies the buffer pointers/counts in,
// runs zlib on the guest memory directly (the guest arena is contiguous host
// memory) and copies the results back.
#include "runtime/Process.hpp"
#include <memory>
#include <mutex>
#include <unordered_map>
#include <zlib.h>

namespace Hydrogen::Runtime {
namespace {

// struct z_stream (armv7): next_in, avail_in, total_in, next_out, avail_out, total_out,
// msg, state, zalloc, zfree, opaque, data_type, adler, reserved.
enum : uint32_t { kNextIn = 0, kAvailIn = 4, kTotalIn = 8, kNextOut = 12, kAvailOut = 16, kTotalOut = 20,
                  kMsg = 24, kState = 28, kDataType = 44, kAdler = 48 };

struct Streams {
    explicit Streams(Process&) {}
    std::mutex mu;
    std::unordered_map<uint32_t, std::unique_ptr<z_stream>> map;   // keyed by the guest z_stream
    uint32_t msgBuf = 0;
};
Streams& Z(Ctx& x) { return x.proc.service<Streams>(); }

z_stream* host(Ctx& x, uint32_t g) {
    std::lock_guard<std::mutex> lk(Z(x).mu);
    auto it = Z(x).map.find(g);
    return it == Z(x).map.end() ? nullptr : it->second.get();
}
z_stream* create(Ctx& x, uint32_t g) {
    std::lock_guard<std::mutex> lk(Z(x).mu);
    auto& p = Z(x).map[g];
    p = std::make_unique<z_stream>();
    *p = z_stream{};
    x.mem.write<uint32_t>(g + kState, g);   // non-null "internal state"
    return p.get();
}
void destroy(Ctx& x, uint32_t g) { std::lock_guard<std::mutex> lk(Z(x).mu); Z(x).map.erase(g); }

void pull(Ctx& x, uint32_t g, z_stream& s) {
    const uint32_t in = x.mem.read<uint32_t>(g + kNextIn), out = x.mem.read<uint32_t>(g + kNextOut);
    s.next_in = in ? x.mem.host(in) : Z_NULL;
    s.avail_in = x.mem.read<uint32_t>(g + kAvailIn);
    s.next_out = out ? x.mem.host(out) : Z_NULL;
    s.avail_out = x.mem.read<uint32_t>(g + kAvailOut);
}
void push(Ctx& x, uint32_t g, const z_stream& s) {
    const uint32_t in = x.mem.read<uint32_t>(g + kNextIn), out = x.mem.read<uint32_t>(g + kNextOut);
    if (in) x.mem.write<uint32_t>(g + kNextIn, in + uint32_t(s.next_in - x.mem.host(in)));
    if (out) x.mem.write<uint32_t>(g + kNextOut, out + uint32_t(s.next_out - x.mem.host(out)));
    x.mem.write<uint32_t>(g + kAvailIn, s.avail_in);
    x.mem.write<uint32_t>(g + kAvailOut, s.avail_out);
    x.mem.write<uint32_t>(g + kTotalIn, uint32_t(s.total_in));
    x.mem.write<uint32_t>(g + kTotalOut, uint32_t(s.total_out));
    x.mem.write<uint32_t>(g + kDataType, uint32_t(s.data_type));
    x.mem.write<uint32_t>(g + kAdler, uint32_t(s.adler));
    uint32_t msg = 0;
    if (s.msg) {
        auto& z = Z(x);
        if (!z.msgBuf) z.msgBuf = x.proc.heap().allocZeroed(128);
        std::string m(s.msg);
        m.resize(std::min<size_t>(m.size(), 127));
        x.mem.copyIn(z.msgBuf, m.c_str(), m.size() + 1);
        msg = z.msgBuf;
    }
    x.mem.write<uint32_t>(g + kMsg, msg);
}

template <typename F> int32_t run(Ctx& x, uint32_t g, F fn) {
    z_stream* s = host(x, g);
    if (!s) return Z_STREAM_ERROR;
    pull(x, g, *s);
    const int r = fn(s);
    push(x, g, *s);
    return r;
}

// ---- inflate
int32_t z_inflateInit(Ctx& x, GuestPtr g, GuestPtr, int32_t) { z_stream* s = create(x, g); const int r = inflateInit(s); push(x, g, *s); return r; }
int32_t z_inflateInit2(Ctx& x, GuestPtr g, int32_t bits, GuestPtr, int32_t) { z_stream* s = create(x, g); const int r = inflateInit2(s, bits); push(x, g, *s); return r; }
int32_t z_inflate(Ctx& x, GuestPtr g, int32_t flush) { return run(x, g, [&](z_stream* s) { return inflate(s, flush); }); }
int32_t z_inflateEnd(Ctx& x, GuestPtr g) {
    z_stream* s = host(x, g);
    const int r = s ? inflateEnd(s) : Z_STREAM_ERROR;
    destroy(x, g);
    x.mem.write<uint32_t>(g + kState, 0);
    return r;
}
int32_t z_inflateReset(Ctx& x, GuestPtr g) { return run(x, g, [](z_stream* s) { return inflateReset(s); }); }
int32_t z_inflateReset2(Ctx& x, GuestPtr g, int32_t bits) { return run(x, g, [&](z_stream* s) { return inflateReset2(s, bits); }); }
int32_t z_inflateSetDictionary(Ctx& x, GuestPtr g, GuestPtr dict, uint32_t n) {
    return run(x, g, [&](z_stream* s) { return inflateSetDictionary(s, x.mem.host(dict), n); });
}

// ---- deflate
int32_t z_deflateInit(Ctx& x, GuestPtr g, int32_t level, GuestPtr, int32_t) { z_stream* s = create(x, g); const int r = deflateInit(s, level); push(x, g, *s); return r; }
// deflateInit2_(strm, level, method, windowBits, memLevel, strategy, version, size): the last 4 are on the stack.
void z_deflateInit2(Ctx& x) {
    const uint32_t g = x.cpu.r[0];
    const int level = int32_t(x.cpu.r[1]), method = int32_t(x.cpu.r[2]), bits = int32_t(x.cpu.r[3]);
    const uint32_t sp = x.cpu.r[CPU::kSP];
    const int memLevel = int32_t(x.mem.read<uint32_t>(sp)), strategy = int32_t(x.mem.read<uint32_t>(sp + 4));
    z_stream* s = create(x, g);
    const int r = deflateInit2(s, level, method, bits, memLevel, strategy);
    push(x, g, *s);
    x.cpu.r[0] = uint32_t(r);
}
int32_t z_deflate(Ctx& x, GuestPtr g, int32_t flush) { return run(x, g, [&](z_stream* s) { return deflate(s, flush); }); }
int32_t z_deflateEnd(Ctx& x, GuestPtr g) {
    z_stream* s = host(x, g);
    const int r = s ? deflateEnd(s) : Z_STREAM_ERROR;
    destroy(x, g);
    x.mem.write<uint32_t>(g + kState, 0);
    return r == Z_DATA_ERROR ? Z_OK : r;
}
int32_t z_deflateReset(Ctx& x, GuestPtr g) { return run(x, g, [](z_stream* s) { return deflateReset(s); }); }
uint32_t z_deflateBound(Ctx& x, GuestPtr g, uint32_t n) { z_stream* s = host(x, g); return uint32_t(s ? deflateBound(s, n) : compressBound(n)); }

// ---- one-shot
int32_t z_uncompress(Ctx& x, GuestPtr dst, GuestPtr dstLen, GuestPtr src, uint32_t srcLen) {
    uLongf n = x.mem.read<uint32_t>(dstLen);
    const int r = uncompress(x.mem.host(dst), &n, x.mem.host(src), srcLen);
    x.mem.write<uint32_t>(dstLen, uint32_t(n));
    return r;
}
int32_t z_compress2(Ctx& x, GuestPtr dst, GuestPtr dstLen, GuestPtr src, uint32_t srcLen, int32_t level) {
    uLongf n = x.mem.read<uint32_t>(dstLen);
    const int r = compress2(x.mem.host(dst), &n, x.mem.host(src), srcLen, level);
    x.mem.write<uint32_t>(dstLen, uint32_t(n));
    return r;
}
int32_t z_compress(Ctx& x, GuestPtr dst, GuestPtr dstLen, GuestPtr src, uint32_t srcLen) { return z_compress2(x, dst, dstLen, src, srcLen, Z_DEFAULT_COMPRESSION); }
uint32_t z_compressBound(Ctx&, uint32_t n) { return uint32_t(compressBound(n)); }
uint32_t z_crc32(Ctx& x, uint32_t crc, GuestPtr buf, uint32_t n) { return buf ? uint32_t(crc32(crc, x.mem.host(buf), n)) : uint32_t(crc32(0, Z_NULL, 0)); }
uint32_t z_adler32(Ctx& x, uint32_t a, GuestPtr buf, uint32_t n) { return buf ? uint32_t(adler32(a, x.mem.host(buf), n)) : uint32_t(adler32(0, Z_NULL, 0)); }
uint32_t z_version(Ctx& x) {
    static const char vKey = 0; uint32_t& v = x.proc.processSlot(&vKey);
    if (!v) v = x.proc.allocCString(zlibVersion());
    return v;
}

// ---- gz* files (handle = small integer)
struct GzFiles {
    explicit GzFiles(Process&) {}
    std::mutex mu;
    std::unordered_map<uint32_t, gzFile> files;
    uint32_t next = 0x67A00001;
};
gzFile gz(Ctx& x, uint32_t h) {
    auto& g = x.proc.service<GzFiles>();
    std::lock_guard<std::mutex> lk(g.mu);
    auto it = g.files.find(h);
    return it == g.files.end() ? nullptr : it->second;
}
uint32_t z_gzopen(Ctx& x, GuestPtr path, GuestPtr mode) {
    gzFile f = gzopen(x.proc.readCString(path).c_str(), x.proc.readCString(mode).c_str());
    if (!f) return 0;
    auto& g = x.proc.service<GzFiles>();
    std::lock_guard<std::mutex> lk(g.mu);
    const uint32_t h = g.next++;
    g.files[h] = f;
    return h;
}
int32_t z_gzread(Ctx& x, uint32_t h, GuestPtr buf, uint32_t n) { gzFile f = gz(x, h); return f ? gzread(f, x.mem.host(buf), n) : -1; }
int32_t z_gzwrite(Ctx& x, uint32_t h, GuestPtr buf, uint32_t n) { gzFile f = gz(x, h); return f ? gzwrite(f, x.mem.host(buf), n) : 0; }
int32_t z_gzclose(Ctx& x, uint32_t h) {
    gzFile f = gz(x, h);
    if (!f) return Z_STREAM_ERROR;
    { auto& g = x.proc.service<GzFiles>(); std::lock_guard<std::mutex> lk(g.mu); g.files.erase(h); }
    return gzclose(f);
}
int32_t z_gzeof(Ctx& x, uint32_t h) { gzFile f = gz(x, h); return f ? gzeof(f) : 1; }
int32_t z_gzseek(Ctx& x, uint32_t h, int32_t off, int32_t whence) { gzFile f = gz(x, h); return f ? int32_t(gzseek(f, off, whence)) : -1; }
int32_t z_gztell(Ctx& x, uint32_t h) { gzFile f = gz(x, h); return f ? int32_t(gztell(f)) : -1; }
int32_t z_gzrewind(Ctx& x, uint32_t h) { gzFile f = gz(x, h); return f ? gzrewind(f) : -1; }

} // namespace

void registerZlib(Process& p) {
    p.def<&z_inflateInit>("_inflateInit_");
    p.def<&z_inflateInit2>("_inflateInit2_");
    p.def<&z_inflate>("_inflate");
    p.def<&z_inflateEnd>("_inflateEnd");
    p.def<&z_inflateReset>("_inflateReset");
    p.def<&z_inflateReset2>("_inflateReset2");
    p.def<&z_inflateSetDictionary>("_inflateSetDictionary");
    p.def<&z_deflateInit>("_deflateInit_");
    p.defineFunction("_deflateInit2_", [](Ctx& x) { z_deflateInit2(x); });
    p.def<&z_deflate>("_deflate");
    p.def<&z_deflateEnd>("_deflateEnd");
    p.def<&z_deflateReset>("_deflateReset");
    p.def<&z_deflateBound>("_deflateBound");
    p.def<&z_uncompress>("_uncompress");
    p.def<&z_compress>("_compress");
    p.def<&z_compress2>("_compress2");
    p.def<&z_compressBound>("_compressBound");
    p.def<&z_crc32>("_crc32");
    // get_crc_table(): zlib's 256-entry table, copied into guest memory once.
    p.def<+[](Ctx& x) {
        static const char key = 0;
        uint32_t& table = x.proc.processSlot(&key);
        if (!table) {
            table = x.proc.heap().alloc(256 * 4);
            const auto* host = get_crc_table();
            for (int i = 0; i < 256; ++i) x.mem.write<uint32_t>(table + i * 4, uint32_t(host[i]));
        }
        return table;
    }>("_get_crc_table");
    p.def<&z_adler32>("_adler32");
    p.def<&z_version>("_zlibVersion");
    p.def<&z_gzopen>("_gzopen");
    p.def<&z_gzread>("_gzread");
    p.def<&z_gzwrite>("_gzwrite");
    p.def<&z_gzclose>("_gzclose");
    p.def<&z_gzeof>("_gzeof");
    p.def<&z_gzseek>("_gzseek");
    p.def<&z_gztell>("_gztell");
    p.def<&z_gzrewind>("_gzrewind");
}

} // namespace Hydrogen::Runtime
