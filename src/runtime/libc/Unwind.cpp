// HydrogenVM — C++ and Objective-C exceptions for 32-bit iOS.
//
// iOS armv6/armv7 code uses setjmp/longjmp ("SJLJ") exception handling, for
// both C++ (__gxx_personality_sj0) and Objective-C 2 (__objc_personality_v0).
// Every function with a try/catch or a cleanup registers a function context:
//
//   +0  prev            previous context (a per-thread linked list)
//   +4  call_site       index into the function's call-site table, set before
//                       each call that may throw; -1 = no action, 0 = terminate
//   +8  data[0..3]      landing pad parameters: [0] exception, [1] selector
//   +24 personality
//   +28 lsda            the function's exception table (GCC LSDA format)
//   +32 jbuf            __builtin_setjmp buffer: [0] fp (r7), [1] dispatch
//                       address, [2] sp
//
// A throw walks the list twice, as the real unwinder does. Phase 1 finds the
// first frame whose action table catches the exception. Phase 2 lands in every
// frame with a cleanup on the way (each cleanup ends in _Unwind_SjLj_Resume),
// then in the handler, with data[0] = the exception and data[1] = the
// matching type filter. Landing = __builtin_longjmp: restore r7 and sp and
// jump to the dispatch address; the dispatch block reloads everything else.
//
// The personality routines aren't called: the LSDA format is the same for
// both languages, and matching is done here (C++ type_info names and base
// classes; Objective-C classes through OBJC_EHTYPE records).
#include "core/cpu/Engine.hpp"
#include "runtime/Process.hpp"
#include "runtime/objc/ObjC.hpp"
#include "runtime/libc/Unwind.hpp"
#include <cxxabi.h>

namespace Hydrogen::Runtime {
namespace {

constexpr uint32_t kFcPrev = 0, kFcCallSite = 4, kFcData0 = 8, kFcData1 = 12, kFcLsda = 28, kFcJbuf = 32;
constexpr uint32_t kHeader = 128;   // bytes reserved before a C++ exception object
constexpr uint32_t kUeOffset = 32;  // the _Unwind_Exception sits this far before the object

enum class Kind { Cxx, ObjC };

struct Exception {
    Kind kind = Kind::Cxx;
    uint32_t typeinfo = 0;   // C++: thrown std::type_info
    uint32_t object = 0;     // C++: the thrown object; ObjC: the id
    uint32_t adjusted = 0;   // what __cxa_begin_catch returns for the chosen handler
    bool searched = false;
    uint32_t handlerFc = 0, handlerLp = 0;
    int32_t handlerSelector = 0;
    uint32_t lastCleanupFc = 0;
};

// Type-info vtables we export, so a type_info's kind can be told from its vtable.
struct Eh {
    explicit Eh(Process&) {}
    std::mutex mu;
    std::unordered_map<uint32_t, Exception> live;   // keyed by _Unwind_Exception address
    uint32_t vtClass = 0, vtSi = 0, vtVmi = 0, vtPointer = 0, vtPbase = 0, vtFundamental = 0, ehtypeVtable = 0;
};
Eh& eh(Ctx& x) { return x.proc.service<Eh>(); }

// ---------------------------------------------------------------- LSDA ---

struct Reader {
    Memory::GuestArena& m;
    uint32_t p;
    uint8_t u8() { return m.read<uint8_t>(p++); }
    uint32_t uleb() {
        uint32_t v = 0; int shift = 0; uint8_t b;
        do { b = u8(); if (shift < 32) v |= uint32_t(b & 0x7F) << shift; shift += 7; } while (b & 0x80);
        return v;
    }
    int32_t sleb() {
        uint32_t v = 0; int shift = 0; uint8_t b;
        do { b = u8(); if (shift < 32) v |= uint32_t(b & 0x7F) << shift; shift += 7; } while (b & 0x80);
        if (shift < 32 && (b & 0x40)) v |= ~0u << shift;
        return int32_t(v);
    }
    uint32_t encoded(uint8_t enc) {
        if (enc == 0xFF) return 0;
        const uint32_t start = p;
        uint32_t v;
        switch (enc & 0x0F) {
            case 0x00: case 0x03: case 0x0B: v = m.read<uint32_t>(p); p += 4; break;
            case 0x01: v = uleb(); break;
            case 0x09: v = uint32_t(sleb()); break;
            case 0x02: v = m.read<uint16_t>(p); p += 2; break;
            case 0x0A: v = uint32_t(int32_t(int16_t(m.read<uint16_t>(p)))); p += 2; break;
            case 0x04: case 0x0C: v = uint32_t(m.read<uint64_t>(p)); p += 8; break;
            default: throw std::runtime_error("exception table: unsupported pointer encoding");
        }
        if (v && (enc & 0x70) == 0x10) v += start;      // pcrel
        if (v && (enc & 0x80)) v = m.read<uint32_t>(v); // indirect
        return v;
    }
};
uint32_t encodedSize(uint8_t enc) {
    switch (enc & 0x0F) { case 0x02: case 0x0A: return 2; case 0x04: case 0x0C: return 8; default: return 4; }
}

struct Frame {
    bool action = false;       // the current call site has a landing pad
    bool terminate = false;    // call_site 0: std::terminate
    uint32_t landingPad = 0;   // value for call_site when landing
    uint32_t actionRecord = 0; // 0 = cleanup only
    uint32_t classInfo = 0;
    uint8_t ttypeEnc = 0xFF;
};

Frame analyze(Ctx& x, uint32_t fc) {
    Frame f;
    const int32_t cs = int32_t(x.mem.read<uint32_t>(fc + kFcCallSite));
    const uint32_t lsda = x.mem.read<uint32_t>(fc + kFcLsda);
    if (cs < 0 || !lsda) return f;
    if (cs == 0) { f.terminate = true; return f; }
    Reader r{x.mem, lsda};
    const uint8_t lpEnc = r.u8();
    if (lpEnc != 0xFF) r.encoded(lpEnc);
    f.ttypeEnc = r.u8();
    if (f.ttypeEnc != 0xFF) { const uint32_t off = r.uleb(); f.classInfo = r.p + off; }
    r.u8();                                   // call-site encoding: uleb128 for SJLJ
    const uint32_t len = r.uleb();
    const uint32_t actionTable = r.p + len;
    uint32_t lp = 0, act = 0;
    for (int32_t i = 0; i < cs; ++i) {
        if (r.p >= actionTable) return f;     // index past the table: no action
        lp = r.uleb();
        act = r.uleb();
    }
    f.action = true;
    f.landingPad = lp;
    f.actionRecord = act ? actionTable + act - 1 : 0;
    return f;
}

// ------------------------------------------------------------ matching ---

std::string typeName(Ctx& x, uint32_t ti) {
    std::string n = x.proc.readCString(x.mem.read<uint32_t>(ti + 4), 512);
    if (!n.empty() && n[0] == '*') n.erase(0, 1);   // GCC marks local types with '*'
    return n;
}

// Is class type `t` (or one of its bases) the class `c`? `off` gets the base's offset.
bool classMatch(Ctx& x, uint32_t t, uint32_t c, int32_t& off, int depth = 0) {
    if (!t || !c || depth > 32) return false;
    if (t == c || typeName(x, t) == typeName(x, c)) { off = 0; return true; }
    Eh& e = eh(x);
    const uint32_t vt = x.mem.read<uint32_t>(t);
    if (vt == e.vtSi + 8) return classMatch(x, x.mem.read<uint32_t>(t + 8), c, off, depth + 1);
    if (vt == e.vtVmi + 8) {
        const uint32_t n = x.mem.read<uint32_t>(t + 12);
        for (uint32_t i = 0; i < n && i < 64; ++i) {
            const uint32_t base = x.mem.read<uint32_t>(t + 16 + i * 8);
            const int32_t flags = int32_t(x.mem.read<uint32_t>(t + 20 + i * 8));
            int32_t o = 0;
            if (classMatch(x, base, c, o, depth + 1)) {
                off = (flags & 1) ? o : (flags >> 8) + o;   // virtual bases: no static offset
                return true;
            }
        }
    }
    return false;
}

bool isPointerType(Ctx& x, uint32_t ti) {
    const uint32_t vt = x.mem.read<uint32_t>(ti);
    return vt == eh(x).vtPointer + 8 || vt == eh(x).vtPbase + 8;
}

// Does catch clause type `c` (0 = catch-all) take the exception? Sets `adjusted`.
bool catches(Ctx& x, const Exception& ex, uint32_t c, uint32_t& adjusted) {
    if (!c) { adjusted = ex.kind == Kind::Cxx ? ex.object : ex.object; return true; }
    Eh& e = eh(x);
    const bool ehtype = x.mem.read<uint32_t>(c) == e.ehtypeVtable + 8;
    if (ex.kind == Kind::ObjC) {
        if (!ehtype) return false;
        const std::string name = x.proc.readCString(x.mem.read<uint32_t>(c + 4), 256);
        uint32_t cls = x.mem.read<uint32_t>(c + 8);
        if (name == "id" && !cls) { adjusted = ex.object; return true; }
        auto& r = ObjC::rt(x);
        if (!cls) cls = r.getClass(name);
        if (cls && ex.object && r.isKindOf(ex.object, cls)) { adjusted = ex.object; return true; }
        return false;
    }
    if (ehtype || !ex.typeinfo) return false;
    if (isPointerType(x, c) && isPointerType(x, ex.typeinfo)) {
        const uint32_t cp = x.mem.read<uint32_t>(c + 12), tp = x.mem.read<uint32_t>(ex.typeinfo + 12);
        const uint32_t value = x.mem.read<uint32_t>(ex.object);
        if (cp && typeName(x, cp) == "v") { adjusted = value; return true; }   // catch (void*)
        int32_t off = 0;
        if (classMatch(x, tp, cp, off)) { adjusted = value ? value + uint32_t(off) : 0; return true; }
        return false;
    }
    int32_t off = 0;
    if (classMatch(x, ex.typeinfo, c, off)) { adjusted = ex.object + uint32_t(off); return true; }
    return false;
}

enum class Action { None, Cleanup, Handler };

Action actions(Ctx& x, const Frame& f, const Exception& ex, int32_t& selector, uint32_t& adjusted) {
    if (!f.action) return Action::None;
    if (!f.actionRecord) return Action::Cleanup;
    bool cleanup = false;
    Reader r{x.mem, f.actionRecord};
    for (int guard = 0; guard < 256; ++guard) {
        const int32_t filter = r.sleb();
        const uint32_t dispAt = r.p;
        const int32_t disp = r.sleb();
        if (filter > 0) {
            Reader t{x.mem, f.classInfo - uint32_t(filter) * encodedSize(f.ttypeEnc)};
            const uint32_t type = t.encoded(f.ttypeEnc);
            if (catches(x, ex, type, adjusted)) { selector = filter; return Action::Handler; }
        } else if (filter == 0) {
            cleanup = true;
        }
        // filter < 0: an exception specification; treated as not matching.
        if (!disp) break;
        r.p = dispAt + uint32_t(disp);
    }
    return cleanup ? Action::Cleanup : Action::None;
}

// ------------------------------------------------------------ throwing ---

std::string describe(Ctx& x, const Exception& ex) {
    if (ex.kind == Kind::ObjC) {
        auto& r = ObjC::rt(x);
        return "Uncaught Objective-C exception: " + (r.describeException ? r.describeException(ex.object) : r.objectClassName(ex.object));
    }
    std::string type = ex.typeinfo ? typeName(x, ex.typeinfo) : "?";
    int st = 0;
    if (char* d = abi::__cxa_demangle(type.c_str(), nullptr, nullptr, &st)) { if (!st) type = d; std::free(d); }
    return "Uncaught C++ exception of type " + type;
}

void land(Ctx& x, GuestThread& t, uint32_t fc, uint32_t lp, int32_t selector, uint32_t ue) {
    x.mem.write<uint32_t>(fc + kFcCallSite, lp);
    x.mem.write<uint32_t>(fc + kFcData0, ue);
    x.mem.write<uint32_t>(fc + kFcData1, uint32_t(selector));
    t.sjljTop = fc;
    uint32_t pc = x.mem.read<uint32_t>(fc + kFcJbuf + 4);
    auto th = t.sjljThumb.find(fc);
    if (!(pc & 1) && th != t.sjljThumb.end() && th->second) pc |= 1;
    x.cpu.r[7] = x.mem.read<uint32_t>(fc + kFcJbuf);
    x.cpu.r[CPU::kSP] = x.mem.read<uint32_t>(fc + kFcJbuf + 8);
    CPU::Engine::hleJump(x.cpu, pc);
}

// Phase 1: the first frame (inside the current host->guest call) that catches `ex`.
bool search(Ctx& x, Exception& ex) {
    GuestThread& t = x.proc.currentThread();
    const uint32_t boundary = x.cpu.call_boundary_sp;
    for (uint32_t fc = t.sjljTop, n = 0; fc && fc < boundary && n < 100000; fc = x.mem.read<uint32_t>(fc + kFcPrev), ++n) {
        const Frame f = analyze(x, fc);
        if (f.terminate) return false;
        int32_t sel = 0; uint32_t adj = 0;
        if (actions(x, f, ex, sel, adj) == Action::Handler) {
            ex.handlerFc = fc; ex.handlerLp = f.landingPad; ex.handlerSelector = sel; ex.adjusted = adj;
            ex.searched = true;
            return true;
        }
    }
    return false;
}

// Throws (phase 1 if needed, then phase 2 from `from`). Lands in guest code, or
// stops the app if nothing catches the exception.
void unwind(Ctx& x, uint32_t ue, uint32_t from) {
    Eh& e = eh(x);
    GuestThread& t = x.proc.currentThread();
    Exception ex;
    {
        std::lock_guard<std::mutex> lk(e.mu);
        auto it = e.live.find(ue);
        if (it == e.live.end()) throw std::runtime_error("the app resumed an unknown exception");
        ex = it->second;
    }
    const uint32_t boundary = x.cpu.call_boundary_sp;
    if (!ex.searched && !search(x, ex)) throw std::runtime_error(describe(x, ex));
    for (uint32_t fc = from, n = 0; fc && fc < boundary && n < 100000; fc = x.mem.read<uint32_t>(fc + kFcPrev), ++n) {
        if (fc == ex.handlerFc) {
            { std::lock_guard<std::mutex> lk(e.mu); e.live[ue] = ex; }
            land(x, t, fc, ex.handlerLp, ex.handlerSelector, ue);
            return;
        }
        const Frame f = analyze(x, fc);
        int32_t sel = 0; uint32_t adj = 0;
        if (f.action && actions(x, f, ex, sel, adj) != Action::None) {
            ex.lastCleanupFc = fc;
            { std::lock_guard<std::mutex> lk(e.mu); e.live[ue] = ex; }
            land(x, t, fc, f.landingPad, 0, ue);
            return;
        }
    }
    throw std::runtime_error(describe(x, ex) + " (its handler is no longer on the stack)");
}

void raise(Ctx& x, uint32_t ue, Exception ex) {
    {
        std::lock_guard<std::mutex> lk(eh(x).mu);
        eh(x).live[ue] = ex;
    }
    unwind(x, ue, x.proc.currentThread().sjljTop);
}

// ------------------------------------------------------------ SJLJ API ---

void u_register(Ctx& x, GuestPtr fc) {
    GuestThread& t = x.proc.currentThread();
    x.mem.write<uint32_t>(fc + kFcPrev, t.sjljTop);
    t.sjljTop = fc;
    t.sjljThumb[fc] = (x.cpu.r[CPU::kLR] & 1) != 0;
}
void u_unregister(Ctx& x, GuestPtr fc) {
    GuestThread& t = x.proc.currentThread();
    t.sjljTop = x.mem.read<uint32_t>(fc + kFcPrev);
    t.sjljThumb.erase(fc);
}
void u_resume(Ctx& x, GuestPtr ue) {
    Exception ex;
    {
        std::lock_guard<std::mutex> lk(eh(x).mu);
        auto it = eh(x).live.find(ue);
        if (it == eh(x).live.end()) throw std::runtime_error("_Unwind_SjLj_Resume: unknown exception");
        ex = it->second;
    }
    // The frame whose cleanup just ran is being unwound: continue past it.
    GuestThread& t = x.proc.currentThread();
    uint32_t from = t.sjljTop;
    if (ex.lastCleanupFc) from = x.mem.read<uint32_t>(ex.lastCleanupFc + kFcPrev);
    unwind(x, ue, from);
}
int32_t u_raise(Ctx& x, GuestPtr ue) {
    {
        std::lock_guard<std::mutex> lk(eh(x).mu);
        if (!eh(x).live.count(ue)) return 5;   // _URC_END_OF_STACK: not one of ours
    }
    unwind(x, ue, x.proc.currentThread().sjljTop);
    return 0;
}
void u_delete(Ctx& x, GuestPtr ue) { std::lock_guard<std::mutex> lk(eh(x).mu); eh(x).live.erase(ue); }

// ----------------------------------------------------------------- C++ ---

uint32_t x_allocate_exception(Ctx& x, uint32_t size) {
    const uint32_t a = x.proc.heap().allocZeroed(size + kHeader);
    if (!a) throw std::runtime_error("__cxa_allocate_exception: out of guest memory");
    return a + kHeader;
}
void x_free_exception(Ctx& x, GuestPtr obj) { if (obj) x.proc.heap().free(obj - kHeader); }
void x_throw(Ctx& x, GuestPtr obj, GuestPtr typeinfo, GuestPtr) {
    const uint32_t ue = obj - kUeOffset;
    x.mem.write<uint32_t>(ue, 0x432B2B00);       // "GNUCC++\0", as libstdc++ does
    x.mem.write<uint32_t>(ue + 4, 0x474E5543);
    Exception ex;
    ex.kind = Kind::Cxx;
    ex.typeinfo = typeinfo;
    ex.object = obj;
    raise(x, ue, ex);
}
uint32_t x_begin_catch(Ctx& x, GuestPtr ue) {
    GuestThread& t = x.proc.currentThread();
    std::lock_guard<std::mutex> lk(eh(x).mu);
    auto it = eh(x).live.find(ue);
    if (it == eh(x).live.end()) return ue ? ue + kUeOffset : 0;
    it->second.searched = false;                 // a rethrow searches again
    it->second.lastCleanupFc = 0;
    t.caughtExceptions.push_back(ue);
    return it->second.adjusted;
}
uint32_t x_get_exception_ptr(Ctx& x, GuestPtr ue) {
    std::lock_guard<std::mutex> lk(eh(x).mu);
    auto it = eh(x).live.find(ue);
    return it == eh(x).live.end() ? ue + kUeOffset : it->second.adjusted;
}
void x_end_catch(Ctx& x) {
    GuestThread& t = x.proc.currentThread();
    if (!t.caughtExceptions.empty()) t.caughtExceptions.pop_back();
}
void x_rethrow(Ctx& x) {
    GuestThread& t = x.proc.currentThread();
    if (t.caughtExceptions.empty()) throw std::runtime_error("the app rethrew with no exception being handled (std::terminate)");
    const uint32_t ue = t.caughtExceptions.back();
    unwind(x, ue, t.sjljTop);
}
uint32_t x_current_type(Ctx& x) {
    GuestThread& t = x.proc.currentThread();
    if (t.caughtExceptions.empty()) return 0;
    std::lock_guard<std::mutex> lk(eh(x).mu);
    auto it = eh(x).live.find(t.caughtExceptions.back());
    return it == eh(x).live.end() ? 0 : it->second.typeinfo;
}
int32_t x_uncaught(Ctx&) { return 0; }

// --------------------------------------------------------- Objective-C ---

void o_throw(Ctx& x, uint32_t obj) {
    const uint32_t ue = x.proc.heap().allocZeroed(64);
    x.mem.write<uint32_t>(ue, 0x434A424F);       // "OBJC"
    x.mem.write<uint32_t>(ue + 4, 0x00000000);
    Exception ex;
    ex.kind = Kind::ObjC;
    ex.object = obj;
    raise(x, ue, ex);
}
void o_rethrow(Ctx& x) { x_rethrow(x); }
uint32_t o_begin_catch(Ctx& x, GuestPtr ue) {
    GuestThread& t = x.proc.currentThread();
    std::lock_guard<std::mutex> lk(eh(x).mu);
    auto it = eh(x).live.find(ue);
    if (it == eh(x).live.end()) return ue;
    it->second.searched = false;
    it->second.lastCleanupFc = 0;
    t.caughtExceptions.push_back(ue);
    return it->second.object;
}

void e_void(Ctx&) {}

} // namespace

uint32_t prepareGuestObjCException(Ctx& x, uint32_t obj) {
    Exception ex;
    ex.kind = Kind::ObjC;
    ex.object = obj;
    if (!search(x, ex)) return 0;
    const uint32_t ue = x.proc.heap().allocZeroed(64);
    x.mem.write<uint32_t>(ue, 0x434A424F);
    std::lock_guard<std::mutex> lk(eh(x).mu);
    eh(x).live[ue] = ex;
    return ue;
}

void landGuestException(Ctx& x, uint32_t ue) { unwind(x, ue, x.proc.currentThread().sjljTop); }

void registerUnwinder(Process& p) {
    Eh& e = p.service<Eh>();
    // type_info vtables: 16 words each, so a type_info's vtable pointer (vtable + 8) identifies its kind.
    e.vtClass = p.defineDataBlock("__ZTVN10__cxxabiv117__class_type_infoE", 64);
    e.vtSi = p.defineDataBlock("__ZTVN10__cxxabiv120__si_class_type_infoE", 64);
    e.vtVmi = p.defineDataBlock("__ZTVN10__cxxabiv121__vmi_class_type_infoE", 64);
    e.vtPointer = p.defineDataBlock("__ZTVN10__cxxabiv119__pointer_type_infoE", 64);
    e.vtPbase = p.defineDataBlock("__ZTVN10__cxxabiv117__pbase_type_infoE", 64);
    e.vtFundamental = p.defineDataBlock("__ZTVN10__cxxabiv123__fundamental_type_infoE", 64);
    for (const char* n : {"__ZTVN10__cxxabiv116__enum_type_infoE", "__ZTVN10__cxxabiv120__function_type_infoE",
                          "__ZTVN10__cxxabiv117__array_type_infoE", "__ZTVN10__cxxabiv129__pointer_to_member_type_infoE"})
        p.defineDataBlock(n, 64);
    e.ehtypeVtable = p.defineDataBlock("_objc_ehtype_vtable", 64);

    auto& m = p.mem();
    auto typeinfo = [&](const std::string& sym, const std::string& name, uint32_t vtable, uint32_t base = 0) {
        const uint32_t a = p.defineDataBlock(sym, 16);
        m.write<uint32_t>(a, vtable + 8);
        m.write<uint32_t>(a + 4, p.allocCString(name));
        if (base) m.write<uint32_t>(a + 8, base);
        return a;
    };
    // Fundamental types (throw 42; throw "message";).
    uint32_t tc = 0;
    for (auto [sym, name] : std::initializer_list<std::pair<const char*, const char*>>{
             {"__ZTIi", "i"}, {"__ZTIj", "j"}, {"__ZTIl", "l"}, {"__ZTIm", "m"}, {"__ZTIx", "x"}, {"__ZTIy", "y"},
             {"__ZTIs", "s"}, {"__ZTIt", "t"}, {"__ZTIb", "b"}, {"__ZTIf", "f"}, {"__ZTId", "d"}, {"__ZTIv", "v"}, {"__ZTIc", "c"}}) {
        const uint32_t a = typeinfo(sym, name, e.vtFundamental);
        if (std::string(name) == "c") tc = a;
    }
    const uint32_t tKc = typeinfo("__ZTIKc", "Kc", e.vtFundamental);
    for (auto [sym, name, pointee] : std::initializer_list<std::tuple<const char*, const char*, uint32_t>>{
             {"__ZTIPKc", "PKc", tKc}, {"__ZTIPc", "Pc", tc}}) {
        const uint32_t a = typeinfo(sym, name, e.vtPointer);
        m.write<uint32_t>(a + 8, 0);
        m.write<uint32_t>(a + 12, pointee);
    }
    // The standard exception hierarchy, so catch (std::exception&) works.
    const uint32_t exc = typeinfo("__ZTISt9exception", "St9exception", e.vtClass);
    const uint32_t logic = typeinfo("__ZTISt11logic_error", "St11logic_error", e.vtSi, exc);
    const uint32_t runtime = typeinfo("__ZTISt13runtime_error", "St13runtime_error", e.vtSi, exc);
    typeinfo("__ZTISt9bad_alloc", "St9bad_alloc", e.vtSi, exc);
    typeinfo("__ZTISt8bad_cast", "St8bad_cast", e.vtSi, exc);
    typeinfo("__ZTISt13bad_exception", "St13bad_exception", e.vtSi, exc);
    for (auto [sym, name] : std::initializer_list<std::pair<const char*, const char*>>{
             {"__ZTISt12out_of_range", "St12out_of_range"}, {"__ZTISt12length_error", "St12length_error"},
             {"__ZTISt16invalid_argument", "St16invalid_argument"}, {"__ZTISt12domain_error", "St12domain_error"}})
        typeinfo(sym, name, e.vtSi, logic);
    for (auto [sym, name] : std::initializer_list<std::pair<const char*, const char*>>{
             {"__ZTISt11range_error", "St11range_error"}, {"__ZTISt14overflow_error", "St14overflow_error"},
             {"__ZTISt15underflow_error", "St15underflow_error"}})
        typeinfo(sym, name, e.vtSi, runtime);
    // Objective-C @catch types: {vtable, class name, class}. The class is looked up by name when 0.
    for (auto [sym, name] : std::initializer_list<std::pair<const char*, const char*>>{
             {"_OBJC_EHTYPE_id", "id"}, {"_OBJC_EHTYPE_$_NSException", "NSException"}}) {
        const uint32_t a = p.defineDataBlock(sym, 12);
        m.write<uint32_t>(a, e.ehtypeVtable + 8);
        m.write<uint32_t>(a + 4, p.allocCString(name));
    }

    p.def<&u_register>("__Unwind_SjLj_Register");
    p.def<&u_unregister>("__Unwind_SjLj_Unregister");
    p.def<&u_resume>("__Unwind_SjLj_Resume");
    p.def<&u_resume>("__Unwind_SjLj_Resume_or_Rethrow");
    p.def<&u_raise>("__Unwind_SjLj_RaiseException");
    p.def<&u_delete>("__Unwind_DeleteException");
    p.def<&e_void>("___gxx_personality_sj0");
    p.def<&e_void>("___objc_personality_v0");

    p.def<&x_allocate_exception>("___cxa_allocate_exception");
    p.def<&x_free_exception>("___cxa_free_exception");
    p.def<&x_throw>("___cxa_throw");
    p.def<&x_rethrow>("___cxa_rethrow");
    p.def<&x_begin_catch>("___cxa_begin_catch");
    p.def<&x_end_catch>("___cxa_end_catch");
    p.def<&x_get_exception_ptr>("___cxa_get_exception_ptr");
    p.def<&x_current_type>("___cxa_current_exception_type");
    p.def<&x_uncaught>("__ZSt18uncaught_exceptionv");

    p.def<&o_throw>("_objc_exception_throw");
    p.def<&o_rethrow>("_objc_exception_rethrow");
    p.def<&o_begin_catch>("_objc_begin_catch");
    p.def<&x_end_catch>("_objc_end_catch");
}

} // namespace Hydrogen::Runtime
