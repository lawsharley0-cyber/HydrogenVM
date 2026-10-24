// HydrogenVM — the parts of Apple's GCC 4.2 libstdc++ (libstdc++.6.dylib)
// that 32-bit apps import: ios_base::Init, the copy-on-write std::string
// entry points, std::list node linking, and file streams.
//
// Most of libstdc++ is header templates compiled into the app, so an app only
// imports what libstdc++ instantiates itself. The app's inline code still reads
// some fields of library objects directly, so those keep GCC's layout:
//
//  * std::string holds a char* to the data, preceded by its _Rep header
//    {length, capacity, refcount}. The empty string shares _S_empty_rep_storage.
//  * Streams: the app reserves the object and calls our constructor. Inline
//    code reads gcount() at istream+4 and the stream state through the
//    virtual-base offset in the vtable (vptr[-3]) at ios_base+20. We give each
//    stream a vtable whose virtual-base offset points at a private ios_base
//    area inside the object, and keep the FILE* on the host side. The filebuf
//    member (what is_open() receives) is at +8 (ifstream, ofstream) or +16
//    (fstream: it is 8-byte aligned), and fstream's ostream subobject is at +8.
#include "runtime/Process.hpp"
#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace Hydrogen::Runtime {
void registerLibStdcxxContainers(Process& p);
void registerLibStdcxxStreams(Process& p);
uint32_t hvmOstreamWrite(Ctx& x, uint32_t os, uint32_t buf, int32_t n);
uint32_t hvmIstreamRead(Ctx& x, uint32_t is, uint32_t buf, int32_t n);
void hvmStreamSeek(Ctx& x, uint32_t stream, int64_t off, uint32_t dir, uint32_t which);
int64_t hvmStreamTell(Ctx& x, uint32_t stream, uint32_t which);
namespace {

// --------------------------------------------------------------- streams ---

enum class Kind { In, Out, InOut, FileBuf };
constexpr uint32_t kVbase = 0x40;          // where our ios_base lives inside the object
constexpr uint32_t kStateOff = 20;         // ios_base::_M_streambuf_state
constexpr uint32_t kGoodbit = 0, kBadbit = 1, kEofbit = 2, kFailbit = 4;
constexpr uint32_t kApp = 1, kAte = 2, kBinary = 4, kIn = 8, kOut = 16, kTrunc = 32;

struct Stream { Kind kind; std::FILE* f = nullptr; };

struct Streams {
    explicit Streams(Process&) {}
    std::mutex mu;
    std::unordered_map<uint32_t, Stream> objects;   // keyed by the complete object's address
    std::unordered_map<uint32_t, std::FILE*> basicFiles; // keyed by std::__basic_file<char> self pointer
    uint32_t vtIn = 0, vtOut = 0, vtIo = 0, vtIoOstream = 0;
};
Streams& S(Ctx& x) { return x.proc.service<Streams>(); }

// filebuf holds an mbstate_t (with a long long), so it is 8-byte aligned.
uint32_t filebufOffset(Kind k) { return k == Kind::InOut ? 16 : 8; }

// The complete object for a pointer to the stream, its ostream part or its filebuf.
uint32_t objectFor(Ctx& x, uint32_t p, int part /*0 stream, 1 ostream, 2 filebuf*/) {
    Streams& s = S(x);
    std::lock_guard<std::mutex> lk(s.mu);
    auto has = [&](uint32_t o, auto pred) { auto it = s.objects.find(o); return it != s.objects.end() && pred(it->second.kind); };
    if (part == 0 && s.objects.count(p) && s.objects[p].kind != Kind::FileBuf) return p;
    if (part == 1) {
        if (has(p, [](Kind k) { return k == Kind::Out; })) return p;
        if (has(p - 8, [](Kind k) { return k == Kind::InOut; })) return p - 8;
    }
    if (part == 2) {
        if (has(p, [](Kind k) { return k == Kind::FileBuf; })) return p;
        for (Kind k : {Kind::In, Kind::Out, Kind::InOut})
            if (has(p - filebufOffset(k), [k](Kind kk) { return kk == k; })) return p - filebufOffset(k);
    }
    throw std::runtime_error("libstdc++: stream call on an object HydrogenVM didn't construct");
}
bool isFileStream(Ctx& x, uint32_t p, int part) {
    try { objectFor(x, p, part); return true; } catch (const std::runtime_error&) { return false; }
}

uint32_t state(Ctx& x, uint32_t obj) { return x.mem.read<uint32_t>(obj + kVbase + kStateOff); }
void setState(Ctx& x, uint32_t obj, uint32_t st) { x.mem.write<uint32_t>(obj + kVbase + kStateOff, st); }
void addState(Ctx& x, uint32_t obj, uint32_t bits) { setState(x, obj, state(x, obj) | bits); }

void construct(Ctx& x, uint32_t obj, Kind k) {
    Streams& s = S(x);
    // Clear our part of the object: header, ios_base area (136 bytes).
    for (uint32_t i = 0; i < kVbase + 136; i += 4) x.mem.write<uint32_t>(obj + i, 0);
    x.mem.write<uint32_t>(obj, k == Kind::In ? s.vtIn : k == Kind::Out ? s.vtOut : s.vtIo);
    if (k == Kind::InOut) x.mem.write<uint32_t>(obj + 8, s.vtIoOstream);
    x.mem.write<uint32_t>(obj + kVbase + 4, 6);     // precision
    x.mem.write<uint32_t>(obj + kVbase + 12, 0x1002); // flags: skipws | dec
    x.mem.write<uint32_t>(obj + kVbase + 120, obj + filebufOffset(k)); // _M_streambuf
    std::lock_guard<std::mutex> lk(s.mu);
    Stream& st = s.objects[obj];
    if (st.f) std::fclose(st.f);
    st = Stream{k, nullptr};
}

void destroy(Ctx& x, uint32_t obj) {
    Streams& s = S(x);
    std::lock_guard<std::mutex> lk(s.mu);
    auto it = s.objects.find(obj);
    if (it == s.objects.end()) return;
    if (it->second.f) std::fclose(it->second.f);
    s.objects.erase(it);
}

std::FILE* fileOf(Ctx& x, uint32_t obj) {
    Streams& s = S(x);
    std::lock_guard<std::mutex> lk(s.mu);
    auto it = s.objects.find(obj);
    return it == s.objects.end() ? nullptr : it->second.f;
}

bool openFileObj(Ctx& x, uint32_t obj, GuestPtr path, uint32_t mode) {
    Kind k;
    {
        std::lock_guard<std::mutex> lk(S(x).mu);
        k = S(x).objects.at(obj).kind;
    }
    if (k == Kind::In) mode |= kIn;
    if (k == Kind::Out) mode |= kOut;
    if ((mode & (kIn | kOut | kApp)) == 0) mode |= kIn;
    if (fileOf(x, obj)) {
        if (k != Kind::FileBuf) addState(x, obj, kFailbit);
        return false;
    }
    const uint32_t m = mode & (kIn | kOut | kTrunc | kApp);
    const char* fm = nullptr;
    if (m == kIn) fm = "rb";
    else if (m == kOut || m == (kOut | kTrunc)) fm = "wb";
    else if (m == (kOut | kApp) || m == kApp) fm = "ab";
    else if (m == (kIn | kOut)) fm = "r+b";
    else if (m == (kIn | kOut | kTrunc)) fm = "w+b";
    else if (m == (kIn | kOut | kApp) || m == (kIn | kApp)) fm = "a+b";
    std::FILE* f = (fm && path) ? std::fopen(x.proc.hostPath(x.proc.readCString(path)).c_str(), fm) : nullptr;
    if (f && (mode & kAte)) std::fseek(f, 0, SEEK_END);
    if (!f) {
        if (k != Kind::FileBuf) addState(x, obj, kFailbit);
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(S(x).mu);
        S(x).objects[obj].f = f;
    }
    if (k != Kind::FileBuf) setState(x, obj, kGoodbit);
    return true;
}

void open(Ctx& x, uint32_t obj, GuestPtr path, uint32_t mode) { openFileObj(x, obj, path, mode); }

bool closeFileObj(Ctx& x, uint32_t obj) {
    std::FILE* f = nullptr;
    Kind k = Kind::FileBuf;
    {
        std::lock_guard<std::mutex> lk(S(x).mu);
        auto it = S(x).objects.find(obj);
        if (it != S(x).objects.end()) { k = it->second.kind; f = it->second.f; it->second.f = nullptr; }
    }
    if (!f || std::fclose(f) != 0) {
        if (k != Kind::FileBuf) addState(x, obj, kFailbit);
        return false;
    }
    return true;
}

void close(Ctx& x, uint32_t obj) { closeFileObj(x, obj); }

// Constructors / destructors / open / close for the three stream types and basic_filebuf.
template <Kind K> void st_ctor(Ctx& x, GuestPtr self) { construct(x, self, K); }
template <Kind K> void st_ctorOpen(Ctx& x, GuestPtr self, GuestPtr path, uint32_t mode) { construct(x, self, K); open(x, self, path, mode); }
void st_dtor(Ctx& x, GuestPtr self) { destroy(x, self); }
void st_open(Ctx& x, GuestPtr self, GuestPtr path, uint32_t mode) { open(x, objectFor(x, self, 0), path, mode); }
void st_close(Ctx& x, GuestPtr self) { close(x, objectFor(x, self, 0)); }

uint32_t fb_ctor(Ctx& x, GuestPtr sb) {
    for (uint32_t i = 0; i < 32; i += 4) x.mem.write<uint32_t>(sb + i, 0);
    Streams& s = S(x);
    std::lock_guard<std::mutex> lk(s.mu);
    Stream& st = s.objects[sb];
    if (st.f) std::fclose(st.f);
    st = Stream{Kind::FileBuf, nullptr};
    return sb;
}
uint32_t fb_dtor(Ctx& x, GuestPtr sb) { destroy(x, sb); return sb; }
void fb_dtorDelete(Ctx& x, GuestPtr sb) { destroy(x, sb); if (x.proc.heap().blockSize(sb)) x.proc.heap().free(sb); }
uint32_t fb_open(Ctx& x, GuestPtr sb, GuestPtr path, uint32_t mode) {
    if (!isFileStream(x, sb, 2)) fb_ctor(x, sb);
    const uint32_t obj = objectFor(x, sb, 2);
    return openFileObj(x, obj, path, mode) ? sb : 0;
}
uint32_t fb_close(Ctx& x, GuestPtr sb) {
    if (!isFileStream(x, sb, 2)) return 0;
    const uint32_t obj = objectFor(x, sb, 2);
    return closeFileObj(x, obj) ? sb : 0;
}
bool fb_isOpen(Ctx& x, GuestPtr filebuf) {
    if (!isFileStream(x, filebuf, 2)) return false;
    return fileOf(x, objectFor(x, filebuf, 2)) != nullptr;
}

int whence(uint32_t dir) { return dir == 0 ? SEEK_SET : dir == 1 ? SEEK_CUR : SEEK_END; }

// -------------------------------------------------------- __basic_file<char> ---

uint32_t bfile_ctor(Ctx& x, GuestPtr self, GuestPtr lock) {
    x.mem.write<uint32_t>(self, 0);
    x.mem.write<uint8_t>(self + 4, 0);
    Streams& s = S(x);
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.basicFiles.count(self) && s.basicFiles[self]) std::fclose(s.basicFiles[self]);
    s.basicFiles[self] = nullptr;
    return self;
}

void bfile_dtor(Ctx& x, GuestPtr self) {
    Streams& s = S(x);
    std::lock_guard<std::mutex> lk(s.mu);
    auto it = s.basicFiles.find(self);
    if (it != s.basicFiles.end()) {
        if (it->second) std::fclose(it->second);
        s.basicFiles.erase(it);
    }
}

// A __basic_file the game reaches through inline code inside a stream or
// filebuf that HydrogenVM constructed (std::ifstream f(path); f.is_open()):
// its address is inside that object, so use the object's file.
std::FILE* hleFileAround(Ctx& x, GuestPtr self) {
    Streams& s = S(x);
    std::lock_guard<std::mutex> lk(s.mu);
    std::FILE* best = nullptr;
    uint32_t bestObj = 0;
    for (const auto& [obj, st] : s.objects)
        if (self >= obj && self < obj + 0x200 && obj >= bestObj) { bestObj = obj; best = st.f; }
    return bestObj ? best : nullptr;
}

bool bfile_isOpen(Ctx& x, GuestPtr self) {
    {
        Streams& s = S(x);
        std::lock_guard<std::mutex> lk(s.mu);
        auto it = s.basicFiles.find(self);
        if (it != s.basicFiles.end()) return it->second != nullptr;
    }
    return hleFileAround(x, self) != nullptr;
}

uint32_t bfile_open(Ctx& x, GuestPtr self, GuestPtr path, uint32_t mode, int32_t prot) {
    if (bfile_isOpen(x, self)) return 0;
    const uint32_t m = mode & (kIn | kOut | kTrunc | kApp);
    const char* fm = nullptr;
    if (m == kIn) fm = "rb";
    else if (m == kOut || m == (kOut | kTrunc)) fm = "wb";
    else if (m == (kOut | kApp) || m == kApp) fm = "ab";
    else if (m == (kIn | kOut)) fm = "r+b";
    else if (m == (kIn | kOut | kTrunc)) fm = "w+b";
    else if (m == (kIn | kOut | kApp) || m == (kIn | kApp)) fm = "a+b";
    std::FILE* f = (fm && path) ? std::fopen(x.proc.hostPath(x.proc.readCString(path)).c_str(), fm) : nullptr;
    if (f && (mode & kAte)) std::fseek(f, 0, SEEK_END);
    if (!f) return 0;
    {
        Streams& s = S(x);
        std::lock_guard<std::mutex> lk(s.mu);
        s.basicFiles[self] = f;
    }
    x.mem.write<uint8_t>(self + 4, 1);
    return self;
}

uint32_t bfile_close(Ctx& x, GuestPtr self) {
    std::FILE* f = nullptr;
    {
        Streams& s = S(x);
        std::lock_guard<std::mutex> lk(s.mu);
        auto it = s.basicFiles.find(self);
        if (it != s.basicFiles.end()) {
            f = it->second;
            it->second = nullptr;
        }
    }
    x.mem.write<uint8_t>(self + 4, 0);
    if (!f || std::fclose(f) != 0) return 0;
    return self;
}

int32_t bfile_fd(Ctx& x, GuestPtr self) {
    Streams& s = S(x);
    std::lock_guard<std::mutex> lk(s.mu);
    auto it = s.basicFiles.find(self);
    if (it != s.basicFiles.end() && it->second) return fileno(it->second);
    return -1;
}

uint32_t bfile_file(Ctx& x, GuestPtr self) {
    return bfile_isOpen(x, self) ? self : 0;
}

int32_t bfile_sync(Ctx& x, GuestPtr self) {
    Streams& s = S(x);
    std::lock_guard<std::mutex> lk(s.mu);
    auto it = s.basicFiles.find(self);
    if (it != s.basicFiles.end() && it->second) return std::fflush(it->second);
    return -1;
}

int32_t bfile_xsgetn(Ctx& x, GuestPtr self, GuestPtr buf, int32_t n) {
    if (n <= 0) return 0;
    Streams& s = S(x);
    std::lock_guard<std::mutex> lk(s.mu);
    auto it = s.basicFiles.find(self);
    if (it == s.basicFiles.end() || !it->second) return 0;
    return int32_t(std::fread(x.mem.host(buf), 1, size_t(n), it->second));
}

int32_t bfile_xsputn(Ctx& x, GuestPtr self, GuestPtr buf, int32_t n) {
    if (n <= 0) return 0;
    Streams& s = S(x);
    std::lock_guard<std::mutex> lk(s.mu);
    auto it = s.basicFiles.find(self);
    if (it == s.basicFiles.end() || !it->second) return 0;
    return int32_t(std::fwrite(x.mem.host(buf), 1, size_t(n), it->second));
}

int64_t bfile_seekoff(Ctx& x, GuestPtr self, int64_t off, uint32_t dir, uint32_t mode) {
    Streams& s = S(x);
    std::lock_guard<std::mutex> lk(s.mu);
    auto it = s.basicFiles.find(self);
    if (it == s.basicFiles.end() || !it->second) return -1;
    if (std::fseek(it->second, long(off), whence(dir)) != 0) return -1;
    return int64_t(std::ftell(it->second));
}

// istream& istream::read(char* s, streamsize n)
uint32_t is_read(Ctx& x, GuestPtr self, GuestPtr buf, int32_t n) {
    if (!isFileStream(x, self, 0)) return hvmIstreamRead(x, self, buf, n);
    const uint32_t obj = objectFor(x, self, 0);
    x.mem.write<uint32_t>(self + 4, 0);   // gcount
    if (state(x, obj) != kGoodbit) { addState(x, obj, kFailbit); return self; }
    std::FILE* f = fileOf(x, obj);
    size_t got = 0;
    if (f && n > 0) got = std::fread(x.mem.host(buf), 1, size_t(n), f);
    x.mem.write<uint32_t>(self + 4, uint32_t(got));
    if (!f || int32_t(got) < n) addState(x, obj, f ? (kEofbit | kFailbit) : kFailbit);
    return self;
}

// ostream& ostream::write(const char* s, streamsize n)
uint32_t os_write(Ctx& x, GuestPtr self, GuestPtr buf, int32_t n) {
    if (!isFileStream(x, self, 1)) return hvmOstreamWrite(x, self, buf, n);
    const uint32_t obj = objectFor(x, self, 1);
    if (state(x, obj) != kGoodbit) return self;
    std::FILE* f = fileOf(x, obj);
    if (!f || (n > 0 && std::fwrite(x.mem.host(buf), 1, size_t(n), f) != size_t(n))) addState(x, obj, kBadbit);
    return self;
}

// 32-bit iOS passes arguments APCS-style (64-bit values aren't moved to an even
// register): this in r0, the offset in r1:r2, the direction in r3.
void seekAt(Ctx& x, int part, uint32_t self, int64_t off, uint32_t dir) {
    if (!isFileStream(x, self, part)) {
        hvmStreamSeek(x, self, off, dir, part == 0 ? kIn : kOut);
        x.cpu.r[0] = self;
        return;
    }
    const uint32_t obj = objectFor(x, self, part);
    std::FILE* f = fileOf(x, obj);
    if (!(state(x, obj) & (kFailbit | kBadbit))) {
        if (!f || std::fseek(f, long(off), whence(dir)) != 0) addState(x, obj, kFailbit);
    }
    x.cpu.r[0] = self;
}
void seekImpl(Ctx& x, int part) {
    seekAt(x, part, x.cpu.r[0], int64_t((uint64_t(x.cpu.r[2]) << 32) | x.cpu.r[1]), x.cpu.r[3]);
}

// streampos istream::tellg(): fpos { long long off; mbstate_t state[128] } returned through r0; this in r1.
void tellgImpl(Ctx& x) {
    const uint32_t ret = x.cpu.r[0], self = x.cpu.r[1];
    int64_t pos = -1;
    if (!isFileStream(x, self, 0)) {
        pos = hvmStreamTell(x, self, kIn);
    } else {
        const uint32_t obj = objectFor(x, self, 0);
        std::FILE* f = fileOf(x, obj);
        if (f && !(state(x, obj) & (kFailbit | kBadbit))) pos = int64_t(std::ftell(f));
    }
    x.mem.write<uint64_t>(ret, uint64_t(pos));
    for (uint32_t i = 8; i < 8 + 128; i += 4) x.mem.write<uint32_t>(ret + i, 0);
    x.cpu.r[0] = ret;
}

// istream& seekg(pos_type): the 136-byte fpos is passed by value.
void seekgPos(Ctx& x) {   // the fpos starts in r1: its offset is r1:r2
    seekAt(x, 0, x.cpu.r[0], int64_t((uint64_t(x.cpu.r[2]) << 32) | x.cpu.r[1]), 0);
}

void noop(Ctx&) {}
void noop1(Ctx&, GuestPtr) {}
void noop2(Ctx&, GuestPtr, GuestPtr) {}

} // namespace

bool hvmIsFilebuf(Ctx& x, uint32_t sb) { return sb && isFileStream(x, sb, 2); }
int32_t hvmFilebufPeek(Ctx& x, uint32_t sb) {
    if (!hvmIsFilebuf(x, sb)) return -1;
    std::FILE* f = fileOf(x, objectFor(x, sb, 2));
    if (!f) return -1;
    const int c = std::fgetc(f);
    if (c != EOF) std::ungetc(c, f);
    return c == EOF ? -1 : (c & 0xFF);
}
int32_t hvmFilebufGet(Ctx& x, uint32_t sb) {
    if (!hvmIsFilebuf(x, sb)) return -1;
    std::FILE* f = fileOf(x, objectFor(x, sb, 2));
    if (!f) return -1;
    const int c = std::fgetc(f);
    return c == EOF ? -1 : (c & 0xFF);
}
int32_t hvmFilebufUnget(Ctx& x, uint32_t sb, int32_t c) {
    if (!hvmIsFilebuf(x, sb)) return -1;
    std::FILE* f = fileOf(x, objectFor(x, sb, 2));
    if (!f) return -1;
    const int r = std::ungetc(c == -1 ? 0 : (c & 0xFF), f);
    return r == EOF ? -1 : (r & 0xFF);
}
int32_t hvmFilebufRead(Ctx& x, uint32_t sb, uint32_t buf, int32_t n) {
    if (!hvmIsFilebuf(x, sb) || n <= 0) return 0;
    std::FILE* f = fileOf(x, objectFor(x, sb, 2));
    return f ? int32_t(std::fread(x.mem.host(buf), 1, size_t(n), f)) : 0;
}
int32_t hvmFilebufWrite(Ctx& x, uint32_t sb, const char* data, uint32_t n) {
    if (!hvmIsFilebuf(x, sb) || !n) return 0;
    std::FILE* f = fileOf(x, objectFor(x, sb, 2));
    return f ? int32_t(std::fwrite(data, 1, n, f)) : 0;
}
int64_t hvmFilebufSeek(Ctx& x, uint32_t sb, int64_t off, uint32_t dir) {
    if (!hvmIsFilebuf(x, sb)) return -1;
    std::FILE* f = fileOf(x, objectFor(x, sb, 2));
    if (!f || std::fseek(f, long(off), whence(dir)) != 0) return -1;
    return int64_t(std::ftell(f));
}

// For the stream code: formatted output to one of our file streams. False if `os` isn't one.
bool hvmFstreamWrite(Ctx& x, uint32_t os, const char* data, uint32_t n) {
    if (!isFileStream(x, os, 1)) return false;
    const uint32_t obj = objectFor(x, os, 1);
    if (state(x, obj) != kGoodbit) return true;
    std::FILE* f = fileOf(x, obj);
    if (!f || (n && std::fwrite(data, 1, n, f) != n)) addState(x, obj, kBadbit);
    return true;
}

void registerLibStdcxx(Process& p) {
    registerLibStdcxxContainers(p);   // std::string, std::map/set trees, std::list
    // std::allocator<T> has no state: construction and destruction do nothing.
    for (const char* t : {"c", "w"})
        for (const char* v : {"C1Ev", "C2Ev", "D1Ev", "D2Ev"}) p.def<&noop1>(std::string("__ZNSaI") + t + "E" + v);
    for (const char* t : {"c", "w"})
        for (const char* v : {"C1ERKS_", "C2ERKS_"}) p.def<&noop2>(std::string("__ZNSaI") + t + "E" + v);
    // iostreams
    p.def<&noop1>("__ZNSt8ios_base4InitC1Ev");
    p.def<&noop1>("__ZNSt8ios_base4InitD1Ev");
    p.def<&noop1>("__ZNSt8ios_base4InitC2Ev");
    p.def<&noop1>("__ZNSt8ios_base4InitD2Ev");

    // Stream vtables: [vbase offset][offset to top][typeinfo][16 virtual slots]; vptr = +12.
    Streams& s = p.service<Streams>();
    const uint32_t badVirtual = p.registerHostFunction("libstdc++ stream virtual call", [](Ctx&) {
        throw std::runtime_error("the app made a virtual call on a C++ file stream (not supported yet)");
    });
    auto vtable = [&](uint32_t vbase, int32_t toTop) {
        const uint32_t a = p.defineDataBlock("hvm_stream_vtable_" + std::to_string(vbase) + "_" + std::to_string(-toTop), 12 + 16 * 4);
        p.mem().write<uint32_t>(a, vbase);
        p.mem().write<uint32_t>(a + 4, uint32_t(toTop));
        for (uint32_t i = 0; i < 16; ++i) p.mem().write<uint32_t>(a + 12 + i * 4, badVirtual);
        return a + 12;
    };
    s.vtIn = vtable(kVbase, 0);
    s.vtOut = s.vtIn;
    s.vtIo = s.vtIn;
    s.vtIoOstream = vtable(kVbase - 8, -8);

    const char* in = "St14basic_ifstreamIcSt11char_traitsIcEE";
    const char* out = "St14basic_ofstreamIcSt11char_traitsIcEE";
    const char* io = "St13basic_fstreamIcSt11char_traitsIcEE";
    const char* fb = "St13basic_filebufIcSt11char_traitsIcEE";
    auto sym = [](const char* cls, const char* rest) { return std::string("__ZN") + cls + rest; };
    for (const char* v : {"C1Ev", "C2Ev"}) {
        p.def<&st_ctor<Kind::In>>(sym(in, v));
        p.def<&st_ctor<Kind::Out>>(sym(out, v));
        p.def<&st_ctor<Kind::InOut>>(sym(io, v));
        p.def<&fb_ctor>(sym(fb, v));
    }
    for (const char* v : {"C1EPKcSt13_Ios_Openmode", "C2EPKcSt13_Ios_Openmode"}) {
        p.def<&st_ctorOpen<Kind::In>>(sym(in, v));
        p.def<&st_ctorOpen<Kind::Out>>(sym(out, v));
        p.def<&st_ctorOpen<Kind::InOut>>(sym(io, v));
    }
    for (const char* cls : {in, out, io}) {
        for (const char* v : {"D0Ev", "D1Ev", "D2Ev"}) p.def<&st_dtor>(sym(cls, v));
        p.def<&st_open>(sym(cls, "4openEPKcSt13_Ios_Openmode"));
        p.def<&st_close>(sym(cls, "5closeEv"));
    }
    p.def<&fb_dtor>(sym(fb, "D1Ev"));
    p.def<&fb_dtor>(sym(fb, "D2Ev"));
    p.def<&fb_dtorDelete>(sym(fb, "D0Ev"));
    p.def<&fb_open>(sym(fb, "4openEPKcSt13_Ios_Openmode"));
    p.def<&fb_close>(sym(fb, "5closeEv"));
    p.def<&fb_isOpen>("__ZNKSt13basic_filebufIcSt11char_traitsIcEE7is_openEv");

    // std::__basic_file<char>
    p.def<&bfile_ctor>("__ZNSt12__basic_fileIcEC1EP15__gthread_mutex");
    p.def<&bfile_ctor>("__ZNSt12__basic_fileIcEC2EP15__gthread_mutex");
    p.def<&bfile_dtor>("__ZNSt12__basic_fileIcED1Ev");
    p.def<&bfile_dtor>("__ZNSt12__basic_fileIcED2Ev");
    p.def<&bfile_isOpen>("__ZNKSt12__basic_fileIcE7is_openEv");
    p.def<&bfile_open>("__ZNSt12__basic_fileIcE4openEPKcSt13_Ios_Openmodei");
    p.def<&bfile_close>("__ZNSt12__basic_fileIcE5closeEv");
    p.def<&bfile_fd>("__ZNKSt12__basic_fileIcE2fdEv");
    p.def<&bfile_file>("__ZNKSt12__basic_fileIcE4fileEv");
    p.def<&bfile_sync>("__ZNSt12__basic_fileIcE4syncEv");
    p.def<&bfile_xsgetn>("__ZNSt12__basic_fileIcE6xsgetnEPci");
    p.def<&bfile_xsputn>("__ZNSt12__basic_fileIcE6xsputnEPKci");
    p.defineFunction("__ZNSt12__basic_fileIcE7seekoffExSt12_Ios_SeekdirSt13_Ios_Openmode", [](Ctx& x) {
        const uint32_t self = x.cpu.r[0];
        const int64_t off = int64_t((uint64_t(x.cpu.r[2]) << 32) | x.cpu.r[1]);
        const uint32_t dir = x.cpu.r[3];
        const uint32_t mode = x.mem.read<uint32_t>(x.cpu.r[CPU::kSP]);
        const int64_t pos = bfile_seekoff(x, self, off, dir, mode);
        x.cpu.r[0] = uint32_t(pos);
        x.cpu.r[1] = uint32_t(pos >> 32);
    });

    p.def<&is_read>("__ZNSi4readEPci");
    p.def<&os_write>("__ZNSo5writeEPKci");
    p.defineFunction("__ZNSi5seekgExSt12_Ios_Seekdir", [](Ctx& x) { seekImpl(x, 0); });
    p.defineFunction("__ZNSo5seekpExSt12_Ios_Seekdir", [](Ctx& x) { seekImpl(x, 1); });
    p.defineFunction("__ZNSi5tellgEv", [](Ctx& x) { tellgImpl(x); });
    p.defineFunction("__ZNSi5seekgESt4fposI11__mbstate_tE", [](Ctx& x) { seekgPos(x); });
    registerLibStdcxxStreams(p);
    (void)noop;
}

} // namespace Hydrogen::Runtime
