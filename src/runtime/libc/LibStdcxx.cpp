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
namespace {

// ------------------------------------------------------------ std::string ---

constexpr uint32_t kRepHeader = 12;   // length, capacity, refcount

struct StringState { explicit StringState(Process&) {} uint32_t emptyRep = 0; };

void disposeRep(Ctx& x, uint32_t data) {
    if (!data) return;
    const uint32_t rep = data - kRepHeader;
    if (rep == x.proc.service<StringState>().emptyRep) return;
    const int32_t rc = int32_t(x.mem.read<uint32_t>(rep + 8));
    x.mem.write<uint32_t>(rep + 8, uint32_t(rc - 1));
    if (rc <= 0) x.proc.heap().free(rep);
}

// std::string::~string() (D1/D2)
void s_dtor(Ctx& x, GuestPtr self) { disposeRep(x, x.mem.read<uint32_t>(self)); }

// std::string& std::string::assign(const char* s, size_type n)
uint32_t s_assign(Ctx& x, GuestPtr self, GuestPtr s, uint32_t n) {
    std::string bytes(n, '\0');
    for (uint32_t i = 0; i < n; ++i) bytes[i] = char(x.mem.read<uint8_t>(s + i));   // s may point into *this
    const uint32_t rep = x.proc.heap().alloc(kRepHeader + n + 1);
    if (!rep) throw std::runtime_error("std::string::assign: out of guest memory");
    x.mem.write<uint32_t>(rep, n);
    x.mem.write<uint32_t>(rep + 4, n);
    x.mem.write<uint32_t>(rep + 8, 0);
    for (uint32_t i = 0; i < n; ++i) x.mem.write<uint8_t>(rep + kRepHeader + i, uint8_t(bytes[i]));
    x.mem.write<uint8_t>(rep + kRepHeader + n, 0);
    disposeRep(x, x.mem.read<uint32_t>(self));
    x.mem.write<uint32_t>(self, rep + kRepHeader);
    return self;
}

// ------------------------------------------------------------- std::list ---
// _List_node_base { _List_node_base* next; _List_node_base* prev; }

void l_hook(Ctx& x, GuestPtr self, GuestPtr pos) {
    const uint32_t prev = x.mem.read<uint32_t>(pos + 4);
    x.mem.write<uint32_t>(self, pos);
    x.mem.write<uint32_t>(self + 4, prev);
    x.mem.write<uint32_t>(prev, self);
    x.mem.write<uint32_t>(pos + 4, self);
}
void l_unhook(Ctx& x, GuestPtr self) {
    const uint32_t next = x.mem.read<uint32_t>(self), prev = x.mem.read<uint32_t>(self + 4);
    x.mem.write<uint32_t>(prev, next);
    x.mem.write<uint32_t>(next + 4, prev);
}

// --------------------------------------------------------------- streams ---

enum class Kind { In, Out, InOut };
constexpr uint32_t kVbase = 0x40;          // where our ios_base lives inside the object
constexpr uint32_t kStateOff = 20;         // ios_base::_M_streambuf_state
constexpr uint32_t kGoodbit = 0, kBadbit = 1, kEofbit = 2, kFailbit = 4;
constexpr uint32_t kApp = 1, kAte = 2, kBinary = 4, kIn = 8, kOut = 16, kTrunc = 32;

struct Stream { Kind kind; std::FILE* f = nullptr; };

struct Streams {
    explicit Streams(Process&) {}
    std::mutex mu;
    std::unordered_map<uint32_t, Stream> objects;   // keyed by the complete object's address
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
    if (part == 0 && s.objects.count(p)) return p;
    if (part == 1) {
        if (has(p, [](Kind k) { return k == Kind::Out; })) return p;
        if (has(p - 8, [](Kind k) { return k == Kind::InOut; })) return p - 8;
    }
    if (part == 2) {
        for (Kind k : {Kind::In, Kind::Out, Kind::InOut})
            if (has(p - filebufOffset(k), [k](Kind kk) { return kk == k; })) return p - filebufOffset(k);
    }
    throw std::runtime_error("libstdc++: stream call on an object HydrogenVM didn't construct");
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

void open(Ctx& x, uint32_t obj, GuestPtr path, uint32_t mode) {
    Kind k;
    {
        std::lock_guard<std::mutex> lk(S(x).mu);
        k = S(x).objects.at(obj).kind;
    }
    if (k == Kind::In) mode |= kIn;
    if (k == Kind::Out) mode |= kOut;
    if (fileOf(x, obj)) { addState(x, obj, kFailbit); return; }
    const uint32_t m = mode & (kIn | kOut | kTrunc | kApp);
    const char* fm = nullptr;
    if (m == kIn) fm = "rb";
    else if (m == kOut || m == (kOut | kTrunc)) fm = "wb";
    else if (m == (kOut | kApp) || m == kApp) fm = "ab";
    else if (m == (kIn | kOut)) fm = "r+b";
    else if (m == (kIn | kOut | kTrunc)) fm = "w+b";
    else if (m == (kIn | kOut | kApp) || m == (kIn | kApp)) fm = "a+b";
    std::FILE* f = fm ? std::fopen(x.proc.readCString(path).c_str(), fm) : nullptr;
    if (f && (mode & kAte)) std::fseek(f, 0, SEEK_END);
    if (!f) { addState(x, obj, kFailbit); return; }
    {
        std::lock_guard<std::mutex> lk(S(x).mu);
        S(x).objects[obj].f = f;
    }
    setState(x, obj, kGoodbit);
}

void close(Ctx& x, uint32_t obj) {
    std::FILE* f = nullptr;
    {
        std::lock_guard<std::mutex> lk(S(x).mu);
        auto it = S(x).objects.find(obj);
        if (it != S(x).objects.end()) { f = it->second.f; it->second.f = nullptr; }
    }
    if (!f || std::fclose(f) != 0) addState(x, obj, kFailbit);
}

// Constructors / destructors / open / close for the three stream types.
template <Kind K> void st_ctor(Ctx& x, GuestPtr self) { construct(x, self, K); }
template <Kind K> void st_ctorOpen(Ctx& x, GuestPtr self, GuestPtr path, uint32_t mode) { construct(x, self, K); open(x, self, path, mode); }
void st_dtor(Ctx& x, GuestPtr self) { destroy(x, self); }
void st_open(Ctx& x, GuestPtr self, GuestPtr path, uint32_t mode) { open(x, objectFor(x, self, 0), path, mode); }
void st_close(Ctx& x, GuestPtr self) { close(x, objectFor(x, self, 0)); }
bool fb_isOpen(Ctx& x, GuestPtr filebuf) { return fileOf(x, objectFor(x, filebuf, 2)) != nullptr; }

// istream& istream::read(char* s, streamsize n)
uint32_t is_read(Ctx& x, GuestPtr self, GuestPtr buf, int32_t n) {
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
    const uint32_t obj = objectFor(x, self, 1);
    if (state(x, obj) != kGoodbit) return self;
    std::FILE* f = fileOf(x, obj);
    if (!f || (n > 0 && std::fwrite(x.mem.host(buf), 1, size_t(n), f) != size_t(n))) addState(x, obj, kBadbit);
    return self;
}

int whence(uint32_t dir) { return dir == 0 ? SEEK_SET : dir == 1 ? SEEK_CUR : SEEK_END; }

// istream& istream::seekg(off_type, ios_base::seekdir) and ostream::seekp:
// this in r0, the 64-bit offset in r2:r3, the direction on the stack.
void seekImpl(Ctx& x, int part) {
    const uint32_t self = x.cpu.r[0];
    const int64_t off = int64_t((uint64_t(x.cpu.r[3]) << 32) | x.cpu.r[2]);
    const uint32_t dir = x.mem.read<uint32_t>(x.cpu.r[CPU::kSP]);
    const uint32_t obj = objectFor(x, self, part);
    std::FILE* f = fileOf(x, obj);
    if (!(state(x, obj) & (kFailbit | kBadbit))) {
        if (!f || std::fseek(f, long(off), whence(dir)) != 0) addState(x, obj, kFailbit);
    }
    x.cpu.r[0] = self;
}

// streampos istream::tellg(): fpos { long long off; mbstate_t state[128] } returned through r0; this in r1.
void tellgImpl(Ctx& x) {
    const uint32_t ret = x.cpu.r[0], self = x.cpu.r[1];
    const uint32_t obj = objectFor(x, self, 0);
    std::FILE* f = fileOf(x, obj);
    int64_t pos = -1;
    if (f && !(state(x, obj) & (kFailbit | kBadbit))) pos = int64_t(std::ftell(f));
    x.mem.write<uint64_t>(ret, uint64_t(pos));
    for (uint32_t i = 8; i < 8 + 128; i += 4) x.mem.write<uint32_t>(ret + i, 0);
    x.cpu.r[0] = ret;
}

void noop(Ctx&) {}
void noop1(Ctx&, GuestPtr) {}

} // namespace

void registerLibStdcxx(Process& p) {
    // std::string
    const uint32_t empty = p.defineDataBlock("__ZNSs4_Rep20_S_empty_rep_storageE", 16);
    p.service<StringState>().emptyRep = empty;
    p.def<&s_dtor>("__ZNSsD1Ev");
    p.def<&s_dtor>("__ZNSsD2Ev");
    p.def<&s_assign>("__ZNSs6assignEPKcm");
    p.def<&s_assign>("__ZNSs6assignEPKcj");
    // std::list
    p.def<&l_hook>("__ZNSt15_List_node_base4hookEPS_");
    p.def<&l_unhook>("__ZNSt15_List_node_base6unhookEv");
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
    auto sym = [](const char* cls, const char* rest) { return std::string("__ZN") + cls + rest; };
    for (const char* v : {"C1Ev", "C2Ev"}) {
        p.def<&st_ctor<Kind::In>>(sym(in, v));
        p.def<&st_ctor<Kind::Out>>(sym(out, v));
        p.def<&st_ctor<Kind::InOut>>(sym(io, v));
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
    p.def<&fb_isOpen>("__ZNKSt13basic_filebufIcSt11char_traitsIcEE7is_openEv");
    p.def<&is_read>("__ZNSi4readEPci");
    p.def<&os_write>("__ZNSo5writeEPKci");
    p.defineFunction("__ZNSi5seekgExSt12_Ios_Seekdir", [](Ctx& x) { seekImpl(x, 0); });
    p.defineFunction("__ZNSo5seekpExSt12_Ios_Seekdir", [](Ctx& x) { seekImpl(x, 1); });
    p.defineFunction("__ZNSi5tellgEv", [](Ctx& x) { tellgImpl(x); });
    (void)noop;
}

} // namespace Hydrogen::Runtime
