// HydrogenVM — libc++ (std::__1::) iostreams: the parts that live in libc++.dylib.
//
// An app built against libc++ instantiates basic_stringbuf, basic_stringstream,
// basic_filebuf and getline itself (they are templates), with real get/put areas,
// and imports the rest: ios_base, locale and its facets, the sentries, the
// arithmetic inserters and the stream destructors. Those are here.
// The streambuf base class and ios_base::init are in LibStdcxxStreams.cpp.
//
// Layout (armv7):
//   ios_base  (72): +0 vptr, +4 fmtflags, +8 precision, +12 width, +16 rdstate,
//                   +20 exceptions, +24 rdbuf, +28 locale, +32.. callbacks and words
//   basic_ios (80): +72 tie, +76 fill (int; -1 = not yet widened)
//   streambuf (32): +0 vptr, +4 locale, +8 eback, +12 gptr, +16 egptr,
//                   +20 pbase, +24 pptr, +28 epptr
//   locale::facet:  +0 vptr, +4 shared owners
//   ctype<char>:    facet, +8 mask table (uint32[256]), +12 owns-table flag
//   A stream reaches its basic_ios through the vbase offset at vptr[-12].
#include "runtime/Support.hpp"
#include "runtime/Process.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace Hydrogen::Runtime {
std::string hvmIosFormatInt(uint32_t flags, int64_t v, bool isSigned, int bits, size_t& prefix);
std::string hvmIosFormatDouble(uint32_t flags, int prec, double d, size_t& prefix);

namespace {

constexpr uint32_t kFlags = 4, kPrecision = 8, kWidth = 12, kState = 16, kExceptions = 20, kBuf = 24, kLocale = 28,
                   kTie = 72, kFill = 76;
constexpr uint32_t kGptr = 12, kEgptr = 16;
constexpr uint32_t kBad = 1, kEof = 2, kFail = 4;
constexpr uint32_t fBoolalpha = 1, fInternal = 16, fLeft = 32, fRight = 128, fSkipws = 4096, fUnitbuf = 8192;
constexpr int32_t kEofChar = -1;
// streambuf virtual slots
enum Slot : uint32_t { sD1, sD0, sImbue, sSetbuf, sSeekoff, sSeekpos, sSync, sShowmanyc, sXsgetn, sUnderflow, sUflow,
                       sPbackfail, sXsputn, sOverflow };
constexpr uint32_t kRuneTypeOff = 8 + 32 + 4 + 4 + 4;   // _RuneLocale.__runetype
constexpr uint32_t kRuneSpace = 0x4000;                 // _CTYPE_S

struct LibcxxStreamState {
    explicit LibcxxStreamState(Process&) {}
    uint32_t ctypeId = 0, codecvtId = 0;       // std::ctype<char>::id, std::codecvt<char, char, mbstate_t>::id
    uint32_t ctypeVptr = 0, codecvtVptr = 0;
    uint32_t ctype = 0, codecvt = 0;           // the "C" locale's facets, made on first use
    uint32_t locale = 0;                       // the classic locale's implementation object
    std::mutex mu;                             // two threads can make their first stream call together
};
// The name is unique on purpose: Process::service finds its object by type name, and clang gives two
// `State` structs in different anonymous namespaces the same one.
LibcxxStreamState& St(Ctx& x) { return x.proc.service<LibcxxStreamState>(); }

uint32_t rd(Ctx& x, uint32_t a) { return x.mem.read<uint32_t>(a); }
void wr(Ctx& x, uint32_t a, uint32_t v) { x.mem.write<uint32_t>(a, v); }

uint32_t iosOf(Ctx& x, uint32_t stream) { return stream + uint32_t(int32_t(rd(x, rd(x, stream) - 12))); }
uint32_t topOf(Ctx& x, uint32_t sub) { return sub + uint32_t(int32_t(rd(x, rd(x, sub) - 8))); }

// A guest range the host is about to touch directly: every page must allow `prot`, or the
// guest gets the fault it would have had on a real device.
void need(Ctx& x, uint32_t addr, uint64_t bytes, uint8_t prot, const char* what) {
    if (!bytes) return;
    auto fault = [&](uint32_t at) { throw CPU::GuestFault(CPU::Exit::MemFault, at, x.cpu.r[CPU::kPC], uint32_t(x.cpu.thumb), what); };
    if (uint64_t(addr) + bytes > 0x100000000ull) fault(addr);
    constexpr uint32_t sh = Memory::GuestArena::kGuestPageShift;
    const uint64_t last = (uint64_t(addr) + bytes - 1) >> sh;
    for (uint64_t pg = addr >> sh; pg <= last; ++pg)
        if ((x.mem.protAt(uint32_t(pg << sh)) & prot) != prot) fault(std::max<uint32_t>(addr, uint32_t(pg << sh)));
}
uint64_t span(uint32_t low, uint32_t high) { return high > low ? uint64_t(high) - low : 0; }

int32_t vcall(Ctx& x, uint32_t obj, uint32_t slot, std::initializer_list<uint32_t> args) {
    std::vector<uint32_t> a{obj};
    a.insert(a.end(), args);
    return int32_t(uint32_t(x.proc.callGuest(x.cpu, rd(x, rd(x, obj) + slot * 4), a)));
}

// ios_base::clear / setstate. A stream without a buffer is always bad.
void iosClear(Ctx& x, uint32_t ios, uint32_t st) {
    const uint32_t s = st | (rd(x, ios + kBuf) ? 0 : kBad);
    wr(x, ios + kState, s);
    if (s & rd(x, ios + kExceptions))
        x.proc.log(LogLevel::Warning, "ios_base::clear: the app asked for an ios_base::failure exception (not thrown)");
}
void iosSetstate(Ctx& x, uint32_t ios, uint32_t st) { iosClear(x, ios, rd(x, ios + kState) | st); }
void b_clear(Ctx& x, GuestPtr ios, uint32_t st) { iosClear(x, ios, st); }

// ---- streambuf access through the get/put areas and the virtuals
int32_t sbSgetc(Ctx& x, uint32_t sb) {
    const uint32_t g = rd(x, sb + kGptr);
    if (g && g < rd(x, sb + kEgptr)) return x.mem.read<uint8_t>(g);
    return vcall(x, sb, sUnderflow, {});
}
int32_t sbSbumpc(Ctx& x, uint32_t sb) {
    const uint32_t g = rd(x, sb + kGptr);
    if (g && g < rd(x, sb + kEgptr)) { wr(x, sb + kGptr, g + 1); return x.mem.read<uint8_t>(g); }
    return vcall(x, sb, sUflow, {});
}
uint32_t sbSputn(Ctx& x, uint32_t sb, const char* data, uint32_t n) {
    if (!n) return 0;
    const uint32_t tmp = x.proc.heap().alloc(n);
    if (!tmp) return 0;
    std::memcpy(x.mem.host(tmp), data, n);
    const int32_t r = vcall(x, sb, sXsputn, {tmp, n});
    x.proc.heap().free(tmp);
    return r < 0 ? 0 : uint32_t(r);
}
// ostream::flush on a tied stream (or the stream itself).
void osFlush(Ctx& x, uint32_t os) {
    const uint32_t ios = iosOf(x, os);
    const uint32_t sb = rd(x, ios + kBuf);
    if (sb && vcall(x, sb, sSync, {}) == -1) iosSetstate(x, ios, kBad);
}
void flushTie(Ctx& x, uint32_t ios) {
    if (const uint32_t tie = rd(x, ios + kTie)) osFlush(x, tie);
}

// ---- formatted output
char fillOf(Ctx& x, uint32_t ios) {
    const int32_t f = int32_t(rd(x, ios + kFill));
    return f == kEofChar ? ' ' : char(f);
}
// Pads to the stream's width, which every formatted insertion resets.
std::string pad(Ctx& x, uint32_t ios, std::string s, size_t prefix) {
    const int32_t w = int32_t(rd(x, ios + kWidth));
    wr(x, ios + kWidth, 0);
    if (w <= 0 || size_t(w) <= s.size()) return s;
    const std::string f(size_t(w) - s.size(), fillOf(x, ios));
    const uint32_t adj = rd(x, ios + kFlags) & (fLeft | fRight | fInternal);
    if (adj == fLeft) return s + f;
    if (adj == fInternal) return s.substr(0, prefix) + f + s.substr(prefix);
    return f + s;
}
uint32_t emit(Ctx& x, uint32_t os, std::string s, size_t prefix) {
    const uint32_t ios = iosOf(x, os);
    if (rd(x, ios + kState)) return os;   // the sentry failed: nothing is written, the width stays
    flushTie(x, ios);
    const std::string out = pad(x, ios, std::move(s), prefix);
    const uint32_t sb = rd(x, ios + kBuf);
    if (!sb || sbSputn(x, sb, out.data(), uint32_t(out.size())) != out.size()) iosSetstate(x, ios, kBad | kFail);
    if (sb && (rd(x, ios + kFlags) & fUnitbuf)) osFlush(x, os);
    return os;
}
uint32_t putInt(Ctx& x, uint32_t os, int64_t v, bool isSigned, int bits) {
    size_t prefix = 0;
    std::string text = hvmIosFormatInt(rd(x, iosOf(x, os) + kFlags), v, isSigned, bits, prefix);
    return emit(x, os, std::move(text), prefix);
}
uint32_t putDouble(Ctx& x, uint32_t os, double d) {
    const uint32_t ios = iosOf(x, os);
    size_t prefix = 0;
    std::string text = hvmIosFormatDouble(rd(x, ios + kFlags), int32_t(rd(x, ios + kPrecision)), d, prefix);
    return emit(x, os, std::move(text), prefix);
}
// A 64-bit argument after `this`: r1:r2 (iOS armv7 does not pair registers).
int64_t arg64(Ctx& x) { return int64_t((uint64_t(x.cpu.r[2]) << 32) | x.cpu.r[1]); }

uint32_t o_int(Ctx& x, GuestPtr os, int32_t v) { return putInt(x, os, v, true, 32); }
uint32_t o_uint(Ctx& x, GuestPtr os, uint32_t v) { return putInt(x, os, int64_t(v), false, 32); }
// hex and oct print a short as its own 16 bits
uint32_t o_short(Ctx& x, GuestPtr os, int32_t v) {
    const uint32_t base = rd(x, iosOf(x, os) + kFlags) & (8u | 64u);
    return base ? putInt(x, os, int64_t(uint16_t(v)), false, 32) : putInt(x, os, int16_t(v), true, 32);
}
uint32_t o_ushort(Ctx& x, GuestPtr os, uint32_t v) { return putInt(x, os, int64_t(uint16_t(v)), false, 32); }
void o_longlong(Ctx& x) { x.cpu.r[0] = putInt(x, x.cpu.r[0], arg64(x), true, 64); }
void o_ulonglong(Ctx& x) { x.cpu.r[0] = putInt(x, x.cpu.r[0], arg64(x), false, 64); }
uint32_t o_float(Ctx& x, GuestPtr os, uint32_t bits) { float f; std::memcpy(&f, &bits, 4); return putDouble(x, os, f); }
void o_double(Ctx& x) {
    const uint64_t b = uint64_t(arg64(x));
    double d;
    std::memcpy(&d, &b, 8);
    x.cpu.r[0] = putDouble(x, x.cpu.r[0], d);
}
uint32_t o_bool(Ctx& x, GuestPtr os, uint32_t v) {
    if (rd(x, iosOf(x, os) + kFlags) & fBoolalpha) return emit(x, os, (v & 0xFF) ? "true" : "false", 0);
    return putInt(x, os, (v & 0xFF) ? 1 : 0, true, 32);
}
uint32_t o_ptr(Ctx& x, GuestPtr os, uint32_t v) {   // num_put prints a pointer with %p
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%x", v);
    return emit(x, os, buf, 2);
}
// ostream::write: unformatted, no width or fill
uint32_t o_write(Ctx& x, GuestPtr os, GuestPtr s, int32_t n) {
    const uint32_t ios = iosOf(x, os);
    if (rd(x, ios + kState)) return os;
    flushTie(x, ios);
    if (n > 0) {
        const uint32_t sb = rd(x, ios + kBuf);
        if (!sb || vcall(x, sb, sXsputn, {s, uint32_t(n)}) != n) iosSetstate(x, ios, kBad);
    }
    if (rd(x, ios + kBuf) && (rd(x, ios + kFlags) & fUnitbuf)) osFlush(x, os);
    return os;
}
uint32_t o_flush(Ctx& x, GuestPtr os) { osFlush(x, os); return os; }
uint32_t o_put(Ctx& x, GuestPtr os, uint32_t c) {
    const uint32_t ios = iosOf(x, os);
    if (rd(x, ios + kState)) return os;
    flushTie(x, ios);
    const char ch = char(c);
    const uint32_t sb = rd(x, ios + kBuf);
    if (!sb || sbSputn(x, sb, &ch, 1) != 1) iosSetstate(x, ios, kBad);
    if (sb && (rd(x, ios + kFlags) & fUnitbuf)) osFlush(x, os);
    return os;
}

// ---- sentries
// ostream::sentry { bool ok; ostream& os; }
uint32_t osSentryCtor(Ctx& x, GuestPtr self, GuestPtr os) {
    const uint32_t ios = iosOf(x, os);
    wr(x, self + 4, os);
    bool ok = false;
    if (rd(x, ios + kState) == 0) {
        flushTie(x, ios);
        ok = true;
    }
    x.mem.write<uint8_t>(self, ok ? 1 : 0);
    return self;
}
uint32_t osSentryDtor(Ctx& x, GuestPtr self) {
    const uint32_t os = rd(x, self + 4);
    if (!os) return self;
    const uint32_t ios = iosOf(x, os);
    if (rd(x, ios + kBuf) && rd(x, ios + kState) == 0 && (rd(x, ios + kFlags) & fUnitbuf)) osFlush(x, os);
    return self;
}
bool isSpace(Ctx& x, int32_t c) {
    if (c < 0 || c > 127) return false;
    if (const auto rune = x.proc.lookupSymbol("__DefaultRuneLocale"))
        return (rd(x, *rune + kRuneTypeOff + uint32_t(c) * 4) & kRuneSpace) != 0;
    return c == ' ' || (c >= '\t' && c <= '\r');
}
// istream::sentry { bool ok; }: flushes the tied stream and skips leading whitespace.
uint32_t isSentryCtor(Ctx& x, GuestPtr self, GuestPtr is, uint32_t noskipws) {
    const uint32_t ios = iosOf(x, is);
    bool ok = false;
    if (rd(x, ios + kState) == 0) {
        flushTie(x, ios);
        const uint32_t sb = rd(x, ios + kBuf);
        if (!(noskipws & 0xFF) && (rd(x, ios + kFlags) & fSkipws) && sb) {
            for (;;) {
                const int32_t c = sbSgetc(x, sb);
                if (c == kEofChar) { iosSetstate(x, ios, kFail | kEof); break; }
                if (!isSpace(x, c)) break;
                sbSbumpc(x, sb);
            }
        }
        ok = rd(x, ios + kState) == 0;
    } else {
        iosSetstate(x, ios, kFail);
    }
    x.mem.write<uint8_t>(self, ok ? 1 : 0);
    return self;
}

// ---- unformatted and formatted input. basic_istream: +0 vptr, +4 gcount.
bool inSentry(Ctx& x, uint32_t is, bool noskipws) {
    const uint32_t tmp = x.proc.heap().allocZeroed(4);
    isSentryCtor(x, tmp, is, noskipws ? 1 : 0);
    const bool ok = x.mem.read<uint8_t>(tmp) != 0;
    x.proc.heap().free(tmp);
    return ok;
}
// pos_type streambuf::pubseekoff(off, way, which) through the virtual seekoff:
// result by hidden pointer in r0, this in r1, the 64-bit offset in r2:r3, way and which on the stack.
// libc++'s fpos is the 128-byte conversion state followed by the offset.
int64_t sbSeekoff(Ctx& x, uint32_t sb, int64_t off, uint32_t way, uint32_t which) {
    const uint32_t ret = x.proc.heap().allocZeroed(136);
    x.mem.write<uint64_t>(ret + 128, uint64_t(int64_t(-1)));
    x.proc.callGuest(x.cpu, rd(x, rd(x, sb) + sSeekoff * 4), {ret, sb, uint32_t(uint64_t(off)), uint32_t(uint64_t(off) >> 32), way, which});
    const int64_t pos = int64_t(x.mem.read<uint64_t>(ret + 128));
    x.proc.heap().free(ret);
    return pos;
}
uint32_t i_read(Ctx& x, GuestPtr is, GuestPtr s, int32_t n) {
    const uint32_t ios = iosOf(x, is);
    wr(x, is + 4, 0);
    if (!inSentry(x, is, true)) return is;   // the sentry has set failbit
    const int32_t got = n > 0 ? vcall(x, rd(x, ios + kBuf), sXsgetn, {s, uint32_t(n)}) : 0;
    wr(x, is + 4, uint32_t(got < 0 ? 0 : got));
    if (got != n) iosSetstate(x, ios, kFail | kEof);
    return is;
}
// istream& seekg(off_type off, seekdir way): this r0, off r1:r2, way r3
void i_seekgOff(Ctx& x) {
    const uint32_t is = x.cpu.r[0], way = x.cpu.r[3];
    const int64_t off = arg64(x);
    const uint32_t ios = iosOf(x, is);
    iosClear(x, ios, rd(x, ios + kState) & ~kEof);
    if (inSentry(x, is, true) && sbSeekoff(x, rd(x, ios + kBuf), off, way, 8 /*in*/) == -1) iosSetstate(x, ios, kFail);
    x.cpu.r[0] = is;
}
// pos_type tellg(): result through r0, this in r1
void i_tellg(Ctx& x) {
    const uint32_t ret = x.cpu.r[0], is = x.cpu.r[1];
    const uint32_t ios = iosOf(x, is);
    int64_t pos = -1;
    if (inSentry(x, is, true)) pos = sbSeekoff(x, rd(x, ios + kBuf), 0, 1 /*cur*/, 8 /*in*/);
    for (uint32_t i = 0; i < 128; i += 4) wr(x, ret + i, 0);
    x.mem.write<uint64_t>(ret + 128, uint64_t(pos));
}
// getline(char* s, streamsize n, char delim): the delimiter is consumed and not stored
uint32_t i_getline(Ctx& x, GuestPtr is, GuestPtr s, int32_t n, uint32_t delimArg) {
    const uint32_t ios = iosOf(x, is);
    const int32_t delim = int32_t(delimArg & 0xFF);
    uint32_t count = 0, stored = 0, state = 0;
    wr(x, is + 4, 0);
    if (n > 0) need(x, s, uint32_t(n), Memory::kProtRW, "istream::getline buffer");
    if (inSentry(x, is, true)) {
        const uint32_t sb = rd(x, ios + kBuf);
        for (;;) {
            const int32_t c = sbSgetc(x, sb);
            if (c == kEofChar) { state |= kEof; break; }
            if (c == delim) { sbSbumpc(x, sb); ++count; break; }
            if (int32_t(stored) >= n - 1) { state |= kFail; break; }
            x.mem.write<uint8_t>(s + stored++, uint8_t(c));
            sbSbumpc(x, sb);
            ++count;
        }
        if (count == 0) state |= kFail;
    }
    if (n > 0) x.mem.write<uint8_t>(s + stored, 0);
    wr(x, is + 4, count);
    if (state) iosSetstate(x, ios, state);
    return is;
}
// operator>>(int&): skips whitespace, reads an optionally signed number in the stream's base
uint32_t i_extractInt(Ctx& x, GuestPtr is, GuestPtr out) {
    const uint32_t ios = iosOf(x, is);
    if (!inSentry(x, is, false)) return is;
    const uint32_t sb = rd(x, ios + kBuf);
    const uint32_t basefield = rd(x, ios + kFlags) & (2u | 8u | 64u);
    const int base = basefield == 8 ? 16 : basefield == 64 ? 8 : 10;
    uint32_t state = 0;
    bool negative = false, any = false;
    int32_t c = sbSgetc(x, sb);
    if (c == '-' || c == '+') { negative = c == '-'; sbSbumpc(x, sb); c = sbSgetc(x, sb); }
    int64_t v = 0;
    bool overflow = false;
    for (;; c = sbSgetc(x, sb)) {
        if (c == kEofChar) { state |= kEof; break; }
        int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99;
        if (digit >= base) break;
        any = true;
        v = v * base + digit;
        if (v > 0x80000000ll) { overflow = true; v = 0x80000000ll; }
        sbSbumpc(x, sb);
    }
    int32_t result = 0;
    if (!any) state |= kFail;
    else if (overflow || (!negative && v > 0x7FFFFFFFll)) { state |= kFail; result = negative ? INT32_MIN : INT32_MAX; }
    else result = int32_t(negative ? -v : v);
    x.mem.write<uint32_t>(out, uint32_t(result));
    if (state) iosSetstate(x, ios, state);
    return is;
}
int32_t i_get(Ctx& x, GuestPtr is) {
    const uint32_t ios = iosOf(x, is);
    wr(x, is + 4, 0);
    if (!inSentry(x, is, true)) return kEofChar;
    const int32_t c = sbSbumpc(x, rd(x, ios + kBuf));
    if (c == kEofChar) iosSetstate(x, ios, kFail | kEof);
    else wr(x, is + 4, 1);
    return c;
}
int32_t i_peek(Ctx& x, GuestPtr is) {
    const uint32_t ios = iosOf(x, is);
    wr(x, is + 4, 0);
    if (!inSentry(x, is, true)) return kEofChar;
    const int32_t c = sbSgetc(x, rd(x, ios + kBuf));
    if (c == kEofChar) iosSetstate(x, ios, kEof);
    return c;
}
void iosSetBadbit(Ctx& x, GuestPtr ios) { wr(x, ios + kState, rd(x, ios + kState) | kBad); }

// ---- destructors. The stream classes own nothing; a deleting destructor frees the whole
// object, found through the offset to top when it is reached through a thunk.
uint32_t streamDtor(Ctx&, GuestPtr self) { return self; }
void streamDtorDelete(Ctx& x, GuestPtr self) { if (self) x.proc.heap().free(topOf(x, self)); }

// ---- locale and facets. There is one locale, the classic "C" one.
uint32_t classicLocale(Ctx& x) {
    LibcxxStreamState& s = St(x);
    std::lock_guard<std::mutex> lk(s.mu);
    if (!s.locale) s.locale = x.proc.heap().allocZeroed(16);
    return s.locale;
}
uint32_t locDefault(Ctx& x, GuestPtr self) { wr(x, self, classicLocale(x)); return self; }
uint32_t locCopy(Ctx& x, GuestPtr self, GuestPtr other) { wr(x, self, rd(x, other)); return self; }
uint32_t locAssign(Ctx& x, GuestPtr self, GuestPtr other) { wr(x, self, rd(x, other)); return self; }
uint32_t locDtor(Ctx&, GuestPtr self) { return self; }
// locale ios_base::getloc() const — the result goes through r0, this is in r1
void iosGetloc(Ctx& x) {
    const uint32_t out = x.cpu.r[0], ios = x.cpu.r[1];
    const uint32_t loc = rd(x, ios + kLocale);
    wr(x, out, loc ? loc : classicLocale(x));
}
uint32_t facetFor(Ctx& x, uint32_t id) {
    LibcxxStreamState& s = St(x);
    std::lock_guard<std::mutex> lk(s.mu);   // a facet is published only once it is complete
    if (id && id == s.ctypeId) {
        if (!s.ctype) {
            const uint32_t f = x.proc.heap().allocZeroed(16);
            wr(x, f, s.ctypeVptr);
            wr(x, f + 4, 1);   // never released
            if (const auto rune = x.proc.lookupSymbol("__DefaultRuneLocale")) wr(x, f + 8, *rune + kRuneTypeOff);
            s.ctype = f;
        }
        return s.ctype;
    }
    if (id && id == s.codecvtId) {
        if (!s.codecvt) {
            const uint32_t f = x.proc.heap().allocZeroed(8);
            wr(x, f, s.codecvtVptr);
            wr(x, f + 4, 1);
            s.codecvt = f;
        }
        return s.codecvt;
    }
    return 0;
}
bool locHasFacet(Ctx& x, GuestPtr, GuestPtr id) { return facetFor(x, id) != 0; }
uint32_t locUseFacet(Ctx& x, GuestPtr, GuestPtr id) {
    const uint32_t f = facetFor(x, id);
    if (!f) x.proc.log(LogLevel::Warning, "locale::use_facet: only ctype<char> and codecvt<char, char, mbstate_t> exist; this facet is missing");
    return f;
}
uint32_t facetDtor(Ctx&, GuestPtr self) { return self; }
void facetNop(Ctx&, GuestPtr) {}   // the classic facets are never deleted or released

// ctype<char>, "C" locale: ASCII case mapping, widen and narrow are the identity.
uint32_t ctToupper(Ctx&, GuestPtr, uint32_t c) { c &= 0xFF; return (c >= 'a' && c <= 'z') ? c - 32 : c; }
uint32_t ctTolower(Ctx&, GuestPtr, uint32_t c) { c &= 0xFF; return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
uint32_t ctToupperRange(Ctx& x, GuestPtr, GuestPtr low, GuestPtr high) {
    need(x, low, span(low, high), Memory::kProtRW, "ctype<char>::toupper range");
    for (uint32_t p = low; p < high; ++p) x.mem.write<uint8_t>(p, uint8_t(ctToupper(x, 0, x.mem.read<uint8_t>(p))));
    return high;
}
uint32_t ctTolowerRange(Ctx& x, GuestPtr, GuestPtr low, GuestPtr high) {
    need(x, low, span(low, high), Memory::kProtRW, "ctype<char>::tolower range");
    for (uint32_t p = low; p < high; ++p) x.mem.write<uint8_t>(p, uint8_t(ctTolower(x, 0, x.mem.read<uint8_t>(p))));
    return high;
}
uint32_t ctWiden(Ctx&, GuestPtr, uint32_t c) { return c & 0xFF; }
uint32_t ctWidenRange(Ctx& x, GuestPtr, GuestPtr low, GuestPtr high, GuestPtr to) {
    need(x, low, span(low, high), Memory::kProtRead, "ctype<char>::widen source");
    need(x, to, span(low, high), Memory::kProtRW, "ctype<char>::widen destination");
    if (high > low) std::memmove(x.mem.host(to), x.mem.host(low), high - low);
    return high;
}
// do_narrow(c, default): a character outside ASCII becomes the default
uint32_t ctNarrow(Ctx&, GuestPtr, uint32_t c, uint32_t dflt) { c &= 0xFF; return c < 128 ? c : (dflt & 0xFF); }
uint32_t ctNarrowRange(Ctx& x, GuestPtr, GuestPtr low, GuestPtr high, uint32_t dflt, GuestPtr to) {
    need(x, low, span(low, high), Memory::kProtRead, "ctype<char>::narrow source");
    need(x, to, span(low, high), Memory::kProtRW, "ctype<char>::narrow destination");
    for (uint32_t p = low; p < high; ++p, ++to) x.mem.write<uint8_t>(to, uint8_t(ctNarrow(x, 0, x.mem.read<uint8_t>(p), dflt)));
    return high;
}

// codecvt<char, char, mbstate_t>: no conversion.
// do_out / do_in(this, state, from, from_end, from_next&, to, to_end, to_next&)
void cvNoconv(Ctx& x) {
    const uint32_t from = x.cpu.r[2], sp = x.cpu.r[CPU::kSP];
    const uint32_t fromNext = rd(x, sp), to = rd(x, sp + 4), toNext = rd(x, sp + 12);
    if (fromNext) wr(x, fromNext, from);
    if (toNext) wr(x, toNext, to);
    x.cpu.r[0] = 3;   // codecvt_base::noconv
}
uint32_t cvUnshift(Ctx& x, GuestPtr, GuestPtr, GuestPtr to, GuestPtr, GuestPtr toNext) {
    if (toNext) wr(x, toNext, to);
    return 3;
}
uint32_t cvOne(Ctx&, GuestPtr) { return 1; }   // do_encoding, do_always_noconv, do_max_length
uint32_t cvLength(Ctx&, GuestPtr, GuestPtr, GuestPtr from, GuestPtr end, uint32_t max) {
    return end > from ? std::min(end - from, max) : 0;
}

// void std::__sort<__less<unsigned long long>&, unsigned long long*>(first, last, comp)
void sortU64(Ctx& x, GuestPtr first, GuestPtr last, GuestPtr) {
    if (last <= first) return;
    const uint32_t n = (last - first) / 8;
    need(x, first, uint64_t(n) * 8, Memory::kProtRW, "std::__sort range");   // before any allocation
    std::vector<uint64_t> v(n);
    std::memcpy(v.data(), x.mem.host(first), size_t(n) * 8);
    std::sort(v.begin(), v.end());
    std::memcpy(x.mem.host(first), v.data(), size_t(n) * 8);
}
[[noreturn]] void throwOutOfRange(Ctx&, GuestPtr) { throw std::runtime_error("std::__1::out_of_range"); }

} // namespace

void registerLibcxxStreams(Process& p) {
    LibcxxStreamState& s = p.service<LibcxxStreamState>();
    auto vtable = [&](const std::string& sym, std::initializer_list<uint32_t> slots) {
        const uint32_t a = p.defineDataBlock(sym, 8 + uint32_t(slots.size()) * 4);
        uint32_t i = 0;
        for (uint32_t f : slots) p.mem().write<uint32_t>(a + 8 + 4 * i++, f);
        return a + 8;
    };

    // locale and the two facets a char stream needs
    s.ctypeId = p.defineDataBlock("__ZNSt3__15ctypeIcE2idE", 8);
    s.codecvtId = p.defineDataBlock("__ZNSt3__17codecvtIcc11__mbstate_tE2idE", 8);
    const uint32_t fD1 = p.def<&facetDtor>("__ZNSt3__16locale5facetD2Ev");
    const uint32_t fD0 = supportDef<&facetNop>(p, "__ZNSt3__16locale5facetD0Ev", SupportStatus::Real, "The classic locale's facets live as long as the process: deleting one does nothing");
    const uint32_t fZero = supportDef<&facetNop>(p, "__ZNSt3__16locale5facet16__on_zero_sharedEv", SupportStatus::Real, "The classic locale's facets are never released");
    s.ctypeVptr = vtable("__ZTVNSt3__15ctypeIcEE",
        {fD1, fD0, fZero,
         p.def<&ctToupper>("__ZNKSt3__15ctypeIcE10do_toupperEc"), p.def<&ctToupperRange>("__ZNKSt3__15ctypeIcE10do_toupperEPcPKc"),
         p.def<&ctTolower>("__ZNKSt3__15ctypeIcE10do_tolowerEc"), p.def<&ctTolowerRange>("__ZNKSt3__15ctypeIcE10do_tolowerEPcPKc"),
         p.def<&ctWiden>("__ZNKSt3__15ctypeIcE8do_widenEc"), p.def<&ctWidenRange>("__ZNKSt3__15ctypeIcE8do_widenEPKcS3_Pc"),
         p.def<&ctNarrow>("__ZNKSt3__15ctypeIcE9do_narrowEcc"), p.def<&ctNarrowRange>("__ZNKSt3__15ctypeIcE9do_narrowEPKcS3_cPc")});
    const uint32_t cvConv = p.defineFunction("__ZNKSt3__17codecvtIcc11__mbstate_tE6do_outERS1_PKcS5_RS5_PcS7_RS7_", cvNoconv);
    p.defineFunction("__ZNKSt3__17codecvtIcc11__mbstate_tE5do_inERS1_PKcS5_RS5_PcS7_RS7_", cvNoconv);
    const uint32_t cvOneFn = supportDef<&cvOne>(p, "__ZNKSt3__17codecvtIcc11__mbstate_tE11do_encodingEv", SupportStatus::Real, "codecvt<char, char>: one external char per internal char");
    supportDef<&cvOne>(p, "__ZNKSt3__17codecvtIcc11__mbstate_tE16do_always_noconvEv", SupportStatus::Real, "codecvt<char, char> never converts");
    supportDef<&cvOne>(p, "__ZNKSt3__17codecvtIcc11__mbstate_tE13do_max_lengthEv", SupportStatus::Real, "codecvt<char, char>: at most one external char per internal char");
    s.codecvtVptr = vtable("__ZTVNSt3__17codecvtIcc11__mbstate_tEE",
        {fD1, fD0, fZero, cvConv, cvConv,
         p.def<&cvUnshift>("__ZNKSt3__17codecvtIcc11__mbstate_tE10do_unshiftERS1_PcS4_RS4_"),
         cvOneFn, cvOneFn,
         p.def<&cvLength>("__ZNKSt3__17codecvtIcc11__mbstate_tE9do_lengthERS1_PKcS5_m"),
         cvOneFn});
    p.def<&locDefault>("__ZNSt3__16localeC1Ev");
    p.def<&locDefault>("__ZNSt3__16localeC2Ev");
    p.def<&locCopy>("__ZNSt3__16localeC1ERKS0_");
    p.def<&locCopy>("__ZNSt3__16localeC2ERKS0_");
    p.def<&locAssign>("__ZNSt3__16localeaSERKS0_");
    p.def<&locDtor>("__ZNSt3__16localeD1Ev");
    p.def<&locDtor>("__ZNSt3__16localeD2Ev");
    p.def<&locHasFacet>("__ZNKSt3__16locale9has_facetERNS0_2idE");
    p.def<&locUseFacet>("__ZNKSt3__16locale9use_facetERNS0_2idE");
    p.defineFunction("__ZNKSt3__18ios_base6getlocEv", iosGetloc);
    p.def<&b_clear>("__ZNSt3__18ios_base5clearEj");

    // ostream
    const std::string OS = "NSt3__113basic_ostreamIcNS_11char_traitsIcEEE";
    p.def<&osSentryCtor>("__Z" + OS + "6sentryC1ERS3_");
    p.def<&osSentryCtor>("__Z" + OS + "6sentryC2ERS3_");
    p.def<&osSentryDtor>("__Z" + OS + "6sentryD1Ev");
    p.def<&osSentryDtor>("__Z" + OS + "6sentryD2Ev");
    p.def<&o_write>("__Z" + OS + "5writeEPKci");
    p.def<&o_put>("__Z" + OS + "3putEc");
    p.def<&o_flush>("__Z" + OS + "5flushEv");
    p.def<&o_bool>("__Z" + OS + "lsEb");
    p.def<&o_short>("__Z" + OS + "lsEs");
    p.def<&o_ushort>("__Z" + OS + "lsEt");
    p.def<&o_int>("__Z" + OS + "lsEi");
    p.def<&o_uint>("__Z" + OS + "lsEj");
    p.def<&o_int>("__Z" + OS + "lsEl");
    p.def<&o_uint>("__Z" + OS + "lsEm");
    p.defineFunction("__Z" + OS + "lsEx", o_longlong);
    p.defineFunction("__Z" + OS + "lsEy", o_ulonglong);
    p.def<&o_float>("__Z" + OS + "lsEf");
    p.defineFunction("__Z" + OS + "lsEd", o_double);
    p.defineFunction("__Z" + OS + "lsEe", o_double);   // long double is double on armv7
    p.def<&o_ptr>("__Z" + OS + "lsEPKv");

    // istream
    const std::string IS = "NSt3__113basic_istreamIcNS_11char_traitsIcEEE";
    p.def<&isSentryCtor>("__Z" + IS + "6sentryC1ERS3_b");
    p.def<&isSentryCtor>("__Z" + IS + "6sentryC2ERS3_b");
    p.def<&i_read>("__Z" + IS + "4readEPci");
    p.defineFunction("__Z" + IS + "5seekgExNS_8ios_base7seekdirE", i_seekgOff);
    p.defineFunction("__Z" + IS + "5tellgEv", i_tellg);
    p.def<&i_getline>("__Z" + IS + "7getlineEPcic");
    p.def<&i_extractInt>("__Z" + IS + "rsERi");
    p.def<&i_extractInt>("__Z" + IS + "rsERl");
    p.def<&i_get>("__Z" + IS + "3getEv");
    p.def<&i_peek>("__Z" + IS + "4peekEv");
    p.def<&iosSetBadbit>("__ZNSt3__18ios_base33__set_badbit_and_consider_rethrowEv");

    // istream and iostream destructors (ostream's are with the streambuf in LibStdcxxStreams.cpp)
    const std::string IOS = "NSt3__114basic_iostreamIcNS_11char_traitsIcEEE";
    for (const std::string& cls : {IS, IOS}) {
        p.def<&streamDtor>("__Z" + cls + "D1Ev");
        p.def<&streamDtor>("__Z" + cls + "D2Ev");
        p.def<&streamDtorDelete>("__Z" + cls + "D0Ev");
        p.def<&streamDtor>("__ZTv0_n12_" + cls + "D1Ev");
        p.def<&streamDtorDelete>("__ZTv0_n12_" + cls + "D0Ev");
    }
    p.def<&streamDtor>("__ZThn8_" + IOS + "D1Ev");
    p.def<&streamDtorDelete>("__ZThn8_" + IOS + "D0Ev");

    // Vtables and the VTT. An app that builds a plain std::iostream / std::istream over its own
    // streambuf (Terraria's file reader) constructs it inline and takes these from the dylib.
    //   istream:  vptr, gcount, basic_ios @8      ostream: vptr, basic_ios @4
    //   iostream: istream vptr, gcount, ostream vptr @8, basic_ios @12
    // Each group is [vbase or vcall offset][offset to top][typeinfo][D1][D0]; the vptr is the
    // address of D1. A destructor reached through any of them finds the whole object from the
    // offset to top, so every D1 slot is the same function, and every D0 slot likewise.
    {
        const uint32_t d1 = p.registerHostFunction("std::__1 stream destructor", [](Ctx& x) { x.cpu.r[0] = streamDtor(x, x.cpu.r[0]); });
        const uint32_t d0 = p.registerHostFunction("std::__1 stream deleting destructor", [](Ctx& x) { streamDtorDelete(x, x.cpu.r[0]); });
        auto typeinfo = [&](const std::string& name) {
            if (const auto have = p.lookupSymbol(name)) return *have;
            return p.defineDataBlock(name, 12);
        };
        auto table = [&](const std::string& sym, uint32_t ti, std::initializer_list<std::pair<int32_t, int32_t>> groups) {
            const uint32_t a = p.defineDataBlock(sym, uint32_t(groups.size()) * 20);
            uint32_t at = a;
            for (const auto& [first, top] : groups) {
                p.mem().write<uint32_t>(at, uint32_t(first));
                p.mem().write<uint32_t>(at + 4, uint32_t(top));
                p.mem().write<uint32_t>(at + 8, ti);
                p.mem().write<uint32_t>(at + 12, d1);
                p.mem().write<uint32_t>(at + 16, d0);
                at += 20;
            }
            return a;
        };
        const std::string C = "IcNS_11char_traitsIcEEEE";
        const uint32_t tiIs = typeinfo("__ZTINSt3__113basic_istream" + C), tiOs = typeinfo("__ZTINSt3__113basic_ostream" + C);
        const uint32_t tiIos = typeinfo("__ZTINSt3__114basic_iostream" + C), tiBase = typeinfo("__ZTINSt3__19basic_ios" + C);
        if (!p.lookupSymbol("__ZTVNSt3__113basic_istream" + C)) table("__ZTVNSt3__113basic_istream" + C, tiIs, {{8, 0}, {-8, -8}});
        if (!p.lookupSymbol("__ZTVNSt3__113basic_ostream" + C)) table("__ZTVNSt3__113basic_ostream" + C, tiOs, {{4, 0}, {-4, -4}});
        if (!p.lookupSymbol("__ZTVNSt3__19basic_ios" + C)) {
            // no virtual base: [offset to top][typeinfo][D1][D0]
            const uint32_t a = p.defineDataBlock("__ZTVNSt3__19basic_ios" + C, 16);
            p.mem().write<uint32_t>(a + 4, tiBase);
            p.mem().write<uint32_t>(a + 8, d1);
            p.mem().write<uint32_t>(a + 12, d0);
        }
        const uint32_t vtIo = table("__ZTVNSt3__114basic_iostream" + C, tiIos, {{12, 0}, {4, -8}, {-12, -12}});
        // construction vtables for the two bases while a std::iostream is being built
        const uint32_t ctorIs = table("hvm_libcxx_ZTC_iostream_0_istream", tiIs, {{12, 0}, {-12, -12}});
        const uint32_t ctorOs = table("hvm_libcxx_ZTC_iostream_8_ostream", tiOs, {{4, 0}, {-4, -4}});
        const uint32_t vtt = p.defineDataBlock("__ZTTNSt3__114basic_iostream" + C, 28);
        uint32_t i = 0;
        for (uint32_t v : {vtIo + 12, ctorIs + 12, ctorIs + 32, ctorOs + 12, ctorOs + 32, vtIo + 52, vtIo + 32})
            p.mem().write<uint32_t>(vtt + 4 * i++, v);
    }

    p.def<&sortU64>("__ZNSt3__16__sortIRNS_6__lessIyyEEPyEEvT0_S5_T_");
    p.def<&throwOutOfRange>("__ZNKSt3__120__vector_base_commonILb1EE20__throw_out_of_rangeEv");
}

} // namespace Hydrogen::Runtime
