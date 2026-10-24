// HydrogenVM — GCC 4.2 libstdc++ ostreams, string streams and streambufs.
//
// Unlike the file streams (LibStdcxx.cpp), these objects use libstdc++'s real
// layout, because apps build their own: a custom std::streambuf subclass with
// an inline std::ostream around it calls ios_base::ios_base(), basic_ios::init()
// and then our operator<< — so formatted output has to work on any ostream,
// reaching the streambuf through basic_ios::_M_streambuf and its virtuals.
//
// Layout (armv7):
//   ios_base  (112): +0 vptr, +4 precision, +8 width, +12 flags, +16 exceptions,
//                    +20 state, +24 callbacks, +28 word0, +36 local words[8],
//                    +100 word size, +104 words, +108 locale
//   basic_ios (136): +112 tie, +116 fill, +117 fill_init, +120 streambuf,
//                    +124 ctype, +128 num_put, +132 num_get
//   streambuf  (32): +0 vptr, +4 eback, +8 gptr, +12 egptr, +16 pbase, +20 pptr,
//                    +24 epptr, +28 locale
//   stringbuf  (40): + +32 mode, +36 std::string
//   ostringstream: ostream vptr, stringbuf @4, basic_ios @44  (180 bytes)
//   istringstream: istream vptr, gcount, stringbuf @8, basic_ios @48
//   stringstream:  istream vptr, gcount, ostream vptr @8, stringbuf @12, basic_ios @52
//
// Our stringbuf keeps no get/put area (all six pointers null), so the app's
// inline sputc/sgetc always fall through to the virtuals, which hold the
// contents on the host side.
#include "runtime/Process.hpp"
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace Hydrogen::Runtime {
uint32_t guestStringData(Ctx& x, const std::string& s);
std::string guestStringValue(Ctx& x, GuestPtr self);
void guestStringSet(Ctx& x, GuestPtr self, const std::string& s);
uint32_t guestWStringData(Ctx& x, const std::u32string& s);
std::u32string guestWStringValue(Ctx& x, GuestPtr self);
bool hvmFstreamWrite(Ctx& x, uint32_t os, const char* data, uint32_t n);
bool hvmIsFilebuf(Ctx& x, uint32_t sb);
int32_t hvmFilebufPeek(Ctx& x, uint32_t sb);
int32_t hvmFilebufGet(Ctx& x, uint32_t sb);
int32_t hvmFilebufUnget(Ctx& x, uint32_t sb, int32_t c);
int32_t hvmFilebufRead(Ctx& x, uint32_t sb, uint32_t buf, int32_t n);
int32_t hvmFilebufWrite(Ctx& x, uint32_t sb, const char* data, uint32_t n);
int64_t hvmFilebufSeek(Ctx& x, uint32_t sb, int64_t off, uint32_t dir);

namespace {

constexpr uint32_t kPrecision = 4, kWidth = 8, kFlags = 12, kExceptions = 16, kState = 20, kWordSize = 100, kWords = 104,
                   kLocale = 108, kTie = 112, kFill = 116, kFillInit = 117, kBuf = 120, kCtype = 124, kNumPut = 128,
                   kNumGet = 132, kIosSize = 136;
constexpr uint32_t kBad = 1, kEof = 2, kFail = 4;
// fmtflags
constexpr uint32_t fBoolalpha = 1, fDec = 2, fFixed = 4, fHex = 8, fInternal = 16, fLeft = 32, fOct = 64, fRight = 128,
                   fScientific = 256, fShowbase = 512, fShowpoint = 1024, fShowpos = 2048, fSkipws = 4096, fUnitbuf = 8192,
                   fUppercase = 16384;
constexpr uint32_t mIn = 8, mOut = 16, mAte = 2, mApp = 1, mTrunc = 32;
constexpr int32_t kEofChar = -1;
// streambuf virtual slots
enum Slot : uint32_t { sD1, sD0, sImbue, sSetbuf, sSeekoff, sSeekpos, sSync, sShowmanyc, sXsgetn, sUnderflow, sUflow,
                       sPbackfail, sXsputn, sOverflow, kSlots };

struct Buf { std::string s; std::u32string ws; uint32_t g = 0, p = 0, mode = mIn | mOut; };

struct State {
    explicit State(Process&) {}
    std::mutex mu;
    std::unordered_map<uint32_t, Buf> bufs;    // our stringbufs, by address
    uint32_t sbVptr = 0, baseSbVptr = 0, libcxxSbVptr = 0;       // our stringbuf / std::basic_streambuf vtables (+8)
    uint32_t locImpl = 0, ctype = 0, facet = 0;
    uint32_t vtIs = 0, vtOss = 0, vtIss = 0, vtSs = 0, vtSsOut = 0; // stream primary vptrs
    uint32_t vtIos8 = 0, vtIos44 = 0, vtIos48 = 0, vtIos52 = 0;    // basic_ios vptrs per offset
};
State& St(Ctx& x) { return x.proc.service<State>(); }

uint32_t rd(Ctx& x, uint32_t a) { return x.mem.read<uint32_t>(a); }
void wr(Ctx& x, uint32_t a, uint32_t v) { x.mem.write<uint32_t>(a, v); }

uint32_t iosOf(Ctx& x, uint32_t stream) { return stream + uint32_t(int32_t(rd(x, rd(x, stream) - 12))); }

bool ours(Ctx& x, uint32_t sb) { return sb && rd(x, sb) == St(x).sbVptr; }
template <typename F> auto withBuf(Ctx& x, uint32_t sb, F f) {
    std::lock_guard<std::mutex> lk(St(x).mu);
    return f(St(x).bufs[sb]);
}

// ---- generic streambuf access (any guest streambuf)
uint32_t vfn(Ctx& x, uint32_t obj, uint32_t slot) { return rd(x, rd(x, obj) + slot * 4); }
int32_t vcall(Ctx& x, uint32_t obj, uint32_t slot, std::initializer_list<uint32_t> args) {
    std::vector<uint32_t> a{obj};
    a.insert(a.end(), args);
    return int32_t(uint32_t(x.proc.callGuest(x.cpu, vfn(x, obj, slot), a)));
}

// Writes n bytes to a streambuf; returns how many were taken.
uint32_t sbWrite(Ctx& x, uint32_t sb, const char* data, uint32_t n) {
    if (!n) return 0;
    if (ours(x, sb)) {
        return withBuf(x, sb, [&](Buf& b) {
            if (!(b.mode & mOut)) return 0u;
            if (b.mode & mApp) b.p = uint32_t(b.s.size());
            if (b.p > b.s.size()) b.p = uint32_t(b.s.size());
            b.s.replace(b.p, std::min<size_t>(n, b.s.size() - b.p), data, n);
            b.p += n;
            for (uint32_t i = 0; i < n; ++i) b.ws.push_back(char32_t(uint8_t(data[i])));
            return n;
        });
    }
    if (hvmIsFilebuf(x, sb)) {
        const int32_t r = hvmFilebufWrite(x, sb, data, n);
        return r < 0 ? 0 : uint32_t(r);
    }
    const uint32_t tmp = x.proc.heap().alloc(n);
    if (!tmp) return 0;
    std::memcpy(x.mem.host(tmp), data, n);
    const int32_t r = vcall(x, sb, sXsputn, {tmp, n});
    x.proc.heap().free(tmp);
    return r < 0 ? 0 : uint32_t(r);
}

// Sends formatted bytes to an ostream: honours a failed state, sets badbit on a short write.
void osWrite(Ctx& x, uint32_t os, const std::string& bytes) {
    const uint32_t ios = iosOf(x, os);
    if (rd(x, ios + kState)) return;
    if (hvmFstreamWrite(x, os, bytes.data(), uint32_t(bytes.size()))) return;
    const uint32_t sb = rd(x, ios + kBuf);
    if (!sb || sbWrite(x, sb, bytes.data(), uint32_t(bytes.size())) != bytes.size()) wr(x, ios + kState, rd(x, ios + kState) | kBad);
    if (sb && (rd(x, ios + kFlags) & fUnitbuf) && !ours(x, sb) && vcall(x, sb, sSync, {}) == -1)
        wr(x, ios + kState, rd(x, ios + kState) | kBad);
}

char fillOf(Ctx& x, uint32_t ios) {
    if (!x.mem.read<uint8_t>(ios + kFillInit)) {
        x.mem.write<uint8_t>(ios + kFill, ' ');
        x.mem.write<uint8_t>(ios + kFillInit, 1);
    }
    return char(x.mem.read<uint8_t>(ios + kFill));
}

// Pads to the stream width (then resets it). `prefix` is how much of s precedes internal padding.
std::string pad(Ctx& x, uint32_t ios, std::string s, size_t prefix = 0) {
    const int32_t w = int32_t(rd(x, ios + kWidth));
    wr(x, ios + kWidth, 0);
    if (w <= 0 || size_t(w) <= s.size()) return s;
    const std::string f(size_t(w) - s.size(), fillOf(x, ios));
    const uint32_t adj = rd(x, ios + kFlags) & (fLeft | fRight | fInternal);
    if (adj == fLeft) return s + f;
    if (adj == fInternal) return s.substr(0, prefix) + f + s.substr(prefix);
    return f + s;
}

uint32_t emit(Ctx& x, uint32_t os, std::string s, size_t prefix = 0) {
    const uint32_t ios = iosOf(x, os);
    if (rd(x, ios + kState)) { wr(x, ios + kWidth, 0); return os; }
    osWrite(x, os, pad(x, ios, std::move(s), prefix));
    return os;
}

// Integer formatting per the stream's flags. `bits` is the type's width (hex/oct print it unsigned).
uint32_t putInt(Ctx& x, uint32_t os, int64_t v, bool isSigned, int bits) {
    const uint32_t fl = rd(x, iosOf(x, os) + kFlags);
    const uint32_t base = fl & (fDec | fOct | fHex);
    char buf[80];
    std::string pre, digits;
    if (base == fHex || base == fOct) {
        const uint64_t u = bits == 64 ? uint64_t(v) : uint64_t(uint32_t(v));
        std::snprintf(buf, sizeof buf, base == fHex ? ((fl & fUppercase) ? "%" PRIX64 : "%" PRIx64) : "%" PRIo64, u);
        digits = buf;
        if ((fl & fShowbase) && u) pre = base == fOct ? "0" : (fl & fUppercase) ? "0X" : "0x";
        if (base == fOct && pre == "0") { digits = "0" + digits; pre.clear(); }
    } else if (isSigned) {
        std::snprintf(buf, sizeof buf, "%" PRId64, v);
        digits = buf;
        if (v < 0) { pre = "-"; digits.erase(0, 1); }
        else if (fl & fShowpos) pre = "+";
    } else {
        std::snprintf(buf, sizeof buf, "%" PRIu64, uint64_t(v));
        digits = buf;
    }
    return emit(x, os, pre + digits, pre.size());
}

uint32_t putDouble(Ctx& x, uint32_t os, double d) {
    const uint32_t ios = iosOf(x, os);
    const uint32_t fl = rd(x, ios + kFlags);
    int prec = int32_t(rd(x, ios + kPrecision));
    std::string f = "%";
    if (fl & fShowpos) f += '+';
    if (fl & fShowpoint) f += '#';
    const uint32_t ff = fl & (fFixed | fScientific);
    const bool up = fl & fUppercase;
    if (ff == (fFixed | fScientific)) f += up ? "A" : "a";
    else {
        if (prec < 0) prec = 6;
        f += ".*";
        f += ff == fFixed ? (up ? "F" : "f") : ff == fScientific ? (up ? "E" : "e") : (up ? "G" : "g");
    }
    char buf[512];
    if (ff == (fFixed | fScientific)) std::snprintf(buf, sizeof buf, f.c_str(), d);
    else std::snprintf(buf, sizeof buf, f.c_str(), prec, d);
    const size_t prefix = (buf[0] == '-' || buf[0] == '+') ? 1 : 0;
    return emit(x, os, buf, prefix);
}

// A 64-bit argument after `this`: r1:r2 (iOS armv7 passes arguments APCS-style, no register pairing).
int64_t arg64(Ctx& x) { return int64_t((uint64_t(x.cpu.r[2]) << 32) | x.cpu.r[1]); }
double argDouble(Ctx& x) { const uint64_t b = uint64_t(arg64(x)); double d; std::memcpy(&d, &b, 8); return d; }
// After an sret pointer and `this`: r2:r3.
int64_t arg64After2(Ctx& x) { return int64_t((uint64_t(x.cpu.r[3]) << 32) | x.cpu.r[2]); }

// ---- ios_base / basic_ios
void initIosBase(Ctx& x, uint32_t ios) {
    const uint32_t vptr = rd(x, ios);
    for (uint32_t i = 0; i < 112; i += 4) wr(x, ios + i, 0);
    wr(x, ios, vptr);
    wr(x, ios + kWordSize, 8);
    wr(x, ios + kWords, ios + 36);
    wr(x, ios + kLocale, St(x).locImpl);
}
void iosInit(Ctx& x, uint32_t ios, uint32_t sb) {
    wr(x, ios + kPrecision, 6);
    wr(x, ios + kWidth, 0);
    wr(x, ios + kFlags, fSkipws | fDec);
    wr(x, ios + kExceptions, 0);
    wr(x, ios + kState, sb ? 0 : kBad);
    wr(x, ios + kTie, 0);
    x.mem.write<uint8_t>(ios + kFill, 0);
    x.mem.write<uint8_t>(ios + kFillInit, 0);
    wr(x, ios + kBuf, sb);
    wr(x, ios + kCtype, St(x).ctype);
    wr(x, ios + kNumPut, St(x).facet);
    wr(x, ios + kNumGet, St(x).facet);
}
void b_iosCtor(Ctx& x, GuestPtr ios) { initIosBase(x, ios); }
void b_init(Ctx& x, GuestPtr ios, GuestPtr sb) { iosInit(x, ios, sb); }
void b_clear(Ctx& x, GuestPtr ios, uint32_t st) {
    const uint32_t s = st | (rd(x, ios + kBuf) ? 0 : kBad);
    wr(x, ios + kState, s);
    if (s & rd(x, ios + kExceptions)) x.proc.log(LogLevel::Warning, "basic_ios::clear: the app asked for an ios_base::failure exception (ignored)");
}
void b_setstate(Ctx& x, GuestPtr ios, uint32_t st) { b_clear(x, ios, rd(x, ios + kState) | st); }
uint32_t b_copyfmt(Ctx& x, GuestPtr ios, GuestPtr other) {
    for (uint32_t off : {kPrecision, kWidth, kFlags, kExceptions}) wr(x, ios + off, rd(x, other + off));
    x.mem.write<uint8_t>(ios + kFill, x.mem.read<uint8_t>(other + kFill));
    x.mem.write<uint8_t>(ios + kFillInit, x.mem.read<uint8_t>(other + kFillInit));
    return ios;
}
uint32_t b_rdbufSet(Ctx& x, GuestPtr ios, GuestPtr sb) {
    const uint32_t old = rd(x, ios + kBuf);
    wr(x, ios + kBuf, sb);
    wr(x, ios + kState, sb ? 0 : kBad);
    return old;
}
uint32_t b_fill(Ctx& x, GuestPtr ios) { return uint8_t(fillOf(x, ios)); }
uint32_t b_widen(Ctx&, GuestPtr, uint32_t c) { return c & 0xFF; }
uint32_t useFacetCtype(Ctx& x, GuestPtr) { return St(x).ctype; }
uint32_t is_ctorSb(Ctx& x, GuestPtr self, GuestPtr sb) {
    State& s = St(x);
    wr(x, self, s.vtIs);
    wr(x, self + 4, 0);
    const uint32_t ios = self + 8;
    wr(x, ios, s.vtIos8);
    initIosBase(x, ios);
    iosInit(x, ios, sb);
    return self;
}
uint32_t is_ctorSb2(Ctx& x, GuestPtr self, GuestPtr vtt, GuestPtr sb) {
    wr(x, self, vtt ? rd(x, vtt) : St(x).vtIs);
    wr(x, self + 4, 0);
    const uint32_t ios = iosOf(x, self);
    iosInit(x, ios, sb);
    return self;
}
void locCtor(Ctx& x, GuestPtr loc) { wr(x, loc, St(x).locImpl); }
void locCopy(Ctx& x, GuestPtr loc, GuestPtr) { wr(x, loc, St(x).locImpl); }
uint32_t locAssign(Ctx& x, GuestPtr loc, GuestPtr) { wr(x, loc, St(x).locImpl); return loc; }
void nop1(Ctx&, GuestPtr) {}
void nop2(Ctx&, GuestPtr, GuestPtr) {}

// ---- ostream members
uint32_t o_insert(Ctx& x, GuestPtr os, GuestPtr s, int32_t n) {
    std::string v(size_t(std::max(n, 0)), '\0');
    if (n > 0) std::memcpy(v.data(), x.mem.host(s), size_t(n));
    return emit(x, os, std::move(v));
}
uint32_t wo_insert(Ctx& x, GuestPtr os, GuestPtr ws, int32_t n) {
    if (n <= 0 || !ws) return os;
    const uint32_t ios = iosOf(x, os);
    const uint32_t sb = rd(x, ios + kBuf);
    if (ours(x, sb)) {
        withBuf(x, sb, [&](Buf& b) {
            for (int32_t i = 0; i < n; ++i) {
                const uint32_t wc = x.mem.read<uint32_t>(ws + uint32_t(i * 4));
                b.ws.push_back(char32_t(wc));
                b.s.push_back(char(wc & 0xFF));
            }
            b.p = uint32_t(b.s.size());
            return 0;
        });
    }
    return os;
}
uint32_t o_cstr(Ctx& x, GuestPtr os, GuestPtr s) {
    if (!s) { const uint32_t ios = iosOf(x, os); wr(x, ios + kState, rd(x, ios + kState) | kBad); return os; }
    return emit(x, os, x.proc.readCString(s));
}
uint32_t o_char(Ctx& x, GuestPtr os, uint32_t c) { return emit(x, os, std::string(1, char(c))); }
uint32_t o_int(Ctx& x, GuestPtr os, int32_t v) { return putInt(x, os, v, true, 32); }
uint32_t o_short(Ctx& x, GuestPtr os, int32_t v) {
    const uint32_t base = rd(x, iosOf(x, os) + kFlags) & (fDec | fOct | fHex);
    if (base == fHex || base == fOct) return putInt(x, os, uint16_t(v), false, 32);
    return putInt(x, os, int16_t(v), true, 32);
}
uint32_t o_long(Ctx& x, GuestPtr os, int32_t v) { return putInt(x, os, v, true, 32); }
uint32_t o_ulong(Ctx& x, GuestPtr os, uint32_t v) { return putInt(x, os, int64_t(v), false, 32); }
uint32_t o_bool(Ctx& x, GuestPtr os, uint32_t v) {
    if (rd(x, iosOf(x, os) + kFlags) & fBoolalpha) return emit(x, os, (v & 0xFF) ? "true" : "false");
    return putInt(x, os, (v & 0xFF) ? 1 : 0, true, 32);
}
uint32_t o_ptr(Ctx& x, GuestPtr os, uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, v ? "0x%x" : "0", v);
    return emit(x, os, buf, v ? 2 : 0);
}
uint32_t o_put(Ctx& x, GuestPtr os, uint32_t c) {
    const uint32_t ios = iosOf(x, os);
    if (!rd(x, ios + kState)) osWrite(x, os, std::string(1, char(c)));
    return os;
}
uint32_t o_flush(Ctx& x, GuestPtr os) {
    const uint32_t ios = iosOf(x, os);
    const uint32_t sb = rd(x, ios + kBuf);
    if (sb && !ours(x, sb) && vcall(x, sb, sSync, {}) == -1) wr(x, ios + kState, rd(x, ios + kState) | kBad);
    return os;
}
uint32_t o_endl(Ctx& x, GuestPtr os) { o_put(x, os, '\n'); return o_flush(x, os); }
uint32_t o_ends(Ctx& x, GuestPtr os) { return o_put(x, os, 0); }
uint32_t o_string(Ctx& x, GuestPtr os, GuestPtr str) { return emit(x, os, guestStringValue(x, str)); }

// ---- our stringbuf
void initStringbuf(Ctx& x, uint32_t sb, uint32_t mode, const std::string& init) {
    for (uint32_t i = 0; i < 40; i += 4) wr(x, sb + i, 0);
    wr(x, sb, St(x).sbVptr);
    wr(x, sb + 28, St(x).locImpl);
    wr(x, sb + 32, mode);
    wr(x, sb + 36, guestStringData(x, std::string()));
    std::lock_guard<std::mutex> lk(St(x).mu);
    Buf& b = St(x).bufs[sb];
    b = Buf{};
    b.s = init;
    for (unsigned char ch : init) b.ws.push_back(char32_t(ch));
    b.mode = mode;
    b.p = (mode & (mAte | mApp)) ? uint32_t(init.size()) : 0;
}
void dropStringbuf(Ctx& x, uint32_t sb) { std::lock_guard<std::mutex> lk(St(x).mu); St(x).bufs.erase(sb); }

void sb_str(Ctx& x) {   // string stringbuf::str() const — sret in r0, this in r1
    const uint32_t ret = x.cpu.r[0], sb = x.cpu.r[1];
    const std::string v = ours(x, sb) ? withBuf(x, sb, [](Buf& b) { return b.s; }) : std::string();
    wr(x, ret, guestStringData(x, v));
    x.cpu.r[0] = ret;
}
void wsb_str(Ctx& x) {  // wstring wstringbuf::str() const — sret in r0, this in r1
    const uint32_t ret = x.cpu.r[0], sb = x.cpu.r[1];
    const std::u32string v = ours(x, sb) ? withBuf(x, sb, [](Buf& b) { return b.ws; }) : std::u32string();
    wr(x, ret, guestWStringData(x, v));
    x.cpu.r[0] = ret;
}
void sb_setStr(Ctx& x, GuestPtr sb, GuestPtr str) {
    const std::string v = guestStringValue(x, str);
    withBuf(x, sb, [&](Buf& b) { b.s = v; b.g = 0; b.p = (b.mode & (mAte | mApp)) ? uint32_t(v.size()) : 0; return 0; });
}
void sb_stringbufInit(Ctx& x, GuestPtr sb, uint32_t mode) {
    initStringbuf(x, sb, mode, "");
}
void sb_msync(Ctx& x, GuestPtr sb, GuestPtr pbase, GuestPtr pptr, GuestPtr epptr) {
    if (!ours(x, sb)) return;
    if (pptr > pbase) {
        const uint32_t n = pptr - pbase;
        std::string add(n, '\0');
        std::memcpy(add.data(), x.mem.host(pbase), n);
        withBuf(x, sb, [&](Buf& b) {
            b.s.append(add);
            for (unsigned char ch : add) b.ws.push_back(char32_t(ch));
            b.p = uint32_t(b.s.size());
            return 0;
        });
    }
}
void sb_pbump(Ctx& x, GuestPtr sb, GuestPtr pb, GuestPtr epptr, int32_t o) {
    if (!ours(x, sb)) return;
    wr(x, sb + 20, pb + uint32_t(o)); // pptr = pb + o
}
void oss_str(Ctx& x) {
    const uint32_t ret = x.cpu.r[0], oss = x.cpu.r[1];
    const uint32_t sb = oss + 4;
    const std::string v = ours(x, sb) ? withBuf(x, sb, [](Buf& b) { return b.s; }) : std::string();
    wr(x, ret, guestStringData(x, v));
    x.cpu.r[0] = ret;
}
void iss_str(Ctx& x) {
    const uint32_t ret = x.cpu.r[0], iss = x.cpu.r[1];
    const uint32_t sb = iss + 8;
    const std::string v = ours(x, sb) ? withBuf(x, sb, [](Buf& b) { return b.s; }) : std::string();
    wr(x, ret, guestStringData(x, v));
    x.cpu.r[0] = ret;
}
void ss_str(Ctx& x) {
    const uint32_t ret = x.cpu.r[0], ss = x.cpu.r[1];
    const uint32_t sb = ss + 12;
    const std::string v = ours(x, sb) ? withBuf(x, sb, [](Buf& b) { return b.s; }) : std::string();
    wr(x, ret, guestStringData(x, v));
    x.cpu.r[0] = ret;
}
void oss_setStr(Ctx& x, GuestPtr oss, GuestPtr str) { sb_setStr(x, oss + 4, str); }
void iss_setStr(Ctx& x, GuestPtr iss, GuestPtr str) { sb_setStr(x, iss + 8, str); }
void ss_setStr(Ctx& x, GuestPtr ss, GuestPtr str) { sb_setStr(x, ss + 12, str); }
int32_t sb_overflow(Ctx& x, GuestPtr sb, int32_t c) {
    if (c == kEofChar) return 0;
    const char ch = char(c);
    return sbWrite(x, sb, &ch, 1) ? (c & 0xFF) : kEofChar;
}
int32_t sb_xsputn(Ctx& x, GuestPtr sb, GuestPtr s, int32_t n) {
    if (n <= 0) return 0;
    return int32_t(sbWrite(x, sb, reinterpret_cast<const char*>(x.mem.host(s)), uint32_t(n)));
}
int32_t sb_underflow(Ctx& x, GuestPtr sb) {
    return withBuf(x, sb, [](Buf& b) { return (b.mode & mIn) && b.g < b.s.size() ? int32_t(uint8_t(b.s[b.g])) : kEofChar; });
}
int32_t sb_uflow(Ctx& x, GuestPtr sb) {
    return withBuf(x, sb, [](Buf& b) { return (b.mode & mIn) && b.g < b.s.size() ? int32_t(uint8_t(b.s[b.g++])) : kEofChar; });
}
int32_t sb_xsgetn(Ctx& x, GuestPtr sb, GuestPtr s, int32_t n) {
    if (n <= 0) return 0;
    const std::string chunk = withBuf(x, sb, [&](Buf& b) {
        if (!(b.mode & mIn) || b.g >= b.s.size()) return std::string();
        std::string c = b.s.substr(b.g, size_t(n));
        b.g += uint32_t(c.size());
        return c;
    });
    if (!chunk.empty()) std::memcpy(x.mem.host(s), chunk.data(), chunk.size());
    return int32_t(chunk.size());
}
int32_t sb_pbackfail(Ctx& x, GuestPtr sb, int32_t c) {
    return withBuf(x, sb, [&](Buf& b) {
        if (!b.g) return kEofChar;
        --b.g;
        if (c != kEofChar) b.s[b.g] = char(c);
        return int32_t(uint8_t(b.s[b.g]));
    });
}
int32_t sb_showmanyc(Ctx& x, GuestPtr sb) {
    return withBuf(x, sb, [](Buf& b) { return b.g < b.s.size() ? int32_t(b.s.size() - b.g) : -1; });
}
void writePos(Ctx& x, uint32_t ret, int64_t pos) {
    x.mem.write<uint64_t>(ret, uint64_t(pos));
    for (uint32_t i = 8; i < 136; i += 4) wr(x, ret + i, 0);
}
int64_t seekBuf(Ctx& x, uint32_t sb, int64_t off, uint32_t dir, uint32_t which) {
    return withBuf(x, sb, [&](Buf& b) -> int64_t {
        auto target = [&](uint32_t cur) -> int64_t { return off + (dir == 0 ? 0 : dir == 1 ? int64_t(cur) : int64_t(b.s.size())); };
        int64_t r = -1;
        const bool in = (which & mIn) && (b.mode & mIn), out = (which & mOut) && (b.mode & mOut);
        if (!in && !out) return -1;
        if (in && out && dir == 1) return -1;
        if (in) { const int64_t t = target(b.g); if (t < 0 || t > int64_t(b.s.size())) return -1; b.g = uint32_t(t); r = t; }
        if (out) { const int64_t t = target(b.p); if (t < 0 || t > int64_t(b.s.size())) return -1; b.p = uint32_t(t); r = t; }
        return r;
    });
}
// pos seekoff(off_type off, seekdir dir, openmode which): sret r0, this r1, off r2:r3, dir/which on the stack.
void sb_seekoff(Ctx& x) {
    const uint32_t ret = x.cpu.r[0], sb = x.cpu.r[1], sp = x.cpu.r[CPU::kSP];
    writePos(x, ret, ours(x, sb) ? seekBuf(x, sb, arg64After2(x), rd(x, sp), rd(x, sp + 4)) : -1);
}
// pos seekpos(pos_type pos, openmode which): pos (136 bytes) from r2, `which` after it on the stack.
void sb_seekpos(Ctx& x) {
    const uint32_t ret = x.cpu.r[0], sb = x.cpu.r[1], sp = x.cpu.r[CPU::kSP];
    writePos(x, ret, ours(x, sb) ? seekBuf(x, sb, arg64After2(x), 0, rd(x, sp + 128)) : -1);
}
void sb_dtor(Ctx& x, GuestPtr sb) { dropStringbuf(x, sb); }
void sb_dtorDelete(Ctx& x, GuestPtr sb) { dropStringbuf(x, sb); x.proc.heap().free(sb); }

// ---- std::basic_streambuf defaults (for apps' own subclasses)
int32_t bs_zero(Ctx&, GuestPtr) { return 0; }
int32_t bs_eof(Ctx&, GuestPtr) { return kEofChar; }
int32_t bs_eof2(Ctx&, GuestPtr, int32_t) { return kEofChar; }
uint32_t bs_setbuf(Ctx&, GuestPtr sb, GuestPtr, int32_t) { return sb; }
void bs_imbue(Ctx&, GuestPtr, GuestPtr) {}
void bs_badpos(Ctx& x) { writePos(x, x.cpu.r[0], -1); }
int32_t bs_uflow(Ctx& x, GuestPtr sb) {
    if (vcall(x, sb, sUnderflow, {}) == kEofChar) return kEofChar;
    const uint32_t g = rd(x, sb + 8);
    if (!g || g >= rd(x, sb + 12)) return kEofChar;
    wr(x, sb + 8, g + 1);
    return x.mem.read<uint8_t>(g);
}
int32_t bs_xsgetn(Ctx& x, GuestPtr sb, GuestPtr s, int32_t n) {
    int32_t done = 0;
    while (done < n) {
        const uint32_t g = rd(x, sb + 8), e = rd(x, sb + 12);
        if (g < e) {
            const uint32_t k = std::min<uint32_t>(e - g, uint32_t(n - done));
            std::memmove(x.mem.host(s + uint32_t(done)), x.mem.host(g), k);
            wr(x, sb + 8, g + k);
            done += int32_t(k);
            continue;
        }
        const int32_t c = vcall(x, sb, sUflow, {});
        if (c == kEofChar) break;
        x.mem.write<uint8_t>(s + uint32_t(done++), uint8_t(c));
    }
    return done;
}
int32_t bs_xsputn(Ctx& x, GuestPtr sb, GuestPtr s, int32_t n) {
    int32_t done = 0;
    while (done < n) {
        const uint32_t p = rd(x, sb + 20), e = rd(x, sb + 24);
        if (p < e) {
            const uint32_t k = std::min<uint32_t>(e - p, uint32_t(n - done));
            std::memmove(x.mem.host(p), x.mem.host(s + uint32_t(done)), k);
            wr(x, sb + 20, p + k);
            done += int32_t(k);
            continue;
        }
        if (vcall(x, sb, sOverflow, {x.mem.read<uint8_t>(s + uint32_t(done))}) == kEofChar) break;
        ++done;
    }
    return done;
}

// ---- string streams
enum class SS { Out, In, InOut };
struct SSLayout { uint32_t sb, ios; };
SSLayout layoutOf(SS k) { return k == SS::Out ? SSLayout{4, 44} : k == SS::In ? SSLayout{8, 48} : SSLayout{12, 52}; }
uint32_t forcedMode(SS k) { return k == SS::Out ? mOut : k == SS::In ? mIn : 0; }

void ssConstruct(Ctx& x, uint32_t obj, SS k, uint32_t mode, const std::string& init, uint32_t vtt) {
    State& s = St(x);
    const SSLayout L = layoutOf(k);
    uint32_t ios = obj + L.ios;
    if (vtt) {
        // Base-object constructor for an app class derived from us: its vtables (in the VTT) say where basic_ios is.
        wr(x, obj, rd(x, vtt));
        ios = iosOf(x, obj);
    } else {
        wr(x, obj, k == SS::Out ? s.vtOss : k == SS::In ? s.vtIss : s.vtSs);
        if (k == SS::InOut) wr(x, obj + 8, s.vtSsOut);
        wr(x, ios, k == SS::Out ? s.vtIos44 : k == SS::In ? s.vtIos48 : s.vtIos52);
        initIosBase(x, ios);
    }
    if (k != SS::Out) wr(x, obj + 4, 0);   // gcount
    initStringbuf(x, obj + L.sb, mode | forcedMode(k), init);
    iosInit(x, ios, obj + L.sb);
}
template <SS K> void ss_ctor(Ctx& x, GuestPtr self, uint32_t mode) { ssConstruct(x, self, K, mode, {}, 0); }
template <SS K> void ss_ctor2(Ctx& x, GuestPtr self, GuestPtr vtt, uint32_t mode) { ssConstruct(x, self, K, mode, {}, vtt); }
template <SS K> void ss_ctorStr(Ctx& x, GuestPtr self, GuestPtr str, uint32_t mode) { ssConstruct(x, self, K, mode, guestStringValue(x, str), 0); }
template <SS K> void ss_ctorStr2(Ctx& x, GuestPtr self, GuestPtr vtt, GuestPtr str, uint32_t mode) {
    ssConstruct(x, self, K, mode, guestStringValue(x, str), vtt);
}
template <SS K> void ss_dtor(Ctx& x, GuestPtr self) { dropStringbuf(x, self + layoutOf(K).sb); }
template <SS K> void ss_dtorDelete(Ctx& x, GuestPtr self) { ss_dtor<K>(x, self); x.proc.heap().free(self); }
// Destructors reached through a vptr: adjust to the complete object first.
uint32_t topOf(Ctx& x, uint32_t sub) { return sub + uint32_t(int32_t(rd(x, rd(x, sub) - 8))); }
void ssAny_dtor(Ctx& x, GuestPtr self) {
    const uint32_t obj = topOf(x, self);
    for (uint32_t off : {4u, 8u, 12u}) if (ours(x, obj + off)) { dropStringbuf(x, obj + off); return; }
}
void ssAny_dtorDelete(Ctx& x, GuestPtr self) { const uint32_t obj = topOf(x, self); ssAny_dtor(x, self); x.proc.heap().free(obj); }
// "virtual thunk to" a destructor: this += *(vptr - 12).
void thunkD1(Ctx& x, GuestPtr self) { ssAny_dtor(x, self + uint32_t(int32_t(rd(x, rd(x, self) - 12)))); }
void thunkD0(Ctx& x, GuestPtr self) { ssAny_dtorDelete(x, self + uint32_t(int32_t(rd(x, rd(x, self) - 12)))); }
// ostream's / istream's own destructors: nothing to free (the streambuf belongs to someone else).
void os_dtorDelete(Ctx& x, GuestPtr self) { x.proc.heap().free(topOf(x, self)); }

// ---- libc++ (std::__1::) stream & string helpers
[[noreturn]] void libcxx_throw_length_error(Ctx&, GuestPtr) { throw std::runtime_error("std::__1::length_error"); }
[[noreturn]] void libcxx_throw_out_of_range(Ctx&, GuestPtr) { throw std::runtime_error("std::__1::out_of_range"); }
GuestPtr libcxx_str_dtor(Ctx& x, GuestPtr self) {
    if (self && (x.mem.read<uint32_t>(self) & 1u) != 0) {
        const uint32_t ptr = x.mem.read<uint32_t>(self + 8);
        if (ptr && x.proc.heap().blockSize(ptr)) x.proc.heap().free(ptr);
    }
    return self;
}
GuestPtr libcxx_sb_ctor(Ctx& x, GuestPtr sb) {
    for (uint32_t i = 0; i < 32; i += 4) wr(x, sb + i, 0);
    wr(x, sb, St(x).libcxxSbVptr);
    wr(x, sb + 4, St(x).locImpl);
    return sb;
}
GuestPtr libcxx_sb_dtor(Ctx&, GuestPtr sb) { return sb; }
void libcxx_ios_init(Ctx& x, GuestPtr ios, GuestPtr sb) {
    const uint32_t vptr = rd(x, ios);
    for (uint32_t i = 4; i < 60; i += 4) wr(x, ios + i, 0);
    wr(x, ios, vptr);
    wr(x, ios + 4, fSkipws | fDec);
    wr(x, ios + 8, 6);
    wr(x, ios + 12, 0);
    wr(x, ios + 16, sb ? 0 : kBad);
    wr(x, ios + 24, sb);
    wr(x, ios + 28, St(x).locImpl);
}
int32_t libcxx_uflow(Ctx& x, GuestPtr sb) {
    if (vcall(x, sb, sUnderflow, {}) == kEofChar) return kEofChar;
    const uint32_t g = rd(x, sb + 12);
    if (!g || g >= rd(x, sb + 16)) return kEofChar;
    wr(x, sb + 12, g + 1);
    return x.mem.read<uint8_t>(g);
}
int32_t libcxx_xsgetn(Ctx& x, GuestPtr sb, GuestPtr s, int32_t n) {
    int32_t done = 0;
    while (done < n) {
        const uint32_t g = rd(x, sb + 12), e = rd(x, sb + 16);
        if (g < e) {
            const uint32_t k = std::min<uint32_t>(e - g, uint32_t(n - done));
            std::memmove(x.mem.host(s + uint32_t(done)), x.mem.host(g), k);
            wr(x, sb + 12, g + k);
            done += int32_t(k);
            continue;
        }
        const int32_t c = vcall(x, sb, sUflow, {});
        if (c == kEofChar) break;
        x.mem.write<uint8_t>(s + uint32_t(done++), uint8_t(c));
    }
    return done;
}
int32_t libcxx_xsputn(Ctx& x, GuestPtr sb, GuestPtr s, int32_t n) {
    int32_t done = 0;
    while (done < n) {
        const uint32_t p = rd(x, sb + 24), e = rd(x, sb + 28);
        if (p < e) {
            const uint32_t k = std::min<uint32_t>(e - p, uint32_t(n - done));
            std::memmove(x.mem.host(p), x.mem.host(s + uint32_t(done)), k);
            wr(x, sb + 24, p + k);
            done += int32_t(k);
            continue;
        }
        if (vcall(x, sb, sOverflow, {x.mem.read<uint8_t>(s + uint32_t(done))}) == kEofChar) break;
        ++done;
    }
    return done;
}

// ---- generic istream::read / ostream::write / getline / operator>>
int32_t sbPeekc(Ctx& x, uint32_t sb) {
    if (!sb) return kEofChar;
    if (ours(x, sb)) return sb_underflow(x, sb);
    if (hvmIsFilebuf(x, sb)) return hvmFilebufPeek(x, sb);
    const uint32_t g = rd(x, sb + 8), e = rd(x, sb + 12);
    if (g && g < e) return x.mem.read<uint8_t>(g);
    return vcall(x, sb, sUnderflow, {});
}
int32_t sbGetc(Ctx& x, uint32_t sb) {
    if (!sb) return kEofChar;
    if (ours(x, sb)) return sb_uflow(x, sb);
    if (hvmIsFilebuf(x, sb)) return hvmFilebufGet(x, sb);
    const uint32_t g = rd(x, sb + 8), e = rd(x, sb + 12);
    if (g && g < e) { wr(x, sb + 8, g + 1); return x.mem.read<uint8_t>(g); }
    return vcall(x, sb, sUflow, {});
}
int32_t sbUngetc(Ctx& x, uint32_t sb, int32_t c) {
    if (!sb) return kEofChar;
    if (ours(x, sb)) return sb_pbackfail(x, sb, c);
    if (hvmIsFilebuf(x, sb)) return hvmFilebufUnget(x, sb, c);
    const uint32_t b = rd(x, sb + 4), g = rd(x, sb + 8);
    if (b && g > b) { wr(x, sb + 8, g - 1); return x.mem.read<uint8_t>(g - 1); }
    return vcall(x, sb, sPbackfail, {uint32_t(c)});
}

uint32_t std_getlineDelim(Ctx& x, GuestPtr is, GuestPtr str, uint32_t delimArg) {
    const char delim = char(delimArg & 0xFF);
    guestStringSet(x, str, "");
    const uint32_t ios = iosOf(x, is);
    if (rd(x, ios + kState)) { wr(x, ios + kState, rd(x, ios + kState) | kFail); return is; }
    const uint32_t sb = rd(x, ios + kBuf);
    if (!sb) { wr(x, ios + kState, rd(x, ios + kState) | kFail | kBad); return is; }
    std::string out;
    size_t extracted = 0;
    for (;;) {
        const int32_t c = sbGetc(x, sb);
        if (c == kEofChar) {
            wr(x, ios + kState, rd(x, ios + kState) | kEof | (extracted == 0 ? kFail : 0));
            break;
        }
        ++extracted;
        if (char(c) == delim) break;
        out.push_back(char(c));
    }
    if (!out.empty()) guestStringSet(x, str, out);
    return is;
}
uint32_t std_getline(Ctx& x, GuestPtr is, GuestPtr str) { return std_getlineDelim(x, is, str, '\n'); }

uint32_t is_getlineDelim(Ctx& x, GuestPtr is, GuestPtr buf, int32_t n, uint32_t delimArg) {
    const char delim = char(delimArg & 0xFF);
    const uint32_t ios = iosOf(x, is);
    wr(x, is + 4, 0);
    if (rd(x, ios + kState) || n <= 0) {
        if (n > 0 && buf) x.mem.write<uint8_t>(buf, 0);
        wr(x, ios + kState, rd(x, ios + kState) | kFail);
        return is;
    }
    const uint32_t sb = rd(x, ios + kBuf);
    if (!sb) {
        if (buf) x.mem.write<uint8_t>(buf, 0);
        wr(x, ios + kState, rd(x, ios + kState) | kFail | kBad);
        return is;
    }
    int32_t extracted = 0, written = 0;
    for (;;) {
        const int32_t c = sbGetc(x, sb);
        if (c == kEofChar) {
            wr(x, ios + kState, rd(x, ios + kState) | kEof | (extracted == 0 ? kFail : 0));
            break;
        }
        if (char(c) == delim) {
            ++extracted;
            break;
        }
        if (written + 1 >= n) {
            sbUngetc(x, sb, c);
            wr(x, ios + kState, rd(x, ios + kState) | kFail);
            break;
        }
        if (buf) x.mem.write<uint8_t>(buf + uint32_t(written), uint8_t(c));
        ++written;
        ++extracted;
    }
    if (buf) x.mem.write<uint8_t>(buf + uint32_t(written), 0);
    wr(x, is + 4, uint32_t(extracted));
    return is;
}
uint32_t is_getline(Ctx& x, GuestPtr is, GuestPtr buf, int32_t n) { return is_getlineDelim(x, is, buf, n, '\n'); }

std::string extractToken(Ctx& x, uint32_t is, bool& eofReached) {
    eofReached = false;
    const uint32_t ios = iosOf(x, is);
    if (rd(x, ios + kState)) { wr(x, ios + kState, rd(x, ios + kState) | kFail); return {}; }
    const uint32_t sb = rd(x, ios + kBuf);
    if (!sb) { wr(x, ios + kState, rd(x, ios + kState) | kFail | kBad); return {}; }
    const bool skipws = (rd(x, ios + kFlags) & fSkipws) != 0;
    if (skipws) {
        for (;;) {
            const int32_t c = sbPeekc(x, sb);
            if (c == kEofChar) { eofReached = true; wr(x, ios + kState, rd(x, ios + kState) | kEof | kFail); return {}; }
            if (!std::isspace(uint8_t(c))) break;
            sbGetc(x, sb);
        }
    }
    std::string tok;
    for (;;) {
        const int32_t c = sbPeekc(x, sb);
        if (c == kEofChar) { eofReached = true; break; }
        if (std::isspace(uint8_t(c))) break;
        tok.push_back(char(sbGetc(x, sb)));
    }
    if (tok.empty()) wr(x, ios + kState, rd(x, ios + kState) | (eofReached ? (kEof | kFail) : kFail));
    else if (eofReached) wr(x, ios + kState, rd(x, ios + kState) | kEof);
    return tok;
}

template <typename T>
uint32_t is_extractInt(Ctx& x, GuestPtr is, GuestPtr out) {
    bool eof = false;
    const std::string tok = extractToken(x, is, eof);
    if (tok.empty()) return is;
    const uint32_t ios = iosOf(x, is);
    const uint32_t baseFlag = rd(x, ios + kFlags) & (fDec | fOct | fHex);
    const int base = baseFlag == fHex ? 16 : baseFlag == fOct ? 8 : baseFlag == fDec ? 10 : 0;
    char* end = nullptr;
    if constexpr (std::is_signed_v<T>) {
        const long long v = std::strtoll(tok.c_str(), &end, base);
        if (end == tok.c_str()) { wr(x, ios + kState, rd(x, ios + kState) | kFail); return is; }
        if (out) x.mem.write<T>(out, T(v));
    } else {
        const unsigned long long v = std::strtoull(tok.c_str(), &end, base);
        if (end == tok.c_str()) { wr(x, ios + kState, rd(x, ios + kState) | kFail); return is; }
        if (out) x.mem.write<T>(out, T(v));
    }
    return is;
}
template <typename T>
uint32_t is_extractFloat(Ctx& x, GuestPtr is, GuestPtr out) {
    bool eof = false;
    const std::string tok = extractToken(x, is, eof);
    if (tok.empty()) return is;
    const uint32_t ios = iosOf(x, is);
    char* end = nullptr;
    const double v = std::strtod(tok.c_str(), &end);
    if (end == tok.c_str()) { wr(x, ios + kState, rd(x, ios + kState) | kFail); return is; }
    if (out) x.mem.write<T>(out, T(v));
    return is;
}
uint32_t is_extractBool(Ctx& x, GuestPtr is, GuestPtr out) {
    bool eof = false;
    const std::string tok = extractToken(x, is, eof);
    if (tok.empty()) return is;
    const uint32_t ios = iosOf(x, is);
    bool val = false;
    if (tok == "1" || tok == "true") val = true;
    else if (tok == "0" || tok == "false") val = false;
    else { wr(x, ios + kState, rd(x, ios + kState) | kFail); return is; }
    if (out) x.mem.write<uint8_t>(out, val ? 1 : 0);
    return is;
}
uint32_t is_extractStr(Ctx& x, GuestPtr is, GuestPtr str) {
    bool eof = false;
    const std::string tok = extractToken(x, is, eof);
    if (!tok.empty()) guestStringSet(x, str, tok);
    return is;
}
uint32_t is_extractChar(Ctx& x, GuestPtr is, GuestPtr out) {
    const uint32_t ios = iosOf(x, is);
    if (rd(x, ios + kState)) { wr(x, ios + kState, rd(x, ios + kState) | kFail); return is; }
    const uint32_t sb = rd(x, ios + kBuf);
    const bool skipws = (rd(x, ios + kFlags) & fSkipws) != 0;
    int32_t c = kEofChar;
    for (;;) {
        c = sbGetc(x, sb);
        if (c == kEofChar || !skipws || !std::isspace(uint8_t(c))) break;
    }
    if (c == kEofChar) { wr(x, ios + kState, rd(x, ios + kState) | kEof | kFail); return is; }
    if (out) x.mem.write<uint8_t>(out, uint8_t(c));
    return is;
}
int32_t is_get(Ctx& x, GuestPtr is) {
    const uint32_t ios = iosOf(x, is);
    wr(x, is + 4, 0);
    if (rd(x, ios + kState)) { wr(x, ios + kState, rd(x, ios + kState) | kFail); return kEofChar; }
    const int32_t c = sbGetc(x, rd(x, ios + kBuf));
    if (c == kEofChar) { wr(x, ios + kState, rd(x, ios + kState) | kEof | kFail); return kEofChar; }
    wr(x, is + 4, 1);
    return c;
}
uint32_t is_getChar(Ctx& x, GuestPtr is, GuestPtr out) {
    const int32_t c = is_get(x, is);
    if (c != kEofChar && out) x.mem.write<uint8_t>(out, uint8_t(c));
    return is;
}
int32_t is_peek(Ctx& x, GuestPtr is) {
    const uint32_t ios = iosOf(x, is);
    wr(x, is + 4, 0);
    if (rd(x, ios + kState)) return kEofChar;
    const int32_t c = sbPeekc(x, rd(x, ios + kBuf));
    if (c == kEofChar) wr(x, ios + kState, rd(x, ios + kState) | kEof);
    return c;
}
uint32_t is_ignore(Ctx& x, GuestPtr is, int32_t n, int32_t delim) {
    const uint32_t ios = iosOf(x, is);
    wr(x, is + 4, 0);
    if (rd(x, ios + kState) || n <= 0) return is;
    const uint32_t sb = rd(x, ios + kBuf);
    int32_t count = 0;
    while (count < n) {
        const int32_t c = sbGetc(x, sb);
        if (c == kEofChar) { wr(x, ios + kState, rd(x, ios + kState) | kEof); break; }
        ++count;
        if (c == delim) break;
    }
    wr(x, is + 4, uint32_t(count));
    return is;
}
uint32_t is_ignore1(Ctx& x, GuestPtr is) { return is_ignore(x, is, 1, kEofChar); }
uint32_t is_ignoreN(Ctx& x, GuestPtr is, int32_t n) { return is_ignore(x, is, n, kEofChar); }

} // namespace

uint32_t hvmOstreamWrite(Ctx& x, uint32_t os, uint32_t buf, int32_t n) {
    std::string v(size_t(std::max(n, 0)), '\0');
    if (n > 0) std::memcpy(v.data(), x.mem.host(buf), size_t(n));
    const uint32_t ios = iosOf(x, os);
    if (!rd(x, ios + kState)) osWrite(x, os, v);
    return os;
}
uint32_t hvmIstreamRead(Ctx& x, uint32_t is, uint32_t buf, int32_t n) {
    const uint32_t ios = iosOf(x, is);
    wr(x, is + 4, 0);
    if (rd(x, ios + kState)) { wr(x, ios + kState, rd(x, ios + kState) | kFail); return is; }
    const uint32_t sb = rd(x, ios + kBuf);
    int32_t got = 0;
    if (sb && n > 0) {
        if (ours(x, sb)) got = sb_xsgetn(x, sb, buf, n);
        else if (hvmIsFilebuf(x, sb)) got = hvmFilebufRead(x, sb, buf, n);
        else got = vcall(x, sb, sXsgetn, {buf, uint32_t(n)});
    }
    if (got < 0) got = 0;
    wr(x, is + 4, uint32_t(got));
    if (got < n) wr(x, ios + kState, rd(x, ios + kState) | kEof | kFail);
    return is;
}
void hvmStreamSeek(Ctx& x, uint32_t stream, int64_t off, uint32_t dir, uint32_t which) {
    const uint32_t ios = iosOf(x, stream);
    if (rd(x, ios + kState) & (kFail | kBad)) return;
    if (which & mIn) wr(x, ios + kState, rd(x, ios + kState) & ~kEof);
    const uint32_t sb = rd(x, ios + kBuf);
    int64_t pos = -1;
    if (ours(x, sb)) pos = seekBuf(x, sb, off, dir, which);
    else if (hvmIsFilebuf(x, sb)) pos = hvmFilebufSeek(x, sb, off, dir);
    if (pos < 0) wr(x, ios + kState, rd(x, ios + kState) | kFail);
}
int64_t hvmStreamTell(Ctx& x, uint32_t stream, uint32_t which) {
    const uint32_t ios = iosOf(x, stream);
    if (rd(x, ios + kState) & (kFail | kBad)) return -1;
    const uint32_t sb = rd(x, ios + kBuf);
    if (ours(x, sb)) return seekBuf(x, sb, 0, 1, which);
    if (hvmIsFilebuf(x, sb)) return hvmFilebufSeek(x, sb, 0, 1);
    return -1;
}

void registerLibStdcxxStreams(Process& p) {
    State& s = p.service<State>();
    s.locImpl = p.defineDataBlock("hvm_locale_impl", 64);
    s.facet = p.defineDataBlock("hvm_locale_facet", 64);
    // A ctype<char> whose inline widen()/narrow() find ready tables (identity).
    s.ctype = p.defineDataBlock("hvm_ctype_char", 576);
    p.mem().write<uint8_t>(s.ctype + 28, 1);
    for (uint32_t i = 0; i < 256; ++i) {
        p.mem().write<uint8_t>(s.ctype + 29 + i, uint8_t(i));
        p.mem().write<uint8_t>(s.ctype + 285 + i, uint8_t(i));
    }
    p.mem().write<uint8_t>(s.ctype + 541, 1);

    auto fn = [&](const char* name, Process::HostFn f) { return p.registerHostFunction(name, std::move(f)); };
    // streambuf vtables: [offset to top][typeinfo][14 slots]; vptr = +8.
    auto sbVtable = [&](const std::string& sym, std::initializer_list<uint32_t> slots) {
        const uint32_t a = p.defineDataBlock(sym, 8 + kSlots * 4);
        uint32_t i = 0;
        for (uint32_t f : slots) p.mem().write<uint32_t>(a + 8 + 4 * i++, f);
        return a + 8;
    };
    const uint32_t bD1 = p.def<&nop1>("__ZNSt15basic_streambufIcSt11char_traitsIcEED2Ev");
    p.def<&nop1>("__ZNSt15basic_streambufIcSt11char_traitsIcEED1Ev");
    const uint32_t bD0 = p.def<&nop1>("__ZNSt15basic_streambufIcSt11char_traitsIcEED0Ev");
    const uint32_t bImbue = p.def<&bs_imbue>("__ZNSt15basic_streambufIcSt11char_traitsIcEE5imbueERKSt6locale");
    const uint32_t bSetbuf = p.def<&bs_setbuf>("__ZNSt15basic_streambufIcSt11char_traitsIcEE6setbufEPci");
    const uint32_t bSeekoff = p.defineFunction("__ZNSt15basic_streambufIcSt11char_traitsIcEE7seekoffExSt12_Ios_SeekdirSt13_Ios_Openmode", bs_badpos);
    const uint32_t bSeekpos = p.defineFunction("__ZNSt15basic_streambufIcSt11char_traitsIcEE7seekposESt4fposI11__mbstate_tESt13_Ios_Openmode", bs_badpos);
    const uint32_t bSync = p.def<&bs_zero>("__ZNSt15basic_streambufIcSt11char_traitsIcEE4syncEv");
    const uint32_t bShow = p.def<&bs_zero>("__ZNSt15basic_streambufIcSt11char_traitsIcEE9showmanycEv");
    const uint32_t bXsgetn = p.def<&bs_xsgetn>("__ZNSt15basic_streambufIcSt11char_traitsIcEE6xsgetnEPci");
    const uint32_t bUnder = p.def<&bs_eof>("__ZNSt15basic_streambufIcSt11char_traitsIcEE9underflowEv");
    const uint32_t bUflow = p.def<&bs_uflow>("__ZNSt15basic_streambufIcSt11char_traitsIcEE5uflowEv");
    const uint32_t bPback = p.def<&bs_eof2>("__ZNSt15basic_streambufIcSt11char_traitsIcEE9pbackfailEi");
    const uint32_t bXsputn = p.def<&bs_xsputn>("__ZNSt15basic_streambufIcSt11char_traitsIcEE6xsputnEPKci");
    const uint32_t bOver = p.def<&bs_eof2>("__ZNSt15basic_streambufIcSt11char_traitsIcEE8overflowEi");
    s.baseSbVptr = sbVtable("__ZTVSt15basic_streambufIcSt11char_traitsIcEE",
        {bD1, bD0, bImbue, bSetbuf, bSeekoff, bSeekpos, bSync, bShow, bXsgetn, bUnder, bUflow, bPback, bXsputn, bOver});

    const char* SB = "St15basic_stringbufIcSt11char_traitsIcESaIcEE";
    auto sbn = [&](const char* rest) { return std::string("__ZN") + SB + rest; };
    const uint32_t sD1f = p.def<&sb_dtor>(sbn("D1Ev"));
    p.def<&sb_dtor>(sbn("D2Ev"));
    const uint32_t sD0f = p.def<&sb_dtorDelete>(sbn("D0Ev"));
    const uint32_t sSeekoffF = p.defineFunction(sbn("7seekoffExSt12_Ios_SeekdirSt13_Ios_Openmode"), sb_seekoff);
    const uint32_t sSeekposF = p.defineFunction(sbn("7seekposESt4fposI11__mbstate_tESt13_Ios_Openmode"), sb_seekpos);
    const uint32_t sShowF = p.def<&sb_showmanyc>(sbn("9showmanycEv"));
    const uint32_t sXsgetnF = fn("stringbuf::xsgetn", [](Ctx& x) { x.cpu.r[0] = uint32_t(sb_xsgetn(x, x.cpu.r[0], x.cpu.r[1], int32_t(x.cpu.r[2]))); });
    const uint32_t sUnderF = p.def<&sb_underflow>(sbn("9underflowEv"));
    const uint32_t sUflowF = fn("stringbuf::uflow", [](Ctx& x) { x.cpu.r[0] = uint32_t(sb_uflow(x, x.cpu.r[0])); });
    const uint32_t sPbackF = p.def<&sb_pbackfail>(sbn("9pbackfailEi"));
    const uint32_t sXsputnF = p.def<&sb_xsputn>(sbn("6xsputnEPKci"));
    const uint32_t sOverF = p.def<&sb_overflow>(sbn("8overflowEi"));
    const uint32_t sSetbufF = p.def<&bs_setbuf>(sbn("6setbufEPci"));
    s.sbVptr = sbVtable(std::string("__ZTV") + SB,
        {sD1f, sD0f, bImbue, sSetbufF, sSeekoffF, sSeekposF, bSync, sShowF, sXsgetnF, sUnderF, sUflowF, sPbackF, sXsputnF, sOverF});
    p.defineFunction(std::string("__ZNK") + SB + "3strEv", sb_str);
    p.def<&sb_setStr>(sbn("3strERKSs"));
    p.def<&sb_stringbufInit>(sbn("17_M_stringbuf_initESt13_Ios_Openmode"));
    p.def<&sb_msync>(sbn("7_M_syncEPcS1_S1_"));
    p.def<&sb_pbump>(sbn("8_M_pbumpEPcS1_i"));

    // Stream vtables: [vcall offset][vbase offset][offset to top][typeinfo][D1][D0]; vptr = +16.
    auto streamVtable = [&](const std::string& name, int32_t vbase, int32_t top, uint32_t d1, uint32_t d0) {
        const uint32_t a = p.defineDataBlock(name, 24);
        p.mem().write<uint32_t>(a, uint32_t(-vbase - top));   // vcall offset, for virtual-thunk destructors
        p.mem().write<uint32_t>(a + 4, uint32_t(vbase));
        p.mem().write<uint32_t>(a + 8, uint32_t(top));
        p.mem().write<uint32_t>(a + 16, d1);
        p.mem().write<uint32_t>(a + 20, d0);
        return a + 16;
    };
    const uint32_t anyD1 = fn("stringstream dtor", [](Ctx& x) { ssAny_dtor(x, x.cpu.r[0]); });
    const uint32_t anyD0 = fn("stringstream deleting dtor", [](Ctx& x) { ssAny_dtorDelete(x, x.cpu.r[0]); });
    s.vtIs = streamVtable("hvm_vt_istream", 8, 0, anyD1, anyD0);
    s.vtOss = streamVtable("hvm_vt_ostringstream", 44, 0, anyD1, anyD0);
    s.vtIss = streamVtable("hvm_vt_istringstream", 48, 0, anyD1, anyD0);
    s.vtSs = streamVtable("hvm_vt_stringstream", 52, 0, anyD1, anyD0);
    s.vtSsOut = streamVtable("hvm_vt_stringstream_out", 44, -8, anyD1, anyD0);
    const uint32_t tD1 = p.def<&thunkD1>("hvm_ios_thunk_d1"), tD0 = p.def<&thunkD0>("hvm_ios_thunk_d0");
    // basic_ios subobject vtables: vcall offset -ios so the thunks find the whole object.
    auto iosVt = [&](int32_t off) {
        const uint32_t a = p.defineDataBlock("hvm_vt_ios_" + std::to_string(off), 24);
        p.mem().write<uint32_t>(a + 4, uint32_t(-off));   // vcall offset (vptr - 12)
        p.mem().write<uint32_t>(a + 8, uint32_t(-off));   // offset to top (vptr - 8)
        p.mem().write<uint32_t>(a + 16, tD1);
        p.mem().write<uint32_t>(a + 20, tD0);
        return a + 16;
    };
    s.vtIos8 = iosVt(8);
    s.vtIos44 = iosVt(44);
    s.vtIos48 = iosVt(48);
    s.vtIos52 = iosVt(52);
    {
        const uint32_t biosVt = p.defineDataBlock("__ZTVSt9basic_iosIcSt11char_traitsIcEE", 24);
        p.mem().write<uint32_t>(biosVt + 16, bD1);
        p.mem().write<uint32_t>(biosVt + 20, bD0);
    }
    // Standard global streams: cout, cerr, clog, cin
    {
        CPU::Cpu dummyCpu{};
        Ctx initCtx{p, dummyCpu, p.mem()};
        for (const char* sym : {"__ZSt4cout", "__ZSt4cerr", "__ZSt4clog"}) {
            const uint32_t stAddr = p.defineDataBlock(sym, 180);
            ssConstruct(initCtx, stAddr, SS::Out, mOut, {}, 0);
        }
        const uint32_t cinAddr = p.defineDataBlock("__ZSt3cin", 184);
        ssConstruct(initCtx, cinAddr, SS::In, mIn, {}, 0);
    }
    // VTTs (for app classes derived from a string stream): primary, base primary, base's ios, our ios.
    auto vtt = [&](const std::string& name, std::initializer_list<uint32_t> e) {
        const uint32_t a = p.defineDataBlock(name, uint32_t(e.size()) * 4);
        uint32_t i = 0;
        for (uint32_t v : e) p.mem().write<uint32_t>(a + 4 * i++, v);
    };
    const char* OSS = "St19basic_ostringstreamIcSt11char_traitsIcESaIcEE";
    const char* ISS = "St19basic_istringstreamIcSt11char_traitsIcESaIcEE";
    const char* SS_ = "St18basic_stringstreamIcSt11char_traitsIcESaIcEE";
    vtt(std::string("__ZTT") + OSS, {s.vtOss, s.vtOss, s.vtIos44, s.vtIos44});
    vtt(std::string("__ZTT") + ISS, {s.vtIss, s.vtIss, s.vtIos48, s.vtIos48});
    vtt(std::string("__ZTT") + SS_, {s.vtSs, s.vtSs, s.vtIos52, s.vtSsOut, s.vtIos52, s.vtIos52, s.vtSsOut, s.vtIos52});
    p.defineData(std::string("__ZTV") + OSS, s.vtOss - 16);
    p.defineData(std::string("__ZTV") + ISS, s.vtIss - 16);
    p.defineData(std::string("__ZTV") + SS_, s.vtSs - 16);

    auto reg = [&](const char* cls, auto ctor, auto ctor2, auto ctorStr, auto ctorStr2, auto dtor, auto dtorDel) {
        auto n = [&](const char* r) { return std::string("__ZN") + cls + r; };
        p.def<decltype(ctor)::value>(n("C1ESt13_Ios_Openmode"));
        p.def<decltype(ctor2)::value>(n("C2ESt13_Ios_Openmode"));
        p.def<decltype(ctorStr)::value>(n("C1ERKSsSt13_Ios_Openmode"));
        p.def<decltype(ctorStr2)::value>(n("C2ERKSsSt13_Ios_Openmode"));
        p.def<decltype(dtor)::value>(n("D1Ev"));
        p.def<decltype(dtor)::value>(n("D2Ev"));
        p.def<decltype(dtorDel)::value>(n("D0Ev"));
    };
#define HVM_SS(K) std::integral_constant<decltype(&ss_ctor<K>), &ss_ctor<K>>{}, \
                  std::integral_constant<decltype(&ss_ctor2<K>), &ss_ctor2<K>>{}, \
                  std::integral_constant<decltype(&ss_ctorStr<K>), &ss_ctorStr<K>>{}, \
                  std::integral_constant<decltype(&ss_ctorStr2<K>), &ss_ctorStr2<K>>{}, \
                  std::integral_constant<decltype(&ss_dtor<K>), &ss_dtor<K>>{}, \
                  std::integral_constant<decltype(&ss_dtorDelete<K>), &ss_dtorDelete<K>>{}
    reg(OSS, HVM_SS(SS::Out));
    reg(ISS, HVM_SS(SS::In));
    reg(SS_, HVM_SS(SS::InOut));
#undef HVM_SS
    // Virtual thunks for the string streams' destructors (Tv0_n12_).
    for (const char* cls : {OSS, ISS, SS_}) {
        p.def<&thunkD1>(std::string("__ZTv0_n12_N") + cls + "D1Ev");
        p.def<&thunkD0>(std::string("__ZTv0_n12_N") + cls + "D0Ev");
    }
    // stringstream's ostream-subobject thunk ("non-virtual thunk", -8).
    p.defineFunction(std::string("__ZThn8_N") + SS_ + "D1Ev", [](Ctx& x) { ssAny_dtor(x, x.cpu.r[0] - 8); });
    p.defineFunction(std::string("__ZThn8_N") + SS_ + "D0Ev", [](Ctx& x) { ssAny_dtorDelete(x, x.cpu.r[0] - 8); });

    p.defineFunction(std::string("__ZNK") + OSS + "3strEv", oss_str);
    p.defineFunction(std::string("__ZNK") + ISS + "3strEv", iss_str);
    p.defineFunction(std::string("__ZNK") + SS_ + "3strEv", ss_str);
    p.def<&oss_setStr>(std::string("__ZN") + OSS + "3strERKSs");
    p.def<&iss_setStr>(std::string("__ZN") + ISS + "3strERKSs");
    p.def<&ss_setStr>(std::string("__ZN") + SS_ + "3strERKSs");

    // Wide stringstream (std::wstringstream)
    const char* WSS = "St18basic_stringstreamIwSt11char_traitsIwESaIwEE";
    const char* WOSS = "St19basic_ostringstreamIwSt11char_traitsIwESaIwEE";
    const char* WISS = "St19basic_istringstreamIwSt11char_traitsIwESaIwEE";
    const char* WSB = "St15basic_stringbufIwSt11char_traitsIwESaIwEE";
    p.defineFunction(std::string("__ZNK") + WSB + "3strEv", wsb_str);
    p.def<&ss_ctor<SS::InOut>>(std::string("__ZN") + WSS + "C1ESt13_Ios_Openmode");
    p.def<&ss_ctor2<SS::InOut>>(std::string("__ZN") + WSS + "C2ESt13_Ios_Openmode");
    p.def<&ss_dtor<SS::InOut>>(std::string("__ZN") + WSS + "D1Ev");
    p.def<&ss_dtor<SS::InOut>>(std::string("__ZN") + WSS + "D2Ev");
    p.def<&ss_dtorDelete<SS::InOut>>(std::string("__ZN") + WSS + "D0Ev");
    p.def<&ss_ctor<SS::Out>>(std::string("__ZN") + WOSS + "C1ESt13_Ios_Openmode");
    p.def<&ss_dtor<SS::Out>>(std::string("__ZN") + WOSS + "D1Ev");
    p.def<&ss_ctor<SS::In>>(std::string("__ZN") + WISS + "C1ESt13_Ios_Openmode");
    p.def<&ss_dtor<SS::In>>(std::string("__ZN") + WISS + "D1Ev");

    // ios_base, basic_ios, locale
    p.def<&b_iosCtor>("__ZNSt8ios_baseC2Ev");
    p.def<&b_iosCtor>("__ZNSt8ios_baseC1Ev");
    for (const char* v : {"D0Ev", "D1Ev", "D2Ev"}) p.def<&nop1>(std::string("__ZNSt8ios_base") + v);
    p.def<&b_init>("__ZNSt9basic_iosIcSt11char_traitsIcEE4initEPSt15basic_streambufIcS1_E");
    p.def<&b_clear>("__ZNSt9basic_iosIcSt11char_traitsIcEE5clearESt12_Ios_Iostate");
    p.def<&b_setstate>("__ZNSt9basic_iosIcSt11char_traitsIcEE8setstateESt12_Ios_Iostate");
    p.def<&b_copyfmt>("__ZNSt9basic_iosIcSt11char_traitsIcEE7copyfmtERKS2_");
    p.def<&b_rdbufSet>("__ZNSt9basic_iosIcSt11char_traitsIcEE5rdbufEPSt15basic_streambufIcS1_E");
    p.def<&b_fill>("__ZNKSt9basic_iosIcSt11char_traitsIcEE4fillEv");
    p.def<&b_widen>("__ZNKSt9basic_iosIcSt11char_traitsIcEE5widenEc");
    p.def<&useFacetCtype>("__ZSt9use_facetISt5ctypeIcEERKT_RKSt6locale");
    p.def<&is_ctorSb>("__ZNSiC1EPSt15basic_streambufIcSt11char_traitsIcEE");
    p.def<&is_ctorSb2>("__ZNSiC2EPSt15basic_streambufIcSt11char_traitsIcEE");
    p.def<&std_getlineDelim>("__ZSt7getlineIcSt11char_traitsIcESaIcEERSt13basic_istreamIT_T0_ES7_RSbIS4_S5_T1_ES4_");
    p.def<&std_getline>("__ZSt7getlineIcSt11char_traitsIcESaIcEERSt13basic_istreamIT_T0_ES7_RSbIS4_S5_T1_E");
    p.def<&is_getlineDelim>("__ZNSi7getlineEPcic");
    p.def<&is_getline>("__ZNSi7getlineEPci");
    p.def<&is_extractInt<int32_t>>("__ZNSirsERi");
    p.def<&is_extractInt<int16_t>>("__ZNSirsERs");
    p.def<&is_extractInt<uint16_t>>("__ZNSirsERt");
    p.def<&is_extractInt<uint32_t>>("__ZNSirsERj");
    p.def<&is_extractInt<int32_t>>("__ZNSirsERl");
    p.def<&is_extractInt<uint32_t>>("__ZNSirsERm");
    p.def<&is_extractInt<int64_t>>("__ZNSirsERx");
    p.def<&is_extractInt<uint64_t>>("__ZNSirsERy");
    p.def<&is_extractFloat<float>>("__ZNSirsERf");
    p.def<&is_extractFloat<double>>("__ZNSirsERd");
    p.def<&is_extractBool>("__ZNSirsERb");
    p.def<&is_extractInt<uint16_t>>("__ZNSi10_M_extractItEERSiRT_");
    p.def<&is_extractInt<uint32_t>>("__ZNSi10_M_extractIjEERSiRT_");
    p.def<&is_extractInt<int32_t>>("__ZNSi10_M_extractIlEERSiRT_");
    p.def<&is_extractInt<uint32_t>>("__ZNSi10_M_extractImEERSiRT_");
    p.def<&is_extractInt<int64_t>>("__ZNSi10_M_extractIxEERSiRT_");
    p.def<&is_extractInt<uint64_t>>("__ZNSi10_M_extractIyEERSiRT_");
    p.def<&is_extractFloat<float>>("__ZNSi10_M_extractIfEERSiRT_");
    p.def<&is_extractFloat<double>>("__ZNSi10_M_extractIdEERSiRT_");
    p.def<&is_extractBool>("__ZNSi10_M_extractIbEERSiRT_");
    p.def<&is_extractStr>("__ZStrsIcSt11char_traitsIcESaIcEERSt13basic_istreamIT_T0_ES7_RSbIS4_S5_T1_E");
    p.def<&is_extractChar>("__ZStrsIcSt11char_traitsIcEERSt13basic_istreamIT_T0_ES6_RS3_");
    p.def<&is_get>("__ZNSi3getEv");
    p.def<&is_getChar>("__ZNSi3getERc");
    p.def<&is_peek>("__ZNSi4peekEv");
    p.def<&is_ignore>("__ZNSi6ignoreEii");
    p.def<&is_ignoreN>("__ZNSi6ignoreEi");
    p.def<&is_ignore1>("__ZNSi6ignoreEv");
    for (const char* v : {"D0Ev", "D1Ev", "D2Ev"}) p.def<&nop1>(std::string("__ZNSt9basic_iosIcSt11char_traitsIcEE") + v);
    p.def<&locCtor>("__ZNSt6localeC1Ev");
    p.def<&locCtor>("__ZNSt6localeC2Ev");
    p.def<&locCopy>("__ZNSt6localeC1ERKS_");
    p.def<&locCopy>("__ZNSt6localeC2ERKS_");
    p.def<&locAssign>("__ZNSt6localeaSERKS_");
    p.def<&nop1>("__ZNSt6localeD1Ev");
    p.def<&nop1>("__ZNSt6localeD2Ev");

    // ostream: formatted output
    p.def<&o_insert>("__ZSt16__ostream_insertIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_PKS3_i");
    p.def<&wo_insert>("__ZSt16__ostream_insertIwSt11char_traitsIwEERSt13basic_ostreamIT_T0_ES6_PKS3_i");
    p.def<&o_cstr>("__ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc");
    p.def<&o_char>("__ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_c");
    p.def<&o_string>("__ZStlsIcSt11char_traitsIcESaIcEERSt13basic_ostreamIT_T0_ES7_RKSbIS4_S5_T1_E");
    p.def<&o_int>("__ZNSolsEi");
    p.def<&o_int>("__ZNSt13basic_ostreamIwSt11char_traitsIwEElsEi");
    p.def<&o_short>("__ZNSolsEs");
    p.def<&o_long>("__ZNSo9_M_insertIlEERSoT_");
    p.def<&o_ulong>("__ZNSo9_M_insertImEERSoT_");
    p.def<&o_bool>("__ZNSo9_M_insertIbEERSoT_");
    p.def<&o_ptr>("__ZNSo9_M_insertIPKvEERSoT_");
    p.defineFunction("__ZNSo9_M_insertIxEERSoT_", [](Ctx& x) { x.cpu.r[0] = putInt(x, x.cpu.r[0], arg64(x), true, 64); });
    p.defineFunction("__ZNSo9_M_insertIyEERSoT_", [](Ctx& x) { x.cpu.r[0] = putInt(x, x.cpu.r[0], arg64(x), false, 64); });
    p.defineFunction("__ZNSo9_M_insertIdEERSoT_", [](Ctx& x) { x.cpu.r[0] = putDouble(x, x.cpu.r[0], argDouble(x)); });
    p.defineFunction("__ZNSo9_M_insertIeEERSoT_", [](Ctx& x) { x.cpu.r[0] = putDouble(x, x.cpu.r[0], argDouble(x)); });
    p.def<&o_put>("__ZNSo3putEc");
    p.def<&o_flush>("__ZNSo5flushEv");
    p.def<&o_endl>("__ZSt4endlIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_");
    p.def<&o_ends>("__ZSt4endsIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_");
    p.def<&o_flush>("__ZSt5flushIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_");
    // ostream / istream / iostream destructors (an app's own stream classes chain to these).
    for (const char* cls : {"So", "Si", "Sd"}) {
        p.def<&nop1>(std::string("__ZN") + cls + "D1Ev");
        p.def<&nop2>(std::string("__ZN") + cls + "D2Ev");
        p.def<&os_dtorDelete>(std::string("__ZN") + cls + "D0Ev");
        p.def<&nop1>(std::string("__ZTv0_n12_N") + cls + "D1Ev");
        p.defineFunction(std::string("__ZTv0_n12_N") + cls + "D0Ev", [](Ctx& x) {
            const uint32_t self = x.cpu.r[0];
            os_dtorDelete(x, self + uint32_t(int32_t(rd(x, rd(x, self) - 12))));
        });
    }

    // libc++ (std::__1::) stream & string support
    p.def<&libcxx_throw_length_error>("__ZNKSt3__120__vector_base_commonILb1EE20__throw_length_errorEv");
    p.def<&libcxx_throw_length_error>("__ZNKSt3__121__basic_string_commonILb1EE20__throw_length_errorEv");
    p.def<&libcxx_throw_out_of_range>("__ZNKSt3__121__basic_string_commonILb1EE20__throw_out_of_rangeEv");
    p.def<&libcxx_str_dtor>("__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEED1Ev");
    p.def<&libcxx_str_dtor>("__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEED2Ev");
    p.def<&libcxx_sb_ctor>("__ZNSt3__115basic_streambufIcNS_11char_traitsIcEEEC1Ev");
    p.def<&libcxx_sb_ctor>("__ZNSt3__115basic_streambufIcNS_11char_traitsIcEEEC2Ev");
    const uint32_t lcD1 = p.def<&libcxx_sb_dtor>("__ZNSt3__115basic_streambufIcNS_11char_traitsIcEEED1Ev");
    p.def<&libcxx_sb_dtor>("__ZNSt3__115basic_streambufIcNS_11char_traitsIcEEED2Ev");
    const uint32_t lcD0 = p.def<&os_dtorDelete>("__ZNSt3__115basic_streambufIcNS_11char_traitsIcEEED0Ev");
    const uint32_t lcImbue = p.def<&bs_imbue>("__ZNSt3__115basic_streambufIcNS_11char_traitsIcEEE5imbueERKNS_6localeE");
    const uint32_t lcSetbuf = p.def<&bs_setbuf>("__ZNSt3__115basic_streambufIcNS_11char_traitsIcEEE6setbufEPci");
    const uint32_t lcSeekoff = p.defineFunction("__ZNSt3__115basic_streambufIcNS_11char_traitsIcEEE7seekoffExNS_8ios_base7seekdirEj", bs_badpos);
    const uint32_t lcSeekpos = p.defineFunction("__ZNSt3__115basic_streambufIcNS_11char_traitsIcEEE7seekposENS_4fposI11__mbstate_tEEj", bs_badpos);
    const uint32_t lcSync = p.def<&bs_zero>("__ZNSt3__115basic_streambufIcNS_11char_traitsIcEEE4syncEv");
    const uint32_t lcShow = p.def<&bs_zero>("__ZNSt3__115basic_streambufIcNS_11char_traitsIcEEE9showmanycEv");
    const uint32_t lcXsgetn = p.def<&libcxx_xsgetn>("__ZNSt3__115basic_streambufIcNS_11char_traitsIcEEE6xsgetnEPci");
    const uint32_t lcUnder = p.def<&bs_eof>("__ZNSt3__115basic_streambufIcNS_11char_traitsIcEEE9underflowEv");
    const uint32_t lcUflow = p.def<&libcxx_uflow>("__ZNSt3__115basic_streambufIcNS_11char_traitsIcEEE5uflowEv");
    const uint32_t lcPback = p.def<&bs_eof2>("__ZNSt3__115basic_streambufIcNS_11char_traitsIcEEE9pbackfailEi");
    const uint32_t lcXsputn = p.def<&libcxx_xsputn>("__ZNSt3__115basic_streambufIcNS_11char_traitsIcEEE6xsputnEPKci");
    const uint32_t lcOver = p.def<&bs_eof2>("__ZNSt3__115basic_streambufIcNS_11char_traitsIcEEE8overflowEi");
    s.libcxxSbVptr = sbVtable("__ZTVNSt3__115basic_streambufIcNS_11char_traitsIcEEEE",
        {lcD1, lcD0, lcImbue, lcSetbuf, lcSeekoff, lcSeekpos, lcSync, lcShow, lcXsgetn, lcUnder, lcUflow, lcPback, lcXsputn, lcOver});
    p.def<&libcxx_ios_init>("__ZNSt3__18ios_base4initEPv");
    p.def<&libcxx_sb_dtor>("__ZNSt3__19basic_iosIcNS_11char_traitsIcEEED1Ev");
    p.def<&libcxx_sb_dtor>("__ZNSt3__19basic_iosIcNS_11char_traitsIcEEED2Ev");
    p.def<&os_dtorDelete>("__ZNSt3__19basic_iosIcNS_11char_traitsIcEEED0Ev");
    p.def<&libcxx_sb_dtor>("__ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEED1Ev");
    p.def<&libcxx_sb_dtor>("__ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEED2Ev");
    p.def<&os_dtorDelete>("__ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEED0Ev");
    p.def<&libcxx_sb_dtor>("__ZTv0_n12_NSt3__113basic_ostreamIcNS_11char_traitsIcEEED1Ev");
    p.defineFunction("__ZTv0_n12_NSt3__113basic_ostreamIcNS_11char_traitsIcEEED0Ev", [](Ctx& x) {
        const uint32_t self = x.cpu.r[0];
        os_dtorDelete(x, self + uint32_t(int32_t(rd(x, rd(x, self) - 12))));
    });
    p.defineDataBlock("__ZTINSt3__113basic_ostreamIcNS_11char_traitsIcEEEE", 12);
    p.defineDataBlock("__ZTINSt3__115basic_streambufIcNS_11char_traitsIcEEEE", 12);
}

} // namespace Hydrogen::Runtime
