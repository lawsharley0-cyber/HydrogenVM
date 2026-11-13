// HydrogenVM test runner.
//
//   hvm_tests asm <elf> <source.S>...   run hand-written unit tests
//   hvm_tests c <elf> <expected.txt>     run compiled-C tests, compare to host results
//
// Asm test format (in the .S source):
//   @ TEST <symbol>
//   @ INIT r1=5 r2=0x10 nzcv=0x2        (optional; nzcv is the 4-bit NZCV nibble)
//   @ EXPECT r0=1 r1=0xFFFFFFFF nzcv=0x6 q=1 ge=0x3
// The function is called with LR pointing at a "halt" HLE stub, SP at a fresh
// stack, and every other register zero unless INITed.
#include "core/cpu/Engine.hpp"
#include "loader/ElfLoader.hpp"
#include "runtime/Abi.hpp"
#include "runtime/libc/Format.hpp"
#include "selftest/SelfTest.hpp"
#include <array>
#include <atomic>
#include <sys/file.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <map>
#include "runtime/libc/TlsBridge.hpp"
#if defined(HVM_HAVE_OPENSSL) && !defined(__APPLE__)
#include <csignal>
#include <pthread.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#endif
#include <sstream>
#include <string>
#include <vector>

using namespace Hydrogen;

namespace {

constexpr uint32_t kStackTop = 0x08000000u, kStackSize = 0x00100000u;

struct Harness {
    Memory::GuestArena mem;
    CPU::Engine engine{mem};
    Loader::ElfImage img;
    uint32_t haltAddr = 0;

    explicit Harness(const std::string& elf) {
        img = Loader::loadElf(mem, Loader::readFile(elf));
        mem.map(kStackTop - kStackSize, kStackSize, Memory::kProtRW);
        haltAddr = engine.registerHle("halt", [](CPU::Cpu& c) { c.exit = CPU::Exit::Halt; });
    }

    // Returns false (with message) on abnormal exit.
    bool call(CPU::Cpu& c, uint32_t fn, std::string& err, int64_t budget = 2'000'000'000) {
        c.r[CPU::kSP] = kStackTop - 64;
        c.r[CPU::kLR] = haltAddr;
        c.r[CPU::kPC] = fn & ~1u;
        c.thumb = fn & 1;
        CPU::Exit e = engine.run(c, budget);
        if (e == CPU::Exit::Halt) return true;
        char buf[160];
        const char* why = "?";
        switch (e) {
            case CPU::Exit::Undefined:  why = "undefined/unimplemented instruction"; break;
            case CPU::Exit::MemFault:   why = "memory fault"; break;
            case CPU::Exit::Breakpoint: why = "breakpoint"; break;
            case CPU::Exit::Svc:        why = "svc"; break;
            case CPU::Exit::Budget:     why = "timeout (budget exhausted)"; break;
            default: break;
        }
        uint32_t raw = 0;
        if (e == CPU::Exit::Undefined) raw = c.thumb ? mem.read<uint16_t>(c.exit_info) | (uint32_t(mem.read<uint16_t>(c.exit_info + 2)) << 16)
                                                     : mem.read<uint32_t>(c.exit_info);
        std::snprintf(buf, sizeof buf, "%s (info=0x%08X pc=0x%08X %s insn=0x%08X)", why, c.exit_info,
                      c.r[CPU::kPC], c.thumb ? "thumb" : "arm", raw);
        err = buf;
        return false;
    }
};

struct Check { std::string key; uint32_t value; };
struct AsmTest { std::string name; std::vector<Check> init, expect; };

std::vector<Check> parseChecks(std::istringstream& is) {
    std::vector<Check> out;
    std::string tok;
    while (is >> tok) {
        auto eq = tok.find('=');
        if (eq == std::string::npos) continue;
        out.push_back({tok.substr(0, eq), uint32_t(std::stoul(tok.substr(eq + 1), nullptr, 0))});
    }
    return out;
}

std::vector<AsmTest> parseAsmTests(const std::string& path) {
    std::ifstream in(path);
    std::vector<AsmTest> tests;
    std::string line;
    while (std::getline(in, line)) {
        auto p = line.find("@ ");
        if (p == std::string::npos) continue;
        std::istringstream is(line.substr(p + 2));
        std::string kw; is >> kw;
        if (kw == "TEST") { AsmTest t; is >> t.name; tests.push_back(t); }
        else if (kw == "INIT" && !tests.empty()) { auto v = parseChecks(is); tests.back().init.insert(tests.back().init.end(), v.begin(), v.end()); }
        else if (kw == "EXPECT" && !tests.empty()) { auto v = parseChecks(is); tests.back().expect.insert(tests.back().expect.end(), v.begin(), v.end()); }
    }
    return tests;
}

uint32_t* field(CPU::Cpu& c, const std::string& k, uint32_t& tmp) {
    if (k.size() >= 2 && k[0] == 'r') return &c.r[std::stoi(k.substr(1))];
    if (k == "sp") return &c.r[13];
    if (k == "lr") return &c.r[14];
    return &tmp;
}

int runAsm(const std::string& elf, const std::vector<std::string>& sources) {
    Harness h(elf);
    int pass = 0, fail = 0;
    for (const auto& src : sources) {
        for (const auto& t : parseAsmTests(src)) {
            auto it = h.img.symbols.find(t.name);
            if (it == h.img.symbols.end()) { std::printf("FAIL %-28s symbol not found\n", t.name.c_str()); ++fail; continue; }
            CPU::Cpu c;
            for (const auto& ch : t.init) {
                uint32_t tmp;
                if (ch.key == "nzcv") c.nzcv = ch.value << 28;
                else if (ch.key == "q") c.q = ch.value;
                else if (ch.key == "ge") c.ge = ch.value << 16;
                else *field(c, ch.key, tmp) = ch.value;
            }
            std::string err;
            const Check* faultWant = nullptr;
            const Check* undefWant = nullptr;
            for (const auto& ch : t.expect) {
                if (ch.key == "fault") faultWant = &ch;
                if (ch.key == "undef") undefWant = &ch;
            }
            const bool ok = h.call(c, it->second, err);
            if (faultWant) {
                if (!ok && c.exit == CPU::Exit::MemFault && c.exit_info == faultWant->value) ++pass;
                else { std::printf("FAIL %-28s expected fault at 0x%X: %s\n", t.name.c_str(), faultWant->value, ok ? "returned normally" : err.c_str()); ++fail; }
                continue;
            }
            if (undefWant) {
                if (!ok && c.exit == CPU::Exit::Undefined) {
                    uint32_t hw1 = c.thumb ? h.mem.read<uint16_t>(c.exit_info) : 0;
                    uint32_t raw = c.thumb ? (hw1 | (uint32_t(h.mem.read<uint16_t>(c.exit_info + 2)) << 16)) : h.mem.read<uint32_t>(c.exit_info);
                    if (raw == undefWant->value || (c.thumb && hw1 == undefWant->value)) {
                        ++pass;
                        continue;
                    }
                    std::printf("FAIL %-28s expected undef instruction 0x%X, got 0x%X at PC 0x%X\n", t.name.c_str(), undefWant->value, raw, c.exit_info);
                } else {
                    std::printf("FAIL %-28s expected undef instruction 0x%X: %s\n", t.name.c_str(), undefWant->value, ok ? "returned normally" : err.c_str());
                }
                ++fail;
                continue;
            }
            if (!ok) { std::printf("FAIL %-28s %s\n", t.name.c_str(), err.c_str()); ++fail; continue; }
            std::string bad;
            for (const auto& ch : t.expect) {
                uint32_t got, tmp = 0;
                if (ch.key == "nzcv") got = c.nzcv >> 28;
                else if (ch.key == "q") got = c.q;
                else if (ch.key == "ge") got = c.ge >> 16;
                else if (ch.key == "thumb") got = c.thumb;
                else got = *field(c, ch.key, tmp);
                if (got != ch.value) {
                    char b[96]; std::snprintf(b, sizeof b, " %s=0x%X(want 0x%X)", ch.key.c_str(), got, ch.value);
                    bad += b;
                }
            }
            if (bad.empty()) ++pass;
            else { std::printf("FAIL %-28s%s\n", t.name.c_str(), bad.c_str()); ++fail; }
        }
    }
    std::printf("asm %-40s %d passed, %d failed\n", elf.c_str(), pass, fail);
    return fail ? 1 : 0;
}

int runC(const std::string& elf, const std::string& expectedPath) {
    Harness h(elf);
    Harness hs(elf);   // separate image for the time-sliced pass (tests keep static state)
    std::ifstream in(expectedPath);
    std::string name; uint64_t want;
    int pass = 0, fail = 0;
    uint64_t insns = 0;
    while (in >> name >> std::hex >> want) {
        auto it = h.img.symbols.find(name);
        if (it == h.img.symbols.end()) { std::printf("FAIL %-28s symbol not found\n", name.c_str()); ++fail; continue; }
        CPU::Cpu c;
        std::string err;
        if (!h.call(c, it->second, err)) { std::printf("FAIL %-28s %s\n", name.c_str(), err.c_str()); ++fail; continue; }
        insns += c.insn_count;
        const uint64_t got = (uint64_t(c.r[1]) << 32) | c.r[0];
        // Re-run in tiny time slices: every block exit must leave a resumable state.
        CPU::Cpu s;
        s.r[CPU::kSP] = kStackTop - 64; s.r[CPU::kLR] = hs.haltAddr;
        s.r[CPU::kPC] = it->second & ~1u; s.thumb = it->second & 1;
        CPU::Exit e;
        int slices = 0;
        while ((e = hs.engine.run(s, 97)) == CPU::Exit::Budget) ++slices;
        const uint64_t gotSliced = (uint64_t(s.r[1]) << 32) | s.r[0];
        if (got == want && e == CPU::Exit::Halt && gotSliced == want) ++pass;
        else { std::printf("FAIL %-28s got 0x%016llX sliced 0x%016llX (%d slices) want 0x%016llX\n", name.c_str(),
                           (unsigned long long)got, (unsigned long long)gotSliced, slices, (unsigned long long)want); ++fail; }
    }
    std::printf("c   %-40s %d passed, %d failed (%llu guest insns, %llu blocks)\n", elf.c_str(), pass, fail,
                (unsigned long long)insns, (unsigned long long)h.engine.stats().blocks_decoded);
    return fail ? 1 : 0;
}

} // namespace

#include "runtime/Process.hpp"
#include "runtime/Support.hpp"
#include "runtime/FrameHistogram.hpp"
#include "runtime/audio/Audio.hpp"
#include "ToneMp3.inc"
#include "AudioOutput.hpp"
#include <functional>
#include <map>
#include <random>
#include <vector>
namespace Hydrogen::Runtime { std::vector<uint8_t> hvmDigest(int alg, const std::string& s); }
#include "runtime/libc/HostCrypto.hpp"
#include "runtime/uikit/UIKit.hpp"
#include "runtime/gl/HostGL.hpp"
#include "runtime/foundation/Foundation.hpp"
#include "runtime/uikit/CoreGraphics.hpp"
#include "runtime/graphics/Graphics.hpp"
#include "runtime/graphics/TrueType.hpp"
#include "runtime/graphics/Shader.hpp"
#include "TestFont.hpp"
#include "DesktopInput.hpp"
#include <bit>
#include <cmath>
#include <cstring>

int runMinimp3HwcapChecks();
int runHostFaultTests();
int runHostFaultChild(int fd, bool stress, bool afterUnwind);
int runThreadProgressTests();
int runGuestCallBoundaryTests();
int runSuspendTests();
int runGuestCallBoundaryChild(int mode);
int runThreadLifetimeTests();
int runThreadLifetimeChild();
namespace Hydrogen { namespace Runtime { bool hvmDeliverGuestFault(Process& p, CPU::Cpu& c); } }
int runArenaPoolTests();

namespace {

// Independent of HVM_ZOMBIES in the environment: enableZombies is the same
// switch. Returns false when the 16384-record cap is missing or when an
// evicted zombie is handed back to the guest heap.
bool zombieMetadataStaysCapped() {
    using namespace Hydrogen;
    using namespace Hydrogen::Runtime;
    constexpr int kZombieCap = 16384;
    const std::string named = "-[NSObject description]: message sent to deallocated instance";
    ProcessConfig zcfg;
    zcfg.homePath = "/tmp/hvm_zombie_cap";
    zcfg.bundlePath = "/tmp/hvm_zombie_cap/Z.app";
    zcfg.enableZombies = true;
    zcfg.log = [](LogLevel, const std::string&) {};
    Process zp(zcfg);
    auto& zr = ObjC::rt(zp);
    auto& zcpu = zp.mainThread().cpu;
    zp.mem().map(0xBF000000u - (1u << 20), 1u << 20, Memory::kProtRW);
    zcpu.r[CPU::kSP] = 0xBF000000u - 256;
    zcpu.r[7] = 0;
    auto make = [&]() {
        return ObjC::id(zr.send(zcpu, ObjC::id(zr.send(zcpu, zr.getClass("NSObject"), zr.selAlloc)), "init"));
    };
    auto message = [&](ObjC::id obj) {
        std::string msg;
        try { zr.send(zcpu, obj, "description"); } catch (const std::exception& e) { msg = e.what(); }
        return msg;
    };
    std::vector<ObjC::id> objects;
    objects.reserve(size_t(kZombieCap) + 1);
    for (int i = 0; i < kZombieCap + 1; ++i) objects.push_back(make());
    for (int i = 0; i < kZombieCap; ++i) zr.send(zcpu, objects[size_t(i)], "release");
    const bool held = message(objects[0]).find(named) != std::string::npos;
    zr.send(zcpu, objects[size_t(kZombieCap)], "release");
    const std::string evictedMsg = message(objects[0]);
    const bool evicted = evictedMsg.find("message sent to deallocated instance") == std::string::npos &&
                         evictedMsg.find("unrecognized selector sent to instance") != std::string::npos;
    const bool recent = message(objects[1]).find(named) != std::string::npos &&
                        message(objects[size_t(kZombieCap)]).find(named) != std::string::npos;
    const ObjC::id oldest = objects[0];
    zp.mem().write<uint32_t>(oldest, 0x5A5A5A5Au);
    const ObjC::id fresh = make();
    const bool kept = zp.heap().blockSize(oldest) != 0 && fresh != oldest &&
                      zp.mem().read<uint32_t>(oldest) == 0x5A5A5A5Au;
    if (!(held && evicted && recent && kept)) {
        std::printf("    zombie cap detail held=%d evicted=%d recent=%d kept=%d\n", held, evicted, recent, kept);
        std::printf("    evicted message: %.200s\n", evictedMsg.c_str());
    }
    return held && evicted && recent && kept;
}

int runPropertyTests(const std::string& plantOffByOne = "") {
    int pass = 0, fail = 0;
    auto check = [&](bool ok, const char* what) {
        if (ok) { std::printf("  ok   %s\n", what); ++pass; }
        else { std::printf("  FAIL %s\n", what); ++fail; }
    };

    using namespace Hydrogen::Runtime;
    ProcessConfig cfg;
    const std::string home = "/tmp/hvm_prop_tests_" + std::to_string(getpid());
    std::error_code ec;
    std::filesystem::create_directories(home + "/Test.app", ec);
    cfg.homePath = home;
    cfg.bundlePath = home + "/Test.app";
    Process p(cfg);
    auto& cpu = p.mainThread().cpu;
    p.mem().map(0xbee00000, 1u << 20, Memory::kProtRW);
    cpu.r[CPU::kSP] = 0xbef00000 - 256;

    auto call = [&](const char* name, std::initializer_list<uint32_t> args) {
        auto sym = p.lookupSymbol(name);
        return sym ? uint32_t(p.callGuest(cpu, *sym, args)) : 0u;
    };

    const uint32_t bufA = 0xbee10000;
    const uint32_t bufB = 0xbee20000;

    // 0. C-locale wide-character conversions (issue #330). Expected values are
    // fixed by the C/POSIX specification, not computed from the code under test.
    {
        const uint32_t WEOF_ = 0xFFFFFFFFu, SIZE_ERR = 0xFFFFFFFFu, SIZE_INCOMPLETE = 0xFFFFFFFEu;
        const uint32_t dst = bufB;
        p.mem().write<uint8_t>(bufA, 'A'); p.mem().write<uint8_t>(bufA + 1, 0);
        p.mem().write<uint32_t>(dst, 0x7777);
        bool ok = true;
        ok = ok && call("_btowc", {'A'}) == 'A' && call("_btowc", {0xE9}) == 0xE9 && call("_btowc", {0xFFFFFFFFu}) == WEOF_
                && call("_btowc", {256}) == WEOF_;
        ok = ok && call("_wctob", {'A'}) == 'A' && call("_wctob", {0xE9}) == 0xE9 && call("_wctob", {0x20AC}) == 0xFFFFFFFFu;
        // mbrtowc: one byte -> one wide char; NUL byte returns 0; n == 0 is "incomplete"; NULL s resets and returns 0.
        ok = ok && call("_mbrtowc", {dst, bufA, 1, 0}) == 1 && p.mem().read<uint32_t>(dst) == 'A';
        ok = ok && call("_mbrtowc", {dst, bufA + 1, 1, 0}) == 0 && p.mem().read<uint32_t>(dst) == 0;
        p.mem().write<uint32_t>(dst, 0x7777);
        ok = ok && call("_mbrtowc", {dst, bufA, 0, 0}) == SIZE_INCOMPLETE && p.mem().read<uint32_t>(dst) == 0x7777;
        ok = ok && call("_mbrtowc", {dst, 0, 5, 0}) == 0;
        ok = ok && call("_mbrlen", {bufA, 1, 0}) == 1 && call("_mbrlen", {bufA + 1, 1, 0}) == 0 && call("_mbrlen", {bufA, 0, 0}) == SIZE_INCOMPLETE;
        ok = ok && call("_mbtowc", {dst, bufA, 1}) == 1 && p.mem().read<uint32_t>(dst) == 'A' && call("_mbtowc", {dst, 0, 1}) == 0;
        // wcrtomb: wide char <= 0xFF -> one byte; larger fails with EILSEQ (92 on Darwin); NULL s returns 1.
        p.mainThread().errnoAddr = bufB + 224;   // this bare test process has no TLS block
        p.mem().write<uint32_t>(bufB + 224, 0);
        p.mem().write<uint8_t>(bufA + 8, 0);
        ok = ok && call("_wcrtomb", {bufA + 8, 'Z', 0}) == 1 && p.mem().read<uint8_t>(bufA + 8) == 'Z';
        ok = ok && call("_wcrtomb", {0, 'Z', 0}) == 1;
        ok = ok && call("_wcrtomb", {bufA + 8, 0x20AC, 0}) == SIZE_ERR;
        ok = ok && p.mem().read<uint32_t>(p.mainThread().errnoAddr) == 92;
        // strxfrm in the C locale is a bounded copy returning the full source length.
        const char* src = "hello";
        p.mem().copyIn(bufA + 16, src, 6);
        p.mem().write<uint32_t>(bufB + 64, 0xAAAAAAAAu);
        ok = ok && call("_strxfrm", {bufB + 64, bufA + 16, 16}) == 5 && std::strcmp((const char*)p.mem().host(bufB + 64), "hello") == 0;
        ok = ok && call("_strxfrm", {0, bufA + 16, 0}) == 5;
        p.mem().write<uint32_t>(bufB + 96, 0xAAAAAAAAu);
        ok = ok && call("_strxfrm", {bufB + 96, bufA + 16, 3}) == 5 && p.mem().read<uint8_t>(bufB + 96 + 3) == 0xAA;
        // wcscoll / wcsxfrm
        const uint32_t w1 = bufB + 128, w2 = bufB + 160, w3 = bufB + 192;
        for (int i = 0; i < 3; ++i) { p.mem().write<uint32_t>(w1 + i * 4, i == 2 ? 0 : 'a' + i); p.mem().write<uint32_t>(w2 + i * 4, i == 2 ? 0 : 'a' + i); }
        p.mem().write<uint32_t>(w2 + 4, 'z');
        ok = ok && call("_wcscoll", {w1, w1}) == 0 && int32_t(call("_wcscoll", {w1, w2})) < 0 && int32_t(call("_wcscoll", {w2, w1})) > 0;
        ok = ok && call("_wcsxfrm", {w3, w1, 8}) == 2 && p.mem().read<uint32_t>(w3) == 'a' && p.mem().read<uint32_t>(w3 + 4) == 'b' && p.mem().read<uint32_t>(w3 + 8) == 0;
        ok = ok && call("_wcsxfrm", {0, w1, 0}) == 2;
        check(ok, "C-locale wide-char conversions: btowc, wctob, mbrtowc, mbrlen, mbtowc, wcrtomb, strxfrm, wcscoll, wcsxfrm");

        // Memory safety: unterminated sources and non-writable destinations fault instead of
        // reading past the mapping or writing a read-only page.
        const uint32_t guardBase = 0xbf100000;   // outside the 1 MB test mapping
        p.mem().map(guardBase, 4096, Memory::kProtRW);
        // guardBase + 4096 is never mapped, so its guest protection is none (unreadable)
        p.mem().map(guardBase + 8192, 4096, Memory::kProtRead);     // read-only destination
        auto faults = [&](const char* name, std::initializer_list<uint32_t> args) {
            try { call(name, args); } catch (const CPU::GuestFault&) { return true; }
            return false;
        };
        for (uint32_t i = 0; i < 4096; ++i) p.mem().write<uint8_t>(guardBase + i, 'x');   // no NUL before the guard page
        bool safe = true;
        safe = safe && faults("_strxfrm", {bufB + 256, guardBase, 8});
        safe = safe && faults("_wcsxfrm", {bufB + 256, guardBase, 8});
        safe = safe && faults("_wcscoll", {guardBase, guardBase});
        p.mem().write<uint8_t>(guardBase + 4095, 0);                                       // terminate, so only the destination is bad
        safe = safe && faults("_strxfrm", {guardBase + 8192, guardBase + 4090, 8});
        safe = safe && faults("_mbrtowc", {guardBase + 8192, bufA, 1, 0});
        safe = safe && faults("_mbtowc", {guardBase + 8192, bufA, 1});
        safe = safe && faults("_wcrtomb", {guardBase + 8192, 'Q', 0});
        safe = safe && faults("_mbrtowc", {bufB + 256, guardBase + 4096, 1, 0});            // unreadable source byte
        check(safe, "wide-char conversions fault on unterminated sources, unreadable sources and read-only destinations");
    }

    // 0b. libc++ std::__1::basic_string (issue #311). The app's inline code reads the
    // object directly, so these check the bytes of the 12-byte object against the
    // classic little-endian libc++ layout, not only the content. Expected values come
    // from that layout: short = {size << 1, chars...} (char: data at +1, 10 chars;
    // wchar_t: data at +4, 1 char); long = {(cap + 1) | 1, size, data}, with cap + 1
    // rounded up to 16 bytes' worth of characters (__recommend) and growth to at least
    // twice the old capacity (__grow_by).
    {
        auto& m = p.mem();
        const std::string C = "__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE";
        const std::string W = "__ZNSt3__112basic_stringIwNS_11char_traitsIwEENS_9allocatorIwEEE";
        auto callS = [&](const std::string& name, std::initializer_list<uint32_t> args) -> uint32_t {
            auto sym = p.lookupSymbol(name);
            if (!sym) { std::printf("  (missing %s)\n", name.c_str()); return 0xDEADu; }
            return uint32_t(p.callGuest(cpu, *sym, args));
        };
        auto cstr = [&](const char* s) { return p.allocCString(s); };
        auto obj = [&]() { const uint32_t o = p.heap().allocZeroed(16); m.write<uint32_t>(o, 0xA5A5A5A5u); return o; };
        // Content of a char string read straight from the layout (independent of the code under test).
        auto text = [&](uint32_t s) {
            const uint8_t b0 = m.read<uint8_t>(s);
            const bool lng = b0 & 1;
            const uint32_t n = lng ? m.read<uint32_t>(s + 4) : b0 >> 1;
            const uint32_t d = lng ? m.read<uint32_t>(s + 8) : s + 1;
            if (d < 0x10000 || n > (1u << 20)) return std::string("<not a string>");   // unconstructed object
            std::string r(n, '\0');
            for (uint32_t i = 0; i < n; ++i) r[i] = char(m.read<uint8_t>(d + i));
            return m.read<uint8_t>(d + n) == 0 ? r : std::string("<no NUL>");
        };
        auto capWord = [&](uint32_t s) { return m.read<uint32_t>(s); };
        bool ok = true;
        auto expect = [&](bool cond, const char* what) { if (!cond) { std::printf("    libc++ string: %s\n", what); ok = false; } };

        // __init(const char*, size_t): short and long.
        const uint32_t a = obj(), b = obj(), c = obj(), d = obj(), e = obj();
        callS(C + "6__initEPKcm", {a, cstr("hello"), 5});
        expect(m.read<uint8_t>(a) == 10 && text(a) == "hello", "__init short: size byte 5 << 1, data at +1");
        callS(C + "6__initEPKcm", {b, cstr("abcdefghijklmnopqrst"), 20});
        expect(capWord(b) == 33 && m.read<uint32_t>(b + 4) == 20 && text(b) == "abcdefghijklmnopqrst", "__init long: cap word (31 + 1) | 1, size 20");
        // The 10-char limit of the inline buffer.
        callS(C + "6__initEPKcm", {c, cstr("0123456789"), 10});
        expect(m.read<uint8_t>(c) == 20 && text(c) == "0123456789", "__init 10 chars stays short");
        callS(C + "6__initEPKcm", {d, cstr("0123456789A"), 11});
        expect(capWord(d) == 17 && text(d) == "0123456789A", "__init 11 chars goes long with cap 15");
        // __init(const char*, size_t, size_t reserve) and __init(size_t, char).
        callS(C + "6__initEPKcmm", {e, cstr("ab"), 2, 40});
        expect(capWord(e) == 49 && text(e) == "ab", "__init with reserve 40: cap 47");
        const uint32_t f = obj();
        callS(C + "6__initEmc", {f, 3, 'z'});
        expect(m.read<uint8_t>(f) == 6 && text(f) == "zzz", "__init(n, c)");

        // Copy constructor: short copies the 12 bytes; long gets its own buffer. Returns this.
        const uint32_t g = obj(), h = obj();
        const uint32_t gr = callS(C + "C1ERKS5_", {g, a});
        expect(gr == g && m.read<uint32_t>(g) == m.read<uint32_t>(a) && m.read<uint32_t>(g + 4) == m.read<uint32_t>(a + 4) &&
                   m.read<uint32_t>(g + 8) == m.read<uint32_t>(a + 8), "copy ctor short: same 12 bytes, returns this");
        callS(C + "C2ERKS5_", {h, b});
        expect(m.read<uint32_t>(h + 8) != m.read<uint32_t>(b + 8) && text(h) == text(b) && capWord(h) == 33, "copy ctor long: own buffer");
        const uint32_t sub = obj();
        callS(C + "C1ERKS5_mmRKS4_", {sub, b, 2, 3, 0});
        expect(text(sub) == "cde", "substring ctor (pos 2, n 3)");

        // push_back on a full inline buffer grows to cap 31 (max(11, 2 * 10) -> 31).
        callS(C + "9push_backEc", {c, 'X'});
        expect(capWord(c) == 33 && text(c) == "0123456789X", "push_back past 10 chars goes long, cap 31");
        callS(C + "9push_backEc", {a, '!'});
        expect(m.read<uint8_t>(a) == 12 && text(a) == "hello!", "push_back short stays short");

        // assign / append / insert / replace / erase.
        callS(C + "6assignEPKc", {a, cstr("xy")});
        expect(text(a) == "xy" && m.read<uint8_t>(a) == 4, "assign(const char*) short");
        callS(C + "6appendEPKcm", {a, cstr("0123456789"), 10});
        expect(text(a) == "xy0123456789" && capWord(a) == 33, "append grows: max(12, 20) -> cap 31");
        callS(C + "6appendEmc", {a, 2, '-'});
        callS(C + "6appendEPKc", {a, cstr("end")});
        expect(text(a) == "xy0123456789--end", "append(n, c), append(const char*)");
        callS(C + "6insertEmPKc", {a, 2, cstr("<>")});
        expect(text(a) == "xy<>0123456789--end", "insert(pos, const char*)");
        callS(C + "6insertEmmc", {a, 0, 2, '#'});
        callS(C + "7replaceEmmPKcm", {a, 4, 2, cstr("[]"), 2});
        expect(text(a) == "##xy[]0123456789--end", "insert(pos, n, c), replace(pos, n1, s, n2)");
        callS(C + "5eraseEmm", {a, 6, 10});
        expect(text(a) == "##xy[]--end", "erase(pos, n)");
        callS(C + "7replaceEmmmc", {a, 0, 2, 3, '*'});
        expect(text(a) == "***xy[]--end", "replace(pos, n1, n2, c)");
        // Self-aliasing: append part of its own buffer.
        callS(C + "6appendEPKcm", {a, m.read<uint32_t>(a + 8), 3});
        expect(text(a) == "***xy[]--end***", "append from its own buffer");
        callS(C + "aSERKS5_", {g, b});
        expect(text(g) == "abcdefghijklmnopqrst", "operator=(const basic_string&)");
        callS(C + "aSERKS5_", {g, g});
        expect(text(g) == "abcdefghijklmnopqrst", "self assignment");
        callS(C + "6assignERKS5_mm", {f, b, 18, 99});
        expect(text(f) == "st", "assign(str, pos, n) clips n");

        // Queries. Expected positions are counted by hand from the literal strings.
        const uint32_t npos = 0xFFFFFFFFu;
        expect(p.lookupSymbol(C + "4nposE") && m.read<uint32_t>(*p.lookupSymbol(C + "4nposE")) == npos, "npos value");
        const std::string K = "__ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE";
        const uint32_t q = obj();
        callS(C + "6__initEPKcm", {q, cstr("one two one"), 11});
        expect(callS(K + "4findEPKcmm", {q, cstr("one"), 1, 3}) == 8, "find(s, pos 1)");
        expect(callS(K + "4findEcm", {q, 't', 0}) == 4 && callS(K + "4findEcm", {q, 'z', 0}) == npos, "find(c)");
        expect(callS(K + "5rfindEPKcmm", {q, cstr("one"), npos, 3}) == 8 && callS(K + "5rfindEPKcmm", {q, cstr("one"), 7, 3}) == 0, "rfind(s)");
        expect(callS(K + "5rfindEcm", {q, 'o', npos}) == 8, "rfind(c)");
        expect(callS(K + "13find_first_ofEPKcmm", {q, cstr("wt"), 0, 2}) == 4 && callS(K + "12find_last_ofEPKcmm", {q, cstr("wt"), npos, 2}) == 5,
               "find_first_of, find_last_of");
        expect(callS(K + "17find_first_not_ofEPKcmm", {q, cstr("one"), 0, 3}) == 3 && callS(K + "16find_last_not_ofEPKcmm", {q, cstr("one"), npos, 3}) == 7,
               "find_first_not_of, find_last_not_of");
        expect(int32_t(callS(K + "7compareEPKc", {q, cstr("one two one")})) == 0 && int32_t(callS(K + "7compareEPKc", {q, cstr("one two onf")})) < 0 &&
                   int32_t(callS(K + "7compareEPKc", {q, cstr("one")})) > 0, "compare(const char*)");
        expect(int32_t(callS(K + "7compareEmmPKc", {q, 4, 3, cstr("two")})) == 0 && int32_t(callS(K + "7compareEmmPKcm", {q, 4, 3, cstr("twx"), 2})) > 0,
               "compare(pos, n, s), compare(pos, n, s, n2)");
        expect(int32_t(callS(K + "7compareEmmRKS5_mm", {q, 0, 3, q, 8, 3})) == 0, "compare(pos, n, str, pos2, n2)");
        // Unsigned bytes: 0xE9 sorts after 'a'.
        const uint32_t hi = obj();
        callS(C + "6__initEPKcm", {hi, cstr("\xE9"), 1});
        expect(int32_t(callS(K + "7compareEPKc", {hi, cstr("a")})) > 0, "compare treats bytes as unsigned");
        const uint32_t buf = p.heap().allocZeroed(16);
        expect(callS(K + "4copyEPcmm", {q, buf, 3, 4}) == 3 && std::memcmp(m.host(buf), "two", 3) == 0, "copy(buf, n, pos)");
        expect(callS(C + "2atEm", {q, 4}) == m.read<uint32_t>(q + 8) + 4, "at(n) returns the element address");

        // reserve: grows by __recommend, and shrinks back inline when the content fits.
        const uint32_t r = obj();
        callS(C + "6__initEPKcm", {r, cstr("abc"), 3});
        callS(C + "7reserveEm", {r, 100});
        expect(capWord(r) == 113 && text(r) == "abc", "reserve(100): cap 111");
        const uint32_t oldBuf = m.read<uint32_t>(r + 8);
        callS(C + "7reserveEm", {r, 0});
        expect(m.read<uint8_t>(r) == 6 && text(r) == "abc" && oldBuf && p.heap().blockSize(oldBuf) == 0, "reserve(0) moves back inline and frees");
        callS(C + "6resizeEmc", {r, 5, '.'});
        expect(text(r) == "abc..", "resize up with fill");
        callS(C + "6resizeEmc", {r, 1, '.'});
        expect(text(r) == "a", "resize down");

        // insert(const_iterator, c) returns the new element's address.
        const uint32_t it = callS(C + "6insertENS_11__wrap_iterIPKcEEc", {r, r + 1, 'Q'});
        expect(text(r) == "Qa" && it == r + 1, "insert(iterator, c)");

        // __grow_by_and_replace(old_cap, delta, old_sz, n_copy, n_del, n_add, s) as inline code calls it.
        const uint32_t gb = obj();
        callS(C + "6__initEPKcm", {gb, cstr("abcdef"), 6});
        callS(C + "21__grow_by_and_replaceEmmmmmmPKc", {gb, 10, 5, 6, 2, 1, 12, cstr("XXXXXXXXXXXX")});
        expect(text(gb) == "abXXXXXXXXXXXXdef" && capWord(gb) == 33, "__grow_by_and_replace");
        // __grow_by leaves size to the caller but moves the tail.
        const uint32_t gb2 = obj();
        callS(C + "6__initEPKcm", {gb2, cstr("abcdefghijklmnopqrstu"), 21});
        callS(C + "9__grow_byEmmmmmm", {gb2, 31, 1, 21, 21, 0, 0});
        expect(capWord(gb2) == 65 && m.read<uint32_t>(gb2 + 8) >= 0x10000 && std::memcmp(m.host(m.read<uint32_t>(gb2 + 8)), "abcdefghijklmnopqrstu", 21) == 0, "__grow_by: max(32, 62) -> cap 63");

        // Destructor frees a long buffer and returns this.
        const uint32_t hb = m.read<uint32_t>(h + 8);
        expect(callS(C + "D1Ev", {h}) == h && hb && p.heap().blockSize(hb) == 0, "destructor frees the long buffer");
        callS(C + "D2Ev", {g});

        // Errors stop the call instead of corrupting memory.
        auto throwsHost = [&](const std::string& name, std::initializer_list<uint32_t> args) {
            try { callS(name, args); } catch (const std::exception&) { return true; }
            return false;
        };
        expect(throwsHost(C + "6insertEmPKc", {q, 99, cstr("x")}), "insert past the end is out_of_range");
        expect(throwsHost(K + "2atEm", {q, 11}), "at(size()) is out_of_range");
        expect(throwsHost("__ZNKSt3__121__basic_string_commonILb1EE20__throw_length_errorEv", {q}), "__throw_length_error");
        expect(throwsHost(C + "7reserveEm", {q, 0xFFFFFFF0u}), "reserve beyond max_size is length_error");

        // std::to_string returns through the hidden result pointer in r0.
        const uint32_t ts = obj(), td = obj(), tl = obj();
        callS("__ZNSt3__19to_stringEi", {ts, uint32_t(-42)});
        callS("__ZNSt3__19to_stringEd", {td, 0, 0x400C0000u});   // 3.5
        callS("__ZNSt3__19to_stringEy", {tl, 0x00000000u, 0x00000001u});   // 1 << 32
        expect(text(ts) == "-42" && text(td) == "3.500000" && text(tl) == "4294967296", "to_string(int, double, unsigned long long)");

        check(ok, "libc++ std::string: layout, __init, copy, push_back growth, assign/append/insert/replace/erase, find family, compare, reserve, __grow_by, errors, to_string");

        // Guest-memory safety and cost (review on PR #356). Uses the guard pages from section 0:
        // 0xbf100000 is RW, +4096 is unmapped, +8192 is read-only.
        {
            bool safe = true;
            auto sexpect = [&](bool cond, const char* what) { if (!cond) { std::printf("    libc++ string safety: %s\n", what); safe = false; } };
            auto faults = [&](const std::string& name, std::initializer_list<uint32_t> args) {
                try { callS(name, args); } catch (const CPU::GuestFault& f) { return f.exit == CPU::Exit::MemFault; } catch (const std::exception&) { return false; }
                return false;
            };
            auto stops = [&](const std::string& name, std::initializer_list<uint32_t> args) {
                // A host error (length_error, bad_alloc, bad arguments) reaches the caller wrapped
                // in a GuestFault that is not a memory fault.
                try { callS(name, args); } catch (const CPU::GuestFault& f) { return f.exit != CPU::Exit::MemFault; }
                catch (const std::exception&) { return true; }
                return false;
            };
            const uint32_t g = 0xbf100000;
            const uint32_t s1 = obj();
            callS(C + "6__initEPKcm", {s1, cstr("abc"), 3});
            // A source range that runs into the unmapped page faults instead of being read.
            sexpect(faults(C + "6appendEPKcm", {s1, g + 4090, 16}), "append from a range that crosses into an unmapped page");
            sexpect(faults(C + "6__initEPKcm", {obj(), g + 4096, 4}), "__init from an unmapped page");
            // An unterminated C string ending at the unmapped page faults.
            for (uint32_t i = 0; i < 4096; ++i) m.write<uint8_t>(g + i, 'x');
            sexpect(faults(C + "6assignEPKc", {s1, g + 4000}), "assign(const char*) without a NUL before an unmapped page");
            // Writes to a read-only destination fault.
            sexpect(faults(K + "4copyEPcmm", {q, g + 8192, 3, 0}), "copy into a read-only page");
            sexpect(faults(C + "6__initEPKcm", {g + 8192, cstr("abc"), 3}), "__init into a read-only object");
            // A long string whose buffer is on the read-only page (as inline code may build one):
            // reading works, writing in place faults and leaves the object alone.
            const uint32_t ro = obj();
            for (uint32_t i = 0; i < 4; ++i) m.write<uint8_t>(g + 8192 + i, uint8_t("abc"[i]));
            m.write<uint32_t>(ro, 17); m.write<uint32_t>(ro + 4, 3); m.write<uint32_t>(ro + 8, g + 8192);   // cap 15, size 3
            sexpect(callS(K + "4findEcm", {ro, 'c', 0}) == 2, "a read-only buffer can be searched");
            sexpect(faults(C + "9push_backEc", {ro, 'd'}), "push_back into a read-only buffer");
            sexpect(faults(C + "6resizeEmc", {ro, 1, 0}), "shrinking resize of a read-only buffer");
            sexpect(m.read<uint32_t>(ro + 4) == 3 && m.read<uint8_t>(g + 8192 + 3) == 0, "a faulted write leaves size and data alone");
            // A malformed long string (size above capacity, or a buffer running past 4 GB) is refused
            // before any offset is computed from it.
            // Each fixture is fully mapped and readable, so only the consistency check can refuse it.
            const uint32_t roomy = p.heap().allocZeroed(256);   // readable well past any claimed size
            for (uint32_t i = 0; i < 256; ++i) m.write<uint8_t>(roomy + i, uint8_t('a' + i % 26));
            const uint32_t bad = obj();                         // long: capacity 15, size 20
            m.write<uint32_t>(bad, 17); m.write<uint32_t>(bad + 4, 20); m.write<uint32_t>(bad + 8, roomy);
            sexpect(faults(C + "5eraseEmm", {bad, 0, 1}), "erase on a string whose size exceeds its capacity");
            sexpect(m.read<uint32_t>(bad + 8) == roomy && m.read<uint32_t>(bad + 4) == 20, "the refused erase left the object alone");
            const uint32_t sbad = p.heap().allocZeroed(64);     // short: size byte 0xFE claims 127 chars, inline holds 10
            for (uint32_t i = 1; i < 64; ++i) m.write<uint8_t>(sbad + i, 'z');
            m.write<uint8_t>(sbad, 0xFE);
            const uint32_t sdst = p.heap().allocZeroed(256);
            sexpect(faults(K + "4copyEPcmm", {sbad, sdst, 127, 0}), "copy from a short string whose size byte exceeds the inline buffer");
            const uint32_t wsbad = p.heap().allocZeroed(64);    // wide short: size byte 0x0A claims 5 chars, inline holds 1
            m.write<uint8_t>(wsbad, 0x0A);
            const uint32_t wdst = p.heap().allocZeroed(64);
            // resize and at on the same malformed short wide string must not reach past the
            // 12-byte object (sentinel just after it).
            m.write<uint32_t>(wsbad + 12, 0x5E5E5E5Eu);
            sexpect(faults("__ZNSt3__112basic_stringIwNS_11char_traitsIwEENS_9allocatorIwEEE6resizeEmw", {wsbad, 2, 'q'}) &&
                        m.read<uint32_t>(wsbad + 12) == 0x5E5E5E5Eu, "resize of a malformed short wide string leaves the bytes after it alone");
            sexpect(faults("__ZNSt3__112basic_stringIwNS_11char_traitsIwEENS_9allocatorIwEEE2atEm", {wsbad, 2}), "at() on a malformed short wide string");
            sexpect(faults("__ZNKSt3__112basic_stringIwNS_11char_traitsIwEENS_9allocatorIwEEE4copyEPwmm", {wsbad, wdst, 5, 0}), "copy from a short wide string whose size byte exceeds the inline buffer");
            // The wrap case: a wide string claiming 0x40000002 characters. erase at 0x40000000 would
            // compute data + 0x100000004, which wraps to data + 4 and silently moves real characters.
            const uint32_t wrapBuf = p.heap().allocZeroed(64);
            for (uint32_t i = 0; i < 4; ++i) m.write<uint32_t>(wrapBuf + i * 4, 'A' + i);
            const uint32_t wrap = obj();
            m.write<uint32_t>(wrap, 0x40000004u | 1); m.write<uint32_t>(wrap + 4, 0x40000002u); m.write<uint32_t>(wrap + 8, wrapBuf);
            sexpect(faults(W + "5eraseEmm", {wrap, 0x40000000u, 1}), "erase whose offset would wrap past 4 GB");
            sexpect(m.read<uint32_t>(wrapBuf) == 'A' && m.read<uint32_t>(wrapBuf + 4) == 'B', "the wrapped erase did not move characters");
            // A huge length is refused before anything is allocated on the host.
            sexpect(stops(C + "6appendEmc", {s1, 0x7FFFFFF0u, 'z'}), "append(n, c) beyond the guest heap stops with an error");
            sexpect(stops(C + "6__initEmc", {obj(), 0xFFFFFFF0u, 'z'}), "__init(n, c) beyond max_size is length_error");
            sexpect(text(s1) == "abc", "a refused call leaves the string unchanged");
            // Appends within capacity reuse the buffer (amortised O(1) push_back).
            const uint32_t pb = obj();
            callS(C + "6__initEPKcmm", {pb, cstr("x"), 1, 100});
            const uint32_t buf0 = m.read<uint32_t>(pb + 8);
            for (int i = 0; i < 90; ++i) callS(C + "9push_backEc", {pb, uint32_t('a' + i % 26)});
            sexpect(m.read<uint32_t>(pb + 8) == buf0 && m.read<uint32_t>(pb + 4) == 91 && capWord(pb) == 113, "push_back within capacity keeps the buffer");
            // __grow_by with arguments that do not match the string is refused.
            sexpect(stops(C + "9__grow_byEmmmmmm", {pb, 50, 1, 91, 91, 0, 0}), "__grow_by with the wrong old capacity");
            sexpect(stops(C + "9__grow_byEmmmmmm", {pb, 111, 1, 200, 0, 0, 0}), "__grow_by with old size above the capacity");
            sexpect(m.read<uint32_t>(pb + 8) == buf0, "a refused __grow_by keeps the buffer");
            check(safe, "libc++ std::string: faults on unreadable or read-only guest memory, refuses huge lengths before allocating, appends in place, validates __grow_by");
        }

        // wchar_t: inline buffer holds 1 character at +4; capacities round to 4 characters.
        bool wok = true;
        auto wtext = [&](uint32_t s) {
            const uint8_t b0 = m.read<uint8_t>(s);
            const bool lng = b0 & 1;
            const uint32_t n = lng ? m.read<uint32_t>(s + 4) : b0 >> 1;
            const uint32_t dd = lng ? m.read<uint32_t>(s + 8) : s + 4;
            if (dd < 0x10000 || n > (1u << 20)) return std::u32string(U"<not a string>");
            std::u32string t(n, U'\0');
            for (uint32_t i = 0; i < n; ++i) t[i] = char32_t(m.read<uint32_t>(dd + i * 4));
            return m.read<uint32_t>(dd + n * 4) == 0 ? t : std::u32string(U"<no NUL>");
        };
        auto wcs = [&](const char32_t* s) {
            const uint32_t n = uint32_t(std::char_traits<char32_t>::length(s));
            const uint32_t at = p.heap().allocZeroed((n + 1) * 4);
            for (uint32_t i = 0; i < n; ++i) m.write<uint32_t>(at + i * 4, uint32_t(s[i]));
            return at;
        };
        auto wexpect = [&](bool cond, const char* what) { if (!cond) { std::printf("    libc++ wstring: %s\n", what); wok = false; } };
        const uint32_t wa = obj(), wb = obj(), wc = obj();
        callS(W + "6__initEmw", {wa, 1, 'Z'});
        wexpect(m.read<uint8_t>(wa) == 2 && m.read<uint32_t>(wa + 4) == 'Z' && m.read<uint32_t>(wa + 8) == 0, "__init(1, c) short: data at +4");
        callS(W + "6__initEmw", {wb, 3, 'w'});
        wexpect(capWord(wb) == 5 && m.read<uint32_t>(wb + 4) == 3 && wtext(wb) == U"www", "__init(3, c) long: cap 3");
        callS(W + "6assignEPKw", {wa, wcs(U"hello")});
        wexpect(wtext(wa) == U"hello" && capWord(wa) == 9, "assign(const wchar_t*): max(5, 2) -> cap 7");
        callS(W + "C1ERKS5_", {wc, wa});
        wexpect(wtext(wc) == U"hello" && m.read<uint32_t>(wc + 8) != m.read<uint32_t>(wa + 8), "copy ctor long");
        callS(W + "aSERKS5_", {wb, wc});
        wexpect(wtext(wb) == U"hello", "operator=");
        callS(W + "9push_backEw", {wb, 0x20AC});
        wexpect(wtext(wb) == U"hello€", "push_back non-ASCII");
        const std::string WK = "__ZNKSt3__112basic_stringIwNS_11char_traitsIwEENS_9allocatorIwEEE";
        wexpect(callS(WK + "4findEwm", {wb, 0x20AC, 0}) == 5, "find(wchar_t)");
        const uint32_t wneg = obj();
        callS(W + "6__initEmw", {wneg, 1, 0xFFFFFFFFu});   // wchar_t is signed on iOS: -1 sorts first
        wexpect(int32_t(callS(WK + "7compareEPKw", {wneg, wcs(U"a")})) < 0, "compare treats wchar_t as signed");
        const uint32_t wbuf = m.read<uint32_t>(wc + 8);
        wexpect(callS(W + "D1Ev", {wc}) == wc && wbuf && p.heap().blockSize(wbuf) == 0, "destructor frees");
        check(wok, "libc++ std::wstring: inline layout at +4, capacity rounding, assign, copy, operator=, push_back, find, signed compare, destructor");
    }

    // 1. String property tests vs glibc
    // 1a. strlen & strnlen
    {
        bool strOk = true;
        for (size_t len = 0; len <= 64; ++len) {
            std::string s(len, char('a' + (len % 26)));
            p.mem().copyIn(bufA, s.c_str(), s.size() + 1);
            uint32_t gLen = call("_strlen", {bufA});
            if (plantOffByOne == "strlen" && len == 10) gLen += 1;
            if (gLen != s.size()) { strOk = false; break; }

            for (size_t maxlen = 0; maxlen <= len + 10; ++maxlen) {
                uint32_t gNLen = call("_strnlen", {bufA, uint32_t(maxlen)});
                if (plantOffByOne == "strnlen" && len == 10 && maxlen == 5) gNLen += 1;
                size_t expNLen = std::min(s.size(), maxlen);
                if (gNLen != expNLen) { strOk = false; break; }
            }
            if (!strOk) break;
        }
        check(strOk, "strlen and strnlen property tests match glibc across generated string lengths");
    }

    // 1b. strcmp & strncmp
    {
        bool cmpOk = true;
        std::vector<std::string> inputs = {"", "a", "b", "aa", "ab", "ba", "hello", "world", "hydrogen", "hydrogenvm"};
        for (const auto& s1 : inputs) {
            p.mem().copyIn(bufA, s1.c_str(), s1.size() + 1);
            for (const auto& s2 : inputs) {
                p.mem().copyIn(bufB, s2.c_str(), s2.size() + 1);
                int32_t gCmp = int32_t(call("_strcmp", {bufA, bufB}));
                int hostCmp = std::strcmp(s1.c_str(), s2.c_str());
                if ((gCmp > 0) != (hostCmp > 0) || (gCmp < 0) != (hostCmp < 0) || (gCmp == 0) != (hostCmp == 0)) {
                    cmpOk = false; break;
                }
                for (uint32_t n = 0; n <= 12; ++n) {
                    int32_t gNCmp = int32_t(call("_strncmp", {bufA, bufB, n}));
                    int hostNCmp = std::strncmp(s1.c_str(), s2.c_str(), n);
                    if ((gNCmp > 0) != (hostNCmp > 0) || (gNCmp < 0) != (hostNCmp < 0) || (gNCmp == 0) != (hostNCmp == 0)) {
                        cmpOk = false; break;
                    }
                }
                if (!cmpOk) break;
            }
            if (!cmpOk) break;
        }
        check(cmpOk, "strcmp and strncmp property tests match glibc across paired test strings");
    }

    // 1c. strlcpy
    {
        bool lcpyOk = true;
        std::string src = "The quick brown fox jumps over the lazy dog";
        p.mem().copyIn(bufB, src.c_str(), src.size() + 1);
        for (size_t dstSize = 0; dstSize <= src.size() + 5; ++dstSize) {
            std::vector<char> guestDst(dstSize + 8, 'X');
            p.mem().copyIn(bufA, guestDst.data(), guestDst.size());

            uint32_t ret = call("_strlcpy", {bufA, bufB, uint32_t(dstSize)});
            if (plantOffByOne == "strlcpy" && dstSize == 10) ret += 1;

            if (ret != src.size()) { lcpyOk = false; break; }
            p.mem().copyOut(guestDst.data(), bufA, guestDst.size());

            if (dstSize > 0) {
                size_t expectedCopied = std::min(dstSize - 1, src.size());
                if (std::memcmp(guestDst.data(), src.data(), expectedCopied) != 0) { lcpyOk = false; break; }
                if (guestDst[expectedCopied] != '\0') { lcpyOk = false; break; }
            }
            // The whole sentinel region starting at dstSize (also when dstSize is 0) must be untouched.
            for (size_t i = dstSize; i < guestDst.size() && lcpyOk; ++i)
                if (guestDst[i] != 'X') lcpyOk = false;
            if (!lcpyOk) break;
        }
        check(lcpyOk, "strlcpy property test matches BSD/glibc behavior with bounds safety");
    }

    // 2. Memory helpers vs glibc
    // 2a. memcpy & memset & memchr
    {
        bool memOk = true;
        for (size_t n = 0; n <= 128; ++n) {
            std::vector<uint8_t> testData(n);
            for (size_t i = 0; i < n; ++i) testData[i] = uint8_t((i * 37 + 13) & 0xFF);
            p.mem().copyIn(bufB, testData.data(), n);

            // 8 guard bytes right after the destination must survive both calls (no write past n).
            const std::vector<uint8_t> guard(8, 0xC3);
            std::vector<uint8_t> guardBack(8);
            auto guardIntact = [&]() {
                p.mem().copyOut(guardBack.data(), bufA + uint32_t(n), 8);
                return guardBack == guard;
            };
            p.mem().copyIn(bufA + uint32_t(n), guard.data(), 8);
            uint32_t memsetRet = call("_memset", {bufA, 0x5A, uint32_t(n)});
            if (memsetRet != bufA || !guardIntact()) { memOk = false; break; }
            std::vector<uint8_t> readBack(n);
            p.mem().copyOut(readBack.data(), bufA, n);
            for (size_t i = 0; i < n; ++i) {
                if (readBack[i] != 0x5A) { memOk = false; break; }
            }
            if (!memOk) break;

            p.mem().copyIn(bufA + uint32_t(n), guard.data(), 8);
            uint32_t memcpyRet = call("_memcpy", {bufA, bufB, uint32_t(n)});
            if (memcpyRet != bufA || !guardIntact()) { memOk = false; break; }
            p.mem().copyOut(readBack.data(), bufA, n);
            if (std::memcmp(readBack.data(), testData.data(), n) != 0) { memOk = false; break; }

            if (n > 0) {
                uint8_t targetByte = testData[n / 2];
                uint32_t gChr = call("_memchr", {bufB, targetByte, uint32_t(n)});
                void* hChr = std::memchr(testData.data(), targetByte, n);
                uint32_t expectedAddr = bufB + uint32_t((uint8_t*)hChr - testData.data());
                if (gChr != expectedAddr) { memOk = false; break; }
            }
        }
        check(memOk, "memset, memcpy and memchr property tests match glibc across sizes [0..128]");
    }

    // 2b. memcmp
    {
        bool memcmpOk = true;
        std::vector<uint8_t> arr1(64, 0x42);
        std::vector<uint8_t> arr2(64, 0x42);
        arr2[30] = 0x43;

        p.mem().copyIn(bufA, arr1.data(), 64);
        p.mem().copyIn(bufB, arr2.data(), 64);

        int32_t cBefore = int32_t(call("_memcmp", {bufA, bufB, 30}));
        int32_t cAt = int32_t(call("_memcmp", {bufA, bufB, 31}));
        if (plantOffByOne == "memcmp") cBefore = 1;

        if (cBefore != 0 || cAt >= 0) memcmpOk = false;

        // Generated inputs: the sign of the guest _memcmp must equal the host memcmp for every length, mismatch
        // position and byte value (including bytes >= 0x80, which expose signed-char comparisons).
        uint32_t rng = 0x2545F491u;
        auto next = [&]() { rng = rng * 1664525u + 1013904223u; return uint8_t(rng >> 24); };
        for (size_t n = 0; n <= 64 && memcmpOk; ++n) {
            for (int variant = 0; variant < 6 && memcmpOk; ++variant) {
                std::vector<uint8_t> x(n + 1), y(n + 1);   // never empty: x.data() is read even for n == 0
                for (size_t i = 0; i < n; ++i) x[i] = y[i] = next();
                if (n && variant) { const size_t at = next() % n; y[at] = (variant & 1) ? uint8_t(x[at] ^ 0x80) : next(); }
                p.mem().copyIn(bufA, x.data(), n ? n : 1);
                p.mem().copyIn(bufB, y.data(), n ? n : 1);
                const int32_t g = int32_t(call("_memcmp", {bufA, bufB, uint32_t(n)}));
                const int h = n ? std::memcmp(x.data(), y.data(), n) : 0;
                if ((g > 0) != (h > 0) || (g < 0) != (h < 0)) memcmpOk = false;
            }
        }
        check(memcmpOk, "memcmp property test correctly distinguishes exact boundary match and mismatch");
    }

    // 3. formatGuest vs glibc snprintf
    {
        bool fmtOk = true;
        struct IntTestCase {
            const char* fmt;
            int32_t val;
        };
        std::vector<IntTestCase> intCases = {
            {"%d", 0}, {"%d", 1}, {"%d", -1}, {"%d", 42}, {"%d", -42}, {"%d", 2147483647},
            {"%d", int32_t(-2147483647 - 1)},
            {"%i", 123}, {"%u", 0}, {"%u", 42}, {"%u", int32_t(-1)},
            {"%x", 0}, {"%x", 0x12ab}, {"%X", 0x12ab}, {"%08x", 0x12ab}, {"%08X", int32_t(0xbeef)},
            {"%8d", 42}, {"%-8d", 42}, {"%8d", -42}, {"%-8d", -42},
            {"%+d", 42}, {"%+d", -42}, {"% d", 42}, {"% d", -42},
            {"%.5d", 42}, {"%10.5d", 42}, {"%-10.5d", 42},
            {"%o", 0}, {"%o", 0755}, {"%#o", 0755}, {"%#x", int32_t(0xabcd)}
        };

        for (const auto& tc : intCases) {
            char hostBuf[256];
            std::snprintf(hostBuf, sizeof(hostBuf), tc.fmt, tc.val);

            cpu.r[0] = uint32_t(tc.val);
            Runtime::ArgReader a(cpu, p.mem(), 0);
            std::string guestOut = Runtime::formatGuest(p, tc.fmt, a);

            if (plantOffByOne == "sprintf" && std::string(tc.fmt) == "%08x") {
                guestOut += "0";
            }

            // The guest _sprintf entry point itself: return value and the bytes written to the destination.
            {
                const uint32_t fmtAddr = 0xbee30000;
                p.mem().copyIn(fmtAddr, tc.fmt, std::strlen(tc.fmt) + 1);
                const uint32_t ret = call("_sprintf", {bufA, fmtAddr, uint32_t(tc.val)});
                std::string written(std::strlen(hostBuf) + 1, '\0');
                p.mem().copyOut(written.data(), bufA, written.size());
                if (ret != std::strlen(hostBuf) || std::strcmp(written.c_str(), hostBuf) != 0) { fmtOk = false; break; }
            }

            if (guestOut != hostBuf) {
                fmtOk = false;
                break;
            }
        }

        struct LongTestCase {
            const char* fmt;
            uint64_t val;
        };
        std::vector<LongTestCase> longCases = {
            {"%lld", 0}, {"%lld", 1}, {"%lld", uint64_t(-1)}, {"%lld", 0x123456789ABCDEF0ULL},
            {"%llu", 0}, {"%llu", 10000000000ULL},
            {"%llx", 0}, {"%llx", 0x123456789ABCDEF0ULL}, {"%016llx", 0x12345678ULL}
        };
        for (const auto& tc : longCases) {
            char hostBuf[256];
            std::snprintf(hostBuf, sizeof(hostBuf), tc.fmt, (unsigned long long)tc.val);

            cpu.r[0] = uint32_t(tc.val);
            cpu.r[1] = uint32_t(tc.val >> 32);
            Runtime::ArgReader a(cpu, p.mem(), 0);
            std::string guestOut = Runtime::formatGuest(p, tc.fmt, a);
            if (guestOut != hostBuf) {
                fmtOk = false;
                break;
            }
        }

        {
            std::string sample = "Hello World";
            p.mem().copyIn(bufA, sample.c_str(), sample.size() + 1);

            cpu.r[0] = bufA;
            Runtime::ArgReader aStr(cpu, p.mem(), 0);
            std::string sOut = Runtime::formatGuest(p, "%s", aStr);
            if (sOut != sample) fmtOk = false;

            cpu.r[0] = bufA;
            Runtime::ArgReader aPrec(cpu, p.mem(), 0);
            std::string pOut = Runtime::formatGuest(p, "%.5s", aPrec);
            if (pOut != "Hello") fmtOk = false;

            cpu.r[0] = 0;
            Runtime::ArgReader aNull(cpu, p.mem(), 0);
            std::string nOut = Runtime::formatGuest(p, "%s", aNull);
            if (nOut != "(null)") fmtOk = false;
        }

        check(fmtOk, "formatGuest printf-family property tests match glibc across types, widths, precisions and flags");
    }

    std::filesystem::remove_all(home, ec);
    std::printf("glibc property tests: %d passed, %d failed\n", pass, fail);
    return fail;
}

int runFullSubsystemTests() {
    using namespace Hydrogen;
    using namespace Hydrogen::Runtime;
    int pass = 0, fail = 0;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    auto check = [&](bool ok, const char* what) {
        if (ok) { std::printf("  ok   %s\n", what); ++pass; }
        else { std::printf("  FAIL %s\n", what); ++fail; }
    };

    {
        const int hwcapFails = runMinimp3HwcapChecks();
        check(hwcapFails == 0,
              "minimp3 enables NEON only for the 32-bit ARM HWCAP_NEON bit");
    }

    {
        auto wav = [](int channels, uint32_t fact) {
            std::vector<uint8_t> v;
            auto text = [&](const char* s) { v.insert(v.end(), s, s + 4); };
            auto u16 = [&](uint16_t n) { v.push_back(n); v.push_back(n >> 8); };
            auto u32 = [&](uint32_t n) { u16(n); u16(n >> 16); };
            text("RIFF"); u32(52 + channels * 16); text("WAVE");
            text("fmt "); u32(20); u16(17); u16(channels); u32(44100); u32(0);
            u16(channels * 8); u16(4); u16(2); u16(9);
            text("fact"); u32(4); u32(fact); text("data"); u32(channels * 16);
            for (int block = 0; block < 2; ++block) {
                for (int c = 0; c < channels; ++c) { u16((c ? -1 : 1) * (block ? 10000 : 1000)); u16(0); }
                for (int c = 0; c < channels; ++c) for (int i = 0; i < 4; ++i) v.push_back(c ? 0x99 : 0x11);
            }
            return v;
        };
        auto stereo = wav(2, 17);
        const auto a = Audio::decode(stereo.data(), stereo.size());
        bool expected = a.sampleRate == 44100 && a.channels == 2 && a.stereoPcm.size() == 34;
        for (int i = 0; expected && i < 17; ++i) {
            const int value = i < 9 ? 1000 + i : 10000 + i - 9;
            expected = a.stereoPcm[i*2] == value && a.stereoPcm[i*2+1] == -value;
        }
        check(expected && std::abs(a.duration - 17.0/44100) < 1e-9,
              "IMA WAV stereo nibble order, channel groups, predictor reset and fact trimming");
        auto mono = wav(1, 0);
        const auto b = Audio::decode(mono.data(), mono.size());
        bool duplicated = b.stereoPcm.size() == 36 && b.channels == 1;
        for (size_t i = 0; duplicated && i < 18; ++i) duplicated = b.stereoPcm[i*2] == b.stereoPcm[i*2+1];
        check(duplicated, "IMA WAV mono expands to stereo without losing the predictor sample");
        stereo[62] = 89; // first channel's step index, outside the table
        bool invalid = Audio::decode(stereo.data(), stereo.size()).stereoPcm.empty();
        stereo = wav(2, 19); // fact cannot request more samples than encoded
        invalid &= Audio::decode(stereo.data(), stereo.size()).stereoPcm.empty();
        stereo = wav(2, 17); stereo.pop_back();
        invalid &= Audio::decode(stereo.data(), stereo.size()).stereoPcm.empty();
        check(invalid, "IMA WAV rejects invalid step indices, oversized fact counts and truncated data");
    }
    // A CAF whose codec this build cannot decode (AAC without a host decoder) opens as
    // silence of the packet table's length instead of failing: Fruit Ninja ignores the
    // open error and builds its music queue from an uninitialised struct.
    {
        std::vector<uint8_t> v;
        auto text = [&](const char* s) { v.insert(v.end(), s, s + 4); };
        auto be32 = [&](uint32_t n) { for (int i = 3; i >= 0; --i) v.push_back(uint8_t(n >> (i * 8))); };
        auto be64 = [&](uint64_t n) { be32(uint32_t(n >> 32)); be32(uint32_t(n)); };
        text("caff"); v.push_back(0); v.push_back(1); v.push_back(0); v.push_back(0);
        text("desc"); be64(32);
        double rate = 22050; uint64_t rateBits; std::memcpy(&rateBits, &rate, 8); be64(rateBits);
        text("aac "); be32(0); be32(0); be32(1024); be32(2); be32(0);
        text("pakt"); be64(24); be64(3); be64(3000); be32(2112); be32(960);
        text("data"); be64(4 + 30); be32(0); for (int i = 0; i < 30; ++i) v.push_back(uint8_t(i * 7));
        const auto d = Audio::decode(v.data(), v.size());
        const bool silent = d.silent && d.codec == 0x61616320 && d.sampleRate == 22050 && d.channels == 2 &&
                            d.stereoPcm.size() == 6000 && std::all_of(d.stereoPcm.begin(), d.stereoPcm.end(), [](int16_t s) { return s == 0; }) &&
                            std::abs(d.duration - 3000.0 / 22050) < 1e-9;
        if (!silent) std::printf("    AAC CAF: silent %d codec %08x rate %d channels %d samples %zu\n", d.silent, d.codec, d.sampleRate, d.channels, d.stereoPcm.size());
        check(silent, "a CAF in a codec this build cannot decode opens as silence of its real length");
    }
    {
        int sockets[2];
        const bool opened = socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets) == 0;
        bool received = false, stopped = false;
        uint64_t dropped = 0, nonsilent = 0;
        int transportError = -1, setupError = 0;
        if (opened) {
            int capacity = 8192;
            setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &capacity, sizeof capacity);
#ifdef __APPLE__
            // Darwin's default AF_UNIX datagram receive buffer is 4096 and the
            // queued length includes the 16-byte unnamed sockaddr, so an
            // 8192-byte block returns ENOBUFS before any sample is delivered.
            // 8208 holds one block and rejects the next. Linux ignores
            // SO_RCVBUF for this socket family; SO_SNDBUF above is the limit.
            int receiveCapacity = 8192 + 16;
            if (setsockopt(sockets[1], SOL_SOCKET, SO_RCVBUF, &receiveCapacity, sizeof receiveCapacity) != 0)
                setupError = errno;
#endif
            auto mixer = std::make_shared<Audio::Mixer>();
            auto pcm = std::make_shared<std::vector<int16_t>>(4096, 8192);
            Audio::VoiceParams params; params.loop = true;
            mixer->play(pcm, 44100, params);
            DesktopAudioOutput output;
            output.start(sockets[0], mixer);
            pollfd pfd{sockets[1], POLLIN, 0};
            if (poll(&pfd, 1, 1000) == 1) {
                int16_t block[4096];
                received = recv(sockets[1], block, sizeof block, 0) == sizeof block && block[0] > 8000 && block[1] > 8000;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(300)); // leave the consumer stalled
            const auto began = std::chrono::steady_clock::now();
            output.stop();
            dropped = output.droppedBlocks;
            nonsilent = output.nonSilentFrames;
            transportError = output.error;
            stopped = setupError == 0 && dropped > 0 && nonsilent > 0 && transportError == 0 &&
                      std::chrono::steady_clock::now() - began < std::chrono::milliseconds(200);
            close(sockets[0]); close(sockets[1]);
        }
        if (!(received && stopped))
            std::printf("    audio detail open=%d received=%d dropped=%llu nonsilent=%llu error=%d setup=%d\n",
                        int(opened), int(received), (unsigned long long)dropped,
                        (unsigned long long)nonsilent, transportError, setupError);
        check(received && stopped, "Desktop audio transports actual mixer PCM and bounds a stalled consumer without blocking shutdown");
    }

    {
        bool rtti_ok = false;
        try {
            Hydrogen::Runtime::ProcessConfig cfg;
            cfg.stopOnMissingFunction = false;
            Hydrogen::Runtime::Process p(cfg);

            rtti_ok = p.lookupSymbol("_ZTIPv").has_value() && p.lookupSymbol("__ZTIPv").has_value() && p.lookupSymbol("_ZTIPKv").has_value() && p.lookupSymbol("__ZTIPKv").has_value();
        } catch (...) {}
        check(rtti_ok, "C++ RTTI pointer types are successfully registered as missing data symbols");
    }

    // Mixer stats (what Test All Games' music / sound-effects checks read): a short one-shot
    // voice counts as an effect, a looping one as music, a silent one as neither.
    {
        Audio::Mixer m;
        auto loud = std::make_shared<std::vector<int16_t>>(2 * 2048, int16_t(8000));
        auto quiet = std::make_shared<std::vector<int16_t>>(2 * 2048, int16_t(0));
        Audio::VoiceParams once, looped; looped.loop = true;
        m.play(loud, 44100, once);
        m.play(loud, 44100, looped);
        m.play(quiet, 44100, once);
        std::vector<float> l(512), r(512);
        for (int i = 0; i < 16; i++) m.render(l.data(), r.data(), 512);
        const auto st = m.stats();
        const bool ok = st.framesRendered == 16 * 512 && st.effectFrames >= 2048 && st.effectFrames <= 2048 + 512 &&
                        st.musicFrames == 16 * 512 && st.effectVoices == 1 && st.musicVoices == 1 &&
                        st.audibleFrames == 16 * 512 && st.peak > 0.2f;
        if (!ok) std::printf("    mixer stats rendered=%llu audible=%llu music=%llu fx=%llu mv=%u fv=%u peak=%.3f\n",
                             (unsigned long long)st.framesRendered, (unsigned long long)st.audibleFrames,
                             (unsigned long long)st.musicFrames, (unsigned long long)st.effectFrames,
                             st.musicVoices, st.effectVoices, st.peak);
        check(ok, "Mixer stats tell music (looping) from sound effects (one-shot) and ignore silent voices");
    }

    // Mixer headroom and stream stitching (#260: audio blown out / buzzing on the device speaker).
    // Every expected value is analytic, never taken from the mixer: the inputs are int16 sines with an
    // integer period (441 Hz at 44.1 kHz = 100 frames), so the ideal bus value of every output frame
    // follows from the input samples, the voice gains and the definition of linear resampling.
    {
        constexpr double kTwoPi = 6.283185307179586;
        auto sine = [&](int frames, double amp, int period) {   // amp in full-scale units, L == R
            auto pcm = std::make_shared<std::vector<int16_t>>(size_t(frames) * 2);
            for (int n = 0; n < frames; ++n) {
                const auto v = int16_t(std::lround(amp * 32767.0 * std::sin(kTwoPi * double(n) / double(period))));
                (*pcm)[size_t(n) * 2] = v;
                (*pcm)[size_t(n) * 2 + 1] = v;
            }
            return pcm;
        };
        auto renderLeft = [&](Audio::Mixer& m, size_t frames) {
            std::vector<float> out;
            std::vector<float> l(512), r(512);
            while (out.size() < frames) {
                const size_t n = std::min<size_t>(512, frames - out.size());
                m.render(l.data(), r.data(), n);
                out.insert(out.end(), l.begin(), l.begin() + long(n));
            }
            return out;
        };
        struct Fit { double k = 0, residual = 1, peak = 0; int rail = 0; };
        // Least-squares scale of the ideal waveform onto the output, the relative RMS of what is left
        // (distortion, so a constant gain reduction costs nothing), peak, and samples sitting on the rail.
        auto fit = [&](const std::vector<float>& out, const std::vector<double>& ideal, size_t from) {
            Fit f;
            double sxy = 0, sxx = 0;
            for (size_t i = from; i < out.size(); ++i) {
                sxy += double(out[i]) * ideal[i];
                sxx += ideal[i] * ideal[i];
                f.peak = std::max(f.peak, std::fabs(double(out[i])));
                if (std::fabs(out[i]) >= 1.0f) ++f.rail;
            }
            f.k = sxx > 0 ? sxy / sxx : 0;
            double res = 0, sig = 0;
            for (size_t i = from; i < out.size(); ++i) {
                const double e = double(out[i]) - f.k * ideal[i];
                res += e * e;
                sig += (f.k * ideal[i]) * (f.k * ideal[i]);
            }
            f.residual = sig > 0 ? std::sqrt(res / sig) : 1.0;
            return f;
        };
        constexpr int kLoopFrames = 4400;   // 44 whole periods, so a looping voice stays phase-continuous
        constexpr size_t kFrames = 16384, kSettle = 8192;
        constexpr double kMaxResidual = 0.02;   // chosen before measuring: -34 dB distortion

        // 1. Two loud voices at once: 0.7 + 0.7 full scale sums to 1.4. The output must stay a clean
        //    (scaled) sine instead of being flattened against the rail.
        {
            Audio::Mixer m;
            Audio::VoiceParams loop; loop.loop = true;
            auto a = sine(kLoopFrames, 0.7, 100);
            m.play(a, 44100, loop);
            m.play(a, 44100, loop);
            std::vector<double> ideal(kFrames);
            for (size_t i = 0; i < kFrames; ++i) ideal[i] = 2.0 * double((*a)[(i % kLoopFrames) * 2]) / 32768.0;
            const Fit f = fit(renderLeft(m, kFrames), ideal, kSettle);
            const bool ok = f.residual < kMaxResidual && f.rail == 0 && f.k > 0.6;
            if (!ok) std::printf("    mixer headroom (2 x 0.7 FS): k=%.3f residual=%.4f peak=%.4f railSamples=%d\n", f.k, f.residual, f.peak, f.rail);
            check(ok, "Mixer keeps two summed loud voices (bus peak 1.4 FS) undistorted instead of hard-clipping them");
        }

        // 2. One voice with app gain above 1 (OpenAL AL_GAIN may exceed 1): 0.5 FS x 3.0 = 1.5 FS bus.
        {
            Audio::Mixer m;
            Audio::VoiceParams loop; loop.loop = true; loop.gain = 3.0f;
            auto a = sine(kLoopFrames, 0.5, 100);
            m.play(a, 44100, loop);
            std::vector<double> ideal(kFrames);
            for (size_t i = 0; i < kFrames; ++i) ideal[i] = 3.0 * double((*a)[(i % kLoopFrames) * 2]) / 32768.0;
            const Fit f = fit(renderLeft(m, kFrames), ideal, kSettle);
            const bool ok = f.residual < kMaxResidual && f.rail == 0 && f.k > 0.6;
            if (!ok) std::printf("    mixer headroom (0.5 FS x gain 3): k=%.3f residual=%.4f peak=%.4f railSamples=%d\n", f.k, f.residual, f.peak, f.rail);
            check(ok, "Mixer keeps a voice whose gain pushes the bus to 1.5 FS undistorted instead of hard-clipping it");
        }

        // 3. Transparency: a mix that stays under full scale must pass through unchanged (a limiter
        //    must not quieten ordinary content). One voice at 0.5 FS, gain 1: output == input / 32768.
        {
            Audio::Mixer m;
            Audio::VoiceParams loop; loop.loop = true;
            auto a = sine(kLoopFrames, 0.5, 100);
            m.play(a, 44100, loop);
            const std::vector<float> out = renderLeft(m, kFrames);
            double worst = 0;
            for (size_t i = 0; i < kFrames; ++i)
                worst = std::max(worst, std::fabs(double(out[i]) - double((*a)[(i % kLoopFrames) * 2]) / 32768.0));
            const bool ok = worst < 1e-6;
            if (!ok) std::printf("    mixer transparency (0.5 FS): worst error %.3g\n", worst);
            check(ok, "Mixer passes a mix that stays under full scale through bit-for-bit (no level change)");
        }

        // 3b. Stereo image: the limiter gain is shared by both channels. Two voices with unequal channel
        //     levels (left bus peak 1.4 FS, right 0.7 FS) must keep their 2:1 ratio. Clamping or limiting
        //     the channels independently would flatten only the left one and bend the image.
        {
            Audio::Mixer m;
            Audio::VoiceParams loop; loop.loop = true;
            auto uneven = std::make_shared<std::vector<int16_t>>(size_t(kLoopFrames) * 2);
            for (int n = 0; n < kLoopFrames; ++n) {
                const double w = std::sin(kTwoPi * double(n) / 100.0) * 32767.0;
                (*uneven)[size_t(n) * 2] = int16_t(std::lround(0.7 * w));
                (*uneven)[size_t(n) * 2 + 1] = int16_t(std::lround(0.35 * w));
            }
            m.play(uneven, 44100, loop);
            m.play(uneven, 44100, loop);
            std::vector<float> outL, outR, l(512), r(512);
            while (outL.size() < kFrames) {
                m.render(l.data(), r.data(), 512);
                outL.insert(outL.end(), l.begin(), l.end());
                outR.insert(outR.end(), r.begin(), r.end());
            }
            double worstRatio = 0, peakL = 0;
            for (size_t i = kSettle; i < kFrames; ++i) {
                peakL = std::max(peakL, std::fabs(double(outL[i])));
                if (std::fabs(outL[i]) > 0.05f)   // skip zero crossings, where the ratio is noise
                    worstRatio = std::max(worstRatio, std::fabs(double(outR[i]) / double(outL[i]) - 0.5));
            }
            const bool ok = worstRatio < 0.01 && peakL < 1.0 && peakL > 0.6;
            if (!ok) std::printf("    mixer stereo image: worst |R/L - 0.5| = %.4f, left peak %.4f\n", worstRatio, peakL);
            check(ok, "Mixer limits both channels with one shared gain, so unequal channel levels keep their ratio");
        }

        // 3c. Mirror of 3b: with the dominant channel on the right, the shared gain must still follow it.
        //     Together with 3b this catches a limiter that looks at only one channel.
        {
            Audio::Mixer m;
            Audio::VoiceParams loop; loop.loop = true;
            auto uneven = std::make_shared<std::vector<int16_t>>(size_t(kLoopFrames) * 2);
            for (int n = 0; n < kLoopFrames; ++n) {
                const double w = std::sin(kTwoPi * double(n) / 100.0) * 32767.0;
                (*uneven)[size_t(n) * 2] = int16_t(std::lround(0.35 * w));
                (*uneven)[size_t(n) * 2 + 1] = int16_t(std::lround(0.7 * w));
            }
            m.play(uneven, 44100, loop);
            m.play(uneven, 44100, loop);
            std::vector<float> outL, outR, l(512), r(512);
            while (outL.size() < kFrames) {
                m.render(l.data(), r.data(), 512);
                outL.insert(outL.end(), l.begin(), l.end());
                outR.insert(outR.end(), r.begin(), r.end());
            }
            double worstRatio = 0, peakR = 0;
            for (size_t i = kSettle; i < kFrames; ++i) {
                peakR = std::max(peakR, std::fabs(double(outR[i])));
                if (std::fabs(outR[i]) > 0.05f)
                    worstRatio = std::max(worstRatio, std::fabs(double(outL[i]) / double(outR[i]) - 0.5));
            }
            const bool ok = worstRatio < 0.01 && peakR < 1.0 && peakR > 0.6;
            if (!ok) std::printf("    mixer mirrored stereo image: worst |L/R - 0.5| = %.4f, right peak %.4f\n", worstRatio, peakR);
            check(ok, "Mixer limits a dominant right channel with the shared gain too (mirror of the left-dominant case)");
        }

        // 4. Stream stitching: a 22.05 kHz stream of a 441 Hz sine (50-frame period) queued as three
        //    1000-frame buffers. Output frame j reads source position j/2 of the concatenated stream, so
        //    the ideal value at an odd j is the mean of source frames j/2 and j/2 + 1, also across a
        //    buffer boundary. A buffer edge that repeats its last frame instead leaves an error every 1000
        //    source frames (a periodic click train).
        {
            Audio::Mixer m;
            const uint32_t v = m.createStream({});
            constexpr int kChunk = 1000, kChunks = 3;
            auto whole = sine(kChunk * kChunks, 0.5, 50);
            for (int c = 0; c < kChunks; ++c) {
                auto part = std::make_shared<std::vector<int16_t>>(whole->begin() + long(c) * kChunk * 2, whole->begin() + long(c + 1) * kChunk * 2);
                m.enqueue(v, part, 22050, uint64_t(c + 1));
            }
            const size_t outFrames = size_t(kChunk) * kChunks * 2 - 2;
            const std::vector<float> out = renderLeft(m, outFrames);
            double worst = 0;
            size_t worstAt = 0;
            for (size_t j = 0; j < outFrames; ++j) {
                const size_t k = j / 2;
                const double s0 = double((*whole)[k * 2]) / 32768.0;
                const double s1 = double((*whole)[(k + 1) * 2]) / 32768.0;
                const double ideal = (j & 1) ? 0.5 * (s0 + s1) : s0;
                const double e = std::fabs(double(out[j]) - ideal);
                if (e > worst) { worst = e; worstAt = j; }
            }
            const bool ok = worst < 1e-4;
            if (!ok) std::printf("    mixer stream stitching: worst error %.5f FS at output frame %zu\n", worst, worstAt);
            check(ok, "Mixer interpolates across queued stream buffer boundaries (no click at each buffer edge)");
        }
    }

    // Straight-line GLSL semantics: independent names, actual values, honest rejection.
    {
        Shader::Program vertex, fragment;
        bool compiled = vertex.compile(R"(
            #ifdef GL_ES
            precision highp float;
            #else
            #define highp
            #endif
            attribute vec4 coordinates;
            attribute vec4 tint;
            uniform vec4 scale;
            uniform mat4 matrix;
            varying vec4 result[2];
            void main() {
                const vec4 offset = vec4(-1.0, 1.0, 0.0, 0.0);
                gl_Position = matrix * (scale * coordinates + offset);
                result[0] = tint;
                result[0].xy = result[0].yx;
                result[1].xy = coordinates.xy + vec2(0.25);
                result[1].zw = tint.ba;
            }
        )", true);
        Shader::Values values;
        vertex.initialize(values);
        auto put = [&](const char* name, std::initializer_list<float> input) {
            if (auto* v = vertex.find(name)) std::copy(input.begin(), input.end(), &values[v->slot]);
        };
        put("coordinates", {2, 3, 0, 1}); put("tint", {0.2f, 0.4f, 0.6f, 0.8f}); put("scale", {0.5f, -0.5f, 1, 1});
        put("matrix", {1, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 0, 0.25f, 0, 0, 1});
        vertex.execute(values);
        auto* varying = vertex.find("result");
        check(compiled && varying && values[vertex.output] == 0.25f && values[vertex.output + 1] == -1 &&
              values[varying->slot] == 0.4f && values[varying->slot + 1] == 0.2f &&
              values[varying->slot + 4] == 2.25f && values[varying->slot + 7] == 0.8f,
              "GLSL executes component transforms, column-major matrices, varying arrays and overlapping swizzles");
        compiled = fragment.compile(R"(
            uniform sampler2D image;
            varying vec2 coordinate;
            void main() {
                vec4 color = texture2D(image, coordinate);
                gl_FragColor.rgb = vec3(dot(color.rgb, vec3(0.25, 0.5, 0.25)));
                gl_FragColor.a = color.r;
                gl_FragColor += vec4(0.1, 0.0, 0.0, 0.0);
            }
        )", false);
        fragment.initialize(values);
        auto* sampler = fragment.find("image"); auto* coordinate = fragment.find("coordinate");
        if (sampler) values[sampler->slot] = 3;
        if (coordinate) { values[coordinate->slot] = 0.25f; values[coordinate->slot + 1] = 0.5f; }
        bool sampled = false;
        fragment.execute(values, [](void* context, int unit, float u, float v, float* out) {
            *static_cast<bool*>(context) = unit == 3 && u == 0.25f && v == 0.5f;
            out[0] = 0.2f; out[1] = 0.4f; out[2] = 0.6f; out[3] = 0.8f;
        }, &sampled);
        check(compiled && sampled && std::abs(values[fragment.output] - 0.5f) < 1e-6f &&
              std::abs(values[fragment.output + 1] - 0.4f) < 1e-6f && values[fragment.output + 3] == 0.2f,
              "GLSL texture sampling honors sampler, channels, dot products and compound assignments");
        bool rejected = true;
        for (const char* bad : {
            "uniform vec4 x; void main() { x = vec4(0.0); gl_FragColor = x; }",
            "void main() { gl_FragColor.xx = vec2(1.0); }",
            "void main() { gl_FragColor = vec3(1.0); }",
            "varying vec4 x[2]; void main() { gl_FragColor = x[2]; }",
            "void main() { gl_FragColor = vec4(1.0).xgba; }",
            "#include <missing>\nvoid main() { gl_FragColor = vec4(1.0); }"
        }) rejected &= !fragment.compile(bad, false) && !fragment.error.empty();
        std::string deep = "void main() { gl_FragColor = vec4(" + std::string(100, '(') + "1.0" + std::string(100, ')') + "); }";
        rejected &= !fragment.compile(deep, false);
        check(rejected, "GLSL rejects invalid writes, types, indexing, #include and excessive nesting");


        auto near4 = [](const float* g, float r, float gch, float b, float a) {
            return std::abs(g[0] - r) < 1e-5f && std::abs(g[1] - gch) < 1e-5f &&
                   std::abs(g[2] - b) < 1e-5f && std::abs(g[3] - a) < 1e-5f;
        };
        auto runStage = [&](const std::string& src, bool vertex, auto&& load, float out[4]) {
            Shader::Program program;
            if (!program.compile(src, vertex)) {
                std::printf("    compile failed: %s\n", program.error.c_str());
                return false;
            }
            Shader::Values values;
            program.initialize(values);
            load(program, values);
            program.execute(values);
            for (int c = 0; c < 4; ++c) out[c] = values[program.output + c];
            return true;
        };
        auto fails = [](const char* src, const char* snippet) {
            Shader::Program program;
            const bool bad = !program.compile(src, false) && program.error.find(snippet) != std::string::npos;
            if (!bad) std::printf("    expected '%s', got: %s\n", snippet, program.error.c_str());
            return bad;
        };
        float o[4] = {};
        check(runStage("#define K 0.25\nvoid main() { gl_FragColor = vec4(K); }", false, [](auto&, auto&) {}, o) && near4(o, 0.25f, 0.25f, 0.25f, 0.25f),
              "GLSL object-like #define");
        check(runStage("#define SECOND(a, b) b\n#define WRAP(a, b) SECOND(a, b)\nvoid main() { gl_FragColor = vec4(WRAP(1.0, SECOND(0.25, 0.75))); }",
                       false, [](auto&, auto&) {}, o) && near4(o, 0.75f, 0.75f, 0.75f, 0.75f),
              "GLSL function-like macros with nested commas");
        check(runStage("#define ID(x) x\n#define F(x) (x)\n#define ONE() 0.125\n#define EMPTY\nvoid main() { EMPTY gl_FragColor = vec4(ID(F)(ONE())); }",
                       false, [](auto&, auto&) {}, o) && near4(o, 0.125f, 0.125f, 0.125f, 0.125f),
              "GLSL macro rescan, zero-argument macros and empty replacement");
        check(runStage("#define K 0.25\n#undef NOT_THERE\n#undef K\n#define K 0.875\n#define K 0.875\nvoid main() { gl_FragColor = vec4(K); }",
                       false, [](auto&, auto&) {}, o) && near4(o, 0.875f, 0.875f, 0.875f, 0.875f) &&
              fails("#define A 1\n#define A 2\nvoid main() { gl_FragColor = vec4(1.0); }", "macro redefinition"),
              "GLSL #undef, identical redefinition and conflicting redefinition");
        check(runStage(
                  "#if defined GL_ES && defined(GL_ES) && !defined(NOT_THERE) && __VERSION__ == 100 && 0x10 == 16 && 010 == 8 && 1u == 1 && (3 > 1 && 4 == 4)\n"
                  "const float k = 0.25;\n#elif 0\nconst float k = 0.5;\n#else\nconst float k = 0.75;\n#endif\n"
                  "void main() { gl_FragColor = vec4(k); }",
                  false, [](auto&, auto&) {}, o) && near4(o, 0.25f, 0.25f, 0.25f, 0.25f),
              "GLSL #if/#elif/#else with defined(), __VERSION__ and integer expressions");
        check(runStage(
                  "#define MINE 1\n#ifdef ABSENT\n@@@\n#if 1/0\n#endif\n#elif MINE\n#ifndef ABSENT\nconst float k = 1.0;\n#else\nstill skipped\n#endif\n#else\nbogus\n#endif\n"
                  "void main() { gl_FragColor = vec4(k); }",
                  false, [](auto&, auto&) {}, o) && near4(o, 1, 1, 1, 1),
              "GLSL #ifdef/#ifndef skip inactive groups, including invalid text");
        check(runStage("#version 100\nvoid main() { gl_FragColor = vec4(1.0); }", false, [](auto&, auto&) {}, o) && near4(o, 1, 1, 1, 1) &&
                  fails("#version 300\nvoid main() { gl_FragColor = vec4(1.0); }", "unsupported version") &&
                  fails("void main() { gl_FragColor = vec4(1.0); }\n#version 100", "version directive must be first"),
              "GLSL #version 100, and rejection of other versions");
        {
            Shader::Program modern;
            const bool modernOk = modern.compile(
                "#version 300 es\n#define texture2D texture\n"
                "out mediump vec4 ttFragOut[1]; in mediump vec2 T0; uniform sampler2D S0;\n"
                "void main() { ttFragOut[0] = texture2D(S0, T0); }", false);
            Shader::Values modernValues;
            modern.initialize(modernValues);
            if (auto* s = modern.find("S0")) modernValues[s->slot] = 2;
            if (auto* t = modern.find("T0")) { modernValues[t->slot] = 0.5f; modernValues[t->slot + 1] = 0.25f; }
            bool modernSampled = false;
            if (modernOk) modern.execute(modernValues, [](void* context, int unit, float u, float v, float* out) {
                *static_cast<bool*>(context) = unit == 2 && u == 0.5f && v == 0.25f;
                out[0] = 0.1f; out[1] = 0.3f; out[2] = 0.5f; out[3] = 0.7f;
            }, &modernSampled);
            check(modernOk && modernSampled && near4(&modernValues[modern.output], 0.1f, 0.3f, 0.5f, 0.7f),
                  "GLSL ES 3 fragment outputs and texture aliases used by Telltale shaders");
        }
        {
            Shader::Program program;
            const bool ok = program.compile(
                "#extension GL_OES_standard_derivatives : enable\n#extension GL_OES_standard_derivatives : warn\n"
                "#extension GL_OES_standard_derivatives : disable\n#extension all : disable\n#extension all : warn\n"
                "void main() { gl_FragColor = vec4(1.0); }\n", false);
            check(ok && fails("#extension GL_OES_foo : require\nvoid main() { gl_FragColor = vec4(1.0); }\n", "unsupported extension") &&
                      fails("#extension all : require\nvoid main() { gl_FragColor = vec4(1.0); }\n", "invalid extension") &&
                      fails("#extension all : enable\nvoid main() { gl_FragColor = vec4(1.0); }\n", "invalid extension"),
                  "GLSL #extension enable/warn/disable, and rejection of require");
        }
        check(runStage("#pragma optimize(off)\n#pragma STDGL invariant(all)\n// #error hidden by comment\n/* #error hidden by block */\nvoid main() { gl_FragColor = vec4(1.0); }",
                       false, [](auto&, auto&) {}, o) && near4(o, 1, 1, 1, 1),
              "GLSL #pragma is ignored and comments hide directives");
        check(runStage("#line 80\n#if __LINE__ == 80\nvoid main() { gl_FragColor = vec4(1.0); }\n#else\nvoid main() { gl_FragColor = vec4(0.0); }\n#endif",
                       false, [](auto&, auto&) {}, o) && near4(o, 1, 1, 1, 1),
              "GLSL #line sets __LINE__");
        check(runStage("#if 0\n#error hidden\n#endif\nvoid main() { gl_FragColor = vec4(1.0); }", false, [](auto&, auto&) {}, o) &&
                  near4(o, 1, 1, 1, 1) && fails("#error visible\nvoid main() { gl_FragColor = vec4(1.0); }", "visible"),
              "GLSL #error fires only in the active branch");
        check(runStage("#define K \\\n0.5\nvoid main() { gl_\\\nFragColor = vec4(K); }", false, [](auto&, auto&) {}, o) && near4(o, 0.5f, 0.5f, 0.5f, 0.5f),
              "GLSL line continuations splice directives and tokens");
        check(runStage("#\nvoid main() { gl_FragColor = vec4(1.0); }\n#\n", false, [](auto&, auto&) {}, o) && near4(o, 1, 1, 1, 1),
              "GLSL empty directive");
        {
            const std::string eof = "#if 1\nvoid main() { gl_FragColor = vec4(0.25); }\n#endif";
            const std::string tail = "void main() { gl_FragColor = vec4(1.0); }\n#define TAIL 1";
            check(eof.back() != '\n' && tail.back() != '\n' &&
                      runStage(eof, false, [](auto&, auto&) {}, o) && near4(o, 0.25f, 0.25f, 0.25f, 0.25f) &&
                      runStage(tail, false, [](auto&, auto&) {}, o) && near4(o, 1, 1, 1, 1),
                  "GLSL directives at end of input with no trailing newline");
        }
        check(runStage("#define CAT(a, b) a##b\nvoid main() { CAT(gl_Frag, Color) = vec4(0.375); }", false, [](auto&, auto&) {}, o) &&
                  near4(o, 0.375f, 0.375f, 0.375f, 0.375f),
              "GLSL ## token paste");
        check(fails("#define STR(x) #x\nvoid main() { gl_FragColor = vec4(STR(1)); }", "stringification"),
              "GLSL rejects # stringification");
        check(runStage("void main() { gl_FragColor = vec4(\n#if 0\n0.0\n#else\n0.5\n#endif\n); }", false, [](auto&, auto&) {}, o) &&
                  near4(o, 0.5f, 0.5f, 0.5f, 0.5f),
              "GLSL directives between the arguments of a call");
        check(runStage(
                  "precision highp float;\nprecision mediump int;\nprecision lowp sampler2D;\n"
                  "mediump vec2 tmp;\nfloat bare;\nconst highp float k = 0.25;\nuniform lowp vec4 u;\nvarying mediump vec4 v;\n"
                  "void main() { tmp = vec2(k, u.x); bare = tmp.y + v.x; gl_FragColor = vec4(bare, tmp.x, v.y, 1.0); }",
                  false,
                  [](Shader::Program& p, Shader::Values& v) {
                      if (auto* u = p.find("u")) { v[u->slot] = 0.5f; }
                      if (auto* var = p.find("v")) { v[var->slot] = 0.25f; v[var->slot + 1] = 0.125f; }
                  }, o) && near4(o, 0.75f, 0.25f, 0.125f, 1),
              "GLSL precision statements, precision-only globals, bare globals and const globals");
        check(runStage("const vec4 a = vec4(0.25, 0.0, 0.0, 0.0), b = a + vec4(0.0, 0.5, 0.0, 1.0);\nvoid main() { gl_FragColor = b; }",
                       false, [](auto&, auto&) {}, o) && near4(o, 0.25f, 0.5f, 0, 1),
              "GLSL multiple const declarators with initializers");
        check(runStage("uniform vec4 first, second;\nvoid main() { gl_FragColor = first + second; }", false,
                       [](Shader::Program& p, Shader::Values& v) {
                           if (auto* a = p.find("first")) { v[a->slot] = 0.25f; v[a->slot + 3] = 1; }
                           if (auto* b = p.find("second")) { v[b->slot + 1] = 0.5f; }
                       }, o) && near4(o, 0.25f, 0.5f, 0, 1),
              "GLSL multiple declarators on one line");
        {
            Shader::Program program;
            const bool compiled = program.compile(
                "invariant varying mediump vec4 a;\nvarying highp vec4 b;\nmediump varying vec4 c;\nhighp invariant varying vec4 d;\n"
                "invariant gl_Position;\nuniform mediump vec4 e;\nmediump uniform vec4 f;\nattribute highp vec4 g;\nhighp attribute vec4 h;\n"
                "const mediump float i = 1.0;\nmediump const float j = 2.0;\n"
                "void main() { a = e; b = f; c = vec4(i); d = vec4(j); gl_Position = g + h; }", true);
            Shader::Values values;
            if (compiled) { program.initialize(values); program.execute(values); }
            auto* cvar = compiled ? program.find("c") : nullptr;
            auto* dvar = compiled ? program.find("d") : nullptr;
            auto* avar = compiled ? program.find("a") : nullptr;
            auto* pos = compiled ? program.find("gl_Position") : nullptr;
            auto* bvar = compiled ? program.find("b") : nullptr;
            check(compiled && cvar && dvar && avar && pos && bvar && avar->invariant && dvar->invariant && pos->invariant && !bvar->invariant &&
                      values[cvar->slot] == 1 && values[dvar->slot] == 2 && values[program.output] == 0,
                  "GLSL qualifiers in every order, and invariant is stored on the variable");
        }
        check(runStage(
                  "struct Inner { float z; };\nstruct Light { vec3 color; float intensity; Inner extra; };\n"
                  "uniform Light lights[2];\nuniform struct { float x; } anon;\n"
                  "void main() {\n"
                  "  Light w = lights[1];\n"
                  "  gl_FragColor = vec4(w.color * lights[0].intensity, w.extra.z + anon.x);\n}",
                  false,
                  [](Shader::Program& p, Shader::Values& v) {
                      if (auto* lights = p.find("lights")) {
                          p.uniformMem[size_t(lights->slot + 3)] = 2;          // lights[0].intensity
                          p.uniformMem[size_t(lights->slot + 5)] = 0.25f;      // lights[1].color.r
                          p.uniformMem[size_t(lights->slot + 9)] = 0.125f;     // lights[1].extra.z
                      }
                      if (auto* anon = p.find("anon")) v[anon->slot] = 0.25f;
                  }, o) && near4(o, 0.5f, 0, 0, 0.375f),
              "GLSL struct definitions, struct arrays, member access and anonymous struct uniforms");
        check(runStage("struct S { float a; vec2 b; };\nvoid main() { S s = S(0.25, vec2(0.5, 0.75)); gl_FragColor = vec4(s.a, s.b, 1.0); }",
                       false, [](auto&, auto&) {}, o) && near4(o, 0.25f, 0.5f, 0.75f, 1),
              "GLSL struct constructors");
        check(runStage(
                  "uniform vec4 pal[2];\nuniform mat4 bones[2];\nvarying float vf[3];\n"
                  "void main() { gl_FragColor = pal[0] + pal[1] + vec4(vf[1], 0.0, 0.0, 0.0) + (bones[1] * vec4(1.0, 0.0, 0.0, 0.0)); }",
                  false,
                  [](Shader::Program& p, Shader::Values& v) {
                      if (auto* pal = p.find("pal")) {
                          p.uniformMem[size_t(pal->slot)] = 0.25f;
                          p.uniformMem[size_t(pal->slot + 5)] = 0.5f;
                      }
                      if (auto* vf = p.find("vf")) v[vf->slot + 1] = 0.125f;
                      if (auto* bones = p.find("bones")) p.uniformMem[size_t(bones->slot + 16 + 2)] = 0.125f; // column 0, row 2 of element 1
                  }, o) && near4(o, 0.375f, 0.5f, 0.125f, 0),
              "GLSL uniform arrays, varying arrays and matrix array elements");
        {
            bool sampled = false;
            Shader::Program program;
            const bool compiled = program.compile("uniform sampler2D tex[2];\nvoid main() { gl_FragColor = texture2D(tex[1], vec2(0.5, 0.25)); }", false);
            Shader::Values values;
            if (compiled) {
                program.initialize(values);
                if (auto* tex = program.find("tex")) program.uniformMem[size_t(tex->slot + 1)] = 4;
                program.execute(values, [](void* context, int unit, float u, float v, float* out) {
                    *static_cast<bool*>(context) = unit == 4 && u == 0.5f && v == 0.25f;
                    out[0] = 0.25f; out[1] = 0.5f; out[2] = 0.75f; out[3] = 1;
                }, &sampled);
            }
            check(compiled && sampled && near4(&values[program.output], 0.25f, 0.5f, 0.75f, 1),
                  "GLSL sampler arrays");
        }
        check(runStage("struct S { vec4 c[2]; float n; };\nuniform S u;\nvoid main() { gl_FragColor = u.c[0] + u.c[1] + vec4(u.n); }",
                       false,
                       [](Shader::Program& p, Shader::Values& v) {
                           if (auto* u = p.find("u")) {
                               v[u->slot] = 0.25f; v[u->slot + 5] = 0.5f; v[u->slot + 8] = 0.125f;
                           }
                       }, o) && near4(o, 0.375f, 0.625f, 0.125f, 0.125f),
              "GLSL arrays of struct fields");
        {
            Shader::Program vert, frag;
            const bool compiled = vert.compile(
                "struct Material { vec4 color; float intensity; };\nuniform Material u;\nvarying Material m;\n"
                "void main() { m = u; m.intensity = m.color.r; gl_Position = vec4(m.intensity); }", true) &&
                frag.compile("struct Material { vec4 color; float intensity; };\nvarying Material m;\nvoid main() { gl_FragColor = m.color; }", false);
            Shader::Values values;
            bool linked = false;
            if (compiled) {
                vert.initialize(values);
                if (auto* u = vert.find("u")) { values[u->slot] = 0.25f; values[u->slot + 1] = 0.5f; values[u->slot + 4] = 9; }
                vert.execute(values);
                auto* mv = vert.find("m");
                auto* mf = frag.find("m");
                linked = mv && mf && mv->type == mf->type && values[vert.output] == 0.25f && values[mv->slot + 1] == 0.5f && values[mv->slot + 4] == 0.25f;
            }
            check(linked, "GLSL struct varyings keep a shared layout across stages");
        }

        auto shows = [](const char* src, const char* snippet, bool vertex = false) {
            Shader::Program program;
            const bool bad = !program.compile(src, vertex) && program.error.find(snippet) != std::string::npos;
            if (!bad) std::printf("    expected '%s', got: %s\n", snippet, program.error.c_str());
            return bad;
        };
        check(runStage("#if 0 && 1\nconst float k = 0.0;\n#else\nconst float k = 1.0;\n#endif\n"
                       "#if 1 || 0\nvoid main() { gl_FragColor = vec4(k); }\n#else\nvoid main() { gl_FragColor = vec4(0.0); }\n#endif",
                       false, [](auto&, auto&) {}, o) && near4(o, 1, 1, 1, 1) &&
                  runStage("#if 1 || UNDEF\nvoid main() { gl_FragColor = vec4(1.0); }\n#endif", false, [](auto&, auto&) {}, o) &&
                  runStage("#if 0 && (1/0)\n#else\nvoid main() { gl_FragColor = vec4(1.0); }\n#endif", false, [](auto&, auto&) {}, o) &&
                  shows("#if 0 || UNDEF\nvoid main() { gl_FragColor = vec4(1.0); }\n#endif", "undefined preprocessor identifier") &&
                  shows("#if 1 && 1/0\nvoid main() { gl_FragColor = vec4(1.0); }\n#endif", "invalid preprocessor expression") &&
                  shows("#if UNDEF\n#endif\nvoid main() { gl_FragColor = vec4(1.0); }", "undefined preprocessor identifier"),
              "GLSL #if parses both &&/|| operands and only skips evaluation");
        check(runStage("#if (9223372036854775807 + 1) < 0 && (3037000500 * 3037000500) == -9223372036709301616\n"
                       "void main() { gl_FragColor = vec4(1.0); }\n#else\nvoid main() { gl_FragColor = vec4(0.0); }\n#endif",
                       false, [](auto&, auto&) {}, o) && near4(o, 1, 1, 1, 1),
              "GLSL #if add and multiply wrap instead of overflowing");
        check(runStage("#if (-8 >> 1) == -4 && (-1 >> 4) == -1\nvoid main() { gl_FragColor = vec4(1.0); }\n#else\nvoid main() { gl_FragColor = vec4(0.0); }\n#endif",
                       false, [](auto&, auto&) {}, o) && near4(o, 1, 1, 1, 1),
              "GLSL #if >> is an arithmetic shift");
        check(shows("#if 1 ? 2 : 3\nvoid main() { gl_FragColor = vec4(1.0); }\n#endif", "invalid preprocessor expression"),
              "GLSL #if rejects the conditional operator");
        {
            const std::string okIf = "#if " + std::string(256, '(') + "1" + std::string(256, ')') +
                                     "\nvoid main() { gl_FragColor = vec4(1.0); }\n#else\nvoid main() { gl_FragColor = vec4(0.0); }\n#endif";
            const std::string deepIf = "#if " + std::string(257, '(') + "1" + std::string(257, ')') +
                                       "\nvoid main() { gl_FragColor = vec4(1.0); }\n#endif";
            std::string deepStruct = "struct S { ";
            for (int n = 0; n < 300; ++n) deepStruct += "struct { ";
            deepStruct += "float x; ";
            for (int n = 0; n < 300; ++n) deepStruct += "} a; ";
            deepStruct += "}; void main() { S s; gl_FragColor = vec4(1.0); }";
            std::string deepBlock = "void main() { ";
            for (int n = 0; n < 300; ++n) deepBlock += "{";
            deepBlock += "gl_FragColor = vec4(1.0);";
            for (int n = 0; n < 300; ++n) deepBlock += "}";
            deepBlock += "}";
            std::string deepMacro;
            for (int n = 0; n < 300; ++n) deepMacro += "#define M" + std::to_string(n) + " M" + std::to_string(n + 1) + "\n";
            deepMacro += "#define M300 1.0\nvoid main() { gl_FragColor = vec4(M0); }";
            std::string longMacro = "#define A6\n";
            for (int level = 5; level >= 0; --level) {
                longMacro += "#define A" + std::to_string(level);
                for (int n = 0; n < 10; ++n) longMacro += " A" + std::to_string(level + 1);
                longMacro += "\n";
            }
            longMacro += "void main() { A0 gl_FragColor = vec4(1.0); }";
            std::string deepCall = "float f0() { return 1.0; }\n";
            for (int n = 1; n <= 256; ++n)
                deepCall += "float f" + std::to_string(n) + "() { return f" + std::to_string(n - 1) + "(); }\n";
            deepCall += "void main() { gl_FragColor = vec4(f256()); }";
            check(runStage(okIf, false, [](auto&, auto&) {}, o) && near4(o, 1, 1, 1, 1) &&
                      shows(deepIf.c_str(), "preprocessor expression too deep") &&
                      shows(deepStruct.c_str(), "struct nesting too deep") &&
                      shows(deepBlock.c_str(), "statement nesting too deep") &&
                      shows(deepMacro.c_str(), "macro expansion too deep") &&
                      shows(longMacro.c_str(), "macro expansion too long") &&
                      shows(deepCall.c_str(), "function call depth exceeded"),
                  "GLSL limits preprocessor, struct, statement, macro and call depth");
        }
        check(runStage("#line 10 7\n#if __LINE__ == 10 && __FILE__ == 7\nvoid main() { gl_FragColor = vec4(1.0); }\n#else\nvoid main() { gl_FragColor = vec4(0.0); }\n#endif",
                       false, [](auto&, auto&) {}, o) && near4(o, 1, 1, 1, 1) &&
                  runStage("#define LN 12\n#define FID 4\n#line LN FID\n#if __LINE__ == 12 && __FILE__ == 4\nvoid main() { gl_FragColor = vec4(0.5); }\n#else\nvoid main() { gl_FragColor = vec4(0.0); }\n#endif",
                           false, [](auto&, auto&) {}, o) && near4(o, 0.5f, 0.5f, 0.5f, 0.5f),
              "GLSL #line takes a line and an optional source number");
        check(runStage("#define PICK(a, b) b\nvoid main() { gl_FragColor = vec4(PICK(vec2(1.0, 2.0), 0.25)); }",
                       false, [](auto&, auto&) {}, o) && near4(o, 0.25f, 0.25f, 0.25f, 0.25f) &&
                  shows("#define PICK(a, b) b\nvoid main() { gl_FragColor = vec4(PICK(x[1, 2], 0.25)); }", "macro argument count"),
              "GLSL macro arguments group parentheses and split on commas in brackets");
        check(runStage("#if defined(GL_FRAGMENT_PRECISION_HIGH)\nvoid main() { gl_FragColor = vec4(1.0); }\n#else\nvoid main() { gl_FragColor = vec4(0.0); }\n#endif",
                       false, [](auto&, auto&) {}, o) && near4(o, 1, 1, 1, 1) &&
                  runStage("#if defined(GL_FRAGMENT_PRECISION_HIGH)\nvoid main() { gl_Position = vec4(1.0); }\n#else\nvoid main() { gl_Position = vec4(0.0); }\n#endif",
                           true, [](auto&, auto&) {}, o) && near4(o, 1, 1, 1, 1),
              "GLSL predefines GL_FRAGMENT_PRECISION_HIGH in both stages");
        check(shows("#define GL_FOO 1\nvoid main() { gl_FragColor = vec4(1.0); }", "reserved macro name") &&
                  shows("#define __HIDDEN 1\nvoid main() { gl_FragColor = vec4(1.0); }", "reserved macro name") &&
                  shows("#define FOO__BAR 1\nvoid main() { gl_FragColor = vec4(1.0); }", "reserved macro name") &&
                  shows("#undef FOO__BAR\nvoid main() { gl_FragColor = vec4(1.0); }", "reserved macro name") &&
                  shows("#undef GL_ES\nvoid main() { gl_FragColor = vec4(1.0); }", "reserved macro name") &&
                  shows("#undef __VERSION__\nvoid main() { gl_FragColor = vec4(1.0); }", "reserved macro name"),
              "GLSL rejects #define and #undef of GL_ names and names containing __");
        check(shows("#if 9223372036854775808\nvoid main() { gl_FragColor = vec4(1.0); }\n#endif", "invalid preprocessor expression") &&
                  shows("void main() { int x = 9223372036854775808; gl_FragColor = vec4(float(x)); }", "invalid numeric literal"),
              "GLSL rejects integer literals that do not fit");
        check(shows("float sin(float x) { return 1.0; }\nvoid main() { gl_FragColor = vec4(sin(0.0)); }", "cannot redefine built-in function") &&
                  shows("int sin(float x) { return 1; }\nvoid main() { gl_FragColor = vec4(1.0); }", "cannot redefine built-in function") &&
                  runStage("float sin(float a, float b) { return a + b; }\nvoid main() { gl_FragColor = vec4(sin(0.0), sin(0.25, 0.5), 0.0, 1.0); }",
                           false, [](auto&, auto&) {}, o) && near4(o, 0, 0.75f, 0, 1),
              "GLSL rejects an exact built-in signature and keeps other overloads");
        check(runStage("#define N 4\nconst int K = 2;\nconst int A = 2, B = A * 3;\nfloat a[N * 3];\nfloat b[(K + 1)];\nfloat c[B];\n"
                       "void main() { a[11] = 0.25; b[2] = 0.5; c[5] = 0.75; gl_FragColor = vec4(a[11], b[2], c[5], 1.0); }",
                       false, [](auto&, auto&) {}, o) && near4(o, 0.25f, 0.5f, 0.75f, 1) &&
                  shows("float a[2049];\nvoid main() { gl_FragColor = vec4(a[0]); }", "invalid array size") &&
                  shows("float a[0];\nvoid main() { gl_FragColor = vec4(1.0); }", "invalid array size") &&
                  shows("void main() { int n = 3; float a[n]; gl_FragColor = vec4(a[0]); }", "invalid array size"),
              "GLSL accepts constant-expression array sizes and rejects the rest");
        {
            Shader::Program sized;
            const char* bound = "uniform vec4 u[45];\nvoid main() { gl_FragColor = u[44]; }";
            const bool literal = sized.compile(bound, false);
            const auto* uniform = literal ? sized.find("u") : nullptr;
            if (!literal) std::printf("    vec4 u[45] rejected: %s\n", sized.error.c_str());
            Shader::Program nonconst;
            const char* expr = "void main() { int n = 3; float a[45 * n]; gl_FragColor = vec4(a[0]); }";
            const bool rejected = !nonconst.compile(expr, false) && nonconst.error.find("invalid array size") != std::string::npos &&
                                  nonconst.error.find("('45')") != std::string::npos;
            if (!rejected) std::printf("    expected token '45', got: %s\n", nonconst.error.c_str());
            check(literal && uniform && uniform->count == 45 && rejected,
                  "GLSL quotes a non-constant size's first token and accepts a literal bound of 45");
        }
        check(runStage(
                  "const float F = -4.9;\n"
                  "const int N = int(abs(-4.0));\n"
                  "const int A = abs(-2);\n"
                  "const int B = A + int(min(3.0, max(1.0, 4.0)));\n"
                  "const int C = int(floor(abs(F)));\n"
                  "const int D = clamp(int(ceil(3.2)), 1, sign(-1) * -8);\n"
                  "const int E = int(mod(9.0, 5.0));\n"
                  "const int G = int(vec4(4.8, 0.0, 0.0, 1.0).x);\n"
                  "const int H = int(abs(vec2(-4.0, 1.0)).x);\n"
                  "const int I = gl_MaxDrawBuffers + 3;\n"
                  "uniform vec4 u[45];\n"
                  "float a[int(4.0)];\nfloat b[N];\nfloat c[B];\nfloat d[C];\nfloat e[D];\nfloat f[E];\nfloat g[G];\nfloat h[H];\nfloat i[I];\n"
                  "void main() { a[3] = 0.25; b[3] = u[44].x; c[4] = 0.5; d[3] = 0.75; e[3] = 0.125; f[3] = 1.0; h[3] = 0.25; g[3] = h[3]; i[3] = 0.25;\n"
                  "gl_FragColor = vec4(a[3] + i[3], b[3], c[4] + e[3], d[3] + g[3]); }\n",
                  false,
                  [](Shader::Program& p, Shader::Values& v) {
                      if (auto* u = p.find("u")) p.uniformMem[size_t(u->slot + 44 * 4)] = 0.5f;
                  },
                  o) && near4(o, 0.5f, 0.5f, 0.625f, 1.0f) &&
                  shows("float a[abs(-4.0)];\nvoid main() { gl_FragColor = vec4(a[0]); }", "invalid array size") &&
                  shows("float a[4.0];\nvoid main() { gl_FragColor = vec4(1.0); }", "invalid array size") &&
                  shows("float f() { return 4.0; }\nfloat a[int(f())];\nvoid main() { gl_FragColor = vec4(a[0]); }", "invalid array size") &&
                  shows("uniform int u;\nfloat a[u];\nvoid main() { gl_FragColor = vec4(a[0]); }", "invalid array size"),
              "GLSL folds constructors, constant builtins and chained consts into array sizes");
        check(shows("float a[int(1e20)];\nvoid main() { gl_FragColor = vec4(a[0]); }", "invalid array size") &&
                  shows("float a[int(-1e20)];\nvoid main() { gl_FragColor = vec4(a[0]); }", "invalid array size") &&
                  shows("float a[9223372036854775807];\nvoid main() { gl_FragColor = vec4(a[0]); }", "invalid array size") &&
                  shows("float a[int(9223372036854775807.0)];\nvoid main() { gl_FragColor = vec4(a[0]); }", "invalid array size") &&
                  shows("float a[int(vec4(1.0, 2.0, 3.0, 4.0)[int(1e20)])];\nvoid main() { gl_FragColor = vec4(a[0]); }", "invalid array size") &&
                  shows("float a[int(vec4(1.0)[int(-1e20)])];\nvoid main() { gl_FragColor = vec4(a[0]); }", "invalid array size") &&
                  shows("const int i = int(mat2(1.0)[int(2147483648.0)].x);\nfloat a[i];\nvoid main() { gl_FragColor = vec4(a[0]); }", "invalid array size") &&
                  shows("struct C { float x[2048]; };\nstruct B { C c[2048]; };\nstruct A { B b[2048]; };\nA a;\n"
                        "void main() { gl_FragColor = vec4(a.b[0].c[0].x[0]); }", "shader register limit exceeded") &&
                  shows("struct C { float x[2048]; };\nstruct B { C c[2048]; };\nB b[512];\n"
                        "void main() { gl_FragColor = vec4(b[0].c[0].x[0]); }", "shader register limit exceeded") &&
                  shows("struct C { float x[2048]; float extra; };\nvoid main() { gl_FragColor = vec4(1.0); }", "shader register limit exceeded") &&
                  runStage("struct C { float x[2048]; };\nvoid main() { gl_FragColor = vec4(1.0); }", false, [](auto&, auto&) {}, o) &&
                  near4(o, 1, 1, 1, 1) &&
                  runStage("const int N = int(vec4(4.0, 1.0, 2.0, 3.0)[0]);\nfloat a[N];\nvoid main() { a[3] = 0.25; gl_FragColor = vec4(a[3]); }",
                           false, [](auto&, auto&) {}, o) && near4(o, 0.25f, 0.25f, 0.25f, 0.25f) &&
                  runStage("struct S { float x; float y; };\nS s;\nvoid main() { s.x = 0.25; s.y = 0.5; gl_FragColor = vec4(s.x, s.y, 0.0, 1.0); }",
                           false, [](auto&, auto&) {}, o) && near4(o, 0.25f, 0.5f, 0, 1),
              "GLSL rejects out-of-range constant sizes and struct layouts that overflow the register file");
        // Integer literals are stored as float32. 2147483647 rounds to 2147483648,
        // which does not fit in int. The compile must reject that index instead of
        // casting first (undefined, and UBSan aborts).
        check(shows("float a[4];\nvoid main() { gl_FragColor = vec4(a[2147483647]); }", "array index must be an in-range literal") &&
                  shows("float a[4];\nvoid main() { gl_FragColor = vec4(a[2147483648]); }", "array index must be an in-range literal") &&
                  shows("float a[4];\nvoid main() { gl_FragColor = vec4(a[9223372036854775807]); }", "array index must be an in-range literal") &&
                  shows("void main() { vec4 v = vec4(1.0); gl_FragColor = vec4(v[2147483647]); }", "array index must be an in-range literal") &&
                  shows("void main() { ivec4 v = ivec4(1, 2, 3, 4); gl_FragColor = vec4(float(v[2147483647])); }", "array index must be an in-range literal") &&
                  shows("void main() { mat2 m = mat2(1.0); gl_FragColor = vec4(m[2147483647]); }", "array index must be an in-range literal") &&
                  shows("struct S { float x[4]; };\nvoid main() { S s; gl_FragColor = vec4(s.x[2147483647]); }", "array index must be an in-range literal") &&
                  shows("float a[4];\nvoid main() { gl_FragColor = vec4(a[4]); }", "array index must be an in-range literal") &&
                  shows("void main() { vec4 v = vec4(1.0); gl_FragColor = vec4(v[5]); }", "array index must be an in-range literal") &&
                  shows("void main() { mat2 m = mat2(1.0); gl_FragColor = m[2]; }", "array index must be an in-range literal") &&
                  runStage("float a[4];\nvoid main() { a[0] = 0.25; a[3] = 0.5; gl_FragColor = vec4(a[0], a[3], 0.0, 1.0); }",
                           false, [](auto&, auto&) {}, o) && near4(o, 0.25f, 0.5f, 0, 1) &&
                  runStage("void main() { vec4 v = vec4(1.0, 2.0, 3.0, 4.0); mat2 m = mat2(1.0, 2.0, 3.0, 4.0); gl_FragColor = vec4(v[2], m[1].x, m[1].y, 1.0); }",
                           false, [](auto&, auto&) {}, o) && near4(o, 3, 3, 4, 1),
              "GLSL rejects out-of-range constant indexes, including float32 literals that do not fit in int");
        // Literal indexes at the edges of the range check. 16777217 is not a float32;
        // it rounds to 16777216, still far past a[3]. For vec4 the last valid index
        // is 3 and the first invalid one is 4; for ivec2 it is 1 and 2. With
        // v = (0.25, 0.5, 0.75, 1.0), v[3] = 1.0 and v[0] = 0.25.
        check(shows("float a[4];\nvoid main() { gl_FragColor = vec4(a[16777217]); }", "array index must be an in-range literal") &&
                  shows("void main() { vec4 v = vec4(1.0); gl_FragColor = vec4(v[4]); }", "array index must be an in-range literal") &&
                  shows("void main() { ivec2 v = ivec2(1, 2); gl_FragColor = vec4(float(v[2])); }", "array index must be an in-range literal") &&
                  runStage("void main() { vec4 v = vec4(0.25, 0.5, 0.75, 1.0); gl_FragColor = vec4(v[3], v[0], 0.0, 1.0); }",
                           false, [](auto&, auto&) {}, o) && near4(o, 1.0f, 0.25f, 0, 1),
              "GLSL constant index range check accepts the last element and rejects the next");
        {
            Shader::Program warned;
            const bool ok = warned.compile(
                "#extension GL_OES_foo : warn\n#extension GL_OES_standard_derivatives : warn\n"
                "void main() { gl_FragColor = vec4(dFdx(1.0)); }\n",
                false);
            if (!ok) std::printf("    extension warn failed: %s\n", warned.error.c_str());
            const bool saw = warned.warnings.find("warning") != std::string::npos &&
                             warned.warnings.find("GL_OES_foo") != std::string::npos &&
                             warned.warnings.find("GL_OES_standard_derivatives") != std::string::npos;
            if (!saw) std::printf("    expected extension warnings, got: %s\n", warned.warnings.c_str());
            bool ran = false;
            if (ok) {
                Shader::Values values;
                warned.initialize(values);
                warned.execute(values);
                for (int c = 0; c < 4; ++c) o[c] = values[warned.output + c];
                ran = near4(o, 0, 0, 0, 0);
            }
            check(ok && ran && saw, "GLSL #extension : warn warns and still enables a supported extension");
        }
        {
            // Baseline instruction counts. Shaders that fit must keep this stream:
            // uniform arrays change where the data lives, not how many ops a constant
            // index compiles to, and a shader with no uniform array stays on the
            // direct register path.
            Shader::Program simple, pal;
            const bool simpleOk = simple.compile("void main() { gl_FragColor = vec4(0.25); }", false);
            const bool palOk = pal.compile("uniform vec4 pal[2];\nvoid main() { gl_FragColor = pal[0] + pal[1]; }", false);
            if (!simpleOk || !palOk)
                std::printf("    baseline compile failed: %s | %s\n", simple.error.c_str(), pal.error.c_str());
            check(simpleOk && palOk && simple.code.size() == 21 && pal.code.size() == 22 && simple.uniformMem.empty() &&
                      simple.spillCount == 0,
                  "GLSL shaders that fit keep their instruction count");
        }
        {
            const char* skin =
                "uniform mat4 bones[45];\n"
                "uniform vec4 idx;\n"
                "uniform vec4 wgt;\n"
                "void main() {\n"
                "  vec4 p = vec4(0.0);\n"
                "  p = p + (bones[int(idx.x)] * vec4(1.0, 0.0, 0.0, 1.0)) * wgt.x;\n"
                "  p = p + (bones[int(idx.y)] * vec4(1.0, 0.0, 0.0, 1.0)) * wgt.y;\n"
                "  gl_FragColor = p;\n"
                "}\n";
            Shader::Program program;
            const bool compiled = program.compile(skin, false);
            if (!compiled) std::printf("    skinning compile failed: %s\n", program.error.c_str());
            auto* bones = compiled ? program.find("bones") : nullptr;
            auto* idx = compiled ? program.find("idx") : nullptr;
            auto* wgt = compiled ? program.find("wgt") : nullptr;
            bool ran = false;
            if (compiled && bones && bones->memory && idx && wgt && int(program.uniformMem.size()) >= bones->slot + 45 * 16) {
                auto putMat = [&](int element, int col, int row, float value) {
                    program.uniformMem[size_t(bones->slot + element * 16 + col * 4 + row)] = value;
                };
                // bones[3] column0 = (0.5, 0, 0, 0), column3 = (0, 0, 0, 1) so M * (1,0,0,1) = (0.5, 0, 0, 1)
                putMat(3, 0, 0, 0.5f); putMat(3, 1, 1, 1.f); putMat(3, 2, 2, 1.f); putMat(3, 3, 3, 1.f);
                // bones[10] column0 = (0, 0.25, 0, 0), column3 = (0, 0, 0, 1) so M * (1,0,0,1) = (0, 0.25, 0, 1)
                putMat(10, 0, 1, 0.25f); putMat(10, 1, 1, 1.f); putMat(10, 2, 2, 1.f); putMat(10, 3, 3, 1.f);
                Shader::Values values;
                program.initialize(values);
                values[idx->slot] = 3.f; values[idx->slot + 1] = 10.f;
                values[wgt->slot] = 0.5f; values[wgt->slot + 1] = 0.5f;
                ran = program.execute(values);
                for (int c = 0; c < 4; ++c) o[c] = values[program.output + c];
            }
            // Four select-chains over mat4[45] are thousands of instructions; a load per
            // component stays well under that.
            const bool compact = compiled && program.code.size() < 800;
            if (compiled && !compact) std::printf("    skinning emitted %zu instructions\n", program.code.size());
            if (ran && !near4(o, 0.25f, 0.125f, 0.f, 1.f))
                std::printf("    skinning color %g %g %g %g\n", o[0], o[1], o[2], o[3]);
            check(compiled && bones && bones->memory && bones->count == 45 && compact && ran && near4(o, 0.25f, 0.125f, 0.f, 1.f),
                  "GLSL skinning indexes a 45-element mat4 uniform array without blowing the register file");
        }
        {
            std::string src = "void main() { float x = 0.0;\n";
            for (int n = 0; n < 1500; ++n) src += "x = x + 1.0;\n";
            src += "gl_FragColor = vec4(x); }";
            Shader::Program program;
            const bool compiled = program.compile(src, false);
            if (!compiled) std::printf("    recycled adds failed: %s\n", program.error.c_str());
            bool ran = false;
            if (compiled) {
                Shader::Values values;
                program.initialize(values);
                ran = program.execute(values) && program.spillCount == 0;
                for (int c = 0; c < 4; ++c) o[c] = values[program.output + c];
            }
            check(compiled && ran && near4(o, 1500.f, 1500.f, 1500.f, 1500.f),
                  "GLSL recycles temps so a long straight-line shader stays inside the register file");
        }
        {
            const char* src =
                "void main() {\n"
                "  float a[1800];\n"
                "  float b[1800];\n"
                "  a[0] = 0.25;\n"
                "  b[1799] = 0.5;\n"
                "  int i = 4;\n"
                "  b[i] = 0.125;\n"
                "  gl_FragColor = vec4(a[0] + b[1799], b[i], 0.0, 1.0);\n"
                "}\n";
            Shader::Program program;
            const bool compiled = program.compile(src, false);
            if (!compiled) std::printf("    spilled locals failed: %s\n", program.error.c_str());
            bool ran = false;
            if (compiled) {
                Shader::Values values;
                program.initialize(values);
                ran = program.execute(values) && program.spillCount > 0;
                for (int c = 0; c < 4; ++c) o[c] = values[program.output + c];
            }
            if (compiled && !ran)
                std::printf("    spill count %d color %g %g %g %g\n", program.spillCount, o[0], o[1], o[2], o[3]);
            check(compiled && ran && near4(o, 0.75f, 0.125f, 0.f, 1.f),
                  "GLSL spills local arrays that do not fit beside each other in the register file");
        }
        {
            const char* src =
                "uniform vec4 pal[2];\n"
                "void main() {\n"
                "  vec4 huge = pal[int(1e20)];\n"
                "  vec4 neg = pal[int(-1e20)];\n"
                "  vec4 wide = pal[int(2147483648.0)];\n"
                "  vec4 second = pal[1];\n"
                "  float a[4];\n"
                "  a[0] = 0.25; a[1] = 0.5; a[2] = 0.75; a[3] = 1.0;\n"
                "  a[int(1e20)] = 0.125;\n"
                "  a[int(-1e20)] = 0.125;\n"
                "  gl_FragColor = vec4(huge.x, neg.y + wide.z, second.x, a[0] + a[1]);\n"
                "}\n";
            Shader::Program program;
            const bool compiled = program.compile(src, false);
            if (!compiled) std::printf("    out-of-range index failed: %s\n", program.error.c_str());
            bool ran = false;
            if (compiled) {
                auto* pal = program.find("pal");
                if (pal && pal->memory && int(program.uniformMem.size()) >= pal->slot + 8) {
                    program.uniformMem[size_t(pal->slot + 0)] = 0.25f;
                    program.uniformMem[size_t(pal->slot + 1)] = 0.5f;
                    program.uniformMem[size_t(pal->slot + 2)] = 0.75f;
                    program.uniformMem[size_t(pal->slot + 3)] = 1.f;
                    program.uniformMem[size_t(pal->slot + 4)] = 0.125f;
                }
                Shader::Values values;
                program.initialize(values);
                ran = program.execute(values);
                for (int c = 0; c < 4; ++c) o[c] = values[program.output + c];
            }
            if (compiled && ran && !near4(o, 0.25f, 1.25f, 0.125f, 0.75f))
                std::printf("    out-of-range index color %g %g %g %g\n", o[0], o[1], o[2], o[3]);
            check(compiled && ran && near4(o, 0.25f, 1.25f, 0.125f, 0.75f),
                  "GLSL out-of-range dynamic indexes read element 0 and write nothing");
        }
        check(runStage("#extension GL_OES_standard_derivatives : require\nvoid main() { gl_FragColor = vec4(dFdx(1.0)); }",
                       false, [](auto&, auto&) {}, o) && near4(o, 0, 0, 0, 0) &&
                  shows("void main() { gl_FragColor = vec4(dFdx(1.0)); }", "unknown variable/function") &&
                  shows("#extension GL_OES_standard_derivatives : require\nvoid main() { gl_Position = vec4(dFdx(1.0)); }",
                        "not available in a vertex shader", true) &&
                  shows("#extension GL_OES_standard_derivatives : disable\nvoid main() { gl_FragColor = vec4(dFdx(1.0)); }",
                        "unknown variable/function"),
              "GLSL enables dFdx only with GL_OES_standard_derivatives");
        {
            Shader::Program looping;
            const bool compiled = looping.compile("void main() { gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); for(;;) {} }", false);
            Shader::Values vals; looping.initialize(vals);
            int64_t budget = 1000;
            const bool kept = compiled && looping.execute(vals, nullptr, nullptr, &budget);
            check(compiled && !kept && budget < 0, "GLSL execute stops when the shared instruction budget is exhausted");
        }
        check(shows("void main() {\n#pragma optimize(off)\ngl_FragColor = vec4(1.0);\n}", "pragma optimize inside function") &&
                  shows("void main() {\n#pragma debug(on)\ngl_FragColor = vec4(1.0);\n}", "pragma debug inside function") &&
                  runStage("void main() {\n#if 0\n#pragma optimize(off)\n#endif\ngl_FragColor = vec4(1.0);\n}", false, [](auto&, auto&) {}, o) &&
                  near4(o, 1, 1, 1, 1),
              "GLSL rejects optimize and debug pragmas inside functions");
        {
            const std::string bom = std::string("\xEF\xBB\xBF") + "#version 100\nvoid main() { gl_FragColor = vec4(1.0); }";
            const std::string crlf = "#version 100\r\nvoid main() { gl_FragColor = vec4(1.0); }";
            const std::string blanks = "\n\n  #version 100\nvoid main() { gl_FragColor = vec4(1.0); }";
            const std::string commented = "// lead\n/* side */ #version 100 // tail\nvoid main() { gl_FragColor = vec4(1.0); }";
            const std::string nbsp = std::string("#version\xC2\xA0") + "100\nvoid main() { gl_FragColor = vec4(1.0); }";
            Shader::Program bad300, badEs;
            bad300.compile("#version 300\nvoid main() { gl_FragColor = vec4(1.0); }", false);
            badEs.compile("#version 100 es\nvoid main() { gl_FragColor = vec4(1.0); }", false);
            check(runStage(bom, false, [](auto&, auto&) {}, o) && runStage(crlf, false, [](auto&, auto&) {}, o) &&
                      runStage(blanks, false, [](auto&, auto&) {}, o) && runStage(commented, false, [](auto&, auto&) {}, o) &&
                      runStage(nbsp, false, [](auto&, auto&) {}, o) && near4(o, 1, 1, 1, 1) &&
                      bad300.error.find("unsupported version '300'") != std::string::npos &&
                      badEs.error.find("unsupported version '100 es'") != std::string::npos,
                  "GLSL accepts #version 100 with a BOM, CRLF, comments and blank lines, and logs other versions");
        }
        check(runStage("float shade(float x) { return x + 0.25; }\nvoid main() { gl_FragColor = vec4(shade(0.5)); }",
                       false, [](auto&, auto&) {}, o) && near4(o, 0.75f, 0.75f, 0.75f, 0.75f),
              "GLSL user-defined functions");
        check(runStage("float shade(float a, float b);\nvoid main() { gl_FragColor = vec4(shade(0.25, 0.5)); }\nfloat shade(float x, float y) { return x + y; }",
                       false, [](auto&, auto&) {}, o) && near4(o, 0.75f, 0.75f, 0.75f, 0.75f) &&
                  shows("float shade(float a);\nfloat shade(out float a) { return a; }\nvoid main() { gl_FragColor = vec4(1.0); }", "function prototype mismatch") &&
                  shows("float shade(float a) { return a; }\nfloat shade(float a) { return a; }\nvoid main() { gl_FragColor = vec4(1.0); }", "duplicate function definition"),
              "GLSL prototypes, later definitions, and mismatched or duplicate declarations");
        check(runStage("void touch(in float a) { a = 0.0; }\nvoid bump(inout float a, out float b) { a = 0.75; b = 0.75; }\n"
                       "void main() { float x = 1.0; float y = 0.0; touch(x); bump(x, y); gl_FragColor = vec4(x, y, 0.0, 1.0); }",
                       false, [](auto&, auto&) {}, o) && near4(o, 0.75f, 0.75f, 0, 1),
              "GLSL in parameters are copies and inout/out copy back");
        check(shows("float self(float x) { return self(x); }\nvoid main() { gl_FragColor = vec4(self(1.0)); }", "recursion is not allowed") &&
                  shows("float other(float x);\nfloat self(float x) { return other(x); }\nfloat other(float x) { return self(x); }\nvoid main() { gl_FragColor = vec4(self(1.0)); }",
                        "recursion is not allowed"),
              "GLSL rejects direct and mutual recursion");
        check(runStage("void main() { float x = 0.25; if (x > 0.0) x = 0.75; else x = 0.0; gl_FragColor = vec4(x); }",
                       false, [](auto&, auto&) {}, o) && near4(o, 0.75f, 0.75f, 0.75f, 0.75f) &&
                  runStage("void main() { float x = 0.0; if (false) x = 1.0; gl_FragColor = vec4(x); }", false, [](auto&, auto&) {}, o) &&
                  near4(o, 0, 0, 0, 0),
              "GLSL if/else");
        check(runStage("void main() { float s = 0.0; for (int i = 0; i < 4; i++) { if (i == 2) break; s = s + 0.25; } gl_FragColor = vec4(s); }",
                       false, [](auto&, auto&) {}, o) && near4(o, 0.5f, 0.5f, 0.5f, 0.5f),
              "GLSL for and break");
        check(runStage("void main() { float s = 0.0; int i = 0; while (i < 4) { i = i + 1; if (i == 2) continue; s = s + 0.25; } gl_FragColor = vec4(s); }",
                       false, [](auto&, auto&) {}, o) && near4(o, 0.75f, 0.75f, 0.75f, 0.75f),
              "GLSL while and continue");
        check(runStage("void main() { float s = 0.0; int i = 0; do { i = i + 1; s = s + 0.25; } while (i < 2); gl_FragColor = vec4(s); }",
                       false, [](auto&, auto&) {}, o) && near4(o, 0.5f, 0.5f, 0.5f, 0.5f),
              "GLSL do-while");
        check(runStage("void main() { for (;;) { break; } gl_FragColor = vec4(1.0); }", false, [](auto&, auto&) {}, o) && near4(o, 1, 1, 1, 1),
              "GLSL for(;;) compiles and break leaves it");
        {
            Shader::Program fragment, vertex;
            const bool compiled = fragment.compile("void main() { gl_FragColor = vec4(1.0); discard; }", false);
            Shader::Values values;
            bool kept = true;
            if (compiled) { fragment.initialize(values); kept = fragment.execute(values); }
            check(compiled && !kept && shows("void main() { discard; gl_Position = vec4(1.0); }", "discard is not allowed in a vertex shader", true),
                  "GLSL discard drops the fragment and is rejected in a vertex shader");
            (void)vertex;
        }
        check(runStage("float side = 0.0;\nfloat mark() { side = 1.0; return 1.0; }\n"
                       "void main() { float x = true ? 0.25 : mark(); bool y = false && mark() > 0.0; gl_FragColor = vec4(x, side, y ? 1.0 : 0.0, 1.0); }",
                       false, [](auto&, auto&) {}, o) && near4(o, 0.25f, 0, 0, 1),
              "GLSL && and ?: skip the untaken arm");
        check(runStage("float x = 0.25;\nvoid main() { float x = 0.75; { float x = 0.5; } gl_FragColor = vec4(x); }",
                       false, [](auto&, auto&) {}, o) && near4(o, 0.75f, 0.75f, 0.75f, 0.75f),
              "GLSL local declarations can shadow");
        check(runStage("void main() { vec4 v = vec4(1, 2, 3, 4); int t = int(1.9); int n = int(-1.9); mat2 m = mat2(2.0); gl_FragColor = vec4(v.x, float(t), float(n), m[1].y); }",
                       false, [](auto&, auto&) {}, o) && near4(o, 1, 1, -1, 2) &&
                  runStage("void main() { vec4 a = vec4(1.0, 2.0, 3.0, 4.0); vec4 b = vec4(4.0, 3.0, 2.0, 1.0); gl_FragColor = a + b * vec4(0.5) - vec4(2.0); }",
                           false, [](auto&, auto&) {}, o) && near4(o, 1, 1.5f, 2, 2.5f) &&
                  runStage("void main() { mat2 A = mat2(1.0, 2.0, 3.0, 4.0); mat2 B = mat2(1.0, 0.0, 0.0, 1.0); gl_FragColor = vec4((A * B)[0], (A * 2.0)[1]); }",
                           false, [](auto&, auto&) {}, o) && near4(o, 1, 2, 6, 8) &&
                  runStage("void main() { int a = 7 / 2; int b = -7 / 2; gl_FragColor = vec4(float(a), float(b), 0.0, 1.0); }",
                           false, [](auto&, auto&) {}, o) && near4(o, 3, -3, 0, 1),
              "GLSL constructors, vector and matrix operators, and integer division");
        auto nearLoose = [](const float* g, float r, float gch, float b, float a) {
            return std::abs(g[0] - r) < 1e-4f && std::abs(g[1] - gch) < 1e-4f && std::abs(g[2] - b) < 1e-4f && std::abs(g[3] - a) < 1e-4f;
        };
        check(runStage("void main() { gl_FragColor = vec4(sin(0.0), cos(0.0), tan(0.0), exp(0.0)); }", false, [](auto&, auto&) {}, o) && nearLoose(o, 0, 1, 0, 1) &&
                  runStage("void main() { gl_FragColor = vec4(asin(0.0), acos(1.0), atan(0.0), atan(1.0, 1.0)); }", false, [](auto&, auto&) {}, o) &&
                  nearLoose(o, 0, 0, 0, 0.78539816f) &&
                  runStage("void main() { gl_FragColor = vec4(radians(180.0) / 3.14159265, degrees(radians(180.0)) / 180.0, pow(2.0, 3.0) / 8.0, log(1.0)); }",
                           false, [](auto&, auto&) {}, o) && nearLoose(o, 1, 1, 1, 0) &&
                  runStage("void main() { gl_FragColor = vec4(exp2(3.0), log2(8.0), sqrt(4.0), inversesqrt(4.0)); }", false, [](auto&, auto&) {}, o) &&
                  nearLoose(o, 8, 3, 2, 0.5f),
              "GLSL angle, trigonometric and exponential built-ins");
        check(runStage("void main() { gl_FragColor = vec4(abs(-1.5), sign(-2.0), sign(0.0), sign(3.0)); }", false, [](auto&, auto&) {}, o) && near4(o, 1.5f, -1, 0, 1) &&
                  runStage("void main() { gl_FragColor = vec4(floor(1.7), floor(-1.2), ceil(1.2), ceil(-1.2)); }", false, [](auto&, auto&) {}, o) && near4(o, 1, -2, 2, -1) &&
                  runStage("void main() { gl_FragColor = vec4(fract(1.75), mod(5.5, 2.0), min(0.25, 0.75), max(0.25, 0.75)); }", false, [](auto&, auto&) {}, o) &&
                  near4(o, 0.75f, 1.5f, 0.25f, 0.75f) &&
                  runStage("void main() { gl_FragColor = vec4(clamp(1.5, 0.0, 1.0), mix(0.0, 10.0, 0.25), step(0.5, 0.25), step(0.5, 0.5)); }",
                           false, [](auto&, auto&) {}, o) && near4(o, 1, 2.5f, 0, 1) &&
                  runStage("void main() { gl_FragColor = vec4(smoothstep(0.0, 1.0, 0.5)); }", false, [](auto&, auto&) {}, o) && near4(o, 0.5f, 0.5f, 0.5f, 0.5f),
              "GLSL common built-ins");
        check(runStage("void main() { gl_FragColor = vec4(length(vec2(3.0, 4.0)), distance(vec2(0.0), vec2(0.0, 2.0)), dot(vec2(1.0, 2.0), vec2(3.0, 4.0)), 1.0); }",
                       false, [](auto&, auto&) {}, o) && near4(o, 5, 2, 11, 1) &&
                  runStage("void main() { vec3 c = cross(vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0)); vec2 n = normalize(vec2(0.0, 3.0)); gl_FragColor = vec4(c.z, n.y, 0.0, 1.0); }",
                           false, [](auto&, auto&) {}, o) && near4(o, 1, 1, 0, 1) &&
                  runStage("void main() { vec2 f = faceforward(vec2(0.0, 1.0), vec2(0.0, -1.0), vec2(0.0, 1.0)); vec2 r = reflect(vec2(0.0, -1.0), vec2(0.0, 1.0)); gl_FragColor = vec4(f.y, r.y, 0.0, 1.0); }",
                           false, [](auto&, auto&) {}, o) && near4(o, 1, 1, 0, 1) &&
                  runStage("void main() { vec2 r = refract(vec2(0.0, -1.0), vec2(0.0, 1.0), 1.0); mat2 m = matrixCompMult(mat2(1.0, 2.0, 3.0, 4.0), mat2(2.0, 0.0, 0.0, 3.0)); gl_FragColor = vec4(r.y, m[0].x, m[1].y, 1.0); }",
                           false, [](auto&, auto&) {}, o) && near4(o, -1, 2, 12, 1),
              "GLSL geometric built-ins and matrixCompMult");
        check(runStage("void main() { bvec2 c = lessThan(vec2(1.0, 3.0), vec2(2.0, 2.0)); bvec2 n = not(bvec2(true, false)); gl_FragColor = vec4(float(c.x), float(c.y), float(any(c)), float(all(not(n)))); }",
                       false, [](auto&, auto&) {}, o) && near4(o, 1, 0, 1, 0) &&
                  runStage("void main() { gl_FragColor = vec4(float(gl_MaxVertexAttribs == 16), float(gl_MaxVertexUniformVectors == 128), float(gl_MaxVaryingVectors == 8), float(gl_MaxDrawBuffers == 1)); }",
                           false, [](auto&, auto&) {}, o) && near4(o, 1, 1, 1, 1),
              "GLSL relational built-ins and implementation constants");
        check(runStage("void main() { gl_FragColor = vec4(gl_FragCoord.x, gl_FrontFacing ? 1.0 : 0.0, gl_PointCoord.x, gl_DepthRange.diff); }", false,
                       [](Shader::Program& p, Shader::Values& v) {
                           if (auto* c = p.find("gl_FragCoord")) v[c->slot] = 0.25f;
                           if (auto* f = p.find("gl_FrontFacing")) v[f->slot] = 1;
                           if (auto* pc = p.find("gl_PointCoord")) v[pc->slot] = 0.75f;
                           if (auto* d = p.find("gl_DepthRange")) { v[d->slot + 1] = 0.125f; v[d->slot + 2] = 0.125f; }
                       }, o) && near4(o, 0.25f, 1, 0.75f, 0.125f),
              "GLSL gl_FragCoord, gl_FrontFacing, gl_PointCoord and gl_DepthRange");
        check(runStage("void main() { gl_PointSize = 4.0; gl_Position = vec4(gl_PointSize / 4.0); }", true,
                       [](Shader::Program& p, Shader::Values& v) {
                           if (auto* ps = p.find("gl_PointSize")) v[ps->slot] = v[ps->slot];
                       }, o) && near4(o, 1, 1, 1, 1),
              "GLSL gl_PointSize");
        check(runStage("void main() { gl_FragData[0] = vec4(0.25, 0.5, 0.75, 1.0); }", false, [](auto&, auto&) {}, o) && near4(o, 0.25f, 0.5f, 0.75f, 1) &&
                  shows("void main() { gl_FragColor = vec4(1.0); gl_FragData[0] = vec4(0.0); }", "cannot write both"),
              "GLSL gl_FragData aliases the fragment output");
        check(runStage("void main() { float a[4]; a[0] = 0.1; a[1] = 0.2; a[2] = 0.3; a[3] = 0.4; float s = 0.0; for (int i = 0; i < 4; i++) s = s + a[i]; gl_FragColor = vec4(s); }",
                       false, [](auto&, auto&) {}, o) && near4(o, 1, 1, 1, 1),
              "GLSL dynamic array indexing");
        {
            auto sampleTo = [](void* context, int unit, float u, float v, float* out) {
                auto* hit = static_cast<float*>(context);
                hit[0] = float(unit); hit[1] = u; hit[2] = v; hit[3] = 1;
                out[0] = u; out[1] = v; out[2] = 0.5f; out[3] = 1;
            };
            float hit[4] = {};
            Shader::Program program;
            bool proj = program.compile("uniform sampler2D image;\nvoid main() { gl_FragColor = texture2DProj(image, vec3(0.5, 0.25, 2.0)); }", false);
            Shader::Values values;
            if (proj) {
                program.initialize(values);
                if (auto* s = program.find("image")) values[s->slot] = 2;
                program.execute(values, sampleTo, hit);
            }
            bool projOk = proj && hit[0] == 2 && std::abs(hit[1] - 0.25f) < 1e-5f && std::abs(hit[2] - 0.125f) < 1e-5f &&
                          near4(&values[program.output], 0.25f, 0.125f, 0.5f, 1);
            Shader::Program vert;
            bool lod = vert.compile("uniform sampler2D image;\nvoid main() { gl_Position = texture2DLod(image, vec2(0.5, 0.25), 0.0); }", true);
            bool lodOk = false;
            if (lod) {
                vert.initialize(values);
                if (auto* s = vert.find("image")) values[s->slot] = 5;
                vert.execute(values, sampleTo, hit);
                lodOk = hit[0] == 5 && std::abs(hit[1] - 0.5f) < 1e-5f && std::abs(hit[2] - 0.25f) < 1e-5f &&
                        near4(&values[vert.output], 0.5f, 0.25f, 0.5f, 1);
            }
            Shader::Program cube;
            bool cubeOk = cube.compile("uniform samplerCube image;\nvoid main() { gl_FragColor = textureCube(image, vec3(1.0, 0.5, -0.5)); }", false);
            if (cubeOk) {
                cube.initialize(values);
                if (auto* s = cube.find("image")) values[s->slot] = 6;
                cube.execute(values, sampleTo, hit);
                cubeOk = hit[0] == 6 && std::abs(hit[1] - 0.75f) < 1e-5f && std::abs(hit[2] - 0.25f) < 1e-5f;
            }
            check(projOk && lodOk && cubeOk && shows("uniform sampler2D image;\nvoid main() { gl_FragColor = texture2DLod(image, vec2(0.0), 0.0); }", "vertex shader"),
                  "GLSL texture2DProj, texture2DLod and textureCube");
        }
    }

    // Opt-in loopback TCP uses genuine BSD IO, not successful placeholder calls.
    {
        ProcessConfig netConfig;
        netConfig.homePath = "/var/tmp/hvm-loopback-unit";
        Process offline(netConfig);
        auto call = [](Process& p, const char* symbol, std::initializer_list<uint32_t> args) {
            if (!p.mainThread().errnoAddr) p.mainThread().errnoAddr = p.heap().allocZeroed(4);
            if (!p.mainThread().cpu.r[CPU::kSP]) {
                p.mem().map(0xBEF00000, 1u << 20, Memory::kProtRW);
                p.mainThread().cpu.r[CPU::kSP] = 0xBF000000 - 256;
            }
            return int32_t(p.callGuest(p.mainThread().cpu, *p.lookupSymbol(symbol), args));
        };
        auto error = [](Process& p) { return p.mem().read<int32_t>(p.mainThread().errnoAddr); };
        check(call(offline, "_socket", {2,1,0}) == -1 && error(offline) == 50,
              "BSD network remains disabled by default");
        {
            // Loopback-only UDP works offline: bind is forced to 127.0.0.1, broadcasts come
            // back to the device, other destinations are unreachable.
            Process& up = offline; auto& um = up.mem();
            const uint32_t b = up.heap().allocZeroed(256);
            auto sa = [&](uint32_t at, uint32_t ip, uint16_t port) {
                um.write<uint8_t>(at, 16); um.write<uint8_t>(at + 1, 2);
                um.write<uint16_t>(at + 2, htons(port)); um.write<uint32_t>(at + 4, htonl(ip));
            };
            const int a = call(up, "_socket", {2, 2, 0}), c = call(up, "_socket", {2, 2, 17});
            sa(b, 0, 0);   // INADDR_ANY, any port
            bool ok = a >= 0 && c >= 0 && call(up, "_bind", {uint32_t(a), b, 16}) == 0;
            um.write<uint32_t>(b + 20, 16);
            ok = ok && call(up, "_getsockname", {uint32_t(a), b + 32, b + 20}) == 0 && um.read<uint8_t>(b + 33) == 2 &&
                 um.read<uint32_t>(b + 36) == htonl(INADDR_LOOPBACK);
            const uint16_t port = ntohs(um.read<uint16_t>(b + 34));
            um.write<uint32_t>(b + 100, 1);
            ok = ok && call(up, "_setsockopt", {uint32_t(c), 0xffff, 0x20, b + 100, 4}) == 0;   // SO_BROADCAST
            um.copyIn(b + 120, "hi", 2);
            sa(b + 48, 0x7F000001, port);
            sa(b + 64, 0xFFFFFFFF, port);
            sa(b + 80, 0x08080808, port);
            const bool sent = call(up, "_sendto", {uint32_t(c), b + 120, 2, 0, b + 48, 16}) == 2 &&
                              call(up, "_sendto", {uint32_t(c), b + 120, 2, 0, b + 64, 16}) == 2;
            const bool refused = call(up, "_sendto", {uint32_t(c), b + 120, 2, 0, b + 80, 16}) == -1 && error(up) == 51;
            um.write<uint32_t>(b + 160 + uint32_t(a / 32) * 4, 1u << (a % 32));
            um.write<uint32_t>(b + 200, 1);
            const bool readable = call(up, "_select", {uint32_t(a + 1), b + 160, 0, 0, b + 200}) == 1;
            um.write<uint32_t>(b + 20, 16);
            const bool got1 = call(up, "_recvfrom", {uint32_t(a), b + 130, 16, 0, b + 32, b + 20}) == 2 &&
                              std::memcmp(um.host(b + 130), "hi", 2) == 0 && um.read<uint32_t>(b + 36) == htonl(INADDR_LOOPBACK);
            const bool got2 = call(up, "_recvfrom", {uint32_t(a), b + 130, 16, 0, 0, 0}) == 2;
            const bool empty = call(up, "_recvfrom", {uint32_t(a), b + 130, 16, 128, 0, 0}) == -1 && error(up) == 35;
            const bool closed = call(up, "_close", {uint32_t(a)}) == 0 && call(up, "_close", {uint32_t(c)}) == 0;
            check(ok && sent && refused && readable && got1 && got2 && empty && closed,
                  "Offline UDP stays on loopback: bind to 127.0.0.1, broadcast comes back, other hosts are ENETUNREACH, select/recvfrom/EAGAIN work");
        }
        int listener = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in local{}; local.sin_family = AF_INET; local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#ifdef __APPLE__
        local.sin_len = sizeof local;
#endif
        bool ready = listener >= 0 && ::bind(listener, reinterpret_cast<sockaddr*>(&local), sizeof local) == 0 && ::listen(listener,1) == 0;
        socklen_t localSize = sizeof local;
        ready &= ::getsockname(listener, reinterpret_cast<sockaddr*>(&local), &localSize) == 0;
        int leaked = -1;
        {
            netConfig.loopbackNetwork = true;
            netConfig.loopbackAliases = {"explicit.local"};
            Process net(netConfig); auto& mem = net.mem();
            uint32_t block = net.heap().allocZeroed(256);
            auto resolve = [&](const char* host) {
                return call(net,"_getaddrinfo",{net.allocCString(host),net.allocCString(std::to_string(ntohs(local.sin_port))),0,block});
            };
            bool resolved = resolve("localhost") == 0;
            uint32_t info = mem.read<uint32_t>(block), addr = info ? mem.read<uint32_t>(info + 24) : 0;
            resolved &= addr && mem.read<uint8_t>(addr) == 16 && mem.read<uint8_t>(addr + 1) == 2 && mem.read<uint16_t>(addr + 2) == local.sin_port;
            check(resolved && resolve("192.0.2.1") == 8 && mem.read<uint32_t>(block) == 0,
                  "loopback resolver emits Darwin addrinfo and rejects external addresses");
            int fd = call(net,"_socket",{2,1,0});
            bool flags = fd >= 0 && call(net,"_fcntl",{uint32_t(fd),4,6}) == 0 && (call(net,"_fcntl",{uint32_t(fd),3}) & 4) != 0 &&
                (::fcntl(fd,F_GETFL) & O_NONBLOCK) != 0;
            int connected = addr ? call(net,"_connect",{uint32_t(fd),addr,16}) : -1;
            ready &= connected == 0 || (connected == -1 && error(net) == 36);
            pollfd acceptReady{listener,POLLIN,0};
            ready &= ::poll(&acceptReady,1,1000) == 1;
            int peer = ready ? ::accept(listener,nullptr,nullptr) : -1;
            check(flags && peer >= 0, "BSD fcntl applies Darwin nonblocking flags and loopback connect reaches a real listener");
            mem.copyIn(block + 32,"ping",4);
            bool io = peer >= 0 && call(net,"_send",{uint32_t(fd),block + 32,4,0}) == 4;
            pollfd peerReady{peer,POLLIN,0}; char payload[4]{};
            io &= ::poll(&peerReady,1,1000) == 1 && ::recv(peer,payload,4,MSG_DONTWAIT) == 4 && std::memcmp(payload,"ping",4) == 0;
            io &= ::send(peer,"pong",4,0) == 4;
            mem.write<uint32_t>(block + 64 + uint32_t(fd / 32) * 4,1u << (fd % 32));
            mem.write<uint32_t>(block + 200,1);
            io &= call(net,"_select",{uint32_t(fd+1),block + 64,0,0,block + 200}) == 1 &&
                call(net,"_recv",{uint32_t(fd),block + 40,4,0}) == 4 && std::memcmp(mem.host(block+40),"pong",4) == 0;
            io &= call(net,"_recv",{uint32_t(fd),block + 40,4,128}) == -1 && error(net) == 35;
            check(io, "loopback send/select/recv exchanges actual bytes and preserves EAGAIN");
            bool checks = call(net,"_send",{uint32_t(fd),0xfffffff0,32,0}) == -1 && error(net) == 14;
            checks &= call(net,"_fdopen",{uint32_t(fd),net.allocCString("r")}) == 0 && error(net) == 45;
            mem.write<uint32_t>(block+16,4);
            checks &= call(net,"_getsockopt",{uint32_t(fd),0xffff,0x1007,block+12,block+16}) == 0 && mem.read<uint32_t>(block+12) == 0;
            mem.write<uint32_t>(addr+4,htonl(0xc0000201));
            checks &= call(net,"_connect",{uint32_t(fd),addr,16}) == -1 && error(net) == 51;
            check(checks, "loopback validates buffers, unsupported stdio wrapping, SO_ERROR and external connect policy");
            int duplicate = call(net,"_dup",{uint32_t(fd)});
            bool lifetime = duplicate >= 0 && call(net,"_close",{uint32_t(fd)}) == 0 &&
                call(net,"_dup2",{uint32_t(duplicate),uint32_t(fd)}) == fd &&
                call(net,"_close",{uint32_t(duplicate)}) == 0 && call(net,"_send",{uint32_t(fd),block+32,4,0}) == 4;
            check(lifetime, "ordinary dup/dup2/close preserve socket ownership and live aliases");
            call(net,"_freeaddrinfo",{info});
            bool alias = resolve("explicit.local") == 0;
            call(net,"_freeaddrinfo",{mem.read<uint32_t>(block)});
            check(alias, "only explicitly configured hostname aliases resolve to loopback");
            call(net,"_fcntl",{uint32_t(fd),4,2});
            std::atomic<bool> interrupted{false};
            auto began = std::chrono::steady_clock::now();
            std::thread blocked([&] {
                try { interrupted = call(net,"_read",{uint32_t(fd),block+40,4}) == -1; }
                catch (const CPU::GuestFault& fault) { interrupted = fault.exit == CPU::Exit::Halt; }
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(20)); net.requestStop(); blocked.join();
            check(interrupted && std::chrono::steady_clock::now() - began < std::chrono::seconds(1),
                  "process stop interrupts blocking socket read without hanging a guest thread");
            leaked = fd;
            if (peer >= 0) ::close(peer);
        }
        check(leaked >= 0 && ::fcntl(leaked,F_GETFD) == -1 && errno == EBADF,
              "process destruction closes retained loopback socket descriptors");
        {
            // The player's per-game "Allow internet" switch: real resolver, any address.
            ProcessConfig onCfg; onCfg.homePath = "/var/tmp/hvm-internet-unit"; onCfg.internetAccess = true;
            Process on(onCfg); auto& mem = on.mem();
            const uint32_t block = on.heap().allocZeroed(64), hints = on.heap().allocZeroed(32);
            mem.write<uint32_t>(hints, 4);       // AI_NUMERICHOST: no DNS needed in the test
            mem.write<uint32_t>(hints + 8, 1);   // SOCK_STREAM
            const int r = call(on, "_getaddrinfo", {on.allocCString("192.0.2.7"), on.allocCString("80"), hints, block});
            const uint32_t info = mem.read<uint32_t>(block), addr = info ? mem.read<uint32_t>(info + 24) : 0;
            in_addr want{}; inet_pton(AF_INET, "192.0.2.7", &want);
            const bool resolved = r == 0 && addr && mem.read<uint32_t>(info + 4) == 2 && mem.read<uint32_t>(info + 8) == 1 &&
                mem.read<uint8_t>(addr) == 16 && mem.read<uint8_t>(addr + 1) == 2 &&
                mem.read<uint16_t>(addr + 2) == htons(80) && mem.read<uint32_t>(addr + 4) == want.s_addr;
            call(on, "_freeaddrinfo", {info});
            const uint32_t he = call(on, "_gethostbyname", {on.allocCString("10.1.2.3")});
            in_addr want2{}; inet_pton(AF_INET, "10.1.2.3", &want2);
            const uint32_t list = he ? mem.read<uint32_t>(he + 16) : 0, first = list ? mem.read<uint32_t>(list) : 0;
            const bool hostent = he && mem.read<uint32_t>(he + 8) == 2 && mem.read<uint32_t>(he + 12) == 4 && first &&
                mem.read<uint32_t>(first) == want2.s_addr && mem.read<uint32_t>(list + 4) == 0 &&
                on.readCString(mem.read<uint32_t>(he)) == "10.1.2.3";
            const int fd = call(on, "_socket", {2, 1, 0});
            bool reached = false;
            if (fd >= 0 && ready) {
                // Through the real resolver and connect: the local listener is an address like any other.
                mem.write<uint32_t>(hints, 4);
                const int r2 = call(on, "_getaddrinfo", {on.allocCString("127.0.0.1"), on.allocCString(std::to_string(ntohs(local.sin_port)).c_str()), hints, block});
                const uint32_t i2 = mem.read<uint32_t>(block), a2 = i2 ? mem.read<uint32_t>(i2 + 24) : 0;
                const int c = r2 == 0 && a2 ? call(on, "_connect", {uint32_t(fd), a2, 16}) : -1;
                pollfd p{listener, POLLIN, 0};
                const int peer = c == 0 && ::poll(&p, 1, 1000) == 1 ? ::accept(listener, nullptr, nullptr) : -1;
                reached = peer >= 0;
                if (peer >= 0) ::close(peer);
                call(on, "_close", {uint32_t(fd)});
            }
            const uint32_t ref = uint32_t(call(on, "_SCNetworkReachabilityCreateWithName", {0, on.allocCString("gameloft.com")}));
            const uint32_t flagsOut = on.heap().allocZeroed(4);
            check(ref && call(on, "_SCNetworkReachabilityGetFlags", {ref, flagsOut}) == 1 && mem.read<uint32_t>(flagsOut) == 2,
                  "Allow internet: reachability says reachable");
            check(resolved && hostent && fd >= 0 && reached,
                  "Allow internet: TCP sockets open, getaddrinfo/gethostbyname return Darwin records from the host resolver, connect goes through");
            ProcessConfig offCfg; offCfg.homePath = "/var/tmp/hvm-internet-unit-off";
            Process off(offCfg);
            const uint32_t b2 = off.heap().allocZeroed(16);
            check(call(off, "_gethostbyname", {off.allocCString("example.com")}) == 0 &&
                  call(off, "_getaddrinfo", {off.allocCString("192.0.2.7"), 0, 0, b2}) == 8 &&
                  call(off, "_socket", {2, 1, 0}) == -1,
                  "Without the switch a game stays offline: no name lookups, no TCP sockets");
        }
        {
            // A secure connection (port 443) from a game allowed online goes to the TLS bridge's
            // local port, and the bridge hears which server it is for.
            struct FakeBridge final : TlsBridge {
                uint16_t port = 0; std::string host; uint16_t asked = 0;
                uint16_t open(const std::string& h, uint32_t, uint16_t p, std::function<void(const std::string&)>) override {
                    host = h; asked = p; return port;
                }
            };
            auto fake = std::make_shared<FakeBridge>();
            fake->port = ready ? ntohs(local.sin_port) : 0;
            ProcessConfig bCfg; bCfg.homePath = "/var/tmp/hvm-internet-bridge-unit";
            bCfg.internetAccess = true; bCfg.loopbackNetwork = true; bCfg.loopbackAliases = {"secure.example"}; bCfg.tlsBridge = fake;
            Process b(bCfg); auto& mem = b.mem();
            const uint32_t block = b.heap().allocZeroed(64);
            const int r = call(b, "_getaddrinfo", {b.allocCString("secure.example"), b.allocCString("443"), 0, block});
            const uint32_t info = mem.read<uint32_t>(block), addr = info ? mem.read<uint32_t>(info + 24) : 0;
            const int fd = call(b, "_socket", {2, 1, 0});
            const int c = r == 0 && addr && fd >= 0 ? call(b, "_connect", {uint32_t(fd), addr, 16}) : -1;
            pollfd p{listener, POLLIN, 0};
            const int peer = c == 0 && ::poll(&p, 1, 1000) == 1 ? ::accept(listener, nullptr, nullptr) : -1;
            check(peer >= 0 && fake->host == "secure.example" && fake->asked == 443,
                  "Allow internet: a connection to port 443 goes through the TLS bridge, named after the server the game looked up");
            if (peer >= 0) ::close(peer);
            if (fd >= 0) call(b, "_close", {uint32_t(fd)});
        }
#if defined(HVM_HAVE_OPENSSL) && !defined(__APPLE__)
        {
            // The test's own TLS ends write with write(); a closed peer must fail a check, not raise SIGPIPE.
            sigset_t pipeSet, oldSet; sigemptyset(&pipeSet); sigaddset(&pipeSet, SIGPIPE);
            pthread_sigmask(SIG_BLOCK, &pipeSet, &oldSet);
            // The OpenSSL bridge end to end: a TLS 1.0-only client (a 2013 game's OpenSSL) in,
            // a server that refuses anything below TLS 1.2 out, bytes passed both ways.
            SSL_CTX* sctx = SSL_CTX_new(TLS_server_method());
            SSL_CTX_set_min_proto_version(sctx, TLS1_2_VERSION);
            EVP_PKEY* key = EVP_RSA_gen(2048);
            X509* cert = X509_new();
            X509_gmtime_adj(X509_getm_notBefore(cert), 0); X509_gmtime_adj(X509_getm_notAfter(cert), 86400);
            X509_set_pubkey(cert, key); X509_set_issuer_name(cert, X509_get_subject_name(cert)); X509_sign(cert, key, EVP_sha256());
            SSL_CTX_use_certificate(sctx, cert); SSL_CTX_use_PrivateKey(sctx, key);
            int srv = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in sa{}; sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK); socklen_t sl = sizeof sa;
            bool up = ::bind(srv, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == 0 && ::listen(srv, 1) == 0 &&
                      ::getsockname(srv, reinterpret_cast<sockaddr*>(&sa), &sl) == 0;
            std::string serverGot, serverVersion;
            std::thread server([&] {
                pollfd p{srv, POLLIN, 0};
                const int c = up && ::poll(&p, 1, 10000) == 1 ? ::accept(srv, nullptr, nullptr) : -1;
                if (c < 0) return;
                timeval tv{10, 0};   // a bridge that stops forwarding fails the check instead of hanging
                ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); ::setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
                SSL* ss = SSL_new(sctx); SSL_set_fd(ss, c);
                if (SSL_accept(ss) == 1) {
                    serverVersion = SSL_get_version(ss);
                    char b[64] = {}; const int n = SSL_read(ss, b, 31);
                    if (n > 0) serverGot.assign(b, size_t(n));
                    SSL_write(ss, "HTTP/1.1 200 OK\r\n\r\n", 19);
                }
                SSL_shutdown(ss); SSL_free(ss); ::close(c);
            });
            std::string why, logged;
            auto bridge = makeOpenSslTlsBridge(false, &why);
            const uint16_t local = bridge && up ? bridge->open("127.0.0.1", sa.sin_addr.s_addr, ntohs(sa.sin_port),
                                                               [&](const std::string& l) { logged += l + "\n"; }) : 0;
            SSL_CTX* cctx = SSL_CTX_new(TLS_client_method());
            SSL_CTX_set_security_level(cctx, 0);
            SSL_CTX_set_min_proto_version(cctx, TLS1_VERSION); SSL_CTX_set_max_proto_version(cctx, TLS1_VERSION);
            SSL_CTX_set_cipher_list(cctx, "ALL:@SECLEVEL=0");
            std::string clientGot, clientVersion;
            const int cfd = ::socket(AF_INET, SOCK_STREAM, 0);
            { timeval tv{10, 0}; ::setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); ::setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv); }
            sockaddr_in ba{}; ba.sin_family = AF_INET; ba.sin_addr.s_addr = htonl(INADDR_LOOPBACK); ba.sin_port = htons(local);
            if (local && ::connect(cfd, reinterpret_cast<sockaddr*>(&ba), sizeof ba) == 0) {
                SSL* cs = SSL_new(cctx); SSL_set_fd(cs, cfd);
                if (SSL_connect(cs) == 1) {
                    clientVersion = SSL_get_version(cs);
                    SSL_write(cs, "GET /locate?service=auth HTTP/1.1\r\n", 35);
                    char b[64] = {}; const int n = SSL_read(cs, b, 19);
                    if (n > 0) clientGot.assign(b, size_t(n));
                }
                SSL_shutdown(cs); SSL_free(cs);
            }
            ::close(cfd);
            server.join();
            // A game whose own TLS is 1.2 or later is passed through: it sees the real server's certificate.
            bool sawServerCert = false;
            std::thread server2([&] {
                pollfd p{srv, POLLIN, 0};
                const int c = ::poll(&p, 1, 10000) == 1 ? ::accept(srv, nullptr, nullptr) : -1;
                if (c < 0) return;
                timeval tv{10, 0};
                ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
                SSL* ss = SSL_new(sctx); SSL_set_fd(ss, c);
                SSL_accept(ss);
                SSL_shutdown(ss); SSL_free(ss); ::close(c);
            });
            const uint16_t local2 = bridge ? bridge->open("127.0.0.1", sa.sin_addr.s_addr, ntohs(sa.sin_port), nullptr) : 0;
            SSL_CTX* mctx = SSL_CTX_new(TLS_client_method());
            SSL_CTX_set_min_proto_version(mctx, TLS1_2_VERSION);
            const int mfd = ::socket(AF_INET, SOCK_STREAM, 0);
            { timeval tv{10, 0}; ::setsockopt(mfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); }
            sockaddr_in ma{}; ma.sin_family = AF_INET; ma.sin_addr.s_addr = htonl(INADDR_LOOPBACK); ma.sin_port = htons(local2);
            if (local2 && ::connect(mfd, reinterpret_cast<sockaddr*>(&ma), sizeof ma) == 0) {
                SSL* ms = SSL_new(mctx); SSL_set_fd(ms, mfd);
                if (SSL_connect(ms) == 1) {
                    X509* peer = SSL_get1_peer_certificate(ms);
                    sawServerCert = peer && X509_cmp(peer, cert) == 0;
                    X509_free(peer);
                }
                SSL_shutdown(ms); SSL_free(ms);
            }
            ::close(mfd);
            server2.join();
            SSL_CTX_free(mctx);
            check(sawServerCert, "TLS bridge: a game with TLS 1.2 or later is passed through and sees the real server's certificate");
            {   // drop a SIGPIPE left pending by a write to a closed test socket, then restore the mask
                timespec zero{0, 0}; while (sigtimedwait(&pipeSet, nullptr, &zero) > 0) {}
                pthread_sigmask(SIG_SETMASK, &oldSet, nullptr);
            }
            ::close(srv);
            SSL_CTX_free(cctx); SSL_CTX_free(sctx); X509_free(cert); EVP_PKEY_free(key);
            check(clientVersion == "TLSv1" && serverVersion.rfind("TLSv1.", 0) == 0 && serverVersion != "TLSv1" &&
                  serverVersion != "TLSv1.1" && serverGot.rfind("GET /locate?service=auth", 0) == 0 && clientGot.rfind("HTTP/1.1 200 OK", 0) == 0 &&
                  logged.find("bridged") != std::string::npos && logged.find("<- GET /locate HTTP/1.1") != std::string::npos &&
                  logged.find("-> HTTP/1.1 200 OK") != std::string::npos,
                  "TLS bridge: a TLS 1.0 game reaches a TLS 1.2+ server, bytes pass both ways, HTTP lines are logged without queries");
        }
#endif
        if (listener >= 0) ::close(listener);
    }

    // 1. CPU Superinstructions & Time-Sliced Execution (3-insn budget)
    {
        auto rFull = runSelfTest(100'000);
        check(rFull.passed, "CPU selftest continuous with superinstructions (BLX, SUBS+BNE, MOV+ADD)");

        // Also test 3-instruction time slicing on the self-test program to prove every fused exit is resumable
        Memory::GuestArena mem;
        CPU::Engine engine(mem);
        // Same program as SelfTest.cpp plus MOVW+MOVT, LDR+LDR, STR+STR, LDR+ADD, POP {r4-r6,pc}
        static const uint32_t kArmProg[] = {
            0xE92D4070, // push {r4-r6, lr}
            0xE3014234, // movw r4, #0x1234
            0xE3454678, // movt r4, #0x5678  -> fused H_MovImm r4 = 0x56781234
            0xE28D5000, // add r5, sp, #0
            0xE52D4004, // push {r4} (str r4, [sp, #-4]!)
            0xE59D0000, // ldr r0, [sp, #0]
            0xE280000A, // add r0, r0, #10   -> fused H_LdrAddImm r0 = 0x5678123E
            0xE28DDF01, // add sp, sp, #4
            0xE8BD8070, // pop {r4-r6, pc}   -> fused H_MultiPopPc
        };
        mem.map(0x10000, 0x1000, Memory::kProtRX);
        mem.copyIn(0x10000, kArmProg, sizeof kArmProg);
        mem.map(0x000F0000, 0x10000, Memory::kProtRW);
        const uint32_t halt = engine.registerHle("halt", [](CPU::Cpu& c) { c.exit = CPU::Exit::Halt; });
        CPU::Cpu cpu;
        cpu.r[CPU::kSP] = 0x00100000;
        cpu.r[CPU::kLR] = halt;
        cpu.r[CPU::kPC] = 0x10000;
        CPU::Exit ex;
        while ((ex = engine.run(cpu, 2)) == CPU::Exit::Budget) {}
        check(ex == CPU::Exit::Halt && cpu.r[0] == 0x5678123Eu, "CPU superinstructions (MOVW+MOVT, LDR+ADD, POP {pc}) under 2-insn slices");
    }
    // NEON float compare against zero (2-register misc) must read the source register,
    // not register 0 (issue #34: Subway Surfers AABB frustum plane culling).
    {
        Memory::GuestArena mem;
        CPU::Engine engine(mem);
        static const uint32_t kNeonProg[] = {
            0xE3000000, 0xE3430F80, // movw r0, 0; movt r0, 0x3F80 (1.0f)
            0xEE000A10, 0xEE000A90, // vmov s0, r0; vmov s1, r0
            0xEE010A10, 0xEE010A90, // vmov s2, r0; vmov s3, r0
            0xE3000000, 0xE34B0F80, // movw r0, 0; movt r0, 0xBF80 (-1.0f)
            0xEE080A10,             // vmov s16, r0
            0xE3001000, 0xE3441000, // movw r1, 0; movt r1, 0x4000 (2.0f)
            0xEE081A90,             // vmov s17, r1
            0xE3002000, 0xE34C2040, // movw r2, 0; movt r2, 0xC040 (-3.0f)
            0xEE092A10,             // vmov s18, r2
            0xE3A03000,             // mov r3, #0 (0.0f)
            0xEE093A90,             // vmov s19, r3
            0xF3B98648,             // vclt.f32 q4, q4, #0
            0xEE180A10, 0xEE181A90, // vmov r0, s16; vmov r1, s17
            0xEE192A10, 0xEE193A90, // vmov r2, s18; vmov r3, s19
            0xE12FFF1E,             // bx lr
        };
        mem.map(0x10000, 0x1000, Memory::kProtRX);
        mem.copyIn(0x10000, kNeonProg, sizeof kNeonProg);
        mem.map(0x000F0000, 0x10000, Memory::kProtRW);
        const uint32_t halt = engine.registerHle("halt", [](CPU::Cpu& c) { c.exit = CPU::Exit::Halt; });
        CPU::Cpu cpu;
        cpu.r[CPU::kSP] = 0x00100000;
        cpu.r[CPU::kLR] = halt;
        cpu.r[CPU::kPC] = 0x10000;
        CPU::Exit ex;
        while ((ex = engine.run(cpu, 1000)) == CPU::Exit::Budget) {}
        check(ex == CPU::Exit::Halt && cpu.r[0] == 0xFFFFFFFFu && cpu.r[1] == 0 && cpu.r[2] == 0xFFFFFFFFu && cpu.r[3] == 0,
              "NEON float compare with zero (VCLT #0) uses source register");
    }


    // 2. Full Process + Objective-C + UIKit + OpenGL ES + Audio verification
    ProcessConfig cfg;
    cfg.bundlePath = "/tmp/hvm_test_bundle.app";
    // A private home (mkdtemp, mode 0700): a fixed /tmp path could be pre-seeded with a symlink that redirects the
    // fixtures' O_TRUNC writes.
    char privateHome[] = "/tmp/hvm_test_home.XXXXXX";
    if (!mkdtemp(privateHome)) { std::fprintf(stderr, "mkdtemp failed\n"); return 1; }
    cfg.homePath = privateHome;
    struct RemoveHome { const char* path; ~RemoveHome() { std::error_code ec; std::filesystem::remove_all(path, ec); } } removeHome{privateHome};   // declared before proc: runs after it is destroyed
    Process proc(cfg);
    // Renders the mixer (as the platform's audio thread does) and returns how many
    // frames of sound came out: the index after the last non-silent frame.
    auto renderedFrames = [&](size_t maxFrames) {
        std::vector<float> l(512), rr(512);
        size_t last = 0;
        for (size_t done = 0; done < maxFrames; done += 512) {
            proc.mixer().render(l.data(), rr.data(), 512);
            for (size_t i = 0; i < 512; ++i) if (l[i] != 0 || rr[i] != 0) last = done + i + 1;
        }
        return last;
    };
    CPU::Cpu& cpu = proc.mainThread().cpu;
    Ctx cx{proc, cpu, proc.mem()};
    auto& r = ObjC::rt(proc);
    // Initialize main thread stack & environment for direct host->guest ObjC calls
    proc.mem().map(0xBF000000u - (1u << 20), 1u << 20, Memory::kProtRW);
    cpu.r[CPU::kSP] = 0xBF000000u - 256;
    // Crash clues: the crashing thread's last host calls and what they returned, and a
    // "likely cause" for a NULL access right after a function returned 0 or for a trap.
    {
        const uint32_t nilFn = proc.defineFunction("_test_returns_nothing", [](Ctx& x) { x.cpu.r[0] = 0; });
        const uint32_t okFn = proc.defineFunction("_test_returns_object", [](Ctx& x) { x.cpu.r[0] = 0x1234; });
        proc.callGuest(cpu, okFn, {});
        proc.callGuest(cpu, nilFn, {});
        // files the app failed to open, and the last it opened, are part of the clues
        {
            Process fp(ProcessConfig{});
            for (int i = 0; i < 8; ++i) fp.recordFileOpen("/app/missing" + std::to_string(i) + ".xnb", "open", ENOENT, -1, 0, false);
            const uint64_t h = fp.recordFileOpen("/app/Content/Manifest.xnb", "fopen", 0, 1234, 0, true);
            fp.updateFileRead(h, 100);
            const std::string fc = fp.crashClues(CPU::Exit::MemFault, 0, true);
            const size_t m2 = fc.find("open /app/missing2.xnb: No such file or directory"), m7 = fc.find("open /app/missing7.xnb: No such file or directory");
            check(fc.find("files the app could not open (latest last):") != std::string::npos &&
                  fc.find("missing1.xnb") == std::string::npos && m2 != std::string::npos && m7 != std::string::npos && m2 < m7 &&   // the last six, oldest first
                  fc.find("fopen /app/Content/Manifest.xnb (1234 bytes, at least 100 read)") != std::string::npos,
                  "crash clues list the last six files the app could not open and the last files it opened, with sizes");
            check(Process(ProcessConfig{}).crashClues(CPU::Exit::MemFault, 0, true).find("files") == std::string::npos,
                  "crash clues say nothing about files when the app touched none");
        }
        const std::string nullClues = proc.crashClues(CPU::Exit::MemFault, 0, true);
        const bool listed = nullClues.find("_test_returns_object -> 0x00001234") != std::string::npos &&
                            nullClues.find("_test_returns_nothing -> 0x00000000") != std::string::npos &&
                            nullClues.find("_test_returns_object") < nullClues.find("_test_returns_nothing");
        const bool blamed = nullClues.find("likely cause: _test_returns_nothing returned 0") != std::string::npos;
        // Instruction sets: Thumb udf #254 and ARM 0xe7ffdefe are traps; an ARM word whose
        // high half happens to be 0xdefe (a conditional coprocessor op) is not.
        const bool trap = proc.crashClues(CPU::Exit::Undefined, 0xdefe981du, true).find("stopped itself on purpose") != std::string::npos &&
                          proc.crashClues(CPU::Exit::Undefined, 0xe7ffdefeu, false).find("stopped itself on purpose") != std::string::npos;
        const bool notTrap = proc.crashClues(CPU::Exit::Undefined, 0xee000a10u, true).find("stopped itself") == std::string::npos &&
                             proc.crashClues(CPU::Exit::Undefined, 0xdefe0010u, false).find("stopped itself") == std::string::npos;
        // A host call that threw is "did not return", never blamed for the 0 left in r0.
        const uint32_t throwsFn = proc.defineFunction("_test_throws", [](Ctx& x) { x.cpu.r[0] = 0; throw std::logic_error("test"); });
        try { proc.callGuest(cpu, throwsFn, {}); } catch (const std::exception&) {}
        const std::string thrown = proc.crashClues(CPU::Exit::MemFault, 0, true);
        const bool unfinished = thrown.find("_test_throws (did not return)") != std::string::npos &&
                                thrown.find("_test_throws returned 0") == std::string::npos &&
                                thrown.find("likely cause: the crash happened inside _test_throws") != std::string::npos;
        // More than 12 calls: only the newest 12, oldest first.
        const uint32_t countFn = proc.defineFunction("_test_count", [](Ctx& x) { x.cpu.r[0] = x.cpu.r[0] + 100; });
        for (uint32_t k = 1; k <= 14; ++k) proc.callGuest(cpu, countFn, {k});
        const std::string many = proc.crashClues(CPU::Exit::MemFault, 0, true);
        size_t shown = 0, pos = 0;
        while ((pos = many.find("_test_count -> ", pos)) != std::string::npos) { ++shown; ++pos; }
        const bool newest = shown == 12 && many.find("_test_count -> 0x00000066") == std::string::npos &&   // call 2 (2 + 100) dropped
                            many.find("_test_count -> 0x00000067") != std::string::npos &&                   // call 3 is the oldest kept
                            many.find("0x00000067") < many.find("0x00000072") && many.find("_test_throws") == std::string::npos;
        if (!(listed && blamed && trap && notTrap && unfinished && newest))
            std::printf("    clues:\n%s%s%s", nullClues.c_str(), thrown.c_str(), many.c_str());
        check(listed && blamed && trap && notTrap && unfinished && newest,
              "crash clues list the newest 12 host calls in order with results, blame a 0 result only from a call that returned, and name app traps per instruction set");
    }
    const uint32_t outerPool = r.poolPush();

    // #32: exercise Cocoa semantics rather than accepting placeholder class pointers.
    {
        using namespace Foundation;
        auto call=[&](uint32_t object,const char* sel,std::initializer_list<uint32_t> args={}) {return uint32_t(r.send(cpu,object,sel,args));};
        const auto key=makeString(cx,"score");
        const auto low=makeDict(cx,{{key,makeNumberInt(cx,2)}}), high=makeDict(cx,{{key,makeNumberInt(cx,9)}});
        const auto rows=makeArray(cx,{high,low,high});
        bool sorted=false;
        if(r.getClass("NSSortDescriptor")) {
            const auto mutableKey=makeString(cx,"score",false,true);
            const auto descriptor=call(r.getClass("NSSortDescriptor"),"sortDescriptorWithKey:ascending:",{mutableKey,1});
            call(mutableKey,"setString:",{makeString(cx,"changed")});
            auto values=arrayItems(cx,call(rows,"sortedArrayUsingDescriptors:",{makeArray(cx,{descriptor})}));
            auto reverse=call(descriptor,"reversedSortDescriptor");
            sorted=values==std::vector<ObjC::id>{low,high,high} && int32_t(call(reverse,"compareObject:toObject:",{low,high}))>0;
        }
        check(sorted,"NSSortDescriptor copies key, stable-sorts and reverses comparison");
        bool predicates=false;
        if(r.getClass("NSPredicate")) {
            auto predicate=call(r.getClass("NSPredicate"),"predicateWithFormat:",{makeString(cx,"score >= %d"),5});
            auto values=arrayItems(cx,call(rows,"filteredArrayUsingPredicate:",{predicate}));
            auto equal=call(r.getClass("NSPredicate"),"predicateWithFormat:argumentArray:",{makeString(cx,"%K == %@"),makeArray(cx,{key,makeNumberInt(cx,2)})});
            bool rejected=false;try{call(r.getClass("NSPredicate"),"predicateWithFormat:",{makeString(cx,"score BETWEEN {1, 3}")});}catch(const std::exception& e){rejected=std::string(e.what()).find("unsupported NSPredicate format")!=std::string::npos;}
            predicates=values==std::vector<ObjC::id>{high,high} && call(equal,"evaluateWithObject:",{low}) && !call(equal,"evaluateWithObject:",{high}) && rejected;
        }
        check(predicates,"NSPredicate key comparisons/filtering and explicit unsupported syntax");
        // #353: AND / OR / NOT, parentheses and NSCompoundPredicate (Kindle stops on "(%K == %@ || %K == %@)").
        // Expected matches are worked out by hand from the four rows below.
        bool compoundOk=false;
        if(r.getClass("NSPredicate") && r.getClass("NSCompoundPredicate")) {
            const auto K=makeString(cx,"kind"), N=makeString(cx,"n");
            auto row=[&](const char* k,int n){return makeDict(cx,{{K,makeString(cx,k)},{N,makeNumberInt(cx,n)}});};
            const auto a1=row("a",1), a2=row("a",2), b3=row("b",3), c4=row("c",4);
            const auto all=makeArray(cx,{a1,a2,b3,c4});
            auto matches=[&](uint32_t pred){return arrayItems(cx,call(all,"filteredArrayUsingPredicate:",{pred}));};
            // Variadic arguments: unused trailing words are never read by the format.
            auto fmt=[&](const char* f,std::initializer_list<uint32_t> more={}){
                uint32_t a[4]={0,0,0,0}; size_t n=0; for(uint32_t v:more) if(n<4) a[n++]=v;
                return uint32_t(r.send(cpu,r.getClass("NSPredicate"),"predicateWithFormat:",{makeString(cx,f),a[0],a[1],a[2],a[3]}));
            };
            const auto P=r.getClass("NSPredicate");
            bool ok=true;
            auto expectRows=[&](uint32_t pred,std::vector<ObjC::id> want,const char* what){ if(matches(pred)!=want){ std::printf("    NSPredicate: %s\n",what); ok=false; } };
            expectRows(fmt("(%K == %@ || %K == %@)",{K,makeString(cx,"a"),K,makeString(cx,"b")}),{a1,a2,b3},"(%K == %@ || %K == %@)");
            // String operators and IN (Kindle stops on "%K BEGINSWITH %@").
            const auto T=makeString(cx,"title");
            auto book=[&](const char* title){return makeDict(cx,{{T,makeString(cx,title)}});};
            const auto b1=book("Caf\xc3\xa9 Stories"), b2=book("cafe society"), b3t=book("The Last Cafe"), b4=book("Notes");
            const auto books=makeArray(cx,{b1,b2,b3t,b4});
            auto bookMatches=[&](uint32_t pred){return arrayItems(cx,call(books,"filteredArrayUsingPredicate:",{pred}));};
            auto expectBooks=[&](uint32_t pred,std::vector<ObjC::id> want,const char* what){ if(bookMatches(pred)!=want){ std::printf("    NSPredicate: %s\n",what); ok=false; } };
            expectBooks(fmt("%K BEGINSWITH %@",{T,makeString(cx,"caf")}),{b2},"%K BEGINSWITH %@ (case-sensitive)");
            expectBooks(fmt("%K BEGINSWITH[c] %@",{T,makeString(cx,"CAF")}),{b1,b2},"BEGINSWITH[c]");
            expectBooks(fmt("title ENDSWITH[cd] %@",{makeString(cx,"CAFE")}),{b3t},"ENDSWITH[cd]");
            expectBooks(fmt("title CONTAINS[cd] %@",{makeString(cx,"cafe")}),{b1,b2,b3t},"CONTAINS[cd] folds the accent");
            expectBooks(fmt("title CONTAINS %@",{makeString(cx,"cafe")}),{b2},"CONTAINS is case- and accent-sensitive");
            expectBooks(fmt("title BEGINSWITH[d] %@",{makeString(cx,"Cafe")}),{b1},"[d] alone folds the accent and keeps case");
            expectBooks(fmt("title BEGINSWITH[n] %@",{makeString(cx,"cafe")}),{b2},"[n] changes nothing for composed text");
            expectBooks(fmt("title LIKE[c] %@",{makeString(cx,"*c?fe*")}),{b2,b3t},"LIKE with * and ?");
            expectBooks(fmt("title IN %@",{makeArray(cx,{makeString(cx,"Notes"),makeString(cx,"x")})}),{b4},"IN an array");
            expectBooks(fmt("NOT (title BEGINSWITH 'The') AND title CONTAINS[c] 'CAF'"),{b1,b2},"string operators inside NOT/AND");
            expectRows(fmt("n > 1 AND (kind == 'a' OR kind == 'c')"),{a2,c4},"AND with a parenthesised OR");
            expectRows(fmt("NOT (n >= 3)"),{a1,a2},"NOT (...)");
            expectRows(fmt("!(kind == 'a') && n < 4"),{b3},"! and &&");
            expectRows(fmt("kind == 'a' OR kind == 'b' AND n > 2"),{a1,a2,b3},"AND binds tighter than OR");
            expectRows(fmt("kind = 'c' or n <> 1 and n =< 2"),{a2,c4},"lower-case keywords, =, <>, =<");
            expectRows(fmt("TRUEPREDICATE AND NOT FALSEPREDICATE"),{a1,a2,b3,c4},"TRUEPREDICATE / FALSEPREDICATE as operands");
            const auto pa=fmt("kind == 'a'"), pn=fmt("n >= 2");
            expectRows(call(r.getClass("NSCompoundPredicate"),"andPredicateWithSubpredicates:",{makeArray(cx,{pa,pn})}),{a2},"NSCompoundPredicate AND");
            expectRows(call(r.getClass("NSCompoundPredicate"),"orPredicateWithSubpredicates:",{makeArray(cx,{pa,fmt("n == 4")})}),{a1,a2,c4},"NSCompoundPredicate OR");
            expectRows(call(r.getClass("NSCompoundPredicate"),"notPredicateWithSubpredicate:",{pa}),{b3,c4},"NSCompoundPredicate NOT");
            expectRows(call(r.getClass("NSCompoundPredicate"),"andPredicateWithSubpredicates:",{makeArray(cx,{})}),{a1,a2,b3,c4},"empty AND is true");
            expectRows(call(r.getClass("NSCompoundPredicate"),"orPredicateWithSubpredicates:",{makeArray(cx,{})}),{},"empty OR is false");
            (void)P;
            // IN and CONTAINS are supported now (string operators above); BETWEEN and broken syntax still raise.
            for(const char* bad : {"n BETWEEN %@","(kind == 'a'","kind == 'a' extra","kind == 'a' AND","kind CONTAINS[x] 'a'","AND == 1"}) {
                bool rejected=false;
                try{ fmt(bad,{makeArray(cx,{})}); }catch(const std::exception& e){ rejected=std::string(e.what()).find("unsupported NSPredicate format")!=std::string::npos; }
                if(!rejected){ std::printf("    NSPredicate: accepted %s\n",bad); ok=false; }
            }
            // Deep nesting is refused (not a host stack overflow), and a failed parse releases
            // the arguments it had already retained.
            {
                std::string deep(5000,'('); deep+="n == 1"; deep+=std::string(5000,')');
                bool rejected=false;
                try{ fmt(deep.c_str()); }catch(const std::exception& e){ rejected=std::string(e.what()).find("unsupported NSPredicate format")!=std::string::npos; }
                if(!rejected){ std::printf("    NSPredicate: deep nesting not refused\n"); ok=false; }
                const auto arg=makeString(cx,"held",true);
                const uint32_t before=call(arg,"retainCount");
                try{ fmt("kind == %@ AND",{arg}); }catch(const std::exception&){}
                try{ fmt("(kind == %@",{arg}); }catch(const std::exception&){}
                if(call(arg,"retainCount")!=before){ std::printf("    NSPredicate: failed parse leaked a retain\n"); ok=false; }
            }
            // Long AND/OR chains and large compound arrays stay shallow (one n-ary node), so
            // evaluating, copying and freeing them cannot exhaust the host stack; deliberately
            // nested compounds beyond the height limit are refused.
            {
                std::string chain="n == 9";
                for(int k=0;k<20000;++k) chain+=" OR n == 9";
                chain+=" OR n == 4";
                expectRows(fmt(chain.c_str()),{c4},"a 20001-term OR chain");
                std::vector<ObjC::id> many;
                for(int k=0;k<20000;++k) many.push_back(fmt("n == 9"));
                many.push_back(fmt("n == 2"));
                expectRows(call(r.getClass("NSCompoundPredicate"),"orPredicateWithSubpredicates:",{makeArray(cx,many)}),{a2},"a 20001-element compound OR");
                uint32_t nested=fmt("n == 1");
                bool refused=false;
                for(int k=0;k<200 && !refused;++k) {
                    try{ nested=call(r.getClass("NSCompoundPredicate"),"notPredicateWithSubpredicate:",{nested}); }
                    catch(const std::exception& e){ refused=std::string(e.what()).find("too deep")!=std::string::npos; }
                }
                if(!refused){ std::printf("    NSPredicate: 200 nested compounds were not refused\n"); ok=false; }
            }
            compoundOk=ok;
        }
        check(compoundOk,"NSPredicate AND/OR/NOT with parentheses, keyword aliases and NSCompoundPredicate; other syntax still raises");
        bool matches=false;
        try {
            auto cls=r.getClass("NSPredicate");
            auto pattern=call(cls,"predicateWithFormat:",{makeString(cx,"SELF MATCHES %@"),makeString(cx,"a|ab")});
            auto insensitive=call(cls,"predicateWithFormat:argumentArray:",{makeString(cx,"SELF matches[c] %@"),makeArray(cx,{makeString(cx,"cat")})});
            bool invalid=false;try{call(cls,"predicateWithFormat:",{makeString(cx,"SELF MATCHES %@"),makeString(cx,"[")});}
            catch(const std::exception& e){invalid=std::string(e.what()).find("MATCHES regular expression")!=std::string::npos;}
            matches=call(pattern,"evaluateWithObject:",{makeString(cx,"ab")}) && !call(pattern,"evaluateWithObject:",{makeString(cx,"abc")}) &&
                !call(pattern,"evaluateWithObject:",{0}) && call(insensitive,"evaluateWithObject:",{makeString(cx,"CAT")}) && invalid;
        }catch(const std::exception&){}
        check(matches,"NSPredicate MATCHES full-string alternatives, argument-array case folding, nil and invalid pattern");
        bool tables=false;
        if(r.getClass("NSHashTable") && r.getClass("NSMapTable")) {
            auto weak=call(r.getClass("NSHashTable"),"weakObjectsHashTable");
            auto strong=call(r.getClass("NSMapTable"),"strongToStrongObjectsMapTable");
            auto weakMap=call(r.getClass("NSMapTable"),"strongToWeakObjectsMapTable");
            const auto object=r.createInstance(r.nsObject);
            call(weak,"addObject:",{object});call(weakMap,"setObject:forKey:",{object,key});call(strong,"setObject:forKey:",{object,key});
            r.release(cpu,object);
            bool retained=call(weak,"count")==1 && call(strong,"objectForKey:",{key})==object;
            call(strong,"removeObjectForKey:",{key});
            tables=retained && call(weak,"count")==0 && call(weakMap,"count")==0 && !call(weakMap,"objectForKey:",{key});
        }
        check(tables,"NSHashTable/NSMapTable retain strong entries and zero weak entries on release");
        // Other NSPointerFunctions options games pass (Crossy Road stopped at launch on one):
        // object-pointer keys compare by identity, opaque values are stored as is (never
        // retained or zeroed), C-string keys compare by contents, copy-in keys are copied.
        bool tableOptions=false;
        if(r.getClass("NSMapTable")) {
            auto mapCls=r.getClass("NSMapTable");
            auto identity=call(mapCls,"mapTableWithKeyOptions:valueOptions:",{0x200,0x102});
            const auto k1=makeString(cx,"same",true), k2=makeString(cx,"same",true);
            const uint32_t keyRC=call(k1,"retainCount");
            call(identity,"setObject:forKey:",{0x1234,k1});
            const bool byIdentity=call(identity,"objectForKey:",{k1})==0x1234 && call(identity,"objectForKey:",{k2})==0 &&
                                  call(k1,"retainCount")==keyRC+1;
            auto cstrings=call(mapCls,"mapTableWithKeyOptions:valueOptions:",{0x302,0});
            const uint32_t a=proc.heap().allocZeroed(8), b=proc.heap().allocZeroed(8);
            proc.mem().write<uint32_t>(a,0x00636261); proc.mem().write<uint32_t>(b,0x00636261);   // "abc"
            const auto value=r.createInstance(r.nsObject);
            call(cstrings,"setObject:forKey:",{value,a});
            const bool byContents=call(cstrings,"objectForKey:",{b})==value && call(value,"retainCount")==2;
            auto copying=call(mapCls,"mapTableWithKeyOptions:valueOptions:",{0x10000,0});
            const auto mutableKey=call(call(r.getClass("NSMutableString"),"alloc"),"initWithString:",{makeString(cx,"k1",true)});
            call(copying,"setObject:forKey:",{value,mutableKey});
            call(mutableKey,"appendString:",{makeString(cx,"-changed",true)});
            const bool copied=call(copying,"objectForKey:",{makeString(cx,"k1",true)})==value;
            bool refused=false;
            try{ call(mapCls,"mapTableWithKeyOptions:valueOptions:",{0x400,0}); }catch(const std::exception&){ refused=true; }
            tableOptions=byIdentity && byContents && copied && refused;
            if(!tableOptions) std::printf("    NSMapTable options: identity %d contents %d copy-in %d struct refused %d\n",byIdentity,byContents,copied,refused);
        }
        check(tableOptions,"NSMapTable object-pointer, opaque, C-string and copy-in options");
        // for (key in mapTable), NSPointerArray and +predicateWithBlock: (Crossy Road used all three).
        bool enumerated=false, pointers=false, blockPredicate=false;
        {
            auto map=call(r.getClass("NSMapTable"),"strongToStrongObjectsMapTable");
            const auto k1=makeString(cx,"k1",true), k2=makeString(cx,"k2",true);
            call(map,"setObject:forKey:",{key,k1}); call(map,"setObject:forKey:",{key,k2});
            const uint32_t state=proc.heap().allocZeroed(64), buf=proc.heap().allocZeroed(64);
            std::set<uint32_t> seen; uint32_t n;
            while((n=call(map,"countByEnumeratingWithState:objects:count:",{state,buf,16}))!=0)
                for(uint32_t i=0;i<n;++i) seen.insert(proc.mem().read<uint32_t>(proc.mem().read<uint32_t>(state+4)+i*4));
            enumerated=seen==std::set<uint32_t>{k1,k2} && proc.mem().read<uint32_t>(state+8)!=0;
            auto pa=call(r.getClass("NSPointerArray"),"strongObjectsPointerArray");
            const auto object=r.createInstance(r.nsObject);
            call(pa,"addPointer:",{object}); call(pa,"addPointer:",{0}); call(pa,"insertPointer:atIndex:",{key,0});
            const bool built=call(pa,"count")==3 && call(pa,"pointerAtIndex:",{0})==key && call(pa,"pointerAtIndex:",{2})==0 &&
                             call(object,"retainCount")==2;
            call(pa,"compact");
            pointers=built && call(pa,"count")==2 && call(call(pa,"allObjects"),"count")==2;
            r.release(cpu,object);
            static uint32_t wanted=0;
            wanted=k2;
            const uint32_t invoke=proc.defineFunction("test_predicate_block",[](Ctx& x){ x.cpu.r[0]=x.cpu.r[1]==wanted; });
            const uint32_t desc=proc.heap().allocZeroed(8); proc.mem().write<uint32_t>(desc+4,20);
            const uint32_t blk=proc.heap().allocZeroed(20);
            proc.mem().write<uint32_t>(blk+12,invoke); proc.mem().write<uint32_t>(blk+16,desc);
            auto pred=call(r.getClass("NSPredicate"),"predicateWithBlock:",{blk});
            blockPredicate=pred && (call(pred,"evaluateWithObject:",{k2})&0xff)==1 && (call(pred,"evaluateWithObject:",{k1})&0xff)==0 &&
                           call(call(Foundation::makeArray(cx,{k1,k2}),"filteredArrayUsingPredicate:",{pred}),"count")==1;
            if(!(enumerated&&pointers&&blockPredicate)) std::printf("    enumerate %d pointers %d (built %d) block predicate %d\n",enumerated,pointers,built,blockPredicate);
        }
        check(enumerated&&pointers&&blockPredicate,"NSMapTable fast enumeration, NSPointerArray and +[NSPredicate predicateWithBlock:]");
        bool attributed=false;
        if(r.getClass("NSMutableAttributedString")) {
            auto object=call(r.createInstance(r.getClass("NSMutableAttributedString")),"initWithString:attributes:",{makeString(cx,std::u16string(u"A\U0001f600B")),makeDict(cx,{{key,makeNumberInt(cx,1)}})});
            call(object,"addAttribute:value:range:",{key,makeNumberInt(cx,7),1,2});
            auto range=proc.heap().allocZeroed(8);
            auto val=call(object,"attribute:atIndex:effectiveRange:",{key,2,range});
            bool utf16=call(object,"length")==4 && numberInt(cx,val)==7 && proc.mem().read<uint32_t>(range)==1 && proc.mem().read<uint32_t>(range+4)==2;
            auto snapshot=call(object,"copy");auto view=call(object,"mutableString");
            call(view,"replaceCharactersInRange:withString:",{1,2,makeString(cx,"xy")});call(view,"appendString:",{makeString(cx,"!")});
            attributed=utf16 && utf8(cx,call(object,"string"))=="AxyB!" && numberInt(cx,call(object,"attribute:atIndex:effectiveRange:",{key,1,0}))==7 && call(snapshot,"length")==4 && stringValue(cx,call(snapshot,"string"))==u"A\U0001f600B";
            r.release(cpu,snapshot);r.release(cpu,object);proc.heap().free(range);
        }
        check(attributed,"NSMutableAttributedString preserves UTF-16 ranges, copied snapshots and mutableString edits");
        bool support=false;
        if(r.getClass("NSMutableParagraphStyle") && r.getClass("NSURLCredential")) {
            auto style=r.createInstance(r.getClass("NSMutableParagraphStyle"));
            call(style,"setLineSpacing:",{std::bit_cast<uint32_t>(2.5f)});call(style,"setAlignment:",{2});auto copy=call(style,"copy");call(style,"setAlignment:",{1});
            auto password=makeString(cx,"secret",false,true);
            auto credential=call(r.getClass("NSURLCredential"),"credentialWithUser:password:persistence:",{makeString(cx,"user"),password,1});call(password,"setString:",{makeString(cx,"changed")});
            support=call(copy,"alignment")==2 && std::bit_cast<float>(call(copy,"lineSpacing"))==2.5f && utf8(cx,call(credential,"password"))=="secret" && call(credential,"persistence")==1;
            r.release(cpu,copy);r.release(cpu,style);
        }
        check(support,"Paragraph styles copy independent properties; NSURLCredential snapshots password");
        bool indexSetArrays = false;
        {
            const uint32_t a0 = Foundation::makeString(cx, "A"), a1 = Foundation::makeString(cx, "B"),
                           a2 = Foundation::makeString(cx, "C"), a3 = Foundation::makeString(cx, "D"),
                           a4 = Foundation::makeString(cx, "E");
            const uint32_t arr = Foundation::makeArray(cx, {a0, a1, a2, a3, a4});
            const uint32_t is13 = Foundation::makeIndexSet(cx, {1, 3});
            const uint32_t sub = call(arr, "objectsAtIndexes:", {is13});
            bool subOk = Foundation::arrayItems(cx, sub) == std::vector<id>{a1, a3};

            bool oobSub = false;
            try { const uint32_t badIS = Foundation::makeIndexSet(cx, {1, 5}); call(arr, "objectsAtIndexes:", {badIS}); }
            catch (const std::exception& e) { oobSub = std::string(e.what()).find("beyond bounds") != std::string::npos; }

            const uint32_t marr = Foundation::makeArray(cx, {a0, a1, a2, a3, a4}, true, true);
            call(marr, "removeObjectsAtIndexes:", {is13});
            bool remOk = Foundation::arrayItems(cx, marr) == std::vector<id>{a0, a2, a4};

            const uint32_t xObj = Foundation::makeString(cx, "X"), yObj = Foundation::makeString(cx, "Y");
            const uint32_t insArr = Foundation::makeArray(cx, {a0, a1}, true, true);
            const uint32_t insItems = Foundation::makeArray(cx, {xObj, yObj});
            call(insArr, "insertObjects:atIndexes:", {insItems, is13});
            bool insOk = Foundation::arrayItems(cx, insArr) == std::vector<id>{a0, xObj, a1, yObj};

            bool mismatchIns = false;
            try { const uint32_t oneItem = Foundation::makeArray(cx, {xObj}); call(insArr, "insertObjects:atIndexes:", {oneItem, is13}); }
            catch (const std::exception& e) { mismatchIns = std::string(e.what()).find("differs from count") != std::string::npos; }

            const uint32_t replArr = Foundation::makeArray(cx, {a0, a1, a2, a3}, true, true);
            call(replArr, "replaceObjectsAtIndexes:withObjects:", {is13, insItems});
            bool replOk = Foundation::arrayItems(cx, replArr) == std::vector<id>{a0, xObj, a2, yObj};

            bool oobRepl = false;
            try { const uint32_t oobIS = Foundation::makeIndexSet(cx, {1, 10}); call(replArr, "replaceObjectsAtIndexes:withObjects:", {oobIS, insItems}); }
            catch (const std::exception& e) { oobRepl = std::string(e.what()).find("beyond bounds") != std::string::npos; }

            r.release(cpu, marr); r.release(cpu, insArr); r.release(cpu, replArr);
            indexSetArrays = subOk && oobSub && remOk && insOk && mismatchIns && replOk && oobRepl;
        }
        check(indexSetArrays, "NSArray / NSMutableArray index set operations (objectsAtIndexes, remove, insert, replace, bounds)");
        bool geometry=false;
        if(r.getClass("UIBezierPath")) {
            const auto f=[](float v){return std::bit_cast<uint32_t>(v);};
            auto path=call(r.getClass("UIBezierPath"),"bezierPathWithRect:",{f(1),f(2),f(10),f(20)});
            auto copy=call(path,"copy");call(path,"removeAllPoints");
            bool inside=call(copy,"containsPoint:",{f(5),f(5)}) && !call(copy,"containsPoint:",{f(50),f(50)});
            const auto out=proc.heap().allocZeroed(16);
            // ARMv7 CGRect returns through r0, before self and selector.
            proc.callGuest(cpu,r.lookup(r.classOf(copy),r.sel("bounds")),{out,copy,r.sel("bounds")});
            geometry=inside && call(path,"isEmpty") && proc.mem().read<float>(out)==1 && proc.mem().read<float>(out+4)==2 && proc.mem().read<float>(out+8)==10 && proc.mem().read<float>(out+12)==20;
            proc.heap().free(out);r.release(cpu,copy);
        }
        check(geometry,"UIBezierPath owns CGPath copies, contains points and returns ARMv7 CGRect bounds");
    }


    // AVPlayerItem starts Unknown; readiness is observed after association with
    // a player. Apple documents this transition; a synchronous Ready initial
    // observation made UE3 call play on a player it had not created yet.
    {
        struct ReadyObserver : ObjC::HostObject {
            std::vector<int64_t> states;
            std::vector<uint32_t> contexts;
            ObjC::id player = 0, primary = 0;
            bool playerAssigned = true;
        };
        auto cls = r.defineClass("HVMPlayerItemReadyObserver", "NSObject");
        cls.method("observeValueForKeyPath:ofObject:change:context:", [](Ctx& x) {
            auto& state = ObjC::rt(x).host<ReadyObserver>(x.cpu.r[0]);
            Runtime::ArgReader args(x.cpu, x.mem, 2);
            args.word();
            const auto item = args.word(), change = args.word(), context = args.word();
            const int64_t status = Foundation::numberInt(x, Foundation::dictGet(x, change, "new"));
            state.states.push_back(status); state.contexts.push_back(context);
            if (status == 1 && item == state.primary) {
                state.playerAssigned &= state.player != 0;
                if (state.player) ObjC::rt(x).send(x.cpu, state.player, "play");
            }
        }, "v@:@@@^v");
        const auto observer = r.createInstance(cls.cls());
        r.setHost(observer, std::make_unique<ReadyObserver>());
        auto& state = r.host<ReadyObserver>(observer);
        const auto key = Foundation::makeString(cx, "status");
        const std::string mediaPath = proc.homePath() + "/ready-test.wav";
        // Valid PCM WAV with one silent mono sample, independently encoded.
        const uint8_t wave[] = {'R','I','F','F',38,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,
            1,0,1,0,0x40,0x1f,0,0,0x80,0x3e,0,0,2,0,16,0,'d','a','t','a',2,0,0,0,0,0};
        std::ofstream(mediaPath,std::ios::binary).write(reinterpret_cast<const char*>(wave),sizeof wave);
        const auto url = Foundation::makeFileURL(cx, mediaPath);
        auto item = [&] { return uint32_t(r.send(cpu, r.getClass("AVPlayerItem"), "playerItemWithURL:", {url})); };
        auto player = [&](uint32_t i) { return uint32_t(r.send(cpu, r.getClass("AVPlayer"), "playerWithPlayerItem:", {i})); };
        state.primary = item();
        r.send(cpu, state.primary, "addObserver:forKeyPath:options:context:", {observer,key,5,0x1234});
        check(uint32_t(r.send(cpu,state.primary,"status")) == 0 && state.states == std::vector<int64_t>{0},
              "AVPlayerItem Initial KVO reports Unknown before a player exists");
        state.player = player(state.primary);
        Foundation::mainRunLoop(proc).runOnce(cpu, 0);
        check(state.states == std::vector<int64_t>({0,1}) && state.playerAssigned &&
              uint32_t(r.send(cpu,state.player,"rate")) == 0x3f800000u,
              "AVPlayerItem readiness KVO arrives after player construction and can start playback");
        r.send(cpu,state.primary,"removeObserver:forKeyPath:context:",{observer,key,0x1234});
        const auto removed = item();
        r.send(cpu,removed,"addObserver:forKeyPath:options:context:",{observer,key,5,0x5678});
        const size_t before = state.states.size();
        player(removed);
        r.send(cpu,removed,"removeObserver:forKeyPath:context:",{observer,key,0x5678});
        Foundation::mainRunLoop(proc).runOnce(cpu, 0);
        check(state.states.size() == before && uint32_t(r.send(cpu,removed,"status")) == 1,
              "AVPlayerItem removing a status observer cancels pending readiness delivery");
        r.release(cpu, observer);
        std::filesystem::remove(mediaPath);
        {
            // -[NSRunLoop runMode:beforeDate:] runs the calling thread's loop whatever the receiver
            // (it is CFRunLoopRunInMode). Flurry runs [[NSRunLoop mainRunLoop] runMode:beforeDate:]
            // on its upload thread; main-loop sources must not run there (Pocket Ants drew its
            // world from that thread with no GL context).
            std::atomic<int64_t> ranOn{-1};
            Foundation::mainRunLoop(proc).post([&](CPU::Cpu&) { ranOn = int64_t(proc.currentThread().id); });
            const uint32_t fn = proc.defineFunction("test_runloop_receiver_thread", [](Ctx& x) {
                auto& rr = ObjC::rt(x);
                const ObjC::id mainLoop = ObjC::id(rr.send(x.cpu, ObjC::id(rr.getClass("NSRunLoop")), "mainRunLoop"));
                const ObjC::id now = ObjC::id(rr.send(x.cpu, ObjC::id(rr.getClass("NSDate")), "date"));
                rr.send(x.cpu, mainLoop, "runMode:beforeDate:", {Foundation::makeString(x, "kCFRunLoopDefaultMode"), now});
                rr.send(x.cpu, mainLoop, "runUntilDate:", {now});
                x.cpu.r[0] = 0;
            });
            const uint32_t tid = proc.startThread(fn, 0, 64u << 10, false);
            proc.joinThread(tid, nullptr);
            const bool notOnWorker = ranOn.load() == -1;
            Foundation::mainRunLoop(proc).runOnce(cpu, 0);
            check(notOnWorker && ranOn.load() == int64_t(proc.mainThread().id),
                  "NSRunLoop run methods run the calling thread's loop, never another thread's sources");
        }

    }

    {
        const auto backing = Foundation::makeDict(cx, {}, true, true);
        int reads = 0, writes = 0, removes = 0;
        auto subclass = r.defineClass("HVMPrimitiveDictionary", "NSMutableDictionary");
        subclass.method("objectForKey:", [&](Ctx& x) {
            ++reads; x.cpu.r[0] = uint32_t(r.send(x.cpu, backing, "objectForKey:", {x.cpu.r[2]}));
        }, "@@:@");
        subclass.method("setObject:forKey:", [&](Ctx& x) {
            ++writes; r.send(x.cpu, backing, "setObject:forKey:", {x.cpu.r[2],x.cpu.r[3]});
        }, "v@:@@");
        subclass.method("removeObjectForKey:", [&](Ctx& x) {
            ++removes; r.send(x.cpu, backing, "removeObjectForKey:", {x.cpu.r[2]});
        }, "v@:@");
        subclass.method("keyEnumerator", [&](Ctx& x) {
            x.cpu.r[0] = uint32_t(r.send(x.cpu, backing, "keyEnumerator"));
        }, "@@:");
        const auto instance = r.createInstance(subclass.cls());
        const auto key = Foundation::makeString(cx, "key"), value = Foundation::makeString(cx, "value");
        r.send(cpu, instance, "setObject:forKeyedSubscript:", {value,key});
        const bool got = ObjC::id(r.send(cpu, instance, "objectForKeyedSubscript:", {key})) == value;
        r.send(cpu, instance, "setObject:forKeyedSubscript:", {0,key});
        const bool absent = ObjC::id(r.send(cpu, instance, "objectForKeyedSubscript:", {key})) == 0;
        r.send(cpu, instance, "setValue:forKey:", {value,key});
        r.send(cpu, instance, "setValue:forKey:", {0,key});
        if (!(got && absent && reads == 2 && writes == 2 && removes == 2))
            std::printf("dictionary primitive results: got=%d absent=%d reads=%d writes=%d removes=%d\n", got, absent, reads, writes, removes);
        check(got && absent && reads == 2 && writes == 2 && removes == 2,
              "Dictionary subscript/KVC access dispatches subclass primitives and nil removes entries");

        // _class_getIvarLayout test
        const auto ivarLayoutAddr = proc.lookupSymbol("_class_getIvarLayout");
        const bool hasIvarLayout = ivarLayoutAddr.has_value() &&
            proc.callGuest(cpu, *ivarLayoutAddr, {r.getClass("NSObject")}) == 0;
        check(hasIvarLayout, "_class_getIvarLayout symbol exists and returns NULL for NSObject");
        r.send(cpu, instance, "setObject:forKey:", {value,key});
        const auto keys = ObjC::id(r.send(cpu, instance, "allKeys"));
        const auto values = ObjC::id(r.send(cpu, instance, "allValues"));
        check(uint32_t(r.send(cpu, keys, "count")) == 1 &&
              ObjC::id(r.send(cpu, keys, "objectAtIndex:", {0})) == key &&
              ObjC::id(r.send(cpu, values, "objectAtIndex:", {0})) == value,
              "Inherited dictionary allKeys/allValues enumerate subclass primitives without recursion");
        bool rejected = false;
        try { r.send(cpu, backing, "setObject:forKeyedSubscript:", {value,0}); }
        catch (const std::exception& e) { rejected = std::string(e.what()).find("key cannot be nil") != std::string::npos; }
        check(rejected, "Dictionary subscript assignment retains nil-key validation");
        r.release(cpu, instance); r.release(cpu, backing);
    }

    {
        const auto pool = r.poolPush();
        const auto pattern = Foundation::makeString(cx, "^([a-z]+)[0-9]+$");
        const auto regex = ObjC::id(r.send(cpu, r.getClass("NSRegularExpression"),
            "regularExpressionWithPattern:options:error:", {pattern, 1, 0}));
        const auto count = r.retainCount(regex);
        const auto copy = ObjC::id(r.send(cpu, regex, "copyWithZone:", {0}));
        const bool retained = copy == regex && r.retainCount(copy) == count + 1;
        r.poolPop(cpu, pool);
        const auto text = Foundation::makeString(cx, "Abc42");
        const auto match = ObjC::id(r.send(cpu, copy, "firstMatchInString:options:range:", {text, 0, 0, 5}));
        if (match) r.retain(cpu, match);
        const bool copied = retained && uint32_t(r.send(cpu, copy, "options")) == 1 &&
            uint32_t(r.send(cpu, copy, "numberOfCaptureGroups")) == 1 && match;
        r.release(cpu, copy);
        const auto heldRegex = match ? ObjC::id(r.send(cpu, match, "regularExpression")) : 0;
        check(copied && heldRegex == regex && uint32_t(r.send(cpu, heldRegex, "numberOfMatchesInString:options:range:", {text,0,0,5})) == 1,
              "Immutable regex copies retain compiled patterns across pools and match results retain their expression");
        if (match) r.release(cpu, match);
    }

    {
        // Issue #71: NSMethodSignature and NSInvocation lookup for replaced methods, protocols, class methods, and forwarding targets
        auto sigCls = r.getClass("NSMethodSignature");
        auto invCls = r.getClass("NSInvocation");
        bool sigOk = true;

        // 1. Swizzled / replaced method signature
        auto swizCls = r.defineClass("HVMSwizClass", "NSObject");
        swizCls.method("origMethod", [](Ctx& x) { x.cpu.r[0] = 0; }, "v@:");
        const auto swizInst = r.createInstance(swizCls.cls());
        const auto replaceFn = proc.registerHostFunction("hvm_test_repl", [](Ctx& x) { x.cpu.r[0] = 0; });
        const auto replTypesAddr = proc.allocCString("v@:i");
        const auto swizSel = r.sel("origMethod");
        const auto replAddr = proc.lookupSymbol("_class_replaceMethod");
        if (replAddr) proc.callGuest(cpu, *replAddr, {swizCls.cls(), swizSel, replaceFn, replTypesAddr});
        const auto swizSig = ObjC::id(r.send(cpu, swizInst, "methodSignatureForSelector:", {swizSel}));
        const auto swizInv = (swizSig && sigCls && invCls) ? ObjC::id(r.send(cpu, invCls, "invocationWithMethodSignature:", {swizSig})) : 0;
        bool b1 = swizSig != 0 && swizInv != 0 && uint32_t(r.send(cpu, swizSig, "numberOfArguments")) == 3;

        // 2. Protocol method signature lookup
        const uint32_t protoMem = proc.allocStatic(28);
        const uint32_t protoName = proc.allocCString("HVMTestSigProtocol");
        // A real objc2 method_list_t, as in a Mach-O __objc_const section:
        // { entsizeAndFlags = 12, count = 1, { name, types, imp = 0 } }.
        const uint32_t pListMem = proc.allocStatic(20);
        const uint32_t pSelName = proc.allocCString("protoMethod:");
        const uint32_t pTypeEnc = proc.allocCString("v@:i");
        proc.mem().write<uint32_t>(pListMem, 12);
        proc.mem().write<uint32_t>(pListMem + 4, 1);
        proc.mem().write<uint32_t>(pListMem + 8, pSelName);
        proc.mem().write<uint32_t>(pListMem + 12, pTypeEnc);
        proc.mem().write<uint32_t>(pListMem + 16, 0);
        proc.mem().write<uint32_t>(protoMem + 4, protoName);
        proc.mem().write<uint32_t>(protoMem + 12, pListMem); // required instance methods at +12

        auto protoCls = r.defineClass("HVMProtoClass", "NSObject");
        const auto addProtoAddr = proc.lookupSymbol("_class_addProtocol");
        if (addProtoAddr) proc.callGuest(cpu, *addProtoAddr, {protoCls.cls(), protoMem});
        const auto protoInst = r.createInstance(protoCls.cls());
        const auto protoSel = r.sel("protoMethod:");
        const auto protoSig = ObjC::id(r.send(cpu, protoInst, "methodSignatureForSelector:", {protoSel}));
        const auto protoInv = (protoSig && invCls) ? ObjC::id(r.send(cpu, invCls, "invocationWithMethodSignature:", {protoSig})) : 0;
        bool b2 = protoSig != 0 && protoInv != 0 && uint32_t(r.send(cpu, protoSig, "numberOfArguments")) == 3;

        // 3. Forwarding target method signature lookup
        auto targetCls = r.defineClass("HVMForwardTarget", "NSObject");
        targetCls.method("targetMethod:", [](Ctx& x) { x.cpu.r[0] = 0; }, "v@:@");
        const auto realTarget = r.createInstance(targetCls.cls());

        auto fwdCls = r.defineClass("HVMForwarder", "NSObject");
        fwdCls.method("forwardingTargetForSelector:", [realTarget](Ctx& x) { x.cpu.r[0] = realTarget; }, "@@::");
        const auto fwdInst = r.createInstance(fwdCls.cls());
        const auto targetSel = r.sel("targetMethod:");
        const auto fwdSig = ObjC::id(r.send(cpu, fwdInst, "methodSignatureForSelector:", {targetSel}));
        const auto fwdInv = (fwdSig && invCls) ? ObjC::id(r.send(cpu, invCls, "invocationWithMethodSignature:", {fwdSig})) : 0;
        bool b3 = fwdSig != 0 && fwdInv != 0 && uint32_t(r.send(cpu, fwdSig, "numberOfArguments")) == 3;

        sigOk = b1 && b2 && b3;

        r.release(cpu, swizInst);
        r.release(cpu, protoInst);
        r.release(cpu, realTarget);
        r.release(cpu, fwdInst);

        check(sigOk, "Method signature and NSInvocation lookup supports replaced methods, protocols, and forwarding targets");
    }

    {
        auto call = [&](const char* sym, std::initializer_list<uint32_t> args) { return uint32_t(proc.callGuest(cpu, *proc.lookupSymbol(sym), args)); };
        const uint32_t out = proc.heap().allocZeroed(32);
        auto refs = [&](uint32_t port) { return uint32_t(call("_mach_port_get_refs", {0x103, port, 0, out})) == 0 ? proc.mem().read<uint32_t>(out) : 0; };
        check(call("_mach_port_mod_refs", {0x103, 0x103, 0, 1}) == 0 && refs(0x103) == 2 &&
              call("_mach_port_deallocate", {0x103, 0x103}) == 0 && refs(0x103) == 1,
              "Mach task send references increment and deallocate symmetrically");

        // Direct HLE _syscall validation
        const uint32_t errAddr = proc.currentThread().errnoAddr ? proc.currentThread().errnoAddr : proc.heap().allocZeroed(4);
        proc.currentThread().errnoAddr = errAddr;
        const auto getErrno = [&]() { return proc.mem().read<uint32_t>(errAddr); };
        check(call("_syscall", {20}) == 1234, "_syscall(20) returns getpid");
        check(int32_t(call("_syscall", {286})) == -1 && getErrno() == 3, "_syscall(286) returns -1 and ESRCH (3)");
        check(call("_syscall", {372}) == proc.currentThread().id, "_syscall(372) returns thread_selfid");
        check(int32_t(call("_syscall", {999})) == -1 && getErrno() == 78, "_syscall(unhandled) returns -1 and ENOSYS (78)");

        // POSIX named semaphores are unimplemented, so no handle is a valid open semaphore.
        for (uint32_t handle : {0u, 0xFFFFFFFFu, 0x1234u}) {
            proc.mem().write<uint32_t>(errAddr, 0);
            check(int32_t(call("_sem_close", {handle})) == -1 && getErrno() == 22,
                  "sem_close of an unopened handle returns -1 and EINVAL (22)");
        }
        check(call("_mach_port_mod_refs", {42, 0x103, 0, 1}) == 16 &&
              call("_mach_port_mod_refs", {0x103, 42, 0, 1}) == 15 &&
              call("_mach_port_mod_refs", {0x103, 0x103, 1, 1}) == 17 &&
              call("_mach_port_mod_refs", {0x103, 0x103, 9, 1}) == 18 &&
              call("_mach_port_mod_refs", {0x103, 0x103, 0, uint32_t(-2)}) == 18 &&
              call("_mach_port_mod_refs", {0x103, 0x103, 0, 65535}) == 19 && refs(0x103) == 1,
              "Mach rejects invalid tasks/names/rights and reference underflow/overflow without mutation");
        const uint32_t th = uint32_t(call("_mach_thread_self", {}));
        call("_mach_thread_self", {});
        const bool twice = refs(th) == 2;
        call("_mach_port_deallocate", {0x103, th});
        call("_mach_port_deallocate", {0x103, th});
        check(twice && refs(th) == 0 && call("_mach_port_deallocate", {0x103, th}) == 15 &&
              call("_mach_thread_self", {}) == th && refs(th) == 1,
              "Mach thread self acquires references and recreates an exhausted send right");
        call("_task_threads", {0x103, out, out + 4});
        const uint32_t list = proc.mem().read<uint32_t>(out), count = proc.mem().read<uint32_t>(out + 4);
        bool found = false;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t port = proc.mem().read<uint32_t>(list + i*4);
            if (port == th) found = refs(th) == 2;
            call("_mach_port_deallocate", {0x103, port});
        }
        check(found && refs(th) == 1, "task_threads exports owned send rights for the same synthetic threads");
        const uint32_t src = proc.heap().alloc(32), dst = proc.heap().alloc(32);
        proc.mem().write<uint32_t>(src, 0x12345678);
        check(call("_vm_read_overwrite", {0x103, src, 4, dst, out}) == 0 &&
              proc.mem().read<uint32_t>(dst) == 0x12345678 && proc.mem().read<uint32_t>(out) == 4 &&
              call("_vm_read_overwrite", {0x103, 0xFFFFFFFE, 8, dst, out}) == 1 &&
              proc.mem().read<uint32_t>(out) == 0 && call("_vm_read_overwrite", {0x103, 0, 4, dst, out}) == 1,
              "vm_read_overwrite copies guest memory and rejects unmapped/wrapping ranges");
        proc.mem().write<uint64_t>(out, 4096);
        proc.mem().write<uint32_t>(out+8, 0xDEADBEEF);
        check(call("_mach_make_memory_entry_64", {0x103,out,src,0,1,out+8,0}) == 46 &&
              proc.mem().read<uint32_t>(out+8) == 0,
              "Mach memory entries report unsupported capability without exporting a fabricated handle");
    }
    {
        auto call = [&](const char* sym, std::initializer_list<uint32_t> args) { return uint32_t(proc.callGuest(cpu, *proc.lookupSymbol(sym), args)); };
        const uint32_t token = r.poolPush();
        const uint32_t set = call("_CFCharacterSetCreateMutable", {0});
        call("_CFCharacterSetAddCharactersInRange", {set,0x590,0x70});
        const uint32_t copy = call("_CFCharacterSetCreateCopy", {0,set});
        r.poolPop(cpu,token);
        check(call("_CFCharacterSetIsCharacterMember", {set,0x5D0}) && call("_CFCharacterSetIsCharacterMember", {copy,0x5D0}) &&
              !call("_CFCharacterSetIsCharacterMember", {copy,'A'}),
              "CF mutable character sets and copies own their lifetime across autorelease pool drains");
        call("_CFRelease", {copy}); call("_CFRelease", {set});
    }
    {
        auto call = [&](const char* sym, std::initializer_list<uint32_t> args) { return uint32_t(proc.callGuest(cpu, *proc.lookupSymbol(sym), args)); };
        const uint32_t context = proc.heap().allocZeroed(36), info = proc.heap().allocZeroed(16);
        int retains = 0, releases = 0, frees = 0;
        const uint32_t retain = proc.defineFunction("test_allocator_retain", [&](Ctx& x) { ++retains; x.cpu.r[0] += 4; });
        const uint32_t release = proc.defineFunction("test_allocator_release", [&](Ctx& x) { if (x.cpu.r[0] == info + 4) ++releases; });
        const uint32_t allocate = proc.defineFunction("test_allocator_alloc", [&](Ctx& x) { x.cpu.r[0] = x.cpu.r[2] == info + 4 ? x.proc.heap().alloc(x.cpu.r[0]) : 0; });
        const uint32_t deallocate = proc.defineFunction("test_allocator_free", [&](Ctx& x) { if (x.cpu.r[1] == info + 4) ++frees; x.proc.heap().free(x.cpu.r[0]); });
        for (auto [off, value] : std::initializer_list<std::pair<uint32_t,uint32_t>>{{4,info},{8,retain},{12,release},{20,allocate},{28,deallocate}})
            proc.mem().write<uint32_t>(context + off, value);
        const uint32_t a = call("_CFAllocatorCreate", {0, context});
        const uint32_t got = proc.heap().allocZeroed(36);
        call("_CFAllocatorGetContext", {a, got});
        const uint32_t block = call("_CFAllocatorAllocate", {a, 64, 0});
        check(a && block && retains == 1 && proc.mem().read<uint32_t>(got + 4) == info + 4 &&
              call("_CFGetTypeID", {a}) == call("_CFAllocatorGetTypeID", {}),
              "CFAllocatorCreate copies the context, retains info and invokes guest allocation callbacks");
        call("_CFAllocatorDeallocate", {a, block});
        const uint32_t borrowed = call("_CFAllocatorAllocate", {a, 16, 0});
        const uint32_t noCopy = call("_CFDataCreateWithBytesNoCopy", {0, borrowed, 16, a});
        call("_CFRelease", {a});
        check(frees == 1 && releases == 0 && call("_CFDataGetBytePtr", {noCopy}) == borrowed,
              "CFData no-copy preserves the original pointer and retains its custom deallocator");
        call("_CFRelease", {noCopy});
        check(frees == 2 && releases == 1, "CFData destruction deallocates borrowed bytes and releases allocator context exactly once");
        const uint32_t nullAllocator = proc.mem().read<uint32_t>(*proc.lookupSymbol("_kCFAllocatorNull"));
        check(call("_CFAllocatorAllocate", {nullAllocator, 32, 0}) == 0 && call("_CFAllocatorAllocate", {0, 32, 0}) != 0,
              "CFAllocatorNull refuses allocation while the system default allocates");
    }
    {
        // Issue #312: CoreGraphics accepts TrueType files whose sfnt version is
        // 'true' (Apple TrueType) as well as 0x00010000. Same bytes as kTestFont
        // with only the 4-byte version tag changed; cmap must be readable.
        auto call2 = [&](const char* sym, uint32_t a, uint32_t b) {
            return uint32_t(proc.callGuest(cpu, *proc.lookupSymbol(sym), {a, b}));
        };
        uint8_t appleTrue[sizeof kTestFont];
        std::memcpy(appleTrue, kTestFont, sizeof kTestFont);
        appleTrue[0] = 't'; appleTrue[1] = 'r'; appleTrue[2] = 'u'; appleTrue[3] = 'e';
        const auto data = Foundation::makeData(cx, appleTrue, uint32_t(sizeof appleTrue), true);
        const uint32_t provider = call2("_CGDataProviderCreateWithCFData", data, 0);
        const uint32_t cg = call2("_CGFontCreateWithDataProvider", provider, 0);
        const uint32_t cmapTable = cg ? call2("_CGFontCopyTableForTag", cg, 0x636D6170) : 0;
        check(cg != 0 && cmapTable != 0,
              "CGFontCreateWithDataProvider accepts an Apple 'true' sfnt and CGFontCopyTableForTag returns its cmap");
        if (cmapTable) call2("_CFRelease", cmapTable, 0);
        if (cg) call2("_CGFontRelease", cg, 0);
        call2("_CGDataProviderRelease", provider, 0);
        call2("_CFRelease", data, 0);
    }
    {
        // Older fonts (Temple Run's Cheboyga) give the format 4 cmap's final 0xFFFF segment an
        // idRangeOffset of 0xFFFF that points outside the table. iOS loads them; so must we.
        auto call2 = [&](const char* sym, uint32_t a, uint32_t b) {
            return uint32_t(proc.callGuest(cpu, *proc.lookupSymbol(sym), {a, b}));
        };
        std::vector<uint8_t> font(kTestFont, kTestFont + sizeof kTestFont);
        auto u16 = [&](size_t p) { return uint32_t(font[p]) << 8 | font[p + 1]; };
        auto u32 = [&](size_t p) { return u16(p) << 16 | u16(p + 2); };
        bool patched = false;
        for (uint32_t i = 0; i < u16(4); ++i) {
            if (u32(12 + i * 16) != 0x636D6170) continue;
            const size_t cmap = u32(12 + i * 16 + 8);
            for (uint32_t m = 0; m < u16(cmap + 2); ++m) {
                const size_t sub = cmap + u32(cmap + 4 + m * 8 + 4);
                if (u16(sub) != 4) continue;
                const uint32_t segs = u16(sub + 6) / 2;
                const size_t range = sub + 16 + segs * 6 + (segs - 1) * 2;
                if (u16(sub + 14 + (segs - 1) * 2) == 0xFFFF) { font[range] = 0xFF; font[range + 1] = 0xFF; patched = true; }
            }
        }
        const auto data = Foundation::makeData(cx, font.data(), uint32_t(font.size()), true);
        const uint32_t provider = call2("_CGDataProviderCreateWithCFData", data, 0);
        const uint32_t cg = call2("_CGFontCreateWithDataProvider", provider, 0);
        check(patched && cg != 0, "CGFontCreateWithDataProvider loads a font whose 0xFFFF cmap terminator has a bogus idRangeOffset");
        if (cg) call2("_CGFontRelease", cg, 0);
        call2("_CGDataProviderRelease", provider, 0);
        call2("_CFRelease", data, 0);
    }
    {
        // Issue #310: CoreData option and store-type constants are exported data
        // symbols whose value is an NSString equal to the documented string.
        // An unexported constant binds as NULL and becomes a nil dictionary key.
        struct CoreDataConstant { const char* sym; const char* want; };
        const CoreDataConstant kCoreDataConstants[] = {
            {"_NSSQLiteStoreType", "SQLite"},
            {"_NSBinaryStoreType", "Binary"},
            {"_NSInMemoryStoreType", "InMemory"},
            {"_NSMigratePersistentStoresAutomaticallyOption", "NSMigratePersistentStoresAutomaticallyOption"},
            {"_NSInferMappingModelAutomaticallyOption", "NSInferMappingModelAutomaticallyOption"},
            {"_NSSQLitePragmasOption", "NSSQLitePragmasOption"},
            {"_NSReadOnlyPersistentStoreOption", "NSReadOnlyPersistentStoreOption"}
        };
        for (const CoreDataConstant& k : kCoreDataConstants) {
            const auto cell = proc.lookupSymbol(k.sym);
            uint32_t str = 0;
            if (cell) std::memcpy(&str, proc.mem().host(*cell), 4);
            const std::string what = std::string("CoreData constant ") + k.sym + " is a non-nil NSString equal to its documented value";
            check(str != 0 && Foundation::utf8(cx, str) == k.want, what.c_str());
        }
    }
    {
        auto call = [&](const char* sym, std::initializer_list<uint32_t> args) { return uint32_t(proc.callGuest(cpu, *proc.lookupSymbol(sym), args)); };
        const auto face = Graphics::TrueTypeFont::load(std::vector<uint8_t>(std::begin(kTestFont), std::end(kTestFont)));
        check(face && face->glyph('A') == 2 && face->glyph('Z') == 0 && face->ascent(20) == 16 &&
              face->descent(20) == 4 && face->advance(2, 20) == 12,
              "TrueType cmap and metrics use the original font's units per em");
        // Image drawing: an opaque image scaled 2x smoothly (fixed-point path) stays within one step
        // of exact bilinear interpolation; an opaque image drawn unscaled is copied exactly; a tiny
        // horizontal scale draws nothing out of range.
        {
            Graphics::Image img;
            img.width = 4; img.height = 2; img.opaque = true;
            img.rgba.resize(4 * 2 * 4);
            for (int y = 0; y < 2; ++y)
                for (int x = 0; x < 4; ++x) {
                    uint8_t* p = &img.rgba[size_t((y * 4 + x) * 4)];
                    p[0] = uint8_t(x * 80); p[1] = uint8_t(y * 200); p[2] = 30; p[3] = 255;
                }
            std::vector<uint8_t> px(8 * 4 * 4, 0);
            Graphics::Surface dst{px.data(), 8, 4, 8 * 4, Graphics::PixelFormat::RGBA, true};
            Graphics::Clip all; all.x0 = 0; all.y0 = 0; all.x1 = 8; all.y1 = 4;
            Graphics::drawImage(dst, img, Graphics::Affine::scale(2, 2), all, 1, true, Graphics::BlendMode::Normal, nullptr);
            bool close = true;
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 8; ++x) {
                    const float fx = std::clamp((x + 0.5f) / 2 - 0.5f, 0.0f, 3.0f), fy = std::clamp((y + 0.5f) / 2 - 0.5f, 0.0f, 1.0f);
                    const float r = fx * 80, g = fy * 200;
                    const uint8_t* p = &px[size_t((y * 8 + x) * 4)];
                    close = close && std::fabs(p[0] - r) <= 1.01f && std::fabs(p[1] - g) <= 1.01f && p[2] == 30 && p[3] == 255;
                }
            std::vector<uint8_t> px2(4 * 2 * 4, 0);
            Graphics::Surface dst2{px2.data(), 4, 2, 4 * 4, Graphics::PixelFormat::RGBA, true};
            Graphics::Clip all2; all2.x0 = 0; all2.y0 = 0; all2.x1 = 4; all2.y1 = 2;
            Graphics::drawImage(dst2, img, Graphics::Affine{}, all2, 1, true, Graphics::BlendMode::Normal, nullptr);
            std::vector<uint8_t> px3(8 * 4 * 4, 7);
            Graphics::Surface dst3{px3.data(), 8, 4, 8 * 4, Graphics::PixelFormat::RGBA, true};
            Graphics::drawImage(dst3, img, Graphics::Affine::scale(1e-10f, 1), all, 1, true, Graphics::BlendMode::Normal, nullptr);
            check(close && px2 == img.rgba, "image draws: smooth 2x scaling within a step of bilinear, unscaled opaque copies exactly");
        }
        check(!Graphics::TrueTypeFont::load({0,1,0,0}) && !Graphics::TrueTypeFont::load(std::vector<uint8_t>(100,0)),
              "TrueType loading rejects truncated and invalid font headers");
        const auto data = Foundation::makeData(cx, kTestFont, sizeof kTestFont, true);
        const uint32_t provider = uint32_t(call("_CGDataProviderCreateWithCFData", {data}));
        const uint32_t cg = uint32_t(call("_CGFontCreateWithDataProvider", {provider}));
        const uint32_t ct = uint32_t(call("_CTFontCreateWithGraphicsFont", {cg, std::bit_cast<uint32_t>(20.0f), 0, 0}));

        // CGFont table copying and metric queries (B-109)
        const uint32_t cmapTable = uint32_t(call("_CGFontCopyTableForTag", {cg, 0x636D6170}));
        const uint32_t bogusTable = uint32_t(call("_CGFontCopyTableForTag", {cg, 0x78797A77}));
        const uint32_t units = uint32_t(call("_CGFontGetUnitsPerEm", {cg}));
        const int32_t ascent = int32_t(call("_CGFontGetAscent", {cg}));
        const int32_t descent = int32_t(call("_CGFontGetDescent", {cg}));
        const int32_t capHeight = int32_t(call("_CGFontGetCapHeight", {cg}));
        const uint32_t numGlyphs = uint32_t(call("_CGFontGetNumberOfGlyphs", {cg}));
        const uint32_t advBuf = proc.heap().allocZeroed(16);
        const uint32_t testGlyphsMem = proc.heap().allocZeroed(8);
        proc.mem().write<uint16_t>(testGlyphsMem, 2);
        proc.mem().write<uint16_t>(testGlyphsMem + 2, 1);
        const bool advOk = call("_CGFontGetGlyphAdvances", {cg, testGlyphsMem, 2, advBuf});
        const int32_t advA = proc.mem().read<int32_t>(advBuf);
        check(cmapTable != 0 && bogusTable == 0 && units == 1000 && ascent == 800 && descent == -200 &&
              capHeight == 700 && numGlyphs == 3 && advOk && advA == 600,
              "CGFontCopyTableForTag and CGFontGet* unscaled design metrics match TrueType tables");
        call("_CFRelease", {cmapTable});
        proc.heap().free(advBuf);
        proc.heap().free(testGlyphsMem);

        call("_CGDataProviderRelease", {provider}); call("_CFRelease", {data}); call("_CGFontRelease", {cg});
        {
            // CTFontDescriptor attribute dictionaries: kCTFontNameAttribute reaches the descriptor, its copy and the font.
            const uint32_t mockName = Foundation::makeString(cx, "Arial", true);
            // Unresolved _k* imports are materialised by the loader as CFStrings holding the symbol name without the
            // underscore (the convention text_font() already relies on for kCTFontAttributeName); use that value.
            const uint32_t fontNameKey = Foundation::makeString(cx, "kCTFontNameAttribute", true);
            const uint32_t fontAttrs = Foundation::makeDict(cx, {{fontNameKey, mockName}}, true);
            const uint32_t descWithAttrs = uint32_t(call("_CTFontDescriptorCreateWithAttributes", {fontAttrs}));
            const uint32_t createdFont = uint32_t(call("_CTFontCreateWithFontDescriptor", {descWithAttrs, 0, 0}));
            const auto familyAttrs = uint32_t(call("_CTFontCopyFamilyName", {createdFont}));
            const uint32_t otherName = Foundation::makeString(cx, "Courier", true);
            const uint32_t otherAttrs = Foundation::makeDict(cx, {{fontNameKey, otherName}}, true);
            const uint32_t renamed = uint32_t(call("_CTFontDescriptorCreateCopyWithAttributes", {descWithAttrs, otherAttrs}));
            const uint32_t renamedFont = uint32_t(call("_CTFontCreateWithFontDescriptor", {renamed, 0, 0}));
            const auto renamedFamily = uint32_t(call("_CTFontCopyFamilyName", {renamedFont}));
            const uint32_t againFont = uint32_t(call("_CTFontCreateWithFontDescriptor", {descWithAttrs, 0, 0}));
            const auto againFamily = uint32_t(call("_CTFontCopyFamilyName", {againFont}));
            // Renaming a copy of the existing ct fixture's descriptor changes the copy only.
            const uint32_t fixtureDesc = uint32_t(call("_CTFontCopyFontDescriptor", {ct}));
            const uint32_t fixtureRenamed = uint32_t(call("_CTFontDescriptorCreateCopyWithAttributes", {fixtureDesc, otherAttrs}));
            const uint32_t fixtureFont = uint32_t(call("_CTFontCreateWithFontDescriptor", {fixtureRenamed, 0, 0}));
            const auto fixtureFamily = uint32_t(call("_CTFontCopyFamilyName", {fixtureFont}));
            const auto ctFamily = uint32_t(call("_CTFontCopyFamilyName", {ct}));
            const uint32_t emptyDesc = uint32_t(call("_CTFontDescriptorCreateWithAttributes", {0}));
            const uint32_t emptyFont = uint32_t(call("_CTFontCreateWithFontDescriptor", {emptyDesc, 0, 0}));
            const auto emptyFamily = uint32_t(call("_CTFontCopyFamilyName", {emptyFont}));
            // An attribute key other than kCTFontNameAttribute is not modelled: it must be reported, not silently dropped.
            const size_t missingBefore = proc.missingCount();
            const uint32_t sizeKey = Foundation::makeString(cx, "kCTFontSizeAttribute", true);
            const uint32_t sizeVal = Foundation::makeString(cx, "12", true);
            const uint32_t unsupportedAttrs = Foundation::makeDict(cx, {{sizeKey, sizeVal}}, true);
            const uint32_t unsupportedDesc = uint32_t(call("_CTFontDescriptorCreateWithAttributes", {unsupportedAttrs}));
            const bool reportedUnsupported = unsupportedDesc && proc.missingCount() == missingBefore + 1;
            check(reportedUnsupported, "CTFontDescriptor attribute APIs report unsupported attribute keys instead of ignoring them");
            check(descWithAttrs && renamed && createdFont && Foundation::utf8(cx, familyAttrs) == "Arial" &&
                  Foundation::utf8(cx, renamedFamily) == "Courier" && Foundation::utf8(cx, againFamily) == "Arial" &&
                  Foundation::utf8(cx, emptyFamily) == "Helvetica" &&
                  Foundation::utf8(cx, fixtureFamily) == "Courier" && Foundation::utf8(cx, ctFamily) == "HVM Test",
                  "CTFontDescriptor attribute APIs apply kCTFontNameAttribute, leave the source descriptor alone, and default an unnamed descriptor to Helvetica");
        }
        const uint32_t desc = uint32_t(call("_CTFontCopyFontDescriptor", {ct}));
        const uint32_t copy = uint32_t(call("_CTFontCreateWithFontDescriptor", {desc, 0, 0}));
        const auto family = uint32_t(call("_CTFontCopyFamilyName", {copy}));
        check(ct && copy && Foundation::utf8(cx, family) == "HVM Test",
              "CGFont/CTFont own custom font bytes across provider release and descriptor copies");
        const uint32_t chars = proc.heap().allocZeroed(8), glyphs = chars + 4;
        proc.mem().write<uint16_t>(chars, 'A'); proc.mem().write<uint16_t>(chars + 2, 'Z');
        const bool all = call("_CTFontGetGlyphsForCharacters", {ct, chars, glyphs, 2});
        const uint32_t rect = proc.heap().allocZeroed(16);
        call("_CTFontGetBoundingRectsForGlyphs", {rect, ct, 1, glyphs, 0, 1});
        const auto bounds = CG::readStruct<CG::CGRect>(cx, rect);
        check(!all && proc.mem().read<uint16_t>(glyphs) == 2 && proc.mem().read<uint16_t>(glyphs + 2) == 0 &&
              std::fabs(bounds.origin.x - 2) < 0.01f && std::fabs(bounds.size.width - 8) < 0.01f && std::fabs(bounds.size.height - 14) < 0.01f,
              "CoreText glyph mapping and struct-return bounding boxes preserve missing glyphs and geometry");
        const uint32_t bitmap = uint32_t(call("_CGBitmapContextCreate", {0,32,32,8,32,0,7}));
        const uint32_t pos = proc.heap().allocZeroed(8);
        call("_CGContextTranslateCTM", {bitmap, 0, std::bit_cast<uint32_t>(24.0f)});
        call("_CGContextSetAllowsFontSubpixelPositioning", {bitmap, 0});
        call("_CGContextSetShouldSubpixelQuantizeFonts", {bitmap, 0});
        call("_CTFontDrawGlyphs", {ct, glyphs, pos, 1, bitmap});
        const uint32_t pixels = uint32_t(call("_CGBitmapContextGetData", {bitmap}));
        int lit = 0;
        for (int i = 0; i < 1024; ++i) lit += proc.mem().read<uint8_t>(pixels + i) != 0;
        check(lit > 20 && lit < 150, "CTFontDrawGlyphs rasterizes the custom outline into guest A8 pixels");
        const uint32_t key = Foundation::makeString(cx, "kCTFontAttributeName", true);
        const uint32_t attrs = Foundation::makeDict(cx, {{key,ct}}, true);
        const uint32_t string = Foundation::makeString(cx, "AA AA", true);
        const uint32_t attributed = uint32_t(call("_CFAttributedStringCreate", {0,string,attrs}));
        const uint32_t setter = uint32_t(call("_CTTypesetterCreateWithAttributedString", {attributed}));
        const uint64_t width = std::bit_cast<uint64_t>(40.0);
        const uint32_t lineBreak = uint32_t(call("_CTTypesetterSuggestLineBreak", {setter,0,uint32_t(width),uint32_t(width >> 32)}));
        const uint32_t line = uint32_t(call("_CTTypesetterCreateLine", {setter,0,3}));
        const uint32_t runs = uint32_t(call("_CTLineGetGlyphRuns", {line}));
        const auto items = Foundation::arrayItems(cx, runs);
        call("_CFRelease", {attributed}); call("_CFRelease", {setter}); call("_CFRelease", {attrs}); call("_CFRelease", {string}); call("_CFRelease", {key});
        const uint32_t copies = proc.heap().allocZeroed(32);
        if (!items.empty()) call("_CTRunGetGlyphs", {items[0],0,0,copies});
        const auto metrics = proc.callGuest(cpu, *proc.lookupSymbol("_CTLineGetTypographicBounds"), {line,0,0,0});
        check(lineBreak == 3 && items.size() == 1 && call("_CTLineGetGlyphRuns", {line}) == runs &&
              call("_CFGetRetainCount", {line}) == 1 && call("_CTRunGetGlyphCount", {items[0]}) == 3 &&
              proc.mem().read<uint16_t>(copies) == 2 && std::bit_cast<double>(metrics) == 36,
              "CoreText line breaking and owned glyph runs preserve custom metrics after source release");
        {
            // CTLineGetOffsetForStringIndex returns a CGFloat (32-bit float in r0) and also stores it through
            // secondaryOffset; the canary after the four bytes must stay untouched.
            const uint32_t sec = proc.heap().allocZeroed(8);
            proc.mem().write<uint32_t>(sec, 0xDEADBEEF);
            proc.mem().write<uint32_t>(sec + 4, 0xCAFEF00D);
            const uint32_t ret = uint32_t(proc.callGuest(cpu, *proc.lookupSymbol("_CTLineGetOffsetForStringIndex"), {line, 2, sec}));
            check(std::bit_cast<float>(ret) == 24.0f && proc.mem().read<float>(sec) == 24.0f &&
                  proc.mem().read<uint32_t>(sec + 4) == 0xCAFEF00D,
                  "CTLineGetOffsetForStringIndex returns the summed advance as a float and writes secondaryOffset");
            proc.heap().free(sec);
        }
        call("_CFRelease", {line});
        call("_CGContextRelease", {bitmap}); call("_CFRelease", {family}); call("_CFRelease", {copy});
        call("_CFRelease", {desc}); call("_CFRelease", {ct});
    }

    // Directory search must distinguish user and local domains. Clash Royale
    // uses NSCachesDirectory (13) with NSLocalDomainMask (2) during startup.
    {
        auto paths = [&](uint32_t directory, uint32_t domains, uint32_t expand) {
            const auto symbol = proc.lookupSymbol("_NSSearchPathForDirectoriesInDomains");
            if (!symbol) return std::vector<ObjC::id>{};
            const auto array = ObjC::id(proc.callGuest(cpu, *symbol, {directory, domains, expand}));
            return Foundation::arrayItems(cx, array);
        };
        const auto user = paths(13, 1, 1);
        const auto local = paths(13, 2, 1);
        const auto all = paths(13, 0xFFFF, 1);
        const std::string userPath = proc.guestHomePath() + "/Library/Caches";
        const std::string localPath = proc.guestHomePath() + "/LocalDomain/Library/Caches";
        check(user.size() == 1 && Foundation::utf8(cx, user[0]) == userPath,
              "user-domain cache search keeps the app's existing cache path");
        check(local.size() == 1 && Foundation::utf8(cx, local[0]) == localPath,
              "local-domain cache search returns a separate guest-backed directory");
        check(all.size() == 2 && Foundation::utf8(cx, all[0]) == userPath &&
              Foundation::utf8(cx, all[1]) == localPath,
              "all-domain cache search returns user then local without duplicates");
        const auto unexpanded = paths(13, 2, 0);
        const auto tilde = paths(13, 1, 0);
        check(unexpanded.size() == 1 && Foundation::utf8(cx, unexpanded[0]) == localPath &&
              tilde.size() == 1 && Foundation::utf8(cx, tilde[0]) == "~/Library/Caches",
              "tilde expansion affects user paths but not absolute local-domain paths");
        check(paths(13, 0, 1).empty() && paths(13, 4, 1).empty() &&
              paths(9, 2, 1).empty() && paths(0xFFFF, 2, 1).empty(),
              "invalid, unmounted network, and personal local-domain searches stay empty");
        const ObjC::id fm = ObjC::id(r.send(cpu, r.getClass("NSFileManager"), "defaultManager"));
        const auto urls = Foundation::arrayItems(cx, ObjC::id(r.send(cpu, fm, "URLsForDirectory:inDomains:", {13, 2})));
        check(urls.size() == 1 &&
              Foundation::utf8(cx, ObjC::id(r.send(cpu, urls[0], "path"))) == localPath,
              "NSFileManager URL directory search agrees with the C path API");
    }

    // NSUbiquitousKeyValueStore keeps values in the sandbox (no iCloud) so they survive a relaunch.
    {
        const ObjC::id store = ObjC::id(r.send(cpu, r.getClass("NSUbiquitousKeyValueStore"), "defaultStore"));
        r.send(cpu, store, "setObject:forKey:", {Foundation::makeString(cx, "level-7"), Foundation::makeString(cx, "progress")});
        const bool synced = (r.send(cpu, store, "synchronize") & 0xFF) != 0;
        auto saved = Plist::parseFile(proc.homePath() + "/Library/Preferences/HydrogenVM.UbiquitousKeyValueStore.plist");
        const Plist::Value* v = saved ? saved->get("progress") : nullptr;
        check(synced && v && v->s == "level-7", "NSUbiquitousKeyValueStore writes its values to the sandbox");
    }

    // Fixed-offset zones are used by HockeyApp's UTC date formatter on launch.
    {
        const auto cls = r.getClass("NSTimeZone");
        auto zone = [&](int32_t seconds) {
            return ObjC::id(r.send(cpu, cls, "timeZoneForSecondsFromGMT:", {uint32_t(seconds)}));
        };
        const auto gmt = zone(0), east = zone(19800), west = zone(-18000);
        check(gmt && east && west &&
              int32_t(r.send(cpu, gmt, "secondsFromGMT")) == 0 &&
              int32_t(r.send(cpu, east, "secondsFromGMT")) == 19800 &&
              int32_t(r.send(cpu, west, "secondsFromGMT")) == -18000,
              "NSTimeZone fixed-offset factory supports GMT and signed offsets");
        check(Foundation::utf8(cx, ObjC::id(r.send(cpu, gmt, "name"))) == "GMT" &&
              Foundation::utf8(cx, ObjC::id(r.send(cpu, east, "name"))) == "GMT+05:30" &&
              Foundation::utf8(cx, ObjC::id(r.send(cpu, west, "abbreviation"))) == "GMT-05:00",
              "fixed-offset time zones expose stable GMT names and abbreviations");
        const auto winter = Foundation::makeDate(cx, 0), summer = Foundation::makeDate(cx, 180 * 86400.0);
        check(int32_t(r.send(cpu, east, "secondsFromGMTForDate:", {winter})) == 19800 &&
              int32_t(r.send(cpu, east, "secondsFromGMTForDate:", {summer})) == 19800 &&
              !uint32_t(r.send(cpu, east, "isDaylightSavingTime")) &&
              !uint32_t(r.send(cpu, west, "isDaylightSavingTimeForDate:", {summer})),
              "fixed-offset time zones keep their offset and never enter daylight saving");
        const auto utc = ObjC::id(r.send(cpu, cls, "timeZoneWithName:", {Foundation::makeString(cx, "UTC")}));
        check(utc && int32_t(r.send(cpu, utc, "secondsFromGMT")) == 0,
              "named UTC zone is independent of the host's local timezone");
    }

    // 2a0. Guest screen overrides (issue #281, modes 3 and 4): opt-in by environment, per launch.
    {
        auto screenSize = [&](const char* sel) { return UIKit::getRect(cx, ObjC::id(r.send(cpu, r.getClass("UIScreen"), "mainScreen")), sel); };
        auto& u = UIKit::ui(cx);
        const float w0 = u.screenW, h0 = u.screenH;
        const auto processW = proc.config().screenWidth, processH = proc.config().screenHeight;
        const auto orientation = u.statusBarOrientation;
        ::setenv("HVM_VIRTUAL_SCREEN_HEIGHT", "694", 1);
        UIKit::applyGuestScreenEnv(cx);
        check(std::abs(screenSize("bounds").h - 694.0f) < 0.01f && std::abs(screenSize("bounds").w - w0) < 0.01f,
              "HVM_VIRTUAL_SCREEN_HEIGHT gives the guest a taller UIScreen");
        ::setenv("HVM_VIRTUAL_SCREEN_HEIGHT", "99999", 1);
        u.screenH = h0;
        UIKit::applyGuestScreenEnv(cx);
        check(std::abs(screenSize("bounds").h - h0) < 0.01f, "an out-of-range virtual height is ignored");
        ::setenv("HVM_VIRTUAL_SCREEN_HEIGHT", "abc", 1);
        UIKit::applyGuestScreenEnv(cx);
        check(std::abs(screenSize("bounds").h - h0) < 0.01f, "a non-numeric virtual height is ignored");
        ::unsetenv("HVM_VIRTUAL_SCREEN_HEIGHT");
        UIKit::applyGuestScreenEnv(cx);
        check(std::abs(screenSize("bounds").h - h0) < 0.01f, "no variable keeps the previous screen");
        const bool hidden = u.statusBarHidden;
        u.statusBarHidden = false;
        u.statusBarOrientation = 1;
        ::setenv("HVM_SAFE_AREA", "44,0,34,0", 1);
        UIKit::applyGuestScreenEnv(cx);
        const Graphics::Rect af = screenSize("applicationFrame");
        check(std::abs(af.y - 44.0f) < 0.01f && std::abs(af.h - (h0 - 44.0f - 34.0f)) < 0.01f,
              "safe-area insets shrink applicationFrame (top 44, bottom 34)");
        const ObjC::id sharedApp = u.app ? u.app : r.createInstance(r.getClass("UIApplication"));
        {
            // Subway Surfers: respondsTo registerForRemoteNotifications, then an NSInvocation for
            // isRegisteredForRemoteNotifications built from the class's method signature.
            const ObjC::id appCls = ObjC::id(r.getClass("UIApplication"));
            const ObjC::id sig = ObjC::id(r.send(cpu, appCls, "instanceMethodSignatureForSelector:", {r.sel("isRegisteredForRemoteNotifications")}));
            const uint32_t registered = r.send(cpu, sharedApp, "isRegisteredForRemoteNotifications") & 0xff;
            check(sig && registered == 0 && uint32_t(r.send(cpu, sharedApp, "currentUserNotificationSettings")) == 0,
                  "UIApplication has isRegisteredForRemoteNotifications (NO) with a method signature");
        }
        {
            // UIPickerView: a storyboard connects dataSource/delegate with setValue:forKey: (ROBLOX's login).
            const ObjC::id picker = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, ObjC::id(r.getClass("UIPickerView")), r.selAlloc)), "init"));
            const ObjC::id ds = r.createInstance(r.getClass("NSObject"));
            r.send(cpu, picker, "setValue:forKey:", {ds, Foundation::makeString(cx, "dataSource")});
            r.send(cpu, picker, "selectRow:inComponent:animated:", {3, 0, 1});
            check(picker && ObjC::id(r.send(cpu, picker, "dataSource")) == ds &&
                      int32_t(r.send(cpu, picker, "numberOfComponents")) == 0 &&
                      int32_t(r.send(cpu, picker, "selectedRowInComponent:", {0})) == -1,
                  "UIPickerView takes its data source by KVC and asks it for rows");
            r.send(cpu, picker, "release");
        }
        check(std::abs(UIKit::getRect(cx, sharedApp, "statusBarFrame").h - 44.0f) < 0.01f,
              "with safe-area insets the status bar is as tall as the top inset");
        ::setenv("HVM_SAFE_AREA", "0,200,0,200", 1);   // 400 points of side insets on a 320-point screen
        UIKit::applyGuestScreenEnv(cx);
        const Graphics::Rect erased = screenSize("applicationFrame");
        check(std::abs(erased.w - w0) < 0.01f && std::abs(erased.y - 20.0f) < 0.01f,
              "insets that would leave no usable width are rejected");
        ::setenv("HVM_SAFE_AREA", "bogus", 1);
        UIKit::applyGuestScreenEnv(cx);
        check(std::abs(UIKit::getRect(cx, sharedApp, "statusBarFrame").h - 20.0f) < 0.01f, "without valid insets the status bar is 20 points high");
        const Graphics::Rect plain = screenSize("applicationFrame");
        check(std::abs(plain.y - 20.0f) < 0.01f && std::abs(plain.h - (h0 - 20.0f)) < 0.01f,
              "invalid safe-area text means no insets");
        ::unsetenv("HVM_SAFE_AREA");
        UIKit::applyGuestScreenEnv(cx);
        // #306: interface orientations use UIKit's numbers. LandscapeRight (3) has the home
        // button on the right, so the UI's top and the status bar are on the device's right
        // edge, and a root view turns +90 degrees; LandscapeLeft (4) is the mirror image.
        {
            const int32_t savedInit = u.initialOrientation;
            auto rootFor = [&](int32_t o, Graphics::Rect& frame) {
                u.initialOrientation = o; u.statusBarOrientation = o;
                frame = screenSize("applicationFrame");
                const ObjC::id vcO = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, r.getClass("UIViewController"), r.selAlloc)), r.selInit));
                const ObjC::id winO = UIKit::sendRect(cx, ObjC::id(r.send(cpu, r.getClass("UIWindow"), r.selAlloc)), r.sel("initWithFrame:"), {0, 0, w0, h0});
                r.send(cpu, winO, "setRootViewController:", {vcO});   // a plain controller accepts both landscapes
                const UIKit::LayerData& l = UIKit::layerData(cx, UIKit::viewLayer(cx, ObjC::id(r.send(cpu, vcO, "view"))));
                const float m12 = l.transform.m12;
                r.send(cpu, winO, "setHidden:", {1});
                return m12;
            };
            Graphics::Rect right{}, left{};
            const float m12Right = rootFor(3, right), m12Left = rootFor(4, left);
            check(std::abs(right.x) < 0.01f && std::abs(right.w - (w0 - 20.0f)) < 0.01f && std::abs(left.x - 20.0f) < 0.01f &&
                      m12Right > 0.99f && m12Left < -0.99f,
                  "orientation numbers are UIKit's: LandscapeRight (3) has the status bar on the device's right edge and turns the root +90 degrees, LandscapeLeft (4) the mirror");
            u.initialOrientation = savedInit;
            u.statusBarOrientation = 1;
        }
        u.statusBarHidden = hidden;
        u.statusBarOrientation = orientation;
        u.screenW = w0; u.screenH = h0;
        proc.setScreenSize(processW, processH);
    }

    // 2a. UIScrollView
    {
        const ObjC::Class svCls = r.getClass("UIScrollView");
        const ObjC::id sv = UIKit::sendRect(cx, ObjC::id(r.send(cpu, svCls, r.selAlloc)), r.sel("initWithFrame:"), {0, 0, 100, 100});
        r.send(cpu, sv, "setContentSize:", {UIKit::fbits(300), UIKit::fbits(500)});
        r.send(cpu, sv, "setContentOffset:animated:", {UIKit::fbits(25), UIKit::fbits(60), 0});
        const Graphics::Rect b = UIKit::getRect(cx, sv, "bounds");
        check(std::abs(b.x - 25.0f) < 0.01f && std::abs(b.y - 60.0f) < 0.01f, "UIScrollView setContentOffset updates bounds.origin");
    }

    // 2b. UITableView & UITableViewCell
    {
        const ObjC::Class cellCls = r.getClass("UITableViewCell");
        const ObjC::id cell = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, cellCls, r.selAlloc)), "initWithStyle:reuseIdentifier:", {3, Foundation::makeString(cx, "MyCell")}));
        const ObjC::id lbl = ObjC::id(r.send(cpu, cell, "textLabel"));
        const ObjC::id dlbl = ObjC::id(r.send(cpu, cell, "detailTextLabel"));
        r.send(cpu, lbl, "setText:", {Foundation::makeString(cx, "Title")});
        r.send(cpu, dlbl, "setText:", {Foundation::makeString(cx, "Subtitle")});
        const ObjC::id rid = ObjC::id(r.send(cpu, cell, "reuseIdentifier"));
        check(Foundation::utf8(cx, rid) == "MyCell" && Foundation::utf8(cx, ObjC::id(r.send(cpu, lbl, "text"))) == "Title", "UITableViewCell textLabel, detailTextLabel, and reuseIdentifier");

        const ObjC::Class tvCls = r.getClass("UITableView");
        const ObjC::id tv = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, tvCls, r.selAlloc)), "initWithFrame:style:", {0, 0, UIKit::fbits(320), UIKit::fbits(480), 0}));
        r.send(cpu, tv, "registerClass:forCellReuseIdentifier:", {cellCls, Foundation::makeString(cx, "RegCell")});
        const ObjC::id deq = ObjC::id(r.send(cpu, tv, "dequeueReusableCellWithIdentifier:", {Foundation::makeString(cx, "RegCell")}));
        check(deq != 0 && Foundation::utf8(cx, ObjC::id(r.send(cpu, deq, "reuseIdentifier"))) == "RegCell", "UITableView registerClass & dequeueReusableCellWithIdentifier");
    }

    // 2c. UINavigationController, UINavigationBar, UINavigationItem, UIBarButtonItem
    {
        const ObjC::Class vcCls = r.getClass("UIViewController");
        const ObjC::id rootVC = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, vcCls, r.selAlloc)), r.selInit));
        r.send(cpu, rootVC, "setTitle:", {Foundation::makeString(cx, "Root")});
        const ObjC::Class ncCls = r.getClass("UINavigationController");
        const ObjC::id nav = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, ncCls, r.selAlloc)), "initWithRootViewController:", {rootVC}));
        const ObjC::id childVC = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, vcCls, r.selAlloc)), r.selInit));
        r.send(cpu, childVC, "setTitle:", {Foundation::makeString(cx, "Child")});
        r.send(cpu, nav, "pushViewController:animated:", {childVC, 0});
        const ObjC::id top = ObjC::id(r.send(cpu, nav, "topViewController"));
        const ObjC::id navOfChild = ObjC::id(r.send(cpu, childVC, "navigationController"));
        check(top == childVC && navOfChild == nav, "UINavigationController pushViewController & navigationController property");
        const ObjC::id popped = ObjC::id(r.send(cpu, nav, "popViewControllerAnimated:", {0}));
        check(popped == childVC && ObjC::id(r.send(cpu, nav, "topViewController")) == rootVC, "UINavigationController popViewControllerAnimated");

        // UIBarButtonItem addTarget:action:forControlEvents: and customView forwarding test
        const ObjC::Class bbiCls = r.getClass("UIBarButtonItem");
        const ObjC::id item = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, bbiCls, r.selAlloc)), "initWithTitle:style:target:action:",
                                               {Foundation::makeString(cx, "Action"), 0, 0, 0}));
        const ObjC::SEL act = r.sel("btnTapped:");
        r.send(cpu, item, "addTarget:action:forControlEvents:", {rootVC, act, 1u << 6 /* UIControlEventTouchUpInside */});
        const ObjC::id t1 = ObjC::id(r.send(cpu, item, "target"));
        const ObjC::SEL a1 = ObjC::SEL(r.send(cpu, item, "action"));
        check(t1 == rootVC && a1 == act, "UIBarButtonItem addTarget:action:forControlEvents: sets target and action");

        const ObjC::Class btnCls = r.getClass("UIButton");
        const ObjC::id customBtn = UIKit::sendRect(cx, ObjC::id(r.send(cpu, btnCls, r.selAlloc)), r.sel("initWithFrame:"), {0.0f, 0.0f, 40.0f, 40.0f});
        r.send(cpu, item, "setCustomView:", {customBtn});
        const ObjC::id btnAllTargets = ObjC::id(r.send(cpu, customBtn, "allTargets"));
        const auto targetsList = Foundation::setItems(cx, btnAllTargets);
        check(std::find(targetsList.begin(), targetsList.end(), rootVC) != targetsList.end(), "UIBarButtonItem setCustomView forwards existing target/action to customView");

        r.send(cpu, item, "removeTarget:action:forControlEvents:", {rootVC, act, 1u << 6});
        const ObjC::id t2 = ObjC::id(r.send(cpu, item, "target"));
        const ObjC::SEL a2 = ObjC::SEL(r.send(cpu, item, "action"));
        check(t2 == 0 && a2 == 0, "UIBarButtonItem removeTarget:action:forControlEvents: clears target and action");
    }

    // Replacing an installed root controller's view must replace the window's
    // subview too. Minion Rush installs its EAGL view through this exact path.
    {
        const ObjC::id rootWindow = UIKit::sendRect(cx, ObjC::id(r.send(cpu, r.getClass("UIWindow"), r.selAlloc)), r.sel("initWithFrame:"), {0, 0, 320, 480});
        const ObjC::id rootController = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, r.getClass("UIViewController"), r.selAlloc)), r.selInit));
        r.send(cpu, rootWindow, "setRootViewController:", {rootController});
        const ObjC::id oldRootView = ObjC::id(r.send(cpu, rootController, "view"));
        const ObjC::id newRootView = UIKit::sendRect(cx, ObjC::id(r.send(cpu, r.getClass("UIView"), r.selAlloc)), r.sel("initWithFrame:"), {0, 0, 320, 480});
        r.send(cpu, rootController, "setView:", {newRootView});
        check(ObjC::id(r.send(cpu, newRootView, "superview")) == rootWindow &&
                  ObjC::id(r.send(cpu, oldRootView, "superview")) == 0,
              "UIViewController setView replaces the installed root view in its window");
    }

    // CTLineGetOffsetForStringIndex writes through the guest's secondaryOffset pointer; a pointer into a
    // read-only page must be left alone (GuestArena::write does not check permissions itself).
    {
        auto& mem = proc.mem();
        const uint32_t page = mem.findFree(4096);
        mem.map(page, 4096, Memory::kProtRW);
        for (uint32_t i = 0; i < 4; ++i) mem.write<uint8_t>(page + i, 0xAA);
        mem.protect(page, 4096, Memory::kProtRead);
        bool called = false;
        if (const auto sym = proc.lookupSymbol("_CTLineGetOffsetForStringIndex")) {
            try { cx.proc.callGuest(cpu, *sym, {0, 0, page}); called = true; } catch (const std::exception&) {}
        }
        bool intact = true;
        for (uint32_t i = 0; i < 4; ++i) intact &= mem.read<uint8_t>(page + i) == 0xAA;
        mem.unmap(page, 4096);
        check(called && intact, "CTLineGetOffsetForStringIndex leaves a read-only secondaryOffset page untouched");
    }

    // The guest's working directory starts at "/" as on iOS: a path without its leading
    // slash still names a file in the app (Fruit Ninja's file manager joins paths that
    // way and opened nothing, then sized its file table from ftell's -1). chdir/getcwd
    // change the guest's directory, never the host process's.
    {
        const std::string hostCwdBefore = std::filesystem::current_path().string();
        struct ResetCwd { Process& p; ~ResetCwd() { p.setGuestCwd("/"); } } resetCwd{cx.proc};   // on every exit path
        const std::string guestBundle = cx.proc.guestPath(cx.proc.bundlePath());
        bool relativeBundle = cx.proc.hostPath(guestBundle.substr(1) + "/data.bin") == cx.proc.bundlePath() + "/data.bin" &&
                              cx.proc.guestCwd() == "/";
        // A bare name that exists only in the bundle opens there while the app has not set a
        // working directory (PvZ opens "resources.xml").
        {
            std::error_code ec;
            std::filesystem::create_directories(cx.proc.bundlePath(), ec);
            const std::string name = "hvm-relative-test.xml";
            { std::ofstream(cx.proc.bundlePath() + "/" + name) << "x"; }
            // The same name made absolute against "/" first (PvZ builds a CFURL from
            // "sounds/buttonclick.caf") resolves into the bundle too.
            relativeBundle = relativeBundle && cx.proc.hostPath(name) == cx.proc.bundlePath() + "/" + name &&
                             cx.proc.hostPath("/" + name) == cx.proc.bundlePath() + "/" + name &&
                             cx.proc.hostPath("/missing-everywhere.xml") == "/missing-everywhere.xml" &&
                             cx.proc.hostPath("missing-everywhere.xml") == "/missing-everywhere.xml";
            std::filesystem::remove(cx.proc.bundlePath() + "/" + name, ec);
        }
        bool changed = false, failedOnFile = false;
        if (const auto chdirSym = proc.lookupSymbol("_chdir"), getcwdSym = proc.lookupSymbol("_getcwd"); chdirSym && getcwdSym) {
            std::error_code ec;
            std::filesystem::create_directories(cx.proc.homePath() + "/Documents", ec);
            const uint32_t home = cx.proc.allocCString(cx.proc.guestHomePath());
            const int32_t rc = int32_t(cx.proc.callGuest(cpu, *chdirSym, {home}));
            const uint32_t buf = proc.heap().allocZeroed(1024);
            const uint32_t got = cx.proc.callGuest(cpu, *getcwdSym, {buf, 1024});
            changed = rc == 0 && got == buf && cx.proc.readCString(buf) == cx.proc.guestHomePath() &&
                      cx.proc.hostPath("Documents") == cx.proc.homePath() + "/Documents" &&
                      std::filesystem::current_path().string() == hostCwdBefore;
            // A file is not a directory: chdir fails and the directory stays put.
            const std::string file = cx.proc.homePath() + "/Documents/cwd-file";
            { std::ofstream(file) << "x"; }
            const uint32_t filePath = cx.proc.allocCString(cx.proc.guestHomePath() + "/Documents/cwd-file");
            failedOnFile = int32_t(cx.proc.callGuest(cpu, *chdirSym, {filePath})) == -1 && cx.proc.guestCwd() == cx.proc.guestHomePath();
            std::filesystem::remove(file, ec);
        }
        if (!(relativeBundle && changed && failedOnFile))
            std::printf("    cwd: relative bundle %d chdir/getcwd %d file refused %d\n", relativeBundle, changed, failedOnFile);
        check(relativeBundle && changed && failedOnFile, "guest working directory starts at /; chdir/getcwd are the guest's own");
    }

    // libc++'s __next_prime sizes unordered_map's buckets (Minecraft PE crashed on a stub's 0).
    {
        bool primes = false;
        if (const auto np = proc.lookupSymbol("__ZNSt3__112__next_primeEm")) {
            const std::pair<uint32_t, uint32_t> cases[] = {{0, 0}, {1, 2}, {2, 2}, {3, 3}, {4, 5}, {24, 29}, {97, 97},
                                                           {98, 101}, {65536, 65537}, {4294967291u, 4294967291u}};
            primes = true;
            for (auto [in, want] : cases) {
                const uint32_t got = cx.proc.callGuest(cpu, *np, {in});
                if (got != want) { std::printf("    __next_prime(%u) = %u, want %u\n", in, got, want); primes = false; }
            }
        }
        check(primes, "std::__1::__next_prime returns the smallest prime >= n");
    }

    // libc++ shared_ptr control blocks, timed condition waits, recursive_mutex and
    // hardware_concurrency (Minecraft PE: stubs left shared_ptr counts frozen and turned
    // Semaphore::wait into a busy loop).
    {
        static int zeroShared = 0, zeroWeak = 0;
        zeroShared = zeroWeak = 0;
        const uint32_t onShared = proc.defineFunction("__test_on_zero_shared", [](Ctx&) { ++zeroShared; });
        const uint32_t onWeak = proc.defineFunction("__test_on_zero_shared_weak", [](Ctx&) { ++zeroWeak; });
        auto sym = [&](const char* name) { return proc.lookupSymbol(name); };
        auto callSym = [&](const char* name, std::initializer_list<uint32_t> args) { return cx.proc.callGuest(cpu, *sym(name), args); };
        bool counts = false, timed = false, recursive = false;
        if (sym("__ZNSt3__119__shared_weak_count12__add_sharedEv") && sym("__ZNSt3__119__shared_weak_count4lockEv")) {
            const uint32_t vtable = proc.heap().allocZeroed(20), block = proc.heap().allocZeroed(12);
            proc.mem().write<uint32_t>(vtable + 8, onShared);
            proc.mem().write<uint32_t>(vtable + 16, onWeak);
            proc.mem().write<uint32_t>(block, vtable);   // one shared owner, no extra weak ones
            callSym("__ZNSt3__119__shared_weak_count12__add_sharedEv", {block});
            callSym("__ZNSt3__119__shared_weak_count10__add_weakEv", {block});
            const bool twoOwners = proc.mem().read<int32_t>(block + 4) == 1 && proc.mem().read<int32_t>(block + 8) == 1;
            const bool locked = callSym("__ZNSt3__119__shared_weak_count4lockEv", {block}) == block;   // a third owner
            for (int i = 0; i < 2; ++i) callSym("__ZNSt3__119__shared_weak_count16__release_sharedEv", {block});
            const bool stillAlive = zeroShared == 0;
            callSym("__ZNSt3__119__shared_weak_count16__release_sharedEv", {block});
            const bool expired = zeroShared == 1 && zeroWeak == 0 && callSym("__ZNSt3__119__shared_weak_count4lockEv", {block}) == 0;
            callSym("__ZNSt3__119__shared_weak_count14__release_weakEv", {block});
            counts = twoOwners && locked && stillAlive && expired && zeroWeak == 1;
            if (!counts) std::printf("    shared_weak_count: owners %d lock %d alive %d expired %d weak %d\n", twoOwners, locked, stillAlive, expired, zeroWeak);
        }
        if (const auto tw = sym("__ZNSt3__118condition_variable15__do_timed_waitERNS_11unique_lockINS_5mutexEEENS_6chrono10time_pointINS5_12system_clockENS5_8durationIxNS_5ratioILx1ELx1000000000EEEEEEE")) {
            const uint32_t mtx = proc.heap().allocZeroed(64), cv = proc.heap().allocZeroed(64), lock = proc.heap().allocZeroed(8);
            callSym("_pthread_mutex_init", {mtx, 0});
            callSym("_pthread_cond_init", {cv, 0});
            proc.mem().write<uint32_t>(lock, mtx);
            proc.mem().write<uint32_t>(lock + 4, 1);
            callSym("_pthread_mutex_lock", {mtx});
            const auto start = std::chrono::system_clock::now();
            const int64_t deadline = std::chrono::duration_cast<std::chrono::nanoseconds>((start + std::chrono::milliseconds(40)).time_since_epoch()).count();
            cx.proc.callGuest(cpu, *tw, {cv, lock, uint32_t(deadline), uint32_t(uint64_t(deadline) >> 32)});
            const auto waited = std::chrono::system_clock::now() - start;
            callSym("_pthread_mutex_unlock", {mtx});
            timed = waited >= std::chrono::milliseconds(30) && waited < std::chrono::seconds(5);
            if (!timed) std::printf("    __do_timed_wait waited %lld ms\n", (long long)std::chrono::duration_cast<std::chrono::milliseconds>(waited).count());
        }
        if (sym("__ZNSt3__115recursive_mutexC1Ev")) {
            const uint32_t rm = proc.heap().allocZeroed(64);
            try {
                callSym("__ZNSt3__115recursive_mutexC1Ev", {rm});
                callSym("__ZNSt3__115recursive_mutex4lockEv", {rm});
                callSym("__ZNSt3__115recursive_mutex4lockEv", {rm});   // a plain mutex fails (EDEADLK) here
                callSym("__ZNSt3__115recursive_mutex6unlockEv", {rm});
                callSym("__ZNSt3__115recursive_mutex6unlockEv", {rm});
                recursive = callSym("_pthread_mutex_trylock", {rm}) == 0;
                callSym("_pthread_mutex_unlock", {rm});
                callSym("__ZNSt3__115recursive_mutexD1Ev", {rm});
            } catch (const std::exception& e) {
                std::printf("    recursive_mutex: %s\n", e.what());
            }
        }
        const bool cores = sym("__ZNSt3__16thread20hardware_concurrencyEv") && callSym("__ZNSt3__16thread20hardware_concurrencyEv", {}) >= 1;
        check(counts && timed && recursive && cores, "libc++ shared_ptr counts, timed condition wait, recursive_mutex, hardware_concurrency");
    }

    // UILocalNotification keeps what games set on it (three games logged its setters as missing).
    {
        const ObjC::id note = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, r.getClass("UILocalNotification"), r.selAlloc)), r.selInit));
        const ObjC::id body = Foundation::makeString(cx, "Come back!");
        r.send(cpu, note, "setAlertBody:", {body});
        r.send(cpu, note, "setRepeatInterval:", {16});
        r.send(cpu, note, "setApplicationIconBadgeNumber:", {3});
        r.send(cpu, note, "setHasAction:", {0});
        const ObjC::id copy = ObjC::id(r.send(cpu, note, "copy"));
        bool kept = Foundation::utf8(cx, ObjC::id(r.send(cpu, note, "alertBody"))) == "Come back!" &&
                          uint32_t(r.send(cpu, note, "repeatInterval")) == 16 && uint32_t(r.send(cpu, note, "applicationIconBadgeNumber")) == 3 &&
                          (r.send(cpu, note, "hasAction") & 0xff) == 0 && (r.send(cpu, copy, "hasAction") & 0xff) == 0 &&
                          Foundation::utf8(cx, ObjC::id(r.send(cpu, copy, "alertBody"))) == "Come back!" && uint32_t(r.send(cpu, copy, "repeatInterval")) == 16;
        if (!kept) {
            const ObjC::id b = ObjC::id(r.send(cpu, note, "alertBody")), cb = ObjC::id(r.send(cpu, copy, "alertBody"));
            std::printf("    UILocalNotification: body '%s' repeat %u badge %u hasAction %u copy body '%s' repeat %u\n",
                        b ? Foundation::utf8(cx, b).c_str() : "(nil)", uint32_t(r.send(cpu, note, "repeatInterval")),
                        uint32_t(r.send(cpu, note, "applicationIconBadgeNumber")), uint32_t(r.send(cpu, note, "hasAction")),
                        cb ? Foundation::utf8(cx, cb).c_str() : "(nil)", uint32_t(r.send(cpu, copy, "repeatInterval")));
        }
        // The copy owns its values: it still reads them after the original is gone.
        r.release(cpu, note);
        const ObjC::id copiedBody = ObjC::id(r.send(cpu, copy, "alertBody"));
        kept = kept && copiedBody && Foundation::utf8(cx, copiedBody) == "Come back!";
        r.release(cpu, copy);
        check(kept, "UILocalNotification keeps its properties and copies them");
    }

    // Storyboard segue templates: the nib sets the source controller through the
    // "viewController" outlet (ROBLOX stopped there), and performSegueWithIdentifier:
    // instantiates the destination from the storyboard and presents it.
    {
        bool segues = false;
        if (const ObjC::Class modalCls = r.getClass("UIStoryboardModalSegueTemplate")) {
            const ObjC::id vc = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, r.getClass("UIViewController"), r.selAlloc)), r.selInit));
            const ObjC::id sb = r.createInstance(r.getClass("UIStoryboard"));
            r.send(cpu, vc, "_setStoryboard:", {sb});
            const ObjC::id t = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, modalCls, r.selAlloc)), r.selInit));
            r.send(cpu, t, "setValue:forKey:", {vc, Foundation::makeString(cx, "viewController")});
            r.send(cpu, t, "setIdentifier:", {Foundation::makeString(cx, "showGame")});
            r.send(cpu, t, "setDestinationViewControllerIdentifier:", {Foundation::makeString(cx, "UIViewController")});
            r.send(cpu, vc, "_setStoryboardSegueTemplates:", {Foundation::makeArray(cx, {t})});
            r.send(cpu, vc, "performSegueWithIdentifier:sender:", {Foundation::makeString(cx, "showGame"), 0});
            const ObjC::id presented = ObjC::id(r.send(cpu, vc, "presentedViewController"));
            bool refused = false;
            try { r.send(cpu, vc, "performSegueWithIdentifier:sender:", {Foundation::makeString(cx, "nope"), 0}); }
            catch (const std::exception& e) { refused = std::string(e.what()).find("has no segue with identifier 'nope'") != std::string::npos; }
            segues = ObjC::id(r.send(cpu, t, "viewController")) == vc && presented && presented != vc &&
                     r.isKindOf(presented, r.getClass("UIViewController")) && refused;
            if (!segues) std::printf("    segue: outlet %d presented %08x refused %d\n", ObjC::id(r.send(cpu, t, "viewController")) == vc, presented, refused);
        }
        check(segues, "storyboard segue templates take their source outlet and present the destination");
    }

    // iOS 3-5 orientation for a window without a rootViewController (Bad Piggies / Unity 3.x):
    // the controller whose view is the window's first subview gets the rotation callbacks and
    // a landscape view; the accelerometer rests in that orientation until the host reports;
    // beginGeneratingDeviceOrientationNotifications queues the first change notification.
    {
        static int willTo = 0, didFrom = -1, asked = 0;
        willTo = 0; didFrom = -1; asked = 0;
        auto& u = UIKit::ui(cx);
        ObjC::ClassBuilder legacy = r.defineClass("HvmLegacyRotationVC", "UIViewController");
        legacy.method("shouldAutorotateToInterfaceOrientation:", [](Ctx& x) { ++asked; x.cpu.r[0] = x.cpu.r[2] == 3 || x.cpu.r[2] == 4; }, "c@:i")
              .method("willRotateToInterfaceOrientation:duration:", [](Ctx& x) { willTo = int(x.cpu.r[2]); }, "v@:id")
              .method("didRotateFromInterfaceOrientation:", [](Ctx& x) { didFrom = int(x.cpu.r[2]); }, "v@:i");
        const ObjC::id vc = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, legacy.cls(), r.selAlloc)), r.selInit));
        const ObjC::id win = ObjC::id(UIKit::sendRect(cx, ObjC::id(r.send(cpu, r.getClass("UIWindow"), r.selAlloc)), r.sel("initWithFrame:"), {0, 0, 320, 480}));
        const ObjC::id view = ObjC::id(r.send(cpu, vc, "view"));
        r.send(cpu, win, "addSubview:", {view});
        const int savedOrientation = u.statusBarOrientation;
        const bool savedHost = u.accelFromHost;
        u.statusBarOrientation = 3;
        u.accelFromHost = false;
        UIKit::rotateLegacyControllers(cx, "test");
        const auto b = UIKit::getRect(cx, view, "bounds");
        const auto g = UIKit::restingGravity(u);
        const int callsAfterFirst = asked;
        UIKit::rotateLegacyControllers(cx, "test again");   // already laid out: nothing repeats
        const bool rotated = willTo == 3 && didFrom == 1 && b.w > b.h && asked == callsAfterFirst;
        const bool gravity = g[0] == -1 && g[1] == 0;
        u.deviceOrientationNotePending = false;
        r.send(cpu, ObjC::id(r.send(cpu, r.getClass("UIDevice"), "currentDevice")), "beginGeneratingDeviceOrientationNotifications");
        const bool queued = u.deviceOrientationNotePending;
        r.send(cpu, ObjC::id(r.send(cpu, r.getClass("UIDevice"), "currentDevice")), "endGeneratingDeviceOrientationNotifications");
        u.deviceOrientationNotePending = false;
        r.send(cpu, view, "removeFromSuperview");
        u.statusBarOrientation = savedOrientation;
        u.accelFromHost = savedHost;
        if (!(rotated && gravity && queued))
            std::printf("    legacy rotation: will %d did %d bounds %gx%g asked %d/%d gravity %g,%g queued %d\n",
                        willTo, didFrom, b.w, b.h, callsAfterFirst, asked, g[0], g[1], queued);
        check(rotated && gravity && queued, "window-subview controller rotates with callbacks; resting gravity and orientation notification follow");

        // The same controller in three cases that must not be turned:
        //  - an app built for iPhone OS 2 turns its own picture (PvZ drew its title upside down);
        //  - a view the app already made landscape itself (turned twice, the whole game went
        //    sideways and stayed: Minecraft PE with its keyboard up);
        //  - and one with the iOS 2 two-step callbacks gets those, not the one-step call.
        static int firstHalf = 0, secondHalf = 0;
        firstHalf = secondHalf = 0;
        ObjC::ClassBuilder twoStep = r.defineClass("HvmTwoStepRotationVC", "HvmLegacyRotationVC");
        twoStep.method("willAnimateFirstHalfOfRotationToInterfaceOrientation:duration:", [](Ctx& x) { firstHalf = int(x.cpu.r[2]); }, "v@:id")
               .method("willAnimateSecondHalfOfRotationFromInterfaceOrientation:duration:", [](Ctx& x) { secondHalf = int(x.cpu.r[2]); }, "v@:id");
        auto fresh = [&](ObjC::Class cls) {
            const ObjC::id c = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, cls, r.selAlloc)), r.selInit));
            const ObjC::id w = ObjC::id(UIKit::sendRect(cx, ObjC::id(r.send(cpu, r.getClass("UIWindow"), r.selAlloc)), r.sel("initWithFrame:"), {0, 0, 320, 480}));
            r.send(cpu, w, "addSubview:", {ObjC::id(r.send(cpu, c, "view"))});
            return c;
        };
        u.statusBarOrientation = 3;
        u.accelFromHost = false;
        const ObjC::id oldApp = fresh(legacy.cls());
        u.builtForIOS2 = true;
        willTo = 0;
        UIKit::rotateLegacyControllers(cx, "test");
        u.builtForIOS2 = false;
        const auto oldBounds = UIKit::getRect(cx, ObjC::id(r.send(cpu, oldApp, "view")), "bounds");
        const bool ios2Left = willTo == 0 && oldBounds.h > oldBounds.w;
        r.send(cpu, ObjC::id(r.send(cpu, oldApp, "view")), "removeFromSuperview");
        const ObjC::id own = fresh(legacy.cls());
        UIKit::sendRect(cx, ObjC::id(r.send(cpu, own, "view")), r.sel("setBounds:"), {0, 0, 480, 320});
        willTo = 0;
        UIKit::rotateLegacyControllers(cx, "test");
        const auto ownBounds = UIKit::getRect(cx, ObjC::id(r.send(cpu, own, "view")), "bounds");
        const bool ownLeft = willTo == 0 && ownBounds.w == 480 && ownBounds.w > ownBounds.h;
        r.send(cpu, ObjC::id(r.send(cpu, own, "view")), "removeFromSuperview");
        const ObjC::id stepped = fresh(twoStep.cls());
        willTo = 0;
        UIKit::rotateLegacyControllers(cx, "test");
        const bool halves = willTo == 3 && firstHalf == 3 && secondHalf == 1;
        r.send(cpu, ObjC::id(r.send(cpu, stepped, "view")), "removeFromSuperview");
        u.statusBarOrientation = savedOrientation;
        u.accelFromHost = savedHost;
        if (!(ios2Left && ownLeft && halves))
            std::printf("    legacy rotation 2: ios2 %d own %d (%gx%g, will %d) halves %d/%d\n", ios2Left, ownLeft, ownBounds.w, ownBounds.h, willTo, firstHalf, secondHalf);
        check(ios2Left, "an app built for iPhone OS 2 keeps its portrait view in landscape");
        check(ownLeft, "a view the app already made landscape is not turned again");
        check(halves, "a controller with the two-step rotation callbacks gets both halves");
    }

    // Accelerate's vector routines on recorded sound (Talking John), and long long rounding.
    {
        const uint32_t mem = proc.heap().allocZeroed(128);
        const float in[4] = {0.25f, -0.75f, 0.5f, 40000.0f};
        proc.mem().copyIn(mem, in, 16);
        auto fn = [&](const char* sym, std::initializer_list<uint32_t> args) { return proc.callGuest(cpu, *proc.lookupSymbol(sym), args); };
        fn("_vDSP_maxmgv", {mem, 1, mem + 32, 3});
        const float peak = proc.mem().read<float>(mem + 32);
        const float gain = 1000.0f;
        proc.mem().write<float>(mem + 36, gain);
        fn("_vDSP_vsmul", {mem, 1, mem + 36, mem + 48, 1, 4});
        fn("_vDSP_vfix16", {mem + 48, 1, mem + 80, 1, 4});
        const int16_t s0 = proc.mem().read<int16_t>(mem + 80), s1 = proc.mem().read<int16_t>(mem + 82), s3 = proc.mem().read<int16_t>(mem + 86);
        double half = 2.5;
        uint32_t bits[2];
        std::memcpy(bits, &half, 8);
        const int64_t rounded = int64_t(fn("_llrint", {bits[0], bits[1]}));
        proc.heap().free(mem);
        check(peak == 0.75f && s0 == 250 && s1 == -750 && s3 == 32767, "vDSP_maxmgv, vDSP_vsmul and vDSP_vfix16 work on guest arrays (and saturate)");
        check(rounded == 2, "llrint rounds to even and returns a long long");
    }

    // NSUserDefaults hands out one long-lived object per stored value (Tiny Wings keeps the
    // player name it reads without retaining it); a replaced value's object outlives the turn.
    {
        const ObjC::id ud = ObjC::id(r.send(cpu, r.getClass("NSUserDefaults"), "standardUserDefaults"));
        const ObjC::id key = Foundation::makeString(cx, "hvmTestPlayerName");
        r.send(cpu, ud, "setObject:forKey:", {Foundation::makeString(cx, "Lawson"), key});
        const uint32_t pool = r.poolPush();
        const ObjC::id first = ObjC::id(r.send(cpu, ud, "objectForKey:", {key}));
        r.poolPop(cpu, pool);   // a per-call autoreleased copy would be freed here
        const ObjC::id again = ObjC::id(r.send(cpu, ud, "objectForKey:", {key}));
        const bool same = first && first == again && Foundation::utf8(cx, again) == "Lawson" &&
                          ObjC::id(r.send(cpu, ud, "stringForKey:", {key})) == first;
        const uint32_t pool2 = r.poolPush();
        r.send(cpu, ud, "setObject:forKey:", {Foundation::makeString(cx, "Larry"), key});
        const ObjC::id replaced = ObjC::id(r.send(cpu, ud, "objectForKey:", {key}));
        const bool oldStillValid = Foundation::utf8(cx, first) == "Lawson";   // let go only at the pool drain
        r.poolPop(cpu, pool2);
        const bool updated = replaced != first && Foundation::utf8(cx, replaced) == "Larry";
        r.send(cpu, ud, "removeObjectForKey:", {key});
        check(same && oldStillValid && updated, "NSUserDefaults returns one long-lived object per stored value");
    }

    // locale(const locale&, Facet*) as libstdc++ 4.2 inlines it (ROBLOX's boost path):
    // new _Impl(*other._M_impl, 1), then _M_install_facet; copies share the _Impl.
    {
        auto callSym = [&](const char* name, std::initializer_list<uint32_t> args) { return uint32_t(cx.proc.callGuest(cpu, *proc.lookupSymbol(name), args)); };
        auto rd = [&](uint32_t a) { return proc.mem().read<uint32_t>(a); };
        const uint32_t base = proc.heap().allocZeroed(4), custom = proc.heap().allocZeroed(4), copy = proc.heap().allocZeroed(4);
        callSym("__ZNSt6localeC1Ev", {base});
        const uint32_t facet = proc.heap().allocZeroed(16);
        callSym("__ZNSt7codecvtIwc11__mbstate_tEC2Em", {facet, 0});
        const bool facetOk = rd(facet) != 0 && rd(facet + 4) == 0;
        const uint32_t id = proc.heap().allocZeroed(4);   // the derived facet's static id
        const uint32_t impl = proc.heap().allocZeroed(20);
        callSym("__ZNSt6locale5_ImplC1ERKS0_m", {impl, rd(base), 1});
        callSym("__ZNSt6locale5_Impl16_M_install_facetEPKNS_2idEPKNS_5facetE", {impl, id, facet});
        proc.mem().write<uint32_t>(custom, impl);
        const uint32_t index = callSym("__ZNKSt6locale2id5_M_idEv", {id});
        const bool installed = rd(impl) == 1 && rd(id) == index + 1 && index < rd(impl + 8) &&
                               rd(rd(impl + 4) + index * 4) == facet && rd(facet + 4) == 1;
        callSym("__ZNSt6localeC1ERKS_", {copy, custom});
        const bool shared = rd(copy) == impl && rd(impl) == 2 && rd(rd(base) + 4) != 0;
        const uint32_t ti = proc.lookupSymbol("__ZTISt7codecvtIwc11__mbstate_tE").value_or(0);
        // use_facet<codecvt<wchar_t> >: the facet installed under codecvt<wchar_t>::id, else
        // the "C" default, whose do_out narrows and stops at a character above 0xFF.
        const uint32_t cvId = proc.lookupSymbol("__ZNSt7codecvtIwc11__mbstate_tE2idE").value_or(0);
        const uint32_t impl2 = proc.heap().allocZeroed(20), withCv = proc.heap().allocZeroed(4);
        callSym("__ZNSt6locale5_ImplC1ERKS0_m", {impl2, rd(base), 1});
        callSym("__ZNSt6locale5_Impl16_M_install_facetEPKNS_2idEPKNS_5facetE", {impl2, cvId, facet});
        proc.mem().write<uint32_t>(withCv, impl2);
        const char* useFacet = "__ZSt9use_facetISt7codecvtIwc11__mbstate_tEERKT_RKSt6locale";
        const uint32_t def = callSym(useFacet, {base});
        const bool lookup = cvId && callSym(useFacet, {withCv}) == facet && def && def != facet;
        const uint32_t wide = proc.heap().allocZeroed(16), narrow = proc.heap().allocZeroed(8), nexts = proc.heap().allocZeroed(8);
        for (uint32_t i = 0; i < 3; ++i) proc.mem().write<uint32_t>(wide + i * 4, i == 2 ? 0x263A : uint32_t('a' + i));
        const uint32_t doOut = rd(rd(def) + 8);   // vptr slots: D1 D0 do_out
        const uint32_t res = uint32_t(cx.proc.callGuest(cpu, doOut, {def, 0, wide, wide + 12, nexts, narrow, narrow + 8, nexts + 4}));
        const bool converted = res == 2 && rd(nexts) == wide + 8 && rd(nexts + 4) == narrow + 2 &&
                               proc.readCString(narrow) == "ab";
        check(facetOk && installed && shared && ti && rd(ti + 8) && lookup && converted,
              "locale(const locale&, Facet*): _Impl copy, _M_install_facet, locale::id and shared copies");
    }

    // std::set_terminate / get_terminate keep the handler (Terraria's HockeyApp installs one) and
    // <fenv.h> saves and restores the FPSCR (Clumsy Ninja's FMOD).
    {
        auto callSym = [&](const char* name, std::initializer_list<uint32_t> args) { return cx.proc.callGuest(cpu, *proc.lookupSymbol(name), args); };
        const uint32_t before = callSym("__ZSt13set_terminatePFvvE", {0x1235});
        const bool terminate = callSym("__ZSt13get_terminatev", {}) == 0x1235 && callSym("__ZSt13set_terminatePFvvE", {before}) == 0x1235;
        const uint32_t env = proc.heap().allocZeroed(16);
        const uint32_t saved = cpu.fpscr;
        cpu.fpscr = (3u << 22) | (1u << 24) | 0x10;   // round toward zero, flush-to-zero, IXC raised
        callSym("_fegetenv", {env});
        const bool got = proc.mem().read<uint32_t>(env) == cpu.fpscr;
        callSym("_fesetround", {0});
        const bool rounded = callSym("_fegetround", {}) == 0 && callSym("_fetestexcept", {0x10}) == 0x10;
        callSym("_feclearexcept", {0x10});
        const bool cleared = callSym("_fetestexcept", {0x9F}) == 0;
        callSym("_fesetenv", {env});
        const bool restored = cpu.fpscr == ((3u << 22) | (1u << 24) | 0x10);
        cpu.fpscr = saved;
        if (!(terminate && got && rounded && cleared && restored))
            std::printf("    terminate %d fenv get %d round %d clear %d restore %d\n", terminate, got, rounded, cleared, restored);
        check(terminate && got && rounded && cleared && restored, "std::set_terminate/get_terminate and <fenv.h> environment, rounding and flags");
    }

    // UIViewController has the rotation and appearance hooks engines copy encodings from
    // (Unity's ObjCSetKnownInstanceMethod asserts on a NULL Method).
    {
        bool all = false;
        const auto getMethod = proc.lookupSymbol("_class_getInstanceMethod"), encoding = proc.lookupSymbol("_method_getTypeEncoding");
        if (getMethod && encoding) {
            all = true;
            const std::pair<const char*, const char*> expected[] = {
                {"willRotateToInterfaceOrientation:duration:", "v@:id"}, {"didRotateFromInterfaceOrientation:", "v@:i"},
                {"willAnimateRotationToInterfaceOrientation:duration:", "v@:id"},
                {"viewWillTransitionToSize:withTransitionCoordinator:", "v@:{CGSize=ff}@"},
                {"preferredStatusBarUpdateAnimation", "i@:"}, {"prefersHomeIndicatorAutoHidden", "c@:"},
                {"preferredScreenEdgesDeferringSystemGestures", "I@:"},
                {"childViewControllerForStatusBarHidden", "@@:"}, {"childViewControllerForStatusBarStyle", "@@:"},
                {"childViewControllerForHomeIndicatorAutoHidden", "@@:"}, {"childViewControllerForScreenEdgesDeferringSystemGestures", "@@:"},
                {"setNeedsUpdateOfHomeIndicatorAutoHidden", "v@:"}, {"setNeedsUpdateOfScreenEdgesDeferringSystemGestures", "v@:"}};
            for (const auto& [name, types] : expected) {
                const uint32_t m = cx.proc.callGuest(cpu, *getMethod, {uint32_t(r.getClass("UIViewController")), uint32_t(r.sel(name))});
                const std::string got = m ? cx.proc.readCString(cx.proc.callGuest(cpu, *encoding, {m})) : "(no method)";
                if (got != types) { std::printf("    UIViewController %s: %s\n", name, got.c_str()); all = false; }
            }
        }
        check(all, "UIViewController has rotation and status-bar hooks with their type encodings");
    }

    // NIB loading: ~iphone before the base name, and a real exception when the
    // file is missing or cannot be decoded. Landscape/Portrait are not modifiers.
    {
        // The standalone fulltest runner has no on-disk app bundle by default.
        // NSBundle needs the directory to exist before mainBundle is queried.
        std::error_code bundleEc;
        const bool madeTestBundle = std::filesystem::create_directories(cx.proc.bundlePath(), bundleEc);
        const ObjC::Class bundleCls = r.getClass("NSBundle");
        const ObjC::id bundle = ObjC::id(r.send(cpu, bundleCls, "mainBundle"));

        // No guest @catch is installed here, so the raised NSException terminates
        // the host→guest call. The text is the uncaught-exception message.
        const auto nibFailure = [&](const char* name) -> std::string {
            try {
                r.send(cpu, bundle, "loadNibNamed:owner:options:", {Foundation::makeString(cx, name), 0, 0});
                return {};
            } catch (const CPU::GuestFault& e) {
                return e.what();
            } catch (const ObjC::ObjCError& e) {
                return e.what();
            } catch (const std::exception& e) {
                return e.what();
            }
        };
        const auto isNibException = [](const std::string& msg, const char* name) {
            return msg.find("NSInternalInconsistencyException") != std::string::npos &&
                   msg.find("Could not load NIB in bundle") != std::string::npos &&
                   msg.find(name) != std::string::npos;
        };
        const std::string missing = nibFailure("DefinitelyMissingNibThatDoesntExist");
        if (!isNibException(missing, "DefinitelyMissingNibThatDoesntExist"))
            std::printf("missing NIB raised: %s\n", missing.empty() ? "(returned nil)" : missing.c_str());
        check(isNibException(missing, "DefinitelyMissingNibThatDoesntExist"),
              "loadNibNamed:owner:options: missing NIB raises NSInternalInconsistencyException");

        // An app built with an iPhone OS 2/3 SDK gets nil instead (OpenFeint falls back on
        // nil from "<name>LandscapeOf"; Fruit Ninja stopped at launch on the exception).
        {
            const Plist::Value* was = cx.proc.infoPlist();
            Plist::ValuePtr saved = was ? std::make_shared<Plist::Value>(*was) : nullptr;
            Plist::ValuePtr old = Plist::Value::makeDict();
            old->set("DTSDKName", Plist::Value::makeString("iphoneos2.2.1"));
            cx.proc.setInfoPlist(old);
            const int sdk = cx.proc.linkedSdkMajor();
            const std::string oldMissing = nibFailure("RootControllerLandscapeOf");
            old->set("DTSDKName", Plist::Value::makeString("iphoneos4.0"));
            const std::string newMissing = nibFailure("RootControllerLandscapeOf");
            cx.proc.setInfoPlist(saved);
            check(sdk == 2 && oldMissing.empty() && isNibException(newMissing, "RootControllerLandscapeOf"),
                  "loadNibNamed: missing NIB returns nil for an app built with SDK 2/3, raises for SDK 4");
        }

        // A real, minimal NIBArchive: root -> NSArray -> NSString. This must
        // decode a known object. A missing or malformed name raises instead.
        auto nibBytes = [](const std::string& marker) {
            std::vector<uint8_t> b(50, 0);
            const char magic[] = "NIBArchive";
            std::copy(magic, magic + 10, b.begin());
            auto u32 = [&](uint32_t n) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(n >> (i * 8))); };
            auto var = [&](uint32_t n) { b.push_back(uint8_t(n | 0x80)); };
            auto set32 = [&](size_t off, uint32_t n) {
                for (int i = 0; i < 4; ++i) b[off + i] = uint8_t(n >> (i * 8));
            };
            // The varint writer is enough for this three-object fixture; all
            // indices and string lengths fit in seven bits.
            const uint32_t objOff = uint32_t(b.size());
            for (const auto& obj : {std::array<uint32_t, 3>{0, 0, 1}, {1, 1, 1}, {2, 2, 1}})
                for (uint32_t n : obj) var(n);
            const uint32_t keyOff = uint32_t(b.size());
            for (const std::string key : {"UINibTopLevelObjectsKey", "NS.objects", "NS.string"}) {
                var(uint32_t(key.size())); b.insert(b.end(), key.begin(), key.end());
            }
            const uint32_t valOff = uint32_t(b.size());
            var(0); b.push_back(10); u32(1);  // root -> NSArray
            var(1); b.push_back(10); u32(2);  // NSArray -> NSString
            var(2); b.push_back(8); var(uint32_t(marker.size()));
            b.insert(b.end(), marker.begin(), marker.end());
            const uint32_t clsOff = uint32_t(b.size());
            for (const std::string cls : {"NSObject", "NSArray", "NSString"}) {
                var(uint32_t(cls.size())); var(0);
                b.insert(b.end(), cls.begin(), cls.end());
            }
            set32(18, 3); set32(22, objOff); set32(26, 3); set32(30, keyOff);
            set32(34, 3); set32(38, valOff); set32(42, 3); set32(46, clsOff);
            return b;
        };
        auto writeNib = [](const std::string& path, const std::vector<uint8_t>& bytes) {
            FILE* f = std::fopen(path.c_str(), "wb");
            if (!f) return false;
            const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
            return std::fclose(f) == 0 && ok;
        };
        const std::string base = cx.proc.bundlePath() + "/HVMNibFixture189";
        const std::string exact = base + ".nib", idiom = base + "~iphone.nib";
        const std::string malformed = cx.proc.bundlePath() + "/HVMMalformedNib189.nib";
        // Previous interrupted test runs may have left these exact fixtures.
        std::remove(exact.c_str()); std::remove(idiom.c_str()); std::remove(malformed.c_str());
        const auto loadMarker = [&](const char* name) -> std::string {
            const ObjC::id loaded = ObjC::id(r.send(cpu, bundle, "loadNibNamed:owner:options:", {Foundation::makeString(cx, name), 0, 0}));
            if (!loaded || uint32_t(r.send(cpu, loaded, "count")) != 1) return {};
            const ObjC::id value = ObjC::id(r.send(cpu, loaded, "objectAtIndex:", {0}));
            return Foundation::utf8(cx, value);
        };
        const bool wroteExact = writeNib(exact, nibBytes("exact-object"));
        bool decodedExact = false, decodedIdiom = false, preferredIdiom = false;
        bool orientationRejected = false, malformedRejected = false;
        if (wroteExact) {
            decodedExact = loadMarker("HVMNibFixture189") == "exact-object";
            const bool wroteIdiom = writeNib(idiom, nibBytes("idiom-object"));
            if (wroteIdiom) {
                preferredIdiom = loadMarker("HVMNibFixture189") == "idiom-object";
                std::remove(exact.c_str());
                decodedIdiom = loadMarker("HVMNibFixture189") == "idiom-object";
                // "Landscape" and "Portrait" are not resource modifiers. With the
                // idiom file present, those names must still fail to load.
                const std::string landscape = nibFailure("HVMNibFixture189Landscape");
                const std::string portrait = nibFailure("HVMNibFixture189Portrait");
                orientationRejected = isNibException(landscape, "HVMNibFixture189Landscape") &&
                                      isNibException(portrait, "HVMNibFixture189Portrait");
                if (!orientationRejected) {
                    std::printf("landscape NIB raised: %s\nportrait NIB raised: %s\n",
                                landscape.empty() ? "(returned nil)" : landscape.c_str(),
                                portrait.empty() ? "(returned nil)" : portrait.c_str());
                }
            }
            if (writeNib(malformed, std::vector<uint8_t>{'b', 'a', 'd'})) {
                const std::string bad = nibFailure("HVMMalformedNib189");
                malformedRejected = isNibException(bad, "HVMMalformedNib189");
                if (!malformedRejected) std::printf("malformed NIB raised: %s\n", bad.empty() ? "(returned nil)" : bad.c_str());
            }
        }
        std::remove(exact.c_str()); std::remove(idiom.c_str()); std::remove(malformed.c_str());
        if (madeTestBundle) std::filesystem::remove(cx.proc.bundlePath(), bundleEc);
        check(decodedExact && decodedIdiom && preferredIdiom,
              "NSBundle decodes valid exact/~iphone NIBs and prefers idiom-specific resource");
        check(orientationRejected,
              "loadNibNamed: does not strip Landscape or Portrait from the nib name");
        check(malformedRejected,
              "loadNibNamed: malformed NIB raises NSInternalInconsistencyException");
    }

    // UIButton shadow offset getter/setter
    {
        const ObjC::Class btnCls = r.getClass("UIButton");
        const ObjC::id btn = UIKit::sendRect(cx, ObjC::id(r.send(cpu, btnCls, r.selAlloc)), r.sel("initWithFrame:"), {0.0f, 0.0f, 100.0f, 100.0f});

        CG::CGSize s{2.0f, 3.0f};
        const ObjC::SEL sSel = r.sel("setTitleShadowOffset:");

        // setTitleShadowOffset: takes CGSize which is passed in registers on ARMv7
        cx.proc.callGuest(cpu, r.lookupForSend(r.classOf(btn), sSel), {btn, sSel, std::bit_cast<uint32_t>(s.width), std::bit_cast<uint32_t>(s.height)});

        uint32_t tmp = cx.proc.heap().allocZeroed(16);
        const ObjC::SEL gSel = r.sel("titleShadowOffset");
        cx.proc.callGuest(cpu, r.lookupForSend(r.classOf(btn), gSel), {tmp, btn, gSel});
        CG::CGSize sOut = CG::readStruct<CG::CGSize>(cx, tmp);
        cx.proc.heap().free(tmp);

        check(sOut.width == 2.0f && sOut.height == 3.0f, "UIButton setTitleShadowOffset:/titleShadowOffset preserves custom shadow offset");
        const ObjC::id titleLabel = ObjC::id(r.send(cpu, btn, "titleLabel"));
        const uint32_t labelResult = cx.proc.heap().allocZeroed(12);
        const ObjC::SEL labelSel = r.sel("shadowOffset");
        cx.proc.callGuest(cpu, r.lookupForSend(r.classOf(titleLabel), labelSel), {labelResult, titleLabel, labelSel});
        const CG::CGSize labelOffset = CG::readStruct<CG::CGSize>(cx, labelResult);
        cx.proc.heap().free(labelResult);
        check(labelOffset.width == 2.0f && labelOffset.height == 3.0f,
              "UIButton shadow-offset setter propagates ARMv7 CGSize to its UILabel");
    }

    // A new UIButton reports a zero title shadow offset (UIKit's default), so its title label must start there
    // too; the UILabel default {0,-1} would draw a displaced shadow for a button that reports no offset.
    {
        const ObjC::Class btnCls = r.getClass("UIButton");
        const ObjC::id btn = UIKit::sendRect(cx, ObjC::id(r.send(cpu, btnCls, r.selAlloc)), r.sel("initWithFrame:"), {0.0f, 0.0f, 100.0f, 100.0f});
        const ObjC::SEL gSel = r.sel("titleShadowOffset");
        const uint32_t tmp = cx.proc.heap().allocZeroed(16);
        cx.proc.callGuest(cpu, r.lookupForSend(r.classOf(btn), gSel), {tmp, btn, gSel});
        const CG::CGSize reported = CG::readStruct<CG::CGSize>(cx, tmp);
        const ObjC::id titleLabel = ObjC::id(r.send(cpu, btn, "titleLabel"));
        const ObjC::SEL labelSel = r.sel("shadowOffset");
        cx.proc.callGuest(cpu, r.lookupForSend(r.classOf(titleLabel), labelSel), {tmp, titleLabel, labelSel});
        const CG::CGSize labelOffset = CG::readStruct<CG::CGSize>(cx, tmp);
        cx.proc.heap().free(tmp);
        check(reported.width == 0.0f && reported.height == 0.0f && labelOffset.width == 0.0f && labelOffset.height == 0.0f,
              "a new UIButton's title label starts with the button's zero shadow offset, not the UILabel default");
    }

    // 2d. UITabBarController, UITabBar, UITabBarItem
    {
        const ObjC::Class vcCls = r.getClass("UIViewController");
        const ObjC::id vc1 = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, vcCls, r.selAlloc)), r.selInit));
        const ObjC::id vc2 = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, vcCls, r.selAlloc)), r.selInit));
        r.send(cpu, vc1, "setTitle:", {Foundation::makeString(cx, "Tab 1")});
        r.send(cpu, vc2, "setTitle:", {Foundation::makeString(cx, "Tab 2")});
        const ObjC::Class tbcCls = r.getClass("UITabBarController");
        const ObjC::id tbc = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, tbcCls, r.selAlloc)), r.selInit));
        r.send(cpu, tbc, "setViewControllers:", {Foundation::makeArray(cx, {vc1, vc2})});
        r.send(cpu, tbc, "setSelectedIndex:", {1});
        check(ObjC::id(r.send(cpu, tbc, "selectedViewController")) == vc2 && uint32_t(r.send(cpu, tbc, "selectedIndex")) == 1, "UITabBarController viewControllers & selectedIndex");
    }

    // 2d3. Interface orientation is settled after launch (issue #306). A landscape app whose
    // root view controller says "portrait only" while setRootViewController: runs (Unity's
    // controller answers before the player has loaded its settings) must still come up in
    // its Info.plist orientation once the controller accepts it. An orientation the app
    // set itself with setStatusBarOrientation: is never undone.
    {
        auto& u = UIKit::ui(cx);
        const int32_t savedSb = u.statusBarOrientation, savedInit = u.initialOrientation;
        const bool savedByApp = u.orientationSetByApp;
        const ObjC::id savedKey = u.keyWindow;
        bool landscapeOk = false, onlyLandscapeLeft = false;
        auto rotCls = r.defineClass("HVMLateLandscapeController", "UIViewController");
        rotCls.method("shouldAutorotateToInterfaceOrientation:", [&](Ctx& rx) {
            const uint32_t o = rx.cpu.r[2];
            rx.cpu.r[0] = (o == 1 || (landscapeOk && (o == 4 || (o == 3 && !onlyLandscapeLeft)))) ? 1 : 0;
        }, "c@:i");
        auto makeWindow = [&]() {
            const ObjC::id w = UIKit::sendRect(cx, ObjC::id(r.send(cpu, r.getClass("UIWindow"), r.selAlloc)), r.sel("initWithFrame:"), {0, 0, 320, 480});
            r.send(cpu, w, "makeKeyAndVisible");
            return w;
        };
        u.initialOrientation = 3;          // UIInterfaceOrientationLandscapeRight from Info.plist
        u.statusBarOrientation = 3;
        u.orientationSetByApp = false;
        const ObjC::id win = makeWindow();
        const ObjC::id vc = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, rotCls.cls(), r.selAlloc)), r.selInit));
        r.send(cpu, win, "setRootViewController:", {vc});
        const bool portraitAtRoot = u.statusBarOrientation == 1;
        r.send(cpu, r.getClass("UIViewController"), "attemptRotationToDeviceOrientation");
        const bool stillPortrait = u.statusBarOrientation == 1;   // controller still says no
        landscapeOk = true;
        r.send(cpu, r.getClass("UIViewController"), "attemptRotationToDeviceOrientation");
        const bool landscapeAfter = u.statusBarOrientation == 3;
        const ObjC::id view = ObjC::id(r.send(cpu, vc, "view"));
        const UIKit::LayerData& vl = UIKit::layerData(cx, UIKit::viewLayer(cx, view));
        const bool rotatedBounds = vl.bounds.w > vl.bounds.h;     // the root view is laid out for landscape
        // An app whose Info.plist lists landscape only is never turned to portrait: not by a
        // controller that accepts portrait only, and not by setStatusBarOrientation: (Minecraft PE's
        // keyboard turned the whole game to portrait and left it there).
        landscapeOk = false;
        u.statusBarOrientation = 3;
        u.plistLandscapeOnly = true;
        const ObjC::id winL = makeWindow();
        const ObjC::id vcL = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, rotCls.cls(), r.selAlloc)), r.selInit));
        r.send(cpu, winL, "setRootViewController:", {vcL});
        const bool staysAtRoot = u.statusBarOrientation == 3;
        r.send(cpu, u.app ? u.app : r.createInstance(r.getClass("UIApplication")), "setStatusBarOrientation:", {1});
        const bool staysWhenAsked = u.statusBarOrientation == 3 && !u.orientationSetByApp;
        u.plistLandscapeOnly = false;
        check(staysAtRoot && staysWhenAsked, "an app that lists landscape only in Info.plist is never turned to portrait");
        // The app's own choice wins.
        landscapeOk = false;
        u.statusBarOrientation = 3;
        const ObjC::id win2 = makeWindow();
        const ObjC::id vc2 = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, rotCls.cls(), r.selAlloc)), r.selInit));
        r.send(cpu, win2, "setRootViewController:", {vc2});
        r.send(cpu, u.app ? u.app : r.createInstance(r.getClass("UIApplication")), "setStatusBarOrientation:", {4});
        landscapeOk = true;
        r.send(cpu, r.getClass("UIViewController"), "attemptRotationToDeviceOrientation");
        const bool appChoiceKept = u.statusBarOrientation == 4;
        // Info.plist names map to UIKit's values: LandscapeLeft is 4, LandscapeRight is 3. A controller
        // that accepts only LandscapeLeft must get it, and a second visible window follows along.
        const bool namesOk = UIKit::orientationFromString("UIInterfaceOrientationLandscapeLeft") == 4 &&
                             UIKit::orientationFromString("UIInterfaceOrientationLandscapeRight") == 3 &&
                             UIKit::orientationFromString("UIInterfaceOrientationPortraitUpsideDown") == 2 &&
                             UIKit::orientationFromString("UIInterfaceOrientationPortrait") == 1;
        u.orientationSetByApp = false;
        u.initialOrientation = UIKit::orientationFromString("UIInterfaceOrientationLandscapeLeft");
        u.statusBarOrientation = u.initialOrientation;
        landscapeOk = false;
        onlyLandscapeLeft = true;
        const ObjC::id win3 = makeWindow(), win4 = makeWindow();   // win4 ends up key
        const ObjC::id vc3 = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, rotCls.cls(), r.selAlloc)), r.selInit));
        const ObjC::id vc4 = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, rotCls.cls(), r.selAlloc)), r.selInit));
        r.send(cpu, win3, "setRootViewController:", {vc3});
        r.send(cpu, win4, "setRootViewController:", {vc4});
        const bool bothPortrait = u.statusBarOrientation == 1;
        landscapeOk = true;
        r.send(cpu, r.getClass("UIViewController"), "attemptRotationToDeviceOrientation");
        auto landscapeView = [&](ObjC::id c) {
            const UIKit::LayerData& l = UIKit::layerData(cx, UIKit::viewLayer(cx, ObjC::id(r.send(cpu, c, "view"))));
            return l.bounds.w > l.bounds.h;
        };
        const bool leftOnly = u.statusBarOrientation == 4 && landscapeView(vc4) && landscapeView(vc3);
        // An iOS 6 controller that says NO to shouldAutorotate is not rotated by
        // attemptRotationToDeviceOrientation, but the one-time settle after launch still applies.
        auto stillCls = r.defineClass("HVMNoAutorotateController", "HVMLateLandscapeController");
        stillCls.method("shouldAutorotate", [](Ctx& rx) { rx.cpu.r[0] = 0; }, "c@:");
        onlyLandscapeLeft = false;
        landscapeOk = false;
        u.initialOrientation = 3; u.statusBarOrientation = 3; u.orientationSetByApp = false;
        const ObjC::id win5 = makeWindow();
        const ObjC::id vc5 = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, stillCls.cls(), r.selAlloc)), r.selInit));
        r.send(cpu, win5, "setRootViewController:", {vc5});
        landscapeOk = true;
        r.send(cpu, r.getClass("UIViewController"), "attemptRotationToDeviceOrientation");
        const bool refusedRotation = u.statusBarOrientation == 1;
        UIKit::reevaluateRootOrientation(cx, "after launch");
        const bool settledAfterLaunch = u.statusBarOrientation == 3;
        r.send(cpu, win5, "setHidden:", {1});
        // A controller presented over the root before the recheck is laid out for the new
        // orientation too, and a presented controller's shouldAutorotate == NO vetoes rotation.
        u.statusBarOrientation = 1; landscapeOk = false;
        const ObjC::id win6 = makeWindow();
        const ObjC::id vc6 = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, rotCls.cls(), r.selAlloc)), r.selInit));
        r.send(cpu, win6, "setRootViewController:", {vc6});
        const ObjC::id shown = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, r.getClass("UIViewController"), r.selAlloc)), r.selInit));
        r.send(cpu, vc6, "presentViewController:animated:completion:", {shown, 0, 0});
        landscapeOk = true;
        r.send(cpu, r.getClass("UIViewController"), "attemptRotationToDeviceOrientation");
        const bool presentedFollows = u.statusBarOrientation == 3 && landscapeView(shown);
        r.send(cpu, vc6, "dismissViewControllerAnimated:completion:", {0, 0});
        u.statusBarOrientation = 1; landscapeOk = false;
        const ObjC::id vc7 = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, rotCls.cls(), r.selAlloc)), r.selInit));
        r.send(cpu, win6, "setRootViewController:", {vc7});
        const ObjC::id vetoer = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, stillCls.cls(), r.selAlloc)), r.selInit));
        r.send(cpu, vc7, "presentViewController:animated:completion:", {vetoer, 0, 0});
        landscapeOk = true;
        r.send(cpu, r.getClass("UIViewController"), "attemptRotationToDeviceOrientation");
        const bool presentedVeto = u.statusBarOrientation == 1;
        r.send(cpu, vc7, "dismissViewControllerAnimated:completion:", {0, 0});
        // A presented controller whose mask excludes the target orientation keeps portrait.
        u.statusBarOrientation = 1; landscapeOk = false;
        const ObjC::id vc8 = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, rotCls.cls(), r.selAlloc)), r.selInit));
        r.send(cpu, win6, "setRootViewController:", {vc8});
        auto portraitCls = r.defineClass("HVMPortraitOnlyController", "UIViewController");
        portraitCls.method("supportedInterfaceOrientations", [](Ctx& rx) { rx.cpu.r[0] = 1u << 1; }, "I@:");
        const ObjC::id portraitOnly = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, portraitCls.cls(), r.selAlloc)), r.selInit));
        r.send(cpu, vc8, "presentViewController:animated:completion:", {portraitOnly, 0, 0});
        landscapeOk = true;
        r.send(cpu, r.getClass("UIViewController"), "attemptRotationToDeviceOrientation");
        const bool presentedMask = u.statusBarOrientation == 1;
        r.send(cpu, vc8, "dismissViewControllerAnimated:completion:", {0, 0});
        r.send(cpu, win6, "setHidden:", {1});
        const std::pair<const char*, bool> parts[] = {
            {"portraitAtRoot", portraitAtRoot}, {"stillPortrait", stillPortrait}, {"landscapeAfter", landscapeAfter},
            {"rotatedBounds", rotatedBounds}, {"appChoiceKept", appChoiceKept}, {"namesOk", namesOk},
            {"bothPortrait", bothPortrait}, {"leftOnly", leftOnly}, {"refusedRotation", refusedRotation},
            {"settledAfterLaunch", settledAfterLaunch}, {"presentedFollows", presentedFollows},
            {"presentedVeto", presentedVeto}, {"presentedMask", presentedMask}};
        bool allParts = true;
        for (const auto& [name, passed] : parts) if (!passed) { std::printf("    orientation part failed: %s\n", name); allParts = false; }
        check(allParts,
              "orientation: a late-accepting root controller gets the Info.plist landscape (UIKit values, every window); an app-set orientation is kept");
        u.statusBarOrientation = savedSb; u.initialOrientation = savedInit; u.orientationSetByApp = savedByApp;
        if (savedKey) r.send(cpu, savedKey, "makeKeyWindow");
        for (ObjC::id w : {win, win2, win3, win4}) r.send(cpu, w, "setHidden:", {1});
    }

    // 2d2. KVC keys that nibs and older apps set on UIViewController (issue #354): iOS answers
    // them through the private _setStoryboard: / _setParentViewController:. parentViewController
    // is not retained; storyboard is.
    {
        const ObjC::Class vcCls = r.getClass("UIViewController");
        const ObjC::id child = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, vcCls, r.selAlloc)), r.selInit));
        const ObjC::id parent = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, vcCls, r.selAlloc)), r.selInit));
        const ObjC::id board = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, r.getClass("NSObject"), r.selAlloc)), r.selInit));
        auto rc = [&](ObjC::id o) { return uint32_t(r.send(cpu, o, "retainCount")); };
        bool ok = true;
        try {
            const uint32_t parentRc = rc(parent), boardRc = rc(board);
            r.send(cpu, child, "setValue:forKey:", {parent, Foundation::makeString(cx, "parentViewController")});
            ok = ok && ObjC::id(r.send(cpu, child, "parentViewController")) == parent && rc(parent) == parentRc;
            r.send(cpu, child, "setValue:forKey:", {board, Foundation::makeString(cx, "storyboard")});
            ok = ok && ObjC::id(r.send(cpu, child, "storyboard")) == board && rc(board) == boardRc + 1;
            r.send(cpu, child, "setValue:forKey:", {0, Foundation::makeString(cx, "storyboard")});
            ok = ok && ObjC::id(r.send(cpu, child, "storyboard")) == 0 && rc(board) == boardRc;
        } catch (const std::exception& e) {
            std::printf("    %s\n", e.what());
            ok = false;
        }
        check(ok, "UIViewController KVC: parentViewController (not retained) and storyboard (retained) keys");
    }

    // 2e. UITextField, UISwitch, UISlider, UISegmentedControl, UIStoryboard
    {
        const ObjC::Class tfCls = r.getClass("UITextField");
        const ObjC::id tf = UIKit::sendRect(cx, ObjC::id(r.send(cpu, tfCls, r.selAlloc)), r.sel("initWithFrame:"), {0, 0, 120, 30});
        r.send(cpu, tf, "setText:", {Foundation::makeString(cx, "HelloInput")});
        const bool became = (r.send(cpu, tf, "becomeFirstResponder") & 0xFF) != 0;
        const bool editing = (r.send(cpu, tf, "isEditing") & 0xFF) != 0;
        r.send(cpu, tf, "resignFirstResponder");
        check(became && editing && Foundation::utf8(cx, ObjC::id(r.send(cpu, tf, "text"))) == "HelloInput", "UITextField text & firstResponder editing lifecycle");

        const ObjC::Class swCls = r.getClass("UISwitch");
        const ObjC::id sw = UIKit::sendRect(cx, ObjC::id(r.send(cpu, swCls, r.selAlloc)), r.sel("initWithFrame:"), {0, 0, 79, 27});
        r.send(cpu, sw, "setOn:animated:", {1, 0});
        const ObjC::Class slCls = r.getClass("UISlider");
        const ObjC::id sl = UIKit::sendRect(cx, ObjC::id(r.send(cpu, slCls, r.selAlloc)), r.sel("initWithFrame:"), {0, 0, 100, 23});
        r.send(cpu, sl, "setMaximumValue:", {UIKit::fbits(100.0f)});
        r.send(cpu, sl, "setValue:animated:", {UIKit::fbits(42.5f), 0});
        const float slVal = UIKit::bitsf(uint32_t(r.send(cpu, sl, "value")));
        const ObjC::Class segCls = r.getClass("UISegmentedControl");
        const ObjC::id seg = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, segCls, r.selAlloc)), "initWithItems:",
                                             {Foundation::makeArray(cx, {Foundation::makeString(cx, "A"), Foundation::makeString(cx, "B")})}));
        r.send(cpu, seg, "setSelectedSegmentIndex:", {1});
        check((r.send(cpu, sw, "isOn") & 0xFF) != 0 && std::abs(slVal - 42.5f) < 0.01f && int32_t(r.send(cpu, seg, "selectedSegmentIndex")) == 1,
              "UISwitch, UISlider, and UISegmentedControl");
    }

    // On-screen keyboard (#303): a focused text field asks the host for a keyboard with its traits; the edits the
    // host sends back go through the guest's delegate, notifications and control events, on the main run loop.
    {
        if (!UIKit::ui(proc).app) UIKit::ui(proc).app = r.createInstance(r.getClass("UIApplication"));   // control events go through it
        std::vector<UIKit::TextInputRequest> requests;
        UIKit::setTextInputObserver(proc, [&](const UIKit::TextInputRequest& q) { requests.push_back(q); });
        std::vector<std::string> changes;
        int returns = 0, began = 0, ended = 0, editingChanged = 0, viewChanged = 0;
        ObjC::ClassBuilder del = r.defineClass("HVMTestTextDelegate", "NSObject");
        del.method("textField:shouldChangeCharactersInRange:replacementString:", [&](Ctx& x) {
            Runtime::ArgReader args(x.cpu, x.mem, 2);
            args.word();   // the text field
            const uint32_t loc = args.word(), len = args.word();
            const std::string rep = Foundation::utf8(x, args.word());
            changes.push_back(std::to_string(loc) + "," + std::to_string(len) + ":" + rep);
            x.cpu.r[0] = rep.find('x') == std::string::npos ? 1 : 0;   // refuse any edit that adds an 'x'
        }, "c@:@{_NSRange=II}@");
        del.method("textFieldShouldReturn:", [&](Ctx& x) { ++returns; x.cpu.r[0] = 1; }, "c@:@");
        del.method("textFieldDidBeginEditing:", [&](Ctx&) { ++began; }, "v@:@");
        bool refuseEnd = false, rewrite = false;
        del.method("textFieldShouldEndEditing:", [&](Ctx& x) { x.cpu.r[0] = refuseEnd ? 0 : 1; }, "c@:@");
        del.method("rewrite:", [&](Ctx& x) {
            if (rewrite) { rewrite = false; ObjC::rt(x).send(x.cpu, x.cpu.r[2], "setText:", {Foundation::makeString(x, "REWRITTEN")}); }
        }, "v@:@");
        del.method("textFieldDidEndEditing:", [&](Ctx&) { ++ended; }, "v@:@");
        del.method("changed:", [&](Ctx&) { ++editingChanged; }, "v@:@");
        del.method("textViewDidChange:", [&](Ctx&) { ++viewChanged; }, "v@:@");
        const ObjC::id delegate = r.createInstance(del.cls());
        uint64_t seq = 0;
        const auto send = [&](UIKit::TextInputEvent::Kind k, const std::string& t) {
            UIKit::submitTextInput(proc, {k, t, requests.empty() ? 0 : requests.back().focusId, ++seq});
        };
        const auto pump = [&] { for (int i = 0; i < 3; ++i) Foundation::mainRunLoop(proc).runOnce(cpu, 0.0); };
        const auto textOfField = [&](ObjC::id f) { return Foundation::utf8(cx, ObjC::id(r.send(cpu, f, "text"))); };

        const ObjC::id field = UIKit::sendRect(cx, ObjC::id(r.send(cpu, r.getClass("UITextField"), r.selAlloc)), r.sel("initWithFrame:"), {0, 0, 120, 30});
        r.send(cpu, field, "setDelegate:", {delegate});
        r.send(cpu, field, "setKeyboardType:", {4});
        r.send(cpu, field, "setReturnKeyType:", {4});
        r.send(cpu, field, "setSecureTextEntry:", {1});
        r.send(cpu, field, "setPlaceholder:", {Foundation::makeString(cx, "Name")});
        r.send(cpu, field, "addTarget:action:forControlEvents:", {delegate, r.sel("changed:"), 1u << 17});
        check(uint32_t(r.send(cpu, field, "keyboardType")) == 4 && uint32_t(r.send(cpu, field, "returnKeyType")) == 4,
              "UITextField keeps the keyboard and return key traits it was given");
        const bool became = (r.send(cpu, field, "becomeFirstResponder") & 0xFF) != 0;
        check(became && requests.size() == 1 && requests[0].active && requests[0].keyboardType == 4 && requests[0].returnKeyType == 4 &&
              requests[0].secure && !requests[0].multiline && requests[0].placeholder == "Name" && began == 1,
              "a text field that becomes first responder asks the host for a keyboard with its traits");

        send(UIKit::TextInputEvent::SetText, "ab"); pump();
        check(textOfField(field) == "ab" && changes == std::vector<std::string>{"0,0:ab"} && editingChanged == 1,
              "typed text reaches the field through the delegate and the editing-changed control event");
        send(UIKit::TextInputEvent::SetText, "abx"); pump();
        check(textOfField(field) == "ab" && changes.back() == "2,0:x" && editingChanged == 1 && requests.back().active && requests.back().text == "ab",
              "an edit the delegate refuses is not applied, and the host is told the text to go back to");
        send(UIKit::TextInputEvent::SetText, "a"); pump();
        check(textOfField(field) == "a" && changes.back() == "1,1:" && editingChanged == 2, "deleting reaches the delegate as a replacement of the removed range");
        send(UIKit::TextInputEvent::Return, ""); pump();
        check(returns == 1, "the return key asks the delegate textFieldShouldReturn:");
        send(UIKit::TextInputEvent::Dismiss, ""); pump();
        check((r.send(cpu, field, "isFirstResponder") & 0xFF) == 0 && ended == 1 && !requests.back().active,
              "dismissing the keyboard resigns the field and tells the host the keyboard is gone");

        // A refused edit is acknowledged by sequence number, so a correction that arrives after newer typing is stale.
        r.send(cpu, field, "becomeFirstResponder");
        const uint64_t focusA = requests.back().focusId;
        send(UIKit::TextInputEvent::SetText, "q"); pump();
        send(UIKit::TextInputEvent::SetText, "qx"); pump();   // refused by the delegate
        check(requests.back().ackSeq == seq && requests.back().text == "q", "a correction names the host edit it answers (ackSeq)");

        // Guest code that rewrites the text during a callback is published back to the keyboard.
        r.send(cpu, field, "addTarget:action:forControlEvents:", {delegate, r.sel("rewrite:"), 1u << 17});
        rewrite = true;
        send(UIKit::TextInputEvent::SetText, "qw"); pump();
        rewrite = false;
        check(textOfField(field) == "REWRITTEN" && requests.back().text == "REWRITTEN",
              "text changed by guest callbacks after an edit is republished to the keyboard");

        // The guest may refuse to end editing: the keyboard stays.
        refuseEnd = true;
        send(UIKit::TextInputEvent::Dismiss, ""); pump();
        check((r.send(cpu, field, "isFirstResponder") & 0xFF) != 0 && requests.back().active,
              "when the guest refuses to resign, the keyboard is told the field is still active");
        refuseEnd = false;

        // The guest setting the focused field's text directly (clearing it after "send") reaches the keyboard;
        // an unchanged value does not, and the UIKit default autocapitalization (Sentences) is kept.
        const size_t setTextBefore = requests.size();
        r.send(cpu, field, "setText:", {Foundation::makeString(cx, "cleared")});
        check(requests.size() == setTextBefore + 1 && requests.back().active && requests.back().text == "cleared",
              "setText: on the focused field publishes the new text to the keyboard");
        r.send(cpu, field, "setText:", {Foundation::makeString(cx, "cleared")});
        check(requests.size() == setTextBefore + 1, "setText: with an unchanged value does not republish");
        check(uint32_t(r.send(cpu, field, "autocapitalizationType")) == 2 && requests.back().autocapitalization == 2,
              "a field that never sets autocapitalizationType defaults to Sentences, as on iOS");

        // Host edits replace whole characters: U+1F600 -> U+1F601 share a high surrogate, and the delegate must see the
        // whole emoji as the replaced range and the replacement, not a lone low surrogate.
        send(UIKit::TextInputEvent::SetText, "cleared\xF0\x9F\x98\x80"); pump();
        send(UIKit::TextInputEvent::SetText, "cleared\xF0\x9F\x98\x81"); pump();
        check(textOfField(field) == "cleared\xF0\x9F\x98\x81" && changes.back() == "7,2:\xF0\x9F\x98\x81",
              "replacing one emoji with another reports whole characters (no split surrogate pair) to the delegate");

        // Events for a field that has lost focus never reach the new first responder.
        const ObjC::id second = UIKit::sendRect(cx, ObjC::id(r.send(cpu, r.getClass("UITextField"), r.selAlloc)), r.sel("initWithFrame:"), {0, 120, 120, 30});
        r.send(cpu, second, "becomeFirstResponder");
        check(requests.back().focusId != focusA, "each focus gets its own id");
        UIKit::submitTextInput(proc, {UIKit::TextInputEvent::SetText, "stale", focusA, ++seq}); pump();
        UIKit::submitTextInput(proc, {UIKit::TextInputEvent::Dismiss, "", focusA, ++seq}); pump();
        check(textOfField(second).empty() && (r.send(cpu, second, "isFirstResponder") & 0xFF) != 0,
              "text and dismiss events for an older focus are dropped");
        r.send(cpu, second, "resignFirstResponder");

        // A tap focuses a field, as on iOS, and a UITextView asks for a multi-line keyboard.
        const ObjC::id other = UIKit::sendRect(cx, ObjC::id(r.send(cpu, r.getClass("UITextField"), r.selAlloc)), r.sel("initWithFrame:"), {0, 40, 120, 30});
        const size_t reqBefore = requests.size();
        r.send(cpu, other, "touchesEnded:withEvent:", {ObjC::id(r.send(cpu, r.getClass("NSSet"), "set")), 0});
        check((r.send(cpu, other, "isFirstResponder") & 0xFF) != 0 && requests.size() == reqBefore + 1 && requests.back().active,
              "tapping a text field focuses it and asks for the keyboard");
        r.send(cpu, other, "resignFirstResponder");
        const ObjC::id view = UIKit::sendRect(cx, ObjC::id(r.send(cpu, r.getClass("UITextView"), r.selAlloc)), r.sel("initWithFrame:"), {0, 80, 120, 80});
        r.send(cpu, view, "setDelegate:", {delegate});
        r.send(cpu, view, "becomeFirstResponder");
        send(UIKit::TextInputEvent::SetText, "hi\nthere"); pump();
        check(requests.back().multiline && textOfField(view) == "hi\nthere" && viewChanged == 1,
              "a UITextView asks for a multi-line keyboard and reports changes through textViewDidChange:");
        r.send(cpu, view, "resignFirstResponder");
        UIKit::setTextInputObserver(proc, nullptr);
    }

    // Custom UIKeyInput responders must request the host keyboard too.
    {
        std::vector<UIKit::TextInputRequest> requests;
        UIKit::setTextInputObserver(proc, [&](const UIKit::TextInputRequest& q) { requests.push_back(q); });
        ObjC::ClassBuilder custom = r.defineClass("HVMTestCustomKeyInput", "UIView");
        std::vector<std::string> inserted;
        int deleted = 0; bool refuseEnd = false;
        custom.method("canBecomeFirstResponder", [](Ctx& x) { x.cpu.r[0] = 1; }, "c@:");
        custom.method("hasText", [&](Ctx& x) { x.cpu.r[0] = inserted.empty() ? 0 : 1; }, "c@:");
        custom.method("insertText:", [&](Ctx& x) { inserted.push_back(Foundation::utf8(x, x.cpu.r[2])); }, "v@:@");
        custom.method("deleteBackward", [&](Ctx&) { ++deleted; }, "v@:");
        custom.method("keyboardType", [](Ctx& x) { x.cpu.r[0] = 4; }, "i@:");
        custom.method("returnKeyType", [](Ctx& x) { x.cpu.r[0] = 9; }, "i@:");
        custom.method("resignFirstResponder", [&](Ctx& x) {
            x.cpu.r[0] = refuseEnd ? 0 : uint32_t(ObjC::rt(x).sendSuper(x.cpu, x.cpu.r[0], UIKit::ui(x).responder, x.cpu.r[1]));
        }, "c@:");
        const ObjC::id input = UIKit::sendRect(cx, ObjC::id(r.send(cpu, custom.cls(), r.selAlloc)), r.sel("initWithFrame:"), {0, 0, 120, 30});
        const bool became = (r.send(cpu, input, "becomeFirstResponder") & 0xFF) != 0;
        check(became && requests.size() == 1 && requests.back().active && requests.back().keyInput &&
              requests.back().keyboardType == 4 && requests.back().returnKeyType == 9 && !requests.back().hasText,
              "custom UIView implementing UIKeyInput requests host keyboard when focused");
        const uint64_t focus = requests.empty() ? 0 : requests.back().focusId;
        const auto send = [&](UIKit::TextInputEvent::Kind kind, const std::string& text, uint64_t target) {
            UIKit::submitTextInput(proc, {kind, text, target, 0});
            for (int i = 0; i < 3; ++i) Foundation::mainRunLoop(proc).runOnce(cpu, 0.0);
        };
        r.send(cpu, input, "becomeFirstResponder");
        check(requests.size() == 1, "refocusing the same custom responder keeps its focus and keyboard");
        send(UIKit::TextInputEvent::InsertText, "A\xF0\x9F\x98\x80", focus);
        send(UIKit::TextInputEvent::DeleteBackward, "", focus);
        send(UIKit::TextInputEvent::Return, "", focus);
        check(inserted == std::vector<std::string>{"A\xF0\x9F\x98\x80", "\n"} && deleted == 1 && requests.back().hasText,
              "custom UIKeyInput receives original Unicode insert, delete and return messages");
        send(UIKit::TextInputEvent::SetText, "invented full-text replacement", focus);
        send(UIKit::TextInputEvent::InsertText, "stale", focus + 1);
        send(UIKit::TextInputEvent::Dismiss, "", focus + 1);
        check(inserted.size() == 2 && (r.send(cpu, input, "isFirstResponder") & 0xFF),
              "custom UIKeyInput drops full-text replacement and events for another focus");
        refuseEnd = true;
        send(UIKit::TextInputEvent::Dismiss, "", focus);
        const ObjC::id field = UIKit::sendRect(cx, ObjC::id(r.send(cpu, r.getClass("UITextField"), r.selAlloc)), r.sel("initWithFrame:"), {0, 40, 120, 30});
        check((r.send(cpu, field, "becomeFirstResponder") & 0xFF) == 0 && (r.send(cpu, input, "isFirstResponder") & 0xFF) && requests.back().active,
              "custom responder refusal keeps focus and keyboard during dismiss or focus transfer");
        refuseEnd = false;
        send(UIKit::TextInputEvent::Dismiss, "", focus);
        check(!(r.send(cpu, input, "isFirstResponder") & 0xFF) && !requests.back().active,
              "custom UIKeyInput dismissal publishes an inactive request");
        r.send(cpu, field, "becomeFirstResponder");
        send(UIKit::TextInputEvent::InsertText, "old responder", focus);
        check(Foundation::utf8(cx, ObjC::id(r.send(cpu, field, "text"))).empty() && requests.back().focusId != focus,
              "switching from custom input to UITextField rejects old incremental events");
        r.send(cpu, field, "resignFirstResponder");
        r.send(cpu, input, "resignFirstResponder");
        UIKit::setTextInputObserver(proc, nullptr);
    }

    // GL presents are held to the display rate by a schedule of deadlines. A game driven by the
    // 60 Hz frame loop is never held (it was, and ran at 50-55 fps on the phone); a game that
    // presents in a tight loop gets exactly one present per period however late each sleep wakes.
    {
        const int64_t ms = 1'000'000;
        HostGL::PresentPacer grid;
        int held = 0;
        int64_t t = 5 * ms;
        for (int i = 0; i < 600; ++i) {
            // one present per 16.667 ms frame, give or take 0.8 ms
            const int64_t at = t + (i % 3 - 1) * (ms * 8 / 10);
            if (grid.next(at)) ++held;
            t += 16'666'667;
        }
        HostGL::PresentPacer loop;
        int64_t now = 5 * ms, first = 0, last = 0;
        int presents = 0;
        for (int i = 0; i < 600; ++i) {
            const int64_t until = loop.next(now);
            if (until) now = until + ms + ms / 2;   // every sleep wakes 1.5 ms late
            if (!first) first = now;
            last = now;
            ++presents;
            now += ms;                              // 1 ms of work, then the next present
        }
        const double period = double(last - first) / double(presents - 1) / double(ms);
        if (!(held == 0 && period > 16.4 && period < 16.6)) std::printf("    pacing: held %d period %.3f ms\n", held, period);
        check(held == 0, "a game paced by the 60 Hz frame loop is never held at present");
        check(period > 16.4 && period < 16.6, "a tight render loop presents once per period, however late its sleeps wake");
    }

    // Video is played by the host: MPMoviePlayerController hands it the file and stays "playing"
    // until the host says the movie ended (Goat Simulator's logo, Minion Rush's loading video).
    // Without a host player the movie finishes at once, as before.
    {
        const std::string moviePath = proc.homePath() + "/intro-test.mp4";
        { std::ofstream file(moviePath, std::ios::binary); file << "not really a movie"; }
        const auto url = ObjC::id(r.send(cpu, r.getClass("NSURL"), "fileURLWithPath:", {Foundation::makeString(cx, moviePath)}));
        auto newPlayer = [&] {
            return ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, r.getClass("MPMoviePlayerController"), r.selAlloc)), "initWithContentURL:", {url}));
        };
        auto state = [&](ObjC::id player) { return uint32_t(r.send(cpu, player, "playbackState")); };
        // Runs the main loop for up to a second, or until the condition holds.
        auto spin = [&](const std::function<bool()>& done) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (!done() && std::chrono::steady_clock::now() < deadline) {
                Foundation::mainRunLoop(proc).runOnce(cpu, 0.01);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        };
        auto settle = [&] { const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(120); spin([&] { return std::chrono::steady_clock::now() >= until; }); };
        const auto plain = newPlayer();
        r.send(cpu, plain, "play");
        const bool playsFirst = state(plain) == 1;
        spin([&] { return state(plain) == 0; });
        const bool finishesAlone = playsFirst && state(plain) == 0;
        std::vector<UIKit::MovieRequest> requests;
        UIKit::setMovieObserver(proc, [&](const UIKit::MovieRequest& q) { requests.push_back(q); });
        const auto hosted = newPlayer();
        r.send(cpu, hosted, "play");
        settle();
        const bool asked = requests.size() == 1 && requests[0].active && requests[0].requestId && requests[0].hostPath == proc.hostPath(moviePath);
        const bool stillPlaying = state(hosted) == 1;
        UIKit::movieFinished(proc, requests.empty() ? 0 : requests[0].requestId + 1000);   // an answer for another movie
        settle();
        const bool ignoresStranger = state(hosted) == 1;
        UIKit::movieFinished(proc, requests.empty() ? 0 : requests[0].requestId);
        spin([&] { return state(hosted) == 0; });
        const bool ended = state(hosted) == 0 && requests.size() == 2 && !requests[1].active &&
                           requests[1].requestId == requests[0].requestId;
        // The game stops a movie itself (a tap to skip): the host is told to take it down.
        const auto skipped = newPlayer();
        r.send(cpu, skipped, "play");
        r.send(cpu, skipped, "stop");
        settle();
        const bool stopped = requests.size() == 4 && requests[2].active && !requests[3].active && requests[3].requestId == requests[2].requestId &&
                             state(skipped) == 0;
        // An AVPlayer on a video file goes the same way; replacing its item takes the video down.
        const size_t before = requests.size();
        const auto av = ObjC::id(r.send(cpu, r.getClass("AVPlayer"), "playerWithURL:", {url}));
        r.send(cpu, av, "play");
        settle();
        const bool avAsked = requests.size() == before + 1 && requests.back().active;
        const uint64_t avId = requests.back().requestId;
        r.send(cpu, av, "replaceCurrentItemWithPlayerItem:", {0});
        const bool avReplaced = requests.size() == before + 2 && !requests.back().active && requests.back().requestId == avId;
        // Starting a second movie while one plays ends the first (its player is let go).
        const auto first = newPlayer(), second = newPlayer();
        r.send(cpu, first, "play");
        r.send(cpu, second, "play");
        settle();
        const bool replacedFirst = state(second) == 1 && requests.size() == before + 5 && !requests[before + 3].active;
        r.send(cpu, second, "stop");
        settle();
        UIKit::setMovieObserver(proc, nullptr);
        check(avAsked && avReplaced, "an AVPlayer's video file is handed to the host, and replacing its item takes it down");
        check(replacedFirst, "a second movie replaces the one that is playing");
        if (!(asked && stillPlaying && ignoresStranger && ended && stopped))
            std::printf("    movie: asked %d playing %d stranger %d ended %d stopped %d requests %zu\n", asked, stillPlaying, ignoresStranger, ended, stopped, requests.size());
        check(finishesAlone, "without a host player a movie finishes at once");
        check(asked && stillPlaying, "a movie file is handed to the host and stays playing");
        check(ignoresStranger && ended, "the movie ends when the host reports its own request finished, and the host is told to take it down");
        check(stopped, "a movie the game stops is taken down on the host");
    }

    // Photo-library presentation is a shared host service, with guest-main-loop delivery.
    {
        std::vector<UIKit::PhotoPickerRequest> requests;
        check(!(r.send(cpu,r.getClass("UIImagePickerController"),"isSourceTypeAvailable:",{0})&255),"photo library is unavailable without a host provider");
        UIKit::setPhotoPickerObserver(proc,[&](const auto& q){requests.push_back(q);});
        check((r.send(cpu,r.getClass("UIImagePickerController"),"isSourceTypeAvailable:",{0})&255) && !(r.send(cpu,r.getClass("UIImagePickerController"),"isSourceTypeAvailable:",{1})&255),"photo host enables library, never camera capture");
        int selected=0,cancelled=0;bool imageOk=false;
        ObjC::ClassBuilder del=r.defineClass("HVMPhotoDelegate","NSObject");
        del.method("imagePickerController:didFinishPickingMediaWithInfo:",[&](Ctx& x){
            ++selected;const auto info=x.cpu.r[3];const auto img=Foundation::dictGet(x,info,Foundation::makeString(x,"UIImagePickerControllerOriginalImage"));
            const auto cg=ObjC::id(r.send(cpu,img,"CGImage"));auto pixels=CG::imageOf(x,cg);
            imageOk=pixels && pixels->width==2 && pixels->height==1 && pixels->rgba[0]==255 && pixels->rgba[5]==255;
            r.send(cpu,x.cpu.r[2],"dismissViewControllerAnimated:completion:",{0,0});
        },"v@:@@");
        del.method("imagePickerControllerDidCancel:",[&](Ctx& x){++cancelled;r.send(cpu,x.cpu.r[2],"dismissViewControllerAnimated:completion:",{0,0});},"v@:@");
        const auto delegate=r.createInstance(del.cls());
        const auto presenter=ObjC::id(r.send(cpu,r.getClass("UIViewController"),r.sel("new")));
        const auto picker=ObjC::id(r.send(cpu,r.getClass("UIImagePickerController"),r.sel("new")));
        r.send(cpu,picker,"setDelegate:",{delegate});r.send(cpu,picker,"setAllowsEditing:",{1});
        check((r.send(cpu,picker,"allowsEditing")&255) && uint32_t(r.send(cpu,picker,"delegate"))==delegate,"image picker preserves delegate and configuration");
        const auto present=[&]{r.send(cpu,presenter,"presentViewController:animated:completion:",{picker,0,0});};
        const auto pump=[&]{for(int i=0;i<3;++i)Foundation::mainRunLoop(proc).runOnce(cpu,0.0);};
        present();const auto first=requests.back().requestId;
        Graphics::Image image;image.width=2;image.height=1;image.rgba={255,0,0,255,0,255,0,255};const auto png=Graphics::encodePng(image);
        UIKit::submitPhotoPickerResult(proc,first+1,png);pump();check(selected==0,"image picker drops mismatched request epochs");
        UIKit::submitPhotoPickerResult(proc,first,png);check(selected==0,"photo result waits for the guest main run loop");pump();
        check(selected==1 && imageOk && !requests.back().active,"selected pixels reach the UIImage delegate, followed by dismissal");
        UIKit::submitPhotoPickerResult(proc,first,png);pump();check(selected==1,"dismissed photo requests cannot deliver duplicate results");
        present();const auto second=requests.back().requestId;UIKit::submitPhotoPickerResult(proc,first,png);UIKit::submitPhotoPickerResult(proc,second,{});pump();
        check(second!=first && selected==1 && cancelled==1 && !requests.back().active,"new photo requests reject stale selections and deliver cancellation");
        r.release(cpu,picker);r.release(cpu,presenter);r.release(cpu,delegate);
        UIKit::setPhotoPickerObserver(proc,nullptr);
    }

    // Import-only document selection copies bytes before delivering a guest URL.
    {
        std::vector<UIKit::DocumentPickerRequest> requests;
        UIKit::setDocumentPickerObserver(proc, [&](const auto& q) { requests.push_back(q); });
        int selected = 0, cancelled = 0;
        std::string imported;
        ObjC::ClassBuilder delegateClass = r.defineClass("HVMDocumentDelegate", "NSObject");
        delegateClass.method("documentPicker:didPickDocumentAtURL:", [&](Ctx& x) {
            ++selected;
            imported = Foundation::urlFilePath(x, x.cpu.r[3]);
            r.send(cpu, x.cpu.r[2], "dismissViewControllerAnimated:completion:", {0, 0});
        }, "v@:@@");
        delegateClass.method("documentPickerWasCancelled:", [&](Ctx& x) {
            ++cancelled;
            r.send(cpu, x.cpu.r[2], "dismissViewControllerAnimated:completion:", {0, 0});
        }, "v@:@");
        const auto delegate = r.createInstance(delegateClass.cls());
        const auto presenter = ObjC::id(r.send(cpu, r.getClass("UIViewController"), r.sel("new")));
        const auto types = Foundation::makeArray(cx, {Foundation::makeString(cx, "public.mp3")});
        const auto picker = ObjC::id(r.send(cpu, r.send(cpu, r.getClass("UIDocumentPickerViewController"), "alloc"),
                                           "initWithDocumentTypes:inMode:", {types, 0}));
        r.send(cpu, picker, "setDelegate:", {delegate});
        const auto present = [&] { r.send(cpu, presenter, "presentViewController:animated:completion:", {picker, 0, 0}); };
        const auto pump = [&] { for (int i = 0; i < 3; ++i) Foundation::mainRunLoop(proc).runOnce(cpu, 0.0); };
        const auto source = std::filesystem::temp_directory_path() / ("hvm-document-source-" + std::to_string(picker) + ".mp3");
        { std::ofstream file(source, std::ios::binary); file << "independent audio fixture"; }
        present();
        const auto first = requests.back().requestId;
        check(requests.back().types == std::vector<std::string>{"public.mp3"}, "document picker publishes requested content types");
        UIKit::submitDocumentPickerResult(proc, first + 1, source.string()); pump();
        check(selected == 0 && cancelled == 0, "document picker rejects stale result IDs");
        UIKit::submitDocumentPickerResult(proc, first, source.string());
        check(selected == 0, "document picker delivers on the guest main loop");
        pump();
        std::ifstream copy(imported, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(copy)), {});
        check(selected == 1 && bytes == "independent audio fixture" && imported != source.string() && !requests.back().active,
              "document picker imports an independent sandbox copy and dismisses");
        UIKit::submitDocumentPickerResult(proc, first, source.string()); pump();
        check(selected == 1, "document picker ignores duplicate delivery");
        present(); const auto second = requests.back().requestId;
        UIKit::submitDocumentPickerResult(proc, first, source.string());
        UIKit::submitDocumentPickerResult(proc, second, {}); pump();
        check(selected == 1 && cancelled == 1 && second != first, "document picker cancellation preserves its earlier import");
        present(); UIKit::submitDocumentPickerResult(proc, requests.back().requestId, source.string() + ".missing"); pump();
        check(cancelled == 2 && std::filesystem::exists(source), "unreadable document selection cancels and preserves its source");
        std::filesystem::remove(source);
        if (!imported.empty()) { std::filesystem::remove(imported); std::filesystem::remove(std::filesystem::path(imported).parent_path()); }
        r.release(cpu, picker); r.release(cpu, presenter); r.release(cpu, delegate);
        UIKit::setDocumentPickerObserver(proc, nullptr);
    }

    // Composed ranges use UTF-16 coordinates and expand to whole Unicode clusters.
    {
        const auto range = [&](const std::u16string& text, uint32_t location, uint32_t length, bool atIndex = false) {
            const ObjC::id string = Foundation::makeString(cx, text);
            const ObjC::SEL selector = r.sel(atIndex ? "rangeOfComposedCharacterSequenceAtIndex:" : "rangeOfComposedCharacterSequencesForRange:");
            const ObjC::IMP imp = r.lookupForSend(r.classOf(string), selector);
            if (!imp) throw ObjC::ObjCError("missing composed string range method");
            const uint32_t out = proc.heap().allocZeroed(8);
            try {
                if (atIndex) proc.callGuest(cpu, imp, {out, string, selector, location});
                else proc.callGuest(cpu, imp, {out, string, selector, location, length});
            } catch (...) { proc.heap().free(out); throw; }
            const auto result = std::pair{proc.mem().read<uint32_t>(out), proc.mem().read<uint32_t>(out + 4)};
            proc.heap().free(out); return result;
        };
        struct Case { const char16_t* text; uint32_t location, length, first, count; };
        const Case cases[] = {
            {u"abc", 1, 1, 1, 1}, {u"a\u0301b", 1, 1, 0, 2},
            {u"A\U0001F600Z", 2, 1, 1, 2}, {u"x\U0001F469\u200D\U0001F4BBy", 4, 1, 1, 5},
            {u"a\U0001F44D\U0001F3FDb", 3, 1, 1, 4}, {u"x\U0001F1FA\U0001F1F8y", 3, 1, 1, 4},
            {u"x\u1100\u1161\u11A8y", 2, 1, 1, 3}, {u"x\r\ny", 2, 1, 1, 2},
            {u"a\u0301b\U0001F600c", 1, 3, 0, 5}, {u"a\u0301b", 1, 0, 0, 2},
            {u"a\u0301b", 2, 0, 2, 1}, {u"a\u0301b", 3, 0, 3, 0}, {u"", 0, 0, 0, 0}
        };
        bool expanded = true;
        for (const auto& c : cases) {
            try { expanded &= range(c.text, c.location, c.length) == std::pair{c.first, c.count}; }
            catch (...) { expanded = false; }
        }
        check(expanded, "NSString composed ranges expand accents, surrogates, joined emoji, modifiers, flags, Hangul and CRLF");
        bool indexed = false;
        try { indexed = range(u"a\u0301b", 1, 0, true) == std::pair{0u, 2u} && range(u"A\U0001F600Z", 2, 0, true) == std::pair{1u, 2u}; }
        catch (...) {}
        check(indexed, "NSString composed sequence at index shares the same Unicode boundaries");
        bool invalid = true;
        for (const auto& c : std::vector<std::pair<uint32_t,uint32_t>>{{4,0}, {3,1}, {1,UINT32_MAX}, {UINT32_MAX,0}}) {
            bool threw = false; try { range(u"abc", c.first, c.second); }
            catch (const std::exception& error) { threw = std::string(error.what()).find("out of bounds") != std::string::npos; }
            invalid &= threw;
        }
        check(invalid, "NSString composed ranges reject invalid and overflowing guest bounds");
    }

    // Key-path KVC goes through a class's own valueForKey:/setValue:forKey: (iPhoto reads "mainTabIndex" this way).
    {
        const ObjC::Class udCls = r.getClass("NSUserDefaults");
        const ObjC::id ud = ObjC::id(r.send(cpu, udCls, r.sel("standardUserDefaults")));
        const ObjC::id key = Runtime::Foundation::makeString(cx, "kvcMainTabIndex");
        bool raised = false;
        ObjC::id unsetValue = 1;
        try { unsetValue = ObjC::id(r.send(cpu, ud, r.sel("valueForKeyPath:"), {uint32_t(key)})); }
        catch (const std::exception&) { raised = true; }
        check(!raised && unsetValue == 0, "NSUserDefaults valueForKeyPath: of an unset key returns nil instead of raising");
        const ObjC::id stored = Runtime::Foundation::makeString(cx, "tab-3");
        r.send(cpu, ud, r.sel("setValue:forKeyPath:"), {uint32_t(stored), uint32_t(key)});
        const ObjC::id viaObject = ObjC::id(r.send(cpu, ud, r.sel("objectForKey:"), {uint32_t(key)}));
        check(viaObject && Runtime::Foundation::utf8(cx, viaObject) == "tab-3",
              "NSUserDefaults setValue:forKeyPath: writes through to the defaults");
        const ObjC::id viaPath = ObjC::id(r.send(cpu, ud, r.sel("valueForKeyPath:"), {uint32_t(key)}));
        check(viaPath && Runtime::Foundation::utf8(cx, viaPath) == "tab-3",
              "NSUserDefaults valueForKeyPath: reads the stored value");
        const ObjC::id keys = Runtime::Foundation::makeArray(cx, {key});
        const ObjC::id dict = ObjC::id(r.send(cpu, ud, r.sel("dictionaryWithValuesForKeys:"), {uint32_t(keys)}));
        const ObjC::id dictValue = dict ? Runtime::Foundation::dictGet(cx, dict, key) : 0;
        check(dictValue && Runtime::Foundation::utf8(cx, dictValue) == "tab-3",
              "NSUserDefaults dictionaryWithValuesForKeys: uses the defaults");
        r.send(cpu, ud, r.sel("removeObjectForKey:"), {uint32_t(key)});
        // NSObject's own valueForKey: must not recurse into itself.
        const ObjC::id plain = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, r.getClass("NSObject"), r.selAlloc)), r.selInit));
        bool plainRaised = false;
        try { r.send(cpu, plain, r.sel("valueForKey:"), {uint32_t(key)}); } catch (const std::exception&) { plainRaised = true; }
        check(plainRaised, "NSObject valueForKey: of an unknown key still raises (and does not recurse)");
    }

    // NSUserDefaults rejects non-property-list values with a guest-visible exception.
    {
        const ObjC::Class nsoCls = r.getClass("NSObject");
        const ObjC::id nso = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, nsoCls, r.selAlloc)), r.selInit));
        const ObjC::Class udCls = r.getClass("NSUserDefaults");
        const ObjC::id ud = ObjC::id(r.send(cpu, udCls, r.sel("standardUserDefaults")));
        const ObjC::id testKey = Runtime::Foundation::makeString(cx, "testKeyNonPlist");
        // A previous value must survive the rejected write.
        const ObjC::id initialValue = Runtime::Foundation::makeString(cx, "initialValue");
        r.send(cpu, ud, r.sel("setObject:forKey:"), {uint32_t(initialValue), uint32_t(testKey)});
        // No guest @catch is installed in this runner, so the raised NSException terminates the host->guest
        // call as a GuestFault whose text carries the exception name and reason (as for the NIB tests).
        bool guestException = false;
        std::string faultText;
        try { r.send(cpu, ud, r.sel("setObject:forKey:"), {uint32_t(nso), uint32_t(testKey)}); }
        catch (const Hydrogen::CPU::GuestFault& e) { guestException = true; faultText = e.what(); }
        catch (const ObjC::ObjCError&) { /* Pre-fix behavior: host exception, not a guest exception. */ }
        check(guestException, "NSUserDefaults setObject:forKey: raises a guest exception for non-plist objects");
        check(faultText.find("NSInvalidArgumentException") != std::string::npos &&
              faultText.find("Attempt to insert non-property list object") != std::string::npos &&
              faultText.find("testKeyNonPlist") != std::string::npos,
              "NSUserDefaults rejection carries the exception name, reason and key");
        const ObjC::id keptValue = ObjC::id(r.send(cpu, ud, r.sel("objectForKey:"), {uint32_t(testKey)}));
        check(Runtime::Foundation::utf8(cx, keptValue) == "initialValue",
              "NSUserDefaults keeps the previous value after a rejected write");
        {
            // objectForKey:'s result stays the same live object while the key keeps an equal
            // value, across a synchronize that re-reads the store (Tiny Wings keeps it in an
            // ivar without retaining it and saves it again on resign); a changed value gets a
            // new object. A number read with stringForKey: is cached the same way.
            const ObjC::id k2 = Runtime::Foundation::makeString(cx, "testKeyHandedOut");
            r.send(cpu, ud, r.sel("setObject:forKey:"), {uint32_t(Runtime::Foundation::makeString(cx, "Player")), uint32_t(k2)});
            const ObjC::id first = ObjC::id(r.send(cpu, ud, r.sel("objectForKey:"), {uint32_t(k2)}));
            r.send(cpu, ud, r.sel("synchronize"));
            const ObjC::id again = ObjC::id(r.send(cpu, ud, r.sel("objectForKey:"), {uint32_t(k2)}));
            r.send(cpu, ud, r.sel("setInteger:forKey:"), {7, uint32_t(k2)});
            const ObjC::id asText = ObjC::id(r.send(cpu, ud, r.sel("stringForKey:"), {uint32_t(k2)}));
            const ObjC::id asTextAgain = ObjC::id(r.send(cpu, ud, r.sel("stringForKey:"), {uint32_t(k2)}));
            check(first && first == again && Runtime::Foundation::utf8(cx, again) == "Player" &&
                      asText && asText == asTextAgain && Runtime::Foundation::utf8(cx, asText) == "7",
                  "NSUserDefaults hands out one live object per key while its value is unchanged");
        }
        const ObjC::id validKey = Runtime::Foundation::makeString(cx, "testKeyPlist");
        const ObjC::id validString = Runtime::Foundation::makeString(cx, "validStringValue");
        const ObjC::id validArray = Runtime::Foundation::makeArray(cx, {validString});
        const ObjC::id validDict = Runtime::Foundation::makeDict(cx, {{validString, validArray}});
        r.send(cpu, ud, r.sel("setObject:forKey:"), {uint32_t(validDict), uint32_t(validKey)});
        r.send(cpu, ud, r.sel("synchronize"));
        const ObjC::id validVal = ObjC::id(r.send(cpu, ud, r.sel("objectForKey:"), {uint32_t(validKey)}));
        const ObjC::id validArrBack = Runtime::Foundation::dictGet(cx, validVal, validString);
        const auto validItems = Runtime::Foundation::arrayItems(cx, validArrBack);
        const std::string validDesc = validItems.empty() ? std::string() : Runtime::Foundation::utf8(cx, validItems[0]);
        check(!validItems.empty() && validDesc == "validStringValue", "NSUserDefaults preserves nested supported plist values across round-trip");
    }

    // Multi-touch delivery. Each event carries one changed finger; the others still down are
    // UITouchPhaseStationary and are not sent to the view again. A game's held move button
    // stuck "down" when a second finger (looking around) made the first finger's
    // touchesBegan: arrive twice. Expected calls are written out by hand below.
    {
        auto& u = UIKit::ui(cx);
        const ObjC::id savedApp = u.app, savedKey = u.keyWindow;
        if (!u.app) u.app = r.createInstance(r.getClass("UIApplication"));
        std::vector<std::string> calls;
        std::vector<ObjC::id> seen;   // touch objects in order of first appearance: A, B, ...
        int stationaryOthers = -1, touchesInEndEvent = -1;
        auto letters = [&](Ctx& rx, ObjC::id set) {
            std::string out;
            for (ObjC::id t : Runtime::Foundation::setItems(rx, set)) {
                auto it = std::find(seen.begin(), seen.end(), t);
                if (it == seen.end()) { seen.push_back(t); it = seen.end() - 1; }
                out += char('A' + (it - seen.begin()));
            }
            std::sort(out.begin(), out.end());
            return out;
        };
        auto logCls = r.defineClass("HVMTouchLogView", "UIView");
        logCls.method("touchesBegan:withEvent:", [&](Ctx& rx) { calls.push_back("began:" + letters(rx, rx.cpu.r[2])); }, "v@:@@");
        logCls.method("touchesMoved:withEvent:", [&](Ctx& rx) { calls.push_back("moved:" + letters(rx, rx.cpu.r[2])); }, "v@:@@");
        logCls.method("touchesCancelled:withEvent:", [&](Ctx& rx) { calls.push_back("cancelled:" + letters(rx, rx.cpu.r[2])); }, "v@:@@");
        logCls.method("touchesEnded:withEvent:", [&](Ctx& rx) {
            const ObjC::id touches = rx.cpu.r[2], event = rx.cpu.r[3];
            calls.push_back("ended:" + letters(rx, touches));
            if (stationaryOthers >= 0) return;   // only the first lift is inspected
            // The event still lists every finger; the ones that did not change are stationary (2).
            const auto ended = Runtime::Foundation::setItems(rx, touches);
            const auto all = Runtime::Foundation::setItems(rx, ObjC::id(r.send(rx.cpu, event, "allTouches")));
            touchesInEndEvent = int(all.size());
            stationaryOthers = 0;
            for (ObjC::id t : all)
                if (std::find(ended.begin(), ended.end(), t) == ended.end() && int32_t(r.send(rx.cpu, t, "phase")) == 2) ++stationaryOthers;
        }, "v@:@@");
        const ObjC::id win = UIKit::sendRect(cx, ObjC::id(r.send(cpu, r.getClass("UIWindow"), r.selAlloc)), r.sel("initWithFrame:"), {0, 0, 320, 480});
        r.send(cpu, win, "makeKeyAndVisible");
        const ObjC::id pad = UIKit::sendRect(cx, ObjC::id(r.send(cpu, logCls.cls(), r.selAlloc)), r.sel("initWithFrame:"), {0, 0, 320, 480});
        r.send(cpu, pad, "setMultipleTouchEnabled:", {1});
        r.send(cpu, win, "addSubview:", {pad});
        using TP = Runtime::TouchPhase;
        auto touch = [&](TP phase, float tx, float ty, uint32_t finger) {
            proc.injectTouch(phase, tx, ty, finger);
            UIKit::deliverPendingInput(cx);
        };
        touch(TP::Began, 50, 400, 1);    // move button
        touch(TP::Began, 250, 100, 2);   // look finger goes down while the first is held
        touch(TP::Moved, 240, 110, 2);
        touch(TP::Moved, 60, 400, 1);
        touch(TP::Ended, 60, 400, 1);    // move button released; the look finger stays
        touch(TP::Moved, 230, 120, 2);
        touch(TP::Ended, 230, 120, 2);
        const std::vector<std::string> want = {"began:A", "began:B", "moved:B", "moved:A", "ended:A", "moved:B", "ended:B"};
        if (calls != want) { std::printf("    touch calls:"); for (const auto& c : calls) std::printf(" %s", c.c_str()); std::printf("\n"); }
        check(calls == want && touchesInEndEvent == 2 && stationaryOthers == 1,
              "each touch event delivers only the finger that changed; fingers held still are stationary, not begun or moved again");
        // A view can enable multi-touch by overriding the getter instead of setting the property
        // (Minecraft's EAGLView): a second finger on it is delivered. Without either, the second
        // finger on the same view is not.
        {
            auto overrideCls = r.defineClass("HVMTouchGetterView", "HVMTouchLogView");
            overrideCls.method("isMultipleTouchEnabled", [](Ctx& rx) { rx.cpu.r[0] = 1; }, "c@:");
            for (const bool byGetter : {true, false}) {
                r.send(cpu, pad, "setHidden:", {1});
                const ObjC::id v = UIKit::sendRect(cx, ObjC::id(r.send(cpu, byGetter ? overrideCls.cls() : logCls.cls(), r.selAlloc)), r.sel("initWithFrame:"), {0, 0, 320, 480});
                r.send(cpu, win, "addSubview:", {v});
                calls.clear();
                seen.clear();
                touch(TP::Began, 50, 400, 11);
                touch(TP::Began, 250, 100, 12);
                touch(TP::Moved, 240, 110, 12);
                touch(TP::Ended, 240, 110, 12);
                touch(TP::Ended, 50, 400, 11);
                const std::vector<std::string> both = {"began:A", "began:B", "moved:B", "ended:B", "ended:A"}, one = {"began:A", "ended:A"};
                if (calls != (byGetter ? both : one)) { std::printf("    getter %d touch calls:", byGetter); for (const auto& c : calls) std::printf(" %s", c.c_str()); std::printf("\n"); }
                check(calls == (byGetter ? both : one),
                      byGetter ? "a view that overrides -isMultipleTouchEnabled receives a second finger"
                               : "a view without multi-touch receives only its first finger");
                r.send(cpu, v, "removeFromSuperview");
                r.send(cpu, pad, "setHidden:", {0});
            }
        }
        // A finger that lifts while the app ignores interaction events is forgotten, so the
        // same finger works again afterwards (it used to stay "down" forever).
        calls.clear();
        touch(TP::Began, 100, 100, 5);
        r.send(cpu, u.app, "beginIgnoringInteractionEvents");
        touch(TP::Ended, 100, 100, 5);
        const size_t whileIgnoring = calls.size();   // 1: the began; the lift is dropped
        r.send(cpu, u.app, "endIgnoringInteractionEvents");
        touch(TP::Began, 100, 100, 5);
        touch(TP::Ended, 100, 100, 5);
        size_t begans = 0, endeds = 0;
        for (const auto& c : calls) { begans += c.rfind("began:", 0) == 0; endeds += c.rfind("ended:", 0) == 0; }
        if (!(whileIgnoring == 1 && begans == 2 && endeds == 1)) { std::printf("    touch calls:"); for (const auto& c : calls) std::printf(" %s", c.c_str()); std::printf("\n"); }
        check(whileIgnoring == 1 && begans == 2 && endeds == 1, "a finger lifted during beginIgnoringInteractionEvents can touch again afterwards");
        // An input-file line goes through the same applyTouch the hvm loop calls and reaches UIKit
        // with its finger id: "touch 0 100 100 7" makes host finger 7 active (not the default 1),
        // and "touch 3 100 100 7" ends that same finger.
        calls.clear();
        auto activeIds = [&] { std::vector<uint32_t> ids; for (const auto& at : u.activeTouches) ids.push_back(at.hostId); return ids; };
        const bool downOk = DesktopInput::applyTouch(proc, "touch 0 100 100 7", true);
        UIKit::deliverPendingInput(cx);
        const auto whileDown = activeIds();
        const bool upOk = DesktopInput::applyTouch(proc, "touch 3 100 100 7", true);
        UIKit::deliverPendingInput(cx);
        const auto afterUp = activeIds();
        const bool badRejected = !DesktopInput::applyTouch(proc, "touch 0 100 100 two", true);
        UIKit::deliverPendingInput(cx);
        if (whileDown != std::vector<uint32_t>{7}) { std::printf("    active host ids:"); for (auto i : whileDown) std::printf(" %u", i); std::printf("\n"); }
        check(downOk && upOk && badRejected && whileDown == std::vector<uint32_t>{7} && afterUp.empty() && calls.size() == 2 &&
              calls[0].rfind("began:", 0) == 0 && calls[1].rfind("ended:", 0) == 0,
              "an input-file touch line reaches UIKit with its own finger id");
        r.send(cpu, win, "setHidden:", {1});
        u.app = savedApp;
        if (savedKey) u.keyWindow = savedKey;
    }

    // `hvm run --input-file` touch lines: the optional finger field reaches injectTouch, and a
    // malformed one rejects the line instead of silently meaning finger 1. Expected values are
    // read off each literal line by hand.
    {
        using DesktopInput::parseTouch;
        const auto two = parseTouch("touch 0 100 200 2");
        const auto dflt = parseTouch("touch 3 12.5 40");
        const auto crlf = parseTouch("touch 1 7 8 10\r");
        const bool fingerKept = two && two->phase == 0 && two->x == 100.0f && two->y == 200.0f && two->finger == 2;
        const bool defaultOne = dflt && dflt->phase == 3 && dflt->x == 12.5f && dflt->y == 40.0f && dflt->finger == 1;
        const bool maxFinger = crlf && crlf->phase == 1 && crlf->finger == 10;
        int wronglyAccepted = 0;
        for (const char* bad : {"touch 3 100 200 two", "touch 3 100 200 2x", "touch 3 100 200 11", "touch 3 100 200 -1",
                                "touch 3 100 200 +2", "touch 3 100 200 2 3", "touch 2 100 200", "touch 0 nan 200",
                                "touch 0 100 200y", "touch 0 100", "touchy 0 100 200"})
            if (parseTouch(bad)) { std::printf("    accepted: %s\n", bad); ++wronglyAccepted; }
        // A NUL inside a field is part of the token, not its end: finger "1\0x" and y "200\0x" are invalid.
        using namespace std::string_view_literals;
        for (std::string_view bad : {"touch 3 100 200 1\0x"sv, "touch 3 100 200\0x 2"sv})
            if (parseTouch(bad)) { std::printf("    accepted a field with an embedded NUL\n"); ++wronglyAccepted; }
        check(fingerKept && defaultOne && maxFinger && wronglyAccepted == 0,
              "input-file touch lines keep their finger id (default 1) and reject a malformed finger field");
    }

    // NSGetSizeAndAlignment (armv7 iOS layout, worked out by hand): the size and alignment of the
    // first encoded type, and a pointer to the next one. Dungeon Raid's PHInvocation sizes its
    // argument buffers with it.
    {
        const uint32_t out = proc.heap().allocZeroed(8);
        struct Case { const char* type; uint32_t size, align, consumed; };
        const Case cases[] = {
            {"i", 4, 4, 1}, {"c", 1, 1, 1}, {"s", 2, 2, 1}, {"q", 8, 4, 1}, {"d", 8, 4, 1}, {"@", 4, 4, 1}, {":", 4, 4, 1},
            {"@8:12", 4, 4, 2},                                // an argument with its frame offset
            {"^{CGRect={CGPoint=ff}{CGSize=ff}}i", 4, 4, 33},  // a pointer skips its pointee; the i is the next type
            {"{CGRect={CGPoint=ff}{CGSize=ff}}", 16, 4, 32},
            {"{pair=cq}", 12, 4, 9},                           // c at 0, q at 4 (8-byte scalars align to 4)
            {"{mix=csc}", 6, 2, 9},                            // c at 0, s at 2, c at 4, padded to 6
            {"[3s]", 6, 2, 4},
            {"(u=cd)", 8, 4, 6},
            {"r*", 4, 4, 2},                                   // const char*
            {"@\"NSString\"i", 4, 4, 11},
        };
        bool ok = true;
        for (const Case& c : cases) {
            const uint32_t text = proc.heap().allocZeroed(uint32_t(std::strlen(c.type)) + 1);
            std::memcpy(proc.mem().host(text), c.type, std::strlen(c.type) + 1);
            proc.mem().write<uint32_t>(out, 0xDEAD);
            proc.mem().write<uint32_t>(out + 4, 0xDEAD);
            const uint32_t next = uint32_t(proc.callGuest(cpu, *proc.lookupSymbol("_NSGetSizeAndAlignment"), {text, out, out + 4}));
            const uint32_t size = proc.mem().read<uint32_t>(out), align = proc.mem().read<uint32_t>(out + 4);
            if (size != c.size || align != c.align || next != text + c.consumed) {
                std::printf("    NSGetSizeAndAlignment(\"%s\"): size %u align %u consumed %u\n", c.type, size, align, next - text);
                ok = false;
            }
        }
        check(ok, "NSGetSizeAndAlignment sizes scalars, structs, arrays, unions and pointers and returns the next type");
    }

    // libc++ iostreams, the part that lives in the dylib (Minecraft PE 0.10 builds its
    // stringstreams itself and imports this). A stream is made by hand over the base
    // streambuf with a 64-byte put area; expected text worked out by hand.
    {
        auto sym = [&](const char* n) { return *proc.lookupSymbol(n); };
        auto call = [&](const char* n, std::initializer_list<uint32_t> a) { return uint32_t(proc.callGuest(cpu, sym(n), a)); };
        auto& m = proc.mem();
        const char* OS = "__ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEE";
        auto os = [&](const char* suffix) { return std::string(OS) + suffix; };
        // vtable: vbase offset 4 at vptr-12, offset to top 0
        const uint32_t vt = proc.heap().allocZeroed(16);
        m.write<uint32_t>(vt, 4);
        auto makeStream = [&](uint32_t sb) {
            const uint32_t st = proc.heap().allocZeroed(4 + 80);
            m.write<uint32_t>(st, vt + 12);
            call("__ZNSt3__18ios_base4initEPv", {st + 4, sb});
            return st;
        };
        const uint32_t sb = proc.heap().allocZeroed(32), area = proc.heap().allocZeroed(64);
        call("__ZNSt3__115basic_streambufIcNS_11char_traitsIcEEEC2Ev", {sb});
        m.write<uint32_t>(sb + 20, area); m.write<uint32_t>(sb + 24, area); m.write<uint32_t>(sb + 28, area + 64);
        const uint32_t st = makeStream(sb), ios = st + 4;

        // basic_ios::init (in the app) widens ' ' through the locale's ctype facet for the fill
        const uint32_t loc = proc.heap().allocZeroed(4);
        call("__ZNKSt3__18ios_base6getlocEv", {loc, ios});
        const uint32_t ctypeId = sym("__ZNSt3__15ctypeIcE2idE"), codecvtId = sym("__ZNSt3__17codecvtIcc11__mbstate_tE2idE");
        const uint32_t ct = call("__ZNKSt3__16locale9use_facetERNS0_2idE", {loc, ctypeId});
        const uint32_t cv = call("__ZNKSt3__16locale9use_facetERNS0_2idE", {loc, codecvtId});
        auto virt = [&](uint32_t obj, uint32_t slot, std::initializer_list<uint32_t> a) {
            std::vector<uint32_t> args{obj}; args.insert(args.end(), a);
            return uint32_t(proc.callGuest(cpu, m.read<uint32_t>(m.read<uint32_t>(obj) + slot * 4), args));
        };
        const uint32_t unknownId = proc.heap().allocZeroed(8);
        const bool facets = ct && cv && virt(ct, 7, {' '}) == ' ' && virt(ct, 3, {'q'}) == 'Q' && virt(ct, 5, {'Q'}) == 'q' &&
            virt(ct, 9, {0xE9, '?'}) == '?' && virt(cv, 7, {}) == 1 && virt(cv, 6, {}) == 1 &&
            (m.read<uint32_t>(m.read<uint32_t>(ct + 8) + ' ' * 4) & 0x4000) != 0 &&      // the mask table: space is _CTYPE_S
            (m.read<uint32_t>(m.read<uint32_t>(ct + 8) + 'a' * 4) & 0x4000) == 0 &&
            (call("__ZNKSt3__16locale9has_facetERNS0_2idE", {loc, ctypeId}) & 0xFF) == 1 &&
            (call("__ZNKSt3__16locale9has_facetERNS0_2idE", {loc, unknownId}) & 0xFF) == 0;
        check(facets, "libc++ locale: ctype<char> and codecvt<char,char> facets widen, map case, narrow and report no conversion");
        m.write<uint32_t>(ios + 76, virt(ct, 7, {' '}));

        call(os("lsEi").c_str(), {st, uint32_t(-42)});
        m.write<uint32_t>(ios + 12, 6);                                       // width 6, for one insertion
        call(os("lsEi").c_str(), {st, 7});
        const bool widthReset = m.read<uint32_t>(ios + 12) == 0;
        const uint32_t flags = m.read<uint32_t>(ios + 4);
        m.write<uint32_t>(ios + 4, (flags & ~2u) | 8u | 512u);                // hex | showbase
        call(os("lsEm").c_str(), {st, 255});
        m.write<uint32_t>(ios + 4, flags);
        call(os("lsEb").c_str(), {st, 1});
        m.write<uint32_t>(ios + 4, flags | 1u);                               // boolalpha
        call(os("lsEb").c_str(), {st, 1});
        m.write<uint32_t>(ios + 4, flags);
        const double d = 1.5; uint64_t db; std::memcpy(&db, &d, 8);
        call(os("lsEd").c_str(), {st, uint32_t(db), uint32_t(db >> 32)});
        const float f = 0.25f; uint32_t fb; std::memcpy(&fb, &f, 4);
        call(os("lsEf").c_str(), {st, fb});
        const int64_t big = -5000000000LL;
        call(os("lsEx").c_str(), {st, uint32_t(uint64_t(big)), uint32_t(uint64_t(big) >> 32)});
        call(os("lsEPKv").c_str(), {st, 0x1234});
        const uint32_t abc = proc.heap().allocZeroed(4);
        std::memcpy(m.host(abc), "abc", 3);
        call(os("5writeEPKci").c_str(), {st, abc, 2});
        call(os("lsEs").c_str(), {st, uint32_t(-3)});
        const std::string want = "-42     70xff1true1.50.25-50000000000x1234ab-3";
        const uint32_t wrote = m.read<uint32_t>(sb + 24) - area;
        const std::string got(reinterpret_cast<const char*>(m.host(area)), wrote);
        if (got != want) std::printf("    libc++ ostream wrote \"%s\"\n", got.c_str());
        check(got == want && widthReset && m.read<uint32_t>(ios + 16) == 0,
              "libc++ ostream: operator<< for int, unsigned long, bool, double, float, long long, pointer and short, and write()");

        // fixed notation with precision 600: "1." and 600 zeros, 602 characters, none lost
        {
            const uint32_t bigSb = proc.heap().allocZeroed(32), bigArea = proc.heap().allocZeroed(700);
            call("__ZNSt3__115basic_streambufIcNS_11char_traitsIcEEEC2Ev", {bigSb});
            m.write<uint32_t>(bigSb + 20, bigArea); m.write<uint32_t>(bigSb + 24, bigArea); m.write<uint32_t>(bigSb + 28, bigArea + 700);
            const uint32_t bigSt = makeStream(bigSb);
            m.write<uint32_t>(bigSt + 4 + 76, ' ');
            m.write<uint32_t>(bigSt + 4 + 4, m.read<uint32_t>(bigSt + 4 + 4) | 4u);   // fixed
            m.write<uint32_t>(bigSt + 4 + 8, 600);                                     // precision
            const double one = 1.0; uint64_t ob; std::memcpy(&ob, &one, 8);
            call(os("lsEd").c_str(), {bigSt, uint32_t(ob), uint32_t(ob >> 32)});
            const uint32_t len = m.read<uint32_t>(bigSb + 24) - bigArea;
            const std::string longText(reinterpret_cast<const char*>(m.host(bigArea)), len);
            check(longText == "1." + std::string(600, '0'), "libc++ ostream: a 602-character fixed-notation double is written whole");
            // the base streambuf's seekpos answers -1, which libc++ keeps after the 128-byte state
            const uint32_t posOut = proc.heap().allocZeroed(136), posIn = proc.heap().allocZeroed(136);
            cpu.r[13] -= 256;
            call("__ZNSt3__115basic_streambufIcNS_11char_traitsIcEEE7seekposENS_4fposI11__mbstate_tEEj", {posOut, bigSb, posIn, 8});
            cpu.r[13] += 256;
            check(int64_t(m.read<uint64_t>(posOut + 128)) == -1 && m.read<uint32_t>(posOut) == 0,
                  "libc++ streambuf::seekpos on the base class returns position -1 in libc++'s fpos layout");
        }

        // a full buffer: the base overflow refuses, so the stream goes bad|fail and stops writing
        m.write<uint32_t>(sb + 28, m.read<uint32_t>(sb + 24));
        call(os("lsEi").c_str(), {st, 1});
        const uint32_t stateFull = m.read<uint32_t>(ios + 16);
        const uint32_t sentry = proc.heap().allocZeroed(8);
        call(os("6sentryC1ERS3_").c_str(), {sentry, st});
        const bool sentryBad = m.read<uint8_t>(sentry) == 0 && m.read<uint32_t>(sentry + 4) == st;
        call("__ZNSt3__18ios_base5clearEj", {ios, 0});
        call(os("6sentryC1ERS3_").c_str(), {sentry, st});
        check(stateFull == 5 && sentryBad && m.read<uint8_t>(sentry) == 1,
              "libc++ ostream: a refused write sets badbit|failbit; the sentry is false until clear()");

        // istream::sentry skips whitespace through the get area; only whitespace is failbit|eofbit
        auto input = [&](const char* text) {
            const uint32_t isb = proc.heap().allocZeroed(32), buf = proc.heap().allocZeroed(16);
            call("__ZNSt3__115basic_streambufIcNS_11char_traitsIcEEEC2Ev", {isb});
            std::memcpy(m.host(buf), text, std::strlen(text));
            m.write<uint32_t>(isb + 8, buf); m.write<uint32_t>(isb + 12, buf); m.write<uint32_t>(isb + 16, buf + uint32_t(std::strlen(text)));
            return std::pair<uint32_t, uint32_t>(isb, buf);
        };
        const char* IS = "__ZNSt3__113basic_istreamIcNS_11char_traitsIcEEE6sentryC1ERS3_b";
        const auto [isb1, buf1] = input("  \tx y");
        const uint32_t is1 = makeStream(isb1);
        call(IS, {sentry, is1, 0});
        const bool skipOk = m.read<uint8_t>(sentry) == 1 && m.read<uint32_t>(isb1 + 12) == buf1 + 3 && m.read<uint32_t>(is1 + 4 + 16) == 0;
        const auto [isb2, buf2] = input("  \tx");
        const uint32_t is2 = makeStream(isb2);
        call(IS, {sentry, is2, 1});                                           // noskipws: nothing consumed
        const bool noskipOk = m.read<uint8_t>(sentry) == 1 && m.read<uint32_t>(isb2 + 12) == buf2;
        const auto [isb3, buf3] = input("  ");
        const uint32_t is3 = makeStream(isb3);
        call(IS, {sentry, is3, 0});
        const bool eofOk = m.read<uint8_t>(sentry) == 0 && m.read<uint32_t>(is3 + 4 + 16) == 6 && m.read<uint32_t>(isb3 + 12) == buf3 + 2;
        if (!(skipOk && noskipOk && eofOk)) std::printf("    istream sentry: skip %d noskip %d eof %d\n", skipOk, noskipOk, eofOk);
        check(skipOk && noskipOk && eofOk, "libc++ istream::sentry skips leading whitespace, honours noskipws, and fails at end of input");

        // unformatted and formatted input (Terraria reads its data files with these). The stream
        // layout here is vptr, gcount, then basic_ios at +8.
        {
            const uint32_t vt8 = proc.heap().allocZeroed(16);
            m.write<uint32_t>(vt8, 8);
            auto makeIn = [&](const char* text) {
                const auto [isb, buf] = input(text);
                const uint32_t st2 = proc.heap().allocZeroed(8 + 80);
                m.write<uint32_t>(st2, vt8 + 12);
                call("__ZNSt3__18ios_base4initEPv", {st2 + 8, isb});
                return std::pair<uint32_t, uint32_t>(st2, isb);
            };
            const std::string I = "__ZNSt3__113basic_istreamIcNS_11char_traitsIcEEE";
            const auto [in1, sb1] = makeIn("ab\ncd  -42x");
            const uint32_t line = proc.heap().allocZeroed(16), num = proc.heap().allocZeroed(4);
            call((I + "7getlineEPcic").c_str(), {in1, line, 16, '\n'});
            const bool lineOk = proc.readCString(line) == "ab" && m.read<uint32_t>(in1 + 4) == 3 && m.read<uint32_t>(in1 + 8 + 16) == 0;
            call((I + "4readEPci").c_str(), {in1, line, 2});
            const bool readOk = std::memcmp(m.host(line), "cd", 2) == 0 && m.read<uint32_t>(in1 + 4) == 2;
            call((I + "rsERi").c_str(), {in1, num});
            const bool intOk = int32_t(m.read<uint32_t>(num)) == -42 && m.read<uint32_t>(in1 + 8 + 16) == 0 &&
                               (call((I + "4peekEv").c_str(), {in1}) & 0xFF) == 'x';
            // 3 bytes asked, 1 left: the count is 1 and the stream is failbit|eofbit
            call((I + "4readEPci").c_str(), {in1, line, 3});
            const bool shortOk = m.read<uint32_t>(in1 + 4) == 1 && m.read<uint32_t>(in1 + 8 + 16) == 6;
            // the base streambuf cannot seek: tellg is -1 (the offset follows the 128-byte state), seekg sets failbit
            const auto [in2, sb2] = makeIn("xyz");
            const uint32_t pos = proc.heap().allocZeroed(136);
            call((I + "5tellgEv").c_str(), {pos, in2});
            const bool tellOk = int64_t(m.read<uint64_t>(pos + 128)) == -1 && m.read<uint32_t>(in2 + 8 + 16) == 0;
            cpu.r[13] -= 64;
            call((I + "5seekgExNS_8ios_base7seekdirE").c_str(), {in2, 0, 0, 2});
            cpu.r[13] += 64;
            const bool seekOk = m.read<uint32_t>(in2 + 8 + 16) == 4;
            const auto [in3, sb3] = makeIn("zz");
            call((I + "rsERi").c_str(), {in3, num});
            const bool notNumber = m.read<uint32_t>(num) == 0 && m.read<uint32_t>(in3 + 8 + 16) == 4;
            (void)sb1; (void)sb2; (void)sb3;
            if (!(lineOk && readOk && intOk && shortOk && tellOk && seekOk && notNumber))
                std::printf("    libc++ istream: line %d read %d int %d short %d tell %d seek %d nan %d\n", lineOk, readOk, intOk, shortOk, tellOk, seekOk, notNumber);
            check(lineOk && readOk && intOk && shortOk && tellOk && seekOk && notNumber,
                  "libc++ istream: getline, read, operator>>(int), peek, a short read, tellg and seekg on a buffer that cannot seek");
        }

        // A std::iostream built the way an app's inline constructor does it, from the dylib's
        // vtable: istream vptr at +0, ostream vptr at +8, basic_ios at +12 (Terraria).
        {
            const uint32_t ztv = sym("__ZTVNSt3__114basic_iostreamIcNS_11char_traitsIcEEEE");
            const uint32_t ztt = sym("__ZTTNSt3__114basic_iostreamIcNS_11char_traitsIcEEEE");
            const bool offsets = int32_t(m.read<uint32_t>(ztv)) == 12 && m.read<uint32_t>(ztv + 4) == 0 &&       // istream part
                                 int32_t(m.read<uint32_t>(ztv + 20)) == 4 && int32_t(m.read<uint32_t>(ztv + 24)) == -8 &&   // ostream part at +8
                                 int32_t(m.read<uint32_t>(ztv + 40)) == -12 && int32_t(m.read<uint32_t>(ztv + 44)) == -12 && // basic_ios at +12
                                 m.read<uint32_t>(ztt) == ztv + 12 && m.read<uint32_t>(ztt + 20) == ztv + 52 && m.read<uint32_t>(ztt + 24) == ztv + 32 &&
                                 int32_t(m.read<uint32_t>(m.read<uint32_t>(ztt + 4) - 12)) == 12 &&    // istream while constructing: basic_ios 12 on
                                 int32_t(m.read<uint32_t>(m.read<uint32_t>(ztt + 12) - 12)) == 4;      // ostream while constructing: basic_ios 4 on
            const uint32_t obj = proc.heap().allocZeroed(12 + 80);
            m.write<uint32_t>(obj, ztv + 12);
            m.write<uint32_t>(obj + 8, ztv + 32);
            m.write<uint32_t>(obj + 12, ztv + 52);
            const uint32_t iosb = proc.heap().allocZeroed(32), ioArea = proc.heap().allocZeroed(16);
            call("__ZNSt3__115basic_streambufIcNS_11char_traitsIcEEEC2Ev", {iosb});
            m.write<uint32_t>(iosb + 20, ioArea); m.write<uint32_t>(iosb + 24, ioArea); m.write<uint32_t>(iosb + 28, ioArea + 16);
            call("__ZNSt3__18ios_base4initEPv", {obj + 12, iosb});
            const uint32_t text = proc.heap().allocZeroed(4);
            std::memcpy(m.host(text), "hey", 3);
            call(os("5writeEPKci").c_str(), {obj + 8, text, 3});                    // through the ostream half
            const bool wrote = std::memcmp(m.host(ioArea), "hey", 3) == 0 && m.read<uint32_t>(obj + 12 + 16) == 0;
            // `delete` through the basic_ios subobject frees the whole stream
            proc.callGuest(cpu, m.read<uint32_t>(m.read<uint32_t>(obj + 12) + 4), {obj + 12});
            check(offsets && wrote && proc.heap().blockSize(obj) == 0,
                  "libc++ vtable and VTT for std::iostream: base offsets, a write through the ostream half, delete through basic_ios");
        }

        // Wide streams (Terraria formats "Gender (1/9)" through a wostringstream; with no
        // ctype<wchar_t> facet it crashed when a character was created). wchar_t is 4 bytes.
        {
            const uint32_t wid = sym("__ZNSt3__15ctypeIwE2idE");
            const uint32_t wct = call("__ZNKSt3__16locale9use_facetERNS0_2idE", {loc, wid});
            const bool wfacet = wct && virt(wct, 11, {' '}) == ' ' && virt(wct, 7, {'q'}) == 'Q' && virt(wct, 9, {'Q'}) == 'q' &&
                                virt(wct, 13, {0x263A, '?'}) == '?' && virt(wct, 13, {'z', '?'}) == 'z' &&
                                (virt(wct, 3, {0x4000, ' '}) & 0xFF) == 1 && (virt(wct, 3, {0x4000, 'a'}) & 0xFF) == 0 &&
                                (call("__ZNKSt3__16locale9has_facetERNS0_2idE", {loc, wid}) & 0xFF) == 1;
            const char* WSB = "__ZNSt3__115basic_streambufIwNS_11char_traitsIwEEE";
            const uint32_t wsb = proc.heap().allocZeroed(32), warea = proc.heap().allocZeroed(64);
            call((std::string(WSB) + "C2Ev").c_str(), {wsb});
            m.write<uint32_t>(wsb + 20, warea); m.write<uint32_t>(wsb + 24, warea); m.write<uint32_t>(wsb + 28, warea + 64);
            const uint32_t wst = makeStream(wsb);
            m.write<uint32_t>(wst + 4 + 76, 0xFFFFFFFFu);                         // fill not yet widened: a space
            const char* WOS = "__ZNSt3__113basic_ostreamIwNS_11char_traitsIwEEElsEi";
            call(WOS, {wst, uint32_t(-42)});
            m.write<uint32_t>(wst + 4 + 12, 4);                                   // width 4
            m.write<uint32_t>(wst + 4 + 76, '*');
            call(WOS, {wst, 7});
            const uint32_t wantWide[7] = {'-', '4', '2', '*', '*', '*', '7'};
            const bool wrote = m.read<uint32_t>(wsb + 24) == warea + 28 && std::memcmp(m.host(warea), wantWide, 28) == 0 &&
                               m.read<uint32_t>(wst + 4 + 12) == 0 && m.read<uint32_t>(wst + 4 + 16) == 0;
            // the put area is full after 9 more characters: 16 fit, the rest is refused and the stream goes bad|fail
            call(WOS, {wst, 123456789});
            call(WOS, {wst, 5});
            const bool full = m.read<uint32_t>(wsb + 24) == warea + 64 && m.read<uint32_t>(warea + 60) == '9' && m.read<uint32_t>(wst + 4 + 16) == 5;
            // reading: xsgetn copies whole characters out of the get area and stops at its end
            const uint32_t rsb = proc.heap().allocZeroed(32), rarea = proc.heap().allocZeroed(16), rout = proc.heap().allocZeroed(32);
            call((std::string(WSB) + "C2Ev").c_str(), {rsb});
            const uint32_t src4[3] = {'a', 0x263A, 'c'};
            std::memcpy(m.host(rarea), src4, 12);
            m.write<uint32_t>(rsb + 8, rarea); m.write<uint32_t>(rsb + 12, rarea); m.write<uint32_t>(rsb + 16, rarea + 12);
            // (the base class's uflow asks underflow, which has nothing to offer: end of file. A get
            // area is read directly, by sbumpc inlined in the app or by xsgetn here.)
            const uint32_t baseUflow = call((std::string(WSB) + "5uflowEv").c_str(), {rsb});
            const uint32_t took = call((std::string(WSB) + "6xsgetnEPwi").c_str(), {rsb, rout, 5});
            const bool readBack = baseUflow == 0xFFFFFFFFu && took == 3 && m.read<uint32_t>(rout) == 'a' && m.read<uint32_t>(rout + 4) == 0x263A &&
                                  m.read<uint32_t>(rout + 8) == 'c' && m.read<uint32_t>(rsb + 12) == rarea + 12;
            if (!(wfacet && wrote && full && readBack)) std::printf("    wide streams: facet %d wrote %d full %d read %d\n", wfacet, wrote, full, readBack);
            check(wfacet && wrote && full && readBack,
                  "libc++ wide streams: ctype<wchar_t>, wostream << int with width and fill, a full buffer, and wstreambuf reads");
        }

        // std::__sort for unsigned long long
        const uint32_t arr = proc.heap().allocZeroed(24);
        const uint64_t vals[3] = {3, 1ull << 40, 2};
        std::memcpy(m.host(arr), vals, 24);
        call("__ZNSt3__16__sortIRNS_6__lessIyyEEPyEEvT0_S5_T_", {arr, arr + 24, 0});
        check(m.read<uint64_t>(arr) == 2 && m.read<uint64_t>(arr + 8) == 3 && m.read<uint64_t>(arr + 16) == (1ull << 40),
              "libc++ __sort orders unsigned long long values");
    }

    // task_info(TASK_BASIC_INFO_32) writes its 8 words and no more, whatever count the caller
    // claims. Terraria passes 32 (the size in bytes) for a 32-byte buffer on its stack; ten words
    // were written and its return address was overwritten.
    {
        const uint32_t buf = proc.heap().allocZeroed(64), count = proc.heap().allocZeroed(4);
        for (uint32_t i = 0; i < 64; i += 4) proc.mem().write<uint32_t>(buf + i, 0xCAFEF00D);
        proc.mem().write<uint32_t>(count, 32);
        const uint32_t kr = uint32_t(proc.callGuest(cpu, *proc.lookupSymbol("_task_info"), {0x103, 4, buf, count}));
        const uint32_t virt = proc.mem().read<uint32_t>(buf + 4), resident = proc.mem().read<uint32_t>(buf + 8);
        const bool basic = kr == 0 && proc.mem().read<uint32_t>(count) == 8 && proc.mem().read<uint32_t>(buf) == 0 &&
                           resident > 0 && virt >= resident && proc.mem().read<uint32_t>(buf + 32) == 0xCAFEF00D;
        proc.mem().write<uint32_t>(count, 4);                      // too small for the structure
        proc.mem().write<uint32_t>(buf, 0xCAFEF00D);
        const uint32_t small = uint32_t(proc.callGuest(cpu, *proc.lookupSymbol("_task_info"), {0x103, 4, buf, count}));
        const bool smallUntouched = proc.mem().read<uint32_t>(buf) == 0xCAFEF00D;
        // flavor 5: the 64-bit sizes when there is room for ten words, the 32-bit structure for eight
        for (uint32_t i = 0; i < 64; i += 4) proc.mem().write<uint32_t>(buf + i, 0xCAFEF00D);
        proc.mem().write<uint32_t>(count, 10);
        const uint32_t kr64 = uint32_t(proc.callGuest(cpu, *proc.lookupSymbol("_task_info"), {0x103, 5, buf, count}));
        const bool wide = kr64 == 0 && proc.mem().read<uint32_t>(count) == 10 && proc.mem().read<uint64_t>(buf + 12) > 0 &&
                          proc.mem().read<uint64_t>(buf + 4) >= proc.mem().read<uint64_t>(buf + 12) &&
                          proc.mem().read<uint64_t>(buf + 4) < (1ull << 32) && proc.mem().read<uint32_t>(buf + 40) == 0xCAFEF00D;
        for (uint32_t i = 0; i < 64; i += 4) proc.mem().write<uint32_t>(buf + i, 0xCAFEF00D);
        proc.mem().write<uint32_t>(count, 8);
        const uint32_t kr8 = uint32_t(proc.callGuest(cpu, *proc.lookupSymbol("_task_info"), {0x103, 5, buf, count}));
        const bool narrow = kr8 == 0 && proc.mem().read<uint32_t>(count) == 8 && proc.mem().read<uint32_t>(buf + 8) > 0 &&
                            proc.mem().read<uint32_t>(buf + 4) >= proc.mem().read<uint32_t>(buf + 8) &&
                            proc.mem().read<uint32_t>(buf + 32) == 0xCAFEF00D;
        // a flavor with no layout: zeros, at most what the caller has room for, and reported as missing
        for (uint32_t i = 0; i < 64; i += 4) proc.mem().write<uint32_t>(buf + i, 0xCAFEF00D);
        proc.mem().write<uint32_t>(count, 3);
        const size_t missingBefore = proc.missingCount();
        const uint32_t krOdd = uint32_t(proc.callGuest(cpu, *proc.lookupSymbol("_task_info"), {0x103, 99, buf, count}));
        const bool unknown = krOdd == 0 && proc.mem().read<uint32_t>(count) == 3 && proc.mem().read<uint32_t>(buf + 8) == 0 &&
                             proc.mem().read<uint32_t>(buf + 12) == 0xCAFEF00D && proc.missingCount() == missingBefore + 1;
        check(basic && small == 4 && smallUntouched && wide && narrow && unknown,
              "task_info(TASK_BASIC_INFO_32) writes 8 words whatever the caller's count; too small a count is KERN_INVALID_ARGUMENT; flavor 5 fits the room given; an unknown flavor is reported");
    }

    // ungetc takes back more than one byte, as Darwin's does. PvZ's XML reader reads three bytes
    // looking for a byte-order mark and returns all three; with one byte of pushback it lost
    // "<?" and could not parse resources.xml, so the game never drew anything.
    {
        const std::string path = proc.homePath() + "/unget_test.xml";
        { std::ofstream(path) << "<?xml?>"; }
        auto call = [&](const char* n, std::initializer_list<uint32_t> a) { return uint32_t(proc.callGuest(cpu, *proc.lookupSymbol(n), a)); };
        auto cstr = [&](const std::string& t) { const uint32_t a = proc.heap().allocZeroed(uint32_t(t.size()) + 1); std::memcpy(proc.mem().host(a), t.data(), t.size()); return a; };
        const uint32_t fp = call("_fopen", {cstr(path), cstr("rb")});
        const uint32_t c1 = call("_fgetc", {fp}), c2 = call("_fgetc", {fp}), c3 = call("_fgetc", {fp});
        call("_ungetc", {c3, fp}); call("_ungetc", {c2, fp}); call("_ungetc", {c1, fp});
        const bool told = call("_ftell", {fp}) == 0 && call("_feof", {fp}) == 0;
        const uint32_t buf = proc.heap().allocZeroed(16);
        const uint32_t first = call("_fgetc", {fp});
        const uint32_t got = call("_fread", {buf, 1, 6, fp});
        const bool reread = first == '<' && got == 6 && std::memcmp(proc.mem().host(buf), "?xml?>", 6) == 0;
        // a seek discards what was pushed back
        call("_ungetc", {'Z', fp});
        call("_fseek", {fp, 1, 0});
        bool seekDrops = call("_fgetc", {fp}) == '?';
        // SEEK_CUR counts from the stream's position, which is behind the file's by the bytes
        // pushed back: after reading "<?" twice over and returning two bytes, +1 lands on 'x'... 
        call("_fseek", {fp, 0, 0});
        const uint32_t d1 = call("_fgetc", {fp}), d2 = call("_fgetc", {fp}), d3 = call("_fgetc", {fp});
        call("_ungetc", {d3, fp}); call("_ungetc", {d2, fp}); call("_ungetc", {d1, fp});    // the stream is at 0, the file at 3
        call("_fseek", {fp, 1, 1 /*SEEK_CUR*/});
        seekDrops = seekDrops && call("_ftell", {fp}) == 1 && call("_fgetc", {fp}) == '?';
        // a seek that fails keeps the pushed-back byte
        call("_ungetc", {'Q', fp});
        const bool failedSeekKeeps = int32_t(call("_fseek", {fp, uint32_t(-100), 0})) != 0 && call("_fgetc", {fp}) == 'Q';
        seekDrops = seekDrops && failedSeekKeeps;
        call("_fclose", {fp});
        std::filesystem::remove(path);
        check(fp && c1 == '<' && c2 == '?' && c3 == 'x' && told && reread && seekDrops,
              "ungetc returns three bytes to a stream; fgetc and fread read them again in order; fseek discards them, counts SEEK_CUR from the stream position, and keeps them when it fails");
    }

    // sbrk as Darwin emulates it: 4 MB handed out in order, then -1 (not 0: an app's dlmalloc
    // took the missing sbrk's 0 for a heap at address zero, PvZ).
    {
        auto sbrk = [&](int32_t n) { return uint32_t(proc.callGuest(cpu, *proc.lookupSymbol("_sbrk"), {uint32_t(n)})); };
        const uint32_t start = sbrk(0);
        const uint32_t a = sbrk(4096), b = sbrk(8192);
        proc.mem().write<uint32_t>(a, 0x12345678);                // the region is real memory
        const bool grows = start != 0 && start != 0xFFFFFFFFu && a == start && b == start + 4096 && sbrk(0) == start + 12288 &&
                           sbrk(-4096) == start + 12288 && proc.mem().read<uint32_t>(a) == 0x12345678;
        const bool tooBig = sbrk(4 << 20) == 0xFFFFFFFFu && sbrk(0) == start + 12288;
        const bool rest = sbrk((4 << 20) - 12288) == start + 12288 && sbrk(1) == 0xFFFFFFFFu;
        const bool brkFails = uint32_t(proc.callGuest(cpu, *proc.lookupSymbol("_brk"), {start})) == 0xFFFFFFFFu &&
                              proc.mem().read<uint32_t>(proc.currentThread().errnoAddr) == 12 /*ENOMEM*/;
        check(grows && tooBig && rest && brkFails,
              "sbrk hands out its 4 MB region in order, reports the break for 0 or less, and returns -1 when it does not fit; brk always fails");
    }

    // Thread-specific data lives in the guest slot array the thread register points at. Unity
    // stores its per-thread allocator with pthread_setspecific and reads it back inline, as
    // "thread register + key * 4"; with the values kept elsewhere it read nothing (Crossy Road).
    {
        auto call = [&](const char* n, std::initializer_list<uint32_t> a) { return uint32_t(proc.callGuest(cpu, *proc.lookupSymbol(n), a)); };
        // This test process never loaded an app, so its main thread has no pthread block yet:
        // give it one laid out as Process::initThreadState does.
        auto& thread = proc.currentThread();
        const uint32_t savedSelf = thread.self, savedErrno = thread.errnoAddr, savedTp = cpu.tpidruro;
        thread.self = proc.heap().allocZeroed(kThreadSpecificSlots * 4 + 64);
        thread.errnoAddr = thread.self + kThreadSpecificSlots * 4;
        proc.mem().write<uint32_t>(thread.self, thread.self);
        cpu.tpidruro = thread.self;
        const uint32_t keyOut = proc.heap().allocZeroed(8);
        const uint32_t created = call("_pthread_key_create", {keyOut, 0});
        const uint32_t key = proc.mem().read<uint32_t>(keyOut);
        const uint32_t base = cpu.tpidruro & ~3u;                        // what "mrc p15, 0, r0, c13, c0, 3" gives
        const bool setOk = call("_pthread_setspecific", {key, 0x12345678}) == 0;
        const bool inlineRead = proc.mem().read<uint32_t>(base + key * 4) == 0x12345678;
        proc.mem().write<uint32_t>(base + key * 4, 0x0BADF00D);          // an inlined setspecific
        const bool functionRead = call("_pthread_getspecific", {key}) == 0x0BADF00D;
        call("_pthread_setspecific", {key, 0});
        const bool selfSlot = proc.mem().read<uint32_t>(base) == call("_pthread_self", {});
        const uint32_t errnoAddr = proc.currentThread().errnoAddr;
        const bool errnoClear = errnoAddr >= base + 768 * 4;             // no key's slot is errno
        const bool outOfRange = call("_pthread_setspecific", {768, 1}) == 22 && call("_pthread_getspecific", {768}) == 0;
        thread.self = savedSelf; thread.errnoAddr = savedErrno; cpu.tpidruro = savedTp;
        check(created == 0 && key >= 256 && key < 768 && setOk && inlineRead && functionRead && selfSlot && errnoClear && outOfRange,
              "pthread_setspecific stores in the slot the thread register points at, where inlined pthread_getspecific reads; key 768 is EINVAL");
    }

    // CoreText's font trait keys bind to string constants when an app imports them. They were
    // missing data (nil), and Crossy Road stopped on "setObject:forKey: key cannot be nil".
    {
        auto keyText = [&](const char* symbol) -> std::string {
            const uint32_t cell = proc.resolveMissingSymbol(symbol, Process::MissingSymbolMode::Import);
            const uint32_t str = cell ? proc.mem().read<uint32_t>(cell) : 0;
            return str ? Foundation::utf8(cx, str) : std::string("(nil)");
        };
        check(keyText("_kCTFontSymbolicTrait") == "kCTFontSymbolicTrait" && keyText("_kCTFontWeightTrait") == "kCTFontWeightTrait" &&
              keyText("_kCTFontSlantTrait") == "kCTFontSlantTrait",
              "CoreText font trait keys imported by an app are string constants, not nil");
        // dlsym is not answered from the naming rule: an app probing for a symbol must not be told it exists
        check(proc.resolveMissingSymbol("_kCTFontWidthTrait") == 0, "the CoreText trait rule applies to imports only, not to dlsym");
    }

    // A failed stat through the guest call is part of the file history in the crash clues.
    {
        const uint32_t missingPath = proc.heap().allocZeroed(64), statOut = proc.heap().allocZeroed(128);
        std::memcpy(proc.mem().host(missingPath), "/no/such/dir/Manifest.xnb", 26);
        const int32_t statResult = int32_t(uint32_t(proc.callGuest(cpu, *proc.lookupSymbol("_stat"), {missingPath, statOut})));
        check(statResult == -1 && proc.mem().read<uint32_t>(proc.currentThread().errnoAddr) == ENOENT &&
              proc.crashClues(CPU::Exit::MemFault, 0, true).find("stat /no/such/dir/Manifest.xnb: No such file or directory") != std::string::npos,
              "a stat that fails is recorded: -1, ENOENT, and the path in the crash clues");
    }

    // An app with its own crash reporter catches the real fault and raises the signal again.
    // The stop message for that raise names the fault that started it (Terraria: PLCrashReporter
    // hid a null read inside std::string behind "the app raised signal 11").
    {
        ProcessConfig fcfg;
        fcfg.homePath = (std::filesystem::temp_directory_path() / ("hvm_handled_fault_" + std::to_string(::getpid()))).string();
        struct RemoveHome { std::string path; ~RemoveHome() { std::error_code ec; std::filesystem::remove_all(path, ec); } } removeHome{fcfg.homePath};
        Process fproc(fcfg);
        auto& fcpu = fproc.mainThread().cpu;
        std::string raised;
        const uint32_t handler = fproc.defineFunction("test_crash_reporter", [&](Ctx& x) {
            try { x.proc.callGuest(x.cpu, *x.proc.lookupSymbol("_raise"), {11}); }
            catch (const std::exception& e) { raised = e.what(); throw; }
        });
        fproc.callGuest(fcpu, *fproc.lookupSymbol("_signal"), {11, handler});
        fcpu.exit_info = 0x10;                        // the address the guest touched
        fcpu.r[13] = fproc.heap().allocZeroed(8192) + 8192;   // a stack for the signal frame
        fcpu.r[15] = handler;
        const bool resumed = hvmDeliverGuestFault(fproc, fcpu);
        if (raised.find("after its own crash handler caught a bad memory access at 0x00000010 (NULL pointer) at ") == std::string::npos)
            std::printf("    raise said: %s\n", raised.c_str());
        // A handler that returns has dealt with its fault: a later raise does not mention it.
        int recovered = 0;
        const uint32_t quiet = fproc.defineFunction("test_recovering_handler", [&](Ctx&) { ++recovered; });
        fproc.callGuest(fcpu, *fproc.lookupSymbol("_signal"), {11, quiet});
        fcpu.exit_info = 0x20;
        fcpu.r[15] = quiet;
        fcpu.r[13] = fproc.heap().allocZeroed(8192) + 8192;
        const bool resumedQuietly = hvmDeliverGuestFault(fproc, fcpu);
        std::string later;
        try { fproc.callGuest(fcpu, *fproc.lookupSymbol("_raise"), {6}); } catch (const std::exception& e) { later = e.what(); }
        if (later.find("crash handler") != std::string::npos) std::printf("    later raise said: %s\n", later.c_str());
        check(!resumed && raised.find("the app raised signal 11 after") != std::string::npos &&
              raised.find("after its own crash handler caught a bad memory access at 0x00000010 (NULL pointer) at ") != std::string::npos &&
              resumedQuietly && recovered == 1 && later.find("the app raised signal 6") != std::string::npos &&
              later.find("crash handler") == std::string::npos,
              "a signal raised by the app's crash handler reports the fault the handler caught; a fault the handler recovered from is forgotten");
    }

    // 2f. OpenGL ES 1.1 & 2.0 + CAEAGLLayer + EAGLContext + Compositor
    {
        const ObjC::Class winCls = r.getClass("UIWindow");
        const ObjC::id win = UIKit::sendRect(cx, ObjC::id(r.send(cpu, winCls, r.selAlloc)), r.sel("initWithFrame:"), {0, 0, 320, 480});
        r.send(cpu, win, "makeKeyAndVisible");

        const ObjC::Class eaglLayerCls = r.getClass("CAEAGLLayer");
        const ObjC::id glLayer = ObjC::id(r.send(cpu, eaglLayerCls, "layer"));
        UIKit::sendRect(cx, glLayer, r.sel("setFrame:"), {0, 0, 64, 64});
        r.send(cpu, UIKit::viewLayer(cx, win), "addSublayer:", {glLayer});

        const ObjC::Class ctxCls = r.getClass("EAGLContext");
        const ObjC::id glCtx = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, ctxCls, r.selAlloc)), "initWithAPI:", {2}));
        r.send(cpu, ctxCls, "setCurrentContext:", {glCtx});

        auto callGL = [&](const char* sym, std::initializer_list<uint32_t> args) -> uint64_t {
            auto addr = proc.lookupSymbol(sym);
            return proc.callGuest(cpu, *addr, args);
        };

        const uint32_t idsAddr = proc.heap().allocZeroed(64);
        callGL("_glGenFramebuffers", {1, idsAddr});
        const uint32_t fbo = proc.mem().read<uint32_t>(idsAddr);
        callGL("_glBindFramebuffer", {0x8D40 /* GL_FRAMEBUFFER */, fbo});
        const uint32_t emptyStatus = uint32_t(callGL("_glCheckFramebufferStatus", {0x8D40}));
        check(emptyStatus == 0x8CD7 /* GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT */,
              "Empty FBO reports GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT");
        callGL("_glBindFramebuffer", {0x8D40, 0});
        const uint32_t defaultStatus = uint32_t(callGL("_glCheckFramebufferStatus", {0x8D40}));
        check(defaultStatus == 0x8CD5 /* GL_FRAMEBUFFER_COMPLETE */,
              "Default FBO 0 reports GL_FRAMEBUFFER_COMPLETE");
        callGL("_glBindFramebuffer", {0x8D40, fbo});

        callGL("_glGenRenderbuffers", {1, idsAddr + 4});
        const uint32_t rbo = proc.mem().read<uint32_t>(idsAddr + 4);
        callGL("_glBindRenderbuffer", {0x8D41 /* GL_RENDERBUFFER */, rbo});
        const bool rbOk = (r.send(cpu, glCtx, "renderbufferStorage:fromDrawable:", {0x8D41, glLayer}) & 0xFF) != 0;
        callGL("_glFramebufferRenderbuffer", {0x8D40, 0x8CE0 /* GL_COLOR_ATTACHMENT0 */, 0x8D41, rbo});
        const uint32_t fbStatus = uint32_t(callGL("_glCheckFramebufferStatus", {0x8D40}));
        check(rbOk && fbStatus == 0x8CD5 /* GL_FRAMEBUFFER_COMPLETE */, "EAGLContext renderbufferStorage:fromDrawable: & FBO complete");

        {
            // Completeness follows the attached images: missing storage, mismatched sizes, detach, and the target asked about.
            const uint32_t kFb = 0x8D40, kRb = 0x8D41, kColor = 0x8CE0, kDepth = 0x8D00;
            callGL("_glGenRenderbuffers", {1, idsAddr + 32});
            const uint32_t extra = proc.mem().read<uint32_t>(idsAddr + 32);
            callGL("_glBindRenderbuffer", {kRb, extra});
            callGL("_glFramebufferRenderbuffer", {kFb, kDepth, kRb, extra});
            const uint32_t noStorage = uint32_t(callGL("_glCheckFramebufferStatus", {kFb}));
            callGL("_glRenderbufferStorage", {kRb, 0x81A5 /* GL_DEPTH_COMPONENT16 */, 16, 16});
            const uint32_t wrongSize = uint32_t(callGL("_glCheckFramebufferStatus", {kFb}));
            callGL("_glFramebufferRenderbuffer", {kFb, kDepth, kRb, 0});
            const uint32_t depthDetached = uint32_t(callGL("_glCheckFramebufferStatus", {kFb}));
            callGL("_glFramebufferRenderbuffer", {kFb, kColor, kRb, 0});
            const uint32_t colorDetached = uint32_t(callGL("_glCheckFramebufferStatus", {kFb}));
            callGL("_glFramebufferRenderbuffer", {kFb, kColor, kRb, rbo});   // restore for the rest of the test
            check(noStorage == 0x8CD6 && wrongSize == 0x8CD9 && depthDetached == 0x8CD5 && colorDetached == 0x8CD7,
                  "FBO status: attachment without storage is INCOMPLETE_ATTACHMENT, mismatched sizes INCOMPLETE_DIMENSIONS, detaching returns to the previous state");

            // A query with a different read binding asks about the read framebuffer.
            callGL("_glGenFramebuffers", {1, idsAddr + 36});
            const uint32_t emptyFb = proc.mem().read<uint32_t>(idsAddr + 36);
            callGL("_glBindFramebuffer", {0x8CA9 /* GL_DRAW_FRAMEBUFFER_APPLE */, emptyFb});
            callGL("_glBindFramebuffer", {0x8CA8 /* GL_READ_FRAMEBUFFER_APPLE */, fbo});
            const uint32_t readStatus = uint32_t(callGL("_glCheckFramebufferStatus", {0x8CA8}));
            const uint32_t drawStatus = uint32_t(callGL("_glCheckFramebufferStatus", {0x8CA9}));
            callGL("_glBindFramebuffer", {kFb, fbo});
            check(readStatus == 0x8CD5 && drawStatus == 0x8CD7,
                  "glCheckFramebufferStatus reports the read or draw framebuffer according to its target");
        }

        // Compile & link a GLSL ES 2.0 shader program and draw a full-screen triangle/quad
        const uint32_t vs = uint32_t(callGL("_glCreateShader", {0x8B31 /* GL_VERTEX_SHADER */}));
        const uint32_t fs = uint32_t(callGL("_glCreateShader", {0x8B30 /* GL_FRAGMENT_SHADER */}));
        const uint32_t vsStr = proc.allocCString("attribute vec4 a_position; void main() { gl_Position = a_position; }");
        const uint32_t fsStr = proc.allocCString("uniform vec4 u_color; void main() { gl_FragColor = u_color; }");
        proc.mem().write<uint32_t>(idsAddr + 8, vsStr);
        proc.mem().write<uint32_t>(idsAddr + 12, fsStr);
        callGL("_glShaderSource", {vs, 1, idsAddr + 8, 0});
        callGL("_glCompileShader", {vs});
        callGL("_glShaderSource", {fs, 1, idsAddr + 12, 0});
        callGL("_glCompileShader", {fs});
        const uint32_t prog = uint32_t(callGL("_glCreateProgram", {}));
        callGL("_glAttachShader", {prog, vs});
        callGL("_glAttachShader", {prog, fs});
        callGL("_glLinkProgram", {prog});
        callGL("_glUseProgram", {prog});

        const uint32_t uColName = proc.allocCString("u_color");
        const int32_t uColLoc = int32_t(callGL("_glGetUniformLocation", {prog, uColName}));
        callGL("_glUniform4f", {uint32_t(uColLoc), UIKit::fbits(0.0f), UIKit::fbits(1.0f), UIKit::fbits(0.0f), UIKit::fbits(1.0f)});

        // Clear to blue (0, 0, 1, 1), then draw a green triangle covering the center
        callGL("_glClearColor", {UIKit::fbits(0.0f), UIKit::fbits(0.0f), UIKit::fbits(1.0f), UIKit::fbits(1.0f)});
        callGL("_glClear", {0x00004000 /* GL_COLOR_BUFFER_BIT */});

        const float triVerts[] = { -1.0f, -1.0f,  1.0f, -1.0f,  0.0f, 1.0f };
        const uint32_t vboData = proc.heap().alloc(sizeof triVerts);
        proc.mem().copyIn(vboData, triVerts, sizeof triVerts);
        callGL("_glGenBuffers", {1, idsAddr + 16});
        const uint32_t vbo = proc.mem().read<uint32_t>(idsAddr + 16);
        callGL("_glBindBuffer", {0x8892 /* GL_ARRAY_BUFFER */, vbo});
        callGL("_glBufferData", {0x8892, sizeof triVerts, vboData, 0x88E4});
        callGL("_glEnableVertexAttribArray", {0});
        callGL("_glVertexAttribPointer", {0, 2, 0x1406 /* GL_FLOAT */, 0, 0, 0});
        callGL("_glDrawArrays", {0x0004 /* GL_TRIANGLES */, 0, 3});

        // Read center pixel (32, 16) -> should be green (0, 255, 0, 255)
        callGL("_glReadPixels", {32, 16, 1, 1, 0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */, idsAddr + 24});
        const uint8_t rPx = proc.mem().read<uint8_t>(idsAddr + 24);
        const uint8_t gPx = proc.mem().read<uint8_t>(idsAddr + 25);
        const uint8_t bPx = proc.mem().read<uint8_t>(idsAddr + 26);
        const uint8_t aPx = proc.mem().read<uint8_t>(idsAddr + 27);
        check(rPx == 0 && gPx == 255 && bPx == 0 && aPx == 255, "OpenGL ES 2.0 shader + VBO + glDrawArrays rasterizes triangle");

        // Minion Rush refuses to install its EAGL view unless all three
        // depth/stencil extensions are present. The formats and stencil pipeline
        // must work, not just appear in the extension string.
        const uint32_t depthExtPtr = uint32_t(callGL("_glGetString", {0x1F03}));
        std::string extensions;
        for (uint32_t n = 0; n < 512 && depthExtPtr; ++n) {
            const char ch = char(proc.mem().read<uint8_t>(depthExtPtr + n));
            if (!ch) break;
            extensions.push_back(ch);
        }
        callGL("_glGenRenderbuffers", {2, idsAddr + 48});
        const uint32_t depthRb = proc.mem().read<uint32_t>(idsAddr + 48);
        const uint32_t stencilRb = proc.mem().read<uint32_t>(idsAddr + 52);
        callGL("_glBindRenderbuffer", {0x8D41, depthRb});
        callGL("_glRenderbufferStorage", {0x8D41, 0x81A6, 64, 64});
        callGL("_glGetRenderbufferParameteriv", {0x8D41, 0x8D54, idsAddr + 36});
        callGL("_glGetRenderbufferParameteriv", {0x8D41, 0x8D55, idsAddr + 40});
        const bool depth24 = proc.mem().read<int32_t>(idsAddr + 36) == 24 && proc.mem().read<int32_t>(idsAddr + 40) == 0;
        callGL("_glBindRenderbuffer", {0x8D41, stencilRb});
        callGL("_glRenderbufferStorage", {0x8D41, 0x8D48, 64, 64});
        callGL("_glGetRenderbufferParameteriv", {0x8D41, 0x8D54, idsAddr + 36});
        callGL("_glGetRenderbufferParameteriv", {0x8D41, 0x8D55, idsAddr + 40});
        const bool stencil8 = proc.mem().read<int32_t>(idsAddr + 36) == 0 && proc.mem().read<int32_t>(idsAddr + 40) == 8;
        check(depth24 && stencil8 && extensions.find("GL_OES_depth24") != std::string::npos &&
                  extensions.find("GL_OES_stencil8") != std::string::npos &&
                  extensions.find("GL_OES_packed_depth_stencil") != std::string::npos,
              "GLES advertises and allocates depth24, stencil8 and packed depth/stencil renderbuffers");

        callGL("_glGenRenderbuffers", {1, idsAddr + 56});
        const uint32_t packedRb = proc.mem().read<uint32_t>(idsAddr + 56);
        callGL("_glBindRenderbuffer", {0x8D41, packedRb});
        callGL("_glRenderbufferStorage", {0x8D41, 0x88F0, 64, 64});
        callGL("_glFramebufferRenderbuffer", {0x8D40, 0x8D00, 0x8D41, packedRb});
        callGL("_glFramebufferRenderbuffer", {0x8D40, 0x8D20, 0x8D41, packedRb});
        callGL("_glGetRenderbufferParameteriv", {0x8D41, 0x8D54, idsAddr + 36});
        callGL("_glGetRenderbufferParameteriv", {0x8D41, 0x8D55, idsAddr + 40});
        const bool packedBits = proc.mem().read<int32_t>(idsAddr + 36) == 24 && proc.mem().read<int32_t>(idsAddr + 40) == 8;
        callGL("_glClearStencil", {1});
        callGL("_glClear", {0x00000400});
        callGL("_glEnable", {0x0B90});
        callGL("_glStencilFunc", {0x0207, 3, 0xFF}); // ALWAYS, reference 3
        callGL("_glStencilOp", {0x1E00, 0x1E00, 0x1E01}); // KEEP, KEEP, REPLACE
        callGL("_glStencilMask", {0x0F});
        callGL("_glUseProgram", {prog});
        callGL("_glDrawArrays", {4, 0, 3});
        callGL("_glStencilFunc", {0x0202, 3, 0x0F}); // EQUAL, previous write must be 3
        callGL("_glDrawArrays", {4, 0, 3});
        callGL("_glUniform4f", {uint32_t(uColLoc), UIKit::fbits(1.0f), 0, 0, UIKit::fbits(1.0f)});
        callGL("_glStencilFunc", {0x0202, 4, 0x0F}); // must reject and preserve green
        callGL("_glDrawArrays", {4, 0, 3});
        callGL("_glReadPixels", {32, 16, 1, 1, 0x1908, 0x1401, idsAddr + 24});
        callGL("_glDisable", {0x0B90});
        callGL("_glUniform4f", {uint32_t(uColLoc), 0, UIKit::fbits(1.0f), 0, UIKit::fbits(1.0f)});
        check(packedBits && proc.mem().read<uint8_t>(idsAddr + 24) == 0 && proc.mem().read<uint8_t>(idsAddr + 25) == 255,
              "GLES packed depth/stencil supports clear, compare, mask and replace operations");

        // OpenGL ES 2.0 §2.10: deletion is deferred while a program is current.
        // Telltale's render thread draws with programs deleted by another thread.
        const uint32_t deletedProg = uint32_t(callGL("_glCreateProgram", {}));
        callGL("_glAttachShader", {deletedProg, vs});
        callGL("_glAttachShader", {deletedProg, fs});
        callGL("_glLinkProgram", {deletedProg});
        callGL("_glUseProgram", {deletedProg});
        const int32_t deletedCol = int32_t(callGL("_glGetUniformLocation", {deletedProg, uColName}));
        callGL("_glUniform4f", {uint32_t(deletedCol), UIKit::fbits(0.0f), UIKit::fbits(1.0f), UIKit::fbits(0.0f), UIKit::fbits(1.0f)});
        callGL("_glDeleteProgram", {deletedProg});
        callGL("_glClear", {0x4000});
        callGL("_glDrawArrays", {4, 0, 3});
        callGL("_glReadPixels", {32, 16, 1, 1, 0x1908, 0x1401, idsAddr + 24});
        callGL("_glGetIntegerv", {0x8B8D /* GL_CURRENT_PROGRAM */, idsAddr + 36});
        check(proc.mem().read<uint8_t>(idsAddr + 25) == 255 &&
              proc.mem().read<uint32_t>(idsAddr + 36) == deletedProg &&
              callGL("_glIsProgram", {deletedProg}) == 0,
              "GLES2 defers program deletion while the program is current");
        callGL("_glUseProgram", {prog});

        // Software rendering also supports the old worker-thread fallback to
        // the last active context; that binding must retain the program too.
        const uint32_t workerProg = uint32_t(callGL("_glCreateProgram", {}));
        callGL("_glAttachShader", {workerProg, vs});
        callGL("_glAttachShader", {workerProg, fs});
        callGL("_glLinkProgram", {workerProg});
        callGL("_glUseProgram", {workerProg});
        const int32_t workerCol = int32_t(callGL("_glGetUniformLocation", {workerProg, uColName}));
        callGL("_glUniform4f", {uint32_t(workerCol), UIKit::fbits(0.0f), UIKit::fbits(1.0f), UIKit::fbits(0.0f), UIKit::fbits(1.0f)});
        r.send(cpu, ctxCls, "setCurrentContext:", {0});
        callGL("_glDeleteProgram", {workerProg});
        callGL("_glClear", {0x4000});
        callGL("_glDrawArrays", {4, 0, 3});
        callGL("_glReadPixels", {32, 16, 1, 1, 0x1908, 0x1401, idsAddr + 24});
        check(proc.mem().read<uint8_t>(idsAddr + 25) == 255 && callGL("_glIsProgram", {workerProg}) == 0,
              "GLES2 worker fallback retains a deleted current program");
        r.send(cpu, ctxCls, "setCurrentContext:", {glCtx});

        // Telltale's main thread can change the fallback context while its
        // worker is drawing. A draw must observe one consistent program.
        std::atomic<bool> stopProgramChanges{false};
        std::atomic<int> programChanges{0};
        const auto changeProgram = proc.defineFunction("__gl_program_change_worker", [&](Ctx& workerContext) {
            while (!stopProgramChanges.load(std::memory_order_relaxed)) {
                proc.callGuest(workerContext.cpu, *proc.lookupSymbol("_glUseProgram"), {0});
                programChanges.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
            }
            workerContext.cpu.r[0] = 0;
        });
        const uint32_t programThread = proc.startThread(changeProgram, 0, 64u << 10, false);
        bool stableDraw = true;
        for (int iteration = 0; iteration < 30; ++iteration) {
            callGL("_glUseProgram", {prog});
            callGL("_glDrawArrays", {4, 0, 3});
        }
        stopProgramChanges.store(true, std::memory_order_relaxed);
        uint32_t programThreadResult = 1;
        stableDraw &= proc.joinThread(programThread, &programThreadResult) && programThreadResult == 0;
        check(stableDraw && programChanges.load(std::memory_order_relaxed) > 0,
              "GLES2 drawing stays consistent during cross-thread program changes");

        std::atomic<bool> stopProgramObjects{false};
        std::atomic<int> programObjects{0};
        const auto recyclePrograms = proc.defineFunction("__gl_program_recycle_worker", [&](Ctx& workerContext) {
            while (!stopProgramObjects.load(std::memory_order_relaxed)) {
                const uint32_t temporary = uint32_t(proc.callGuest(workerContext.cpu, *proc.lookupSymbol("_glCreateProgram"), {}));
                proc.callGuest(workerContext.cpu, *proc.lookupSymbol("_glDeleteProgram"), {temporary});
                programObjects.fetch_add(1, std::memory_order_relaxed);
            }
            workerContext.cpu.r[0] = 0;
        });
        const uint32_t recycleThread = proc.startThread(recyclePrograms, 0, 64u << 10, false);
        for (int iteration = 0; iteration < 60; ++iteration) {
            callGL("_glUseProgram", {prog});
            callGL("_glDrawArrays", {4, 0, 3});
        }
        stopProgramObjects.store(true, std::memory_order_relaxed);
        uint32_t recycleResult = 1;
        const bool recycled = proc.joinThread(recycleThread, &recycleResult) && recycleResult == 0;
        check(recycled && programObjects.load(std::memory_order_relaxed) > 0,
              "GLES2 program table stays valid when another thread creates and deletes programs");

        // End-to-end programmable pipeline: arbitrary names, two varyings,
        // perspective interpolation and a nonzero sampler texture unit.
        auto shader = [&](uint32_t stage, const char* source) {
            uint32_t id = uint32_t(callGL("_glCreateShader", {stage}));
            proc.mem().write<uint32_t>(idsAddr + 8, proc.allocCString(source));
            callGL("_glShaderSource", {id, 1, idsAddr + 8, 0}); callGL("_glCompileShader", {id});
            return id;
        };
        uint32_t vertex = shader(0x8B31, "attribute vec4 location; attribute float weight; uniform vec4 scale; varying vec4 first; varying vec4 second; varying vec2 uv; void main() { gl_Position = location * scale; first = vec4(weight); second = vec4(0.0,0.0,0.25,1.0); uv = vec2(0.5); }");
        uint32_t fragment = shader(0x8B30, "uniform sampler2D image; varying vec4 first; varying vec4 second; varying vec2 uv; void main() { vec4 tx = texture2D(image,uv); gl_FragColor = vec4(tx.r * first.x,tx.g,second.b,1.0); }");
        uint32_t pipeline = uint32_t(callGL("_glCreateProgram", {}));
        callGL("_glAttachShader", {pipeline, vertex}); callGL("_glAttachShader", {pipeline, fragment});
        callGL("_glBindAttribLocation", {pipeline, 0, proc.allocCString("location")});
        callGL("_glBindAttribLocation", {pipeline, 1, proc.allocCString("weight")});
        callGL("_glLinkProgram", {pipeline}); callGL("_glUseProgram", {pipeline});
        int32_t scale = int32_t(callGL("_glGetUniformLocation", {pipeline, proc.allocCString("scale")}));
        callGL("_glUniform4f", {uint32_t(scale), UIKit::fbits(1), UIKit::fbits(1), UIKit::fbits(1), UIKit::fbits(1)});
        int32_t image = int32_t(callGL("_glGetUniformLocation", {pipeline, proc.allocCString("image")}));
        callGL("_glUniform1i", {uint32_t(image), 3});
        callGL("_glActiveTexture", {0x84C3}); callGL("_glGenTextures", {1, idsAddr + 32});
        callGL("_glBindTexture", {0x0DE1, proc.mem().read<uint32_t>(idsAddr + 32)});
        const uint32_t texel = proc.heap().alloc(4); const uint8_t rgba[] = {128,64,192,255};
        proc.mem().copyIn(texel, rgba, 4);
        callGL("_glTexImage2D", {0x0DE1,0,0x1908,1,1,0,0x1908,0x1401,texel});
        const float vertices[] = {-1,-1,0,1,0, 2,-2,0,2,1, 0,4,0,4,0};
        const uint32_t data = proc.heap().alloc(sizeof vertices); proc.mem().copyIn(data, vertices, sizeof vertices);
        callGL("_glBufferData", {0x8892, sizeof vertices, data, 0x88E4});
        callGL("_glVertexAttribPointer", {0,4,0x1406,0,20,0});
        callGL("_glEnableVertexAttribArray", {1}); callGL("_glVertexAttribPointer", {1,1,0x1406,0,20,16});
        callGL("_glClear", {0x4000}); callGL("_glDrawArrays", {4,0,3});
        callGL("_glReadPixels", {32,16,1,1,0x1908,0x1401,idsAddr + 24});
        // Pixel-center barycentrics are (93,97,66)/256. Divide each by clip W.
        const float perspective = (97.0f / 512) / (93.0f / 256 + 97.0f / 512 + 66.0f / 1024);
        int expectedRed = int(128 * perspective + 0.5f);
        check(std::abs(int(proc.mem().read<uint8_t>(idsAddr + 24)) - expectedRed) <= 1 &&
              proc.mem().read<uint8_t>(idsAddr + 25) == 64 && proc.mem().read<uint8_t>(idsAddr + 26) == 64 &&
              proc.mem().read<uint8_t>(idsAddr + 27) == 255,
              "GLES2 rasterizes perspective-correct varyings, multiple colors and texture unit 3");
        auto bad = shader(0x8B30, "void main() { gl_FragColor = missing(); }");
        callGL("_glGetShaderiv", {bad,0x8B81,idsAddr + 36});
        callGL("_glGetShaderInfoLog", {bad,16,idsAddr + 40,idsAddr + 44});
        bool diagnostics = proc.mem().read<uint32_t>(idsAddr + 36) == 0 &&
            proc.mem().read<uint32_t>(idsAddr + 40) == 15 && proc.mem().read<uint8_t>(idsAddr + 59) == 0;
        uint32_t badProgram = uint32_t(callGL("_glCreateProgram", {}));
        callGL("_glAttachShader", {badProgram,vertex}); callGL("_glAttachShader", {badProgram,bad});
        callGL("_glLinkProgram", {badProgram}); callGL("_glGetProgramiv", {badProgram,0x8B82,idsAddr + 36});
        diagnostics &= proc.mem().read<uint32_t>(idsAddr + 36) == 0 &&
            int32_t(callGL("_glGetUniformLocation", {pipeline,proc.allocCString("absent")})) == -1 &&
            int32_t(callGL("_glGetAttribLocation", {pipeline,proc.allocCString("absent")})) == -1;
        check(diagnostics, "GLES2 exposes compile/link errors, bounded info logs and unknown locations");

        // GL_MAX_VARYING_VECTORS agrees with the linker and gl_MaxVaryingVectors (#14):
        // N vec4 varyings link, N + 1 do not.
        {
            callGL("_glGetIntegerv", {0x8DFC /* GL_MAX_VARYING_VECTORS */, idsAddr + 36});
            const int32_t maxVaryings = int32_t(proc.mem().read<uint32_t>(idsAddr + 36));
            auto linksWith = [&](int n) {
                std::string decl, vbody = "gl_Position = a;", fbody = "vec4 s = vec4(0.0);";
                for (int i = 0; i < n; ++i) {
                    decl += "varying vec4 v" + std::to_string(i) + ";";
                    vbody += "v" + std::to_string(i) + " = a;";
                    fbody += "s += v" + std::to_string(i) + ";";
                }
                const std::string vsrc = "attribute vec4 a;" + decl + "void main(){" + vbody + "}";
                const std::string fsrc = "precision mediump float;" + decl + "void main(){" + fbody + "gl_FragColor = s;}";
                const uint32_t v = uint32_t(callGL("_glCreateShader", {0x8B31})), f = uint32_t(callGL("_glCreateShader", {0x8B30}));
                proc.mem().write<uint32_t>(idsAddr + 48, proc.allocCString(vsrc.c_str()));
                proc.mem().write<uint32_t>(idsAddr + 52, proc.allocCString(fsrc.c_str()));
                callGL("_glShaderSource", {v, 1, idsAddr + 48, 0}); callGL("_glCompileShader", {v});
                callGL("_glShaderSource", {f, 1, idsAddr + 52, 0}); callGL("_glCompileShader", {f});
                const uint32_t pr = uint32_t(callGL("_glCreateProgram", {}));
                callGL("_glAttachShader", {pr, v}); callGL("_glAttachShader", {pr, f});
                callGL("_glLinkProgram", {pr});
                callGL("_glGetProgramiv", {pr, 0x8B82 /* GL_LINK_STATUS */, idsAddr + 36});
                return proc.mem().read<uint32_t>(idsAddr + 36) != 0;
            };
            check(maxVaryings == 16 && linksWith(maxVaryings) && !linksWith(maxVaryings + 1),
                  "GL_MAX_VARYING_VECTORS (16) matches the linker: 16 vec4 varyings link, 17 do not");
        }

        // Struct leaves and array elements are real uniform locations. The count==1
        // uniforms above keep the locations this draw already checked.
        uint32_t structVs = shader(0x8B31,
            "struct Material { vec4 color; float intensity; };\n"
            "uniform Material u;\nuniform vec4 pal[2];\nattribute vec4 a_position;\nvarying Material m;\n"
            "void main() { gl_Position = a_position; m = u; m.color = m.color * m.intensity + pal[0] + pal[1]; }");
        uint32_t structFs = shader(0x8B30,
            "struct Material { vec4 color; float intensity; };\nvarying Material m;\nvoid main() { gl_FragColor = m.color; }");
        uint32_t structProg = uint32_t(callGL("_glCreateProgram", {}));
        callGL("_glAttachShader", {structProg, structVs});
        callGL("_glAttachShader", {structProg, structFs});
        callGL("_glBindAttribLocation", {structProg, 0, proc.allocCString("a_position")});
        callGL("_glLinkProgram", {structProg});
        callGL("_glGetProgramiv", {structProg, 0x8B82, idsAddr + 36});
        const bool structLinked = proc.mem().read<int32_t>(idsAddr + 36) == 1;
        callGL("_glUseProgram", {structProg});
        const int32_t colorLoc = int32_t(callGL("_glGetUniformLocation", {structProg, proc.allocCString("u.color")}));
        const int32_t intensityLoc = int32_t(callGL("_glGetUniformLocation", {structProg, proc.allocCString("u.intensity")}));
        const int32_t palLoc = int32_t(callGL("_glGetUniformLocation", {structProg, proc.allocCString("pal")}));
        const int32_t pal0 = int32_t(callGL("_glGetUniformLocation", {structProg, proc.allocCString("pal[0]")}));
        const int32_t pal1 = int32_t(callGL("_glGetUniformLocation", {structProg, proc.allocCString("pal[1]")}));
        callGL("_glGetProgramiv", {structProg, 0x8B86, idsAddr + 36});
        const int32_t activeUniforms = proc.mem().read<int32_t>(idsAddr + 36);
        bool palActive = false;
        const uint32_t activeName = proc.heap().allocZeroed(64);
        for (int32_t i = 0; i < activeUniforms; ++i) {
            callGL("_glGetActiveUniform", {structProg, uint32_t(i), 64, 0, idsAddr + 40, idsAddr + 44, activeName});
            std::string nm;
            for (int n = 0; n < 64; ++n) {
                char ch = char(proc.mem().read<uint8_t>(activeName + uint32_t(n)));
                if (!ch) break;
                nm.push_back(ch);
            }
            if (nm == "pal[0]" && proc.mem().read<int32_t>(idsAddr + 40) == 2 && proc.mem().read<uint32_t>(idsAddr + 44) == 0x8B52)
                palActive = true;
        }
        callGL("_glUniform4f", {uint32_t(colorLoc), UIKit::fbits(0.5f), UIKit::fbits(0), UIKit::fbits(0), UIKit::fbits(1)});
        callGL("_glUniform1f", {uint32_t(intensityLoc), UIKit::fbits(1)});
        const float palData[] = {0, 0.5f, 0, 0, 0, 0, 1, 0};
        const uint32_t palPtr = proc.heap().alloc(sizeof palData);
        proc.mem().copyIn(palPtr, palData, sizeof palData);
        callGL("_glUniform4fv", {uint32_t(palLoc), 2, palPtr});
        const float cover[] = {-1, -1, 1, -1, 0, 1};
        const uint32_t coverPtr = proc.heap().alloc(sizeof cover);
        proc.mem().copyIn(coverPtr, cover, sizeof cover);
        callGL("_glBufferData", {0x8892, sizeof cover, coverPtr, 0x88E4});
        callGL("_glVertexAttribPointer", {0, 2, 0x1406, 0, 0, 0});
        callGL("_glDrawArrays", {0x0004, 0, 3});
        callGL("_glReadPixels", {32, 16, 1, 1, 0x1908, 0x1401, idsAddr + 24});
        check(structLinked && activeUniforms == 3 && palActive && colorLoc >= 0 && intensityLoc == colorLoc + 1 &&
              palLoc == pal0 && pal1 == palLoc + 1 && palLoc != colorLoc &&
              proc.mem().read<uint8_t>(idsAddr + 24) == 128 && proc.mem().read<uint8_t>(idsAddr + 25) == 128 &&
              proc.mem().read<uint8_t>(idsAddr + 26) == 255 && proc.mem().read<uint8_t>(idsAddr + 27) == 255,
              "GLES2 links struct-uniform leaves and array elements to consecutive locations");

        uint32_t matVs = shader(0x8B31, "attribute vec4 a_position;\nvoid main() { gl_Position = a_position; }");
        uint32_t matFs = shader(0x8B30,
            "uniform mat3 m3;\nuniform mat2 m2;\nuniform int counter;\nuniform bool flag;\nuniform samplerCube cube;\n"
            "void main() { gl_FragColor = vec4(m3[0].z, m3[1].x, m2[1].x, float(counter)); }");
        uint32_t matProg = uint32_t(callGL("_glCreateProgram", {}));
        callGL("_glAttachShader", {matProg, matVs});
        callGL("_glAttachShader", {matProg, matFs});
        callGL("_glBindAttribLocation", {matProg, 0, proc.allocCString("a_position")});
        callGL("_glLinkProgram", {matProg});
        callGL("_glGetProgramiv", {matProg, 0x8B82, idsAddr + 36});
        const bool matLinked = proc.mem().read<int32_t>(idsAddr + 36) == 1;
        callGL("_glUseProgram", {matProg});
        bool sawMat3 = false, sawMat2 = false, sawInt = false, sawBool = false, sawCube = false;
        callGL("_glGetProgramiv", {matProg, 0x8B86, idsAddr + 36});
        const int32_t matUniforms = proc.mem().read<int32_t>(idsAddr + 36);
        for (int32_t i = 0; i < matUniforms; ++i) {
            callGL("_glGetActiveUniform", {matProg, uint32_t(i), 64, 0, idsAddr + 40, idsAddr + 44, activeName});
            std::string nm;
            for (int n = 0; n < 64; ++n) {
                char ch = char(proc.mem().read<uint8_t>(activeName + uint32_t(n)));
                if (!ch) break;
                nm.push_back(ch);
            }
            const uint32_t ty = proc.mem().read<uint32_t>(idsAddr + 44);
            if (nm == "m3" && ty == 0x8B5B) sawMat3 = true;
            if (nm == "m2" && ty == 0x8B5A) sawMat2 = true;
            if (nm == "counter" && ty == 0x1404) sawInt = true;
            if (nm == "flag" && ty == 0x8B56) sawBool = true;
            if (nm == "cube" && ty == 0x8B60) sawCube = true;
        }
        const float m3data[] = {0, 1, 0, 0, 0, 1, 1, 0, 0};
        const float m2data[] = {0, 1, 0, 0};
        const uint32_t m3ptr = proc.heap().alloc(sizeof m3data);
        const uint32_t m2ptr = proc.heap().alloc(sizeof m2data);
        proc.mem().copyIn(m3ptr, m3data, sizeof m3data);
        proc.mem().copyIn(m2ptr, m2data, sizeof m2data);
        const int32_t m3loc = int32_t(callGL("_glGetUniformLocation", {matProg, proc.allocCString("m3")}));
        const int32_t m2loc = int32_t(callGL("_glGetUniformLocation", {matProg, proc.allocCString("m2")}));
        const int32_t counterLoc = int32_t(callGL("_glGetUniformLocation", {matProg, proc.allocCString("counter")}));
        callGL("_glUniformMatrix3fv", {uint32_t(m3loc), 1, 1, m3ptr});
        callGL("_glUniformMatrix2fv", {uint32_t(m2loc), 1, 1, m2ptr});
        callGL("_glUniform1i", {uint32_t(counterLoc), 1});
        callGL("_glBufferData", {0x8892, sizeof cover, coverPtr, 0x88E4});
        callGL("_glVertexAttribPointer", {0, 2, 0x1406, 0, 0, 0});
        callGL("_glDrawArrays", {0x0004, 0, 3});
        callGL("_glReadPixels", {32, 16, 1, 1, 0x1908, 0x1401, idsAddr + 24});
        check(matLinked && sawMat3 && sawMat2 && sawInt && sawBool && sawCube &&
                  proc.mem().read<uint8_t>(idsAddr + 24) == 255 && proc.mem().read<uint8_t>(idsAddr + 25) == 255 &&
                  proc.mem().read<uint8_t>(idsAddr + 26) == 255 && proc.mem().read<uint8_t>(idsAddr + 27) == 255,
              "GLES2 transposes mat2/mat3 when requested and reports int, bool and samplerCube types");

        uint32_t derivVs = shader(0x8B31, "attribute vec4 a_position;\nvoid main() { gl_Position = a_position; }");
        uint32_t derivFs = shader(0x8B30,
            "#extension GL_OES_standard_derivatives : require\n"
            "void main() { gl_FragColor = vec4(dFdx(gl_FragCoord.x), dFdy(gl_FragCoord.y), fwidth(gl_FragCoord.x), 1.0); }");
        uint32_t derivProg = uint32_t(callGL("_glCreateProgram", {}));
        callGL("_glAttachShader", {derivProg, derivVs});
        callGL("_glAttachShader", {derivProg, derivFs});
        callGL("_glBindAttribLocation", {derivProg, 0, proc.allocCString("a_position")});
        callGL("_glLinkProgram", {derivProg});
        callGL("_glGetProgramiv", {derivProg, 0x8B82, idsAddr + 36});
        const bool derivLinked = proc.mem().read<int32_t>(idsAddr + 36) == 1;
        callGL("_glUseProgram", {derivProg});
        callGL("_glBufferData", {0x8892, sizeof cover, coverPtr, 0x88E4});
        callGL("_glVertexAttribPointer", {0, 2, 0x1406, 0, 0, 0});
        callGL("_glDrawArrays", {0x0004, 0, 3});
        callGL("_glReadPixels", {32, 16, 1, 1, 0x1908, 0x1401, idsAddr + 24});
        const uint32_t extPtr = uint32_t(callGL("_glGetString", {0x1F03}));
        std::string extStr;
        for (uint32_t n = 0; n < 512 && extPtr; ++n) {
            char ch = char(proc.mem().read<uint8_t>(extPtr + n));
            if (!ch) break;
            extStr.push_back(ch);
        }
        check(derivLinked && extStr.find("GL_OES_standard_derivatives") != std::string::npos &&
                  proc.mem().read<uint8_t>(idsAddr + 24) == 255 && proc.mem().read<uint8_t>(idsAddr + 25) == 255 &&
                  proc.mem().read<uint8_t>(idsAddr + 26) == 255 && proc.mem().read<uint8_t>(idsAddr + 27) == 255,
              "GLES2 dFdx/dFdy/fwidth use 2x2 quads and advertise GL_OES_standard_derivatives");

        uint32_t warnFs = shader(0x8B30, "#extension GL_OES_foo : warn\nvoid main() { gl_FragColor = vec4(1.0); }\n");
        callGL("_glGetShaderiv", {warnFs, 0x8B81, idsAddr + 36});
        const bool warnCompiled = proc.mem().read<int32_t>(idsAddr + 36) == 1;
        callGL("_glGetShaderiv", {warnFs, 0x8B84, idsAddr + 40});
        const int32_t warnLen = proc.mem().read<int32_t>(idsAddr + 40);
        const uint32_t warnLog = proc.heap().allocZeroed(256);
        callGL("_glGetShaderInfoLog", {warnFs, 256, 0, warnLog});
        std::string warnText;
        for (int n = 0; n < 255; ++n) {
            char ch = char(proc.mem().read<uint8_t>(warnLog + uint32_t(n)));
            if (!ch) break;
            warnText.push_back(ch);
        }
        uint32_t boneVs = shader(0x8B31, "attribute vec4 a_position;\nvoid main() { gl_Position = a_position; }");
        uint32_t boneFs = shader(0x8B30,
            "const int N = int(abs(-4.0));\nconst int Bones = 41 + N;\nuniform vec4 u[Bones];\n"
            "void main() { gl_FragColor = u[44]; }");
        uint32_t boneProg = uint32_t(callGL("_glCreateProgram", {}));
        callGL("_glAttachShader", {boneProg, boneVs});
        callGL("_glAttachShader", {boneProg, boneFs});
        callGL("_glBindAttribLocation", {boneProg, 0, proc.allocCString("a_position")});
        callGL("_glLinkProgram", {boneProg});
        callGL("_glGetProgramiv", {boneProg, 0x8B82, idsAddr + 36});
        const bool boneLinked = proc.mem().read<int32_t>(idsAddr + 36) == 1;
        callGL("_glUseProgram", {boneProg});
        const int32_t boneLoc = int32_t(callGL("_glGetUniformLocation", {boneProg, proc.allocCString("u[44]")}));
        callGL("_glUniform4f", {uint32_t(boneLoc), UIKit::fbits(1.0f), UIKit::fbits(0.0f), UIKit::fbits(0.0f), UIKit::fbits(1.0f)});
        callGL("_glBufferData", {0x8892, sizeof cover, coverPtr, 0x88E4});
        callGL("_glVertexAttribPointer", {0, 2, 0x1406, 0, 0, 0});
        callGL("_glDrawArrays", {0x0004, 0, 3});
        callGL("_glReadPixels", {32, 16, 1, 1, 0x1908, 0x1401, idsAddr + 24});
        check(warnCompiled && warnLen > 1 && warnText.find("warning") != std::string::npos &&
                  warnText.find("GL_OES_foo") != std::string::npos && boneLinked && boneLoc >= 0 &&
                  proc.mem().read<uint8_t>(idsAddr + 24) == 255 && proc.mem().read<uint8_t>(idsAddr + 25) == 0 &&
                  proc.mem().read<uint8_t>(idsAddr + 26) == 0 && proc.mem().read<uint8_t>(idsAddr + 27) == 255,
              "GLES2 warns on #extension : warn and draws a uniform vec4 array of size 45");

        uint32_t skinVs = shader(0x8B31,
            "attribute vec4 a_position;\nuniform mat4 bones[45];\nuniform int bone;\n"
            "void main() { gl_Position = bones[bone] * a_position; }\n");
        uint32_t skinFs = shader(0x8B30, "void main() { gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }\n");
        uint32_t skinProg = uint32_t(callGL("_glCreateProgram", {}));
        callGL("_glAttachShader", {skinProg, skinVs});
        callGL("_glAttachShader", {skinProg, skinFs});
        callGL("_glBindAttribLocation", {skinProg, 0, proc.allocCString("a_position")});
        callGL("_glLinkProgram", {skinProg});
        callGL("_glGetProgramiv", {skinProg, 0x8B82, idsAddr + 36});
        const bool skinLinked = proc.mem().read<int32_t>(idsAddr + 36) == 1;
        callGL("_glUseProgram", {skinProg});
        const int32_t skinBone = int32_t(callGL("_glGetUniformLocation", {skinProg, proc.allocCString("bone")}));
        const int32_t skinMat = int32_t(callGL("_glGetUniformLocation", {skinProg, proc.allocCString("bones[44]")}));
        const float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        const uint32_t identPtr = proc.heap().alloc(sizeof ident);
        proc.mem().copyIn(identPtr, ident, sizeof ident);
        callGL("_glUniform1i", {uint32_t(skinBone), 44});
        callGL("_glUniformMatrix4fv", {uint32_t(skinMat), 1, 0, identPtr});
        callGL("_glClearColor", {UIKit::fbits(0.0f), UIKit::fbits(0.0f), UIKit::fbits(1.0f), UIKit::fbits(1.0f)});
        callGL("_glClear", {0x00004000});
        callGL("_glBufferData", {0x8892, sizeof cover, coverPtr, 0x88E4});
        callGL("_glVertexAttribPointer", {0, 2, 0x1406, 0, 0, 0});
        callGL("_glDrawArrays", {0x0004, 0, 3});
        callGL("_glReadPixels", {32, 16, 1, 1, 0x1908, 0x1401, idsAddr + 24});
        check(skinLinked && skinBone >= 0 && skinMat >= 0 &&
                  proc.mem().read<uint8_t>(idsAddr + 24) == 255 && proc.mem().read<uint8_t>(idsAddr + 25) == 0 &&
                  proc.mem().read<uint8_t>(idsAddr + 26) == 0 && proc.mem().read<uint8_t>(idsAddr + 27) == 255,
              "GLES2 draws a skinned triangle from uniform mat4 bones[45]");

        uint32_t loopVs = shader(0x8B31, "attribute vec4 a_position;\nvoid main() { gl_Position = a_position; }");
        uint32_t loopFs = shader(0x8B30, "void main() { gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); for(;;) {} }");
        uint32_t loopProg = uint32_t(callGL("_glCreateProgram", {}));
        callGL("_glAttachShader", {loopProg, loopVs});
        callGL("_glAttachShader", {loopProg, loopFs});
        callGL("_glBindAttribLocation", {loopProg, 0, proc.allocCString("a_position")});
        callGL("_glLinkProgram", {loopProg});
        callGL("_glGetProgramiv", {loopProg, 0x8B82, idsAddr + 36});
        const bool loopLinked = proc.mem().read<int32_t>(idsAddr + 36) == 1;
        callGL("_glGetError", {});
        callGL("_glClearColor", {UIKit::fbits(0.0f), UIKit::fbits(0.0f), UIKit::fbits(1.0f), UIKit::fbits(1.0f)});
        callGL("_glClear", {0x00004000});
        callGL("_glUseProgram", {loopProg});
        callGL("_glDrawArrays", {0x0004, 0, 3});
        const uint32_t loopErr = uint32_t(callGL("_glGetError", {}));
        callGL("_glReadPixels", {32, 16, 1, 1, 0x1908, 0x1401, idsAddr + 24});
        const bool stayedBlue = proc.mem().read<uint8_t>(idsAddr + 24) == 0 && proc.mem().read<uint8_t>(idsAddr + 25) == 0 &&
                                proc.mem().read<uint8_t>(idsAddr + 26) == 255 && proc.mem().read<uint8_t>(idsAddr + 27) == 255;
        callGL("_glUseProgram", {prog});
        callGL("_glDrawArrays", {0x0004, 0, 3});
        callGL("_glReadPixels", {32, 16, 1, 1, 0x1908, 0x1401, idsAddr + 24});
        const bool drewGreen = proc.mem().read<uint8_t>(idsAddr + 24) == 0 && proc.mem().read<uint8_t>(idsAddr + 25) == 255 &&
                               proc.mem().read<uint8_t>(idsAddr + 26) == 0 && proc.mem().read<uint8_t>(idsAddr + 27) == 255;
        const uint32_t afterErr = uint32_t(callGL("_glGetError", {}));
        check(loopLinked && loopErr == 0x0502 && stayedBlue && drewGreen && afterErr == 0,
              "GLES2 aborts a draw that exceeds the instruction budget and discards its fragments");
        // Homogeneous clip: triangles that cross the camera (w<=0) or the near
        // plane (z<-w) must be clipped in clip space before the perspective divide.
        {
            uint32_t clipVs = shader(0x8B31, "attribute vec4 pos; attribute vec4 col; varying vec4 vcol; void main() { gl_Position = pos; vcol = col; }");
            uint32_t clipFs = shader(0x8B30, "varying vec4 vcol; void main() { gl_FragColor = vcol; }");
            uint32_t clipProg = uint32_t(callGL("_glCreateProgram", {}));
            callGL("_glBindAttribLocation", {clipProg, 0, proc.allocCString("pos")});
            callGL("_glBindAttribLocation", {clipProg, 1, proc.allocCString("col")});
            callGL("_glAttachShader", {clipProg, clipVs});
            callGL("_glAttachShader", {clipProg, clipFs});
            callGL("_glLinkProgram", {clipProg});
            callGL("_glUseProgram", {clipProg});
            callGL("_glViewport", {0, 0, 64, 64});
            callGL("_glDisable", {0x0B44 /* GL_CULL_FACE */});
            callGL("_glDisable", {0x0B71 /* GL_DEPTH_TEST */});
            callGL("_glEnableVertexAttribArray", {0});
            callGL("_glEnableVertexAttribArray", {1});
            const uint32_t pix = proc.heap().allocZeroed(16);
            auto readPx = [&](int x, int y, uint8_t& pr, uint8_t& pg, uint8_t& pb, uint8_t& pa) {
                callGL("_glReadPixels", {uint32_t(x), uint32_t(y), 1, 1, 0x1908, 0x1401, pix});
                pr = proc.mem().read<uint8_t>(pix);
                pg = proc.mem().read<uint8_t>(pix + 1);
                pb = proc.mem().read<uint8_t>(pix + 2);
                pa = proc.mem().read<uint8_t>(pix + 3);
            };
            auto closeTo = [](uint8_t pr, uint8_t pg, uint8_t pb, int er, int eg, int eb) {
                return std::abs(int(pr) - er) <= 3 && std::abs(int(pg) - eg) <= 3 && std::abs(int(pb) - eb) <= 3;
            };
            auto clearBlue = [&]() {
                callGL("_glClearColor", {UIKit::fbits(0), UIKit::fbits(0), UIKit::fbits(1), UIKit::fbits(1)});
                callGL("_glClear", {0x4000});
            };
            auto drawClip = [&](uint32_t mode, const float* verts, int nVerts) {
                const uint32_t nbytes = uint32_t(nVerts * 8 * sizeof(float));
                const uint32_t mem = proc.heap().alloc(nbytes);
                proc.mem().copyIn(mem, verts, nbytes);
                callGL("_glBufferData", {0x8892, nbytes, mem, 0x88E4});
                callGL("_glVertexAttribPointer", {0, 4, 0x1406, 0, 32, 0});
                callGL("_glVertexAttribPointer", {1, 4, 0x1406, 0, 32, 16});
                callGL("_glDrawArrays", {mode, 0, uint32_t(nVerts)});
            };
            uint8_t pr = 0, pg = 0, pb = 0, pa = 0, qr = 0, qg = 0, qb = 0, qa = 0;

            const float insideTri[] = {
                -0.5f, -0.5f, 0.f, 1.f,  1.f, 0.f, 0.f, 1.f,
                 0.5f, -0.5f, 0.f, 1.f,  0.f, 1.f, 0.f, 1.f,
                 0.0f,  0.5f, 0.f, 1.f,  0.f, 0.f, 1.f, 1.f,
            };
            clearBlue();
            drawClip(0x0004, insideTri, 3);
            readPx(32, 40, pr, pg, pb, pa);
            std::printf("  clip inside (32,40)=%u,%u,%u,%u\n", pr, pg, pb, pa);
            check(closeTo(pr, pg, pb, 26, 34, 195) && pa == 255,
                  "GLES2 triangle entirely in front of the near plane keeps its coverage and perspective-correct color");

            const float crossW[] = {
                -0.8f, -0.8f,  0.f,  1.f,  1.f, 0.f, 0.f, 1.f,
                -0.8f,  0.8f,  0.f,  1.f,  1.f, 0.f, 0.f, 1.f,
                 0.2f,  0.0f, -2.f, -1.f,  1.f, 0.f, 0.f, 1.f,
            };
            clearBlue();
            drawClip(0x0004, crossW, 3);
            readPx(7, 32, pr, pg, pb, pa);
            readPx(4, 35, qr, qg, qb, qa);
            std::printf("  clip w<0 (7,32)=%u,%u,%u,%u (4,35)=%u,%u,%u,%u\n", pr, pg, pb, pa, qr, qg, qb, qa);
            check(pr == 0 && pb == 255 && qr == 255 && qg == 0 && qb == 0 && qa == 255,
                  "GLES2 clips a triangle crossing the camera plane (w<0) instead of mirroring it through the origin");

            const float crossNear[] = {
                -0.5f, -0.5f,  0.f, 1.f,  1.f, 0.f, 0.f, 1.f,
                 0.5f, -0.5f,  0.f, 1.f,  1.f, 0.f, 0.f, 1.f,
                 0.0f,  0.5f, -2.f, 1.f,  1.f, 0.f, 0.f, 1.f,
            };
            clearBlue();
            drawClip(0x0004, crossNear, 3);
            readPx(32, 36, pr, pg, pb, pa);
            readPx(33, 23, qr, qg, qb, qa);
            std::printf("  clip z<-w (32,36)=%u,%u,%u,%u (33,23)=%u,%u,%u,%u\n", pr, pg, pb, pa, qr, qg, qb, qa);
            check(pr == 0 && pb == 255 && qr == 255 && qg == 0 && qb == 0,
                  "GLES2 clips a triangle crossing the near plane (z<-w, w>0) and keeps the visible fragment");

            const float behindTri[] = {
                 0.5f,  0.5f, 0.f, -1.f,  1.f, 0.f, 0.f, 1.f,
                -0.5f,  0.5f, 0.f, -1.f,  1.f, 0.f, 0.f, 1.f,
                 0.0f, -0.5f, 0.f, -1.f,  1.f, 0.f, 0.f, 1.f,
            };
            clearBlue();
            drawClip(0x0004, behindTri, 3);
            readPx(32, 32, pr, pg, pb, pa);
            std::printf("  clip all behind (32,32)=%u,%u,%u,%u\n", pr, pg, pb, pa);
            check(pr == 0 && pg == 0 && pb == 255 && pa == 255,
                  "GLES2 drops a triangle whose vertices are all behind the camera (w<0)");

            const float crossLine[] = {
                0.f, 0.f,  0.f,  1.f,  1.f, 0.f, 0.f, 1.f,
                0.5f, 0.f, -2.f, -1.f,  1.f, 0.f, 0.f, 1.f,
            };
            clearBlue();
            drawClip(0x0001 /* GL_LINES */, crossLine, 2);
            readPx(20, 32, pr, pg, pb, pa);
            readPx(36, 32, qr, qg, qb, qa);
            std::printf("  clip line (20,32)=%u,%u,%u,%u (36,32)=%u,%u,%u,%u\n", pr, pg, pb, pa, qr, qg, qb, qa);
            check(pr == 0 && pb == 255 && qr == 255 && qg == 0 && qb == 0,
                  "GLES2 clips a line crossing the camera plane to the near-plane segment");

            const float pts[] = {
                 0.5f, 0.f,  0.f, -1.f,  1.f, 0.f, 0.f, 1.f,
                 0.f,  0.5f, -2.f,  1.f,  1.f, 0.f, 0.f, 1.f,
                 0.f,  0.f,   0.f,  1.f,  1.f, 0.f, 0.f, 1.f,
            };
            clearBlue();
            drawClip(0x0000 /* GL_POINTS */, pts, 3);
            readPx(16, 32, pr, pg, pb, pa);
            readPx(32, 48, qr, qg, qb, qa);
            uint8_t rr = 0, rg = 0, rb = 0, ra = 0;
            readPx(32, 32, rr, rg, rb, ra);
            std::printf("  clip points behind=%u,%u,%u,%u near=%u,%u,%u,%u inside=%u,%u,%u,%u\n",
                        pr, pg, pb, pa, qr, qg, qb, qa, rr, rg, rb, ra);
            check(pr == 0 && pb == 255 && qr == 0 && qb == 255 && rr == 255 && rg == 0 && rb == 0,
                  "GLES2 discards points with w<0 or z<-w and still draws a point in front of the near plane");

            // Different w on every vertex. Equal w makes screen-linear and
            // perspective-correct interpolation the same color, so this lock
            // only holds if the fragment stage divides by the clipped 1/w.
            const float attrTri[] = {
                -0.5f, -0.5f,  0.f, 1.f,  1.f, 0.f, 0.f, 1.f,
                 2.0f, -2.0f,  0.f, 4.f,  0.f, 1.f, 0.f, 1.f,
                 0.0f,  0.5f, -2.f, 1.f,  0.f, 0.f, 1.f, 1.f,
            };
            clearBlue();
            drawClip(0x0004, attrTri, 3);
            readPx(38, 18, pr, pg, pb, pa);
            readPx(50, 0, qr, qg, qb, qa);
            std::printf("  clip attrib (38,18)=%u,%u,%u,%u (50,0)=%u,%u,%u,%u\n", pr, pg, pb, pa, qr, qg, qb, qa);
            check(closeTo(pr, pg, pb, 131, 84, 40) && pa == 255 && qr == 0 && qb == 255,
                  "GLES2 perspective-correct varyings use a different w on each vertex, including the near-plane edge");

            // w=1, w=1.5, and a vertex through the w guard. The guard lerp
            // undershoots 1e-6 in float32 (about 9.54e-7). Rejecting that
            // output drops the visible triangle (about 2587 of 4096 pixels
            // before the side planes). Side planes then keep NDC inside the
            // viewport, so the unbounded projection's extra pixels stay clear.
            const uint32_t fb = proc.heap().alloc(64 * 64 * 4);
            auto countCovered = [&]() {
                callGL("_glReadPixels", {0, 0, 64, 64, 0x1908, 0x1401, fb});
                const uint8_t* p = proc.mem().host(fb);
                int n = 0;
                for (int i = 0; i < 64 * 64; ++i)
                    if (p[i * 4] >= 250 && p[i * 4 + 2] <= 5) ++n;
                return n;
            };
            const float guardTri[] = {
                -0.8f,  0.2f, 0.f,  1.f,    1.f, 0.f, 0.f, 1.f,
                 0.9f, -0.9f, 0.f,  1.5f,   1.f, 0.f, 0.f, 1.f,
                 0.25f, 0.25f, 0.f, -1e-4f, 1.f, 0.f, 0.f, 1.f,
            };
            clearBlue();
            drawClip(0x0004, guardTri, 3);
            readPx(20, 41, pr, pg, pb, pa);
            readPx(8, 13, qr, qg, qb, qa);
            const int guardCov = countCovered();
            std::printf("  clip guard tri (20,41)=%u,%u,%u,%u (8,13)=%u,%u,%u,%u covered=%d\n",
                        pr, pg, pb, pa, qr, qg, qb, qa, guardCov);
            check(pr == 255 && pg == 0 && pb == 0 && pa == 255 &&
                  qr == 0 && qb == 255 && guardCov >= 1900 && guardCov <= 2100,
                  "GLES2 keeps a w=1/1.5/through-guard triangle whose clip boundary undershoots 1e-6, and side planes bound it to the viewport");

            // The guard hit stays inside the side planes, so its w remains
            // ~9.54e-7 after every plane. The segment must still be drawn.
            const float guardSeg[] = {
                -0.6f, -0.4f, 0.f, 1.5f,  1.f, 0.f, 0.f, 1.f,
                 1e-6f, 5e-7f, 0.f, 5e-7f, 1.f, 0.f, 0.f, 1.f,
            };
            clearBlue();
            drawClip(0x0001, guardSeg, 2);
            readPx(40, 34, pr, pg, pb, pa);
            readPx(0, 0, qr, qg, qb, qa);
            std::printf("  clip guard seg (40,34)=%u,%u,%u,%u (0,0)=%u,%u,%u,%u\n",
                        pr, pg, pb, pa, qr, qg, qb, qa);
            check(pr == 255 && pg == 0 && pb == 0 && qr == 0 && qb == 255,
                  "GLES2 draws a segment whose clipped endpoint is the float32 guard boundary (~9.54e-7)");

            // clip x = 1e10, w = 1. The screen delta exceeds INT_MAX. Side
            // planes bring NDC x to 1, and the walker must not convert that
            // unclipped delta to int.
            const float hugeLine[] = {
                0.f, 0.f, 0.f, 1.f,     1.f, 0.f, 0.f, 1.f,
                1e10f, 0.f, 0.f, 1.f,   1.f, 0.f, 0.f, 1.f,
            };
            clearBlue();
            drawClip(0x0001, hugeLine, 2);
            readPx(48, 32, pr, pg, pb, pa);
            readPx(63, 32, qr, qg, qb, qa);
            uint8_t sr = 0, sg = 0, sb = 0, sa = 0;
            readPx(0, 32, sr, sg, sb, sa);
            std::printf("  clip huge line (48,32)=%u,%u,%u,%u (63,32)=%u,%u,%u,%u (0,32)=%u,%u,%u,%u\n",
                        pr, pg, pb, pa, qr, qg, qb, qa, sr, sg, sb, sa);
            check(pr == 255 && pg == 0 && pb == 0 && qr == 255 && qg == 0 && qb == 0 && sr == 0 && sb == 255,
                  "GLES2 clips a line whose screen delta exceeds INT_MAX to the framebuffer and still draws the visible pixels");
        }

        // 2f11. OpenGL ES occlusion queries state machine
        {
            const uint32_t ids = proc.heap().allocZeroed(8);

            // Gen queries
            callGL("_glGenQueriesEXT", {2, ids});
            const uint32_t q1 = proc.mem().read<uint32_t>(ids);
            const uint32_t q2 = proc.mem().read<uint32_t>(ids + 4);

            check(q1 > 0 && q2 > q1, "glGenQueriesEXT produces monotonic query ids");

            // Check isQuery before begin
            const bool isq1_pre = callGL("_glIsQueryEXT", {q1}) != 0;
            const bool isq2_pre = callGL("_glIsQueryEXT", {q2}) != 0;
            check(!isq1_pre && !isq2_pre, "glIsQueryEXT is false for reserved but unbegun query names");

            // Invalid target begin checking
            callGL("_glBeginQueryEXT", {0x8866, q1});
            const uint32_t err1 = callGL("_glGetError", {});

            // Begin query
            callGL("_glBeginQueryEXT", {0x8C2F, q1});
            const uint32_t err2 = callGL("_glGetError", {});

            check(err1 == 0x0500 /* GL_INVALID_ENUM */ && err2 == 0, "glBeginQueryEXT only accepts occlusion query targets (GL_ANY_SAMPLES_PASSED_EXT)");

            // Check isQuery after begin
            const bool isq1_post = callGL("_glIsQueryEXT", {q1}) != 0;
            const bool isq2_post = callGL("_glIsQueryEXT", {q2}) != 0;
            const bool isq3 = callGL("_glIsQueryEXT", {q2 + 1}) != 0;
            check(isq1_post && !isq2_post && !isq3, "glIsQueryEXT is true only after glBeginQueryEXT");

            // Get query object params on active query (should fail/ignore)
            const uint32_t params = proc.heap().allocZeroed(4);
            proc.mem().write<uint32_t>(params, 0xFFFFFFFF);
            callGL("_glGetQueryObjectuivEXT", {q1, 0x8866, params}); // GL_QUERY_RESULT
            const uint32_t res_active = proc.mem().read<uint32_t>(params);

            // End query
            callGL("_glEndQueryEXT", {0x8C2F});

            // Get query object params after end
            proc.mem().write<uint32_t>(params, 0xFFFFFFFF);
            callGL("_glGetQueryObjectuivEXT", {q1, 0x8867, params}); // GL_QUERY_RESULT_AVAILABLE
            const uint32_t avail = proc.mem().read<uint32_t>(params);
            proc.mem().write<uint32_t>(params, 0xFFFFFFFF);
            callGL("_glGetQueryObjectuivEXT", {q1, 0x8866, params}); // GL_QUERY_RESULT
            const uint32_t res = proc.mem().read<uint32_t>(params);

            proc.mem().write<uint32_t>(params, 0xFFFFFFFF);
            callGL("_glGetQueryObjectuivEXT", {q2, 0x8866, params}); // Should not modify params (unbegun)
            const uint32_t res_unbegun = proc.mem().read<uint32_t>(params);

            check(res_active == 0xFFFFFFFF && avail == 1 && res == 1 && res_unbegun == 0xFFFFFFFF, "glGetQueryObjectuivEXT ignores active and unbegun queries, writes 1 for completed");

            // Duplicate/invalid begin checking
            callGL("_glBeginQueryEXT", {0x8C2F, q2}); // valid begin
            // call begin again on same target with another ID (should fail)
            callGL("_glBeginQueryEXT", {0x8C2F, q1});

            // Cross-context check
            const ObjC::id ctx2 = ObjC::id(r.send(cpu, r.getClass("EAGLContext"), r.selAlloc));
            r.send(cpu, ctx2, "initWithAPI:sharegroup:", {2, uint32_t(r.send(cpu, glCtx, "sharegroup"))});
            r.send(cpu, r.getClass("EAGLContext"), "setCurrentContext:", {ctx2});

            const bool cross_ctx_q1 = callGL("_glIsQueryEXT", {q1}) != 0;
            const bool cross_ctx_q2 = callGL("_glIsQueryEXT", {q2}) != 0;
            check(!cross_ctx_q1 && !cross_ctx_q2, "glIsQueryEXT proves query objects are NOT shared between contexts");

            r.send(cpu, r.getClass("EAGLContext"), "setCurrentContext:", {glCtx});

            // Delete queries (while q2 is active)
            callGL("_glDeleteQueriesEXT", {2, ids});

            const bool disq1 = callGL("_glIsQueryEXT", {q1}) != 0;
            check(!disq1, "glDeleteQueriesEXT removes ids from state");

            // Deleting an active query keeps it bound until glEndQueryEXT, which then releases it.
            callGL("_glGetError", {});
            const uint32_t qp = proc.heap().allocZeroed(4);
            proc.mem().write<uint32_t>(qp, 0xFFFFFFFF);
            callGL("_glGetQueryivEXT", {0x8C2F, 0x8865, qp});   // GL_CURRENT_QUERY_EXT
            const uint32_t cur_before = proc.mem().read<uint32_t>(qp);
            const bool still_q2 = callGL("_glIsQueryEXT", {q2}) != 0;
            check(cur_before == q2 && !still_q2, "glDeleteQueriesEXT frees the name at once but leaves an active query bound (GL_CURRENT_QUERY_EXT reports it) until glEndQueryEXT");
            callGL("_glEndQueryEXT", {0x8C2F});
            const uint32_t endErr = callGL("_glGetError", {});
            proc.mem().write<uint32_t>(qp, 0xFFFFFFFF);
            callGL("_glGetQueryivEXT", {0x8C2F, 0x8865, qp});
            check(endErr == 0 && proc.mem().read<uint32_t>(qp) == 0 && callGL("_glIsQueryEXT", {q2}) == 0,
                  "glEndQueryEXT releases a query deleted while active");

            // Negative counts are GL_INVALID_VALUE and touch no guest memory.
            const uint32_t nid = proc.heap().allocZeroed(4);
            proc.mem().write<uint32_t>(nid, 0xAAAAAAAA);
            callGL("_glGenQueriesEXT", {uint32_t(-1), nid});
            const uint32_t negGen = callGL("_glGetError", {});
            callGL("_glDeleteQueriesEXT", {uint32_t(-1), nid});
            const uint32_t negDel = callGL("_glGetError", {});
            check(negGen == 0x0501 && negDel == 0x0501 && proc.mem().read<uint32_t>(nid) == 0xAAAAAAAA,
                  "glGenQueriesEXT/glDeleteQueriesEXT reject negative counts with GL_INVALID_VALUE");

            // One occlusion query at a time across both targets; an object keeps its first target.
            callGL("_glGenQueriesEXT", {2, ids});
            const uint32_t a1 = proc.mem().read<uint32_t>(ids), a2 = proc.mem().read<uint32_t>(ids + 4);
            callGL("_glBeginQueryEXT", {0x8C2F, a1});
            const uint32_t okBegin = callGL("_glGetError", {});
            callGL("_glBeginQueryEXT", {0x8D6A, a2});   // conservative target while any-samples is active
            const uint32_t crossErr = callGL("_glGetError", {});
            callGL("_glEndQueryEXT", {0x8C2F});
            callGL("_glGetError", {});
            callGL("_glBeginQueryEXT", {0x8D6A, a1});   // a1 was first begun on 0x8C2F
            const uint32_t retargetErr = callGL("_glGetError", {});
            callGL("_glBeginQueryEXT", {0x8D6A, a2});
            const uint32_t okCons = callGL("_glGetError", {});
            proc.mem().write<uint32_t>(qp, 0);
            callGL("_glGetQueryivEXT", {0x8D6A, 0x8865, qp});
            const uint32_t consCur = proc.mem().read<uint32_t>(qp);
            callGL("_glEndQueryEXT", {0x8D6A});
            check(okBegin == 0 && crossErr == 0x0502 && retargetErr == 0x0502 && okCons == 0 && consCur == a2,
                  "glBeginQueryEXT allows one active occlusion query across both targets and rejects re-targeting an existing object");

            // Enum validation in glGetQueryivEXT / glGetQueryObjectuivEXT.
            callGL("_glGetError", {});
            callGL("_glGetQueryivEXT", {0x8C2F, 0x1234, qp});
            const uint32_t badPname = callGL("_glGetError", {});
            callGL("_glGetQueryivEXT", {0x1234, 0x8865, qp});
            const uint32_t badTarget = callGL("_glGetError", {});
            proc.mem().write<uint32_t>(qp, 0xFFFFFFFF);
            callGL("_glGetQueryObjectuivEXT", {a1, 0x1234, qp});
            const uint32_t badObjPname = callGL("_glGetError", {});
            check(badPname == 0x0500 && badTarget == 0x0500 && badObjPname == 0x0500 && proc.mem().read<uint32_t>(qp) == 0xFFFFFFFF,
                  "glGetQueryivEXT/glGetQueryObjectuivEXT reject unsupported targets and pnames with GL_INVALID_ENUM");

            // Query name 0 is never valid.
            callGL("_glGetError", {});
            callGL("_glBeginQueryEXT", {0x8C2F, 0});
            const uint32_t zeroErr = callGL("_glGetError", {});
            proc.mem().write<uint32_t>(qp, 0xFFFFFFFF);
            callGL("_glGetQueryivEXT", {0x8C2F, 0x8865, qp});
            check(zeroErr == 0x0502 && proc.mem().read<uint32_t>(qp) == 0, "glBeginQueryEXT with query name 0 is GL_INVALID_OPERATION and starts nothing");
        }

        const bool presOk = (r.send(cpu, glCtx, "presentRenderbuffer:", {0x8D41}) & 0xFF) != 0;
        UIKit::compositeFrame(cx);
        check(presOk && !UIKit::ui(proc).framebuffer.rgba.empty(), "CAEAGLLayer presentRenderbuffer composites into UIKit framebuffer");
    }

    // 2f8. OpenAL spatial queries and HVMCFRawDictionary mutators
    {
        auto call_tmp = [&](const char* sym, std::initializer_list<uint32_t> args) -> uint64_t {
            auto addr = proc.lookupSymbol(sym);
            return proc.callGuest(cpu, *addr, args);
        };
        auto cstr2 = [&](const char* s) -> uint32_t { return proc.allocCString(s); };

        const uint32_t dev = uint32_t(call_tmp("_alcOpenDevice", {0}));
        const uint32_t actx = uint32_t(call_tmp("_alcCreateContext", {dev, 0}));
        call_tmp("_alcMakeContextCurrent", {actx});
        const uint32_t alIds = proc.heap().allocZeroed(16);
        call_tmp("_alGenSources", {1, alIds});
        const uint32_t alSrc = proc.mem().read<uint32_t>(alIds);

        const uint32_t f = proc.heap().allocZeroed(12);
        call_tmp("_alGetSource3f", {alSrc, 0x1004 /* AL_POSITION */, f, f+4, f+8});
        bool okPosDefault = proc.mem().read<float>(f) == 0.0f && proc.mem().read<float>(f+4) == 0.0f && proc.mem().read<float>(f+8) == 0.0f;

        call_tmp("_alSource3f", {alSrc, 0x1004 /* AL_POSITION */, 0x3f800000 /* 1.0f */, 0x40000000 /* 2.0f */, 0x40400000 /* 3.0f */});
        call_tmp("_alGetSource3f", {alSrc, 0x1004 /* AL_POSITION */, f, f+4, f+8});
        bool okPosSet = proc.mem().read<float>(f) == 1.0f && proc.mem().read<float>(f+4) == 2.0f && proc.mem().read<float>(f+8) == 3.0f;

        call_tmp("_alSource3f", {alSrc, 0x1006 /* AL_VELOCITY */, 0x40800000 /* 4.0f */, 0x40a00000 /* 5.0f */, 0x40c00000 /* 6.0f */});
        call_tmp("_alGetSource3f", {alSrc, 0x1006 /* AL_VELOCITY */, f, f+4, f+8});
        bool okVelSet = proc.mem().read<float>(f) == 4.0f && proc.mem().read<float>(f+4) == 5.0f && proc.mem().read<float>(f+8) == 6.0f;

        call_tmp("_alSource3f", {alSrc, 0x1005 /* AL_DIRECTION */, 0x40e00000 /* 7.0f */, 0x41000000 /* 8.0f */, 0x41100000 /* 9.0f */});
        call_tmp("_alGetSource3f", {alSrc, 0x1005 /* AL_DIRECTION */, f, f+4, f+8});
        bool okDirSet = proc.mem().read<float>(f) == 7.0f && proc.mem().read<float>(f+4) == 8.0f && proc.mem().read<float>(f+8) == 9.0f;

        const uint32_t i = proc.heap().allocZeroed(4);
        call_tmp("_alcGetIntegerv", {dev, 0x1000 /* ALC_MAJOR_VERSION */, 1, i});
        bool okAlcMajor = proc.mem().read<int32_t>(i) == 1;
        call_tmp("_alcGetIntegerv", {dev, 0x1002 /* ALC_ATTRIBUTES_SIZE */, 1, i});
        bool okAlcAttr = proc.mem().read<int32_t>(i) == 0;
        call_tmp("_alcGetIntegerv", {dev + 1, 0x1000, 1, i});
        const bool badDevice = call_tmp("_alcGetError", {dev}) == 0xA001 && call_tmp("_alcGetError", {dev}) == 0;
        call_tmp("_alcGetIntegerv", {dev, 0x1000, 0, i});
        const bool badSize = call_tmp("_alcGetError", {dev}) == 0xA004;
        proc.mem().write<int32_t>(i, 77);
        call_tmp("_alcGetIntegerv", {dev, 0x1000, uint32_t(-1), i});
        const bool negativeSize = call_tmp("_alcGetError", {dev}) == 0xA004 && proc.mem().read<int32_t>(i) == 77;   // rejected, nothing written
        proc.mem().write<int32_t>(i, 77);
        call_tmp("_alcGetIntegerv", {0, 0x1001 /* ALC_MINOR_VERSION */, 1, i});
        const bool nullDeviceVersion = proc.mem().read<int32_t>(i) == 1 && call_tmp("_alcGetError", {dev}) == 0;
        call_tmp("_alcGetIntegerv", {0, 0x1002 /* ALC_ATTRIBUTES_SIZE */, 1, i});
        const bool nullDeviceOther = call_tmp("_alcGetError", {dev}) == 0xA001;
        call_tmp("_alcGetIntegerv", {dev, 0xFFFF, 1, i});
        const bool badEnum = call_tmp("_alcGetError", {dev}) == 0xA003;
        call_tmp("_alGetSource3f", {alSrc + 123, 0x1004, f, f+4, f+8});
        const bool badSource = call_tmp("_alGetError", {}) == 0xA001;
        call_tmp("_alGetSource3f", {alSrc, 0xFFFF, f, f+4, f+8});
        const bool badSourceEnum = call_tmp("_alGetError", {}) == 0xA002;
        const uint32_t nullOutErrors = [&] {
            uint32_t ok = 1;
            for (const auto& args : {std::initializer_list<uint32_t>{alSrc, 0x1004, 0, f+4, f+8}, {alSrc, 0x1004, f, 0, f+8}, {alSrc, 0x1004, f, f+4, 0}}) {
                call_tmp("_alGetSource3f", args);
                ok &= call_tmp("_alGetError", {}) == 0xA003 ? 1u : 0u;   // AL_INVALID_VALUE
            }
            return ok;
        }();

        const uint32_t d = uint32_t(call_tmp("_CFDictionaryCreateMutable", {0, 0, 0, 0}));
        const uint32_t k = cstr2("key"), v = cstr2("value");
        call_tmp("_objc_msgSend", {d, r.sel("setObject:forKey:"), v, k});
        uint32_t ret = uint32_t(call_tmp("_objc_msgSend", {d, r.sel("objectForKey:"), k}));
        bool okDictMut = ret == v;
        const uint32_t v2 = cstr2("replacement");
        call_tmp("_objc_msgSend", {d, r.sel("setObject:forKey:"), v2, k});
        const bool okRawOverwrite = uint32_t(call_tmp("_objc_msgSend", {d, r.sel("objectForKey:"), k})) == v2 &&
            uint32_t(call_tmp("_objc_msgSend", {d, r.sel("count")})) == 1;

        call_tmp("_objc_msgSend", {d, r.sel("removeObjectForKey:"), k});
        uint32_t ret2 = uint32_t(call_tmp("_objc_msgSend", {d, r.sel("objectForKey:"), k}));
        bool okDictRm = ret2 == 0;

        const uint32_t idict = uint32_t(call_tmp("_CFDictionaryCreate", {0, 0, 0, 0, 0, 0}));
        bool immutableRejected = false;
        try { call_tmp("_objc_msgSend", {idict, r.sel("setObject:forKey:"), v, k}); }
        catch (const CPU::GuestFault&) { immutableRejected = true; }
        catch (const ObjC::ObjCError&) { immutableRejected = true; }
        bool removeRejected = false, removeAllRejected = false;
        try { call_tmp("_objc_msgSend", {idict, r.sel("removeObjectForKey:"), k}); }
        catch (const CPU::GuestFault&) { removeRejected = true; }
        catch (const ObjC::ObjCError&) { removeRejected = true; }
        try { call_tmp("_objc_msgSend", {idict, r.sel("removeAllObjects")}); }
        catch (const CPU::GuestFault&) { removeAllRejected = true; }
        catch (const ObjC::ObjCError&) { removeAllRejected = true; }
        uint32_t ret3 = uint32_t(call_tmp("_objc_msgSend", {idict, r.sel("objectForKey:"), k}));
        bool okIDictMut = immutableRejected && removeRejected && removeAllRejected && ret3 == 0;

        call_tmp("_objc_msgSend", {d, r.sel("setObject:forKey:"), v, k});
        call_tmp("_objc_msgSend", {d, r.sel("removeAllObjects")});
        uint32_t ret4 = uint32_t(call_tmp("_objc_msgSend", {d, r.sel("objectForKey:"), k}));
        bool okDictRmAll = ret4 == 0;

        call_tmp("_objc_msgSend", {d, r.sel("setObject:forKey:"), v, k});
        bool rawEnumRejected = false, rawArrayRejected = false;
        try { call_tmp("_objc_msgSend", {d, r.sel("keyEnumerator")}); }
        catch (const CPU::GuestFault&) { rawEnumRejected = true; }
        catch (const ObjC::ObjCError&) { rawEnumRejected = true; }
        try { call_tmp("_objc_msgSend", {d, r.sel("allKeys")}); }
        catch (const CPU::GuestFault&) { rawArrayRejected = true; }
        catch (const ObjC::ObjCError&) { rawArrayRejected = true; }

        // Object-key callbacks allow safe Objective-C enumeration even when
        // values are raw pointers; zero callbacks must never retain raw keys.
        const uint32_t keyCallbacks = proc.heap().allocZeroed(24);
        proc.mem().write<uint32_t>(keyCallbacks + 4, 1);
        const uint32_t objectKeyDict = uint32_t(call_tmp("_CFDictionaryCreateMutable", {0, 0, keyCallbacks, 0}));
        const ObjC::id objcKey = Foundation::makeString(cx, "objc-key");
        const uint32_t beforeRetain = uint32_t(r.send(cpu, objcKey, "retainCount"));
        call_tmp("_objc_msgSend", {objectKeyDict, r.sel("setObject:forKey:"), v, objcKey});
        const uint32_t heldRetain = uint32_t(r.send(cpu, objcKey, "retainCount"));
        const uint32_t keys = uint32_t(call_tmp("_objc_msgSend", {objectKeyDict, r.sel("allKeys")}));
        const bool objectKeysSafe = uint32_t(call_tmp("_objc_msgSend", {keys, r.sel("count")})) == 1 &&
            uint32_t(call_tmp("_objc_msgSend", {keys, r.sel("objectAtIndex:"), 0})) == objcKey;
        call_tmp("_objc_msgSend", {objectKeyDict, r.sel("removeAllObjects")});
        const uint32_t afterRetain = uint32_t(r.send(cpu, objcKey, "retainCount"));
        // allKeys retains its elements. Removing the dictionary's entry must
        // drop exactly one retain while the returned array still owns the key.
        const bool keyBalanced = heldRetain == beforeRetain + 1 && afterRetain == heldRetain;
        // Enumerate after the retain accounting above: the enumerator keeps its own reference to the key.
        call_tmp("_objc_msgSend", {objectKeyDict, r.sel("setObject:forKey:"), v, objcKey});
        const uint32_t keyEnum = uint32_t(call_tmp("_objc_msgSend", {objectKeyDict, r.sel("keyEnumerator")}));
        const bool keyEnumOk = keyEnum != 0 && uint32_t(call_tmp("_objc_msgSend", {keyEnum, r.sel("nextObject")})) == objcKey &&
            uint32_t(call_tmp("_objc_msgSend", {keyEnum, r.sel("nextObject")})) == 0;   // one key, then exhausted
        call_tmp("_objc_msgSend", {objectKeyDict, r.sel("removeAllObjects")});

        // Value-only object callbacks: an overwrite releases the old value,
        // and removal releases the replacement without retaining a raw key.
        const uint32_t valCallbacks = proc.heap().allocZeroed(24);
        proc.mem().write<uint32_t>(valCallbacks + 4, 1);
        const uint32_t valueDict = uint32_t(call_tmp("_CFDictionaryCreateMutable", {0, 0, 0, valCallbacks}));
        const ObjC::id valueA = Foundation::makeString(cx, "value-A");
        const ObjC::id valueB = Foundation::makeString(cx, "value-B");
        const uint32_t a0 = uint32_t(r.send(cpu, valueA, "retainCount"));
        const uint32_t b0 = uint32_t(r.send(cpu, valueB, "retainCount"));
        call_tmp("_objc_msgSend", {valueDict, r.sel("setObject:forKey:"), valueA, k});
        const bool firstRetain = uint32_t(r.send(cpu, valueA, "retainCount")) == a0 + 1;
        call_tmp("_objc_msgSend", {valueDict, r.sel("setObject:forKey:"), valueB, k});
        const bool overwriteRetain = uint32_t(r.send(cpu, valueA, "retainCount")) == a0 &&
            uint32_t(r.send(cpu, valueB, "retainCount")) == b0 + 1;
        call_tmp("_objc_msgSend", {valueDict, r.sel("removeObjectForKey:"), k});
        const bool removedRetain = uint32_t(r.send(cpu, valueB, "retainCount")) == b0;
        call_tmp("_objc_msgSend", {valueDict, r.sel("setObject:forKey:"), valueB, k});
        call_tmp("_objc_msgSend", {valueDict, r.sel("removeAllObjects")});
        const bool clearRetain = uint32_t(r.send(cpu, valueB, "retainCount")) == b0;
        // Releasing a non-empty dictionary (rd_dealloc) gives back the value retains it still holds.
        call_tmp("_objc_msgSend", {valueDict, r.sel("setObject:forKey:"), valueB, k});
        const bool heldBeforeRelease = uint32_t(r.send(cpu, valueB, "retainCount")) == b0 + 1;
        call_tmp("_CFRelease", {valueDict});
        const bool deallocRetain = heldBeforeRelease && uint32_t(r.send(cpu, valueB, "retainCount")) == b0;

        check(okPosDefault && okPosSet && okVelSet && okDirSet && okAlcMajor && okAlcAttr && badDevice && badSize && negativeSize && nullOutErrors && nullDeviceVersion && nullDeviceOther && badEnum && badSource && badSourceEnum,
              "OpenAL valid queries, invalid source/device/enum/size errors");
        check(okDictMut && okRawOverwrite && okDictRm && okIDictMut && okDictRmAll && rawEnumRejected && rawArrayRejected,
              "raw CFDictionary mutation, immutable rejection, unsafe key enumeration");
        check(objectKeysSafe && keyEnumOk, "CFDictionary object-key callback supports safe ObjC enumeration");
        check(keyBalanced, "CFDictionary object-key callback balances retain/release");
        check(firstRetain && overwriteRetain && removedRetain && clearRetain && deallocRetain,
              "CFDictionary object-value callbacks balance overwrite/remove/clear ownership");
    }

    // 2f2. libstdc++ containers (GCC 4.2 ABI): red-black tree, COW std::string
    {
        auto call = [&](const char* sym, std::initializer_list<uint32_t> args) -> uint32_t {
            auto a = proc.lookupSymbol(sym);
            return a ? uint32_t(proc.callGuest(cpu, *a, args)) : 0xDEADu;
        };
        auto& m = proc.mem();
        // __gnu_cxx::__exchange_and_add (both GCC 4.2 symbols): returns the previous value, wraps at 32 bits,
        // and concurrent adds from several guest threads are not lost.
        {
            const uint32_t word = proc.heap().allocZeroed(4);
            for (const char* sym : {"__ZN9__gnu_cxx18__exchange_and_addEPVii", "__ZN9__gnu_cxx27__exchange_and_add_dispatchEPii"}) {
                m.write<uint32_t>(word, 5);
                const uint32_t prev = call(sym, {word, 3});
                const uint32_t afterAdd = m.read<uint32_t>(word);
                m.write<uint32_t>(word, 0x7FFFFFFFu);
                const uint32_t prevWrap = call(sym, {word, 1});
                const uint32_t wrapped = m.read<uint32_t>(word);
                m.write<uint32_t>(word, 2);
                const uint32_t prevNeg = call(sym, {word, uint32_t(-5)});
                const std::string what = std::string(sym) + " returns the previous value and wraps at 32 bits";
                check(prev == 5 && afterAdd == 8 && prevWrap == 0x7FFFFFFFu && wrapped == 0x80000000u &&
                      prevNeg == 2 && m.read<uint32_t>(word) == uint32_t(-3),
                      what.c_str());
            }
            m.write<uint32_t>(word, 0);
            const uint32_t addAddr = *proc.lookupSymbol("__ZN9__gnu_cxx18__exchange_and_addEPVii");
            const uint32_t worker = proc.defineFunction("test_exchange_and_add_worker", [word, addAddr](Ctx& x) {
                for (int i = 0; i < 2000; ++i) x.proc.callGuest(x.cpu, addAddr, {word, 1});
                x.cpu.r[0] = 0;
            });
            uint32_t threads[4];
            for (uint32_t& t : threads) t = proc.startThread(worker, 0, 64u << 10, false);
            bool joined = true;
            for (uint32_t t : threads) { uint32_t rv = 0; joined = proc.joinThread(t, &rv) && joined; }
            check(joined && m.read<uint32_t>(word) == 8000, "__exchange_and_add loses no update across four concurrent guest threads");
        }
        // Tree: header + nodes {color, parent, left, right, key}.
        const uint32_t header = proc.heap().allocZeroed(16);
        m.write<uint32_t>(header + 8, header);   // leftmost
        m.write<uint32_t>(header + 12, header);  // rightmost
        std::map<uint32_t, uint32_t> model;      // key -> node
        std::mt19937 rng(1234);
        bool treeOk = true;
        auto key = [&](uint32_t n) { return m.read<uint32_t>(n + 16); };
        auto verify = [&]() {
            // In-order walk via _Rb_tree_increment matches the model; RB invariants hold.
            std::vector<uint32_t> walk;
            for (uint32_t n = m.read<uint32_t>(header + 8); n != header && walk.size() <= model.size(); n = call("__ZSt18_Rb_tree_incrementPSt18_Rb_tree_node_base", {n}))
                walk.push_back(key(n));
            std::vector<uint32_t> want;
            for (auto& [k, n] : model) want.push_back(k);
            if (walk != want) return false;
            const uint32_t root = m.read<uint32_t>(header + 4);
            if (root && m.read<uint32_t>(root) != 1) return false;   // black root
            std::function<int(uint32_t)> blackHeight = [&](uint32_t n) -> int {
                if (!n) return 1;
                const uint32_t l = m.read<uint32_t>(n + 8), r = m.read<uint32_t>(n + 12);
                if (m.read<uint32_t>(n) == 0 && ((l && m.read<uint32_t>(l) == 0) || (r && m.read<uint32_t>(r) == 0))) return -1;   // red-red
                if ((l && m.read<uint32_t>(l + 4) != n) || (r && m.read<uint32_t>(r + 4) != n)) return -1;   // parent links
                const int a = blackHeight(l), b = blackHeight(r);
                if (a < 0 || b < 0 || a != b) return -1;
                return a + (m.read<uint32_t>(n) == 1 ? 1 : 0);
            };
            if (blackHeight(root) < 0) return false;
            if (!model.empty() && (m.read<uint32_t>(header + 8) != model.begin()->second || m.read<uint32_t>(header + 12) != model.rbegin()->second)) return false;
            return true;
        };
        for (int step = 0; step < 600 && treeOk; ++step) {
            const uint32_t k = rng() % 200;
            if (!model.count(k) && (step < 300 || rng() % 2)) {
                // std::map-style insert position: descend from the root.
                uint32_t parent = header, cur = m.read<uint32_t>(header + 4);
                bool left = true;
                while (cur) { parent = cur; left = k < key(cur); cur = m.read<uint32_t>(cur + (left ? 8 : 12)); }
                const uint32_t node = proc.heap().allocZeroed(20);
                m.write<uint32_t>(node + 16, k);
                call("__ZSt29_Rb_tree_insert_and_rebalancebPSt18_Rb_tree_node_baseS0_RS_", {uint32_t(left || parent == header), node, parent, header});
                model[k] = node;
            } else if (model.count(k)) {
                const uint32_t node = model[k];
                const uint32_t victim = call("__ZSt28_Rb_tree_rebalance_for_erasePSt18_Rb_tree_node_baseRS_", {node, header});
                treeOk = victim == node;
                model.erase(k);
            }
            if (step % 25 == 0 || step == 599) treeOk = treeOk && verify();
        }
        const uint32_t last = model.empty() ? header : model.rbegin()->second;
        const uint32_t dec = call("__ZSt18_Rb_tree_decrementPSt18_Rb_tree_node_base", {header});
        check(treeOk && dec == last, "std::map red-black tree: 600 random inserts/erases keep order and RB invariants");

        // COW std::string
        auto cstr = [&](const char* s) { return proc.allocCString(s); };
        auto str = [&](uint32_t obj) { const uint32_t d = m.read<uint32_t>(obj); return std::string(reinterpret_cast<const char*>(m.host(d)), m.read<uint32_t>(d - 12)); };
        const uint32_t a = proc.heap().allocZeroed(4), b = proc.heap().allocZeroed(4), c = proc.heap().allocZeroed(4), sub = proc.heap().allocZeroed(4);
        call("__ZNSsC1EPKcRKSaIcE", {a, cstr("hello"), 0});
        call("__ZNSsC1ERKSs", {b, a});
        const bool shared = m.read<uint32_t>(a) == m.read<uint32_t>(b) && m.read<uint32_t>(m.read<uint32_t>(a) - 4) == 1;
        call("__ZNSs6appendEPKcm", {b, cstr(" world"), 6});
        call("__ZNSs6insertEmPKcm", {b, 0, cstr(">> "), 3});
        const uint32_t pos = call("__ZNKSs4findEPKcmm", {b, cstr("world"), 0, 5});
        call("__ZNKSs6substrEmm", {sub, b, pos, 0xFFFFFFFFu});
        call("__ZNSsC1EPKcRKSaIcE", {c, cstr("abc"), 0});
        const int32_t cmp = int32_t(call("__ZNKSs7compareERKSs", {c, a}));
        call("__ZNSs4swapERSs", {a, c});
        call("__ZNSs6resizeEm", {a, 2});
        const uint32_t emptyRep = call("__ZNSs4_Rep12_S_empty_repEv", {});
        const uint32_t emptyRc = m.read<uint32_t>(emptyRep + 8);
        const bool ok = shared && str(a) == "ab" && str(c) == "hello" && str(b) == ">> hello world" && pos == 9 && str(sub) == "world" && cmp < 0 && emptyRc == 0xFFFFFFFFu;
        for (uint32_t s : {a, b, c, sub}) call("__ZNSsD1Ev", {s});
        check(ok, "std::string (COW): sharing on copy, append, insert, find, substr, compare, swap, resize, emptyRep");

        // insert(pos, const char*) and iterator erase (Hungry Shark), the wchar_t _M_replace_aux and
        // iterator erase, and wcstombs (PvZ's XML parser).
        {
            const uint32_t t = proc.heap().allocZeroed(4);
            call("__ZNSsC1EPKcRKSaIcE", {t, cstr("shark"), 0});
            call("__ZNSs6insertEmPKc", {t, 0, cstr("hungry ")});
            const uint32_t d = m.read<uint32_t>(t);
            const uint32_t it = call("__ZNSs5eraseEN9__gnu_cxx17__normal_iteratorIPcSsEES2_", {t, d + 6, d + 7});
            const bool narrow = str(t) == "hungryshark" && it == m.read<uint32_t>(t) + 6;
            const uint32_t w = proc.heap().allocZeroed(4);
            m.write<uint32_t>(w, *proc.lookupSymbol("__ZNSbIwSt11char_traitsIwESaIwEE4_Rep20_S_empty_rep_storageE") + 12);
            call("__ZNSbIwSt11char_traitsIwESaIwEE14_M_replace_auxEmmmw", {w, 0, 0, 3, uint32_t('Z')});
            const uint32_t wd = m.read<uint32_t>(w);
            call("__ZNSbIwSt11char_traitsIwESaIwEE5eraseEN9__gnu_cxx17__normal_iteratorIPwS2_EES6_", {w, wd, wd + 4});
            const uint32_t wd2 = m.read<uint32_t>(w);
            const bool wide = m.read<uint32_t>(wd2 - 12) == 2 && m.read<uint32_t>(wd2) == 'Z' && m.read<uint32_t>(wd2 + 4) == 'Z';
            const uint32_t ws = proc.heap().allocZeroed(16), out = proc.heap().allocZeroed(8);
            for (int i = 0; i < 3; ++i) m.write<uint32_t>(ws + i * 4, uint32_t("xml"[i]));
            const uint32_t n = call("_wcstombs", {out, ws, 8});
            const bool conv = n == 3 && proc.readCString(out) == "xml" && call("_wcstombs", {0, ws, 0}) == 3;
            if (!(narrow && wide && conv)) std::printf("    string extras: narrow '%s' %d wide %d wcstombs %u\n", str(t).c_str(), narrow, wide, n);
            check(narrow && wide && conv, "libstdc++ insert(pos, char*), iterator erase, wstring _M_replace_aux/erase and wcstombs");
        }

        // Out-of-line members some games import (Army of Darkness Defense).
        {
            const uint32_t t = proc.heap().allocZeroed(4);
            call("__ZNSsC1EPKcRKSaIcE", {t, cstr("abcdef"), 0});
            call("__ZNSs7replaceEmmPKc", {t, 1, 2, cstr("XYZ")});
            const bool replaced = str(t) == "aXYZdef";
            const uint32_t sret = proc.heap().allocZeroed(4);
            call("__ZNSs6rbeginEv", {sret, t});
            const uint32_t rb = m.read<uint32_t>(sret);
            const bool rbeginOk = rb == m.read<uint32_t>(t) + 7;
            // _M_set_length_and_sharable on a private rep: length 3, terminator, refcount 0.
            const uint32_t rep = m.read<uint32_t>(t) - 12;
            call("__ZNSs4_Rep26_M_set_length_and_sharableEm", {rep, 3});
            const bool setLen = str(t) == "aXY" && m.read<uint8_t>(rep + 12 + 3) == 0 && m.read<uint32_t>(rep + 8) == 0;
            call("__ZNSs5clearEv", {t});
            const bool cleared = str(t).empty();
            const uint32_t hider = proc.heap().allocZeroed(4);
            call("__ZNSs12_Alloc_hiderC1EPcRKSaIcE", {hider, 0x1234, 0});
            call("__ZNSsD1Ev", {t});
            check(replaced && rbeginOk && setLen && cleared && m.read<uint32_t>(hider) == 0x1234,
                  "std::string out-of-line members: replace(pos,n,const char*), rbegin, _M_set_length_and_sharable, clear, _Alloc_hider");
        }

        // libstdc++ __basic_file<char> tests
        const uint32_t bfile = proc.heap().allocZeroed(16);
        call("__ZNSt12__basic_fileIcEC1EP15__gthread_mutex", {bfile, 0});
        const std::string tmpPath = proc.homePath() + "/basic_file_test.txt";
        const uint32_t cpath = cstr(tmpPath.c_str());
        const uint32_t opened = call("__ZNSt12__basic_fileIcE4openEPKcSt13_Ios_Openmodei", {bfile, cpath, 16 /* out */ | 32 /* trunc */, 0666});
        const bool isOpen = (call("__ZNKSt12__basic_fileIcE7is_openEv", {bfile}) & 0xFF) != 0;
        const int32_t fd = int32_t(call("__ZNKSt12__basic_fileIcE2fdEv", {bfile}));
        const uint32_t writeBuf = cstr("Hello basic_file!");
        const int32_t written = int32_t(call("__ZNSt12__basic_fileIcE6xsputnEPKci", {bfile, writeBuf, 17}));
        call("__ZNSt12__basic_fileIcE5closeEv", {bfile});
        const bool closed = !(call("__ZNKSt12__basic_fileIcE7is_openEv", {bfile}) & 0xFF);

        // Reopen for reading and seek
        call("__ZNSt12__basic_fileIcE4openEPKcSt13_Ios_Openmodei", {bfile, cpath, 8 /* in */, 0666});
        const uint32_t readBuf = proc.heap().allocZeroed(32);
        const int32_t readBytes = int32_t(call("__ZNSt12__basic_fileIcE6xsgetnEPci", {bfile, readBuf, 32}));
        const std::string readStr = proc.readCString(readBuf);
        call("__ZNSt12__basic_fileIcED1Ev", {bfile});

        const uint32_t ifstreamObj = proc.heap().allocZeroed(520);
        const uint32_t ifstreamVtt = proc.lookupSymbol("__ZTTSt14basic_ifstreamIcSt11char_traitsIcEE").value();
        call("__ZNSt14basic_ifstreamIcSt11char_traitsIcEEC2EPKcSt13_Ios_Openmode", {ifstreamObj, ifstreamVtt, cpath, 8});
        const uint32_t ifstreamReadBuf = proc.heap().allocZeroed(32);
        const uint32_t readCount = call("__ZNSi4readEPci", {ifstreamObj, ifstreamReadBuf, 17});
        const std::string ifstreamReadStr = proc.readCString(ifstreamReadBuf);
        call("__ZNSt14basic_ifstreamIcSt11char_traitsIcEED1Ev", {ifstreamObj});
        // `delete stream` on a heap ifstream goes through the vtable's deleting destructor
        // (Flight Control's file manager): the file closes and the block is freed.
        {
            const uint32_t heapStream = proc.heap().allocZeroed(520);
            call("__ZNSt14basic_ifstreamIcSt11char_traitsIcEEC1EPKcSt13_Ios_Openmode", {heapStream, cpath, 8});
            const uint32_t probe = proc.heap().allocZeroed(8);
            const bool readable = call("__ZNSi4readEPci", {heapStream, probe, 5}) == heapStream && proc.readCString(probe) == "Hello";
            const uint32_t deleting = proc.mem().read<uint32_t>(proc.mem().read<uint32_t>(heapStream) + 4);
            bool threw = false;
            try { proc.callGuest(cpu, deleting, {heapStream}); } catch (const std::exception&) { threw = true; }
            check(readable && !threw && proc.heap().blockSize(heapStream) == 0,
                  "libstdc++ ifstream: the virtual deleting destructor closes the file and frees the stream");
        }
        std::filesystem::remove(tmpPath);
        check(opened != 0 && isOpen && fd >= 0 && written == 17 && closed && readBytes == 17 && readStr == "Hello basic_file!" &&
              readCount == ifstreamObj && ifstreamReadStr == "Hello basic_file!",
              "libstdc++ __basic_file<char> and inline-constructed basic_ifstream<char> via C2+VTT");

        // std::bad_alloc
        const uint32_t badAllocObj = proc.heap().allocZeroed(32);
        proc.mem().write<uint32_t>(badAllocObj, proc.lookupSymbol("__ZTVSt9bad_alloc").value() + 8);
        const uint32_t baWhat = call("__ZNKSt9bad_alloc4whatEv", {badAllocObj});
        const std::string baWhatStr = proc.readCString(baWhat);
        call("__ZNSt9bad_allocD1Ev", {badAllocObj});
        check(baWhatStr == "std::bad_alloc", "std::bad_alloc::what() returns std::bad_alloc");
    }

    // 2f9. POSIX ftime, std::string::copy and SCNetworkReachability
    {
        auto call = [&](const char* sym, std::initializer_list<uint32_t> args) -> uint32_t {
            auto a = proc.lookupSymbol(sym);
            return a ? uint32_t(proc.callGuest(cpu, *a, args)) : 0xDEADu;
        };

        const uint32_t tp = proc.heap().allocZeroed(16);
        const uint32_t ret = call("_ftime", {tp});
        check(ret == 0 && proc.mem().read<uint32_t>(tp) > 0 && proc.mem().read<uint16_t>(tp + 4) < 1000,
              "ftime returns seconds and milliseconds");

        const uint32_t sObj = proc.heap().allocZeroed(4);
        call("__ZNSsC1EPKcRKSaIcE", {sObj, proc.allocCString("HydrogenVM"), 0}); // string("HydrogenVM")
        const uint32_t buf = proc.heap().allocZeroed(32);
        // Copy entire string
        const uint32_t count1 = call("__ZNKSs4copyEPcmm", {sObj, buf, 10, 0});
        check(count1 == 10 && std::strncmp((const char*)proc.mem().host(buf), "HydrogenVM", 10) == 0, "string::copy(buf, 10, 0) copies full string");
        // Partial copy and no null terminator written after copy (ensure buffer byte at 8 is unmodified zero since we allocZeroed)
        proc.mem().write<uint8_t>(buf + 8, 0x42);
        const uint32_t count2 = call("__ZNKSs4copyEPcmm", {sObj, buf, 8, 0});
        check(count2 == 8 && std::strncmp((const char*)proc.mem().host(buf), "Hydrogen", 8) == 0 && proc.mem().read<uint8_t>(buf + 8) == 0x42, "string::copy(buf, 8, 0) partial copy");
        // Nonzero starting position and exceeding string length
        const uint32_t count3 = call("__ZNKSs4copyEPcmm", {sObj, buf, 20, 8});
        check(count3 == 2 && std::strncmp((const char*)proc.mem().host(buf), "VM", 2) == 0, "string::copy(buf, 20, 8) handles pos > 0 and len > remaining");
        // Zero length copy
        const uint32_t count4 = call("__ZNKSs4copyEPcmm", {sObj, buf, 0, 0});
        check(count4 == 0, "string::copy(buf, 0, 0) zero copy");
        // Invalid pos raises logic error (which throws in host, halting the run context or just halting HLE execution)
        // (Testing exception from basic_string is covered by other libstdc++ tests or would abort guest; we just skip invoking it invalidly here).
        {
            // find_*_of(const char*, pos), (const string&, pos) and (char, pos): ROBLOX imports them.
            const uint32_t set = proc.heap().allocZeroed(4);
            call("__ZNSsC1EPKcRKSaIcE", {set, proc.allocCString("gV"), 0});
            const uint32_t npos = 0xffffffffu;
            check(call("__ZNKSs12find_last_ofEPKcm", {sObj, proc.allocCString("ny"), npos}) == 7 &&
                  call("__ZNKSs12find_last_ofEPKcm", {sObj, proc.allocCString("Hn"), 6}) == 0 &&
                  call("__ZNKSs13find_first_ofEPKcm", {sObj, proc.allocCString("oe"), 0}) == 4 &&
                  call("__ZNKSs13find_first_ofERKSsm", {sObj, set, 0}) == 5 &&
                  call("__ZNKSs12find_last_ofERKSsm", {sObj, set, npos}) == 8 &&
                  call("__ZNKSs12find_last_ofEcm", {sObj, uint32_t('o'), npos}) == 4 &&
                  call("__ZNKSs13find_first_ofEcm", {sObj, uint32_t('z'), 0}) == npos &&
                  call("__ZNKSs17find_first_not_ofEPKcm", {sObj, proc.allocCString("Hyd"), 0}) == 3 &&
                  call("__ZNKSs16find_last_not_ofEPKcm", {sObj, proc.allocCString("MV"), npos}) == 7 &&
                  call("__ZNKSs16find_last_not_ofEcm", {sObj, uint32_t('M'), npos}) == 8 &&
                  call("__ZNKSs17find_first_not_ofERKSsm", {sObj, set, 5}) == 6,
                  "string::find_first_of / find_last_of / *_not_of overloads taking a C string, string or char");
            call("__ZNSsD1Ev", {set});
            // _M_ibegin/_M_iend and replace(iterator, iterator, const char*, const char*).
            const uint32_t t = proc.heap().allocZeroed(4);
            call("__ZNSsC1EPKcRKSaIcE", {t, proc.allocCString("abcdef"), 0});
            const uint32_t b = call("__ZNKSs9_M_ibeginEv", {t}), e = call("__ZNKSs7_M_iendEv", {t});
            const uint32_t rep = proc.allocCString("XYZ");
            call("__ZNSs7replaceEN9__gnu_cxx17__normal_iteratorIPcSsEES2_PKcS4_", {t, b + 1, b + 3, rep, rep + 3});
            check(e == b + 6 && proc.readCString(proc.mem().read<uint32_t>(t)) == "aXYZdef",
                  "string::_M_iend and replace(iterator, iterator, const char*, const char*)");
            call("__ZNSsD1Ev", {t});
        }
        call("__ZNSsD1Ev", {sObj});

        // SystemConfiguration Tests
        const uint32_t ref1 = call("_SCNetworkReachabilityCreateWithName", {0, proc.allocCString("apple.com")});
        const uint32_t flagsOut = proc.heap().allocZeroed(4);
        const uint32_t getFlagsOk1 = call("_SCNetworkReachabilityGetFlags", {ref1, flagsOut});
        proc.mem().write<uint32_t>(flagsOut, 0xFFFFFFFFu);
        const uint32_t getFlagsOk1b = call("_SCNetworkReachabilityGetFlags", {ref1, flagsOut});
        check(ref1 != 0 && getFlagsOk1 == 1 && getFlagsOk1b == 1 && proc.mem().read<uint32_t>(flagsOut) == 0,
              "SCNetworkReachabilityGetFlags succeeds and reports not reachable (no outbound network)");

        const uint32_t getFlagsOkRepeated = call("_SCNetworkReachabilityGetFlags", {ref1, flagsOut});
        check(getFlagsOkRepeated == 1, "SCNetworkReachabilityGetFlags repeated queries work");

        const uint32_t getFlagsInvalidRef = call("_SCNetworkReachabilityGetFlags", {0, flagsOut});
        check(getFlagsInvalidRef == 0, "SCNetworkReachabilityGetFlags handles invalid reference");

        const uint32_t getFlagsNullOut = call("_SCNetworkReachabilityGetFlags", {ref1, 0});
        check(getFlagsNullOut == 1, "SCNetworkReachabilityGetFlags handles invalid output pointer but succeeds");

        // CFRelease to clean up
        call("_CFRelease", {ref1});
    }

    // #31: independent ARMv7 API-family oracles (metadata is laid out by the public ABI).
    {
        auto bound = [&](std::initializer_list<const char*> names) { for (auto n : names) if (!proc.lookupSymbol(n)) return false; return true; };
        auto call = [&](const char* name, std::initializer_list<uint32_t> args) { return uint32_t(proc.callGuest(cpu, *proc.lookupSymbol(name), args)); };
        const uint32_t scratch = proc.heap().allocZeroed(512);
        bool libc = bound({"_getprogname", "_strsignal", "_writev", "_flock", "_pthread_cancel"});
        if (libc) {
            int descriptors[2]; libc = ::pipe(descriptors) == 0;
            if (libc) {
                proc.mem().copyIn(scratch + 64, "ABCdef", 6);
                proc.mem().write<uint32_t>(scratch, scratch + 64); proc.mem().write<uint32_t>(scratch + 4, 3);
                proc.mem().write<uint32_t>(scratch + 8, scratch + 67); proc.mem().write<uint32_t>(scratch + 12, 3);
                libc = call("_writev", {uint32_t(descriptors[1]), scratch, 2}) == 6;
                ::close(descriptors[1]);
                char bytes[6]{}; libc &= ::read(descriptors[0], bytes, 6) == 6 && std::memcmp(bytes, "ABCdef", 6) == 0;
                ::close(descriptors[0]);
            }
            libc &= proc.readCString(call("_strsignal", {10})) == "Bus error" && !proc.readCString(call("_getprogname", {})).empty();
            libc &= call("_pthread_cancel", {0xdeadbeef}) == 3;
            if (bound({"_pthread_testcancel", "_pthread_join"})) {
                std::atomic<bool> entered{false};
                const auto worker = proc.defineFunction("__startup_cancel_worker", [&](Ctx& workerContext) {
                    entered = true;
                    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                    while (std::chrono::steady_clock::now() < end) {
                        proc.callGuest(workerContext.cpu, *proc.lookupSymbol("_pthread_testcancel"), {});
                        std::this_thread::yield();
                    }
                    workerContext.cpu.r[0] = 42;
                });
                const auto thread = proc.startThread(worker, 0, 512u << 10, false);
                const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                while (!entered && std::chrono::steady_clock::now() < end) std::this_thread::yield();
                const bool prior = libc;
                const uint32_t cancelled = call("_pthread_cancel", {thread});
                uint32_t returned = 0;
                const bool joined = proc.joinThread(thread, &returned);
                libc &= cancelled == 0 && joined && returned == 0xffffffffu;
                if (!libc) std::printf("    startup libc prior=%d entered=%d cancel=%u joined=%d returned=%08x\n", prior, bool(entered), cancelled, joined, returned);
            } else libc = false;
        }
        check(libc, "startup libc exports writev bytes, Darwin signal names and invalid-thread cancellation errors");
        bool mutexHandoff = bound({"_pthread_mutex_init", "_pthread_mutex_lock", "_pthread_mutex_trylock", "_pthread_mutex_unlock"});
        if (mutexHandoff) {
            const uint32_t mutex = scratch + 256, results = scratch + 320;
            proc.mem().write<uint32_t>(results, 99);
            proc.mem().write<uint32_t>(results + 4, 99);
            mutexHandoff = call("_pthread_mutex_init", {mutex, 0}) == 0 && call("_pthread_mutex_lock", {mutex}) == 0;
            const auto worker = proc.defineFunction("__normal_mutex_handoff", [&](Ctx& workerContext) {
                const auto busy = proc.callGuest(workerContext.cpu, *proc.lookupSymbol("_pthread_mutex_trylock"), {mutex});
                const auto released = proc.callGuest(workerContext.cpu, *proc.lookupSymbol("_pthread_mutex_unlock"), {mutex});
                proc.mem().write<uint32_t>(results, busy);
                proc.mem().write<uint32_t>(results + 4, released);
                workerContext.cpu.r[0] = 0;
            });
            const auto thread = proc.startThread(worker, 0, 64u << 10, false);
            uint32_t returned = 1;
            mutexHandoff &= proc.joinThread(thread, &returned) && returned == 0;
            mutexHandoff &= proc.mem().read<uint32_t>(results) == 16 && proc.mem().read<uint32_t>(results + 4) == 0;
            mutexHandoff &= call("_pthread_mutex_trylock", {mutex}) == 0 && call("_pthread_mutex_unlock", {mutex}) == 0;
        }
        check(mutexHandoff, "normal pthread mutex supports old-iOS cross-thread handoff while trylock reports busy");
        bool protocols = bound({"_protocol_conformsToProtocol", "_protocol_copyProtocolList", "_protocol_copyMethodDescriptionList"});
        if (protocols) {
            const uint32_t parent = scratch + 96, child = scratch + 128, list = scratch + 160, methods = scratch + 176;
            proc.mem().write<uint32_t>(parent + 4, proc.internCString("Parent31"));
            proc.mem().write<uint32_t>(child + 4, proc.internCString("Child31"));
            proc.mem().write<uint32_t>(child + 8, list); proc.mem().write<uint32_t>(list, 1); proc.mem().write<uint32_t>(list + 4, parent);
            // method_list_t: entsize 12, count 1, then { name, types, imp }.
            proc.mem().write<uint32_t>(child + 12, methods); proc.mem().write<uint32_t>(methods, 12); proc.mem().write<uint32_t>(methods + 4, 1);
            proc.mem().write<uint32_t>(methods + 8, proc.internCString("method31")); proc.mem().write<uint32_t>(methods + 12, proc.internCString("v@:"));
            proc.mem().write<uint32_t>(methods + 16, 0);
            const auto copied = call("_protocol_copyProtocolList", {child, scratch + 208});
            const auto descriptions = call("_protocol_copyMethodDescriptionList", {child, 1, 1, scratch + 212});
            protocols = call("_protocol_conformsToProtocol", {child, parent}) && !call("_protocol_conformsToProtocol", {parent, child}) &&
                proc.mem().read<uint32_t>(scratch + 208) == 1 && proc.mem().read<uint32_t>(copied) == parent && proc.mem().read<uint32_t>(copied + 4) == 0 &&
                proc.mem().read<uint32_t>(scratch + 212) == 1 && proc.mem().read<uint32_t>(descriptions) == r.sel("method31") &&
                proc.readCString(proc.mem().read<uint32_t>(descriptions + 4)) == "v@:";
            proc.heap().free(copied); proc.heap().free(descriptions);
            if (protocols && bound({"_protocol_getMethodDescription"})) {
                const uint32_t desc = proc.heap().allocZeroed(8);
                call("_protocol_getMethodDescription", {desc, child, r.sel("method31"), 1, 1});
                const bool found = proc.mem().read<uint32_t>(desc) == r.sel("method31") &&
                                   proc.readCString(proc.mem().read<uint32_t>(desc + 4)) == "v@:";
                call("_protocol_getMethodDescription", {desc, child, r.sel("nothing31"), 1, 1});
                protocols = found && proc.mem().read<uint32_t>(desc) == 0 && proc.mem().read<uint32_t>(desc + 4) == 0;
                proc.heap().free(desc);
            } else protocols = false;
        }
        check(protocols, "startup protocol metadata preserves adoption, method selectors, types and owned lists; protocol_getMethodDescription finds them");
        bool bitvectors = bound({"_CFBitVectorCreate", "_CFBitVectorCreateMutableCopy", "_CFBitVectorGetBitAtIndex", "_CFBitVectorSetBitAtIndex"});
        if (bitvectors) {
            proc.mem().write<uint8_t>(scratch + 224, 0xa5);
            const auto original = call("_CFBitVectorCreate", {0, scratch + 224, 8});
            const auto copy = call("_CFBitVectorCreateMutableCopy", {0, 0, original});
            call("_CFBitVectorSetBitAtIndex", {copy, 1, 1});
            bitvectors = call("_CFBitVectorGetBitAtIndex", {original, 0}) == 1 && call("_CFBitVectorGetBitAtIndex", {original, 1}) == 0 &&
                call("_CFBitVectorGetBitAtIndex", {copy, 1}) == 1 && call("_CFBitVectorGetBitAtIndex", {original, 7}) == 1;
            call("_CFRelease", {copy}); call("_CFRelease", {original});
        }
        check(bitvectors, "startup CF bit vectors use MSB-first bits and mutable copies own their storage");
        bool mach = bound({"_mach_port_allocate", "_mach_port_insert_right", "_mach_msg", "_task_swap_exception_ports", "_thread_swap_exception_ports", "_task_set_exception_ports", "_thread_set_exception_ports", "_vm_map", "_vm_protect"});
        if (mach) {
            mach = call("_mach_port_allocate", {0x103, 1, scratch + 256}) == 0;
            const auto port = proc.mem().read<uint32_t>(scratch + 256);
            mach &= call("_mach_port_insert_right", {0x103, port, port, 20}) == 0;
            const uint32_t msg = scratch + 288;
            proc.mem().write<uint32_t>(msg, 19); proc.mem().write<uint32_t>(msg + 4, 28); proc.mem().write<uint32_t>(msg + 8, port);
            proc.mem().write<uint32_t>(msg + 24, 0x13579bdf);
            mach &= call("_mach_msg", {msg, 3, 28, 28, port, 0, 0}) == 0 && proc.mem().read<uint32_t>(msg + 12) == port && proc.mem().read<uint32_t>(msg + 24) == 0x13579bdf;
            mach &= call("_mach_msg", {msg, 0x102, 0, 28, port, 0, 0}) == 0x10004003;
            mach &= call("_vm_map", {0x103, scratch + 260, 4096, 4095, 1, 0, 0, 0, 0, 3, 7, 0}) == 0;
            const auto address = proc.mem().read<uint32_t>(scratch + 260);
            mach &= address && !(address & 4095) && call("_vm_protect", {0x103, address, 4096, 0, 1}) == 0 && proc.mem().protAt(address) == Memory::kProtRead;
            mach &= proc.lookupSymbol("_NDR_record").has_value();
            call("_vm_protect", {0x103, address, 4096, 0, 3}); call("_vm_deallocate", {0x103, address, 4096});
            call("_mach_port_destroy", {0x103, port});
        }
        check(mach, "startup Mach endpoints exchange bounded messages, time out, map aligned memory and retain page permissions");
    }

    // A signal selects one already-waiting thread; broadcast releases the rest.
    {
        ProcessConfig pcfg;
        pcfg.homePath = "/tmp/hvm_pthreads_signal";
        Process isolated(pcfg);
        auto& icpu = isolated.mainThread().cpu;
        const uint32_t cv = isolated.heap().allocZeroed(4), mu = isolated.heap().allocZeroed(4);
        auto call = [&](const char* name, std::initializer_list<uint32_t> args) {
            return uint32_t(isolated.callGuest(icpu, *isolated.lookupSymbol(name), args));
        };
        call("_pthread_mutex_init", {mu, 0});
        call("_pthread_cond_init", {cv, 0});
        call("_pthread_cond_signal", {cv}); // A signal without waiters is not retained.
        struct Counts { std::atomic<int> ready{0}, awakened{0}, finished{0}; };
        const auto counts = std::make_shared<Counts>();
        const uint32_t waiter = isolated.registerHostFunction("cond_signal_waiter", [cv, mu, counts](Runtime::Ctx& x) {
            try {
                x.proc.callGuest(x.cpu, *x.proc.lookupSymbol("_pthread_mutex_lock"), {mu});
                ++counts->ready;
                x.proc.callGuest(x.cpu, *x.proc.lookupSymbol("_pthread_cond_wait"), {cv, mu});
                ++counts->awakened;
                x.proc.callGuest(x.cpu, *x.proc.lookupSymbol("_pthread_mutex_unlock"), {mu});
            } catch (const Runtime::ProcessExit&) {
            } catch (const CPU::GuestFault&) {}
            ++counts->finished;
        });
        for (int i = 0; i < 4; ++i) isolated.startThread(waiter, 0, 16u << 10, false);
        auto waitFor = [](const std::atomic<int>& value, int target) {
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (value.load() < target && std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return value.load() >= target;
        };
        const bool ready = waitFor(counts->ready, 4);
        bool acquired = false;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (ready && !(acquired = call("_pthread_mutex_trylock", {mu}) == 0) &&
               std::chrono::steady_clock::now() < until) std::this_thread::yield();
        bool one = false, two = false, all = false;
        if (acquired) {
            // Acquiring the mutex after all workers reached wait proves they have released it.
            call("_pthread_cond_signal", {cv});
            call("_pthread_mutex_unlock", {mu});
            const bool woke = waitFor(counts->awakened, 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(200)); // covers multiple 50 ms stop polls
            one = woke && counts->awakened.load() == 1;
            call("_pthread_cond_signal", {cv});
            const bool wokeAgain = waitFor(counts->awakened, 2);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            two = wokeAgain && counts->awakened.load() == 2;
            call("_pthread_cond_broadcast", {cv});
            all = waitFor(counts->awakened, 4);
        }
        check(ready && acquired && one && two && all,
              "pthread cond signals wake one waiter each and broadcast wakes the remainder");
        if (all && waitFor(counts->finished, 4)) {
            const uint32_t ts = isolated.heap().allocZeroed(8);
            isolated.mem().write<int32_t>(ts + 4, 1000000); // one millisecond, relative
            call("_pthread_mutex_lock", {mu});
            const auto result = call("_pthread_cond_timedwait_relative_np", {cv, mu, ts});
            call("_pthread_mutex_unlock", {mu});
            call("_pthread_cond_signal", {cv}); // The expired waiter must no longer be queued.
            check(result == 60, "pthread timed wait expires and leaves no waiter to signal");
        } else {
            check(false, "pthread timed wait expires and leaves no waiter to signal");
        }
        isolated.requestStop();
        waitFor(counts->finished, 4);
    }

    // 2f13. Pthread condition variable process stop broadcast
    {
        ProcessConfig pcfg;
        pcfg.homePath = "/tmp/hvm_pthreads_stop";
        Process isolated(pcfg);
        auto& icpu = isolated.mainThread().cpu;

        const uint32_t cv = isolated.heap().allocZeroed(4);
        const uint32_t mu = isolated.heap().allocZeroed(4);
        isolated.callGuest(icpu, *isolated.lookupSymbol("_pthread_mutex_init"), {mu, 0});
        isolated.callGuest(icpu, *isolated.lookupSymbol("_pthread_cond_init"), {cv, 0});

        // The waiters' state is shared with the host function so it outlives this scope if a waiter is slow to exit.
        struct StopCounts { std::atomic<int> ready{0}, finished{0}; };
        const auto counts = std::make_shared<StopCounts>();
        auto& readyCount = counts->ready;
        auto& finishedCount = counts->finished;
        const uint32_t waiterFn = isolated.registerHostFunction("waiter", [cv, mu, counts](Runtime::Ctx& x) {
            auto& readyCount = counts->ready;
            auto& finishedCount = counts->finished;
            x.proc.callGuest(x.cpu, *x.proc.lookupSymbol("_pthread_mutex_lock"), {mu});
            ++readyCount;
            try {
                x.proc.callGuest(x.cpu, *x.proc.lookupSymbol("_pthread_cond_wait"), {cv, mu});
            } catch (const Runtime::ProcessExit&) {
            } catch (const CPU::GuestFault&) {}
            ++finishedCount;
        });

        for (int i = 0; i < 3; ++i) isolated.startThread(waiterFn, 0, 16u << 10, false);

        const auto tStart = std::chrono::steady_clock::now();
        while (readyCount < 3 && std::chrono::steady_clock::now() - tStart < std::chrono::seconds(2)) std::this_thread::yield();
        const bool allReady = readyCount.load() == 3;   // recorded before the lock loop, not inferred afterwards
        // Only a held mutex proves all three waiters are parked in cond_wait (it was released by each one's wait).
        bool acquired = false;
        while (!(acquired = isolated.callGuest(icpu, *isolated.lookupSymbol("_pthread_mutex_trylock"), {mu}) == 0) &&
               std::chrono::steady_clock::now() - tStart < std::chrono::seconds(2)) std::this_thread::yield();
        if (acquired) isolated.callGuest(icpu, *isolated.lookupSymbol("_pthread_mutex_unlock"), {mu});

        const auto t0 = std::chrono::steady_clock::now();
        isolated.requestStop();

        while (finishedCount < 3 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2)) std::this_thread::yield();
        const auto t1 = std::chrono::steady_clock::now();

        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        check(allReady && acquired && finishedCount.load() == 3 && ms < 35.0,
              "pthread condition variable process stop broadcast wakes up and joins quickly");
        // The Process must outlive every waiter, even when the check above failed: give a stuck one time to leave.
        const auto drain = std::chrono::steady_clock::now();
        while (finishedCount < 3 && std::chrono::steady_clock::now() - drain < std::chrono::seconds(10)) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // A signal selects one current waiter; signals do not accumulate for future waits.
    {
        ProcessConfig pcfg;
        pcfg.homePath = "/tmp/hvm_pthreads_signal";
        Process isolated(pcfg);
        auto& icpu = isolated.mainThread().cpu;
        const uint32_t cv = isolated.heap().allocZeroed(4), mu = isolated.heap().allocZeroed(4);
        auto call = [&](const char* name, std::initializer_list<uint32_t> args) {
            return uint32_t(isolated.callGuest(icpu, *isolated.lookupSymbol(name), args));
        };
        bool ok = call("_pthread_mutex_init", {mu, 0}) == 0 && call("_pthread_cond_init", {cv, 0}) == 0;
        ok &= call("_pthread_cond_signal", {cv}) == 0;
        struct Counts { std::atomic<int> ready{0}, finished{0}, errors{0}; };
        const auto counts = std::make_shared<Counts>();
        const uint32_t waiter = isolated.registerHostFunction("signal_waiter", [cv, mu, counts](Runtime::Ctx& x) {
            auto call = [&](const char* name, std::initializer_list<uint32_t> args) {
                return uint32_t(x.proc.callGuest(x.cpu, *x.proc.lookupSymbol(name), args));
            };
            try {
                if (call("_pthread_mutex_lock", {mu}) != 0) { ++counts->errors; return; }
                ++counts->ready;
                if (call("_pthread_cond_wait", {cv, mu}) != 0) ++counts->errors;
                ++counts->finished;
                if (call("_pthread_mutex_unlock", {mu}) != 0) ++counts->errors;
            } catch (const Runtime::ProcessExit&) {}
        });
        std::vector<uint32_t> threads;
        for (int i = 0; i < 4; ++i) threads.push_back(isolated.startThread(waiter, 0, 16u << 10, false));
        auto awaitCount = [](const std::atomic<int>& count, int target) {
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (count.load() < target && std::chrono::steady_clock::now() < end)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return count.load() >= target;
        };
        const bool ready = awaitCount(counts->ready, 4);
        // Holding the guest mutex after all workers have acquired it proves they entered cond_wait.
        bool locked = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (ready && !(locked = call("_pthread_mutex_trylock", {mu}) == 0) &&
               std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        ok &= ready && locked;
        if (locked) {
            ok &= counts->finished.load() == 0;
            ok &= call("_pthread_cond_signal", {cv}) == 0;
            call("_pthread_mutex_unlock", {mu});
            ok &= awaitCount(counts->finished, 1);
            // Longer than the runtime's 50 ms stop/cancellation polling interval.
            std::this_thread::sleep_for(std::chrono::milliseconds(160));
            ok &= counts->finished.load() == 1;
            call("_pthread_mutex_lock", {mu});
            ok &= call("_pthread_cond_signal", {cv}) == 0;
            ok &= call("_pthread_cond_signal", {cv}) == 0;
            call("_pthread_mutex_unlock", {mu});
            ok &= awaitCount(counts->finished, 3);
            std::this_thread::sleep_for(std::chrono::milliseconds(160));
            ok &= counts->finished.load() == 3;
            ok &= call("_pthread_cond_broadcast", {cv}) == 0;
            ok &= awaitCount(counts->finished, 4);
        }
        if (counts->finished.load() != 4) isolated.requestStop();
        else for (uint32_t thread : threads) { uint32_t retval; isolated.joinThread(thread, &retval); }
        check(ok && counts->finished.load() == 4 && counts->errors.load() == 0,
              "pthread signal wakes one current waiter per call and broadcast wakes the rest");
        bool timed = false;
        if (counts->finished.load() == 4 && !isolated.stopping()) {
            const uint32_t ts = isolated.heap().allocZeroed(8);
            isolated.mem().write<uint32_t>(ts + 4, 20000000); // 20 ms relative timeout
            if (call("_pthread_mutex_lock", {mu}) == 0) {
                timed = call("_pthread_cond_timedwait_relative_np", {cv, mu, ts}) == 60 &&
                        call("_pthread_mutex_trylock", {mu}) == 16; // Darwin ETIMEDOUT, EBUSY
                timed &= call("_pthread_mutex_unlock", {mu}) == 0;
                timed &= call("_pthread_cond_signal", {cv}) == 0;
            }
        }
        check(timed, "pthread timed wait removes its waiter and reacquires the mutex on timeout");
    }

    // 2g. Audio Engine: AVAudioPlayer, AudioQueue, and OpenAL
    {
        // Build a valid 44.1 kHz 16-bit stereo RIFF/WAVE in memory (4410 frames = 0.1s)
        std::vector<uint8_t> wav(44 + 4410 * 4, 0);
        const uint32_t dataBytes = 4410 * 4;
        const uint32_t riffSize = 36 + dataBytes;
        const uint32_t sr = 44100, byteRate = sr * 4;
        const uint16_t fmt = 1, ch = 2, blockAlign = 4, bps = 16;
        std::memcpy(&wav[0], "RIFF", 4); std::memcpy(&wav[4], &riffSize, 4); std::memcpy(&wav[8], "WAVEfmt ", 8);
        const uint32_t fmtChunkSize = 16;
        std::memcpy(&wav[16], &fmtChunkSize, 4); std::memcpy(&wav[20], &fmt, 2); std::memcpy(&wav[22], &ch, 2);
        std::memcpy(&wav[24], &sr, 4); std::memcpy(&wav[28], &byteRate, 4); std::memcpy(&wav[32], &blockAlign, 2);
        std::memcpy(&wav[34], &bps, 2); std::memcpy(&wav[36], "data", 4); std::memcpy(&wav[40], &dataBytes, 4);
        for (size_t i = 0; i < 4410; ++i) {
            int16_t sample = int16_t(std::sin(double(i) * 0.1) * 16000.0);
            std::memcpy(&wav[44 + i * 4], &sample, 2);
            std::memcpy(&wav[44 + i * 4 + 2], &sample, 2);
        }

        auto audioCall = [&](const char* symbol, std::initializer_list<uint32_t> args) {
            return uint32_t(proc.callGuest(cpu, *proc.lookupSymbol(symbol), args));
        };
        const std::string filePath = proc.homePath() + "/audiofile-regression.wav";
        { std::ofstream file(filePath, std::ios::binary); file << "invalid audio"; }
        const auto fileURL = ObjC::id(r.send(cpu, r.getClass("NSURL"), "fileURLWithPath:", {Foundation::makeString(cx, filePath)}));
        const uint32_t fileOut = proc.heap().allocZeroed(64), pcmOut = proc.heap().allocZeroed(64);
        proc.mem().write<uint32_t>(fileOut, 0xDEADBEEF);
        const auto badStatus = audioCall("_AudioFileOpenURL", {fileURL, 1, 0, fileOut});
        const auto badFile = proc.mem().read<uint32_t>(fileOut);
        check(badStatus == 1954115647 && badFile == 0,
              "AudioFileOpenURL rejects undecodable audio instead of publishing a successful empty stream");
        if (!badStatus) audioCall("_AudioFileClose", {badFile});
        { std::ofstream file(filePath, std::ios::binary); file.write(reinterpret_cast<const char*>(wav.data()), wav.size()); }
        const auto goodStatus = audioCall("_AudioFileOpenURL", {fileURL, 1, 0, fileOut});
        const auto fileID = proc.mem().read<uint32_t>(fileOut);
        proc.mem().write<uint32_t>(fileOut + 4, 8);
        audioCall("_AudioFileGetProperty", {fileID, 0x62636E74, fileOut + 4, fileOut + 8});
        proc.mem().write<uint32_t>(fileOut + 4, 8);
        audioCall("_AudioFileGetProperty", {fileID, 0x70636E74, fileOut + 4, fileOut + 16});
        proc.mem().write<uint32_t>(fileOut + 24, 12);
        proc.mem().write<uint32_t>(fileOut + 28, 10);
        const auto readStatus = audioCall("_AudioFileReadPacketData", {fileID, 0, fileOut + 24, 0, 0, 0, fileOut + 28, pcmOut});
        check(goodStatus == 0 && readStatus == 0 && proc.mem().read<uint64_t>(fileOut + 8) == dataBytes &&
              proc.mem().read<uint64_t>(fileOut + 16) == 4410 && proc.mem().read<uint32_t>(fileOut + 24) == 12 &&
              proc.mem().read<uint32_t>(fileOut + 28) == 3 && std::memcmp(proc.mem().host(pcmOut), wav.data() + 44, 12) == 0,
              "AudioFileOpenURL decoded LPCM packets retain sample bytes and packet/byte counts");
        // The packet table is answered, and a property AudioFile does not have is an error
        // that leaves the caller's buffer alone (Fruit Ninja sized a 1.7 GB buffer from the
        // garbage a silent "success" left behind). AudioQueue refuses such a buffer.
        {
            const uint32_t io = proc.heap().allocZeroed(32);
            proc.mem().write<uint32_t>(io, 16);
            for (uint32_t i = 0; i < 4; ++i) proc.mem().write<uint32_t>(io + 4 + i * 4, 0x6D757369);
            const auto tableStatus = audioCall("_AudioFileGetProperty", {fileID, 0x706E666F, io, io + 4});
            const bool table = tableStatus == 0 && proc.mem().read<uint64_t>(io + 4) == 4410 &&
                               proc.mem().read<uint32_t>(io + 12) == 0 && proc.mem().read<uint32_t>(io + 16) == 0;
            proc.mem().write<uint32_t>(io, 4);
            proc.mem().write<uint32_t>(io + 20, 0x6D757369);
            const auto unknownStatus = audioCall("_AudioFileGetProperty", {fileID, 0x6D676963 /* 'mgic' */, io, io + 20});
            const auto infoStatus = audioCall("_AudioFileGetPropertyInfo", {fileID, 0x6D676963, io, 0});
            const bool unknown = unknownStatus == 0x7074793F && infoStatus == 0x7074793F && proc.mem().read<uint32_t>(io + 20) == 0x6D757369;
            const uint32_t queueOut = proc.heap().allocZeroed(8), asbd = proc.heap().allocZeroed(40);
            proc.mem().write<double>(asbd, 44100.0);
            proc.mem().write<uint32_t>(asbd + 28, 2);
            proc.mem().write<uint32_t>(asbd + 32, 16);
            audioCall("_AudioQueueNewOutput", {asbd, 0, 0, 0, 0, 0, queueOut});
            const uint32_t q = proc.mem().read<uint32_t>(queueOut);
            const auto hugeStatus = audioCall("_AudioQueueAllocateBuffer", {q, 0x6973756D, queueOut + 4});
            // 'flst' stays unsupported. FMOD's AudioQueue codec (Subway Surfers) reads it, takes the
            // format's frames per packet as its output buffer size, and with decoded PCM (1 frame per
            // packet) renders 0 frames forever, so the game never finishes loading. Unsupported, FMOD
            // gives the clip up and the game starts.
            const uint32_t fl = proc.heap().allocZeroed(64);
            proc.mem().write<uint32_t>(fl, 44);
            const auto flstStatus = audioCall("_AudioFileGetProperty", {fileID, 0x666C7374, fl, fl + 4});
            check(flstStatus != 0, "AudioFile does not offer a format list (FMOD would loop on decoded PCM)");
            audioCall("_AudioQueueDispose", {q, 1});
            if (!(table && unknown && hugeStatus == uint32_t(-108)))
                std::printf("    AudioFile: pnfo %d (%x) unknown %x/%x huge buffer %d\n", table, tableStatus, unknownStatus, infoStatus, int32_t(hugeStatus));
            check(table && unknown && hugeStatus == uint32_t(-108),
                  "AudioFile answers the packet table, fails unknown properties, AudioQueue refuses a huge buffer");
        }
        // AudioQueue offline rendering (FMOD's AudioQueue codec decodes Unity games' music this way):
        // the app primes one buffer, then each AudioQueueOfflineRender converts the enqueued audio to
        // the offline format, handing every consumed buffer back to the callback, which enqueues the
        // next one or flushes at the end. Source: a 22050 Hz mono ramp 0, 1000, ... 11000 in three
        // 4-frame buffers. Output: 44100 Hz stereo, so linear interpolation gives 500 * k per frame
        // (both channels) until the last source frame, which holds at 11000.
        {
            static uint32_t offlineFed = 0, offlineCalls = 0, offlineFlushed = 0;
            offlineFed = 0; offlineCalls = 0; offlineFlushed = 0;
            auto fill = [](Ctx& x, uint32_t buf) {
                const uint32_t data = x.mem.read<uint32_t>(buf + 4);
                for (uint32_t i = 0; i < 4; ++i) x.mem.write<int16_t>(data + i * 2, int16_t((offlineFed + i) * 1000));
                x.mem.write<uint32_t>(buf + 8, 8);
                offlineFed += 4;
            };
            const uint32_t offlineCb = proc.defineFunction("_test_aq_offline_callback", [fill](Ctx& x) {
                const uint32_t q = x.cpu.r[1], buf = x.cpu.r[2];
                ++offlineCalls;
                if (offlineFed < 12) {
                    fill(x, buf);
                    x.proc.callGuest(x.cpu, *x.proc.lookupSymbol("_AudioQueueEnqueueBuffer"), {q, buf, 0, 0});
                } else if (!offlineFlushed) {
                    offlineFlushed = 1;
                    x.proc.callGuest(x.cpu, *x.proc.lookupSymbol("_AudioQueueFlush"), {q});
                }
                x.cpu.r[0] = 0;
            });
            const uint32_t src = proc.heap().allocZeroed(40), dst = proc.heap().allocZeroed(40), outs = proc.heap().allocZeroed(16);
            const uint32_t ts = proc.heap().allocZeroed(64);
            proc.mem().write<double>(src, 22050.0);
            proc.mem().write<uint32_t>(src + 8, 0x6C70636D);
            proc.mem().write<uint32_t>(src + 12, 0x0C);
            for (uint32_t o : {16u, 24u}) proc.mem().write<uint32_t>(src + o, 2);
            proc.mem().write<uint32_t>(src + 20, 1);
            proc.mem().write<uint32_t>(src + 28, 1);
            proc.mem().write<uint32_t>(src + 32, 16);
            proc.mem().write<double>(dst, 44100.0);
            proc.mem().write<uint32_t>(dst + 8, 0x6C70636D);
            proc.mem().write<uint32_t>(dst + 12, 0x0C);
            for (uint32_t o : {16u, 24u}) proc.mem().write<uint32_t>(dst + o, 4);
            proc.mem().write<uint32_t>(dst + 20, 1);
            proc.mem().write<uint32_t>(dst + 28, 2);
            proc.mem().write<uint32_t>(dst + 32, 16);
            audioCall("_AudioQueueNewOutput", {src, offlineCb, 0, 0, 0, 0, outs});
            const uint32_t q = proc.mem().read<uint32_t>(outs);
            audioCall("_AudioQueueAllocateBuffer", {q, 8, outs + 4});
            audioCall("_AudioQueueAllocateBuffer", {q, 16 * 4, outs + 8});
            const uint32_t srcBuf = proc.mem().read<uint32_t>(outs + 4), outBuf = proc.mem().read<uint32_t>(outs + 8);
            const uint32_t notOffline = audioCall("_AudioQueueOfflineRender", {q, ts, outBuf, 16});
            const uint32_t formatStatus = audioCall("_AudioQueueSetOfflineRenderFormat", {q, dst, 0});
            audioCall("_AudioQueueStart", {q, 0});
            const uint32_t primeStatus = audioCall("_AudioQueueOfflineRender", {q, ts, outBuf, 0});
            const uint32_t primeBytes = proc.mem().read<uint32_t>(outBuf + 8);
            Ctx fx{proc, cpu, proc.mem()};
            fill(fx, srcBuf);
            audioCall("_AudioQueueEnqueueBuffer", {q, srcBuf, 0, 0});
            const uint32_t firstStatus = audioCall("_AudioQueueOfflineRender", {q, ts, outBuf, 16});
            const uint32_t firstBytes = proc.mem().read<uint32_t>(outBuf + 8);
            const uint32_t outData = proc.mem().read<uint32_t>(outBuf + 4);
            bool firstRamp = firstBytes == 64;
            for (uint32_t k = 0; firstRamp && k < 16; ++k)
                firstRamp = proc.mem().read<int16_t>(outData + k * 4) == int16_t(500 * k) && proc.mem().read<int16_t>(outData + k * 4 + 2) == int16_t(500 * k);
            audioCall("_AudioQueueOfflineRender", {q, ts, outBuf, 16});
            const uint32_t secondBytes = proc.mem().read<uint32_t>(outBuf + 8);
            const int16_t secondFirst = proc.mem().read<int16_t>(outData), secondLast = proc.mem().read<int16_t>(outData + 7 * 4);
            // A byte size past the buffer's allocation is clamped to it; stop discards what is queued.
            audioCall("_AudioQueueStop", {q, 1});   // drop the tail of the ramp
            audioCall("_AudioQueueStart", {q, 0});
            proc.mem().write<uint32_t>(srcBuf + 8, 0x7FFFFFF0);
            audioCall("_AudioQueueEnqueueBuffer", {q, srcBuf, 0, 0});
            offlineFed = 12;   // the callback flushes instead of refilling
            audioCall("_AudioQueueOfflineRender", {q, ts, outBuf, 16});
            const uint32_t clampedBytes = proc.mem().read<uint32_t>(outBuf + 8);
            audioCall("_AudioQueueEnqueueBuffer", {q, srcBuf, 0, 0});
            audioCall("_AudioQueueStop", {q, 1});
            audioCall("_AudioQueueOfflineRender", {q, ts, outBuf, 16});
            const uint32_t afterStopBytes = proc.mem().read<uint32_t>(outBuf + 8);
            // Reset in the middle of a buffer: the frames not yet rendered are gone too.
            audioCall("_AudioQueueStart", {q, 0});
            proc.mem().write<uint32_t>(srcBuf + 8, 8);
            audioCall("_AudioQueueEnqueueBuffer", {q, srcBuf, 0, 0});
            audioCall("_AudioQueueOfflineRender", {q, ts, outBuf, 2});
            const uint32_t partialBytes = proc.mem().read<uint32_t>(outBuf + 8);
            audioCall("_AudioQueueReset", {q});
            audioCall("_AudioQueueOfflineRender", {q, ts, outBuf, 16});
            const uint32_t afterResetBytes = proc.mem().read<uint32_t>(outBuf + 8);
            audioCall("_AudioQueueDispose", {q, 1});
            // AudioQueueAllocateBuffer rounds an 8-byte request up to 16: 8 mono frames -> 16 stereo frames at twice the rate.
            const bool bounded = clampedBytes == 16 * 4 && afterStopBytes == 0 && partialBytes == 2 * 4 && afterResetBytes == 0;
            if (!bounded) std::printf("    offline bounds: clamped %u after stop %u partial %u after reset %u\n", clampedBytes, afterStopBytes, partialBytes, afterResetBytes);
            check(bounded, "AudioQueueOfflineRender reads no further than a buffer's allocation; stop and reset discard queued audio");
            const bool ok = notOffline == uint32_t(-66626) && formatStatus == 0 && primeStatus == 0 && primeBytes == 0 && firstStatus == 0 &&
                            firstRamp && secondBytes == 32 && secondFirst == 8000 && secondLast == 11000 && offlineFlushed == 1;
            if (!ok)
                std::printf("    offline render: %d %x %x/%u %x/%u ramp %d second %u %d..%d calls %u flushed %u\n", int32_t(notOffline), formatStatus,
                            primeStatus, primeBytes, firstStatus, firstBytes, firstRamp, secondBytes, secondFirst, secondLast, offlineCalls, offlineFlushed);
            check(ok, "AudioQueueOfflineRender converts enqueued PCM to the offline format and calls back for more until the flush");
        }
        audioCall("_AudioFileClose", {fileID});
        std::filesystem::remove(filePath);
        // An MP3 file is presented as its compressed packets, one MPEG frame each, and decoded
        // by an AudioQueue created with the file's format. FMOD decodes Unity games' music this
        // way; shown decoded PCM instead, it gave up and the games were silent (Subway Surfers).
        // The fixture's layout is in ToneMp3.inc: tag 58, Info frame 208, then 21 packets.
        {
            const size_t mp3Size = sizeof kToneMp3;
            Audio::Mp3Packets index;
            const bool indexed = Audio::indexMp3(kToneMp3, mp3Size, index);
            const bool layout = indexed && index.sampleRate == 44100 && index.channels == 2 && index.framesPerPacket == 1152 &&
                                index.packets.size() == 21 && index.dataOffset == 266 && index.dataBytes == 4388 && index.maxPacketSize == 209 &&
                                index.packets[0] == std::pair<uint32_t, uint32_t>(266, 208) && index.packets[1] == std::pair<uint32_t, uint32_t>(474, 209) &&
                                index.packets[20] == std::pair<uint32_t, uint32_t>(4654 - 209, 209);
            Audio::Mp3Packets cut, none;
            const bool cutShort = Audio::indexMp3(kToneMp3, mp3Size - 100, cut) && cut.packets.size() == 20;   // the last frame is incomplete
            std::vector<uint8_t> noise(4096);
            for (size_t i = 0; i < noise.size(); ++i) noise[i] = uint8_t(i * 37 + 11);
            const bool rejects = !Audio::indexMp3(noise.data(), noise.size(), none) && !Audio::indexMp3(kToneMp3, 40, none);
            check(layout && cutShort && rejects,
                  "indexMp3 skips the ID3 tag and the Info frame, lists each MPEG frame as a packet, and rejects data that is not MP3");

            // Packet by packet, and with one packet split across two calls: the same samples.
            std::vector<int16_t> whole, split;
            auto a = Audio::mp3StreamNew(), b = Audio::mp3StreamNew();
            int rate = 0;
            for (const auto& [off, size] : index.packets) rate = std::max(rate, Audio::mp3StreamDecode(*a, kToneMp3 + off, size, whole));
            for (size_t i = 0; i < index.packets.size(); ++i) {
                const auto& [off, size] = index.packets[i];
                if (i == 5) { Audio::mp3StreamDecode(*b, kToneMp3 + off, 100, split); Audio::mp3StreamDecode(*b, kToneMp3 + off + 100, size - 100, split); }
                else Audio::mp3StreamDecode(*b, kToneMp3 + off, size, split);
            }
            int peak = 0;
            for (size_t i = 4 * 1152 * 2; i < whole.size(); ++i) peak = std::max(peak, std::abs(int(whole[i])));
            // 21 frames of 1152; a decoder may give nothing for the first while its bit reservoir fills.
            const size_t frames = whole.size() / 2;
            const bool decoded = rate == 44100 && frames >= 20 * 1152 && frames <= 21 * 1152 && frames % 1152 == 0 &&
                                 peak > 2600 && peak < 3000 && whole == split;     // 4096 / sqrt(2) = 2896 before encoding; ffmpeg's decoder gives 2766
            if (!decoded) std::printf("    mp3 stream: rate %d frames %zu peak %d same %d\n", rate, frames, peak, whole == split);
            check(decoded, "mp3StreamDecode decodes packets as they arrive, across a packet split in two, to the tone's amplitude");

            const std::string mp3Path = proc.homePath() + "/tone-test.mp3";
            { std::ofstream file(mp3Path, std::ios::binary); file.write(reinterpret_cast<const char*>(kToneMp3), std::streamsize(mp3Size)); }
            const auto mp3URL = ObjC::id(r.send(cpu, r.getClass("NSURL"), "fileURLWithPath:", {Foundation::makeString(cx, mp3Path)}));
            const uint32_t io = proc.heap().allocZeroed(128), fmt = proc.heap().allocZeroed(48), data = proc.heap().allocZeroed(1024), descs = proc.heap().allocZeroed(64);
            const uint32_t opened = audioCall("_AudioFileOpenURL", {mp3URL, 1, 0, io});
            const uint32_t mp3File = proc.mem().read<uint32_t>(io);
            auto prop = [&](uint32_t id, uint32_t size, uint32_t out) { proc.mem().write<uint32_t>(io + 4, size); return audioCall("_AudioFileGetProperty", {mp3File, id, io + 4, out}); };
            const uint32_t dfmt = prop(0x64666D74, 40, fmt);
            const bool format = dfmt == 0 && proc.mem().read<double>(fmt) == 44100.0 && proc.mem().read<uint32_t>(fmt + 8) == 0x2E6D7033 &&
                                proc.mem().read<uint32_t>(fmt + 16) == 0 && proc.mem().read<uint32_t>(fmt + 20) == 1152 && proc.mem().read<uint32_t>(fmt + 28) == 2;
            const uint32_t listInfo = audioCall("_AudioFileGetPropertyInfo", {mp3File, 0x666C7374, io + 8, 0});
            const uint32_t list = prop(0x666C7374, 48, io + 16);
            const bool formatList = listInfo == 0 && proc.mem().read<uint32_t>(io + 8) == 48 && list == 0 &&
                                    proc.mem().read<uint32_t>(io + 16 + 8) == 0x2E6D7033 && proc.mem().read<uint32_t>(io + 16 + 40) == 0x00650002;
            prop(0x70636E74, 8, io + 72); prop(0x706B7562, 4, io + 80); prop(0x62636E74, 8, io + 88); prop(0x706E666F, 16, io + 96);
            const bool counts = proc.mem().read<uint64_t>(io + 72) == 21 && proc.mem().read<uint32_t>(io + 80) == 209 &&
                                proc.mem().read<uint64_t>(io + 88) == 4388 && proc.mem().read<uint64_t>(io + 96) == 21 * 1152;
            // three packets from the start: 208 + 209 + 209 bytes, each described
            proc.mem().write<uint32_t>(io + 4, 0);
            proc.mem().write<uint32_t>(io + 8, 3);
            const uint32_t read3 = audioCall("_AudioFileReadPackets", {mp3File, 0, io + 4, descs, 0, 0, io + 8, data});
            const bool three = read3 == 0 && proc.mem().read<uint32_t>(io + 8) == 3 && proc.mem().read<uint32_t>(io + 4) == 626 &&
                               std::memcmp(proc.mem().host(data), kToneMp3 + 266, 626) == 0 &&
                               proc.mem().read<uint64_t>(descs) == 0 && proc.mem().read<uint32_t>(descs + 12) == 208 &&
                               proc.mem().read<uint64_t>(descs + 16) == 208 && proc.mem().read<uint32_t>(descs + 28) == 209 &&
                               proc.mem().read<uint64_t>(descs + 32) == 417 && proc.mem().read<uint32_t>(descs + 44) == 209;
            // AudioFileReadPacketData stops at the room it is given: 300 bytes hold one packet
            proc.mem().write<uint32_t>(io + 4, 300);
            proc.mem().write<uint32_t>(io + 8, 3);
            audioCall("_AudioFileReadPacketData", {mp3File, 0, io + 4, descs, 1, 0, io + 8, data});
            const bool bounded = proc.mem().read<uint32_t>(io + 8) == 1 && proc.mem().read<uint32_t>(io + 4) == 209 &&
                                 std::memcmp(proc.mem().host(data), kToneMp3 + 474, 209) == 0;
            // past the end: no packets
            proc.mem().write<uint32_t>(io + 8, 2);
            audioCall("_AudioFileReadPackets", {mp3File, 0, io + 4, descs, 21, 0, io + 8, data});
            const bool atEnd = proc.mem().read<uint32_t>(io + 8) == 0 && proc.mem().read<uint32_t>(io + 4) == 0;
            if (!(opened == 0 && format && formatList && counts && three && bounded && atEnd))
                std::printf("    mp3 file: open %x format %d list %d counts %d three %d bounded %d end %d\n", opened, format, formatList, counts, three, bounded, atEnd);
            check(opened == 0 && format && formatList && counts && three && bounded && atEnd,
                  "AudioFile presents an MP3 as compressed packets: format, format list, counts, and packet reads with descriptions");

            // The app's side of FMOD's decode: a queue in the file's format, one packet per
            // buffer, rendered offline to 16-bit stereo.
            const uint32_t outs = proc.heap().allocZeroed(64), dst = proc.heap().allocZeroed(40), ts = proc.heap().allocZeroed(64);
            proc.mem().write<double>(dst, 44100.0);
            proc.mem().write<uint32_t>(dst + 8, 0x6C70636D);
            proc.mem().write<uint32_t>(dst + 12, 0x0C);
            for (uint32_t o : {16u, 24u}) proc.mem().write<uint32_t>(dst + o, 4);
            proc.mem().write<uint32_t>(dst + 20, 1);
            proc.mem().write<uint32_t>(dst + 28, 2);
            proc.mem().write<uint32_t>(dst + 32, 16);
            audioCall("_AudioQueueNewOutput", {fmt, 0, 0, 0, 0, 0, outs});
            const uint32_t q = proc.mem().read<uint32_t>(outs);
            audioCall("_AudioQueueAllocateBuffer", {q, 1152 * 4, outs + 4});
            const uint32_t outBuf = proc.mem().read<uint32_t>(outs + 4);
            audioCall("_AudioQueueSetOfflineRenderFormat", {q, dst, 0});
            audioCall("_AudioQueueStart", {q, 0});
            for (uint32_t i = 0; i < 8; ++i) {
                audioCall("_AudioQueueAllocateBuffer", {q, 209, outs + 8});
                const uint32_t packet = proc.mem().read<uint32_t>(outs + 8);
                const auto& [off, size] = index.packets[i];
                std::memcpy(proc.mem().host(proc.mem().read<uint32_t>(packet + 4)), kToneMp3 + off, size);
                proc.mem().write<uint32_t>(packet + 8, size);
                audioCall("_AudioQueueEnqueueBuffer", {q, packet, 0, 0});
            }
            uint32_t rendered = 0;
            int queuePeak = 0;
            for (int pass = 0; pass < 10; ++pass) {
                audioCall("_AudioQueueOfflineRender", {q, ts, outBuf, 1152});
                const uint32_t got = proc.mem().read<uint32_t>(outBuf + 8) / 4;
                const uint32_t pcm = proc.mem().read<uint32_t>(outBuf + 4);
                if (rendered >= 4 * 1152)
                    for (uint32_t i = 0; i < got * 2; ++i) queuePeak = std::max(queuePeak, std::abs(int(proc.mem().read<int16_t>(pcm + i * 2))));
                rendered += got;
            }
            audioCall("_AudioQueueDispose", {q, 1});
            // ExtAudioFile over the same AudioFile decodes it: its length is the decoded frame count
            // (21 packets of 1152, less whatever the first frame's missing bit reservoir costs).
            const uint32_t wrapped = audioCall("_ExtAudioFileWrapAudioFileID", {mp3File, 0, io});
            const uint32_t ext = proc.mem().read<uint32_t>(io);
            proc.mem().write<uint32_t>(io + 4, 8);
            audioCall("_ExtAudioFileGetProperty", {ext, 0x2366726D /* #frm */, io + 4, io + 8});
            const uint64_t extFrames = proc.mem().read<uint64_t>(io + 8);
            if (!(wrapped == 0 && extFrames >= 20 * 1152 && extFrames <= 21 * 1152)) std::printf("    ext wrap: status %x frames %llu\n", wrapped, (unsigned long long)extFrames);
            check(wrapped == 0 && extFrames >= 20 * 1152 && extFrames <= 21 * 1152,
                  "ExtAudioFileWrapAudioFileID on an MP3 AudioFile decodes it and reports its frames");
            audioCall("_ExtAudioFileDispose", {ext});
            audioCall("_AudioFileClose", {mp3File});
            std::filesystem::remove(mp3Path);
            // 8 packets: 7 or 8 frames of 1152 decoded; the resampler holds back the last sample.
            const bool queueOk = rendered >= 7 * 1152 - 2 && rendered <= 8 * 1152 && queuePeak > 2600 && queuePeak < 3000;
            if (!queueOk) std::printf("    mp3 queue: rendered %u peak %d\n", rendered, queuePeak);
            check(queueOk, "an AudioQueue in MP3 format decodes enqueued packets: offline render returns the tone");
        }
        // Recording: AudioFileCreateWithURL + AudioFileWriteBytes + close, then the file plays back
        // (Talking Larry / John record the player's voice this way). CAF, WAV and AIFF.
        {
            bool roundTrips = true;
            for (const auto& [type, ext] : {std::pair<uint32_t, const char*>{0x63616666, "caf"}, {0x57415645, "wav"}, {0x41494646, "aif"}}) {
                const std::string recPath = proc.homePath() + "/recording-test." + ext;
                const auto recURL = ObjC::id(r.send(cpu, r.getClass("NSURL"), "fileURLWithPath:", {Foundation::makeString(cx, recPath)}));
                const uint32_t asbd = proc.heap().allocZeroed(40), outId = proc.heap().allocZeroed(8), pcm = proc.heap().allocZeroed(400);
                proc.mem().write<double>(asbd, 22050.0);
                proc.mem().write<uint32_t>(asbd + 8, 0x6C70636D);
                proc.mem().write<uint32_t>(asbd + 12, 0x0C);   // signed integer, packed, little-endian
                proc.mem().write<uint32_t>(asbd + 16, 2); proc.mem().write<uint32_t>(asbd + 20, 1);
                proc.mem().write<uint32_t>(asbd + 24, 2); proc.mem().write<uint32_t>(asbd + 28, 1); proc.mem().write<uint32_t>(asbd + 32, 16);
                for (int i = 0; i < 200; ++i) proc.mem().write<int16_t>(pcm + i * 2, int16_t(i * 100));
                const auto created = audioCall("_AudioFileCreateWithURL", {recURL, type, asbd, 1, outId});
                const uint32_t rec = proc.mem().read<uint32_t>(outId);
                proc.mem().write<uint32_t>(outId + 4, 400);
                const auto wrote = audioCall("_AudioFileWriteBytes", {rec, 0, 0, 0, outId + 4, pcm});
                audioCall("_AudioFileClose", {rec});
                const auto opened = audioCall("_AudioFileOpenURL", {recURL, 1, 0, outId});
                const uint32_t back = proc.mem().read<uint32_t>(outId);
                proc.mem().write<uint32_t>(outId + 4, 8);
                audioCall("_AudioFileGetProperty", {back, 0x70636E74, outId + 4, pcm});
                const uint64_t packets = proc.mem().read<uint64_t>(pcm);
                if (opened == 0) audioCall("_AudioFileClose", {back});
                const bool ok = created == 0 && wrote == 0 && opened == 0 && packets == 200;
                if (!ok) std::printf("    recording %s: create %u write %u open %u packets %llu\n", ext, created, wrote, opened, (unsigned long long)packets);
                roundTrips = roundTrips && ok;
                std::filesystem::remove(recPath);
            }
            check(roundTrips, "AudioFileCreateWithURL / WriteBytes / Close write CAF, WAV and AIFF recordings that open again");
        }


        const ObjC::id nsData = Foundation::makeData(cx, wav.data(), uint32_t(wav.size()));
        const ObjC::Class apCls = r.getClass("AVAudioPlayer");
        const ObjC::id player = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, apCls, r.selAlloc)), "initWithData:error:", {nsData, 0}));
        check((r.send(cpu, player, "prepareToPlay") & 255) != 0, "decoded AVAudioPlayer prepares for playback");
        const auto emptyPlayer = ObjC::id(r.send(cpu, apCls, "new"));
        check((r.send(cpu, emptyPlayer, "prepareToPlay") & 255) == 0, "empty AVAudioPlayer cannot prepare for playback");
        r.release(cpu, emptyPlayer);
        uint64_t durBits = r.send(cpu, player, "duration");
        double dur = 0; std::memcpy(&dur, &durBits, 8);
        const bool playOk = (r.send(cpu, player, "play") & 0xFF) != 0;
        const size_t apFrames = renderedFrames(8192);
        check(playOk && std::abs(dur - 0.1) < 0.005 && apFrames >= 4405 && apFrames <= 4410,
              "AVAudioPlayer WAV decoding, duration calculation, and mixer output");
        r.send(cpu, player, "stop");

        // OpenAL test
        auto callAL = [&](const char* sym, std::initializer_list<uint32_t> args) -> uint64_t {
            auto addr = proc.lookupSymbol(sym);
            return proc.callGuest(cpu, *addr, args);
        };
        const uint32_t dev = uint32_t(callAL("_alcOpenDevice", {0}));
        const uint32_t actx = uint32_t(callAL("_alcCreateContext", {dev, 0}));
        callAL("_alcMakeContextCurrent", {actx});
        const uint32_t alIds = proc.heap().allocZeroed(16);
        callAL("_alGenBuffers", {1, alIds});
        const uint32_t alBuf = proc.mem().read<uint32_t>(alIds);
        const uint32_t pcmGuest = proc.heap().alloc(1000 * 4);
        proc.mem().copyIn(pcmGuest, &wav[44], 1000 * 4);
        callAL("_alBufferData", {alBuf, 0x1103 /* AL_FORMAT_STEREO16 */, pcmGuest, 1000 * 4, 44100});
        callAL("_alGenSources", {1, alIds + 4});
        const uint32_t alSrc = proc.mem().read<uint32_t>(alIds + 4);
        callAL("_alGetSourcef", {alSrc, 0x100E /* AL_MAX_GAIN */, alIds + 8});
        const float defaultMaxGain = proc.mem().read<float>(alIds + 8);
        callAL("_alSourcef", {alSrc, 0x100D /* AL_MIN_GAIN */, std::bit_cast<uint32_t>(0.25f)});
        callAL("_alSourcef", {alSrc, 0x100E /* AL_MAX_GAIN */, std::bit_cast<uint32_t>(0.75f)});
        callAL("_alGetSourcef", {alSrc, 0x100D, alIds + 8});
        callAL("_alGetSourcef", {alSrc, 0x100E, alIds + 12});
        check(defaultMaxGain == 1.0f && proc.mem().read<float>(alIds + 8) == 0.25f && proc.mem().read<float>(alIds + 12) == 0.75f,
              "OpenAL source min/max gain defaults and getter/setter round trip");
        callAL("_alSourcei", {alSrc, 0x1009 /* AL_BUFFER */, alBuf});
        callAL("_alSourcePlay", {alSrc});
        callAL("_alGetSourcei", {alSrc, 0x1010 /* AL_SOURCE_STATE */, alIds + 8});
        const int32_t srcState = proc.mem().read<int32_t>(alIds + 8);
        const size_t alFrames = renderedFrames(4096);
        callAL("_alGetSourcei", {alSrc, 0x1010 /* AL_SOURCE_STATE */, alIds + 8});
        const int32_t endState = proc.mem().read<int32_t>(alIds + 8);
        check(dev != 0 && actx != 0 && srcState == 0x1012 /* AL_PLAYING */ && alFrames >= 995 && alFrames <= 1000,
              "OpenAL context, buffer upload, source playback, and mixer output");
        check(endState == 0x1014 /* AL_STOPPED */, "OpenAL source returns to AL_STOPPED when its buffer finishes");
        // Two sources at once (different rates) both sound: the mixer overlaps them.
        callAL("_alSourcePlay", {alSrc});
        callAL("_alGenSources", {1, alIds + 12});
        const uint32_t alSrc2 = proc.mem().read<uint32_t>(alIds + 12);
        callAL("_alGenBuffers", {1, alIds + 16});
        const uint32_t alBuf2 = proc.mem().read<uint32_t>(alIds + 16);
        callAL("_alBufferData", {alBuf2, 0x1103, pcmGuest, 1000 * 4, 22050});
        callAL("_alSourcei", {alSrc2, 0x1009, alBuf2});
        callAL("_alSourcePlay", {alSrc2});
        const size_t bothFrames = renderedFrames(8192);
        check(bothFrames >= 1990 && bothFrames <= 2000, "two OpenAL sources mix; a 22.05 kHz buffer plays at the right speed");
        // A looping source repeats its queue and never finishes a buffer (Pocket Ants' rain is one
        // queued buffer on a looping source; it stopped after a single pass).
        {
            callAL("_alSourceStop", {alSrc});
            callAL("_alSourceStop", {alSrc2});
            renderedFrames(4096);
            const uint32_t loopIds = proc.heap().allocZeroed(16);
            callAL("_alGenSources", {1, loopIds});
            const uint32_t loopSrc = proc.mem().read<uint32_t>(loopIds);
            callAL("_alSourcei", {loopSrc, 0x1007 /* AL_LOOPING */, 1});
            proc.mem().write<uint32_t>(loopIds + 4, alBuf);
            callAL("_alSourceQueueBuffers", {loopSrc, 1, loopIds + 4});
            callAL("_alSourcePlay", {loopSrc});
            const size_t loopFrames = renderedFrames(4096);   // four passes of the 1000-frame buffer
            callAL("_alGetSourcei", {loopSrc, 0x1010 /* AL_SOURCE_STATE */, loopIds + 8});
            callAL("_alGetSourcei", {loopSrc, 0x1016 /* AL_BUFFERS_PROCESSED */, loopIds + 12});
            check(loopFrames >= 4000 && proc.mem().read<int32_t>(loopIds + 8) == 0x1012 && proc.mem().read<int32_t>(loopIds + 12) == 0,
                  "a looping OpenAL source repeats its queued buffers and finishes none of them");
            callAL("_alSourceStop", {loopSrc});
            callAL("_alDeleteSources", {1, loopIds});
            renderedFrames(4096);
        }
        // __maskrune answers for the class asked about: code built without the inline <ctype.h>
        // calls it for ASCII too, and "yes" to iscntrl for a letter dropped typed characters.
        {
            auto mask = [&](int c, uint32_t m) { return uint32_t(proc.callGuest(cpu, *proc.lookupSymbol("___maskrune"), {uint32_t(c), m})); };
            check(mask('L', 0x100 /* alpha */) != 0 && mask('L', 0x200 /* cntrl */) == 0 && mask('7', 0x400 /* digit */) != 0 &&
                  mask('7', 0x100) == 0 && mask('\n', 0x200) != 0 && mask(' ', 0x40000 /* print */) != 0 && mask(-1, 0x500) == 0 && mask(200, 0x100) == 0,
                  "__maskrune reports only the character classes in its mask");
        }
        // cStringUsingEncoding: / getCString: in a wide encoding give the characters alone: no
        // byte-order mark (Plants vs. Zombies read 0xFF as the typed letter), a wide terminator.
        {
            auto& sr = ObjC::rt(proc);
            const uint32_t lit = proc.allocCString("La");
            const ObjC::id str = ObjC::id(sr.send(cpu, ObjC::id(sr.getClass("NSString")), "stringWithUTF8String:", {lit}));
            const uint32_t wide = uint32_t(sr.send(cpu, str, "cStringUsingEncoding:", {10 /* NSUnicodeStringEncoding */}));
            const bool utf16 = wide && proc.mem().read<uint16_t>(wide) == 'L' && proc.mem().read<uint16_t>(wide + 2) == 'a' && proc.mem().read<uint16_t>(wide + 4) == 0;
            const uint32_t w32 = uint32_t(sr.send(cpu, str, "cStringUsingEncoding:", {0x9C000100u /* UTF-32LE */}));
            const bool utf32 = w32 && proc.mem().read<uint32_t>(w32) == 'L' && proc.mem().read<uint32_t>(w32 + 4) == 'a' && proc.mem().read<uint32_t>(w32 + 8) == 0;
            const uint32_t len32 = uint32_t(sr.send(cpu, str, "lengthOfBytesUsingEncoding:", {0x9C000100u}));
            const bool can32 = (uint32_t(sr.send(cpu, str, "canBeConvertedToEncoding:", {0x9C000100u})) & 0xFF) != 0;
            check(len32 == 8 && can32, "UTF-32 byte lengths and conversion checks match what getCString: writes");
            const ObjC::id back = ObjC::id(sr.send(cpu, ObjC::id(sr.send(cpu, ObjC::id(sr.getClass("NSString")), sr.selAlloc)), "initWithBytes:length:encoding:", {w32, 8, 0x9C000100u}));
            check(back && (uint32_t(sr.send(cpu, back, "isEqualToString:", {str})) & 0xFF) != 0 &&
                  uint32_t(sr.send(cpu, str, "maximumLengthOfBytesUsingEncoding:", {0x8C000100u})) >= uint32_t(sr.send(cpu, str, "lengthOfBytesUsingEncoding:", {0x8C000100u})),
                  "UTF-32 bytes read back as the same string, and the maximum length covers the mark");
            const uint32_t out = proc.heap().allocZeroed(16);
            std::memset(proc.mem().host(out), 0x55, 16);
            const bool fits = (uint32_t(sr.send(cpu, str, "getCString:maxLength:encoding:", {out, 6, 10})) & 0xFF) != 0;
            const bool tooSmall = (uint32_t(sr.send(cpu, str, "getCString:maxLength:encoding:", {out, 5, 10})) & 0xFF) == 0;
            check(utf16 && utf32 && fits && tooSmall && proc.mem().read<uint16_t>(out) == 'L' && proc.mem().read<uint16_t>(out + 4) == 0 &&
                  proc.mem().read<uint8_t>(out + 6) == 0x55,
                  "C strings in UTF-16 and UTF-32 carry no byte-order mark and end with a wide terminator");
            proc.heap().free(out);
            proc.heap().free(lit);
        }

        // NSString byte conversion: literal Unicode/encoding oracles, not game output.
        {
            auto& sr = ObjC::rt(proc);
            const char* selector = "getBytes:maxLength:usedLength:encoding:options:range:remainingRange:";
            const auto str = Foundation::constString(proc, "A\xc3\xa9\xf0\x9f\x98\x80Z");
            const bool supported = (sr.send(cpu, str, "respondsToSelector:", {sr.sel(selector)}) & 0xff) != 0;
            check(supported, "NSString range byte conversion is implemented (BADLAND Crashlytics startup)");
            if (supported) {
                const auto out = proc.heap().allocZeroed(64);
                auto convert = [&](uint32_t max, uint32_t enc, uint32_t options, uint32_t loc, uint32_t len, bool countOnly = false) {
                    std::memset(proc.mem().host(out), 0x55, 64);
                    return (sr.send(cpu, str, selector, {countOnly ? 0u : out, max, out + 32, enc, options, loc, len, out + 36}) & 0xff) != 0;
                };
                auto used = [&] { return proc.mem().read<uint32_t>(out + 32); };
                auto left = [&](uint32_t loc, uint32_t len) { return proc.mem().read<uint32_t>(out + 36) == loc && proc.mem().read<uint32_t>(out + 40) == len; };
                check(convert(7, 4, 0, 0, 5) && used() == 7 && left(4, 1) &&
                      std::memcmp(proc.mem().host(out), "A\xc3\xa9\xf0\x9f\x98\x80", 7) == 0 && proc.mem().read<uint8_t>(out + 7) == 0x55,
                      "NSString getBytes writes partial UTF-8 without a terminator and counts UTF-16 units");
                check(convert(3, 4, 0, 1, 3) && used() == 2 && left(2, 2) && proc.mem().read<uint8_t>(out + 2) == 0x55,
                      "NSString getBytes never splits a UTF-8 scalar or surrogate pair");
                check(!convert(1, 4, 0, 1, 1) && used() == 0 && left(1, 1) && proc.mem().read<uint8_t>(out) == 0x55,
                      "NSString getBytes fails when no character fits");
                check(convert(0, 4, 0, 0, 5, true) && used() == 8 && left(5, 0),
                      "NSString getBytes NULL buffer measures the full range independent of maxLength");
                check(convert(16, 1, 0, 0, 5) && used() == 1 && left(1, 4) && proc.mem().read<uint8_t>(out + 1) == 0x55,
                      "NSString getBytes stops at an unrepresentable character with a successful prefix");
                check(convert(16, 1, 1, 1, 1) && used() == 1 && proc.mem().read<uint8_t>(out) == 'e',
                      "NSString lossy ASCII uses ASCII fallback, not the CF control byte 0xff");
                check(convert(16, 10, 0, 1, 1) && used() == 2 && left(2, 0) && proc.mem().read<uint16_t>(out) == 0xe9 && proc.mem().read<uint8_t>(out + 2) == 0x55,
                      "NSString getBytes UTF-16 has no byte-order mark by default");
                check(convert(16, 10, 2, 1, 1) && used() == 4 && proc.mem().read<uint16_t>(out) == 0xfeff && proc.mem().read<uint16_t>(out + 2) == 0xe9,
                      "NSString getBytes external representation includes the UTF-16 byte-order mark");
                check(!convert(16, 0xdeadbeef, 0, 0, 5) && used() == 0 && left(0, 5),
                      "NSString getBytes rejects unknown encodings");
                bool rejected = false;
                try { convert(16, 4, 0, 0xffffffff, 2); } catch (const CPU::GuestFault&) { rejected = true; }
                check(rejected, "NSString getBytes rejects overflowing out-of-bounds ranges");
                const uint32_t ro = 0xbf320000;
                proc.mem().map(ro, 4096, Memory::kProtRead);
                for (int output = 0; output < 3; ++output) {
                    bool faulted = false;
                    std::memset(proc.mem().host(ro), 0x55, 4096);
                    try { sr.send(cpu, str, selector, {output == 0 ? ro : out, 16, output == 1 ? ro : out + 32,
                                                     4, 0, 0, 1, output == 2 ? ro : out + 36}); }
                    catch (const CPU::GuestFault& e) { faulted = e.exit == CPU::Exit::MemFault; }
                    check(faulted && proc.mem().read<uint32_t>(ro) == 0x55555555,
                          "NSString getBytes faults without modifying read-only buffer/used/range outputs");
                }
                proc.heap().free(out);
            }
        }

        // AudioQueue test with guest ARMv7 callback
        const uint32_t aqOut = proc.heap().allocZeroed(32);
        // Guest callback at 0x20000: mov r3, #1; str r3, [r0]; bx lr
        proc.mem().map(0x20000, 0x1000, Memory::kProtRX);
        const uint32_t aqCbCode[] = { 0xE3A03001u, 0xE5803000u, 0xE12FFF1Eu };
        proc.mem().copyIn(0x20000, aqCbCode, sizeof aqCbCode);
        const uint32_t userFlagAddr = proc.heap().allocZeroed(4);
        auto callAT = [&](const char* sym, std::initializer_list<uint32_t> args) -> uint64_t {
            auto addr = proc.lookupSymbol(sym);
            return proc.callGuest(cpu, *addr, args);
        };
        callAT("_AudioQueueNewOutput", {0, 0x20000, userFlagAddr, 0, 0, 0, aqOut});
        const uint32_t qid = proc.mem().read<uint32_t>(aqOut);
        callAT("_AudioQueueAllocateBuffer", {qid, 400, aqOut + 4});
        const uint32_t qbuf = proc.mem().read<uint32_t>(aqOut + 4);
        const uint32_t qdata = proc.mem().read<uint32_t>(qbuf + 4);
        proc.mem().copyIn(qdata, &wav[44], 400);
        proc.mem().write<uint32_t>(qbuf + 8, 400);
        callAT("_AudioQueueStart", {qid, 0});
        callAT("_AudioQueueEnqueueBuffer", {qid, qbuf, 0, 0});
        Foundation::mainRunLoop(proc).runOnce(cpu, 0.0);
        const uint32_t cbEarly = proc.mem().read<uint32_t>(userFlagAddr);   // not played yet: no callback
        const size_t aqFrames = renderedFrames(1024);
        for (int i = 0; i < 20 && !proc.mem().read<uint32_t>(userFlagAddr); ++i) Foundation::mainRunLoop(proc).runOnce(cpu, 0.02);
        const uint32_t cbRan = proc.mem().read<uint32_t>(userFlagAddr);
        callAT("_AudioQueueDispose", {qid, 1});
        check(qid != 0 && qbuf != 0 && aqFrames >= 95 && aqFrames <= 100 && cbEarly == 0 && cbRan == 1,
              "AudioQueue buffer playback through the mixer; the callback fires after the buffer has played");

        // RemoteIO output: starting the unit must pull PCM from the app's callback.
        {
            std::atomic<int> calls{0};
            uint32_t gotFrames = 0, gotBytes = 0, renderStack = 0;
            bool wrotePcm = false;
            const auto cb = proc.registerHostFunction("hvm_test_au_output", [&](Ctx& cx) {
                ++calls;
                renderStack = cx.proc.currentThread().stackSize;
                gotFrames = cx.mem.read<uint32_t>(cx.cpu.r[CPU::kSP]);
                const uint32_t abl = cx.mem.read<uint32_t>(cx.cpu.r[CPU::kSP] + 4);
                if (!abl || cx.mem.read<uint32_t>(abl) != 1) return;
                const uint32_t data = cx.mem.read<uint32_t>(abl + 12);
                gotBytes = cx.mem.read<uint32_t>(abl + 8);
                if (!data || gotBytes < gotFrames * 4) return;
                for (uint32_t i = 0; i < gotFrames * 2; ++i) cx.mem.write<int16_t>(data + i * 2, 12000);
                wrotePcm = true;
            });
            const uint32_t out = proc.heap().allocZeroed(16), callback = proc.heap().allocZeroed(8);
            callAT("_AudioComponentInstanceNew", {0, out});
            const uint32_t unit = proc.mem().read<uint32_t>(out);
            proc.mem().write<uint32_t>(callback, cb);
            callAT("_AudioUnitSetProperty", {unit, 23, 1, 0, callback, 8});
            callAT("_AudioUnitInitialize", {unit});
            callAT("_AudioOutputUnitStart", {unit});
            for (int i = 0; i < 100 && !calls; ++i) Foundation::mainRunLoop(proc).runOnce(cpu, 0.02);
            const size_t auFrames = renderedFrames(1024);
            callAT("_AudioOutputUnitStop", {unit});
            callAT("_AudioUnitUninitialize", {unit});
            callAT("_AudioComponentInstanceDispose", {unit});
            check(unit != 0 && calls > 0 && wrotePcm && gotFrames > 0 && gotBytes >= gotFrames * 4 && auFrames >= gotFrames,
                  "AudioOutputUnitStart invokes the render callback and mixes its signed 16-bit stereo PCM");
            // CoreAudio's I/O thread has a 512 KB stack; Smash Hit's callback needs over 256 KB.
            check(renderStack >= (512u << 10), "AudioUnit render callbacks run on a thread with a 512 KB stack");
        }

        // RemoteIO <- multichannel mixer <- converter units with their own callbacks, wired with
        // kAudioUnitProperty_MakeConnection, plus a render notification on the output unit:
        // Plants vs. Zombies plays every sound effect this way and starts them from the notification.
        {
            std::atomic<int> fed{0}, pre{0}, post{0};
            const auto feedCb = proc.registerHostFunction("hvm_test_au_converter_feed", [&](Ctx& cx) {
                const uint32_t frames = cx.mem.read<uint32_t>(cx.cpu.r[CPU::kSP]);
                const uint32_t abl = cx.mem.read<uint32_t>(cx.cpu.r[CPU::kSP] + 4);
                if (!abl || cx.mem.read<uint32_t>(abl) != 1) return;
                const uint32_t data = cx.mem.read<uint32_t>(abl + 12);
                for (uint32_t i = 0; data && i < frames * 2; ++i) cx.mem.write<int16_t>(data + i * 2, 9000);
                ++fed;
            });
            std::atomic<int> order{0};   // 1: pre came before the input's render; 2: post came after it
            const auto notifyCb = proc.registerHostFunction("hvm_test_au_render_notify", [&](Ctx& cx) {
                const uint32_t flags = cx.mem.read<uint32_t>(cx.cpu.r[1]);
                if (flags & 4) { ++pre; if (fed.load() == 0 && order.load() == 0) order = 1; }
                if (flags & 8) { ++post; if (fed.load() > 0 && order.load() == 1) order = 2; }
                cx.cpu.r[0] = 0;   // noErr
            });
            const uint32_t m = proc.heap().allocZeroed(128);
            auto unitOf = [&](uint32_t type, uint32_t sub) {
                proc.mem().write<uint32_t>(m, type);
                proc.mem().write<uint32_t>(m + 4, sub);
                proc.mem().write<uint32_t>(m + 8, 0x6170706C);   // 'appl'
                const uint32_t comp = uint32_t(callAT("_AudioComponentFindNext", {0, m}));
                callAT("_AudioComponentInstanceNew", {comp, m + 32});
                return proc.mem().read<uint32_t>(m + 32);
            };
            const uint32_t rio = unitOf(0x61756F75 /* auou */, 0x72696F63 /* rioc */);
            const uint32_t mixer = unitOf(0x61756D78 /* aumx */, 0x6D636D78 /* mcmx */);
            const uint32_t conv = unitOf(0x61756663 /* aufc */, 0x636F6E76 /* conv */);
            // A converter's input format is what its callback gives; its output side is left alone.
            const uint32_t fmt = m + 40;
            proc.mem().write<double>(fmt, 22050.0);
            const uint32_t lpcm[8] = {0x6C70636D, 0x0C, 4, 1, 4, 2, 16, 0};
            proc.mem().copyIn(fmt + 8, lpcm, 32);
            callAT("_AudioUnitSetProperty", {conv, 8, 1, 0, fmt, 40});
            proc.mem().write<uint32_t>(m + 96, feedCb);
            callAT("_AudioUnitSetProperty", {conv, 23, 1, 0, m + 96, 8});
            // mixer input 3 <- converter, RemoteIO <- mixer
            proc.mem().write<uint32_t>(m + 104, conv); proc.mem().write<uint32_t>(m + 108, 0); proc.mem().write<uint32_t>(m + 112, 3);
            callAT("_AudioUnitSetProperty", {mixer, 1, 1, 3, m + 104, 12});
            proc.mem().write<uint32_t>(m + 104, mixer); proc.mem().write<uint32_t>(m + 112, 0);
            callAT("_AudioUnitSetProperty", {rio, 1, 1, 0, m + 104, 12});
            const uint32_t added = uint32_t(callAT("_AudioUnitAddRenderNotify", {rio, notifyCb, 5}));
            callAT("_AudioUnitInitialize", {rio});
            callAT("_AudioOutputUnitStart", {rio});
            for (int i = 0; i < 100 && (!fed || !post); ++i) Foundation::mainRunLoop(proc).runOnce(cpu, 0.02);
            const size_t heard = renderedFrames(2048);
            callAT("_AudioUnitRemoveRenderNotify", {rio, notifyCb, 5});
            const int postAfterRemove = post.load();
            const int fedAtRemove = fed.load();
            for (int i = 0; i < 100 && fed.load() < fedAtRemove + 3; ++i) Foundation::mainRunLoop(proc).runOnce(cpu, 0.02);
            const bool cyclesAfterRemove = fed.load() >= fedAtRemove + 3;
            callAT("_AudioOutputUnitStop", {rio});
            for (int i = 0; i < 10; ++i) Foundation::mainRunLoop(proc).runOnce(cpu, 0.02);   // a render thread that would still pump
            const size_t afterStop = renderedFrames(4096);
            check(fed > 0 && heard > 0, "a mixer input connected to a converter unit plays the converter's callback");
            check(afterStop == 0, "stopping the output silences the converters its mixer renders");
            check(added == 0 && pre > 0 && post > 0 && order.load() == 2 && cyclesAfterRemove && post.load() <= postAfterRemove + 1,
                  "AudioUnitAddRenderNotify is called before and after each render cycle, and stops when removed");
            for (uint32_t u : {conv, mixer, rio}) callAT("_AudioComponentInstanceDispose", {u});
        }

        // A buffer asked for with no packet descriptions still has room for them (PvZ copies one
        // per frame of its music between buffers and crashed on a null pointer).
        {
            const uint32_t outAt = proc.heap().allocZeroed(64);
            const uint32_t fmt = proc.heap().allocZeroed(40);
            proc.mem().write<double>(fmt, 44100.0);
            const uint32_t lpcm[8] = {0x6C70636D, 0x0C, 4, 1, 4, 2, 16, 0};
            proc.mem().copyIn(fmt + 8, lpcm, 32);
            callAT("_AudioQueueNewOutput", {fmt, 0, 0, 0, 0, 0, outAt});
            const uint32_t q = proc.mem().read<uint32_t>(outAt);
            const uint32_t status = uint32_t(callAT("_AudioQueueAllocateBufferWithPacketDescriptions", {q, 4096, 0, outAt + 8}));
            const uint32_t buf = proc.mem().read<uint32_t>(outAt + 8);
            const bool room = status == 0 && buf && proc.mem().read<uint32_t>(buf + 20) != 0 && proc.mem().read<uint32_t>(buf + 16) >= 1024;
            callAT("_AudioQueueDispose", {q, 1});
            check(room, "an audio queue buffer asked for without packet descriptions still has room for one per frame");
        }

        // AudioQueueStop(queue, false) lets what is queued play to its end, and the queue's
        // IsRunning listener hears about the start and the real stop (the Talking games wait for
        // it before they listen again; stopping at once cut their answer short).
        {
            int finished = 0, notified = 0, finishedAtStopNotice = -1;
            uint32_t noticeProp = 0, noticeUd = 0;
            const auto outCb = proc.registerHostFunction("hvm_test_aq_drain_out", [&](Ctx&) { ++finished; });
            const auto lisCb = proc.registerHostFunction("hvm_test_aq_drain_listener", [&](Ctx& cx) {
                noticeUd = cx.cpu.r[0]; noticeProp = cx.cpu.r[2];
                if (++notified == 2) finishedAtStopNotice = finished;
            });
            const uint32_t outAt = proc.heap().allocZeroed(64);
            const uint32_t fmt = proc.heap().allocZeroed(40);
            proc.mem().write<double>(fmt, 22050.0);
            const uint32_t lpcm[8] = {0x6C70636D, 0x0C, 2, 1, 2, 1, 16, 0};
            proc.mem().copyIn(fmt + 8, lpcm, 32);
            callAT("_AudioQueueNewOutput", {fmt, outCb, 0, 0, 0, 0, outAt});
            const uint32_t q = proc.mem().read<uint32_t>(outAt);
            callAT("_AudioQueueAddPropertyListener", {q, 0x6171726E, lisCb, 77});
            for (uint32_t i = 0; i < 2; ++i) {
                callAT("_AudioQueueAllocateBuffer", {q, 2206, outAt + 8});
                const uint32_t buf = proc.mem().read<uint32_t>(outAt + 8), data = proc.mem().read<uint32_t>(buf + 4);
                for (uint32_t k = 0; k < 1103; ++k) proc.mem().write<int16_t>(data + k * 2, 9000);
                proc.mem().write<uint32_t>(buf + 8, 2206);
                callAT("_AudioQueueEnqueueBuffer", {q, buf, 0, 0});
            }
            callAT("_AudioQueueStart", {q, 0});
            callAT("_AudioQueueStop", {q, 0});
            auto isRunning = [&] {
                proc.mem().write<uint32_t>(outAt + 16, 4);
                callAT("_AudioQueueGetProperty", {q, 0x6171726E, outAt + 12, outAt + 16});
                return proc.mem().read<uint32_t>(outAt + 12);
            };
            const uint32_t runningWhileQueued = isRunning();
            size_t heard = 0;
            for (int i = 0; i < 200 && notified < 2; ++i) {
                heard += renderedFrames(1024);
                Foundation::mainRunLoop(proc).runOnce(cpu, 0.01);
            }
            const uint32_t runningAfter = isRunning();
            check(runningWhileQueued == 1 && finished == 2 && finishedAtStopNotice == 2 && runningAfter == 0,
                  "AudioQueueStop without inImmediate plays what is queued before the queue stops");
            check(notified == 2 && noticeProp == 0x6171726E && noticeUd == 77,
                  "an audio queue's IsRunning listener is told when it starts and when it has stopped");
            // Removed, a listener hears nothing; an immediate stop is still immediate.
            callAT("_AudioQueueRemovePropertyListener", {q, 0x6171726E, lisCb, 77});
            callAT("_AudioQueueStart", {q, 0});
            callAT("_AudioQueueStop", {q, 1});
            for (int i = 0; i < 5; ++i) Foundation::mainRunLoop(proc).runOnce(cpu, 0.01);
            check(notified == 2 && isRunning() == 0, "a removed audio queue listener is not called, and an immediate stop stops at once");
            // AudioQueueDispose(queue, false) on a playing queue: the sound finishes, the listener
            // hears the stop, and only then is the queue gone.
            callAT("_AudioQueueAddPropertyListener", {q, 0x6171726E, lisCb, 78});
            {
                callAT("_AudioQueueAllocateBuffer", {q, 2206, outAt + 8});
                const uint32_t buf = proc.mem().read<uint32_t>(outAt + 8), data = proc.mem().read<uint32_t>(buf + 4);
                for (uint32_t k = 0; k < 1103; ++k) proc.mem().write<int16_t>(data + k * 2, 9000);
                proc.mem().write<uint32_t>(buf + 8, 2206);
                callAT("_AudioQueueEnqueueBuffer", {q, buf, 0, 0});
            }
            const int finishedBefore = finished;
            callAT("_AudioQueueStart", {q, 0});
            callAT("_AudioQueueDispose", {q, 0});
            const uint32_t aliveWhilePlaying = isRunning();
            for (int i = 0; i < 200 && notified < 4; ++i) {
                heard += renderedFrames(1024);
                Foundation::mainRunLoop(proc).runOnce(cpu, 0.01);
            }
            const uint32_t gone = uint32_t(callAT("_AudioQueueAddPropertyListener", {q, 0x6171726E, lisCb, 79}));
            check(aliveWhilePlaying == 1 && finished == finishedBefore + 1 && notified == 4 && noticeUd == 78 && gone != 0,
                  "AudioQueueDispose without inImmediate plays what is queued, reports the stop, then frees the queue");
            (void)heard;
        }

        // An AUGraph with a mixer feeding the output (Unreal Engine 3 sound effects): each mixer
        // input has its own callback, asked with its input number, and a disabled input is not asked.
        {
            std::atomic<int> calls0{0}, calls1{0}, calls2{0};
            uint32_t busSeen0 = 99, busSeen1 = 99, frames1 = 0;
            const auto cb = proc.registerHostFunction("hvm_test_aug_input", [&](Ctx& cx) {
                const uint32_t bus = cx.cpu.r[3];
                const uint32_t frames = cx.mem.read<uint32_t>(cx.cpu.r[CPU::kSP]);
                const uint32_t abl = cx.mem.read<uint32_t>(cx.cpu.r[CPU::kSP] + 4);
                if (cx.cpu.r[0] == 100) { ++calls0; busSeen0 = bus; }
                else if (cx.cpu.r[0] == 101) { ++calls1; busSeen1 = bus; frames1 = frames; }
                else ++calls2;
                cx.cpu.r[0] = 0;
                if (!abl || cx.mem.read<uint32_t>(abl) != 1) return;
                const uint32_t data = cx.mem.read<uint32_t>(abl + 12);
                // input 1 is 22050 Hz mono, the others the default 44100 Hz stereo
                const uint32_t samples = bus == 1 ? frames : frames * 2;
                if (!data || cx.mem.read<uint32_t>(abl + 8) < samples * 2) return;
                for (uint32_t i = 0; i < samples; ++i) cx.mem.write<int16_t>(data + i * 2, 9000);
            });
            const uint32_t scratch = proc.heap().allocZeroed(128);
            const uint32_t descAt = scratch, outAt = scratch + 24, cbAt = scratch + 40, fmtAt = scratch + 48;
            callAT("_NewAUGraph", {outAt});
            const uint32_t graph = proc.mem().read<uint32_t>(outAt);
            const uint32_t mixerDesc[5] = {0x61756D78 /*aumx*/, 0x3364656D /*3dem*/, 0x6170706C, 0, 0};
            const uint32_t outDesc[5] = {0x61756F75 /*auou*/, 0x72696F63 /*rioc*/, 0x6170706C, 0, 0};
            proc.mem().copyIn(descAt, mixerDesc, 20);
            callAT("_AUGraphAddNode", {graph, descAt, outAt});
            const uint32_t mixerNode = proc.mem().read<uint32_t>(outAt);
            proc.mem().copyIn(descAt, outDesc, 20);
            callAT("_AUGraphAddNode", {graph, descAt, outAt});
            const uint32_t outNode = proc.mem().read<uint32_t>(outAt);
            callAT("_AUGraphGetNodeCount", {graph, outAt});
            const uint32_t nodeCount = proc.mem().read<uint32_t>(outAt);
            callAT("_AUGraphOpen", {graph});
            callAT("_AUGraphNodeInfo", {graph, mixerNode, 0, outAt});
            const uint32_t mixer = proc.mem().read<uint32_t>(outAt);
            callAT("_AUGraphConnectNodeInput", {graph, mixerNode, 0, outNode, 0});
            // input 0: AUGraphSetNodeInputCallback; inputs 1 and 2: the mixer unit's own property
            proc.mem().write<uint32_t>(cbAt, cb);
            proc.mem().write<uint32_t>(cbAt + 4, 100);
            callAT("_AUGraphSetNodeInputCallback", {graph, mixerNode, 0, cbAt});
            proc.mem().write<uint32_t>(cbAt + 4, 101);
            callAT("_AudioUnitSetProperty", {mixer, 23, 1, 1, cbAt, 8});
            proc.mem().write<uint32_t>(cbAt + 4, 102);
            callAT("_AudioUnitSetProperty", {mixer, 23, 1, 2, cbAt, 8});
            proc.mem().write<double>(fmtAt, 22050.0);
            const uint32_t mono[8] = {0x6C70636D, 0x0C, 2, 1, 2, 1, 16, 0};
            proc.mem().copyIn(fmtAt + 8, mono, 32);
            const uint32_t fmtStatus = uint32_t(callAT("_AudioUnitSetProperty", {mixer, 8, 1, 1, fmtAt, 40}));
            proc.mem().write<uint32_t>(outAt, 40);
            callAT("_AudioUnitGetProperty", {mixer, 8, 1, 1, scratch + 88, outAt});
            const bool formatReadBack = proc.mem().read<double>(scratch + 88) == 22050.0 && proc.mem().read<uint32_t>(scratch + 88 + 28) == 1;
            const float off = 0.0f;
            uint32_t offBits;
            std::memcpy(&offBits, &off, 4);
            callAT("_AudioUnitSetParameter", {mixer, 5 /*k3DMixerParam_Enable*/, 1, 2, offBits, 0});
            callAT("_AUGraphInitialize", {graph});
            callAT("_AUGraphIsRunning", {graph, outAt});
            const bool runningBefore = proc.mem().read<uint8_t>(outAt) != 0;
            renderedFrames(4096);
            callAT("_AUGraphStart", {graph});
            for (int i = 0; i < 100 && (!calls0 || !calls1); ++i) Foundation::mainRunLoop(proc).runOnce(cpu, 0.02);
            callAT("_AUGraphIsRunning", {graph, outAt});
            const bool runningAfter = proc.mem().read<uint8_t>(outAt) != 0;
            const size_t mixed = renderedFrames(1024);
            callAT("_AUGraphStop", {graph});
            callAT("_DisposeAUGraph", {graph});
            check(graph != 0 && mixer != 0 && nodeCount == 2 && fmtStatus == 0 && calls0 > 0 && calls1 > 0 && busSeen0 == 0 && busSeen1 == 1,
                  "AUGraphStart pulls every mixer input from its own callback, with its input number");
            check(frames1 > 200 && frames1 < 240, "a 22050 Hz mixer input is asked for 22050 Hz worth of frames");
            check(calls2 == 0, "a mixer input disabled with k3DMixerParam_Enable is not pulled");
            check(formatReadBack, "a mixer input reads back the stream format set on it");
            check(mixed > 0 && !runningBefore && runningAfter, "the mixer inputs are audible, and AUGraphIsRunning follows start");
        }

        // OpenGL ES 1.1 Fixed-Function + 2D Texture + glDrawElements test
        const ObjC::Class ctxCls = r.getClass("EAGLContext");
        const ObjC::id gl1Ctx = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, ctxCls, r.selAlloc)), "initWithAPI:", {1}));
        r.send(cpu, ctxCls, "setCurrentContext:", {gl1Ctx});
        auto callGL = [&](const char* sym, std::initializer_list<uint32_t> args) -> uint64_t {
            auto addr = proc.lookupSymbol(sym);
            return proc.callGuest(cpu, *addr, args);
        };
        callGL("_glViewport", {0, 0, 64, 64});
        callGL("_glMatrixMode", {0x1701 /* GL_PROJECTION */});
        callGL("_glLoadIdentity", {});
        callGL("_glOrthof", {UIKit::fbits(-1.0f), UIKit::fbits(1.0f), UIKit::fbits(-1.0f), UIKit::fbits(1.0f), UIKit::fbits(-1.0f), UIKit::fbits(1.0f)});
        callGL("_glMatrixMode", {0x1700 /* GL_MODELVIEW */});
        callGL("_glLoadIdentity", {});
        // Create a 2x2 pure red RGBA texture
        const uint32_t texIds = proc.heap().allocZeroed(32);
        callGL("_glGenTextures", {1, texIds});
        const uint32_t tid = proc.mem().read<uint32_t>(texIds);
        callGL("_glBindTexture", {0x0DE1 /* GL_TEXTURE_2D */, tid});
        const uint8_t redTex[16] = {
            255,0,0,255, 255,0,0,255,
            255,0,0,255, 255,0,0,255
        };
        const uint32_t texMem = proc.heap().alloc(16);
        proc.mem().copyIn(texMem, redTex, 16);
        callGL("_glTexImage2D", {0x0DE1, 0, 0x1908, 2, 2, 0, 0x1908, 0x1401, texMem});
        callGL("_glEnable", {0x0DE1 /* GL_TEXTURE_2D */});
        callGL("_glColor4f", {UIKit::fbits(1.0f), UIKit::fbits(1.0f), UIKit::fbits(1.0f), UIKit::fbits(1.0f)});
        const float quadVerts[] = { -1,-1,  1,-1,  1,1,  -1,1 };
        const float quadUVs[]   = {  0, 0,  1, 0,  1,1,   0,1 };
        const uint16_t quadIdx[] = { 0, 1, 2, 0, 2, 3 };
        const uint32_t vMem = proc.heap().alloc(sizeof quadVerts);
        const uint32_t uvMem = proc.heap().alloc(sizeof quadUVs);
        const uint32_t iMem = proc.heap().alloc(sizeof quadIdx);
        proc.mem().copyIn(vMem, quadVerts, sizeof quadVerts);
        proc.mem().copyIn(uvMem, quadUVs, sizeof quadUVs);
        proc.mem().copyIn(iMem, quadIdx, sizeof quadIdx);
        callGL("_glEnableClientState", {0x8074 /* GL_VERTEX_ARRAY */});
        callGL("_glEnableClientState", {0x8078 /* GL_TEXTURE_COORD_ARRAY */});
        callGL("_glVertexPointer", {2, 0x1406, 0, vMem});
        callGL("_glTexCoordPointer", {2, 0x1406, 0, uvMem});
        callGL("_glDrawElements", {0x0004 /* GL_TRIANGLES */, 6, 0x1403 /* GL_UNSIGNED_SHORT */, iMem});
        callGL("_glReadPixels", {32, 32, 1, 1, 0x1908, 0x1401, texIds + 8});
        const uint8_t r1 = proc.mem().read<uint8_t>(texIds + 8);
        const uint8_t g1 = proc.mem().read<uint8_t>(texIds + 9);
        const uint8_t b1 = proc.mem().read<uint8_t>(texIds + 10);
        check(r1 == 255 && g1 == 0 && b1 == 0, "OpenGL ES 1.1 fixed-function ortho + 2D texture + glDrawElements");

        // Point size array client state (B-111)
        check(!callGL("_glIsEnabled", {0x8B9C /* GL_POINT_SIZE_ARRAY_OES */}), "glPointSizePointerOES initial state disabled");
        callGL("_glEnableClientState", {0x8B9C});
        check(callGL("_glIsEnabled", {0x8B9C}), "glEnableClientState enables GL_POINT_SIZE_ARRAY_OES");
        const float ptSizes[] = {12.0f};
        const uint32_t ptSizeMem = proc.heap().alloc(sizeof ptSizes);
        proc.mem().copyIn(ptSizeMem, ptSizes, sizeof ptSizes);
        callGL("_glPointSizePointerOES", {0x1406 /* GL_FLOAT */, 0, ptSizeMem});
        callGL("_glDisableClientState", {0x8B9C});
        check(!callGL("_glIsEnabled", {0x8B9C}), "glDisableClientState disables GL_POINT_SIZE_ARRAY_OES");
        proc.heap().free(ptSizeMem);

        // Fixed-function path shares the raster clip. A frustum triangle with
        // every vertex behind the camera (eye z>0 => clip w<0) must not paint.
        callGL("_glDisable", {0x0DE1 /* GL_TEXTURE_2D */});
        callGL("_glDisable", {0x0B44 /* GL_CULL_FACE */});
        callGL("_glMatrixMode", {0x1701 /* GL_PROJECTION */});
        callGL("_glLoadIdentity", {});
        callGL("_glFrustumf", {UIKit::fbits(-1.f), UIKit::fbits(1.f), UIKit::fbits(-1.f), UIKit::fbits(1.f), UIKit::fbits(1.f), UIKit::fbits(10.f)});
        callGL("_glMatrixMode", {0x1700 /* GL_MODELVIEW */});
        callGL("_glLoadIdentity", {});
        callGL("_glColor4f", {UIKit::fbits(1.f), UIKit::fbits(0.f), UIKit::fbits(0.f), UIKit::fbits(1.f)});
        callGL("_glClearColor", {UIKit::fbits(0.f), UIKit::fbits(0.f), UIKit::fbits(1.f), UIKit::fbits(1.f)});
        const float frontEye[] = {-0.5f, -0.5f, -2.f,  0.5f, -0.5f, -2.f,  0.f, 0.5f, -2.f};
        const float behindEye[] = {-0.5f, -0.5f, 1.f,  0.5f, -0.5f, 1.f,  0.f, 0.5f, 1.f};
        const uint32_t eyeMem = proc.heap().alloc(sizeof frontEye + sizeof behindEye);
        proc.mem().copyIn(eyeMem, frontEye, sizeof frontEye);
        proc.mem().copyIn(eyeMem + sizeof frontEye, behindEye, sizeof behindEye);
        callGL("_glVertexPointer", {3, 0x1406, 0, eyeMem});
        callGL("_glClear", {0x4000});
        callGL("_glDrawArrays", {4, 0, 3});
        callGL("_glReadPixels", {32, 32, 1, 1, 0x1908, 0x1401, texIds + 8});
        const uint8_t frontR = proc.mem().read<uint8_t>(texIds + 8);
        const uint8_t frontB = proc.mem().read<uint8_t>(texIds + 10);
        callGL("_glVertexPointer", {3, 0x1406, 0, eyeMem + uint32_t(sizeof frontEye)});
        callGL("_glClear", {0x4000});
        callGL("_glDrawArrays", {4, 0, 3});
        callGL("_glReadPixels", {32, 32, 1, 1, 0x1908, 0x1401, texIds + 8});
        const uint8_t backR = proc.mem().read<uint8_t>(texIds + 8);
        const uint8_t backG = proc.mem().read<uint8_t>(texIds + 9);
        const uint8_t backB = proc.mem().read<uint8_t>(texIds + 10);
        std::printf("  clip gles1 front (32,32)=%u,%u behind=%u,%u,%u\n",
                    frontR, frontB, backR, backG, backB);
        check(frontR == 255 && frontB == 0 && backR == 0 && backG == 0 && backB == 255,
              "GLES1 frustum clips a triangle behind the camera and still draws one in front of the near plane");

        // GLES1 color and UV stay screen-linear after clipping. The clip lerps
        // them in clip space so the new edge vertex is correct, and the fill
        // does not reweight by 1/w. Perspective-correct weighting of this same
        // triangle is about (131,84,40); the framebuffer must match (66,169,20).
        callGL("_glMatrixMode", {0x1701 /* GL_PROJECTION */});
        callGL("_glLoadIdentity", {});
        callGL("_glMatrixMode", {0x1700 /* GL_MODELVIEW */});
        callGL("_glLoadIdentity", {});
        callGL("_glEnableClientState", {0x8076 /* GL_COLOR_ARRAY */});
        const float gles1Pos[] = {
            -0.5f, -0.5f, 0.f, 1.f,
             2.0f, -2.0f, 0.f, 4.f,
             0.0f,  0.5f,-2.f, 1.f,
        };
        const float gles1Col[] = {
            1.f, 0.f, 0.f, 1.f,
            0.f, 1.f, 0.f, 1.f,
            0.f, 0.f, 1.f, 1.f,
        };
        const uint32_t pMem = proc.heap().alloc(sizeof gles1Pos);
        const uint32_t cMem = proc.heap().alloc(sizeof gles1Col);
        proc.mem().copyIn(pMem, gles1Pos, sizeof gles1Pos);
        proc.mem().copyIn(cMem, gles1Col, sizeof gles1Col);
        callGL("_glVertexPointer", {4, 0x1406, 0, pMem});
        callGL("_glColorPointer", {4, 0x1406, 0, cMem});
        callGL("_glClear", {0x4000});
        callGL("_glDrawArrays", {4, 0, 3});
        callGL("_glReadPixels", {38, 18, 1, 1, 0x1908, 0x1401, texIds + 8});
        const uint8_t linR = proc.mem().read<uint8_t>(texIds + 8);
        const uint8_t linG = proc.mem().read<uint8_t>(texIds + 9);
        const uint8_t linB = proc.mem().read<uint8_t>(texIds + 10);
        std::printf("  clip gles1 screen-linear (38,18)=%u,%u,%u\n", linR, linG, linB);
        const bool screenLinear = std::abs(int(linR) - 66) <= 3 && std::abs(int(linG) - 169) <= 3 && std::abs(int(linB) - 20) <= 3;
        const bool perspective = std::abs(int(linR) - 131) <= 3 && std::abs(int(linG) - 84) <= 3 && std::abs(int(linB) - 40) <= 3;
        check(screenLinear && !perspective,
              "GLES1 color after clipping is screen-linear (66,169,20), matching the fixed-function fill");

        // CommonCrypto digests against the standard "abc" vectors.
        auto hex = [](const std::vector<uint8_t>& v) { std::string h; char b[3]; for (uint8_t c : v) { std::snprintf(b, 3, "%02x", c); h += b; } return h; };
        check(hex(Runtime::hvmDigest(0, "abc")) == "900150983cd24fb0d6963f7d28e17f72" &&
              hex(Runtime::hvmDigest(1, "abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d" &&
              hex(Runtime::hvmDigest(2, "abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" &&
              hex(Runtime::hvmDigest(1, std::string(1000, 'a'))).size() == 40,
              "CC_MD5 / CC_SHA1 / CC_SHA256 match the reference digests");
        // CCCrypt / CCCryptor (guest entry points, forwarded to the host crypto library)
        // against NIST SP 800-38A F.2.1 (AES-128-CBC) and FIPS-197 C.1 (AES-128-ECB).
        if (Runtime::HostCrypto::available()) {
            auto call = [&](const char* sym, std::initializer_list<uint32_t> args) { return uint32_t(proc.callGuest(cpu, *proc.lookupSymbol(sym), args)); };
            auto unhex = [](const char* s) { std::vector<uint8_t> v; for (; s[0] && s[1]; s += 2) v.push_back(uint8_t(std::stoul(std::string(s, 2), nullptr, 16))); return v; };
            auto put = [&](const std::vector<uint8_t>& v) { const uint32_t g = proc.heap().allocZeroed(uint32_t(v.size()) + 16); proc.mem().copyIn(g, v.data(), v.size()); return g; };
            auto get = [&](uint32_t g, size_t n) { std::vector<uint8_t> v(n); proc.mem().copyOut(v.data(), g, n); return v; };
            const auto key = unhex("2b7e151628aed2a6abf7158809cf4f3c"), iv = unhex("000102030405060708090a0b0c0d0e0f");
            const auto pt = unhex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51");
            const char* ctHex = "7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2";
            const uint32_t gk = put(key), giv = put(iv), gpt = put(pt), out = proc.heap().allocZeroed(64), moved = proc.heap().allocZeroed(4);
            // CCCrypt(encrypt, AES, no padding, key, 16, iv, in, 32, out, 64, &moved)
            const uint32_t st = call("_CCCrypt", {0, 0, 0, gk, 16, giv, gpt, 32, out, 64, moved});
            const bool encOk = st == 0 && proc.mem().read<uint32_t>(moved) == 32 && hex(get(out, 32)) == ctHex;
            // Decrypt it back with PKCS7 after encrypting with PKCS7: round trip of 32 bytes yields 48 then 32.
            const uint32_t padded = proc.heap().allocZeroed(64), back = proc.heap().allocZeroed(64);
            const uint32_t st2 = call("_CCCrypt", {0, 0, 1, gk, 16, giv, gpt, 32, padded, 64, moved});
            const uint32_t n2 = proc.mem().read<uint32_t>(moved);
            const uint32_t st3 = call("_CCCrypt", {1, 0, 1, gk, 16, giv, padded, n2, back, 64, moved});
            const bool roundOk = st2 == 0 && n2 == 48 && st3 == 0 && proc.mem().read<uint32_t>(moved) == 32 && get(back, 32) == pt;
            // Too small an output buffer reports kCCBufferTooSmall and the needed size.
            const uint32_t st4 = call("_CCCrypt", {0, 0, 1, gk, 16, giv, gpt, 32, out, 16, moved});
            const bool smallOk = int32_t(st4) == -4301 && proc.mem().read<uint32_t>(moved) == 48;
            // CCCryptorCreate/Update/Final, ECB mode, FIPS-197 C.1, fed in two pieces.
            const auto k2 = unhex("000102030405060708090a0b0c0d0e0f"), p2 = unhex("00112233445566778899aabbccddeeff");
            const uint32_t gk2 = put(k2), gp2 = put(p2), ref = proc.heap().allocZeroed(4), o2 = proc.heap().allocZeroed(32);
            const uint32_t c1 = call("_CCCryptorCreate", {0, 0, 2, gk2, 16, 0, ref});
            const uint32_t r = proc.mem().read<uint32_t>(ref);
            const uint32_t u1 = call("_CCCryptorUpdate", {r, gp2, 5, o2, 32, moved});
            const uint32_t m1 = proc.mem().read<uint32_t>(moved);
            const uint32_t u2 = call("_CCCryptorUpdate", {r, gp2 + 5, 11, o2 + m1, 32 - m1, moved});
            const uint32_t m2 = proc.mem().read<uint32_t>(moved);
            const uint32_t f = call("_CCCryptorFinal", {r, o2 + m1 + m2, 16, moved});
            call("_CCCryptorRelease", {r});
            const bool streamOk = c1 == 0 && r && u1 == 0 && m1 == 0 && u2 == 0 && m2 == 16 && f == 0 &&
                                  hex(get(o2, 16)) == "69c4e0d86a7b0430d8cdb78070b4c55a";
            // A wrong AES key size is kCCKeySizeError.
            const uint32_t st5 = call("_CCCrypt", {0, 0, 1, gk, 15, giv, gpt, 32, out, 64, moved});
            check(encOk && roundOk && smallOk && streamOk && int32_t(st5) == -4310,
                  "CCCrypt / CCCryptor AES match NIST vectors (CBC, ECB, PKCS7 round trip, buffer and key-size errors)");
        } else {
            std::printf("  (CCCrypt: no host crypto library in this build; skipped)\n");
        }
        // Incremental CCHmacInit/Update/Final (guest entry points) vs RFC 4231 test case 2.
        {
            auto call = [&](const char* sym, std::initializer_list<uint32_t> args) { return uint32_t(proc.callGuest(cpu, *proc.lookupSymbol(sym), args)); };
            const std::string key = "Jefe", data = "what do ya want for nothing?";
            const uint32_t ctx = proc.heap().allocZeroed(384), k = proc.allocCString(key), d = proc.allocCString(data), mac = proc.heap().allocZeroed(32);
            call("_CCHmacInit", {ctx, 2, k, uint32_t(key.size())});
            call("_CCHmacUpdate", {ctx, d, 10});
            call("_CCHmacUpdate", {ctx, d + 10, uint32_t(data.size() - 10)});
            call("_CCHmacFinal", {ctx, mac});
            std::vector<uint8_t> got(proc.mem().host(mac), proc.mem().host(mac) + 32);
            check(hex(got) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
                  "CCHmacInit/Update/Final (SHA-256) matches RFC 4231");
        }

        // zlib: compress + uncompress through the guest entry points.
        {
            auto call = [&](const char* sym, std::initializer_list<uint32_t> args) { return uint32_t(proc.callGuest(cpu, *proc.lookupSymbol(sym), args)); };
            std::string text;
            for (int i = 0; i < 200; ++i) text += "HydrogenVM zlib round trip " + std::to_string(i) + "\n";
            const uint32_t src = proc.heap().alloc(uint32_t(text.size())), packed = proc.heap().alloc(8192), out = proc.heap().alloc(uint32_t(text.size()) + 16);
            const uint32_t lenp = proc.heap().alloc(8);
            proc.mem().copyIn(src, text.data(), text.size());
            proc.mem().write<uint32_t>(lenp, 8192);
            const uint32_t rc1 = call("_compress", {packed, lenp, src, uint32_t(text.size())});
            const uint32_t packedLen = proc.mem().read<uint32_t>(lenp);
            proc.mem().write<uint32_t>(lenp + 4, uint32_t(text.size()) + 16);
            const uint32_t rc2 = call("_uncompress", {out, lenp + 4, packed, packedLen});
            const uint32_t outLen = proc.mem().read<uint32_t>(lenp + 4);
            const bool same = outLen == text.size() && std::memcmp(proc.mem().host(out), text.data(), text.size()) == 0;
            // Streaming inflate of the same data in 100-byte output chunks.
            const uint32_t zs = proc.heap().allocZeroed(56);
            const uint32_t ver = proc.allocCString("1.2.5");
            const uint32_t rc3 = call("_inflateInit_", {zs, ver, 56});
            proc.mem().write<uint32_t>(zs + 0, packed);
            proc.mem().write<uint32_t>(zs + 4, packedLen);
            std::string got;
            const uint32_t chunk = proc.heap().alloc(100);
            int32_t zr = 0;
            for (int i = 0; i < 1000; ++i) {
                proc.mem().write<uint32_t>(zs + 12, chunk);
                proc.mem().write<uint32_t>(zs + 16, 100);
                zr = int32_t(call("_inflate", {zs, 0}));
                const uint32_t produced = 100 - proc.mem().read<uint32_t>(zs + 16);
                got.append(reinterpret_cast<const char*>(proc.mem().host(chunk)), produced);
                if (zr != 0) break;
            }
            call("_inflateEnd", {zs});
            check(rc1 == 0 && rc2 == 0 && same && packedLen < text.size() / 4 && rc3 == 0 && zr == 1 && got == text,
                  "zlib compress/uncompress and streaming inflate on guest buffers");
        }

        // std::ostringstream: formatted output through our stringbuf, read back with str().
        {
            auto call = [&](const char* sym, std::initializer_list<uint32_t> args) { return uint32_t(proc.callGuest(cpu, *proc.lookupSymbol(sym), args)); };
            const uint32_t oss = proc.heap().allocZeroed(180);
            call("__ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEEC1ESt13_Ios_Openmode", {oss, 16});
            const uint32_t hello = proc.allocCString("score=");
            call("__ZSt16__ostream_insertIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_PKS3_i", {oss, hello, 6});
            call("__ZNSolsEi", {oss, uint32_t(-42)});
            // Hex with showbase via the stream's flags (ios_base at +44).
            const uint32_t ios = oss + uint32_t(int32_t(proc.mem().read<uint32_t>(proc.mem().read<uint32_t>(oss) - 12)));
            proc.mem().write<uint32_t>(ios + 12, (proc.mem().read<uint32_t>(ios + 12) & ~(2u | 8u | 64u)) | 8u | 512u);
            call("__ZSt16__ostream_insertIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_PKS3_i", {oss, proc.allocCString(" "), 1});
            call("__ZNSo9_M_insertImEERSoT_", {oss, 255});
            proc.mem().write<uint32_t>(ios + 12, (proc.mem().read<uint32_t>(ios + 12) & ~(8u | 512u)) | 2u);
            proc.mem().write<uint32_t>(ios + 8, 6);   // width 6, right-aligned
            call("__ZNSo9_M_insertImEERSoT_", {oss, 7});
            const uint32_t strRet = proc.heap().allocZeroed(4);
            call("__ZNKSt15basic_stringbufIcSt11char_traitsIcESaIcEE3strEv", {strRet, oss + 4});
            const uint32_t data = proc.mem().read<uint32_t>(strRet);
            const std::string v = proc.readCString(data);
            const uint32_t strRet2 = proc.heap().allocZeroed(4);
            call("__ZNKSt19basic_ostringstreamIcSt11char_traitsIcESaIcEE3strEv", {strRet2, oss});
            const std::string v2 = proc.readCString(proc.mem().read<uint32_t>(strRet2));
            call("__ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEED1Ev", {oss});
            check(v == "score=-42 0xff     7" && v2 == v, "std::ostringstream formatting (int, hex+showbase, width) and str()");
            proc.heap().free(strRet);
            proc.heap().free(strRet2);
        }
        // GCC 4.0-era member operator<< overloads and basic_ios state checks return the stream
        // (Temple Run chained `os << x << endl` and got a NULL stream from the stubs).
        {
            auto call = [&](const char* sym, std::initializer_list<uint32_t> args) { return uint32_t(proc.callGuest(cpu, *proc.lookupSymbol(sym), args)); };
            const uint32_t oss = proc.heap().allocZeroed(180);
            call("__ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEEC1ESt13_Ios_Openmode", {oss, 16});
            const uint32_t endl = *proc.lookupSymbol("__ZSt4endlIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_");
            float half = 1.5f; uint32_t halfBits; std::memcpy(&halfBits, &half, 4);
            bool chained = call("__ZNSolsEb", {oss, 1}) == oss && call("__ZNSolsEm", {oss, 7}) == oss &&
                           call("__ZNSolsEf", {oss, halfBits}) == oss && call("__ZNSolsEPFRSoS_E", {oss, endl}) == oss;
            const uint32_t ios = oss + uint32_t(int32_t(proc.mem().read<uint32_t>(proc.mem().read<uint32_t>(oss) - 12)));
            const bool okState = call("__ZNKSt9basic_iosIcSt11char_traitsIcEEcvPvEv", {ios}) == ios &&
                                 (call("__ZNKSt9basic_iosIcSt11char_traitsIcEEntEv", {ios}) & 0xff) == 0 &&
                                 (call("__ZNKSt9basic_iosIcSt11char_traitsIcEE4goodEv", {ios}) & 0xff) == 1;
            const uint32_t strRet = proc.heap().allocZeroed(4);
            call("__ZNKSt19basic_ostringstreamIcSt11char_traitsIcESaIcEE3strEv", {strRet, oss});
            const std::string v = proc.readCString(proc.mem().read<uint32_t>(strRet));
            call("__ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEED1Ev", {oss});
            if (!(chained && okState && v == "171.5\n")) std::printf("    old ostream members: chained %d state %d text '%s'\n", chained, okState, v.c_str());
            check(chained && okState && v == "171.5\n", "libstdc++ member operator<< (bool, unsigned long, float, manipulator) and basic_ios state checks");
        }

        // Apple XNU cdefs.h: PLATFORM_iPhoneOS has ONLY_64_BIT_INO_T=1,
        // so unsuffixed stat has the same layout as the $INODE64 alias.
        {
            auto call = [&](const char* sym, std::initializer_list<uint32_t> args) { return uint32_t(proc.callGuest(cpu, *proc.lookupSymbol(sym), args)); };
            const std::string file = proc.homePath() + "/Documents/hvm_stat_layout.bin";
            { std::ofstream o(file, std::ios::binary); o << std::string(1234, 'x'); }
            const uint32_t path = proc.allocCString(file);
            const uint32_t buf = proc.heap().allocZeroed(128);
            for (uint32_t i = 108; i < 128; i += 4) proc.mem().write<uint32_t>(buf + i, 0xA5A5A5A5);
            const uint32_t r1 = call("_stat", {path, buf});
            const bool plain = r1 == 0 && proc.mem().read<uint64_t>(buf + 60) == 1234 &&
                               (proc.mem().read<uint16_t>(buf + 4) & 0170000) == 0100000 &&
                               proc.mem().read<uint32_t>(buf + 108) == 0xA5A5A5A5 && proc.mem().read<uint32_t>(buf + 124) == 0xA5A5A5A5;
            std::memset(proc.mem().host(buf), 0, 128);
            const uint32_t r2 = call("_stat$INODE64", {path, buf});
            const bool wide = r2 == 0 && proc.mem().read<uint64_t>(buf + 60) == 1234 &&
                              (proc.mem().read<uint16_t>(buf + 4) & 0170000) == 0100000;
            check(plain && wide, "stat: unsuffixed iPhone symbols and $INODE64 use the 108-byte ARMv7 layout");
            proc.heap().free(buf);
        }

        // CFNumber NaN/infinity constants, NSObject's isa ivar offset, proxy constants.
        {
            auto call = [&](const char* sym, std::initializer_list<uint32_t> args) { return uint32_t(proc.callGuest(cpu, *proc.lookupSymbol(sym), args)); };
            const uint32_t out = proc.heap().allocZeroed(8);
            auto num = [&](const char* sym) {
                const uint32_t n = proc.mem().read<uint32_t>(*proc.lookupSymbol(sym));
                call("_CFNumberGetValue", {n, 13 /* kCFNumberDoubleType */, out});
                return proc.mem().read<double>(out);
            };
            const bool nums = std::isnan(num("_kCFNumberNaN")) && num("_kCFNumberPositiveInfinity") == HUGE_VAL &&
                              num("_kCFNumberNegativeInfinity") == -HUGE_VAL;
            const bool ivar = proc.mem().read<uint32_t>(*proc.lookupSymbol("_OBJC_IVAR_$_NSObject.isa")) == 0;
            const uint32_t proxy = proc.mem().read<uint32_t>(*proc.lookupSymbol("_NSURLProtectionSpaceHTTPProxy"));
            const bool prox = proxy && Foundation::utf8(cx, proxy) == "http";
            check(nums && ivar && prox, "kCFNumberNaN/±Infinity hold real CFNumbers; OBJC_IVAR_$_NSObject.isa is 0; NSURLProtectionSpaceHTTPProxy is @\"http\"");
        }

        // HVM_ZOMBIES=1: a message to a freed object names its class (run only in that mode).
        if (const char* z = std::getenv("HVM_ZOMBIES"); z && *z == '1') {
            const ObjC::id o = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, r.getClass("NSObject"), r.selAlloc)), "init"));
            r.send(cpu, o, "release");
            std::string msg;
            try { r.send(cpu, o, "description"); } catch (const std::exception& e) { msg = e.what(); }
            check(msg.find("-[NSObject description]: message sent to deallocated instance") != std::string::npos,
                  "HVM_ZOMBIES names the class of a freed object that is messaged");
        }

        // Cap is 16384 records in Runtime::disposeInstance. Guest memory of an
        // evicted zombie stays allocated. Runs without HVM_ZOMBIES set, and
        // fails if the cap is removed.
        check(zombieMetadataStaysCapped(),
              "zombie metadata stops at 16384 records and the evicted object's guest memory stays allocated");

        // A virtual call through a vtable nothing provides names the class and slot.
        {
            const uint32_t vt = proc.makeMissingStub("__ZTV13HvmNoSuchType");
            const uint32_t slot3 = proc.mem().read<uint32_t>(vt + 8 + 3 * 4);
            std::string msg;
            try { proc.callGuest(cpu, slot3, {0}); } catch (const std::exception& e) { msg = e.what(); }
            const uint32_t vtt = proc.makeMissingStub("__ZTT13HvmNoSuchType");
            const uint32_t vptr = proc.mem().read<uint32_t>(vtt);
            std::string msg2;
            try { proc.callGuest(cpu, proc.mem().read<uint32_t>(vptr), {0}); } catch (const std::exception& e) { msg2 = e.what(); }
            check(msg.find("virtual function #3 of vtable for HvmNoSuchType") != std::string::npos &&
                  msg2.find("virtual function #0 of VTT for HvmNoSuchType") != std::string::npos &&
                  proc.mem().read<uint32_t>(vptr - 12) == 0,
                  "Missing vtables and VTTs report the class and slot of a virtual call instead of jumping to 0");
        }

        // T2a (#257): every call to a missing or approximated API is recorded with its count, caller,
        // backtrace and time since launch, and the run is tainted from the first hit on.
        {
            ProcessConfig mcfg;
            const std::string mhome = "/tmp/hvm_test_missing_" + std::to_string(getpid());
            std::error_code mec;
            std::filesystem::create_directories(mhome + "/Test.app", mec);
            mcfg.homePath = mhome;
            mcfg.bundlePath = mhome + "/Test.app";
            mcfg.stopOnMissingFunction = false;   // keep going: the stub returns 0 instead of throwing
            Process mp(mcfg);
            auto& mcpu = mp.mainThread().cpu;
            check(!mp.tainted() && mp.missingApis().totalHits() == 0 && mp.missingApis().records().empty(),
                  "a fresh process is not tainted and has no missing-API records");

            // A guest frame-pointer chain: fp0 -> {fp1, lr1}, fp1 -> {0, lr2}.
            constexpr uint32_t kBase = 0xbee00000, kLr0 = 0x00008123, kLr1 = 0x00009001, kLr2 = 0x0000a001;
            mp.mem().map(kBase, 1u << 16, Memory::kProtRW);
            const uint32_t fp0 = kBase + 0x100, fp1 = kBase + 0x200;
            mp.mem().write<uint32_t>(fp0, fp1);  mp.mem().write<uint32_t>(fp0 + 4, kLr1);
            mp.mem().write<uint32_t>(fp1, 0);    mp.mem().write<uint32_t>(fp1 + 4, kLr2);

            // The real stub for a function nobody implements, called as a guest bl would: LR = caller.
            mp.makeMissingStub("_hvm_t2a_no_such_function");
            auto stub = mp.hostFunctionFor("_hvm_t2a_no_such_function");
            check(bool(stub), "a missing function resolves to a host stub that can be called");
            mcpu.r[7] = fp0;
            mcpu.r[CPU::kLR] = kLr0;
            if (stub) stub(mcpu);
            const double sinceLaunch = std::chrono::duration<double>(std::chrono::steady_clock::now() - mp.perf().origin).count();
            auto recs = mp.missingApis().records();
            const bool one = recs.size() == 1 && recs[0].count == 1 &&
                             recs[0].name.find("hvm_t2a_no_such_function") != std::string::npos;
            check(one && recs[0].callerPc == kLr0 && recs[0].caller.find("00008123") != std::string::npos,
                  "a missing function's record has its count and the guest address it was called from");
            check(one && recs[0].backtrace.size() == 4 && recs[0].backtrace[1].find("00008123") != std::string::npos &&
                      recs[0].backtrace[2].find("00009001") != std::string::npos && recs[0].backtrace[3].find("0000a001") != std::string::npos,
                  "a missing function's record has the guest backtrace of its first call, innermost first");
            check(one && recs[0].firstSeconds >= 0 && recs[0].firstSeconds <= sinceLaunch && recs[0].lastSeconds == recs[0].firstSeconds,
                  "a missing function's record has the time since launch of its first call");
            check(mp.tainted() && mp.missingApis().totalHits() == 1, "the first missing-API hit taints the run");

            // More calls from elsewhere: the count and last time move, the first caller and backtrace stay.
            const std::vector<std::string> firstBacktrace = one ? recs[0].backtrace : std::vector<std::string>{};
            mcpu.r[7] = 0;
            mcpu.r[CPU::kLR] = kLr2;
            if (stub) { stub(mcpu); stub(mcpu); }
            recs = mp.missingApis().records();
            check(recs.size() == 1 && recs[0].count == 3 && recs[0].callerPc == kLr0 && recs[0].backtrace == firstBacktrace &&
                      recs[0].lastSeconds >= recs[0].firstSeconds && mp.missingApis().totalHits() == 3,
                  "repeat calls raise the count and keep the first caller and backtrace");
            check(mp.missingCount() == 1 && mp.missingSymbols().size() == 1,
                  "the once-per-name missing list and count are unchanged by repeat calls");

            // Any reportMissing caller (ObjC selectors, framework stubs) is counted, not only function stubs.
            mp.reportMissing("-[HvmNoSuchClass noSuchSelector] (returned nil)");
            check(mp.missingApis().distinct() == 2 && mp.missingApis().totalHits() == 4 && mp.missingCount() == 2,
                  "reportMissing from any call site adds a record");

            // The report JSON carries the flag, the hit count and the records.
            const std::string fields = mp.missingApis().reportFields("  ");
            check(fields.find("\"tainted\": true") != std::string::npos && fields.find("\"stubHits\": 4") != std::string::npos &&
                      fields.find("\"missingApis\": [") != std::string::npos &&
                      fields.find("hvm_t2a_no_such_function\", \"count\": 3") != std::string::npos &&
                      fields.find("\"callerPc\": " + std::to_string(kLr0)) != std::string::npos,
                  "the report fields expose tainted, stubHits and each API's count and caller");
        }

        // T2a (#257): the log on its own, with no Process.
        {
            MissingApiLog log;
            int siteCalls = 0;
            auto site = [&](MissingApiLog::Site& s) {
                ++siteCalls;
                s.callerPc = 7;
                s.caller = "caller";
                for (int i = 0; i < 40; ++i) s.backtrace.push_back("frame" + std::to_string(i));
            };
            log.hit("b", 2.0, site);
            log.hit("a", 3.0, site);
            log.hit("b", 4.0, site);
            const auto recs = log.records();
            bool orderOk = recs.size() == 2 && recs[0].name == "b" && recs[1].name == "a";
            check(orderOk && recs[0].count == 2 && recs[0].firstSeconds == 2.0 && recs[0].lastSeconds == 4.0 && recs[1].count == 1,
                  "records come back in first-hit order with their counts and first and last times");
            check(siteCalls == 2 && orderOk && recs[0].callerPc == 7 && recs[0].backtrace.size() == MissingApiLog::kMaxFrames &&
                      recs[0].backtrace.front() == "frame0" && recs[0].backtrace.back() == "frame" + std::to_string(MissingApiLog::kMaxFrames - 1),
                  "the caller is captured once per name and a long backtrace keeps its innermost frames");

            MissingApiLog full;
            for (size_t i = 0; i < MissingApiLog::kMaxNames + 10; ++i) full.hit("n" + std::to_string(i), 1.0, site);
            full.hit("n0", 2.0, site);
            const auto fr = full.records();
            check(full.distinct() == MissingApiLog::kMaxNames && full.totalHits() == MissingApiLog::kMaxNames + 11 &&
                      full.overflowHits() == 10 && !fr.empty() && fr[0].name == "n0" && fr[0].count == 2,
                  "the log stores at most kMaxNames APIs, still counts the rest, and keeps counting stored ones");

            MissingApiLog shared;
            int sharedSites = 0;
            std::vector<std::thread> workers;
            for (int t = 0; t < 4; ++t)
                workers.emplace_back([&] { for (int i = 0; i < 1000; ++i) shared.hit("shared", 1.0, [&](MissingApiLog::Site&) { ++sharedSites; }); });
            for (auto& w : workers) w.join();
            check(shared.totalHits() == 4000 && shared.records().size() == 1 && shared.records()[0].count == 4000 && sharedSites == 1,
                  "hits from several threads are all counted and the first-hit capture runs once");

            // A hit that locks the log later can carry an earlier timestamp (the time is read before the lock).
            MissingApiLog skew;
            auto noSite = [](MissingApiLog::Site&) {};
            skew.hit("_late", 5.0, noSite);
            skew.hit("_late", 2.0, noSite);
            skew.hit("_late", 9.0, noSite);
            const auto skewRecs = skew.records();
            check(skewRecs.size() == 1 && skewRecs[0].firstSeconds == 2.0 && skewRecs[0].lastSeconds == 9.0 && skewRecs[0].count == 3,
                  "Missing-API log keeps first/last times as chronological bounds even when hits arrive out of order (T2a)");

            // --launches N builds a new Process per launch but writes one report: absorb() carries the earlier launches over.
            MissingApiLog launch1, launch2, combined;
            launch1.hit("_only_first", 1.0, noSite);
            launch1.hit("_both", 2.0, noSite);
            launch2.hit("_both", 0.5, noSite);
            launch2.hit("_both", 3.0, noSite);
            MissingApiLog cleanLaunch;
            combined.absorb(launch1);
            combined.absorb(cleanLaunch);
            const bool carriedTaint = combined.tainted() && combined.totalHits() == 2;
            combined.absorb(launch2);
            const auto crecs = combined.records();
            check(carriedTaint && combined.tainted() && combined.totalHits() == 4 && combined.distinct() == 2 && crecs.size() == 2 &&
                      crecs[0].name == "_only_first" && crecs[0].count == 1 && crecs[1].name == "_both" && crecs[1].count == 3 &&
                      crecs[1].firstSeconds == 0.5 && crecs[1].lastSeconds == 3.0 &&
                      combined.reportFields("").find("\"tainted\": true") != std::string::npos,
                  "Missing-API log absorb() keeps a hit from an earlier launch tainted in the final report (T2a)");
            check(!cleanLaunch.tainted() && MissingApiLog().reportFields("").find("\"tainted\": false") != std::string::npos,
                  "A launch with no missing-API hits stays untainted (T2a)");

            check(MissingApiLog::jsonEscape("a\"b\\c\nd\x01") == "a\\\"b\\\\c\\nd\\u0001" &&
                      MissingApiLog::jsonEscape("\xff\xfe") == "??" && MissingApiLog::jsonEscape("h\xc3\xa9llo") == "h\xc3\xa9llo" &&
                      MissingApiLog::jsonEscape("\xed\xa0\x80") == "???" && MissingApiLog::jsonEscape("\xc3") == "?",
                  "JSON strings escape quotes and controls and replace bytes that are not valid UTF-8");
            const auto lines = MissingApiLog::splitLines("    a b\n    c  \n\n  \n");
            check(lines.size() == 2 && lines[0] == "a b" && lines[1] == "c", "backtrace text splits into one trimmed entry per frame");
        }

        // iconv forwarded to the host, and the getpw*_r family.
        {
            auto call = [&](const char* sym, std::initializer_list<uint32_t> args) { return uint32_t(proc.callGuest(cpu, *proc.lookupSymbol(sym), args)); };
            const uint32_t cd = call("_iconv_open", {proc.allocCString("UTF-16LE"), proc.allocCString("UTF-8")});
            const std::string text = "h\xc3\xa9llo";   // "héllo"
            const uint32_t in = proc.allocCString(text), out = proc.heap().allocZeroed(64), ptrs = proc.heap().allocZeroed(16);
            proc.mem().write<uint32_t>(ptrs + 0, in);
            proc.mem().write<uint32_t>(ptrs + 4, uint32_t(text.size()));
            proc.mem().write<uint32_t>(ptrs + 8, out);
            proc.mem().write<uint32_t>(ptrs + 12, 64);
            const uint32_t r = call("_iconv", {cd, ptrs, ptrs + 4, ptrs + 8, ptrs + 12});
            const bool conv = cd != 0xFFFFFFFFu && r == 0 && proc.mem().read<uint32_t>(ptrs + 4) == 0 &&
                              proc.mem().read<uint32_t>(ptrs + 12) == 64 - 10 &&
                              proc.mem().read<uint32_t>(ptrs + 8) == out + 10 &&
                              proc.mem().read<uint16_t>(out + 2) == 0x00E9;
            const bool closed = call("_iconv_close", {cd}) == 0;
            const uint32_t pwd = proc.heap().allocZeroed(40), res = proc.heap().allocZeroed(4);
            const bool pwOk = call("_getpwnam_r", {proc.allocCString("mobile"), pwd, 0, 0, res}) == 0 &&
                              proc.mem().read<uint32_t>(res) == pwd && proc.mem().read<uint32_t>(pwd + 8) == 501 &&
                              proc.readCString(proc.mem().read<uint32_t>(pwd + 28)) == proc.guestHomePath();
            const bool pwMissing = call("_getpwnam_r", {proc.allocCString("root"), pwd, 0, 0, res}) == 0 && proc.mem().read<uint32_t>(res) == 0;
            check(conv && closed && pwOk && pwMissing, "iconv converts UTF-8 to UTF-16LE through the host and advances the guest pointers; getpwnam_r finds only mobile");
        }

        // CVPixelBuffer holds real memory; AVAssetWriter accepts inputs; dlopen(NULL) is RTLD_DEFAULT.
        {
            auto call = [&](const char* sym, std::initializer_list<uint32_t> args) { return uint32_t(proc.callGuest(cpu, *proc.lookupSymbol(sym), args)); };
            const uint32_t out = proc.heap().allocZeroed(4);
            const bool made = call("_CVPixelBufferCreate", {0, 100, 20, 0x42475241, 0, out}) == 0;
            const uint32_t pbuf = proc.mem().read<uint32_t>(out);
            const uint32_t base = call("_CVPixelBufferGetBaseAddress", {pbuf}), bpr = call("_CVPixelBufferGetBytesPerRow", {pbuf});
            if (base) proc.mem().write<uint32_t>(base + bpr * 19 + 396, 0xAABBCCDD);   // last pixel is writable
            const bool pbOk = made && pbuf && call("_CVPixelBufferGetWidth", {pbuf}) == 100 && call("_CVPixelBufferGetHeight", {pbuf}) == 20 &&
                              bpr >= 400 && base && call("_CVPixelBufferLockBaseAddress", {pbuf, 0}) == 0;
            call("_CVPixelBufferRelease", {pbuf});
            const ObjC::id input = ObjC::id(r.send(cpu, r.getClass("AVAssetWriterInput"), "assetWriterInputWithMediaType:outputSettings:",
                                                   {Foundation::makeString(cx, "vide"), 0}));
            const uint32_t errOut = proc.heap().allocZeroed(4);
            proc.mem().write<uint32_t>(errOut, 0x1234);
            ObjC::id writer = ObjC::id(r.send(cpu, r.getClass("AVAssetWriter"), r.selAlloc));
            // initWithURL:fileType:error: takes the error pointer on the stack.
            const uint32_t sp0 = cpu.r[CPU::kSP];
            cpu.r[CPU::kSP] = sp0 - 16;
            proc.mem().write<uint32_t>(cpu.r[CPU::kSP], errOut);
            writer = ObjC::id(r.send(cpu, writer, "initWithURL:fileType:error:", {0, 0}));
            cpu.r[CPU::kSP] = sp0;
            const bool can = writer && input && (r.send(cpu, writer, "canAddInput:", {input}) & 0xFF) == 1;
            r.send(cpu, writer, "addInput:", {input});
            const bool writes = (r.send(cpu, writer, "startWriting") & 0xFF) == 1 && uint32_t(r.send(cpu, writer, "status")) == 1 &&
                                uint32_t(r.send(cpu, ObjC::id(r.send(cpu, writer, "inputs")), "count")) == 1 && proc.mem().read<uint32_t>(errOut) == 0;
            const bool rtldDefault = call("_dlopen", {0, 1}) == 0xFFFFFFFEu;
            check(pbOk && can && writes && rtldDefault,
                  "CVPixelBuffer has real memory; AVAssetWriter takes inputs and starts writing; dlopen(NULL) returns RTLD_DEFAULT");

            // T5c: Unity/Mono startup checklist assertions (Darwin stat, dlsym RTLD_DEFAULT, getpwuid, AudioQueue 44.1kHz PCM).
            const uint32_t symGetpid = proc.allocCString("getpid");
            const uint32_t dlsymRes = call("_dlsym", {0xFFFFFFFEu, symGetpid});
            check(dlsymRes != 0, "dlsym(RTLD_DEFAULT) resolves guest and HLE exports");
            // Populate the unloaded process's image with a unique synthetic guest export.
            auto& exports = const_cast<Loader::Image&>(proc.image()).exports;
            const char* guestExportName = "_hvm_test_guest_export";
            const bool noHleExport = !proc.lookupSymbol(guestExportName);
            const uint32_t guestExportAddr = proc.heap().allocZeroed(4);
            exports.emplace(guestExportName, guestExportAddr);
            const uint32_t guestDlsymRes = call("_dlsym", {0xFFFFFFFEu, proc.allocCString("hvm_test_guest_export")});
            check(noHleExport && guestExportAddr && guestDlsymRes == guestExportAddr, "dlsym(RTLD_DEFAULT) resolves a guest export");
            exports.erase(guestExportName);

            const uint32_t pw501 = call("_getpwuid", {501});
            const bool pw501Ok = pw501 && proc.readCString(proc.mem().read<uint32_t>(pw501)) == "mobile" &&
                                proc.mem().read<uint32_t>(pw501 + 8) == 501;
            check(pw501Ok, "getpwuid(501) returns mobile");

            const uint32_t stBuf = proc.heap().allocZeroed(128);
            const std::string stPath = proc.guestHomePath() + "/Documents/hvm_stat_test.txt";
            const uint32_t cStPath = proc.allocCString(stPath);
            const uint32_t fdSt = call("_open", {cStPath, 0x0602 /* O_RDWR | O_CREAT | O_TRUNC */, 0644});
            call("_pwrite", {fdSt, proc.allocCString("unity"), 5, 0, 0});   // off_t is two words: low in r3, high on the stack
            call("_close", {fdSt});
            const bool statOk = call("_stat", {cStPath, stBuf}) == 0;
            const uint16_t stMode = proc.mem().read<uint16_t>(stBuf + 4);
            const int64_t stSz = proc.mem().read<int64_t>(stBuf + 60);
            check(statOk && (stMode & 0100000) && stSz == 5, "Darwin 32-bit INODE64 stat mode and size offsets");

            const uint32_t asbdPcm = proc.heap().allocZeroed(40), outPcmAq = proc.heap().allocZeroed(4);
            proc.mem().write<double>(asbdPcm, 44100.0);
            proc.mem().write<uint32_t>(asbdPcm + 8, 0x6c70636d);   // lpcm
            proc.mem().write<uint32_t>(asbdPcm + 12, 12);
            proc.mem().write<uint32_t>(asbdPcm + 16, 4);
            proc.mem().write<uint32_t>(asbdPcm + 20, 1);
            proc.mem().write<uint32_t>(asbdPcm + 24, 4);
            proc.mem().write<uint32_t>(asbdPcm + 28, 2);           // 2 channels
            proc.mem().write<uint32_t>(asbdPcm + 32, 16);          // 16 bits
            const int32_t aqPcmRes = int32_t(call("_AudioQueueNewOutput", {asbdPcm, 0, 0, 0, 0, 0, outPcmAq}));
            const uint32_t qidPcm = proc.mem().read<uint32_t>(outPcmAq);
            if (qidPcm) call("_AudioQueueDispose", {qidPcm, 0});
            check(aqPcmRes == 0 && qidPcm != 0, "AudioQueue accepts 44.1 kHz stereo 16-bit PCM");
        }

        // A std::filebuf has a vtable, so app code can call its virtuals (sgetc, sbumpc, delete).
        {
            auto call = [&](const char* sym, std::initializer_list<uint32_t> args) { return uint32_t(proc.callGuest(cpu, *proc.lookupSymbol(sym), args)); };
            const std::string path = proc.guestHomePath() + "/Documents/hvm_filebuf.txt";
            { std::FILE* f = std::fopen(proc.hostPath(path).c_str(), "wb"); if (f) { std::fputs("Hi", f); std::fclose(f); } }
            const uint32_t fb = proc.heap().allocZeroed(128);
            call("__ZNSt13basic_filebufIcSt11char_traitsIcEEC1Ev", {fb});
            const uint32_t opened = call("__ZNSt13basic_filebufIcSt11char_traitsIcEE4openEPKcSt13_Ios_Openmode", {fb, proc.allocCString(path), 8});
            const uint32_t vptr = proc.mem().read<uint32_t>(fb);
            auto virt = [&](uint32_t slot, std::initializer_list<uint32_t> args) {
                std::vector<uint32_t> a{fb};
                a.insert(a.end(), args);
                return uint32_t(proc.callGuest(cpu, proc.mem().read<uint32_t>(vptr + 4 * slot), a));
            };
            const bool peek = vptr && virt(9, {}) == 'H';          // underflow
            const bool get = virt(10, {}) == 'H' && virt(10, {}) == 'i' && virt(10, {}) == 0xFFFFFFFFu;   // uflow
            check(opened == fb && peek && get, "std::filebuf has a vtable whose underflow/uflow read the open file");
        }

        // GCC-layout string-stream vtables (apps store __ZTV+12 / +32 inline), and
        // +[NSString stringWithString:] of a constant string is the constant.
        {
            const uint32_t ss = *proc.lookupSymbol("__ZTVSt18basic_stringstreamIcSt11char_traitsIcESaIcEE");
            const uint32_t oss = *proc.lookupSymbol("__ZTVSt19basic_ostringstreamIcSt11char_traitsIcESaIcEE");
            auto rd = [&](uint32_t a) { return int32_t(proc.mem().read<uint32_t>(a)); };
            const bool layout = rd(ss + 12 - 12) == 52 && rd(ss + 32 - 12) == 44 && rd(ss + 32 - 8) == -8 && rd(ss + 12) != 0 &&
                                rd(ss + 32) != 0 && rd(oss + 12 - 12) == 44 && rd(oss + 12) != 0;
            const ObjC::id k = Foundation::constString(proc, "hvm constant");
            const bool same = ObjC::id(r.send(cpu, r.getClass("NSString"), "stringWithString:", {k})) == k &&
                              ObjC::id(r.send(cpu, r.getClass("NSMutableString"), "stringWithString:", {k})) != k;
            check(layout && same, "String-stream __ZTV symbols have GCC's address points; stringWithString: of a constant string returns it");
        }

        // -[NSKeyedUnarchiver initForReadingWithData:nil] is an empty unarchiver (iOS 6), not an exception.
        {
            ObjC::id un = 0;
            bool threw = false;
            try {
                un = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, r.getClass("NSKeyedUnarchiver"), r.selAlloc)), "initForReadingWithData:", {0}));
            } catch (const std::exception&) { threw = true; }
            const bool empty = un && !ObjC::id(r.send(cpu, un, "decodeObjectForKey:", {Foundation::makeString(cx, "kusername")}));
            bool junkThrows = false;
            try {
                const uint8_t junk[4] = {1, 2, 3, 4};
                r.send(cpu, ObjC::id(r.send(cpu, r.getClass("NSKeyedUnarchiver"), r.selAlloc)), "initForReadingWithData:",
                       {Foundation::makeData(cx, junk, 4)});
            } catch (const std::exception&) { junkThrows = true; }
            check(!threw && empty && junkThrows, "NSKeyedUnarchiver with nil data decodes nothing; junk data still raises");
        }

        // UIRoundedRectButton (nib class for rounded-rect buttons) and a local GKAchievement.
        {
            const ObjC::Class rr = r.getClass("UIRoundedRectButton");
            const ObjC::id b = rr ? ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, rr, r.selAlloc)), "initWithFrame:", {0, 0, 0x42c80000, 0x42200000})) : 0;
            const bool button = b && (r.send(cpu, b, "isKindOfClass:", {r.getClass("UIButton")}) & 0xFF) && uint32_t(r.send(cpu, b, "buttonType")) == 1;
            const ObjC::id a = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, r.getClass("GKAchievement"), r.selAlloc)), "initWithIdentifier:",
                                               {Foundation::makeString(cx, "ach.one")}));
            const double fifty = 50.0, hundred = 100.0; uint64_t f, h; std::memcpy(&f, &fifty, 8); std::memcpy(&h, &hundred, 8);
            r.send(cpu, a, "setPercentComplete:", {uint32_t(f), uint32_t(f >> 32)});
            const uint64_t got = r.send(cpu, a, "percentComplete");
            double pc; std::memcpy(&pc, &got, 8);
            const bool half = pc == 50.0 && !(r.send(cpu, a, "isCompleted") & 0xFF);
            r.send(cpu, a, "setPercentComplete:", {uint32_t(h), uint32_t(h >> 32)});
            const bool done = (r.send(cpu, a, "isCompleted") & 0xFF) &&
                              Foundation::utf8(cx, ObjC::id(r.send(cpu, a, "identifier"))) == "ach.one";
            check(button && half && done, "UIRoundedRectButton is a rounded-rect UIButton; GKAchievement keeps identifier and percentComplete");
        }

        // CFURLCreateAbsoluteURLWithBytes resolves against the base URL.
        {
            auto call = [&](const char* sym, std::initializer_list<uint32_t> args) { return uint32_t(proc.callGuest(cpu, *proc.lookupSymbol(sym), args)); };
            const ObjC::id base = ObjC::id(r.send(cpu, r.getClass("NSURL"), "fileURLWithPath:isDirectory:", {Foundation::makeString(cx, "/sounds/sfx"), 1}));
            const uint32_t bytes = proc.allocCString("hit.caf");
            const ObjC::id url = ObjC::id(call("_CFURLCreateAbsoluteURLWithBytes", {0, bytes, 7, 0x08000100, base, 0}));
            const std::string abs = url ? Foundation::utf8(cx, ObjC::id(r.send(cpu, url, "absoluteString"))) : "";
            if (abs != "file:///sounds/sfx/hit.caf") std::printf("    got %s\n", abs.c_str());
            check(abs == "file:///sounds/sfx/hit.caf", "CFURLCreateAbsoluteURLWithBytes resolves the bytes against the base URL");
        }

        // A stringbuf built by the app's inline constructor, finished by _M_stringbuf_init.
        {
            auto call = [&](const char* sym, std::initializer_list<uint32_t> args) { return uint32_t(proc.callGuest(cpu, *proc.lookupSymbol(sym), args)); };
            const uint32_t sb = proc.heap().allocZeroed(40);
            const uint32_t alloc = proc.heap().allocZeroed(4);
            call("__ZNSsC1EPKcRKSaIcE", {sb + 36, proc.allocCString("abc"), alloc});
            call("__ZNSt15basic_stringbufIcSt11char_traitsIcESaIcEE17_M_stringbuf_initESt13_Ios_Openmode", {sb, 16u | 2u});
            const uint32_t vt = proc.mem().read<uint32_t>(sb);
            const uint32_t xsputn = proc.mem().read<uint32_t>(vt + 12 * 4);
            proc.callGuest(cpu, xsputn, {sb, proc.allocCString("def"), 3});
            const uint32_t strRet = proc.heap().allocZeroed(4);
            call("__ZNKSt15basic_stringbufIcSt11char_traitsIcESaIcEE3strEv", {strRet, sb});
            check(proc.readCString(proc.mem().read<uint32_t>(strRet)) == "abcdef",
                  "std::stringbuf from an inline constructor: _M_stringbuf_init keeps the initial text and appends (ate)");
        }

        // NSCalendar / NSDateComponents: compose, break down, add, difference.
        {
            const ObjC::id calendar = ObjC::id(r.send(cpu, r.getClass("NSCalendar"), "currentCalendar"));
            auto comps = [&](int y, int mo, int d, int h, int mi, int sec) {
                const ObjC::id c = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, r.getClass("NSDateComponents"), r.selAlloc)), "init"));
                r.send(cpu, c, "setYear:", {uint32_t(y)}); r.send(cpu, c, "setMonth:", {uint32_t(mo)}); r.send(cpu, c, "setDay:", {uint32_t(d)});
                r.send(cpu, c, "setHour:", {uint32_t(h)}); r.send(cpu, c, "setMinute:", {uint32_t(mi)}); r.send(cpu, c, "setSecond:", {uint32_t(sec)});
                return c;
            };
            const ObjC::id leapDay = ObjC::id(r.send(cpu, calendar, "dateFromComponents:", {comps(2024, 2, 29, 13, 45, 10)}));
            const uint32_t units = 4 | 8 | 16 | 32 | 64 | 128 | 512;
            const ObjC::id back = ObjC::id(r.send(cpu, calendar, "components:fromDate:", {units, leapDay}));
            auto get = [&](ObjC::id c, const char* sel) { return int32_t(r.send(cpu, c, sel)); };
            const bool roundTrip = get(back, "year") == 2024 && get(back, "month") == 2 && get(back, "day") == 29 &&
                                   get(back, "hour") == 13 && get(back, "minute") == 45 && get(back, "second") == 10 && get(back, "weekday") == 5;
            const ObjC::id oneYear = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, r.getClass("NSDateComponents"), r.selAlloc)), "init"));
            r.send(cpu, oneYear, "setYear:", {1});
            const ObjC::id next = ObjC::id(r.send(cpu, calendar, "dateByAddingComponents:toDate:options:", {oneYear, leapDay, 0}));
            const ObjC::id nextC = ObjC::id(r.send(cpu, calendar, "components:fromDate:", {units, next}));
            const ObjC::id jan1 = ObjC::id(r.send(cpu, calendar, "dateFromComponents:", {comps(2024, 1, 1, 0, 0, 0)}));
            const ObjC::id mar1 = ObjC::id(r.send(cpu, calendar, "dateFromComponents:", {comps(2024, 3, 1, 0, 0, 0)}));
            const ObjC::id diff = ObjC::id(r.send(cpu, calendar, "components:fromDate:toDate:options:", {16u, jan1, mar1, 0}));
            const ObjC::id diffYM = ObjC::id(r.send(cpu, calendar, "components:fromDate:toDate:options:", {4u | 8u | 16u, jan1, leapDay, 0}));
            check(roundTrip && get(nextC, "year") == 2025 && get(nextC, "month") == 2 && get(nextC, "day") == 28 &&
                  get(diff, "day") == 60 && get(diffYM, "year") == 0 && get(diffYM, "month") == 1 && get(diffYM, "day") == 28,
                  "NSCalendar compose / components / dateByAdding (leap day) / difference");
        }

        // NSScanner: numbers, strings, hex, up-to scans, skipping whitespace.
        {
            Ctx x{proc, cpu, proc.mem()};
            const ObjC::id str = Foundation::makeString(x, "  42 abc 3.5 0x1F rest of it");
            const ObjC::id sc = ObjC::id(r.send(cpu, r.getClass("NSScanner"), "scannerWithString:", {str}));
            const uint32_t out = proc.heap().allocZeroed(16);
            const bool a = uint32_t(r.send(cpu, sc, "scanInt:", {out})) & 0xFF;
            const int32_t i = int32_t(proc.mem().read<uint32_t>(out));
            const bool b = uint32_t(r.send(cpu, sc, "scanString:intoString:", {Foundation::makeString(x, "ABC"), 0})) & 0xFF;   // case-insensitive
            const bool c = uint32_t(r.send(cpu, sc, "scanDouble:", {out})) & 0xFF;
            const double d = proc.mem().read<double>(out);
            const bool e = uint32_t(r.send(cpu, sc, "scanHexInt:", {out})) & 0xFF;
            const uint32_t h = proc.mem().read<uint32_t>(out);
            const bool f = uint32_t(r.send(cpu, sc, "scanUpToString:intoString:", {Foundation::makeString(x, "zzz"), out})) & 0xFF;
            const std::string rest = Foundation::utf8(x, proc.mem().read<uint32_t>(out));
            const bool end = uint32_t(r.send(cpu, sc, "isAtEnd")) & 0xFF;
            check(a && i == 42 && b && c && d == 3.5 && e && h == 31 && f && rest == "rest of it" && end,
                  "NSScanner int / case-insensitive string / double / hex / scanUpToString");
        }

        // AudioQueue recording: microphone samples reach the input callback in the queue's format.
        {
            auto callAT = [&](const char* sym, std::initializer_list<uint32_t> args) { return uint32_t(proc.callGuest(cpu, *proc.lookupSymbol(sym), args)); };
            int calls = 0;
            uint32_t gotBytes = 0, gotPackets = 0;
            int16_t firstSample = 0;
            const auto cb = proc.registerHostFunction("hvm_test_aq_input", [&](Ctx& cx) {
                ++calls;
                const uint32_t buf = cx.cpu.r[2];
                gotBytes = cx.mem.read<uint32_t>(buf + 8);
                gotPackets = cx.mem.read<uint32_t>(cx.cpu.r[CPU::kSP]);
                firstSample = cx.mem.read<int16_t>(cx.mem.read<uint32_t>(buf + 4));
            });
            const uint32_t fmt = proc.heap().allocZeroed(40), out = proc.heap().allocZeroed(8);
            proc.mem().write<double>(fmt, 8000.0);
            proc.mem().write<uint32_t>(fmt + 8, 0x6C70636D);   // lpcm
            proc.mem().write<uint32_t>(fmt + 12, 0x0C);         // signed integer, packed
            proc.mem().write<uint32_t>(fmt + 16, 2);            // bytes per packet
            proc.mem().write<uint32_t>(fmt + 20, 1);
            proc.mem().write<uint32_t>(fmt + 24, 2);            // bytes per frame
            proc.mem().write<uint32_t>(fmt + 28, 1);            // mono
            proc.mem().write<uint32_t>(fmt + 32, 16);
            const uint32_t st1 = callAT("_AudioQueueNewInput", {fmt, cb, 0, 0, 0, 0, out});
            const uint32_t aq = proc.mem().read<uint32_t>(out);
            callAT("_AudioQueueAllocateBuffer", {aq, 1600, out + 4});   // 800 frames = 0.1 s
            const uint32_t buf = proc.mem().read<uint32_t>(out + 4);
            callAT("_AudioQueueEnqueueBuffer", {aq, buf, 0, 0});
            std::vector<float> tone(9600, 0.5f);                          // 0.2 s at 48 kHz
            proc.pushMicrophone(tone.data(), tone.size(), 48000);
            callAT("_AudioQueueStart", {aq, 0});
            auto& loop = Foundation::mainRunLoop(proc);
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (calls == 0 && std::chrono::steady_clock::now() < until) loop.runOnce(cpu, 0.02);
            callAT("_AudioQueueStop", {aq, 1});
            callAT("_AudioQueueDispose", {aq, 1});
            check(st1 == 0 && calls == 1 && gotBytes == 1600 && gotPackets == 800 && std::abs(firstSample - 16384) < 64,
                  "AudioQueue recording fills buffers with microphone audio at the queue's rate and format");
        }


        // A guest thread that returns with pools still pushed must release those
        // objects. NSThread's trampoline pops its own pool before returning, so
        // this uses a raw guest thread: one object is popped explicitly, and two
        // more sit in nested pools that only thread exit can drain.
        {
            std::atomic<int> deallocs{0};
            int explicitDeallocs = 0;
            ObjC::ClassBuilder cls = r.defineClass("HVMThreadExitPool", "NSObject");
            cls.method("dealloc", [&](Ctx& dx) {
                deallocs.fetch_add(1);
                r.disposeInstance(dx.cpu, dx.cpu.r[0]);
            }, "v@:");
            auto make = [&](CPU::Cpu& c) {
                return ObjC::id(r.send(c, ObjC::id(r.send(c, cls.cls(), r.selAlloc)), "init"));
            };
            const auto worker = proc.defineFunction("__hvm_test_thread_exit_pool", [&](Ctx& wx) {
                const uint32_t popped = r.poolPush();
                r.autorelease(wx.cpu, make(wx.cpu));
                r.poolPop(wx.cpu, popped);
                explicitDeallocs = deallocs.load();

                r.poolPush();
                r.autorelease(wx.cpu, make(wx.cpu));
                r.poolPush();
                r.autorelease(wx.cpu, make(wx.cpu));
            });
            const uint32_t thread = proc.startThread(worker, 0, 64u << 10, false);
            uint32_t returned = 0;
            const bool joined = proc.joinThread(thread, &returned);
            if (!(joined && explicitDeallocs == 1 && deallocs.load() == 3)) {
                std::printf("    thread-exit pool: joined=%d explicit=%d deallocs=%d\n",
                            joined, explicitDeallocs, deallocs.load());
            }
            check(joined && explicitDeallocs == 1 && deallocs.load() == 3,
                  "an object autoreleased on a guest thread is released when the thread exits");
        }

        // NSThread -isExecuting / -isFinished across start and the end of the thread's work (Jetpack, #75).
        {
            std::atomic<bool> release{false}, entered{false};
            ObjC::ClassBuilder tgt = r.defineClass("HVMTestThreadTarget", "NSObject");
            tgt.method("work:", [&](Ctx&) {
                entered = true;
                for (int i = 0; i < 400 && !release; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }, "v@:@");
            const ObjC::id target = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, tgt.cls(), r.selAlloc)), "init"));
            const ObjC::id th = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, r.getClass("NSThread"), r.selAlloc)),
                                                "initWithTarget:selector:object:", {target, r.sel("work:"), 0}));
            auto flag = [&](const char* sel) { return (uint32_t(r.send(cpu, th, sel)) & 0xFF) != 0; };
            const bool before = !flag("isExecuting") && !flag("isFinished");
            r.send(cpu, th, "start");
            for (int i = 0; i < 400 && !entered; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            const bool during = flag("isExecuting") && !flag("isFinished");
            release = true;
            for (int i = 0; i < 400 && !flag("isFinished"); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            const bool after = !flag("isExecuting") && flag("isFinished");
            check(before && during && after, "NSThread isExecuting/isFinished follow start and the end of its work");
        }

        // Verify the configured size on the live guest thread, and the post-start contract.
        {
            struct StackProbe { std::atomic<uint32_t> id{0}, threadObject{0}; std::atomic<bool> release{false}, done{false}; };
            auto probe = std::make_shared<StackProbe>();
            ObjC::ClassBuilder targetClass = r.defineClass("HVMTestStackTarget", "NSObject");
            targetClass.method("work:", [probe, &r](Ctx& cx) {
                probe->threadObject = ObjC::id(r.send(cx.cpu, r.getClass("NSThread"), "currentThread"));
                probe->id = cx.proc.currentThread().id;
                while (!probe->release && !cx.proc.stopping()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                probe->done = true;
            }, "v@:@");
            const ObjC::id target = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, targetClass.cls(), r.selAlloc)), "init"));
            const ObjC::id th = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, r.getClass("NSThread"), r.selAlloc)),
                                                "initWithTarget:selector:object:", {target, r.sel("work:"), 0}));
            const uint32_t defaultSize = uint32_t(r.send(cpu, th, "stackSize"));
            r.send(cpu, th, "setStackSize:", {8u << 10});   // below Apple's 16 KiB minimum
            const bool rejectedSmall = uint32_t(r.send(cpu, th, "stackSize")) == defaultSize;
            r.send(cpu, th, "setStackSize:", {16u << 10});   // the smallest size Apple documents
            check(uint32_t(r.send(cpu, th, "stackSize")) == (16u << 10), "NSThread accepts the documented 16 KiB minimum stack size");
            {
                auto p_attr = proc.allocStatic(64);
                proc.callGuest(cpu, *proc.lookupSymbol("_pthread_attr_init"), {p_attr});
                proc.callGuest(cpu, *proc.lookupSymbol("_pthread_attr_setstacksize"), {p_attr, 1024u * 1024u});
                auto p_th = proc.allocStatic(4);
                auto arg = proc.allocStatic(4);
                struct BoundsProbe { std::atomic<uint32_t> faultCount{0}; std::atomic<bool> done{false}; std::atomic<uint32_t> reportedSize{0}; };
                auto bprobe = std::make_shared<BoundsProbe>();
                uint32_t fnAddr = proc.defineFunction("Issue314Worker", [bprobe](Ctx& cx) {
                    uint32_t myTh = cx.proc.currentThread().self;
                    auto symSize = cx.proc.lookupSymbol("_pthread_get_stacksize_np");
                    auto symAddr = cx.proc.lookupSymbol("_pthread_get_stackaddr_np");
                    uint32_t size = symSize ? cx.proc.callGuest(cx.cpu, *symSize, {myTh}) : 0;
                    uint32_t addr = symAddr ? cx.proc.callGuest(cx.cpu, *symAddr, {myTh}) : 0;
                    bprobe->reportedSize = size;
                    if (size >= 1024u * 1024u && addr != 0) {

                        uint32_t codeBlock = cx.proc.allocStatic(4096, 4096);
                        cx.mem.protect(codeBlock, 4096, Memory::kProtRWX);
                        cx.mem.write<uint32_t>(codeBlock, 0xe5d00000); /* ldrb r0, [r0] */
                        cx.mem.write<uint32_t>(codeBlock + 4, 0xe12fff1e); /* bx lr */
                        cx.mem.write<uint32_t>(codeBlock + 8, 0xe5c01000); // strb r1, [r0]
                        cx.mem.write<uint32_t>(codeBlock + 12, 0xe12fff1e);
                        cx.proc.callGuest(cx.cpu, codeBlock + 8, {addr - size, 42});
                        cx.proc.callGuest(cx.cpu, codeBlock + 8, {addr - 1, 42});
                        try {
                            cx.proc.callGuest(cx.cpu, codeBlock, {addr - size - 1});
                        } catch (const CPU::GuestFault& f) {
                            if (f.exit == CPU::Exit::MemFault && f.info == addr - size - 1) bprobe->faultCount++;
                        }
                    }
                    bprobe->done = true;
                });
                proc.callGuest(cpu, *proc.lookupSymbol("_pthread_create"), {p_th, p_attr, fnAddr, arg});
                uint32_t createdTh = proc.mem().read<uint32_t>(p_th);
                proc.callGuest(cpu, *proc.lookupSymbol("_pthread_join"), {createdTh, 0});
                proc.callGuest(cpu, *proc.lookupSymbol("_pthread_attr_destroy"), {p_attr});
                bool got1mb = bprobe->reportedSize >= 1024u * 1024u;
                probe->id = 0; probe->done = false; probe->release = false;
                const ObjC::id stackThread = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, r.getClass("NSThread"), r.selAlloc)),
                    "initWithTarget:selector:object:", {target, r.sel("work:"), 0}));
                r.send(cpu, stackThread, "setStackSize:", {1024u * 1024u});
                r.send(cpu, stackThread, "start");
                for (int i = 0; i < 400 && !probe->id; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
                auto* runningStackThread = proc.findThreadById(probe->id);
                bool ns1mb = runningStackThread && runningStackThread->stackSize == 1024u * 1024u;
                probe->release = true;
                for (int i = 0; i < 400 && !probe->done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
                if (runningStackThread) proc.joinThread(runningStackThread->self, nullptr);
                probe->id = 0; probe->done = false; probe->release = false;
                check(got1mb && ns1mb && bprobe->faultCount == 1 && bprobe->done,
                      "pthread_attr_setstacksize and NSThread stackSize get at least 1MB and overflow hits guard page as guest fault");
            }
            {
                const auto setStack = proc.lookupSymbol("_pthread_attr_setstack");
                const auto setTop = proc.lookupSymbol("_pthread_attr_setstackaddr");
                check(setStack && setTop, "pthread custom stack APIs are registered");
                if (setStack && setTop) {
                    const uint32_t attr = proc.allocStatic(64), out = proc.allocStatic(4);
                    const uint32_t base = 0x0b000000u, size = 0x4000u;
                    proc.mem().map(base, size * 2, Memory::kProtRW);
                    auto observed = std::make_shared<std::atomic<uint32_t>>(0);
                    const auto worker = proc.defineFunction("CustomStackWorker", [observed](Ctx& cx) {
                        *observed = cx.proc.currentThread().stackBase;
                    });
                    auto invoke = [&](const char* name, std::initializer_list<uint32_t> args) {
                        return uint32_t(proc.callGuest(cpu, *proc.lookupSymbol(name), args));
                    };
                    invoke("_pthread_attr_init", {attr});
                    check(invoke("_pthread_attr_setdetachstate", {attr, 3}) == 22,
                          "pthread rejects unsupported detach state");
                    const uint32_t readOnly = 0x0b010000u;
                    proc.mem().map(readOnly, 4096, Memory::kProtRW);
                    proc.mem().write<uint32_t>(readOnly, 0xfeedu);
                    proc.mem().protect(readOnly, 4096, Memory::kProtRead);
                    check(invoke("_pthread_attr_getstacksize", {attr, readOnly}) == 14 &&
                          invoke("_pthread_attr_getdetachstate", {attr, readOnly}) == 14 &&
                          proc.mem().read<uint32_t>(readOnly) == 0xfeedu,
                          "pthread getters reject read-only output without changing bytes");
                    proc.mem().unmap(readOnly, 4096);

                    check(invoke("_pthread_attr_setstack", {attr, base + 1, size}) == 22 &&
                          invoke("_pthread_attr_setstackaddr", {attr, base + size + 1}) == 22,
                          "pthread rejects unaligned custom bases and tops");
                    check(invoke("_pthread_attr_setstack", {attr, base, size}) == 0, "caller-supplied stack attributes accepted");
                    auto launch = [&] {
                        const auto result = invoke("_pthread_create", {out, attr, worker, 0});
                        if (result == 0) invoke("_pthread_join", {proc.mem().read<uint32_t>(out), 0});
                        return result;
                    };
                    const uint32_t outBase = proc.allocStatic(4), outSize = proc.allocStatic(4);
                    check(invoke("_pthread_attr_getstackaddr", {attr, out}) == 0 &&
                          proc.mem().read<uint32_t>(out) == base + size &&
                          invoke("_pthread_attr_getstack", {attr, outBase, outSize}) == 0 &&
                          proc.mem().read<uint32_t>(outBase) == base && proc.mem().read<uint32_t>(outSize) == size,
                          "custom stack getters return supplied base top and size");
                    check(launch() == 0 && *observed == base && proc.mem().isMapped(base),
                          "caller stack is used and remains mapped after join");
                    check(invoke("_pthread_attr_setstackaddr", {attr, base + size * 2}) == 0 &&
                          launch() == 0 && *observed == base + size,
                          "setting stack top supersedes previous base");
                    check(invoke("_pthread_attr_getstackaddr", {attr, out}) == 0 &&
                          proc.mem().read<uint32_t>(out) == base + size * 2 &&
                          invoke("_pthread_attr_getstack", {attr, outBase, outSize}) == 0 &&
                          proc.mem().read<uint32_t>(outBase) == base + size && proc.mem().read<uint32_t>(outSize) == size,
                          "stack-top getters derive the current base and size");
                    invoke("_pthread_attr_setstack", {attr, base, size});
                    proc.mem().protect(base, 4096, Memory::kProtRead);
                    *observed = 0;
                    check(launch() == 22 && *observed == 0, "pthread rejects read-only custom stacks without starting");
                    proc.mem().protect(base, 4096, Memory::kProtRW);
                    proc.mem().write<uint32_t>(attr + 12, 0xfffff000u);
                    check(launch() == 22 && *observed == 0, "pthread create rejects an overflowing raw custom stack");
                    proc.mem().write<uint32_t>(attr + 12, base + 1);
                    check(launch() == 22 && *observed == 0, "pthread create rejects an unaligned raw custom stack");
                    invoke("_pthread_attr_setstack", {attr, base, size});
                    invoke("_pthread_attr_setstackaddr", {attr, 0x1000});
                    check(launch() == 22, "pthread rejects stack top below size");
                    check(invoke("_pthread_attr_setstack", {attr, base, 8192}) == 22,
                          "pthread rejects undersized caller stack");
                    check(invoke("_pthread_attr_setstack", {attr, base, size + 1}) == 22,
                          "pthread rejects caller size smaller than normalized allocation");
                    proc.mem().unmap(base, size * 2);
                }
            }
            const auto threadsBefore = proc.liveThreadIds();
            bool rejectedOverflow = false, rejectedRange = false;
            r.send(cpu, th, "setStackSize:", {0xffffffffu});
            try { r.send(cpu, th, "start"); } catch (const std::exception&) { rejectedOverflow = true; }
            r.send(cpu, th, "setStackSize:", {0x40000000u});
            try { r.send(cpu, th, "start"); } catch (const std::exception&) { rejectedRange = true; }
            const auto threadsAfter = proc.liveThreadIds();
            const bool noNewThreads = std::all_of(threadsAfter.begin(), threadsAfter.end(), [&](uint32_t id) {
                return std::find(threadsBefore.begin(), threadsBefore.end(), id) != threadsBefore.end();
            });
            const bool unchanged = noNewThreads && !(uint32_t(r.send(cpu, th, "isExecuting")) & 0xff);
            // Host-thread creation failing after the guest stack is mapped must leave nothing behind and not
            // poison later launches (the same NSThread can be started again afterwards).
            r.send(cpu, th, "setStackSize:", {1u << 20});
            Process::failNextHostThreadSpawns(1);
            bool spawnFailed = false;
            try { r.send(cpu, th, "start"); } catch (const std::exception&) { spawnFailed = true; }
            Process::failNextHostThreadSpawns(0);
            {
                const auto afterFailure = proc.liveThreadIds();
                // Every live thread must be one that was already live: threads that finish meanwhile may leave, none may appear.
                const bool noneLeaked = std::all_of(afterFailure.begin(), afterFailure.end(), [&](uint32_t id) {
                    return std::find(threadsBefore.begin(), threadsBefore.end(), id) != threadsBefore.end();
                });
                check(spawnFailed && noneLeaked && !(uint32_t(r.send(cpu, th, "isExecuting")) & 0xff),
                      "a host-thread creation failure leaves no guest thread registered and the NSThread startable again");
            }
            r.send(cpu, th, "setStackSize:", {1u << 20});
            const uint32_t setSize = uint32_t(r.send(cpu, th, "stackSize"));
            r.send(cpu, th, "start");
            for (int i = 0; i < 400 && !probe->id; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            const uint32_t threadId = probe->id;
            GuestThread* running = threadId && probe->threadObject == th ? proc.findThreadById(threadId) : nullptr;
            const bool actualSize = running && running->stackSize == (1u << 20);
            r.send(cpu, th, "setStackSize:", {2u << 20});
            const bool keptSize = uint32_t(r.send(cpu, th, "stackSize")) == (1u << 20) && running && running->stackSize == (1u << 20);
            probe->release = true;
            for (int i = 0; i < 400 && !probe->done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            check(defaultSize == (512u << 10) && setSize == (1u << 20) && actualSize && keptSize && probe->done,
                  "NSThread applies the requested guest stack and ignores updates after start");
            check(rejectedSmall, "NSThread ignores stack requests below 64 KiB");
            check(rejectedOverflow && rejectedRange && unchanged && actualSize,
                  "NSThread rejects oversized stacks without starting and can retry with a valid size");

            ObjC::ClassBuilder subclass = r.defineClass("HVMTestThreadSubclass", "NSThread");
            const ObjC::id child = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, subclass.cls(), r.selAlloc)), "init"));
            r.send(cpu, child, "setStackSize:", {1u << 20});
            check(uint32_t(r.send(cpu, child, "stackSize")) == (1u << 20),
                  "NSThread subclass alloc/init retains its requested stack size");
        }

        // AVAudioSession keeps its category, options and mode, and reports a route (Hungry Shark Evolution, #53 / #63).
        {
            Ctx sx{proc, cpu, proc.mem()};
            const ObjC::id session = ObjC::id(r.send(cpu, r.getClass("AVAudioSession"), "sharedInstance"));
            auto same = [&](ObjC::id value, const char* text) {
                return (uint32_t(r.send(cpu, value, "isEqualToString:", {Foundation::makeString(sx, text)})) & 0xFF) != 0;
            };
            const bool initial = same(ObjC::id(r.send(cpu, session, "category")), "AVAudioSessionCategorySoloAmbient") &&
                                 same(ObjC::id(r.send(cpu, session, "mode")), "AVAudioSessionModeDefault");
            const bool setOk = (uint32_t(r.send(cpu, session, "setCategory:withOptions:error:",
                                                {Foundation::makeString(sx, "AVAudioSessionCategoryPlayback"), 1u, 0u})) & 0xFF) != 0;
            const bool kept = same(ObjC::id(r.send(cpu, session, "category")), "AVAudioSessionCategoryPlayback") &&
                              uint32_t(r.send(cpu, session, "categoryOptions")) == 1u;
            r.send(cpu, session, "setMode:error:", {Foundation::makeString(sx, "AVAudioSessionModeVideoRecording"), 0u});
            const bool modeKept = same(ObjC::id(r.send(cpu, session, "mode")), "AVAudioSessionModeVideoRecording");
            r.send(cpu, session, "setCategory:mode:options:error:",
                   {Foundation::makeString(sx, "AVAudioSessionCategoryPlayAndRecord"), Foundation::makeString(sx, "AVAudioSessionModeGameChat"), 4u, 0u});
            const bool combined = same(ObjC::id(r.send(cpu, session, "category")), "AVAudioSessionCategoryPlayAndRecord") &&
                                  same(ObjC::id(r.send(cpu, session, "mode")), "AVAudioSessionModeGameChat") &&
                                  uint32_t(r.send(cpu, session, "categoryOptions")) == 4u;
            check(initial && setOk && kept && modeKept && combined,
                  "AVAudioSession keeps category, categoryOptions and mode across setCategory:/setMode:");
            r.send(cpu, session, "setCategory:withOptions:error:",
                   {Foundation::makeString(sx, "AVAudioSessionCategoryPlayAndRecord"), 0u, 0u});
            r.send(cpu, session, "setMode:error:", {Foundation::makeString(sx, "AVAudioSessionModeVideoChat"), 0u});
            const bool videoDefaults = uint32_t(r.send(cpu, session, "categoryOptions")) == 12u;
            r.send(cpu, session, "setCategory:withOptions:error:",
                   {Foundation::makeString(sx, "AVAudioSessionCategoryPlayAndRecord"), 1u, 0u});
            r.send(cpu, session, "setMode:error:", {Foundation::makeString(sx, "AVAudioSessionModeVideoChat"), 0u});
            check(videoDefaults && uint32_t(r.send(cpu, session, "categoryOptions")) == 13u,
                  "AVAudioSession video chat adds Bluetooth and speaker options while preserving existing options");
            r.send(cpu, session, "setCategory:mode:options:error:",
                   {Foundation::makeString(sx, "AVAudioSessionCategoryPlayAndRecord"), Foundation::makeString(sx, "AVAudioSessionModeVideoChat"), 0u, 0u});
            check(uint32_t(r.send(cpu, session, "categoryOptions")) == 12u,
                  "AVAudioSession combined video chat setter adds Bluetooth and speaker defaults");
            r.send(cpu, session, "setCategory:withOptions:error:",
                   {Foundation::makeString(sx, "AVAudioSessionCategoryPlayAndRecord"), 0u, 0u});
            r.send(cpu, session, "setMode:error:", {Foundation::makeString(sx, "AVAudioSessionModeVoiceChat"), 0u});
            const bool voiceDefaults = uint32_t(r.send(cpu, session, "categoryOptions")) == 4u;
            r.send(cpu, session, "setCategory:withOptions:error:",
                   {Foundation::makeString(sx, "AVAudioSessionCategoryPlayAndRecord"), 1u, 0u});
            r.send(cpu, session, "setMode:error:", {Foundation::makeString(sx, "AVAudioSessionModeVoiceChat"), 0u});
            check(voiceDefaults && uint32_t(r.send(cpu, session, "categoryOptions")) == 5u,
                  "AVAudioSession voice chat adds Bluetooth while preserving existing options");
            r.send(cpu, session, "setCategory:mode:options:error:",
                   {Foundation::makeString(sx, "AVAudioSessionCategoryPlayAndRecord"), Foundation::makeString(sx, "AVAudioSessionModeVoiceChat"), 0u, 0u});
            const bool combinedVoiceDefaults = uint32_t(r.send(cpu, session, "categoryOptions")) == 4u;
            r.send(cpu, session, "setCategory:mode:options:error:",
                   {Foundation::makeString(sx, "AVAudioSessionCategoryPlayAndRecord"), Foundation::makeString(sx, "AVAudioSessionModeVoiceChat"), 1u, 0u});
            check(combinedVoiceDefaults && uint32_t(r.send(cpu, session, "categoryOptions")) == 5u,
                  "AVAudioSession combined voice chat setter adds Bluetooth while preserving options");
            const auto playAndRecord = Foundation::makeString(sx, "AVAudioSessionCategoryPlayAndRecord");
            const auto playback = Foundation::makeString(sx, "AVAudioSessionCategoryPlayback");
            const auto record = Foundation::makeString(sx, "AVAudioSessionCategoryRecord");
            const auto voice = Foundation::makeString(sx, "AVAudioSessionModeVoiceChat");
            const auto video = Foundation::makeString(sx, "AVAudioSessionModeVideoChat");
            const auto defaultMode = Foundation::makeString(sx, "AVAudioSessionModeDefault");
            auto options = [&] { return uint32_t(r.send(cpu, session, "categoryOptions")); };
            r.send(cpu, session, "setCategory:mode:options:error:", {playAndRecord, voice, 1u, 0u});
            r.send(cpu, session, "setCategory:withOptions:error:", {playAndRecord, 0u, 0u});
            check(options() == 4u, "AVAudioSession keeps voice defaults when category options change");
            r.send(cpu, session, "setCategory:mode:options:error:", {playAndRecord, video, 0u, 0u});
            r.send(cpu, session, "setMode:error:", {defaultMode, 0u});
            const bool clearedVideoDefaults = options() == 0u;
            r.send(cpu, session, "setCategory:mode:options:error:", {playAndRecord, video, 5u, 0u});
            r.send(cpu, session, "setMode:error:", {defaultMode, 0u});
            check(clearedVideoDefaults && options() == 5u, "AVAudioSession clears video defaults but keeps explicitly requested bits");
            const bool acceptedVoice = (uint32_t(r.send(cpu, session, "setCategory:mode:options:error:", {playback, voice, 1u, 0u})) & 0xff) != 0;
            const bool playbackVoice = options() == 1u && same(ObjC::id(r.send(cpu, session, "mode")), "AVAudioSessionModeVoiceChat");
            const bool acceptedVideo = (uint32_t(r.send(cpu, session, "setMode:error:", {video, 0u})) & 0xff) != 0;
            const bool playbackVideo = options() == 1u && same(ObjC::id(r.send(cpu, session, "mode")), "AVAudioSessionModeVideoChat");
            check(acceptedVoice && acceptedVideo && playbackVoice && playbackVideo,
                  "AVAudioSession accepts chat modes on playback without chat defaults");
            r.send(cpu, session, "setCategory:error:", {record, 0u});
            const bool recordVideo = options() == 12u;
            r.send(cpu, session, "setMode:error:", {voice, 0u});
            const bool recordVoice = options() == 0u;
            r.send(cpu, session, "setCategory:error:", {playAndRecord, 0u});
            check(recordVideo && recordVoice && options() == 4u,
                  "AVAudioSession recomputes chat defaults when the category or mode changes");
            {
                const auto playAndRecord = Foundation::makeString(sx, "AVAudioSessionCategoryPlayAndRecord");
                const auto playback = Foundation::makeString(sx, "AVAudioSessionCategoryPlayback");
                const auto record = Foundation::makeString(sx, "AVAudioSessionCategoryRecord");
                const auto voice = Foundation::makeString(sx, "AVAudioSessionModeVoiceChat");
                const auto video = Foundation::makeString(sx, "AVAudioSessionModeVideoChat");
                const auto defaultMode = Foundation::makeString(sx, "AVAudioSessionModeDefault");
                r.send(cpu, session, "setCategory:mode:options:error:", {playAndRecord, voice, 1u, 0u});
                r.send(cpu, session, "setCategory:withOptions:error:", {playAndRecord, 0u, 0u});
                const bool voiceAfterOptions = uint32_t(r.send(cpu, session, "categoryOptions")) == 4u;
                r.send(cpu, session, "setCategory:error:", {playAndRecord, 0u});
                check(voiceAfterOptions && uint32_t(r.send(cpu, session, "categoryOptions")) == 4u,
                      "AVAudioSession retains voice chat defaults after category setters");
                r.send(cpu, session, "setMode:error:", {video, 0u});
                r.send(cpu, session, "setMode:error:", {defaultMode, 0u});
                const bool videoCleared = uint32_t(r.send(cpu, session, "categoryOptions")) == 0u;
                r.send(cpu, session, "setCategory:mode:options:error:", {playAndRecord, video, 4u, 0u});
                r.send(cpu, session, "setMode:error:", {defaultMode, 0u});
                check(videoCleared && uint32_t(r.send(cpu, session, "categoryOptions")) == 4u,
                      "AVAudioSession clears mode defaults while preserving explicit overlapping options");
                r.send(cpu, session, "setCategory:mode:options:error:", {playAndRecord, voice, 1u, 0u});
                r.send(cpu, session, "setCategory:withOptions:error:", {playback, 1u, 0u});
                const bool voicePlayback = uint32_t(r.send(cpu, session, "categoryOptions")) == 1u &&
                                           (uint32_t(r.send(cpu, session, "setMode:error:", {voice, 0u})) & 0xff) &&
                                           same(ObjC::id(r.send(cpu, session, "mode")), "AVAudioSessionModeVoiceChat") &&
                                           uint32_t(r.send(cpu, session, "categoryOptions")) == 1u;
                const bool videoAccepted = (uint32_t(r.send(cpu, session, "setMode:error:", {video, 0u})) & 0xff) != 0;
                const bool videoPlayback = videoAccepted && uint32_t(r.send(cpu, session, "categoryOptions")) == 1u &&
                                           same(ObjC::id(r.send(cpu, session, "mode")), "AVAudioSessionModeVideoChat");
                r.send(cpu, session, "setCategory:mode:options:error:", {playback, video, 1u, 0u});
                const bool combinedPlayback = uint32_t(r.send(cpu, session, "categoryOptions")) == 1u;
                r.send(cpu, session, "setCategory:withOptions:error:", {record, 1u, 0u});
                const bool videoRecord = uint32_t(r.send(cpu, session, "categoryOptions")) == 13u;
                r.send(cpu, session, "setMode:error:", {voice, 0u});
                check(voicePlayback && videoPlayback && combinedPlayback && videoRecord &&
                      uint32_t(r.send(cpu, session, "categoryOptions")) == 1u,
                      "AVAudioSession applies chat defaults only to supported categories");
            }
            // Mutable inputs make each setter copy and release the previous stored string.
            const auto mutableCategory = Foundation::makeString(sx, "AVAudioSessionCategoryPlayAndRecord", true, true);
            const auto mutablePlayback = Foundation::makeString(sx, "AVAudioSessionCategoryPlayback", true, true);
            const auto mutableVoice = Foundation::makeString(sx, "AVAudioSessionModeVoiceChat", true, true);
            const auto mutableVideo = Foundation::makeString(sx, "AVAudioSessionModeVideoChat", true, true);
            r.send(cpu, session, "setCategory:mode:options:error:", {mutableCategory, mutableVoice, 0u, 0u});
            const auto savedCategory = ObjC::id(r.send(cpu, session, "category"));
            const auto savedMode = ObjC::id(r.send(cpu, session, "mode"));
            r.send(cpu, session, "setCategory:mode:options:error:", {mutablePlayback, mutableVideo, 0u, 0u});
            check(Foundation::utf8(sx, savedCategory) == "AVAudioSessionCategoryPlayAndRecord" &&
                  Foundation::utf8(sx, savedMode) == "AVAudioSessionModeVoiceChat",
                  "AVAudioSession getters keep their results alive after replacement");
            {
                // The setter must copy: mutating the caller's string afterwards must not change the stored category/mode.
                const auto probeCategory = Foundation::makeString(sx, "AVAudioSessionCategoryPlayback", true, true);
                const auto probeMode = Foundation::makeString(sx, "AVAudioSessionModeDefault", true, true);
                r.send(cpu, session, "setCategory:mode:options:error:", {probeCategory, probeMode, 0u, 0u});
                r.send(cpu, probeCategory, "appendString:", {Foundation::makeString(sx, "-mutated")});
                r.send(cpu, probeMode, "appendString:", {Foundation::makeString(sx, "-mutated")});
                check(Foundation::utf8(sx, ObjC::id(r.send(cpu, session, "category"))) == "AVAudioSessionCategoryPlayback" &&
                          Foundation::utf8(sx, ObjC::id(r.send(cpu, session, "mode"))) == "AVAudioSessionModeDefault",
                      "AVAudioSession setters copy mutable category and mode strings");
                r.release(cpu, probeCategory); r.release(cpu, probeMode);
            }
            // Apple adds MixWithOthers implicitly for Ambient and DuckOthers.
            r.send(cpu, session, "setCategory:withOptions:error:", {Foundation::makeString(sx, "AVAudioSessionCategoryPlayback"), 2u, 0u});
            const bool duckMixes = uint32_t(r.send(cpu, session, "categoryOptions")) == 3u;
            r.send(cpu, session, "setCategory:withOptions:error:", {Foundation::makeString(sx, "AVAudioSessionCategoryAmbient"), 0u, 0u});
            check(duckMixes && uint32_t(r.send(cpu, session, "categoryOptions")) == 1u,
                  "AVAudioSession reports the implicit MixWithOthers for DuckOthers and for Ambient");
            // Record is stored, but playback is not silenced: that is reported as missing rather than hidden.
            const uint64_t missingBefore = proc.missingApis().totalHits();
            r.send(cpu, session, "setCategory:error:", {Foundation::makeString(sx, "AVAudioSessionCategoryRecord"), 0u});
            check(proc.missingApis().totalHits() == missingBefore + 1, "AVAudioSession Record category is reported as missing behaviour");
            r.send(cpu, session, "setCategory:error:", {Foundation::makeString(sx, "AVAudioSessionCategoryPlayback"), 0u});

            proc.mem().map(0xbb000000, 0x40000, Memory::kProtRW);
            std::atomic<unsigned> ready{0}, done{0};
            std::atomic<bool> start{false}, optionsOk{true};
            auto worker = [&](unsigned which) {
                CPU::Cpu workerCpu;
                workerCpu.r[CPU::kSP] = 0xbb000000 + (which + 1) * 0x10000 - 256;
                ++ready;
                while (!start) std::this_thread::yield();
                if (which == 3) {
                    unsigned reads = 0;
                    do {
                        const uint32_t value = uint32_t(r.send(workerCpu, session, "categoryOptions"));
                        if (value != 0 && value != 1 && value != 4 && value != 5 && value != 12 && value != 13)
                            optionsOk = false;
                        ++reads;
                    } while (done != 3 || reads < 4000);
                } else {
                    for (unsigned i = 0; i < 2000; ++i) {
                        if (which == 0) {
                            r.send(workerCpu, session, "setCategory:error:", {mutableCategory, 0u});
                            r.send(workerCpu, session, "setCategory:withOptions:error:", {mutablePlayback, 1u, 0u});
                        } else if (which == 1) {
                            r.send(workerCpu, session, "setMode:error:", {mutableVoice, 0u});
                            r.send(workerCpu, session, "setMode:error:", {mutableVideo, 0u});
                        } else {
                            r.send(workerCpu, session, "setCategory:mode:options:error:", {mutableCategory, mutableVideo, 1u, 0u});
                            r.send(workerCpu, session, "setCategory:mode:options:error:", {mutablePlayback, mutableVoice, 0u, 0u});
                        }
                    }
                    ++done;
                }
            };
            std::thread a(worker, 0), b(worker, 1), c(worker, 2), reader(worker, 3);
            while (ready != 4) std::this_thread::yield();
            start = true;
            a.join(); b.join(); c.join(); reader.join();
            check(optionsOk, "AVAudioSession synchronizes options reads with concurrent category and mode updates");
            r.release(cpu, mutableCategory); r.release(cpu, mutablePlayback);
            r.release(cpu, mutableVoice); r.release(cpu, mutableVideo);

            {   // The first currentRoute read builds the route: concurrent first reads must all see the same object. The
                // cache is per session, so each round uses a fresh process (a cold cache) to make the race likely.
                bool allSame = true;
                for (int round = 0; round < 40 && allSame; ++round) {
                    ProcessConfig routeCfg;
                    routeCfg.homePath = "/tmp/hvm_route_race";
                    Process routeProc(routeCfg);
                    auto& rr = ObjC::rt(routeProc);
                    const ObjC::id routeSession = ObjC::id(rr.send(routeProc.mainThread().cpu, rr.getClass("AVAudioSession"), "sharedInstance"));
                    routeProc.mem().map(0xbb000000, 0x40000, Memory::kProtRW);
                    std::atomic<unsigned> routeReady{0};
                    std::atomic<bool> routeStart{false};
                    std::vector<uint32_t> routes(4, 0);
                    auto routeWorker = [&](unsigned which) {
                        CPU::Cpu workerCpu;
                        workerCpu.r[CPU::kSP] = 0xbb000000 + (which + 1) * 0x10000 - 256;
                        ++routeReady;
                        while (!routeStart) std::this_thread::yield();
                        routes[which] = uint32_t(rr.send(workerCpu, routeSession, "currentRoute"));
                    };
                    std::thread t0(routeWorker, 0), t1(routeWorker, 1), t2(routeWorker, 2), t3(routeWorker, 3);
                    while (routeReady != 4) std::this_thread::yield();
                    routeStart = true;
                    t0.join(); t1.join(); t2.join(); t3.join();
                    allSame = routes[0] != 0 && routes[0] == routes[1] && routes[1] == routes[2] && routes[2] == routes[3];
                }
                check(allSame, "concurrent first reads of AVAudioSession currentRoute all get one route");
            }
            const ObjC::id route = ObjC::id(r.send(cpu, session, "currentRoute"));
            const ObjC::id outputs = route ? ObjC::id(r.send(cpu, route, "outputs")) : 0;
            const ObjC::id port = outputs && uint32_t(r.send(cpu, outputs, "count")) == 1 ? ObjC::id(r.send(cpu, outputs, "objectAtIndex:", {0u})) : 0;
            check(port && same(ObjC::id(r.send(cpu, port, "portType")), "Speaker"),
                  "AVAudioSession currentRoute reports the built-in speaker as its one output");
            check(proc.lookupSymbol("_AVAudioSessionModeGameChat") && proc.lookupSymbol("_AVAudioSessionModeVideoRecording") &&
                  proc.lookupSymbol("_AVAudioSessionModeMeasurement") && proc.lookupSymbol("_AVAudioSessionCategoryMultiRoute"),
                  "AVAudioSession exports the remaining mode and category string constants");
        }

        // NSXMLParser: elements, attributes, entities, CDATA, self-closing tags, errors.
        {
            Ctx x{proc, cpu, proc.mem()};
            std::vector<std::string> events;
            ObjC::ClassBuilder del = r.defineClass("HVMTestXMLDelegate", "NSObject");
            del.method("parser:didStartElement:namespaceURI:qualifiedName:attributes:", [&](Ctx& cx) {
                // (self, _cmd, parser, elementName, namespaceURI, qName, attributes): the last three on the stack
                const uint32_t attrs = cx.mem.read<uint32_t>(cx.cpu.r[CPU::kSP] + 8);
                std::string e = "<" + Foundation::utf8(cx, cx.cpu.r[3]);
                for (auto& [k, v] : Foundation::dictEntries(cx, attrs)) e += " " + Foundation::utf8(cx, k) + "=" + Foundation::utf8(cx, v);
                events.push_back(e + ">");
            }, "v@:@@@@@");
            del.method("parser:didEndElement:namespaceURI:qualifiedName:", [&](Ctx& cx) { events.push_back("</" + Foundation::utf8(cx, cx.cpu.r[3]) + ">"); }, "v@:@@@@");
            del.method("parser:foundCharacters:", [&](Ctx& cx) {
                const std::string t = Foundation::utf8(cx, cx.cpu.r[3]);
                if (t.find_first_not_of(" \n\t") != std::string::npos) events.push_back("'" + t + "'");
            }, "v@:@@");
            del.method("parser:parseErrorOccurred:", [&](Ctx&) { events.push_back("ERROR"); }, "v@:@@");
            const ObjC::id delegate = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, del.cls(), r.selAlloc)), "init"));
            auto parse = [&](const std::string& xml) {
                events.clear();
                const ObjC::id data = Foundation::makeData(x, xml.data(), uint32_t(xml.size()));
                const ObjC::id parser = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, r.getClass("NSXMLParser"), r.selAlloc)), "initWithData:", {data}));
                r.send(cpu, parser, "setDelegate:", {delegate});
                const bool ok = uint32_t(r.send(cpu, parser, "parse")) & 0xFF;
                std::string joined;
                for (auto& e : events) joined += e;
                return std::make_pair(ok, joined);
            };
            const auto good = parse("<?xml version=\"1.0\"?><!-- c --><cfg v=\"2\"><item name='a &amp; b'>x &lt; y</item><empty/><![CDATA[<raw>]]></cfg>");
            const auto bad = parse("<a><b></a>");
            if (good.second != "<cfg v=2><item name=a & b>'x < y'</item><empty></empty>'<raw>'</cfg>") std::printf("    xml events: %s / %s\n", good.second.c_str(), bad.second.c_str());
            check(good.first && good.second == "<cfg v=2><item name=a & b>'x < y'</item><empty></empty>'<raw>'</cfg>" && !bad.first && bad.second.find("ERROR") != std::string::npos,
                  "NSXMLParser elements / attributes / entities / CDATA / self-closing / mismatched-tag error");
        }
        {
            // GCController controllers returns empty array and completion blocks work
            const ObjC::Class gccCls = r.getClass("GCController");
            if (gccCls) {
                const ObjC::id arr = ObjC::id(r.send(cpu, gccCls, "controllers"));
                const uint32_t count = uint32_t(r.send(cpu, arr, "count"));

                uint32_t called = 0;
                const uint32_t invoke = proc.defineFunction("test_gc_discovery_completion", [&](Ctx&) { called = 1; });
                const uint32_t desc = proc.heap().allocZeroed(8);
                proc.mem().write<uint32_t>(desc + 4, 20); // 32-bit Block_descriptor size
                const uint32_t bAddr = proc.heap().allocZeroed(20);
                proc.mem().write<uint32_t>(bAddr + 12, invoke);
                proc.mem().write<uint32_t>(bAddr + 16, desc);
                r.send(cpu, gccCls, "startWirelessControllerDiscoveryWithCompletionHandler:", {bAddr});
                proc.heap().free(bAddr); // callback must hold a copied block
                for (int i = 0; i < 20 && !called; ++i) Foundation::mainRunLoop(proc).runOnce(cpu, 0.05);
                proc.heap().free(desc);

                check(count == 0 && called == 1, "GCController controllers returns empty array and triggers discovery block");
            } else {
                check(false, "GCController class not found");
            }
        }
        {
            // SKPaymentTransaction properties and types
            const ObjC::Class sktCls = r.getClass("SKPaymentTransaction");
            const ObjC::Class skpCls = r.getClass("SKPayment");
            if (sktCls && skpCls) {
                const ObjC::id tx = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, sktCls, r.selAlloc)), "init"));
                const ObjC::id tid = ObjC::id(r.send(cpu, tx, "transactionIdentifier"));
                const ObjC::id tdate = ObjC::id(r.send(cpu, tx, "transactionDate"));
                const uint32_t tstate = uint32_t(r.send(cpu, tx, "transactionState"));

                const uint32_t stateSel = r.sel("transactionState");
                const std::string stateSig = r.methodTypes(sktCls, stateSel);

                const ObjC::id prodId = Foundation::makeString(cx, "com.test.product");
                const ObjC::id pay = ObjC::id(r.send(cpu, skpCls, "paymentWithProductIdentifier:", {prodId}));
                const uint32_t qty1 = uint32_t(r.send(cpu, pay, "quantity"));
                const ObjC::id pId = ObjC::id(r.send(cpu, pay, "productIdentifier"));

                const ObjC::Class mPayCls = r.getClass("SKMutablePayment");
                const ObjC::id mPay = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, mPayCls, r.selAlloc)), "init"));
                r.send(cpu, mPay, "setQuantity:", {uint32_t(3)});
                const uint32_t qty2 = uint32_t(r.send(cpu, mPay, "quantity"));
                bool rejectedInvalid = false;
                try { r.send(cpu, mPay, "setQuantity:", {uint32_t(0)}); }
                catch (const ObjC::ObjCError&) { rejectedInvalid = true; }
                catch (const CPU::GuestFault&) { rejectedInvalid = true; }
                const uint32_t qtyAfterInvalid = uint32_t(r.send(cpu, mPay, "quantity"));
                const ObjC::id nilPayment = ObjC::id(r.send(cpu, skpCls, "paymentWithProductIdentifier:", {0}));

                const uint32_t qtySel = r.sel("setQuantity:");
                const std::string qtySig = r.methodTypes(mPayCls, qtySel);

                check(tid == 0 && tdate == 0 && tstate == 0 && stateSig == "i@:" && qtySig == "v@:i" && qty1 == 1 && pId == prodId && qty2 == 3 && rejectedInvalid && qtyAfterInvalid == 3 && !nilPayment,
                      "SKPayment has armv7 NSInteger signatures, retains product ID, defaults quantity to one and rejects invalid quantities");
            } else {
                check(false, "SKPaymentTransaction or SKPayment class not found");
            }
        }
    }

    // ICU regex (libicucore uregex_*), as RegexKitLite calls it: UTF-16
    // pattern and text, UTF-16 offsets, ICU error codes.
    {
        auto has = [&](const char* s) { return bool(proc.lookupSymbol(s)); };
        auto call = [&](const char* sym, std::initializer_list<uint32_t> args) {
            return uint32_t(proc.callGuest(cpu, *proc.lookupSymbol(sym), args));
        };
        auto u16 = [&](const std::u16string& s) {
            const uint32_t a = proc.heap().allocZeroed(uint32_t(s.size() * 2 + 2));
            for (size_t i = 0; i < s.size(); ++i) proc.mem().write<uint16_t>(a + uint32_t(i) * 2, uint16_t(s[i]));
            return a;
        };
        const bool have = has("_uregex_open") && has("_uregex_setText") && has("_uregex_find") && has("_uregex_findNext") &&
                          has("_uregex_start") && has("_uregex_end") && has("_uregex_groupCount") && has("_uregex_close") &&
                          has("_u_strlen") && has("_u_errorName") && has("_uregex_group");
        bool ok = have;
        if (have) {
            const uint32_t st = proc.heap().allocZeroed(4);
            const uint32_t pe = proc.heap().allocZeroed(72);
            auto status = [&] { return int32_t(proc.mem().read<uint32_t>(st)); };
            // "é" is one unit and the emoji two, so offsets past it are UTF-16 offsets.
            const uint32_t pat = u16(u"(\\w+)@(\\d+)");
            const uint32_t txt = u16(u"é x \U0001F600 ab@12 cd@345 z");
            const uint32_t re = call("_uregex_open", {pat, 0xffffffffu, 0, pe, st});
            ok = ok && re && status() == 0 && int32_t(call("_u_strlen", {txt})) == 21;
            ok = ok && call("_uregex_groupCount", {re, st}) == 2;
            call("_uregex_setText", {re, txt, 0xffffffffu, st});
            ok = ok && call("_uregex_find", {re, 0, st}) == 1 && status() == 0;
            ok = ok && call("_uregex_start", {re, 0, st}) == 7 && call("_uregex_end", {re, 0, st}) == 12;
            ok = ok && call("_uregex_start", {re, 2, st}) == 10 && call("_uregex_end", {re, 1, st}) == 9;
            const uint32_t buf = proc.heap().allocZeroed(32);
            ok = ok && call("_uregex_group", {re, 2, buf, 8, st}) == 2 && proc.mem().read<uint16_t>(buf) == u'1' &&
                 proc.mem().read<uint16_t>(buf + 4) == 0;
            ok = ok && call("_uregex_findNext", {re, st}) == 1 && call("_uregex_start", {re, 0, st}) == 13 &&
                 call("_uregex_end", {re, 0, st}) == 19;
            ok = ok && call("_uregex_findNext", {re, st}) == 0 && status() == 0;
            // Asking for a group with no current match is U_REGEX_INVALID_STATE.
            call("_uregex_start", {re, 0, st});
            ok = ok && status() == 0x10302;
            proc.mem().write<uint32_t>(st, 0);
            ok = ok && call("_uregex_find", {re, 13, st}) == 1 && call("_uregex_start", {re, 0, st}) == 13;
            call("_uregex_start", {re, 3, st});
            ok = ok && status() == 8;   // U_INDEX_OUTOFBOUNDS_ERROR
            proc.mem().write<uint32_t>(st, 0);
            call("_uregex_close", {re});
            // Case-insensitive flag and inline (?i).
            const uint32_t re2 = call("_uregex_open", {u16(u"(?i)AB"), 0xffffffffu, 0, pe, st});
            call("_uregex_setText", {re2, u16(u"xxab"), 4, st});
            ok = ok && re2 && call("_uregex_find", {re2, 0, st}) == 1 && call("_uregex_start", {re2, 0, st}) == 2;
            call("_uregex_close", {re2});
            // Syntax the engine cannot honour is an error, never success.
            const uint32_t bad = call("_uregex_open", {u16(u"(?<=a)b"), 0xffffffffu, 0, pe, st});
            ok = ok && bad == 0 && status() == 0x10301 &&
                 proc.readCString(call("_u_errorName", {0x10301})) == "U_REGEX_RULE_SYNTAX";
            // A failure already in *status makes the call a no-op.
            ok = ok && call("_uregex_open", {pat, 0xffffffffu, 0, pe, st}) == 0 && status() == 0x10301;
        }
        check(ok, "ICU uregex_open/setText/find/findNext/start/end/group report UTF-16 offsets and ICU error codes");
    }

    // POSIX regex (Darwin 32-bit ABI) and tmpnam inside the guest sandbox.
    // regoff_t is __darwin_off_t (int64) even on armv7, so regmatch_t is 16 bytes.
    {
        auto call = [&](const char* sym, std::initializer_list<uint32_t> args) {
            return uint32_t(proc.callGuest(cpu, *proc.lookupSymbol(sym), args));
        };
        const bool have = proc.lookupSymbol("_regcomp") && proc.lookupSymbol("_regexec") &&
                          proc.lookupSymbol("_regerror") && proc.lookupSymbol("_regfree") &&
                          proc.lookupSymbol("_tmpnam");
        constexpr uint32_t REG_EXTENDED = 1, REG_ICASE = 2, REG_NOSUB = 4, REG_NEWLINE = 010;
        constexpr uint32_t REG_NOTBOL = 1;
        constexpr uint32_t REG_NOMATCH = 1, REG_BADPAT = 2, REG_ECTYPE = 4, REG_EESCAPE = 5, REG_ESUBREG = 6;
        constexpr uint32_t REG_EBRACK = 7, REG_EPAREN = 8, REG_EBRACE = 9, REG_BADBR = 10, REG_ERANGE = 11;
        constexpr uint32_t REG_BADRPT = 13, REG_INVARG = 16;
        auto i64 = [&](uint32_t a) { return proc.mem().read<int64_t>(a); };
        auto nsub = [&](uint32_t preg) { return proc.mem().read<uint32_t>(preg + 4); };
        auto compile = [&](const char* pat, uint32_t flags) {
            const uint32_t preg = proc.heap().allocZeroed(32);
            const uint32_t rc = call("_regcomp", {preg, proc.allocCString(pat), flags});
            return std::pair{rc, preg};
        };
        auto exec = [&](uint32_t preg, const char* text, uint32_t nmatch, uint32_t pmatch, uint32_t eflags = 0) {
            return call("_regexec", {preg, proc.allocCString(text), nmatch, pmatch, eflags});
        };
        auto offs = [&](uint32_t pm, int i) { return std::pair{i64(pm + uint32_t(i) * 16), i64(pm + uint32_t(i) * 16 + 8)}; };
        const uint32_t pm = proc.heap().allocZeroed(8 * 16);
        auto clearPm = [&] {
            for (uint32_t i = 0; i < 8 * 16; i += 8) proc.mem().write<int64_t>(pm + i, -1);
        };
        auto run = [&](const char* name, auto&& fn) {
            if (!have) { check(false, name); return; }
            bool ok = false;
            try { ok = fn(); }
            catch (const std::exception& e) { std::printf("    %s threw: %s\n", name, e.what()); }
            check(ok, name);
        };
        run("POSIX BRE dot-star, anchors, and re_nsub", [&] {
            auto [rc, re] = compile("a.*b", 0);
            clearPm();
            const uint32_t er = exec(re, "zzaxxbqq", 1, pm);
            const auto dot = offs(pm, 0);
            const uint32_t groups = nsub(re);
            auto [rc2, re2] = compile("^hello$", 0);
            clearPm();
            const uint32_t hit = exec(re2, "hello", 1, pm);
            const auto whole = offs(pm, 0);
            const uint32_t miss = exec(re2, "hello!", 1, pm);
            call("_regfree", {re}); call("_regfree", {re2});
            return rc == 0 && er == 0 && groups == 0 && dot == std::pair<int64_t,int64_t>(2, 6) &&
                   rc2 == 0 && hit == 0 && whole == std::pair<int64_t,int64_t>(0, 5) && miss == REG_NOMATCH;
        });
        run("POSIX BRE grouping reports the last iteration", [&] {
            auto [rc, re] = compile("\\(ab\\)*", 0);
            clearPm();
            const uint32_t er = exec(re, "ababab", 2, pm);
            const bool ok = rc == 0 && er == 0 && nsub(re) == 1 && offs(pm, 0) == std::pair<int64_t,int64_t>(0, 6) &&
                            offs(pm, 1) == std::pair<int64_t,int64_t>(4, 6);
            call("_regfree", {re});
            return ok;
        });
        run("POSIX BRE bounded interval is greedy", [&] {
            auto [rc, re] = compile("a\\{2,4\\}", 0);
            clearPm();
            const uint32_t er = exec(re, "aaaaa", 1, pm);
            const bool ok = rc == 0 && er == 0 && offs(pm, 0) == std::pair<int64_t,int64_t>(0, 4);
            call("_regfree", {re});
            return ok;
        });
        run("POSIX BRE backreferences", [&] {
            auto [rc, re] = compile("\\(a\\)\\1", 0);
            clearPm();
            const uint32_t hit = exec(re, "aa", 2, pm);
            const auto whole = offs(pm, 0);
            const auto g = offs(pm, 1);
            const uint32_t miss = exec(re, "ab", 2, pm);
            const bool ok = rc == 0 && hit == 0 && whole == std::pair<int64_t,int64_t>(0, 2) &&
                            g == std::pair<int64_t,int64_t>(0, 1) && miss == REG_NOMATCH;
            call("_regfree", {re});
            return ok;
        });
        run("POSIX ERE alternation is leftmost-longest", [&] {
            auto [rc, re] = compile("(foo|foobar)|(a|ab)c", REG_EXTENDED);
            clearPm();
            const uint32_t er = exec(re, "foobar", 3, pm);
            const bool one = rc == 0 && er == 0 && offs(pm, 0) == std::pair<int64_t,int64_t>(0, 6) &&
                             offs(pm, 1) == std::pair<int64_t,int64_t>(0, 6) && offs(pm, 2) == std::pair<int64_t,int64_t>(-1, -1);
            auto [rc2, re2] = compile("(a|ab)c", REG_EXTENDED);
            clearPm();
            const uint32_t er2 = exec(re2, "abc", 2, pm);
            const bool two = rc2 == 0 && er2 == 0 && offs(pm, 0) == std::pair<int64_t,int64_t>(0, 3) &&
                             offs(pm, 1) == std::pair<int64_t,int64_t>(0, 2);
            call("_regfree", {re}); call("_regfree", {re2});
            return one && two;
        });
        run("POSIX ERE subexpression offsets", [&] {
            auto [rc, re] = compile("([a-z]+)-([0-9]+)", REG_EXTENDED);
            clearPm();
            const uint32_t er = exec(re, "id-42", 3, pm);
            auto [rc2, re2] = compile("colou?r", REG_EXTENDED);
            const uint32_t c1 = exec(re2, "color", 1, pm);
            const uint32_t c2 = exec(re2, "colour", 1, pm);
            auto [rc3, re3] = compile("[[:digit:]]+", REG_EXTENDED);
            clearPm();
            const uint32_t er3 = exec(re3, "id-42", 1, pm);
            const bool ok = rc == 0 && er == 0 && nsub(re) == 2 &&
                            offs(pm, 0) == std::pair<int64_t,int64_t>(3, 5) &&
                            rc2 == 0 && c1 == 0 && c2 == 0 && rc3 == 0 && er3 == 0;
            // The digit check overwrote pmatch; re-check the capture pattern.
            clearPm();
            exec(re, "id-42", 3, pm);
            const bool caps = offs(pm, 0) == std::pair<int64_t,int64_t>(0, 5) &&
                              offs(pm, 1) == std::pair<int64_t,int64_t>(0, 2) &&
                              offs(pm, 2) == std::pair<int64_t,int64_t>(3, 5);
            call("_regfree", {re}); call("_regfree", {re2}); call("_regfree", {re3});
            return ok && caps;
        });
        run("unmatched optional and alternate groups are -1", [&] {
            auto [rc, re] = compile("a(b)?c", REG_EXTENDED);
            clearPm();
            const uint32_t er = exec(re, "ac", 2, pm);
            const bool opt = rc == 0 && er == 0 && offs(pm, 0) == std::pair<int64_t,int64_t>(0, 2) &&
                             offs(pm, 1) == std::pair<int64_t,int64_t>(-1, -1);
            auto [rc2, re2] = compile("(a)|(b)", REG_EXTENDED);
            clearPm();
            const uint32_t er2 = exec(re2, "b", 3, pm);
            const bool alt = rc2 == 0 && er2 == 0 && nsub(re2) == 2 &&
                             offs(pm, 1) == std::pair<int64_t,int64_t>(-1, -1) &&
                             offs(pm, 2) == std::pair<int64_t,int64_t>(0, 1);
            call("_regfree", {re}); call("_regfree", {re2});
            return opt && alt;
        });
        run("REG_ICASE folds literals and ranges", [&] {
            auto [rc, re] = compile("^abc$", REG_EXTENDED | REG_ICASE);
            clearPm();
            const uint32_t hit = exec(re, "AbC", 1, pm);
            const auto folded = offs(pm, 0);
            auto [rc2, re2] = compile("^abc$", REG_EXTENDED);
            const uint32_t miss = exec(re2, "AbC", 1, pm);
            auto [rc3, re3] = compile("[A-C]", REG_EXTENDED | REG_ICASE);
            clearPm();
            const uint32_t er3 = exec(re3, "b", 1, pm);
            const auto range = offs(pm, 0);
            const bool ok = rc == 0 && hit == 0 && folded == std::pair<int64_t,int64_t>(0, 3) &&
                            rc2 == 0 && miss == REG_NOMATCH && rc3 == 0 && er3 == 0 &&
                            range == std::pair<int64_t,int64_t>(0, 1);
            // offs was overwritten by the class match; the icase literal was checked before that.
            call("_regfree", {re}); call("_regfree", {re2}); call("_regfree", {re3});
            return ok;
        });
        run("REG_NOSUB keeps re_nsub and does not write pmatch", [&] {
            auto [rc, re] = compile("(a)(b)", REG_EXTENDED | REG_NOSUB);
            for (uint32_t i = 0; i < 4 * 16; i += 8) proc.mem().write<int64_t>(pm + i, 99);
            const uint32_t er = exec(re, "ab", 4, pm);
            bool untouched = true;
            for (uint32_t i = 0; i < 4 * 16; i += 8) untouched = untouched && i64(pm + i) == 99;
            const bool ok = rc == 0 && er == 0 && nsub(re) == 2 && untouched;
            call("_regfree", {re});
            return ok;
        });
        run("REG_NEWLINE changes dot, anchors, and negated classes", [&] {
            auto [rc, re] = compile("a.b", REG_EXTENDED | REG_NEWLINE);
            const uint32_t dot = exec(re, "a\nb", 1, pm);
            auto [rc2, re2] = compile("^b", REG_EXTENDED | REG_NEWLINE);
            clearPm();
            const uint32_t bol = exec(re2, "a\nb", 1, pm);
            const auto bm = offs(pm, 0);
            auto [rc3, re3] = compile("a$", REG_EXTENDED | REG_NEWLINE);
            clearPm();
            const uint32_t eol = exec(re3, "a\nb", 1, pm);
            const auto em = offs(pm, 0);
            auto [rc4, re4] = compile("[^a]", REG_EXTENDED | REG_NEWLINE);
            const uint32_t nl = exec(re4, "\n", 1, pm);
            auto [rc5, re5] = compile("^a", REG_EXTENDED);
            const uint32_t notbol = exec(re5, "a", 1, pm, REG_NOTBOL);
            const uint32_t bol0 = exec(re5, "a", 1, pm, 0);
            for (uint32_t r : {re, re2, re3, re4, re5}) call("_regfree", {r});
            return rc == 0 && dot == REG_NOMATCH && rc2 == 0 && bol == 0 && bm == std::pair<int64_t,int64_t>(2, 3) &&
                   rc3 == 0 && eol == 0 && em == std::pair<int64_t,int64_t>(0, 1) && rc4 == 0 && nl == REG_NOMATCH &&
                   rc5 == 0 && notbol == REG_NOMATCH && bol0 == 0;
        });
        run("regcomp returns POSIX error codes", [&] {
            struct Case { const char* pat; uint32_t flags; uint32_t code; };
            const Case cases[] = {
                {"(", REG_EXTENDED, REG_EPAREN},
                {"[", REG_EXTENDED, REG_EBRACK},
                {"a{", REG_EXTENDED, REG_EBRACE},
                {"a{5,1}", REG_EXTENDED, REG_BADBR},
                {"\\", REG_EXTENDED, REG_EESCAPE},
                {"*", REG_EXTENDED, REG_BADRPT},
                {"[[:bogus:]]", REG_EXTENDED, REG_ECTYPE},
                {"[z-a]", REG_EXTENDED, REG_ERANGE},
                {"\\1", 0, REG_ESUBREG},
            };
            bool ok = true;
            for (auto& c : cases) {
                auto [rc, re] = compile(c.pat, c.flags);
                if (rc != c.code) { std::printf("    regcomp '%s' -> %u want %u\n", c.pat, rc, c.code); ok = false; }
                call("_regfree", {re});
            }
            return ok;
        });
        run("regerror reports Darwin strings and the size including NUL", [&] {
            const uint32_t buf = proc.heap().allocZeroed(128);
            auto expect = [&](uint32_t code, const char* text) {
                const uint32_t n = call("_regerror", {code, 0, buf, 128});
                return n == std::strlen(text) + 1 && proc.readCString(buf) == text;
            };
            const uint32_t small = proc.heap().alloc(8);
            for (int i = 0; i < 8; ++i) proc.mem().write<uint8_t>(small + i, 'X');
            const char* nomatch = "regexec() failed to match";
            const uint32_t ntrunc = call("_regerror", {REG_NOMATCH, 0, small, 4});
            const uint32_t nlen = call("_regerror", {REG_BADPAT, 0, 0, 0});
            return expect(REG_NOMATCH, nomatch) &&
                   expect(REG_BADPAT, "invalid regular expression") &&
                   expect(REG_ECTYPE, "invalid character class") &&
                   expect(REG_EESCAPE, "trailing backslash (\\)") &&
                   expect(REG_ESUBREG, "invalid backreference number") &&
                   expect(REG_EBRACK, "brackets ([ ]) not balanced") &&
                   expect(REG_EPAREN, "parentheses not balanced") &&
                   expect(REG_EBRACE, "braces not balanced") &&
                   expect(REG_BADBR, "invalid repetition count(s)") &&
                   expect(REG_ERANGE, "invalid character range") &&
                   expect(REG_BADRPT, "repetition-operator operand invalid") &&
                   expect(REG_INVARG, "invalid argument to regex routine") &&
                   ntrunc == std::strlen(nomatch) + 1 && proc.readCString(small) == "reg" &&
                   nlen == std::strlen("invalid regular expression") + 1;
        });
        run("regexec REG_NOMATCH and regfree invalidates the pattern", [&] {
            auto [rc, re] = compile("^a$", REG_EXTENDED);
            const uint32_t miss = exec(re, "b", 1, pm);
            const uint32_t buf = proc.heap().allocZeroed(64);
            const uint32_t n = call("_regerror", {miss, re, buf, 64});
            const uint32_t hit = exec(re, "a", 1, pm);
            call("_regfree", {re});
            const uint32_t after = exec(re, "a", 1, pm);
            call("_regfree", {re});
            return rc == 0 && miss == REG_NOMATCH && hit == 0 && after == REG_INVARG &&
                   n == std::strlen("regexec() failed to match") + 1 &&
                   proc.readCString(buf) == "regexec() failed to match";
        });
        run("leftmost match may be empty", [&] {
            auto [rc, re] = compile("a*", REG_EXTENDED);
            clearPm();
            const uint32_t er = exec(re, "xaa", 1, pm);
            const bool empty = rc == 0 && er == 0 && offs(pm, 0) == std::pair<int64_t,int64_t>(0, 0);
            auto [rc2, re2] = compile("a+", REG_EXTENDED);
            clearPm();
            const uint32_t er2 = exec(re2, "xaa", 1, pm);
            const bool plus = rc2 == 0 && er2 == 0 && offs(pm, 0) == std::pair<int64_t,int64_t>(1, 3);
            auto [rc3, re3] = compile("", REG_EXTENDED);
            clearPm();
            const uint32_t er3 = exec(re3, "abc", 1, pm);
            const bool blank = rc3 == 0 && er3 == 0 && offs(pm, 0) == std::pair<int64_t,int64_t>(0, 0);
            call("_regfree", {re}); call("_regfree", {re2}); call("_regfree", {re3});
            return empty && plus && blank;
        });

        run("_FORTIFY_SOURCE test strcpy", [&] {
            uint32_t src = call("_malloc", {100}); uint32_t dst = call("_malloc", {100});
            call("_memset", {src, 'A', 99}); proc.mem().write<uint8_t>(src + 99, 0); call("_memset", {dst, 'B', 100});
            auto check_fault = [&](const char* fn, std::initializer_list<uint32_t> args) {
                bool caught = false; try { proc.callGuest(cpu, *proc.lookupSymbol(fn), args); } catch (const CPU::GuestFault& e) { std::string msg = e.what(); caught = (msg.find("buffer overflow detected") != std::string::npos || msg.find("invalid guest memory access") != std::string::npos || msg.find("guest call did not return") != std::string::npos); } catch (...) { caught = false; } return caught;
            };
            if (call("___strcpy_chk", {dst, src, 100}) != dst) return false;
            if (!check_fault("___strcpy_chk", {dst, src, 49})) return false;
            call("_free", {src}); call("_free", {dst}); return true;
        });
        run("_FORTIFY_SOURCE rejects unreadable source buffers and checks the overflow guard", [&] {
            // callGuest reports every host-side fault as "guest call did not return", so the fault is only known
            // to be caused by the missing read permission if the same call succeeds once the hole is mapped.
            auto faults = [&](const char* fn, std::initializer_list<uint32_t> args) {
                try { proc.callGuest(cpu, *proc.lookupSymbol(fn), args); }
                catch (const CPU::GuestFault&) { return true; }
                catch (...) { return false; }
                return false;
            };
            // Guest pages cannot be made unreadable with protect() (no-permission becomes read-only), so the
            // source spans a hole: page 0 mapped, page 1 unmapped, page 2 mapped; only 'A' bytes, NUL at the end.
            // search above the heap range (0x10000000..0x80000000) so heap growth cannot commit the hole
            const uint32_t src = proc.mem().findFree(12288, 0x80000000u);
            proc.mem().map(src, 4096, Memory::kProtRW);
            proc.mem().map(src + 8192, 4096, Memory::kProtRW);
            for (uint32_t i = 0; i < 4096; ++i) { proc.mem().write<uint8_t>(src + i, 'A'); proc.mem().write<uint8_t>(src + 8192 + i, 'A'); }
            proc.mem().write<uint8_t>(src + 8192 + 4095, 0);
            const uint32_t dst = call("_malloc", {9000});
            call("_memset", {dst, 'X', 9000});
            auto dstUntouched = [&] { for (uint32_t i = 0; i < 9000; ++i) if (proc.mem().read<uint8_t>(dst + i) != 'X') return false; return true; };
            bool ok = faults("___strcpy_chk", {dst, src, 9000}) && dstUntouched();
            ok = ok && faults("___memcpy_chk", {dst, src, 9000, 9000}) && dstUntouched();
            ok = ok && faults("___memcpy_chk", {dst, src + 4095, 2, 9000}) && dstUntouched();
            // Same calls succeed once the hole is mapped, so the faults above came from the source permission check.
            proc.mem().map(src + 4096, 4096, Memory::kProtRW);
            for (uint32_t i = 0; i < 4096; ++i) proc.mem().write<uint8_t>(src + 4096 + i, 'A');
            ok = ok && !faults("___memcpy_chk", {dst, src, 9000, 9000}) && proc.mem().read<uint8_t>(dst + 5000) == 'A';
            proc.mem().write<uint8_t>(src + 4196, 0);         // strlen now 4196: fits dst
            call("_memset", {dst, 'X', 9000});
            ok = ok && !faults("___strcpy_chk", {dst, src, 9000}) && proc.mem().read<uint8_t>(dst + 4196) == 0;
            // dst + n wraps past 2^32: the write-only memset_chk must stop before touching memory.
            call("_memset", {dst, 'X', 9000});
            const uint32_t wrapLen = 0xFFFFFFFFu - dst + 1;
            ok = ok && faults("___memset_chk", {dst, 'Z', wrapLen, wrapLen}) && dstUntouched();
            ok = ok && faults("___memcpy_chk", {dst, src, 150, 100}) && dstUntouched();
            proc.mem().unmap(src, 12288);
            call("_free", {dst});
            return ok;
        });
        run("_FORTIFY_SOURCE strcat/strncat bound the destination scan by destlen", [&] {
            // Unterminated destination right before an unmapped page: the bounded scan reports the
            // __chk_fail (info 0); the old unbounded scan walked into the hole and faulted at its address.
            const uint32_t base = proc.mem().findFree(8192, 0x80000000u);
            proc.mem().map(base, 4096, Memory::kProtRW);
            const uint32_t dst = base + 4096 - 16;
            for (uint32_t i = 0; i < 16; ++i) proc.mem().write<uint8_t>(dst + i, 'D');
            const uint32_t src = call("_malloc", {8});
            call("_memset", {src, 'S', 4}); proc.mem().write<uint8_t>(src + 4, 0);
            auto faultInfo = [&](const char* fn, std::initializer_list<uint32_t> args, uint32_t& info) {
                try { proc.callGuest(cpu, *proc.lookupSymbol(fn), args); }
                catch (const CPU::GuestFault& e) { info = e.info; return true; }
                return false;
            };
            uint32_t i1 = 0xFFFF, i2 = 0xFFFF;
            const bool ok = faultInfo("___strcat_chk", {dst, src, 16}, i1) && faultInfo("___strncat_chk", {dst, src, 4, 16}, i2);
            proc.mem().unmap(base, 4096);
            call("_free", {src});
            if (!ok || i1 != 0 || i2 != 0) std::fprintf(stderr, "strcat info %u strncat info %u\n", i1, i2);
            return ok && i1 == 0 && i2 == 0;
        });
        run("_FORTIFY_SOURCE test strncpy", [&] {
            uint32_t src = call("_malloc", {100}); uint32_t dst = call("_malloc", {100});
            call("_memset", {src, 'A', 99}); proc.mem().write<uint8_t>(src + 99, 0); call("_memset", {dst, 'B', 100});
            auto check_fault = [&](const char* fn, std::initializer_list<uint32_t> args) {
                bool caught = false; try { proc.callGuest(cpu, *proc.lookupSymbol(fn), args); } catch (const CPU::GuestFault& e) { std::string msg = e.what(); caught = (msg.find("buffer overflow detected") != std::string::npos || msg.find("invalid guest memory access") != std::string::npos || msg.find("guest call did not return") != std::string::npos); } catch (...) { caught = false; } return caught;
            };
            if (call("___strncpy_chk", {dst, src, 50, 100}) != dst) return false;
            if (!check_fault("___strncpy_chk", {dst, src, 150, 100})) return false;
            call("_free", {src}); call("_free", {dst}); return true;
        });
        run("_FORTIFY_SOURCE test strcat", [&] {
            uint32_t src = call("_malloc", {100}); uint32_t dst = call("_malloc", {100});
            call("_memset", {src, 'A', 99}); proc.mem().write<uint8_t>(src + 99, 0); call("_memset", {dst, 'B', 100});
            auto check_fault = [&](const char* fn, std::initializer_list<uint32_t> args) {
                bool caught = false; try { proc.callGuest(cpu, *proc.lookupSymbol(fn), args); } catch (const CPU::GuestFault& e) { std::string msg = e.what(); caught = (msg.find("buffer overflow detected") != std::string::npos || msg.find("invalid guest memory access") != std::string::npos || msg.find("guest call did not return") != std::string::npos); } catch (...) { caught = false; } return caught;
            };
            proc.mem().write<uint8_t>(dst, 0);
            if (call("___strcat_chk", {dst, src, 100}) != dst) return false;
            proc.mem().write<uint8_t>(dst, 0);
            if (!check_fault("___strcat_chk", {dst, src, 49})) return false;
            call("_free", {src}); call("_free", {dst}); return true;
        });
        run("_FORTIFY_SOURCE test strncat", [&] {
            uint32_t src = call("_malloc", {100}); uint32_t dst = call("_malloc", {100});
            call("_memset", {src, 'A', 99}); proc.mem().write<uint8_t>(src + 99, 0); call("_memset", {dst, 'B', 100});
            auto check_fault = [&](const char* fn, std::initializer_list<uint32_t> args) {
                bool caught = false; try { proc.callGuest(cpu, *proc.lookupSymbol(fn), args); } catch (const CPU::GuestFault& e) { std::string msg = e.what(); caught = (msg.find("buffer overflow detected") != std::string::npos || msg.find("invalid guest memory access") != std::string::npos || msg.find("guest call did not return") != std::string::npos); } catch (...) { caught = false; } return caught;
            };
            proc.mem().write<uint8_t>(dst, 0);
            if (call("___strncat_chk", {dst, src, 50, 100}) != dst) return false;
            proc.mem().write<uint8_t>(dst, 0);
            if (!check_fault("___strncat_chk", {dst, src, 150, 50})) return false;
            call("_free", {src}); call("_free", {dst}); return true;
        });
        run("_FORTIFY_SOURCE test memcpy", [&] {
            uint32_t src = call("_malloc", {100}); uint32_t dst = call("_malloc", {100});
            call("_memset", {src, 'A', 99}); proc.mem().write<uint8_t>(src + 99, 0); call("_memset", {dst, 'B', 100});
            auto check_fault = [&](const char* fn, std::initializer_list<uint32_t> args) {
                bool caught = false; try { proc.callGuest(cpu, *proc.lookupSymbol(fn), args); } catch (const CPU::GuestFault& e) { std::string msg = e.what(); caught = (msg.find("buffer overflow detected") != std::string::npos || msg.find("invalid guest memory access") != std::string::npos || msg.find("guest call did not return") != std::string::npos); } catch (...) { caught = false; } return caught;
            };
            if (call("___memcpy_chk", {dst, src, 50, 100}) != dst) return false;
            if (!check_fault("___memcpy_chk", {dst, src, 150, 100})) return false;
            call("_free", {src}); call("_free", {dst}); return true;
        });
        run("_FORTIFY_SOURCE test memmove", [&] {
            uint32_t src = call("_malloc", {100}); uint32_t dst = call("_malloc", {100});
            call("_memset", {src, 'A', 99}); proc.mem().write<uint8_t>(src + 99, 0); call("_memset", {dst, 'B', 100});
            auto check_fault = [&](const char* fn, std::initializer_list<uint32_t> args) {
                bool caught = false; try { proc.callGuest(cpu, *proc.lookupSymbol(fn), args); } catch (const CPU::GuestFault& e) { std::string msg = e.what(); caught = (msg.find("buffer overflow detected") != std::string::npos || msg.find("invalid guest memory access") != std::string::npos || msg.find("guest call did not return") != std::string::npos); } catch (...) { caught = false; } return caught;
            };
            if (call("___memmove_chk", {dst, src, 50, 100}) != dst) return false;
            if (call("___memmove_chk", {dst + 10, dst, 20, 100}) != dst + 10) return false;
            if (!check_fault("___memmove_chk", {dst, src, 150, 100})) return false;
            call("_free", {src}); call("_free", {dst}); return true;
        });
        run("_FORTIFY_SOURCE test memset", [&] {
            uint32_t src = call("_malloc", {100}); uint32_t dst = call("_malloc", {100});
            call("_memset", {src, 'A', 99}); proc.mem().write<uint8_t>(src + 99, 0); call("_memset", {dst, 'B', 100});
            auto check_fault = [&](const char* fn, std::initializer_list<uint32_t> args) {
                bool caught = false; try { proc.callGuest(cpu, *proc.lookupSymbol(fn), args); } catch (const CPU::GuestFault& e) { std::string msg = e.what(); caught = (msg.find("buffer overflow detected") != std::string::npos || msg.find("invalid guest memory access") != std::string::npos || msg.find("guest call did not return") != std::string::npos); } catch (...) { caught = false; } return caught;
            };
            if (call("___memset_chk", {dst, 'X', 50, 100}) != dst) return false;
            if (!check_fault("___memset_chk", {dst, 'X', 150, 100})) return false;
            call("_free", {src}); call("_free", {dst}); return true;
        });
        run("_FORTIFY_SOURCE test strlcpy", [&] {
            uint32_t src = call("_malloc", {100}); uint32_t dst = call("_malloc", {100});
            call("_memset", {src, 'A', 99}); proc.mem().write<uint8_t>(src + 99, 0); call("_memset", {dst, 'B', 100});
            auto check_fault = [&](const char* fn, std::initializer_list<uint32_t> args) {
                bool caught = false; try { proc.callGuest(cpu, *proc.lookupSymbol(fn), args); } catch (const CPU::GuestFault& e) { std::string msg = e.what(); caught = (msg.find("buffer overflow detected") != std::string::npos || msg.find("invalid guest memory access") != std::string::npos || msg.find("guest call did not return") != std::string::npos); } catch (...) { caught = false; } return caught;
            };
            if (call("___strlcpy_chk", {dst, src, 50, 100}) != 99) return false;
            if (!check_fault("___strlcpy_chk", {dst, src, 50, 20})) return false;
            call("_free", {src}); call("_free", {dst}); return true;
        });
        run("_FORTIFY_SOURCE test strlcat", [&] {
            uint32_t src = call("_malloc", {100}); uint32_t dst = call("_malloc", {100});
            call("_memset", {src, 'A', 99}); proc.mem().write<uint8_t>(src + 99, 0); call("_memset", {dst, 'B', 100});
            auto check_fault = [&](const char* fn, std::initializer_list<uint32_t> args) {
                bool caught = false; try { proc.callGuest(cpu, *proc.lookupSymbol(fn), args); } catch (const CPU::GuestFault& e) { std::string msg = e.what(); caught = (msg.find("buffer overflow detected") != std::string::npos || msg.find("invalid guest memory access") != std::string::npos || msg.find("guest call did not return") != std::string::npos); } catch (...) { caught = false; } return caught;
            };
            proc.mem().write<uint8_t>(dst, 0);
            if (call("___strlcat_chk", {dst, src, 50, 100}) != 99) return false;
            proc.mem().write<uint8_t>(dst, 0);
            if (!check_fault("___strlcat_chk", {dst, src, 50, 20})) return false;
            call("_free", {src}); call("_free", {dst}); return true;
        });

        run("_FORTIFY_SOURCE test strlcat edge cases", [&] {
            uint32_t src = call("_malloc", {100}); uint32_t dst = call("_malloc", {100});
            call("_memset", {src, 'A', 99}); proc.mem().write<uint8_t>(src + 99, 0);
            auto check_fault = [&](const char* fn, std::initializer_list<uint32_t> args) {
                bool caught = false; try { proc.callGuest(cpu, *proc.lookupSymbol(fn), args); } catch (const CPU::GuestFault& e) { std::string msg = e.what(); caught = (msg.find("buffer overflow detected") != std::string::npos || msg.find("invalid guest memory access") != std::string::npos || msg.find("guest call did not return") != std::string::npos); } catch (...) { caught = false; } return caught;
            };
            proc.mem().write<uint8_t>(dst, 0);
            if (call("___strlcat_chk", {dst, src, 0, 100}) != 99) return false;
            proc.mem().write<uint8_t>(dst, 0);
            if (call("___strlcat_chk", {dst, src, 1, 100}) != 99) return false; // 1-byte means just read dst NUL, then slen=99, return 99

            // test unmapped dst with n=0
            if (proc.mem().isMapped(0x40000000)) return false; // precondition: the address really is unmapped
            if (call("___strlcat_chk", {0x40000000, src, 0, 100}) != 99) return false;

            // test in bounds but destlen == SIZE_MAX sentinel
            proc.mem().write<uint8_t>(dst, 0);
            if (call("___strlcat_chk", {dst, src, 50, 0xFFFFFFFF}) != 99) return false;
            call("_free", {src}); call("_free", {dst}); return true;
        });
                                run("_FORTIFY_SOURCE test 32-bit wraparound", [&] {
            uint32_t src = call("_malloc", {100}); uint32_t dst = call("_malloc", {100});
            call("_memset", {dst, 'X', 100});

            auto check_fault = [&](const char* fn, std::initializer_list<uint32_t> args) {
                bool caught = false;
                try {
                    proc.callGuest(cpu, *proc.lookupSymbol(fn), args);
                } catch (const CPU::GuestFault& e) {
                    std::string msg = e.what();
                    // Any fault is OK for this test as long as it faults and buffer isn't modified
                    caught = true;
                } catch (...) {
                    caught = false;
                }
                return caught;
            };

            uint32_t exact_overflow_len = 0xFFFFFFFF - dst + 1; // 2^32 - dst

            bool ok = true;
            if (!check_fault("___memcpy_chk", {dst, src, exact_overflow_len, exact_overflow_len})) {
                ok = false;
            }

            uint8_t val = proc.mem().read<uint8_t>(dst);
            if (val != 'X') {
                ok = false;
            }

            call("_free", {src}); call("_free", {dst}); return ok;
        });

        run("tmpnam names are unique inside the guest sandbox tmp directory", [&] {
            const std::string root = proc.guestHomePath() + "/tmp/";
            std::vector<std::string> names;
            for (int i = 0; i < 6; ++i) {
                const uint32_t buf = proc.heap().allocZeroed(1024);
                const uint32_t p = call("_tmpnam", {buf});
                if (p != buf) return false;
                names.push_back(proc.readCString(buf));
            }
            bool ok = names.size() == 6;
            for (size_t i = 0; i < names.size(); ++i) {
                ok = ok && names[i].rfind(root, 0) == 0;
                ok = ok && names[i].find("..") == std::string::npos;
                ok = ok && !std::filesystem::exists(names[i]);
                for (size_t j = 0; j < i; ++j) ok = ok && names[i] != names[j];
            }
            if (!names.empty()) {
                std::ofstream(names[0]) << "taken";
                const uint32_t buf = proc.heap().allocZeroed(1024);
                const uint32_t p = call("_tmpnam", {buf});
                const std::string again = proc.readCString(p);
                ok = ok && p == buf && again != names[0] && again.rfind(root, 0) == 0 && !std::filesystem::exists(again);
                std::filesystem::remove(names[0]);
            }
            return ok;
        });
        run("tmpnam(NULL) reuses one guest static buffer", [&] {
            const std::string root = proc.guestHomePath() + "/tmp/";
            const uint32_t p1 = call("_tmpnam", {0});
            const std::string s1 = proc.readCString(p1);
            const uint32_t p2 = call("_tmpnam", {0});
            const std::string s2 = proc.readCString(p2);
            return p1 != 0 && p1 == p2 && s1 != s2 && s1.rfind(root, 0) == 0 && s2.rfind(root, 0) == 0 &&
                   !std::filesystem::exists(s1) && !std::filesystem::exists(s2);
        });
        run("symlink with an absolute guest target resolves inside the sandbox; readlink returns it", [&] {
            namespace fs = std::filesystem;
            const std::string hostDir = proc.homePath() + "/tmp/symlink_test/";
            std::error_code ec;
            fs::remove_all(hostDir, ec);
            fs::create_directories(hostDir, ec);
            { std::ofstream out(hostDir + "target.txt"); out << "hi"; }
            const std::string guestDir = proc.guestHomePath() + "/tmp/symlink_test/";
            const uint32_t rc = call("_symlink", {proc.allocCString(guestDir + "target.txt"), proc.allocCString(guestDir + "link")});
            const uint32_t rel = call("_symlink", {proc.allocCString("target.txt"), proc.allocCString(guestDir + "rel")});
            const uint32_t again = call("_symlink", {proc.allocCString("target.txt"), proc.allocCString(guestDir + "rel")});
            const uint32_t buf = proc.heap().allocZeroed(64);
            const int32_t n = int32_t(call("_readlink", {proc.allocCString(guestDir + "rel"), buf, 64}));
            std::ifstream in(hostDir + "link");
            std::string got;
            in >> got;
            return rc == 0 && rel == 0 && int32_t(again) == -1 && fs::is_symlink(hostDir + "link", ec) &&
                   fs::read_symlink(hostDir + "link", ec).is_relative() && got == "hi" && n == 10;
        });
        run("file open ring tracks guest opens across fopen, open, NSData, and NSFileHandle with live updates and rollover", [&] {
            namespace fs = std::filesystem;
            const std::string testDir = proc.homePath() + "/tmp/file_ring_test/";
            std::error_code ec;
            fs::create_directories(testDir, ec);
            const std::string filePath = testDir + "sample.bin";
            {
                std::ofstream out(filePath, std::ios::binary);
                out << "1234567890";
            }
            const uint32_t pathPtr = proc.allocCString(filePath);
            const uint32_t modePtr = proc.allocCString("rb");

            // 1. fopen (read 3 bytes, leave open)
            const uint32_t fp = call("_fopen", {pathPtr, modePtr});
            if (fp) {
                const uint32_t buf = proc.heap().allocZeroed(16);
                call("_fread", {buf, 1, 3, fp});
            }

            // Check live visibility before close
            bool sawLiveOpen = false;
            for (const auto& rec : proc.fileRing()) {
                if (rec.path == filePath && rec.api == "fopen" && rec.isOpen && rec.bytesRead == 3) sawLiveOpen = true;
            }

            if (fp) call("_fclose", {fp});

            // 2. open
            const int32_t fd = int32_t(call("_open", {pathPtr, 0}));
            if (fd >= 0) {
                const uint32_t buf = proc.heap().allocZeroed(16);
                call("_read", {uint32_t(fd), buf, 4});
                call("_close", {uint32_t(fd)});
            }

            // 3. NSData
            const auto nsPath = Foundation::makeString(cx, filePath);
            const auto nsData = ObjC::id(r.send(cpu, r.getClass("NSData"), "dataWithContentsOfFile:", {nsPath}));

            // 4. NSFileHandle
            const auto handle = ObjC::id(r.send(cpu, r.getClass("NSFileHandle"), "fileHandleForReadingAtPath:", {nsPath}));
            if (handle) {
                r.send(cpu, handle, "readDataOfLength:", {5});
                r.send(cpu, handle, "closeFile");
            }

            bool sawFopen = false, sawOpen = false, sawNSData = false, sawHandle = false;
            for (const auto& rec : proc.fileRing()) {
                if (rec.path == filePath) {
                    if (rec.api == "fopen" && rec.size == 10 && rec.bytesRead == 3 && !rec.isOpen) sawFopen = true;
                    if (rec.api == "open" && rec.size == 10 && rec.bytesRead == 4 && !rec.isOpen) sawOpen = true;
                    if (rec.api == "NSData" && rec.size == 10 && rec.bytesRead == 10) sawNSData = true;
                    if (rec.api == "NSFileHandle" && rec.size == 10 && rec.bytesRead == 5 && !rec.isOpen) sawHandle = true;
                }
            }

            // 5. Test 32-entry rollover
            for (int i = 0; i < 40; ++i) {
                const std::string dummyPath = testDir + "dummy_" + std::to_string(i) + ".txt";
                proc.recordFileOpen(dummyPath, "test", 0, 10, 0, false);
            }
            const auto history = proc.fileRing();
            fs::remove_all(testDir, ec);

            return sawLiveOpen && sawFopen && sawOpen && sawNSData && sawHandle && history.size() == 32;
        });
        run("pmatch slots past re_nsub are -1", [&] {
            auto [rc, re] = compile("(a)", REG_EXTENDED);
            for (uint32_t i = 0; i < 4 * 16; i += 8) proc.mem().write<int64_t>(pm + i, 7);
            const uint32_t er = exec(re, "a", 4, pm);
            const bool ok = rc == 0 && er == 0 && nsub(re) == 1 &&
                            offs(pm, 0) == std::pair<int64_t, int64_t>(0, 1) &&
                            offs(pm, 1) == std::pair<int64_t, int64_t>(0, 1) &&
                            offs(pm, 2) == std::pair<int64_t, int64_t>(-1, -1) &&
                            offs(pm, 3) == std::pair<int64_t, int64_t>(-1, -1);
            call("_regfree", {re});
            return ok;
        });
        run("REG_STARTEND limits the region and reports absolute offsets", [&] {
            // Darwin: the pmatch[0] region is the string, so ^/$ are relative to
            // it, and the reported offsets are from the guest pointer.
            constexpr uint32_t REG_STARTEND = 4;
            auto [rc, re] = compile("a+", REG_EXTENDED);
            proc.mem().write<int64_t>(pm + 0, 0);
            proc.mem().write<int64_t>(pm + 8, 2);
            const uint32_t er = exec(re, "aaaa", 1, pm, REG_STARTEND);
            const bool clipped = rc == 0 && er == 0 && offs(pm, 0) == std::pair<int64_t, int64_t>(0, 2);
            auto [rc2, re2] = compile("^b+", REG_EXTENDED);
            proc.mem().write<int64_t>(pm + 0, 3);
            proc.mem().write<int64_t>(pm + 8, 6);
            const uint32_t er2 = exec(re2, "aaabbbccc", 1, pm, REG_STARTEND);
            const bool anchored = rc2 == 0 && er2 == 0 && offs(pm, 0) == std::pair<int64_t, int64_t>(3, 6);
            auto [rc3, re3] = compile("b+$", REG_EXTENDED);
            proc.mem().write<int64_t>(pm + 0, 3);
            proc.mem().write<int64_t>(pm + 8, 6);
            const uint32_t er3 = exec(re3, "aaabbbccc", 1, pm, REG_STARTEND);
            const bool end = rc3 == 0 && er3 == 0 && offs(pm, 0) == std::pair<int64_t, int64_t>(3, 6);
            auto [rc4, re4] = compile("^b", REG_EXTENDED);
            proc.mem().write<int64_t>(pm + 0, 3);
            proc.mem().write<int64_t>(pm + 8, 6);
            const uint32_t notbol = exec(re4, "aaabbbccc", 1, pm, REG_STARTEND | REG_NOTBOL);
            for (uint32_t r : {re, re2, re3, re4}) call("_regfree", {r});
            return clipped && anchored && end && rc4 == 0 && notbol == REG_NOMATCH;
        });
        run("unbounded repeats stay leftmost-longest", [&] {
            const std::string a5000(5000, 'a');
            auto [rc, re] = compile("a*", REG_EXTENDED);
            clearPm();
            const uint32_t er = exec(re, a5000.c_str(), 1, pm);
            const bool star = rc == 0 && er == 0 && offs(pm, 0) == std::pair<int64_t, int64_t>(0, 5000);
            std::string a4097b(4097, 'a');
            a4097b.push_back('b');
            auto [rc2, re2] = compile("a*b", REG_EXTENDED);
            clearPm();
            const uint32_t er2 = exec(re2, a4097b.c_str(), 1, pm);
            const bool left = rc2 == 0 && er2 == 0 && offs(pm, 0) == std::pair<int64_t, int64_t>(0, 4098);
            const uint32_t miss = exec(re2, a5000.c_str(), 1, pm);
            std::string a5000b = a5000;
            a5000b.push_back('b');
            clearPm();
            const uint32_t er3 = exec(re2, a5000b.c_str(), 1, pm);
            const bool plusb = er3 == 0 && offs(pm, 0) == std::pair<int64_t, int64_t>(0, 5001);
            std::string a20b(20, 'a');
            a20b.push_back('b');
            auto [rc4, re4] = compile("(a*)*b", REG_EXTENDED);
            clearPm();
            const uint32_t er4 = exec(re4, a20b.c_str(), 2, pm);
            const bool nested = rc4 == 0 && er4 == 0 && offs(pm, 0) == std::pair<int64_t, int64_t>(0, 21) &&
                                offs(pm, 1) == std::pair<int64_t, int64_t>(0, 20);
            call("_regfree", {re}); call("_regfree", {re2}); call("_regfree", {re4});
            return star && left && miss == REG_NOMATCH && plusb && nested;
        });
#if defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(undefined_behavior_sanitizer)
#    define HVM_RE_SANITIZER 1
#  endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_UNDEFINED__)
#  define HVM_RE_SANITIZER 1
#endif
#ifdef HVM_RE_SANITIZER
        constexpr long long kRepeatBudgetMs = 20000;
        constexpr long long kMissBudgetMs = 5000;
#else
        constexpr long long kRepeatBudgetMs = 2000;
        constexpr long long kMissBudgetMs = 1000;
#endif
        run("long inputs of 100k characters stay fast", [&] {
            const std::string a(100000, 'a');
            std::string ab = a;
            ab.push_back('b');
            auto [rc, re] = compile("a*", REG_EXTENDED);
            auto [rc2, re2] = compile("a*b", REG_EXTENDED);
            auto [rc3, re3] = compile("(a*)*b", REG_EXTENDED);
            clearPm();
            const auto t0 = std::chrono::steady_clock::now();
            const uint32_t e1 = exec(re, a.c_str(), 1, pm);
            const auto s1 = offs(pm, 0);
            clearPm();
            const uint32_t e2 = exec(re2, ab.c_str(), 1, pm);
            const auto s2 = offs(pm, 0);
            clearPm();
            const uint32_t e3 = exec(re3, ab.c_str(), 2, pm);
            const auto s3 = offs(pm, 0);
            const auto s4 = offs(pm, 1);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0).count();
            std::printf("    regex 100k: %lld ms\n", static_cast<long long>(ms));
            call("_regfree", {re}); call("_regfree", {re2}); call("_regfree", {re3});
            return rc == 0 && rc2 == 0 && rc3 == 0 && e1 == 0 && e2 == 0 && e3 == 0 &&
                   s1 == std::pair<int64_t, int64_t>(0, 100000) &&
                   s2 == std::pair<int64_t, int64_t>(0, 100001) &&
                   s3 == std::pair<int64_t, int64_t>(0, 100001) &&
                   s4 == std::pair<int64_t, int64_t>(0, 100000) &&
                   ms < kRepeatBudgetMs;
        });
        run("unanchored (a|aa)*c on 100k a's finishes", [&] {
            const std::string a(100000, 'a');
            auto [rc, re] = compile("(a|aa)*c", REG_EXTENDED);
            clearPm();
            const auto t0 = std::chrono::steady_clock::now();
            const uint32_t er = exec(re, a.c_str(), 1, pm);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0).count();
            std::printf("    regex (a|aa)*c 100k: %lld ms\n", static_cast<long long>(ms));
            call("_regfree", {re});
            return rc == 0 && er == REG_NOMATCH && ms < kMissBudgetMs;
        });
        run("glibc (a*)* backreference splits the run in half", [&] {
            auto [rc, re] = compile("(a*)*\\1", REG_EXTENDED);
            bool ok = rc == 0;
            for (int n : {20, 40, 80}) {
                const std::string s(size_t(n), 'a');
                clearPm();
                const uint32_t er = exec(re, s.c_str(), 2, pm);
                const auto whole = offs(pm, 0);
                const auto g = offs(pm, 1);
                const bool row = er == 0 && whole == std::pair<int64_t, int64_t>(0, n) &&
                                 g == std::pair<int64_t, int64_t>(0, n / 2);
                if (!row) {
                    std::printf("    (a*)*\\1 n=%d er=%u g0=(%lld,%lld) g1=(%lld,%lld)\n", n, er,
                                static_cast<long long>(whole.first), static_cast<long long>(whole.second),
                                static_cast<long long>(g.first), static_cast<long long>(g.second));
                }
                ok = ok && row;
            }
            call("_regfree", {re});
            return ok;
        });
        run("REG_PEND ends the pattern at re_endp", [&] {
            constexpr uint32_t REG_PEND = 32;
            const uint32_t raw = proc.allocCString("a+Xjunk");
            const uint32_t preg = proc.heap().allocZeroed(32);
            proc.mem().write<uint32_t>(preg + 8, raw + 3);
            const uint32_t rc = call("_regcomp", {preg, raw, REG_EXTENDED | REG_PEND});
            clearPm();
            const uint32_t er = exec(preg, "aaaX", 1, pm);
            const bool clipped = rc == 0 && er == 0 && offs(pm, 0) == std::pair<int64_t, int64_t>(0, 4);
            const uint32_t emptyRe = proc.heap().allocZeroed(32);
            proc.mem().write<uint32_t>(emptyRe + 8, raw);
            const uint32_t rcEmpty = call("_regcomp", {emptyRe, raw, REG_EXTENDED | REG_PEND});
            clearPm();
            const uint32_t erEmpty = exec(emptyRe, "abc", 1, pm);
            const bool blank = rcEmpty == 0 && erEmpty == 0 && offs(pm, 0) == std::pair<int64_t, int64_t>(0, 0);
            const uint32_t bad = proc.heap().allocZeroed(32);
            proc.mem().write<uint32_t>(bad + 8, raw - 1);
            const uint32_t rcBad = call("_regcomp", {bad, raw, REG_EXTENDED | REG_PEND});
            const uint32_t erBad = exec(bad, "a", 1, pm);
            call("_regfree", {preg});
            call("_regfree", {emptyRe});
            call("_regfree", {bad});
            return clipped && blank && rcBad == REG_INVARG && erBad == REG_INVARG;
        });
    }

    // 2f4. libc++ (std::__1) threading and concurrency
    {
        auto call = [&](const char* sym, std::initializer_list<uint32_t> args) -> uint32_t {
            auto a = proc.lookupSymbol(sym);
            return a ? uint32_t(proc.callGuest(cpu, *a, args)) : 0xDEADu;
        };
        auto& m = proc.mem();
        const uint32_t tls_ptr = call("__ZNSt3__119__thread_local_dataEv", {});
        const uint32_t tls_key = tls_ptr != 0xDEADu ? m.read<uint32_t>(tls_ptr) : 0;
        bool tls_ok = tls_ptr != 0 && tls_ptr != 0xDEADu && tls_key != 0;

        const uint32_t ts = proc.heap().allocZeroed(16);
        const uint32_t ts_ret = call("__ZNSt3__115__thread_structC1Ev", {ts});
        call("__ZNSt3__115__thread_structD1Ev", {ts});
        bool ts_ok = ts_ret == ts;

        const uint32_t mtx = proc.heap().allocZeroed(16);
        call("_pthread_mutex_init", {mtx, 0});
        const uint32_t lock_ret = call("__ZNSt3__15mutex4lockEv", {mtx});
        const uint32_t unlock_ret = call("__ZNSt3__15mutex6unlockEv", {mtx});
        bool mtx_ok = lock_ret == 0 && unlock_ret == 0;
        // Error paths: an error-checking mutex rejects a second lock by its owner and an unlock by a
        // non-owner, and the shim reports both as exceptions with the errno text.
        {
            const uint32_t attr = proc.heap().allocZeroed(16), emtx = proc.heap().allocZeroed(64);
            const bool setupOk = call("_pthread_mutexattr_init", {attr}) == 0 &&
                                 call("_pthread_mutexattr_settype", {attr, 1}) == 0 &&   // PTHREAD_MUTEX_ERRORCHECK
                                 call("_pthread_mutex_init", {emtx, attr}) == 0;
            auto throwsFor = [&](const char* sym, const char* needle) {
                try { call(sym, {emtx}); }
                catch (const std::exception& e) { return std::string(e.what()).find(needle) != std::string::npos; }
                return false;
            };
            bool errorPaths = false;
            if (setupOk) {   // skip the lock paths unless the error-checking mutex really exists
                const bool unownedUnlock = throwsFor("__ZNSt3__15mutex6unlockEv", "unlock failed");
                const bool firstLock = call("__ZNSt3__15mutex4lockEv", {emtx}) == 0;
                const bool relock = firstLock && throwsFor("__ZNSt3__15mutex4lockEv", "lock failed");
                if (firstLock) call("__ZNSt3__15mutex6unlockEv", {emtx});
                errorPaths = unownedUnlock && firstLock && relock;
            }
            call("_pthread_mutex_destroy", {emtx});
            mtx_ok = mtx_ok && errorPaths;
        }

        const uint64_t t1 = proc.callGuest(cpu, *proc.lookupSymbol("__ZNSt3__16chrono12steady_clock3nowEv"), {});
        const uint64_t t2 = proc.callGuest(cpu, *proc.lookupSymbol("__ZNSt3__16chrono12steady_clock3nowEv"), {});
        bool chrono_ok = t2 >= t1 && t1 > 0;
        // Darwin libc++ system_clock::now() counts microseconds since the epoch.
        const auto hostBefore = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const uint64_t sysNow = proc.callGuest(cpu, *proc.lookupSymbol("__ZNSt3__16chrono12system_clock3nowEv"), {});
        const auto hostAfter = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        chrono_ok = chrono_ok && int64_t(sysNow) >= hostBefore - 1000 && int64_t(sysNow) <= hostAfter + 1000;

        const uint32_t dur = proc.heap().allocZeroed(8);
        m.write<uint64_t>(dur, 1000000); // 1ms
        const auto sleep_start = std::chrono::steady_clock::now();
        call("__ZNSt3__111this_thread9sleep_forERKNS_6chrono8durationIxNS_5ratioILx1ELx1000000000EEEEE", {dur});
        const auto sleep_end = std::chrono::steady_clock::now();
        bool sleep_ok = std::chrono::duration_cast<std::chrono::milliseconds>(sleep_end - sleep_start).count() >= 1;

        // Thread identity and teardown checks
        const uint32_t th_obj = proc.heap().allocZeroed(16);
        const uint32_t worker_th = proc.startThread(proc.defineFunction("hvm_test_std_thread", [](Ctx& ctx){ ctx.cpu.r[0] = 0; }), 0, 64u << 10, false);

        // TLS separation: run a thread that gets a TLS key. Our tests for TLS are basic but we can just use the key we got.
        // C++ thread tests
        m.write<uint32_t>(th_obj, worker_th);
        call("__ZNSt3__16thread6detachEv", {th_obj});
        bool detach_ok = m.read<uint32_t>(th_obj) == 0;

        // Failed std::thread operations throw (libc++ throws system_error) with the errno text.
        auto throwsWith = [&](const char* sym, const char* needle) {
            try { call(sym, {th_obj}); }
            catch (const std::exception& e) { return std::string(e.what()).find(needle) != std::string::npos; }
            return false;
        };
        // Invalid (repeated) detach
        bool detach_inv_ok = throwsWith("__ZNSt3__16thread6detachEv", "detach failed") &&
                             throwsWith("__ZNSt3__16thread6detachEv", std::strerror(EINVAL));

        // Setup another thread to join
        const uint32_t worker2_th = proc.startThread(proc.defineFunction("hvm_test_std_thread2", [](Ctx& ctx){ ctx.cpu.r[0] = 0; }), 0, 64u << 10, false);
        m.write<uint32_t>(th_obj, worker2_th);
        call("__ZNSt3__16thread4joinEv", {th_obj});
        bool join_ok = m.read<uint32_t>(th_obj) == 0;

        // Invalid (repeated) join
        bool join_inv_ok = throwsWith("__ZNSt3__16thread4joinEv", "join failed") &&
                           throwsWith("__ZNSt3__16thread4joinEv", std::strerror(EINVAL));

        // Destructor on joinable thread
        m.write<uint32_t>(th_obj, worker_th);
        bool term_called = false;
        try {
            call("__ZNSt3__16threadD1Ev", {th_obj});
        } catch (const std::exception& e) {
            term_called = std::string(e.what()).find("terminate() called") != std::string::npos;
        }

        // Test condition variable wait: the worker owns the mutex, waits in a predicate loop, and the main
        // thread sets the predicate under the same mutex, so the notification cannot be lost.
        const uint32_t cv = proc.heap().allocZeroed(16);
        call("_pthread_cond_init", {cv, 0});
        const uint32_t cv_ul = proc.heap().allocZeroed(4); // unique_lock just holds the mutex ptr at offset 0
        m.write<uint32_t>(cv_ul, mtx);

        std::atomic<bool> cv_entered{false}, cv_woke{false};
        bool cv_predicate = false;   // guarded by the guest mutex `mtx`
        const uint32_t cv_waiter = proc.defineFunction("hvm_test_cv_wait", [&](Ctx& ctx) {
            proc.callGuest(ctx.cpu, *proc.lookupSymbol("_pthread_mutex_lock"), {mtx});
            cv_entered = true;
            auto a = proc.lookupSymbol("__ZNSt3__118condition_variable4waitERNS_11unique_lockINS_5mutexEEE");
            while (!cv_predicate && a) proc.callGuest(ctx.cpu, *a, {cv, cv_ul});
            cv_woke = cv_predicate;
            proc.callGuest(ctx.cpu, *proc.lookupSymbol("_pthread_mutex_unlock"), {mtx});
            ctx.cpu.r[0] = 0;
        });
        const uint32_t cv_th = proc.startThread(cv_waiter, 0, 64u << 10, false);
        for (int i = 0; i < 400 && !cv_entered.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        call("_pthread_mutex_lock", {mtx});          // only succeeds once the worker is inside cond_wait
        cv_predicate = true;
        call("__ZNSt3__118condition_variable10notify_oneEv", {cv});
        call("_pthread_mutex_unlock", {mtx});

        uint32_t cv_ret;
        proc.joinThread(cv_th, &cv_ret);
        bool cv_ok = cv_woke.load();

        // Test TLS isolation
        const uint32_t tls_test_key = m.read<uint32_t>(tls_ptr);
        std::atomic<uint32_t> th1_val{0}, th2_val{0};
        std::atomic<int> tls_set{0};   // both workers store before either reads, so a shared slot would show up
        auto tlsWorker = [&](uint32_t value, std::atomic<uint32_t>& out) {
            return [&, value](Ctx& ctx) {
                auto set_fn = proc.lookupSymbol("_pthread_setspecific");
                auto get_fn = proc.lookupSymbol("_pthread_getspecific");
                if (set_fn) proc.callGuest(ctx.cpu, *set_fn, {tls_test_key, value});
                ++tls_set;
                for (int i = 0; i < 400 && tls_set.load() < 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
                if (get_fn) out = uint32_t(proc.callGuest(ctx.cpu, *get_fn, {tls_test_key}));
                ctx.cpu.r[0] = 0;
            };
        };
        const uint32_t tls_worker1 = proc.defineFunction("hvm_test_tls1", tlsWorker(1234, th1_val));
        const uint32_t tls_worker2 = proc.defineFunction("hvm_test_tls2", tlsWorker(5678, th2_val));
        const uint32_t tls_th1 = proc.startThread(tls_worker1, 0, 64u << 10, false);
        const uint32_t tls_th2 = proc.startThread(tls_worker2, 0, 64u << 10, false);
        proc.joinThread(tls_th1, nullptr);
        proc.joinThread(tls_th2, nullptr);
        const uint32_t mainTls = uint32_t(proc.callGuest(cpu, *proc.lookupSymbol("_pthread_getspecific"), {tls_test_key}));

        bool tls_isolation_ok = th1_val.load() == 1234 && th2_val.load() == 5678 && tls_set.load() == 2 && mainTls == 0;

        // Ensure all detached background threads finish before tearing down the process
        proc.joinThread(worker_th, nullptr);

        check(tls_ok && ts_ok && mtx_ok && chrono_ok && sleep_ok && detach_ok && detach_inv_ok && join_ok && join_inv_ok && term_called && cv_ok && tls_isolation_ok, "libc++ (std::__1) threading and concurrency");
    }

    // Host-requested shutdown must not become an HLE error. An unrelated
    // exception still remains a failure even if stop was requested concurrently.
    for (bool intentional : {true, false}) {
        Process stoppingProc(cfg);
        auto& stoppingCpu = stoppingProc.mainThread().cpu;
        const uint32_t fn = stoppingProc.defineFunction("test_stop", [intentional](Ctx& x) {
            x.proc.requestStop();
            if (intentional) throw ProcessExit(0, "stopping");
            throw std::runtime_error("test failure during shutdown");
        });
        CPU::Exit exit = CPU::Exit::None;
        try { stoppingProc.callGuest(stoppingCpu, fn, {}); }
        catch (const CPU::GuestFault& fault) { exit = fault.exit; }
        check(exit == (intentional ? CPU::Exit::Halt : CPU::Exit::HostError), intentional ?
              "intentional ProcessExit propagates Halt through HLE calls" :
              "unexpected errors during shutdown remain HostError failures");
    }
    // Frame-time distribution. These checks target FrameHistogram directly so a
    // long run cannot grow the sample store.
    {
        FrameHistogram empty;
        check(empty.count() == 0 && empty.percentile(50) == 0 && empty.percentile(95) == 0 && empty.percentile(99) == 0,
              "frame histogram percentiles are 0 with no frames");

        constexpr uint64_t one = 8'000'000;
        FrameHistogram single;
        single.add(one);
        const int bucket = FrameHistogram::bucketIndex(one);
        const uint64_t lower = FrameHistogram::bucketLower(bucket);
        const uint64_t upper = FrameHistogram::bucketUpper(bucket);
        check(single.count() == 1 && lower <= one && one < upper &&
              single.percentile(50) == lower && single.percentile(95) == lower && single.percentile(99) == lower,
              "one frame: p50, p95, and p99 share that frame's histogram bucket");

        FrameHistogram outlier;
        outlier.add(~uint64_t{0});
        const uint64_t topLower = FrameHistogram::bucketLower(FrameHistogram::kTopIndex);
        const uint64_t belowTop = topLower - 1;
        check(outlier.count() == 1 &&
              FrameHistogram::bucketIndex(~uint64_t{0}) == FrameHistogram::kTopIndex &&
              FrameHistogram::bucketIndex(FrameHistogram::kClampNs) == FrameHistogram::kTopIndex &&
              FrameHistogram::bucketIndex(belowTop) != FrameHistogram::kTopIndex &&
              outlier.percentile(50) == topLower && outlier.percentile(95) == topLower && outlier.percentile(99) == topLower,
              "frame times past the top bucket clamp into that bucket");

        FrameHistogram mixed;
        for (int i = 0; i < 100; ++i) mixed.add(one);
        mixed.add(~uint64_t{0});
        mixed.add(~uint64_t{0});
        check(mixed.count() == 102 && mixed.percentile(50) == lower && mixed.percentile(95) == lower &&
              mixed.percentile(99) == topLower,
              "p50 and p95 stay on the common bucket when two samples sit past the top bucket");
    }

    {
        const char* zero =
            "    \"wallSeconds\": 0,\n"
            "    \"guestInstructions\": 0,\n"
            "    \"guestMIPS\": 0,\n"
            "    \"blocksDecoded\": 0,\n"
            "    \"frames\": 0,\n"
            "    \"framesPresented\": 0,\n"
            "    \"avgFrameMs\": 0,\n"
            "    \"avgCompositeMs\": 0,\n"
            "    \"presentWaitMs\": 0,\n"
            "    \"presentCalls\": 0,\n"
            "    \"presentCallMs\": 0,\n"
            "    \"maxFrameMs\": 0,\n"
            "    \"slowFrames\": 0,\n"
            "    \"p50FrameMs\": 0,\n"
            "    \"p95FrameMs\": 0,\n"
            "    \"p99FrameMs\": 0,\n"
            "    \"timeToFirstFrameMs\": 0\n";
        check(perfJson(Process::Perf{}, 0, 0, 0) == zero,
              "perf report with no frames keeps legacy fields and adds percentiles and time-to-first-frame");

        Process::Perf perf;
        perf.recordFrame(16'700'000); // exactly 16.7 ms is not a slow frame
        perf.recordFrame(20'000'000);
        perf.compositeNs.store(6'000'000);
        perf.notePresentedAt(2'500'000);
        perf.notePresentedAt(9'000'000);
        check(perf.frames.load() == 2 && perf.frameNs.load() == 36'700'000 &&
              perf.maxFrameNs.load() == 20'000'000 && perf.slowFrames.load() == 1 &&
              perf.framesPresented.load() == 2 && perf.timeToFirstFrameNs.load() == 2'500'000,
              "frame counters keep sum, max, and the 16.7 ms slow threshold; the first present wins");

        const uint64_t p50 = perf.frameTimes.percentile(50);
        const uint64_t p95 = perf.frameTimes.percentile(95);
        const uint64_t p99 = perf.frameTimes.percentile(99);
        const uint64_t slowBucket = FrameHistogram::bucketLower(FrameHistogram::bucketIndex(16'700'000));
        const uint64_t longBucket = FrameHistogram::bucketLower(FrameHistogram::bucketIndex(20'000'000));
        check(p50 == slowBucket && p95 == longBucket && p99 == longBucket,
              "two-frame percentiles come from the histogram, not from max or the average");

        std::ostringstream avg, composite, p50Text, p95Text, p99Text;
        avg << (36'700'000.0 / 2.0 / 1e6);
        composite << (6'000'000.0 / 2.0 / 1e6);
        p50Text << (double(p50) / 1e6);
        p95Text << (double(p95) / 1e6);
        p99Text << (double(p99) / 1e6);
        const std::string json = perfJson(perf, 2.0, 4'000'000, 11);
        auto has = [&](const std::string& s) { return json.find(s) != std::string::npos; };
        const auto avgAt = json.find("\"avgFrameMs\":");
        const auto slowAt = json.find("\"slowFrames\":");
        const auto p50At = json.find("\"p50FrameMs\":");
        const auto ttffAt = json.find("\"timeToFirstFrameMs\":");
        check(has("\"wallSeconds\": 2,") && has("\"guestInstructions\": 4000000,") &&
              has("\"guestMIPS\": 2,") && has("\"blocksDecoded\": 11,") &&
              has("\"frames\": 2,") && has("\"framesPresented\": 2,") &&
              has("\"avgFrameMs\": " + avg.str() + ",") &&
              has("\"avgCompositeMs\": " + composite.str() + ",") &&
              has("\"maxFrameMs\": 20,") && has("\"slowFrames\": 1,") &&
              has("\"p50FrameMs\": " + p50Text.str() + ",") &&
              has("\"p95FrameMs\": " + p95Text.str() + ",") &&
              has("\"p99FrameMs\": " + p99Text.str() + ",") &&
              has("\"timeToFirstFrameMs\": 2.5\n") &&
              avgAt != std::string::npos && slowAt != std::string::npos &&
              p50At != std::string::npos && ttffAt != std::string::npos &&
              avgAt < slowAt && slowAt < p50At && p50At < ttffAt,
              "perf JSON adds percentile and time-to-first-frame fields without changing legacy ones");
    }

    r.poolPop(cpu, outerPool);
    {
        ProcessConfig stopCfg;
        stopCfg.homePath = "/tmp/hvm_terminate_callbacks";
        Process stopped(stopCfg);
        auto& loop = Foundation::mainRunLoop(stopped);
        int queued = 0, timers = 0, frames = 0;
        bool saved = false;
        loop.post([&](CPU::Cpu&) { saved = true; stopped.requestStop(); });
        loop.post([&](CPU::Cpu&) { ++queued; });
        loop.addTimer(0, 0, false, [&](CPU::Cpu&) { ++timers; });
        loop.addFrameHook([&](CPU::Cpu&, double) { ++frames; });
        loop.runOnce(stopped.mainThread().cpu, 0);
        // Work has already been collected before the first callback stops the process.
        loop.post([&](CPU::Cpu&) { ++queued; });
        loop.runOnce(stopped.mainThread().cpu, 0);
        check(saved && queued == 0 && timers == 0 && frames == 0,
              "Termination finishes save work and suppresses collected/late queue, timer and frame callbacks");
    }

    // Path mapping tests (Issue #80: short iOS guest paths and boundaries)
    {
        std::string longHome = "/tmp/hydrogenvm_test_long_home_directory_path_1234567890_1234567890_1234567890_1234567890_1234567890_1234567890_1234567890_1234567890";
        std::string longBundle = longHome + "/TestApp.app";
        ProcessConfig pathCfg;
        pathCfg.homePath = longHome;
        pathCfg.bundlePath = longBundle;
        Process pathProc(pathCfg);

        // 1. Check guest home path format (/var/mobile/Applications/<UUID>)
        std::string gHome = pathProc.guestHomePath();
        std::string gBundle = pathProc.guestBundlePath();
        bool validGHome = gHome.rfind("/var/mobile/Applications/", 0) == 0 && gHome.size() == 61;
        bool validGBundle = gBundle == gHome + "/TestApp.app";

        // 2. Test hostPath and guestPath mapping
        std::string hDoc = pathProc.hostPath(gHome + "/Documents/save.dat");
        std::string hBundleRes = pathProc.hostPath(gBundle + "/data/level1.bin");
        std::string hTmp = pathProc.hostPath("/tmp/scratch.tmp");
        std::string hTilde = pathProc.hostPath("~/Library/Preferences");

        bool hostMapOk = (hDoc == longHome + "/Documents/save.dat") &&
                          (hBundleRes == longBundle + "/data/level1.bin") &&
                          (hTmp == longHome + "/tmp/scratch.tmp") &&
                          (hTilde == longHome + "/Library/Preferences");

        // 3. Round-trip testing (300-character guest path round-trip)
        std::string longSubpath = "/Documents/";
        while (longSubpath.size() < 250) longSubpath += "sub_dir_123456789/";
        longSubpath += "file.txt";
        std::string g300 = gHome + longSubpath; // 24 + 36 + ~250 = ~310 chars
        std::string h300 = pathProc.hostPath(g300);
        std::string back300 = pathProc.guestPath(h300);

        bool roundTripOk = (g300.size() >= 300) && (back300 == g300);

        // 4. Double-mapping prevention
        std::string doubleHost = pathProc.hostPath(hDoc);
        std::string doubleGuest = pathProc.guestPath(gHome + "/Documents/save.dat");
        bool noDoubleMap = (doubleHost == hDoc) && (doubleGuest == gHome + "/Documents/save.dat");

        // 5. Virtual guest paths map bidirectionally to host paths
        bool envOk = (pathProc.hostPath(gHome) == longHome) &&
                     (pathProc.guestPath(longHome) == gHome);

        check(validGHome && validGBundle && hostMapOk && roundTripOk && noDoubleMap && envOk,
              "Process::hostPath and guestPath perform 300-char round trips with virtual short paths and no double mapping");
    }

    // The iPhone app passes directory URL paths: trailing '/', and /private/var
    // spellings mixed with /var. Joins must keep their '/' (device regression:
    // ".../<UUID>AngryBirds.app/data/...").
    {
        std::error_code ec;
        const std::string root = "/tmp/hvm_paths_slash/private/var/mobile/Containers/X/Documents/Apps/com.test.app";
        std::filesystem::create_directories(root + "/Game.app", ec);
        ProcessConfig c;
        c.homePath = root + "/";
        c.bundlePath = root + "/Game.app/";
        Process pp(c);
        const std::string gh = pp.guestHomePath(), gb = pp.guestBundlePath();
        bool ok = gb == gh + "/Game.app" && pp.bundlePath() == root + "/Game.app" && pp.homePath() == root;
        ok &= pp.guestPath(root + "/Game.app/data/a.png") == gb + "/data/a.png";
        ok &= pp.guestPath(root + "/Documents/s.dat") == gh + "/Documents/s.dat";
        ok &= pp.hostPath(gb + "/data/a.png") == root + "/Game.app/data/a.png";
        ok &= pp.hostPath(gh + "/Documents/s.dat") == root + "/Documents/s.dat";
        ok &= pp.hostPath("/private" + gh + "/Documents/s.dat") == root + "/Documents/s.dat";
        if (!ok) std::printf("    gh=%s gb=%s g(res)=%s\n", gh.c_str(), gb.c_str(), pp.guestPath(root + "/Game.app/data/a.png").c_str());
        check(ok, "Path mapping keeps '/' when the app passes paths with a trailing slash, and accepts /private/var spellings");
    }

    // Regressions: pthread_from_mach_thread_np, mmap MAP_FIXED, and formatGuest with flags after width
    {
        ProcessConfig cfg;
        const std::string home = "/tmp/hvm_test_fixes_" + std::to_string(getpid());
        std::error_code ec;
        std::filesystem::create_directories(home + "/Test.app", ec);
        cfg.homePath = home;
        cfg.bundlePath = home + "/Test.app";
        Process p(cfg);
        auto& cpu = p.mainThread().cpu;
        p.mem().map(0xbee00000, 1u << 20, Memory::kProtRW);
        cpu.r[CPU::kSP] = 0xbef00000 - 256;

        auto call = [&](const char* name, std::initializer_list<uint32_t> args) {
            auto sym = p.lookupSymbol(name);
            return sym ? uint32_t(p.callGuest(cpu, *sym, args)) : 0u;
        };

        // 1. _pthread_from_mach_thread_np
        const uint32_t thSelf = p.currentThread().self;
        const uint32_t machPort = p.currentThread().id + 0x1000;
        const uint32_t mappedTh = call("_pthread_from_mach_thread_np", {machPort});
        check(mappedTh == thSelf, "_pthread_from_mach_thread_np maps mach port to pthread_t");

        // 2. mmap with MAP_FIXED
        const uint32_t fixedAddr = 0x25000000;
        const uint32_t mapped = call("_mmap", {fixedAddr, 0x4000, 3, 0x1012, 0xFFFFFFFFu, 0, 0});
        check(mapped == fixedAddr, "mmap with MAP_FIXED returns requested guest address");

        // 3. formatGuest with flags after width (e.g. %10#llx)
        const uint64_t v64 = 0x1000;
        cpu.r[0] = uint32_t(v64);
        cpu.r[1] = uint32_t(v64 >> 32);
        Runtime::ArgReader a(cpu, p.mem(), 0);
        std::string s = Runtime::formatGuest(p, "%10#llx", a);
        check(s.find("0x1000") != std::string::npos && s.size() >= 10,
              "formatGuest correctly parses format specifiers with flags after width (%10#llx)");

        std::filesystem::remove_all(home, ec);
    }

    // Asset I/O uses its own Process, guest stack, and temporary bundle. Expected
    // bytes and offsets below come directly from the ten-byte host fixture.
    {
        namespace fs = std::filesystem;
        char temp[] = "/tmp/hvm_asset_io_XXXXXX";
        const char* created = ::mkdtemp(temp);
        if (!created) throw std::runtime_error("mkdtemp for asset I/O tests failed");
        const std::string home = created, bundle = home + "/Game.app";
        fs::create_directories(bundle + "/CookedIPhone");
        fs::create_directories(bundle + "/fr.lproj");
        fs::create_directories(bundle + "/Base.lproj");
        fs::create_directories(bundle + "/Other");
        fs::create_directories(bundle + "/CookedIPhoneBackup");
        fs::create_directories(home + "/Game.app-sibling");
        auto put = [](const std::string& path, const std::string& bytes = "0123456789") {
            std::ofstream out(path, std::ios::binary); out << bytes;
            if (!out) throw std::runtime_error("write asset I/O fixture failed");
        };
        put(bundle + "/CookedIPhone/Texture.tfc");
        put(bundle + "/CookedIPhone/Map.xxx");
        put(bundle + "/Other/Texture.tfc");
        put(bundle + "/CookedIPhoneBackup/Texture.tfc");
        put(bundle + "/CookedIPhone/Texture.dat");
        put(bundle + "/fr.lproj/Word.txt");
        put(bundle + "/Base.lproj/Word.txt");
        put(home + "/Game.app-sibling/secret.tfc");
        fs::create_directory_symlink(home + "/Game.app-sibling", bundle + "/Escape");
        fs::create_directory_symlink(bundle + "/Other", bundle + "/Inside");
        {
            ProcessConfig assetCfg;
            assetCfg.homePath = home; assetCfg.bundlePath = bundle;
            Process ap(assetCfg);
            auto& acpu = ap.mainThread().cpu;
            ap.mem().map(0xbee00000, 1u << 20, Memory::kProtRW);
            acpu.r[CPU::kSP] = 0xbef00000 - 256;
            // A Process without a loaded Mach-O needs explicit guest errno
            // storage, just like the existing libc fixtures in this suite.
            ap.mainThread().errnoAddr = ap.heap().allocZeroed(4);
            Ctx ax{ap, acpu, ap.mem()};
            auto& ar = ObjC::rt(ap);
            const auto pool = ar.poolPush();
            auto call = [&](const char* symbol, std::initializer_list<uint32_t> args) {
                auto address = ap.lookupSymbol(symbol);
                if (!address) throw std::runtime_error(std::string("missing test API: ") + symbol);
                return uint32_t(ap.callGuest(acpu, *address, args));
            };
            auto lookup = [&](const std::string& name, const std::string& type = "tfc", const std::string& dir = "CookedIPhone") {
                return Foundation::bundleResource(ax, name, type, dir);
            };
            // The class directory API used to bypass main-bundle publication.
            // Race that cold entry point against the native main-bundle helper.
            const auto coldName = Foundation::makeString(ax, "Texture");
            const auto coldType = Foundation::makeString(ax, "tfc");
            const auto coldDir = Foundation::makeString(ax, bundle + "/CookedIPhone");
            ap.mem().map(0xbec00000, 0x20000, Memory::kProtRW);
            std::atomic<unsigned> ready{0};
            std::atomic<bool> start{false}, coldOK{true};
            auto coldLookup = [&](bool viaClass) {
                CPU::Cpu coldCPU;
                coldCPU.r[CPU::kSP] = (viaClass ? 0xbec10000 : 0xbec20000) - 256;
                Ctx coldCtx{ap, coldCPU, ap.mem()};
                const auto coldPool = ar.poolPush();
                ++ready;
                while (!start) std::this_thread::yield();
                std::string result;
                if (viaClass) {
                    const auto value = uint32_t(ar.send(coldCPU, ar.getClass("NSBundle"),
                        "pathForResource:ofType:inDirectory:", {coldName, coldType, coldDir}));
                    result = ap.hostPath(Foundation::utf8(coldCtx, value));
                } else result = Foundation::bundleResource(coldCtx, "Texture", "tfc", "CookedIPhone");
                if (result != bundle + "/CookedIPhone/Texture.tfc") coldOK = false;
                ar.poolPop(coldCPU, coldPool);
            };
            std::thread coldClass(coldLookup, true), coldNative(coldLookup, false);
            while (ready != 2) std::this_thread::yield();
            start = true;
            coldClass.join(); coldNative.join();
            check(coldOK, "cold class and native bundle lookups synchronize main-bundle publication");
            {
                // iOS gives every thread its own autorelease pool stack. Two host
                // threads that are not guest threads must not share the main
                // thread's ThreadState. Main still has `pool` open (one mark), so
                // a shared stack would hand these threads tokens 2 and 3. Their
                // own fresh stacks each start at token 1. Turns are sequenced so
                // the check itself never touches one vector from two threads.
                ObjC::ThreadState* const mainState = &ObjC::threadState(ap);
                std::atomic<int> turn{0};
                ObjC::ThreadState* seen[2] = {nullptr, nullptr};
                uint32_t tokens[2] = {0, 0};
                auto probe = [&](int k) {
                    CPU::Cpu probeCPU;
                    while (turn != k) std::this_thread::yield();
                    seen[k] = &ObjC::threadState(ap);
                    tokens[k] = ar.poolPush();
                    ++turn;
                    while (turn != 3 - k) std::this_thread::yield();   // thread 1 pops first (turn 2), then thread 0 (turn 3)
                    ar.poolPop(probeCPU, tokens[k]);
                    ++turn;
                };
                std::thread first(probe, 0), second(probe, 1);
                first.join(); second.join();
                check(seen[0] && seen[1] && seen[0] != seen[1] && seen[0] != mainState && seen[1] != mainState &&
                      tokens[0] == 1 && tokens[1] == 1 && &ObjC::threadState(ap) == mainState,
                      "host threads that are not guest threads get their own autorelease pool stacks");
            }
            check(lookup("Texture") == bundle + "/CookedIPhone/Texture.tfc" &&
                  lookup("Texture.tfc", "") == lookup("Texture") &&
                  lookup("Texture", ".tfc") == lookup("Texture") &&
                  lookup("Texture", "tfc", "Other") == bundle + "/Other/Texture.tfc" &&
                  lookup("Map", "xxx") == bundle + "/CookedIPhone/Map.xxx",
                  "bundle resource cache distinguishes names, extensions and subdirectories");
            const auto main = uint32_t(call("_CFBundleGetMainBundle", {}));
            auto localized = [&](const char* lang) {
                const auto path = uint32_t(ar.send(acpu, main, "pathForResource:ofType:inDirectory:forLocalization:",
                    {Foundation::makeString(ax, "Word"), Foundation::makeString(ax, "txt"), 0, Foundation::makeString(ax, lang)}));
                return ap.hostPath(Foundation::utf8(ax, path));
            };
            check(localized("fr") == bundle + "/fr.lproj/Word.txt" &&
                  localized("Base") == bundle + "/Base.lproj/Word.txt" && localized("fr") == bundle + "/fr.lproj/Word.txt",
                  "bundle resource cache separates explicit localizations");
            check(lookup("secret", "tfc", "../Game.app-sibling").empty() &&
                  lookup("secret", "tfc", home + "/Game.app-sibling").empty() &&
                  lookup("secret", "tfc", "Escape").empty() &&
                  lookup("../Game.app-sibling/secret", "tfc", "").empty() &&
                  lookup(home + "/Game.app-sibling/secret", "tfc", "").empty() &&
                  lookup("Texture", "tfc", bundle + "/CookedIPhone") == lookup("Texture"),
                  "bundle lookup rejects traversal, absolute and symlink escapes with component boundaries");
            check(lookup("Texture", "tfc", "Inside") == bundle + "/Inside/Texture.tfc",
                  "bundle lookup still permits symlinks targeting resources inside the bundle");
            // Missing resources must become visible, and a cached fallback must
            // yield to a newly installed device-specific variant.
            bool mutations = lookup("New").empty();
            put(bundle + "/CookedIPhone/New.tfc");
            mutations &= lookup("New") == bundle + "/CookedIPhone/New.tfc";
            put(bundle + "/CookedIPhone/Texture~iphone.tfc");
            mutations &= lookup("Texture") == bundle + "/CookedIPhone/Texture~iphone.tfc";
            fs::remove(bundle + "/CookedIPhone/Texture~iphone.tfc");
            mutations &= lookup("Texture") == bundle + "/CookedIPhone/Texture.tfc";
            put(bundle + "/CookedIPhone/Link.tfc");
            mutations &= !lookup("Link").empty();
            fs::remove(bundle + "/CookedIPhone/Link.tfc");
            fs::create_symlink(home + "/Game.app-sibling/secret.tfc", bundle + "/CookedIPhone/Link.tfc");
            mutations &= lookup("Link").empty();
            check(mutations, "bundle cache invalidates additions/removals and rejects a cached file replaced by an escaping symlink");
            // A directory's mtime can repeat within one clock tick, so a device
            // variant added right after the plain resource was cached must still
            // win even though the stamp the cache recorded did not change.
            // Restoring that stamp keeps this independent of host timestamp
            // granularity instead of a race that only fails on a coarse clock.
            const std::string dirPath = bundle + "/CookedIPhone";
            const std::string stamped = dirPath + "/Stamped.tfc";
            put(stamped);
            const auto beforeAdd = fs::last_write_time(dirPath);
            const bool cachedPlain = lookup("Stamped") == stamped;
            put(dirPath + "/Stamped~iphone.tfc");
            fs::last_write_time(dirPath, beforeAdd);
            check(cachedPlain && lookup("Stamped") == dirPath + "/Stamped~iphone.tfc",
                  "bundle cache prefers a device variant added under an unchanged directory timestamp");
            const std::string priorityDir = bundle + "/Priority";
            fs::create_directories(priorityDir + "/Base.lproj");
            const std::string earlier = priorityDir + "/pRiOrItY.tfc";
            put(earlier);
            put(priorityDir + "/Base.lproj/Priority.tfc");
            // This fixture requires a case-sensitive filesystem. Removing read
            // permission exposes an unexpected rescan while retaining search
            // permission for cached-path validation.
            if (!fs::exists(priorityDir + "/Priority.tfc")) {
                bool priority = lookup("Priority", "tfc", "Priority") == earlier &&
                                lookup("Priority", "tfc", "Priority/") == earlier;
                const auto permissions = fs::status(priorityDir).permissions();
                fs::permissions(priorityDir, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec);
                priority &= lookup("Priority", "tfc", "Priority") == earlier;
                priority &= lookup("Priority", "tfc", "Priority") == earlier;
                priority &= lookup("Priority", "tfc", "Priority/") == earlier;
                fs::permissions(priorityDir, permissions);
                check(priority, "bundle cache retains an earlier case-insensitive match over a later exact match");
                const auto priorityStamp = fs::last_write_time(priorityDir);
                put(priorityDir + "/Priority~iphone.tfc");
                fs::last_write_time(priorityDir, priorityStamp);
                check(lookup("Priority", "tfc", "Priority") == priorityDir + "/Priority~iphone.tfc",
                      "bundle cache still prefers a new exact device variant in the cached directory");
            }
            // A later exact match must not evict an earlier case-insensitive hit.
            // Add a case-insensitive variant with the same timestamp so a
            // re-scan would visibly select it instead of the cached path.
            fs::create_directories(dirPath + "/Base.lproj");
            const std::string remoteEarlier = dirPath + "/priority.tfc";
            put(remoteEarlier);
            put(dirPath + "/Base.lproj/Priority.tfc");
            if (!fs::exists(dirPath + "/Priority.tfc")) { // case-sensitive filesystem
                const auto priorityStamp = fs::last_write_time(dirPath);
                bool priorityOK = lookup("Priority") == remoteEarlier;
                priorityOK &= lookup("Priority") == remoteEarlier;
                put(dirPath + "/priority~iphone.tfc");
                fs::last_write_time(dirPath, priorityStamp);
                priorityOK &= lookup("Priority") == remoteEarlier;
                priorityOK &= lookup("Priority") == remoteEarlier;
                check(priorityOK, "later exact resource does not evict an earlier case-insensitive cached match");
                put(dirPath + "/Priority~iphone.tfc");
                fs::last_write_time(dirPath, priorityStamp);
                check(lookup("Priority") == dirPath + "/Priority~iphone.tfc",
                      "cached case-insensitive match yields to an exact device variant in the same directory");
            }
            put(home + "/Mutable.tfc");
            const auto mutableBundle = uint32_t(ar.send(acpu, ar.getClass("NSBundle"), "bundleWithPath:", {Foundation::makeString(ax, home)}));
            auto mutableLookup = [&] {
                return uint32_t(ar.send(acpu, mutableBundle, "pathForResource:ofType:",
                    {Foundation::makeString(ax, "Mutable"), Foundation::makeString(ax, "tfc")}));
            };
            bool mutableOK = mutableLookup() != 0;
            fs::remove(home + "/Mutable.tfc");
            check(mutableOK && !mutableLookup(), "sandbox bundle lookups do not retain stale mutable paths");
            fs::create_directories(home + "/Documents");
            fs::create_directories(home + "/Mutable.app/CookedIPhone");
            fs::create_directories(bundle + "/Nested.app/CookedIPhone");
            for (const auto& root : {home + "/Mutable.app", bundle + "/Nested.app"}) {
                put(root + "/CookedIPhone/Texture.tfc");
                put(root + "/CookedIPhone/Texture~iphone.tfc");
            }
            const auto stamp = fs::last_write_time(bundle + "/CookedIPhone");
            fs::last_write_time(home + "/Mutable.app/CookedIPhone", stamp);
            fs::last_write_time(bundle + "/Nested.app/CookedIPhone", stamp);
            auto bundleAt = [&](const std::string& path) {
                return uint32_t(ar.send(acpu, ar.getClass("NSBundle"), "bundleWithPath:", {Foundation::makeString(ax, path)}));
            };
            auto aliasResource = [&](uint32_t object) {
                const auto value = uint32_t(ar.send(acpu, object, "pathForResource:ofType:inDirectory:",
                    {coldName, coldType, Foundation::makeString(ax, "CookedIPhone")}));
                return value ? ap.hostPath(Foundation::utf8(ax, value)) : std::string();
            };
            const auto outsideAlias = home + "/Documents/Alias.app";
            fs::create_directory_symlink(bundle, outsideAlias);
            const auto mutableAlias = bundleAt(outsideAlias);
            bool aliasOK = aliasResource(mutableAlias) == outsideAlias + "/CookedIPhone/Texture.tfc";
            fs::remove(outsideAlias);
            fs::create_directory_symlink(home + "/Mutable.app", outsideAlias);
            check(aliasOK && aliasResource(mutableAlias) == outsideAlias + "/CookedIPhone/Texture~iphone.tfc",
                  "mutable sandbox aliases stay uncached after retarget with matching directory timestamps");
            const auto insideAlias = bundle + "/Alias.bundle";
            fs::create_directory_symlink(bundle, insideAlias);
            const auto nestedAlias = bundleAt(insideAlias);
            aliasOK = aliasResource(nestedAlias) == insideAlias + "/CookedIPhone/Texture.tfc";
            fs::remove(insideAlias);
            fs::create_directory_symlink(bundle + "/Nested.app", insideAlias);
            check(aliasOK && aliasResource(nestedAlias) == insideAlias + "/CookedIPhone/Texture~iphone.tfc",
                  "cached nested bundle aliases invalidate when their canonical bundle changes");
            fs::remove(insideAlias);
            fs::create_directory_symlink(home + "/Mutable.app", insideAlias);
            check(aliasResource(nestedAlias).empty(),
                  "nested bundle aliases cannot escape the retained installation boundary on cache miss or hit");
            const auto name = Foundation::makeString(ax, "Texture"), ext = Foundation::makeString(ax, "tfc"), dir = Foundation::makeString(ax, "CookedIPhone");
            const auto url1 = uint32_t(call("_CFBundleCopyResourceURL", {main, name, ext, dir}));
            const auto url2 = uint32_t(call("_CFBundleCopyResourceURL", {main, name, ext, dir}));
            check(url1 && url2 && Foundation::urlFilePath(ax, url1) == Foundation::urlFilePath(ax, url2),
                  "separate CFBundle Copy results have equivalent resource paths");
            call("_CFRelease", {url1}); call("_CFRelease", {url2});
            // A directory full of distinct successful lookups exercises eviction.
            for (unsigned i = 0; i < 4100; ++i) put(bundle + "/CookedIPhone/evict" + std::to_string(i) + ".tfc");
            bool bounded = true;
            for (unsigned i = 0; i < 4100; ++i) bounded &= !lookup("evict" + std::to_string(i)).empty();
            check(bounded && lookup("Texture") == bundle + "/CookedIPhone/Texture.tfc", "bundle resource lookup remains correct across the 4096-entry cache bound");
            std::atomic<bool> concurrent{true};
            std::vector<std::thread> workers;
            for (unsigned i = 0; i < 4; ++i) workers.emplace_back([&] {
                CPU::Cpu workerCPU;
                Ctx workerCtx{ap, workerCPU, ap.mem()};
                for (unsigned j = 0; j < 100; ++j)
                    if (Foundation::bundleResource(workerCtx, "texture", "tfc", "CookedIPhone") != bundle + "/CookedIPhone/Texture.tfc") concurrent = false;
            });
            for (auto& worker : workers) worker.join();
            check(concurrent, "bundle resource cache supports simultaneous native lookups and case-insensitive fallback");

            const auto buffer = ap.heap().allocZeroed(256);
            auto openAsset = [&](const std::string& path, uint32_t flags = 0) {
                return int32_t(call("_open", {ap.internCString(path), flags, 0600}));
            };
            auto seek = [&](int fd, uint64_t off, int whence) {
                // Apple's ARMv7 ABI packs 64-bit arguments without padding.
                return int64_t(ap.callGuest(acpu, *ap.lookupSymbol("_lseek"), {uint32_t(fd), uint32_t(off), uint32_t(off >> 32), uint32_t(whence)}));
            };
            const auto errnoAddr = uint32_t(call("___error", {}));
            for (const char* leaf : {"Texture.tfc", "Map.xxx"}) {
                const auto path = ap.guestBundlePath() + "/CookedIPhone/" + leaf;
                ap.mem().write<uint32_t>(errnoAddr, 123);
                int a = openAsset(path), b = openAsset(path);
                bool ok = a >= 0 && b >= 0 && a != b && ap.mem().read<uint32_t>(errnoAddr) == 123;
                ok &= call("_read", {uint32_t(a), buffer, 3}) == 3 && ap.readCString(buffer) == "012";
                ok &= seek(a, 6, SEEK_SET) == 6 && seek(b, 0, SEEK_CUR) == 0;
                ok &= call("_pread", {uint32_t(a), buffer, 2, 4, 0}) == 2;
                ok &= std::string(reinterpret_cast<const char*>(ap.mem().host(buffer)), 2) == "45" && seek(a, 0, SEEK_CUR) == 6;
                ok &= call("_pread", {uint32_t(a), buffer, 1, 0xffffffffu, 0xffffffffu}) == uint32_t(-1) &&
                      ap.mem().read<uint32_t>(errnoAddr) == 22 && seek(a, 0, SEEK_CUR) == 6;
                ok &= call("_fstat", {uint32_t(a), buffer}) == 0 && call("_fcntl", {uint32_t(a), 3}) == 0;
                ok &= call("_close", {uint32_t(a)}) == 0;
                ok &= call("_read", {uint32_t(a), buffer, 1}) == uint32_t(-1) && ap.mem().read<uint32_t>(errnoAddr) == 9;
                ok &= call("_read", {uint32_t(b), buffer, 2}) == 2 && std::string(reinterpret_cast<const char*>(ap.mem().host(buffer)), 2) == "01";
                int c = openAsset(path);
                ok &= c >= 0 && seek(c, 0, SEEK_CUR) == 0 && seek(b, 0, SEEK_CUR) == 2;
                call("_close", {uint32_t(b)}); call("_close", {uint32_t(c)});
                check(ok, leaf[0] == 'T' ? "TFC opens own independent descriptors, offsets, pread, fstat, fcntl and close" : "XXX opens own independent descriptors, offsets, pread, fstat, fcntl and close");
            }
            bool errors = openAsset(ap.guestBundlePath() + "/CookedIPhone/missing.tfc") == -1 && ap.mem().read<uint32_t>(errnoAddr) == 2;
            errors &= call("_read", {0xffffffffu, buffer, 1}) == uint32_t(-1) && ap.mem().read<uint32_t>(errnoAddr) == 9;
            errors &= seek(-1, 0, SEEK_SET) == -1 && ap.mem().read<uint32_t>(errnoAddr) == 9;
            check(errors, "asset open/read/lseek failures retain Darwin ENOENT and EBADF");
            bool ordinary = true;
            for (const char* relative : {"Other/Texture.tfc", "CookedIPhoneBackup/Texture.tfc", "CookedIPhone/Texture.dat"}) {
                const int fd = openAsset(ap.guestBundlePath() + "/" + relative);
                ordinary &= fd >= 0 && call("_read", {uint32_t(fd), buffer, 1}) == 1;
                call("_close", {uint32_t(fd)});
            }
            check(ordinary, "non-UE3 paths and CookedIPhone prefix siblings retain ordinary file behavior");
            int rw = openAsset(ap.guestBundlePath() + "/CookedIPhone/Map.xxx", 2);
            check(rw >= 0 && call("_write", {uint32_t(rw), ap.internCString("AB"), 2}) == 2 && seek(rw, 0, SEEK_SET) == 0 &&
                  call("_read", {uint32_t(rw), buffer, 2}) == 2 && std::string(reinterpret_cast<const char*>(ap.mem().host(buffer)), 2) == "AB",
                  "writable UE3 asset opens retain normal write/read/seek behavior");
            call("_close", {uint32_t(rw)});
            fs::rename(bundle, bundle + ".moved");
            fs::create_directory_symlink(home + "/Game.app-sibling", bundle);
            const bool rootEscape = lookup("secret", "tfc", "").empty() && lookup("Texture").empty();
            fs::remove(bundle);
            fs::rename(bundle + ".moved", bundle);
            check(rootEscape && lookup("Texture") == bundle + "/CookedIPhone/Texture.tfc",
                  "cached installation boundary rejects replacing the entire bundle with an external symlink");
            ar.poolPop(acpu, pool);
        }
        {
            // A host thread that is not a guest thread of either Process keeps a
            // separate autorelease pool stack per Process. Push a pool in A,
            // autorelease an object there, push and pop a pool in B, then pop A.
            // The object starts at retain count 1, is retained once (2) and
            // autoreleased; popping A's pool must release it back to 1. A
            // single per-thread slot replaced on the switch to B would leave
            // A's pop with nothing to drain, and the count would stay 2.
            ProcessConfig cfgA, cfgB;
            cfgA.homePath = home; cfgA.bundlePath = bundle;
            cfgB.homePath = home; cfgB.bundlePath = bundle;
            Process procA(cfgA);
            Process procB(cfgB);
            auto& rA = ObjC::rt(procA);
            auto& rB = ObjC::rt(procB);
            const ObjC::id obj = rA.createInstance(rA.getClass("NSObject"));
            rA.rootRetain(obj);
            const uint32_t before = rA.retainCount(obj);
            uint32_t tokenA = 0, tokenB = 0, afterB = 0, afterA = 0;
            std::thread host([&] {
                CPU::Cpu hostCPU;
                tokenA = rA.poolPush();
                rA.autorelease(hostCPU, obj);
                tokenB = rB.poolPush();
                rB.poolPop(hostCPU, tokenB);
                afterB = rA.retainCount(obj);
                rA.poolPop(hostCPU, tokenA);
                afterA = rA.retainCount(obj);
            });
            host.join();
            if (!(before == 2 && tokenA == 1 && tokenB == 1 && afterB == 2 && afterA == 1))
                std::printf("    per-process pools: before %u tokenA %u tokenB %u afterB %u afterA %u\n", before, tokenA, tokenB, afterB, afterA);
            check(before == 2 && tokenA == 1 && tokenB == 1 && afterB == 2 && afterA == 1,
                  "a host thread keeps a separate autorelease pool stack for each Process it calls into");
        }
        {
            ProcessConfig lateCfg;
            lateCfg.homePath = home; lateCfg.bundlePath = home + "/Late.app";
            Process late(lateCfg);
            auto& lateCPU = late.mainThread().cpu;
            late.mem().map(0xbee00000, 1u << 20, Memory::kProtRW);
            lateCPU.r[CPU::kSP] = 0xbef00000 - 256;
            const auto mainSymbol = *late.lookupSymbol("_CFBundleGetMainBundle");
            const bool absent = uint32_t(late.callGuest(lateCPU, mainSymbol, {})) == 0;
            fs::create_directories(lateCfg.bundlePath);
            check(absent && uint32_t(late.callGuest(lateCPU, mainSymbol, {})) != 0,
                  "thread-safe main bundle initialization retries a not-yet-created directory");
        }
        std::error_code ec;
        fs::remove_all(home, ec);
    }

    const uint32_t arenasBefore = Memory::GuestArena::liveCount();
    {
        Memory::GuestArena first, second;
        check(Memory::GuestArena::liveCount() == arenasBefore + 2,
              "GuestArena diagnostics count simultaneous live reservations");
    }
    check(Memory::GuestArena::liveCount() == arenasBefore,
          "GuestArena diagnostics count reservations released at destruction");

    // Issue #307: a destroyed arena keeps its 4 GB range reserved for the next
    // one, so a batch run cannot trip over a transient reservation failure.
    check(runArenaPoolTests() == 0, "guest arena pool reuses one reservation and clears it between games");

    // T1b/#253: Process destruction returns the 4 GB guest arena reservation
    // so batch runs never hit "failed to reserve".
    {
        for (int i = 0; i < 3; ++i) {
            ProcessConfig cfg;
            cfg.homePath = "/tmp/hvm_arena_live";
            Process proc(cfg);
            check(Memory::GuestArena::liveCount() == arenasBefore + 1,
                  "a live Process holds exactly one arena reservation");
        }
        check(Memory::GuestArena::liveCount() == arenasBefore,
              "arena reservations return to the starting count after Processes are destroyed");
    }

    // T1b/#253: waitForRelease helper behavior
    {
        check(Memory::GuestArena::waitForRelease(0.1, arenasBefore),
              "waitForRelease returns true immediately when no extra arenas are held");

        auto held = std::make_unique<Memory::GuestArena>();
        check(!Memory::GuestArena::waitForRelease(0.1, arenasBefore),
              "waitForRelease returns false on timeout when an arena is held");

        std::thread releaser([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            held.reset();
        });
        check(Memory::GuestArena::waitForRelease(1.0, arenasBefore),
              "waitForRelease returns true when arena is released mid-wait");
        releaser.join();
    }

    // EAGLContext affinity: the software renderer rejects sharing one context
    // between live threads. The host renderer transparently gives the second
    // thread a shared host context, so that successful bind must not be reported
    // to the game as a failure (Clumsy Ninja render-thread regression).
    struct AffinityPlatform final : HostGL::Platform {
        const char* name() const override { return "affinity-test"; }
        void* createContext(int, void*, bool& desktop) override { desktop = false; return new uint8_t(1); }
        void destroyContext(void* p) override { delete static_cast<uint8_t*>(p); }
        bool makeCurrent(void*) override { return true; }
        void* proc(void*, const char*) override { return nullptr; }
    };
    for (bool host : {false, true}) {
        ProcessConfig affinityCfg;
        affinityCfg.homePath = "/tmp/hvm_eagl_affinity";
        if (host) affinityCfg.hostGL = std::make_shared<AffinityPlatform>();
        Process affinity(affinityCfg);
        auto& ar = ObjC::rt(affinity);
        auto& acpu = affinity.mainThread().cpu;
        const ObjC::Class cls = ar.getClass("EAGLContext");
        const ObjC::id context = ObjC::id(ar.send(acpu, ObjC::id(ar.send(acpu, ObjC::id(cls), ar.selAlloc)), "initWithAPI:", {2}));
        const uint64_t mainBound = ar.send(acpu, ObjC::id(cls), "setCurrentContext:", {context});
        const uint32_t workerFn = affinity.defineFunction("test_eagl_affinity", [context](Ctx& x) {
            auto& runtime = ObjC::rt(x);
            const ObjC::Class cls = runtime.getClass("EAGLContext");
            x.cpu.r[0] = uint32_t(runtime.send(x.cpu, ObjC::id(cls), "setCurrentContext:", {context}) & 0xFF);
        });
        const uint32_t thread = affinity.startThread(workerFn, 0, 64u << 10, false);
        uint32_t workerBound = 2;
        const bool joined = affinity.joinThread(thread, &workerBound);
        ar.send(acpu, ObjC::id(cls), "setCurrentContext:", {0});
        check((mainBound & 0xFF) == 1 && joined && workerBound == (host ? 1 : 0), host ?
              "GPU EAGLContext reports a successful shared-context bind on a second thread" :
              "software EAGLContext rejects a context already current on another thread");
    }

    // CFRunLoopSource (version 0): signal -> perform on the next loop iteration, in
    // `order`, once; Remove / Invalidate stop it; version-1 (Mach port) is refused.
    {
        ProcessConfig srcCfg;
        srcCfg.homePath = "/tmp/hvm_runloop_source";
        Process srcProc(srcCfg);
        auto& scpu = srcProc.mainThread().cpu;
        auto call = [&](const char* sym, std::initializer_list<uint32_t> args) { return uint32_t(srcProc.callGuest(scpu, *srcProc.lookupSymbol(sym), args)); };
        auto& loop = Foundation::mainRunLoop(srcProc);
        const uint32_t mainLoop = call("_CFRunLoopGetMain", {});
        std::vector<uint32_t> performed, scheduled, cancelled;
        const uint32_t performFn = srcProc.defineFunction("test_source_perform", [&](Ctx& x) { performed.push_back(x.cpu.r[0]); });
        const uint32_t scheduleFn = srcProc.defineFunction("test_source_schedule", [&](Ctx& x) { scheduled.push_back(x.cpu.r[0]); });
        const uint32_t cancelFn = srcProc.defineFunction("test_source_cancel", [&](Ctx& x) { cancelled.push_back(x.cpu.r[0]); });
        const auto makeSource = [&](int32_t order, uint32_t info, uint32_t version = 0) {
            const uint32_t ctx = srcProc.heap().allocZeroed(40);
            srcProc.mem().write<uint32_t>(ctx, version);
            srcProc.mem().write<uint32_t>(ctx + 4, info);
            srcProc.mem().write<uint32_t>(ctx + 28, scheduleFn);
            srcProc.mem().write<uint32_t>(ctx + 32, cancelFn);
            srcProc.mem().write<uint32_t>(ctx + 36, performFn);
            return call("_CFRunLoopSourceCreate", {0, uint32_t(order), ctx});
        };
        const auto spin = [&] { loop.runOnce(scpu, 0.0); loop.runOnce(scpu, 0.0); };

        const uint32_t a = makeSource(0, 0xA1);
        call("_CFRunLoopAddSource", {mainLoop, a, 0});
        check(scheduled == std::vector<uint32_t>{0xA1} && call("_CFRunLoopContainsSource", {mainLoop, a, 0}) != 0,
              "CFRunLoopAddSource schedules the source on the loop and calls its schedule callback");
        spin();
        check(performed.empty(), "an unsignalled run loop source does not perform");
        call("_CFRunLoopSourceSignal", {a});
        spin();
        check(performed == std::vector<uint32_t>{0xA1}, "a signalled run loop source performs once with its info");
        spin();
        check(performed.size() == 1, "a run loop source performs only once per signal");
        call("_CFRunLoopSourceSignal", {a});
        spin();
        check(performed.size() == 2, "a run loop source can be signalled again");

        // Two sources signalled together run lowest `order` first, whichever was added first.
        performed.clear();
        const uint32_t late = makeSource(5, 0xB5), early = makeSource(1, 0xB1);
        call("_CFRunLoopAddSource", {mainLoop, late, 0});
        call("_CFRunLoopAddSource", {mainLoop, early, 0});
        call("_CFRunLoopSourceSignal", {late});
        call("_CFRunLoopSourceSignal", {early});
        spin();
        check(performed == std::vector<uint32_t>{0xB1, 0xB5}, "signalled run loop sources perform in order, lowest first");

        // Remove stops it, and calls cancel once.
        performed.clear();
        cancelled.clear();
        call("_CFRunLoopRemoveSource", {mainLoop, late, 0});
        call("_CFRunLoopSourceSignal", {late});
        spin();
        check(performed.empty() && cancelled == std::vector<uint32_t>{0xB5} && call("_CFRunLoopContainsSource", {mainLoop, late, 0}) == 0,
              "CFRunLoopRemoveSource stops a source firing, calls cancel, and it is no longer contained");

        // Invalidate stops it for good.
        performed.clear();
        cancelled.clear();
        call("_CFRunLoopSourceInvalidate", {early});
        call("_CFRunLoopSourceSignal", {early});
        spin();
        check(performed.empty() && call("_CFRunLoopSourceIsValid", {early}) == 0 && cancelled == std::vector<uint32_t>{0xB1},
              "CFRunLoopSourceInvalidate stops a source firing, calls cancel, and marks it invalid");

        // Signalled before it is added: performs once it is on a loop.
        performed.clear();
        const uint32_t pre = makeSource(0, 0xC1);
        call("_CFRunLoopSourceSignal", {pre});
        spin();
        check(performed.empty(), "a signalled source that is on no loop does not perform");
        call("_CFRunLoopAddSource", {mainLoop, pre, 0});
        spin();
        check(performed == std::vector<uint32_t>{0xC1}, "a source signalled before being added performs once it is on a loop");

        check(makeSource(0, 0xD1, 1) == 0, "a version-1 (Mach port) run loop source is refused instead of faked");

        // One source in two modes: containment and removal are per (loop, mode); it stays attached
        // (and keeps performing) until its last mode is removed, and cancel runs once per mode.
        performed.clear(); scheduled.clear(); cancelled.clear();
        const uint32_t multi = makeSource(0, 0xE1);
        // Modes are CFStrings compared by value: equal strings at different addresses are the same mode, and the
        // source keeps its own reference, so the caller may release its string right after adding.
        Ctx sx{srcProc, scpu, srcProc.mem()};
        const uint32_t modeA1 = Foundation::makeString(sx, "HVMModeA"), modeA2 = Foundation::makeString(sx, "HVMModeA");
        const uint32_t modeB = Foundation::makeString(sx, "HVMModeB"), modeC = Foundation::makeString(sx, "HVMModeC");
        call("_CFRunLoopAddSource", {mainLoop, multi, modeA1});
        call("_CFRunLoopAddSource", {mainLoop, multi, modeA2});   // same mode again: no second membership
        call("_CFRunLoopAddSource", {mainLoop, multi, modeB});
        call("_CFRelease", {modeB});
        check(scheduled == std::vector<uint32_t>{0xE1, 0xE1} &&
              call("_CFRunLoopContainsSource", {mainLoop, multi, modeA2}) != 0 && call("_CFRunLoopContainsSource", {mainLoop, multi, modeB}) != 0 &&
              call("_CFRunLoopContainsSource", {mainLoop, multi, modeC}) == 0,
              "a run loop source can be added in two modes, modes compare by string value, and containment is per mode");
        call("_CFRunLoopRemoveSource", {mainLoop, multi, modeA2});   // removes the membership added with modeA1
        call("_CFRunLoopSourceSignal", {multi});
        spin();
        check(cancelled.size() == 1 && performed == std::vector<uint32_t>{0xE1} &&
              call("_CFRunLoopContainsSource", {mainLoop, multi, modeA1}) == 0 && call("_CFRunLoopContainsSource", {mainLoop, multi, modeB}) != 0,
              "removing one mode leaves the source attached and performing in the other");
        const uint32_t modeB2 = Foundation::makeString(sx, "HVMModeB");
        call("_CFRunLoopRemoveSource", {mainLoop, multi, modeB2});
        performed.clear();
        call("_CFRunLoopSourceSignal", {multi});
        spin();
        check(cancelled.size() == 2 && performed.empty(), "removing the last mode detaches the source and cancels once per mode");
    }

    // Run loop modes: work belongs to the mode it was added in, a timer added to two modes is
    // still one timer, and a run in kCFRunLoopCommonModes runs the default mode.
    {
        ProcessConfig modeCfg;
        modeCfg.homePath = "/tmp/hvm_runloop_modes";
        Process modeProc(modeCfg);
        auto& mcpu = modeProc.mainThread().cpu;
        // A Process with no loaded image starts with no stack; calls that pass arguments on the
        // stack (_CFRunLoopTimerCreate, _CFRunLoopRunInMode) need one.
        const uint32_t stack = modeProc.heap().allocZeroed(8u << 10);
        mcpu.r[CPU::kSP] = stack + (8u << 10) - 64;
        auto call = [&](const char* sym, std::initializer_list<uint32_t> args) { return uint32_t(modeProc.callGuest(mcpu, *modeProc.lookupSymbol(sym), args)); };
        auto callv = [&](const char* sym, const std::vector<uint32_t>& args) { return uint32_t(modeProc.callGuest(mcpu, *modeProc.lookupSymbol(sym), args)); };
        Ctx mx{modeProc, mcpu, modeProc.mem()};
        const auto words = [](double d) { std::vector<uint32_t> w(2); std::memcpy(w.data(), &d, 8); return w; };
        const auto exportMode = [&](const char* sym) { return modeProc.mem().read<uint32_t>(*modeProc.lookupSymbol(sym)); };
        const uint32_t mainLoop = call("_CFRunLoopGetMain", {});
        const uint32_t defaultMode = exportMode("_kCFRunLoopDefaultMode");
        const uint32_t commonModes = exportMode("_kCFRunLoopCommonModes");
        const uint32_t modeA = Foundation::makeString(mx, "HVMModeA");
        const uint32_t modeB = Foundation::makeString(mx, "HVMModeB");
        const uint32_t modeC = Foundation::makeString(mx, "HVMModeC");
        const auto runInMode = [&](uint32_t mode) {
            std::vector<uint32_t> args{mode};   // _CFRunLoopRunInMode(mode, seconds, returnAfterSource)
            const auto seconds = words(0.0);
            args.insert(args.end(), seconds.begin(), seconds.end());
            args.push_back(0);
            callv("_CFRunLoopRunInMode", args);
        };
        std::vector<uint32_t> performed, fired;
        const uint32_t performFn = modeProc.defineFunction("test_mode_source_perform", [&](Ctx& x) { performed.push_back(x.cpu.r[0]); });
        const uint32_t fireFn = modeProc.defineFunction("test_mode_timer_fire", [&](Ctx& x) { fired.push_back(x.cpu.r[1]); });
        const auto makeSource = [&](uint32_t info) {
            const uint32_t ctx = modeProc.heap().allocZeroed(40);
            modeProc.mem().write<uint32_t>(ctx + 4, info);        // CFRunLoopSourceContext.info
            modeProc.mem().write<uint32_t>(ctx + 36, performFn);  // CFRunLoopSourceContext.perform
            return call("_CFRunLoopSourceCreate", {0, 0, ctx});
        };
        const auto makeTimer = [&](uint32_t info) {
            const uint32_t ctx = modeProc.heap().allocZeroed(20);
            modeProc.mem().write<uint32_t>(ctx + 4, info);        // CFRunLoopTimerContext.info
            std::vector<uint32_t> args{0};                        // _CFRunLoopTimerCreate(alloc, fireDate, interval, flags, order, callout, ctx)
            for (double d : {Foundation::now() - 0.001, 0.0}) {   // already due, one shot
                const auto w = words(d);
                args.insert(args.end(), w.begin(), w.end());
            }
            args.push_back(0);
            args.push_back(0);
            args.push_back(fireFn);
            args.push_back(ctx);
            return callv("_CFRunLoopTimerCreate", args);
        };

        // A source added in one mode does not perform in another: it waits, still signalled, for
        // a run in its own mode.
        const uint32_t sourceA = makeSource(0xA1);
        call("_CFRunLoopAddSource", {mainLoop, sourceA, modeA});
        call("_CFRunLoopSourceSignal", {sourceA});
        runInMode(modeB);
        check(performed.empty(), "a run loop source added in one mode does not perform in another mode");
        runInMode(modeA);
        check(performed == std::vector<uint32_t>{0xA1}, "a run loop source performs in the mode it was added in");

        // A timer added to two modes is one timer: it fires once, and only in those modes.
        fired.clear();
        const uint32_t bothModes = makeTimer(0xB1);
        call("_CFRunLoopAddTimer", {mainLoop, bothModes, modeA});
        call("_CFRunLoopAddTimer", {mainLoop, bothModes, modeB});
        runInMode(modeC);
        check(fired.empty(), "a timer added to two modes does not fire in a third mode");
        runInMode(modeA);
        runInMode(modeB);
        check(fired == std::vector<uint32_t>{0xB1}, "a timer added to two modes fires once");

        // Running in kCFRunLoopCommonModes runs the default mode: a default-mode timer fires, a
        // timer that belongs to a private mode waits for that mode.
        fired.clear();
        const uint32_t defaultTimer = makeTimer(0xC1);
        const uint32_t privateTimer = makeTimer(0xC2);
        call("_CFRunLoopAddTimer", {mainLoop, defaultTimer, defaultMode});
        call("_CFRunLoopAddTimer", {mainLoop, privateTimer, modeB});
        runInMode(commonModes);
        check(fired == std::vector<uint32_t>{0xC1},
              "_CFRunLoopRunInMode in kCFRunLoopCommonModes fires a default-mode timer and not a private-mode one");

        // An item added to kCFRunLoopCommonModes belongs to the common modes (the default mode is one),
        // not to a private mode.
        fired.clear();
        const uint32_t commonTimer = makeTimer(0xD1);
        call("_CFRunLoopAddTimer", {mainLoop, commonTimer, commonModes});
        runInMode(modeC);
        check(fired.empty(), "a timer added to the common modes does not fire in a private mode");
        runInMode(defaultMode);
        check(fired == std::vector<uint32_t>{0xD1}, "a timer added to the common modes fires in the default mode");

        // -currentMode is nil when the loop is idle and the running mode's own string inside a callout.
        auto& mr = ObjC::rt(modeProc);
        check(uint32_t(mr.send(mcpu, mainLoop, "currentMode")) == 0, "NSRunLoop currentMode is nil while the loop is not running");
        std::string seenMode = "<unset>";
        const uint32_t modeProbe = modeProc.defineFunction("test_mode_probe_fire", [&](Ctx& x) {
            const uint32_t m = uint32_t(ObjC::rt(x).send(x.cpu, mainLoop, "currentMode"));
            seenMode = m ? Foundation::utf8(x, m) : "<nil>";
        });
        {
            const uint32_t ctx = modeProc.heap().allocZeroed(20);
            std::vector<uint32_t> args{0};
            for (double d : {Foundation::now() - 0.001, 0.0}) {
                const auto w = words(d);
                args.insert(args.end(), w.begin(), w.end());
            }
            args.push_back(0);
            args.push_back(0);
            args.push_back(modeProbe);
            args.push_back(ctx);
            const uint32_t probeTimer = callv("_CFRunLoopTimerCreate", args);
            call("_CFRunLoopAddTimer", {mainLoop, probeTimer, modeA});
        }
        runInMode(modeA);
        check(seenMode == "HVMModeA", "NSRunLoop currentMode inside a callout is the running mode's name");

        // -currentMode answers for the receiver's loop, not the calling thread's: a guest thread (which has a run loop
        // of its own, idle) asking the main loop while it runs in modeA sees modeA.
        // The worker owns its state through shared_ptr, so a worker still running after the wait times out cannot touch
        // destroyed locals.
        struct WorkerState { std::atomic<bool> done{false}; std::mutex mu; std::string saw = "<unset>"; };
        const auto workerState = std::make_shared<WorkerState>();
        auto workerCls = mr.defineClass("HVMModeProbeWorker", "NSObject");
        workerCls.method("work:", [workerState, mainLoop](Ctx& x) {
            ObjC::rt(x).send(x.cpu, ObjC::rt(x).getClass("NSRunLoop"), "currentRunLoop");   // this thread's own loop, not running
            const uint32_t m = uint32_t(ObjC::rt(x).send(x.cpu, mainLoop, "currentMode"));
            { std::lock_guard<std::mutex> lk(workerState->mu); workerState->saw = m ? Foundation::utf8(x, m) : "<nil>"; }
            workerState->done = true;
        }, "v@:@");
        const uint32_t workerTarget = mr.createInstance(workerCls.cls());
        const uint32_t crossProbe = modeProc.defineFunction("test_mode_cross_thread_fire", [workerState, workerTarget](Ctx& x) {
            const uint32_t th = uint32_t(ObjC::rt(x).send(x.cpu, ObjC::id(ObjC::rt(x).send(x.cpu, ObjC::rt(x).getClass("NSThread"), ObjC::rt(x).selAlloc)),
                                                         "initWithTarget:selector:object:", {workerTarget, ObjC::rt(x).sel("work:"), 0}));
            ObjC::rt(x).send(x.cpu, th, "start");
            for (int i = 0; i < 5000 && !workerState->done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        });
        {
            const uint32_t ctx = modeProc.heap().allocZeroed(20);
            std::vector<uint32_t> args{0};
            for (double d : {Foundation::now() - 0.001, 0.0}) {
                const auto w = words(d);
                args.insert(args.end(), w.begin(), w.end());
            }
            args.push_back(0);
            args.push_back(0);
            args.push_back(crossProbe);
            args.push_back(ctx);
            call("_CFRunLoopAddTimer", {mainLoop, callv("_CFRunLoopTimerCreate", args), modeA});
        }
        runInMode(modeA);
        std::string workerSaw;
        { std::lock_guard<std::mutex> lk(workerState->mu); workerSaw = workerState->saw; }
        check(workerState->done && workerSaw == "HVMModeA", "NSRunLoop currentMode answers for the receiver's loop, not the calling thread's");

        // performSelector:withObject:afterDelay:inModes: runs only in the modes it names.
        int delayedRuns = 0;
        auto delayedCls = mr.defineClass("HVMDelayedModeTarget", "NSObject");
        delayedCls.method("go:", [&](Ctx&) { ++delayedRuns; }, "v@:@");
        const uint32_t delayedTarget = mr.createInstance(delayedCls.cls());
        {
            const uint32_t modeList = Foundation::makeArray(mx, {modeA});
            const auto w = words(0.0);
            const uint32_t sp = mcpu.r[CPU::kSP];
            mcpu.r[CPU::kSP] = sp - 16;   // soft-float: the delay is 8-aligned on the stack after (sel, arg); modes follows
            modeProc.mem().write<uint32_t>(mcpu.r[CPU::kSP], w[0]);
            modeProc.mem().write<uint32_t>(mcpu.r[CPU::kSP] + 4, w[1]);
            modeProc.mem().write<uint32_t>(mcpu.r[CPU::kSP] + 8, modeList);
            mr.send(mcpu, delayedTarget, "performSelector:withObject:afterDelay:inModes:", {mr.sel("go:"), 0});
            mcpu.r[CPU::kSP] = sp;
        }
        runInMode(modeB);
        check(delayedRuns == 0, "a delayed selector scoped to one mode does not run in another mode");
        runInMode(modeA);
        check(delayedRuns == 1, "a delayed selector scoped to a mode runs in that mode");
    }

    // UIApplication's main loop runs the main run loop in the default mode: a source that belongs
    // to another mode waits for a run in that mode instead of performing on a frame.
    {
        ProcessConfig appCfg;
        appCfg.homePath = "/tmp/hvm_app_loop_mode";
        Process appProc(appCfg);
        auto& acpu = appProc.mainThread().cpu;
        auto call = [&](const char* sym, std::initializer_list<uint32_t> args) { return uint32_t(appProc.callGuest(acpu, *appProc.lookupSymbol(sym), args)); };
        Ctx ax{appProc, acpu, appProc.mem()};
        const uint32_t mainLoop = call("_CFRunLoopGetMain", {});
        const uint32_t privateMode = Foundation::makeString(ax, "HVMModeB");
        std::vector<uint32_t> performed;
        const uint32_t performFn = appProc.defineFunction("test_app_loop_source_perform", [&](Ctx& x) { performed.push_back(x.cpu.r[0]); });
        const auto makeSource = [&](uint32_t info) {
            const uint32_t ctx = appProc.heap().allocZeroed(40);
            appProc.mem().write<uint32_t>(ctx + 4, info);
            appProc.mem().write<uint32_t>(ctx + 36, performFn);
            return call("_CFRunLoopSourceCreate", {0, 0, ctx});
        };
        const uint32_t privateSource = makeSource(0xD1);
        call("_CFRunLoopAddSource", {mainLoop, privateSource, privateMode});
        call("_CFRunLoopSourceSignal", {privateSource});
        UIKit::runApplicationLoop(ax, Foundation::now() + 0.01);
        check(performed.empty(), "UIApplication's main loop does not perform a source added in another mode");
        const uint32_t defaultSource = makeSource(0xD2);
        call("_CFRunLoopAddSource", {mainLoop, defaultSource, 0});
        call("_CFRunLoopSourceSignal", {defaultSource});
        UIKit::runApplicationLoop(ax, Foundation::now() + 0.01);
        check(performed == std::vector<uint32_t>{0xD2}, "UIApplication's main loop performs a default-mode source");
    }

    // runMode:beforeDate: returns NO if the mode is empty and YES if there was at least a timer.
    {
        ProcessConfig modeCfg;
        modeCfg.homePath = "/tmp/hvm_runmode_returns";
        Process modeProc(modeCfg);
        auto& mcpu = modeProc.mainThread().cpu;
        Ctx mx{modeProc, mcpu, modeProc.mem()};
        auto& mr = ObjC::rt(modeProc);
        const uint32_t nsRunLoop = mr.getClass("NSRunLoop");
        const uint32_t currentRunLoop = uint32_t(mr.send(mcpu, nsRunLoop, "currentRunLoop"));
        const uint32_t emptyMode = Foundation::makeString(mx, "HVMEmptyMode");
        const uint32_t timerMode = Foundation::makeString(mx, "HVMTimerMode");

        uint32_t date0 = Foundation::makeDate(mx, Foundation::now() + 0.05);
        uint32_t runModeReturn = uint32_t(mr.send(mcpu, currentRunLoop, "runMode:beforeDate:", {emptyMode, date0}));
        check(runModeReturn == 0, "runMode:beforeDate: returns NO for an empty mode");

        auto call = [&](const char* sym, std::initializer_list<uint32_t> args) { return uint32_t(modeProc.callGuest(mcpu, *modeProc.lookupSymbol(sym), args)); };
        const uint32_t performFn = modeProc.defineFunction("test_app_loop_timer_perform2", [&](Ctx& x) {});
        const auto makeSource = [&](uint32_t info) {
            const uint32_t ctx = modeProc.heap().allocZeroed(40);
            modeProc.mem().write<uint32_t>(ctx + 4, info);
            modeProc.mem().write<uint32_t>(ctx + 36, performFn);
            return call("_CFRunLoopSourceCreate", {0, 0, ctx});
        };
        const uint32_t mainLoop = call("_CFRunLoopGetMain", {});
        const uint32_t source = makeSource(0xD3);
        call("_CFRunLoopAddSource", {mainLoop, source, timerMode});

        uint32_t date1 = Foundation::makeDate(mx, Foundation::now() + 0.01);
        runModeReturn = uint32_t(mr.send(mcpu, currentRunLoop, "runMode:beforeDate:", {timerMode, date1}));
        check(runModeReturn != 0, "runMode:beforeDate: returns YES for a mode with a source");

        uint32_t date2 = Foundation::makeDate(mx, Foundation::now() - 1.0);
        runModeReturn = uint32_t(mr.send(mcpu, currentRunLoop, "runMode:beforeDate:", {timerMode, date2}));
        check(runModeReturn != 0, "runMode:beforeDate: returns YES for a mode with a source but timeout reached");

        call("_CFRunLoopRemoveSource", {mainLoop, source, timerMode});

        // Once the source is gone the private mode has no input again, so it must answer NO at once.
        uint32_t date3 = Foundation::makeDate(mx, Foundation::now() + 0.05);
        runModeReturn = uint32_t(mr.send(mcpu, currentRunLoop, "runMode:beforeDate:", {timerMode, date3}));
        check(runModeReturn == 0, "runMode:beforeDate: returns NO again after the mode's only source is removed");

        // A worker thread's run loop with nothing scheduled has no input (Apple: runMode returns NO at once);
        // posted work counts as input in any mode; the main loop always has the host pump in its default mode.
        {
            Foundation::RunLoop worker(modeProc);
            check(!worker.hasModeInput("s:kCFRunLoopDefaultMode"), "an empty worker run loop has no input in the default mode");
            worker.post([](CPU::Cpu&) {});
            check(worker.hasModeInput("s:HVMTimerMode"), "posted work counts as input for a private mode");
            check(Foundation::mainRunLoop(modeProc).hasModeInput("s:kCFRunLoopDefaultMode"), "the main run loop has input in the default mode");
        }

        // The default mode is always fed by the host event pump, so a game's own run-loop pump keeps waiting (YES).
        const uint32_t defaultMode = Foundation::makeString(mx, "kCFRunLoopDefaultMode");
        uint32_t date4 = Foundation::makeDate(mx, Foundation::now() + 0.01);
        runModeReturn = uint32_t(mr.send(mcpu, currentRunLoop, "runMode:beforeDate:", {defaultMode, date4}));
        check(runModeReturn != 0, "runMode:beforeDate: returns YES for the default mode even with no guest timers");
    }

    {
        ProcessConfig traceCfg = cfg;
        traceCfg.homePath = "/tmp/hvm_symbolication_test";
        traceCfg.log = [](LogLevel, const std::string&) {};
        Process trace(traceCfg);
        auto& methods = ObjC::rt(trace);
        auto alpha = methods.defineClass("HVMTraceAlpha", "NSObject");
        auto beta = methods.defineClass("HVMTraceBeta", "NSObject");
        methods.addMethod(alpha.cls(), methods.sel("first"), 0x00101001u, "v@:");
        methods.addMethod(beta.meta(), methods.sel("second"), 0x00102000u, "v@:");
        CPU::Cpu frames{};
        frames.r[15] = 0x00101001u; // Thumb IMP, stripped of a Mach-O symbol
        frames.r[14] = 0x00102000u; // ARM class-method IMP
        const std::string backtrace = trace.backtrace(frames);
        check(backtrace.find("-[HVMTraceAlpha first]") != std::string::npos &&
              backtrace.find("+[HVMTraceBeta second]") != std::string::npos,
              "ObjC IMP backtrace names both guest instance and class methods without Mach-O symbols");
        auto symbols = std::atomic_load(&trace.image().methodSymbols);
        Loader::Image image;
        image.lowAddr = 0x00100000u; image.highAddr = 0x00110000u;
        image.segments.push_back({"__TEXT", image.lowAddr, 0x10000u, 0, 0, 5, 5});
        image.methodSymbols = symbols;
        check(image.symbolicate(0x00101000u) == "-[HVMTraceAlpha first]" &&
              image.symbolicate(0x00101004u).empty(),
              "ObjC IMP normalizes Thumb and refuses unbounded interior guesses");
        image.functionStarts = {0x00101000u, 0x00101020u, 0x00102000u};
        check(image.symbolicate(0x00101005u) == "-[HVMTraceAlpha first] + 0x4" &&
              image.symbolicate(0x00101024u).empty(),
              "ObjC interior frame names stop at the next function boundary");
        image.symbolsByAddr[0x00101011u] = "_ordinary_c_function";
        check(image.symbolicate(0x00101014u) == "_ordinary_c_function + 0x4",
              "Mach-O symbol boundary prevents ObjC misattribution");
        const auto first = methods.sel("first"), alias = methods.sel("alias");
        methods.addMethod(alpha.cls(), alias, 0x00101001u, "v@:");
        methods.addMethod(alpha.cls(), first, 0x00103000u, "v@:", true);
        check(image.symbolicate(0x00101000u) == "-[HVMTraceAlpha alias]" &&
              image.symbolicate(0x00103000u) == "-[HVMTraceAlpha first]",
              "ObjC replacement removes only its old binding and preserves shared IMP aliases");
        methods.addMethod(alpha.cls(), alias, 0x00103000u, "v@:", true);
        const auto shared = image.symbolicate(0x00103000u);
        check(shared.find("-[HVMTraceAlpha first]") != std::string::npos &&
              shared.find("-[HVMTraceAlpha alias]") != std::string::npos &&
              symbols->describe(0x00101000u).empty(),
              "ObjC shared IMP reports aliases and drops addresses without bindings");
        methods.addMethod(alpha.cls(), alias, 0, "v@:", true);
        methods.addMethod(alpha.cls(), first, 0x00104000u, "v@:", false); // rejected duplicate
        check(image.symbolicate(0x00103000u) == "-[HVMTraceAlpha first]" &&
              symbols->describe(0x00104000u).empty(),
              "ObjC rejected additions and null replacements keep symbol map consistent");
        Loader::Image other;
        check(other.symbolicate(0x00103000u).empty(), "ObjC method map is not global");
        // Minimal MH_DYLIB with one __TEXT and LC_FUNCTION_STARTS. ULEBs
        // 0x101, 0x20 encode Thumb starts relative to the unslid text base.
        auto parseStarts = [](const std::vector<uint8_t>& stream, uint32_t dataOffset = 128) {
            std::vector<uint8_t> file(256);
            auto word = [&](size_t at, uint32_t value) { std::memcpy(file.data() + at, &value, 4); };
            word(0, 0xFEEDFACEu); word(4, 12); word(8, 9); word(12, 6);
            word(16, 2); word(20, 72);
            word(28, 1); word(32, 56); std::memcpy(file.data() + 36, "__TEXT", 6);
            word(52, 0x10000); word(56, 0x1000); word(64, 256);
            word(68, 5); word(72, 5);
            word(84, 0x26); word(88, 16); word(92, dataOffset); word(96, uint32_t(stream.size()));
            std::copy(stream.begin(), stream.end(), file.begin() + 128);
            Memory::GuestArena arena;
            Loader::LoadOptions options; options.loadAddress = 0x20000;
            return Loader::loadMachO(arena, file, "synthetic-starts.dylib", options).functionStarts;
        };
        check(parseStarts({0x81, 0x02, 0x20, 0}) == std::vector<uint32_t>({0x20100, 0x20120}),
              "Mach-O function starts accumulate ULEB deltas before Thumb normalization and slide");
        check(parseStarts({0x81, 0x02, 0x80}).empty() &&
              parseStarts({0x81, 0x02, 0xFF, 0xFF, 0xFF, 0xFF, 0x1F, 0}).empty() &&
              parseStarts({0x81, 0x02, 0x80, 0x20, 0}).empty() &&
              parseStarts({0x81, 0x02, 0}, 255).empty(),
              "Mach-O ignores truncated, overflowing, out-of-text and out-of-file function starts");
        auto host = methods.defineClass("HVMTraceHost", "NSObject");
        host.method("knownHostMethod", [](Ctx&) {});
        frames.r[15] = methods.lookup(host.cls(), methods.sel("knownHostMethod"));
        frames.r[14] = 0;
        check(trace.backtrace(frames).find("-[HVMTraceHost knownHostMethod]") != std::string::npos,
              "ObjC host-method HLE backtrace names are preserved");
    }

    {
        int propFails = runPropertyTests("");
        check(propFails == 0, "glibc property tests for printf/string/memory helpers (T5d)");
    }

    // EAGLSharegroup teardown: the implicit per-thread worker contexts made for a sharegroup are released when
    // it dies, whether their owning thread already exited or is still running (and may still hold the context).
    {
        struct CountingPlatform final : HostGL::Platform {
            std::atomic<int> live{0};
            std::mutex mu;
            std::unordered_map<void*, std::thread::id> owner;
            const char* name() const override { return "sharegroup-test"; }
            void* createContext(int, void*, bool& desktop) override { desktop = false; ++live; return new uint8_t(1); }
            void destroyContext(void* p) override { std::lock_guard<std::mutex> lk(mu); owner.erase(p); --live; delete static_cast<uint8_t*>(p); }
            bool makeCurrent(void* p) override {
                std::lock_guard<std::mutex> lk(mu);
                const auto me = std::this_thread::get_id();
                if (!p) { for (auto it = owner.begin(); it != owner.end();) it = it->second == me ? owner.erase(it) : std::next(it); return true; }
                auto it = owner.find(p);
                if (it != owner.end() && it->second != me) return false;   // EGL: current on another thread
                owner[p] = me;
                return true;
            }
            void* proc(void*, const char*) override { return nullptr; }
        };
        // 0: owner exited before teardown; 1: owner then issues GL; 2: owner then exits without GL;
        // 3: owner then switches to no context (bind).
        for (int mode = 0; mode < 4; ++mode) {
            const bool ownerAlive = mode != 0;
            auto platform = std::make_shared<CountingPlatform>();
            ProcessConfig sgCfg;
            sgCfg.homePath = "/tmp/hvm_sharegroup_teardown";
            sgCfg.hostGL = platform;
            Process sgProc(sgCfg);
            auto& sr = ObjC::rt(sgProc);
            auto& scpu = sgProc.mainThread().cpu;
            const ObjC::id sg = ObjC::id(sr.send(scpu, ObjC::id(sr.send(scpu, ObjC::id(sr.getClass("EAGLSharegroup")), sr.selAlloc)), "init"));
            const ObjC::Class ctxCls = sr.getClass("EAGLContext");
            const ObjC::id context = ObjC::id(sr.send(scpu, ObjC::id(sr.send(scpu, ObjC::id(ctxCls), sr.selAlloc)),
                                                      "initWithAPI:sharegroup:", {2, uint32_t(sg)}));
            sr.send(scpu, ObjC::id(ctxCls), "setCurrentContext:", {context});   // main thread owns the root context
            std::atomic<bool> bound{false}, groupGone{false};
            const uint32_t workerFn = sgProc.defineFunction("test_sharegroup_worker", [&, context, mode](Ctx& x) {
                auto& rt = ObjC::rt(x);
                rt.send(x.cpu, ObjC::id(rt.getClass("EAGLContext")), "setCurrentContext:", {context});   // gets its own shared context
                bound = true;
                if (ownerAlive) {
                    for (int i = 0; i < 1000 && !groupGone.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    if (mode == 1) x.proc.callGuest(x.cpu, *x.proc.lookupSymbol("_glFlush"), {});   // first GL call after the sharegroup died
                    if (mode == 3) rt.send(x.cpu, ObjC::id(rt.getClass("EAGLContext")), "setCurrentContext:", {0});
                }
                x.cpu.r[0] = 0;
            });
            const uint32_t tid = sgProc.startThread(workerFn, 0, 64u << 10, false);
            for (int i = 0; i < 1000 && !bound.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (!ownerAlive) sgProc.joinThread(tid, nullptr);   // the owning thread has exited before teardown
            const int beforeTeardown = platform->live.load();   // root + the worker's context
            sr.send(scpu, ObjC::id(ctxCls), "setCurrentContext:", {0});
            sr.send(scpu, context, "release");
            sr.send(scpu, sg, "release");
            const int afterTeardown = platform->live.load();
            groupGone = true;
            if (ownerAlive) sgProc.joinThread(tid, nullptr);
            const int afterWorker = platform->live.load();
            const char* names[] = {
                "sharegroup teardown frees the worker contexts of threads that already exited",
                "sharegroup teardown retires a worker context a running thread holds; its next GL call releases it",
                "a thread that exits after sharegroup teardown releases its retired worker context",
                "unbinding after sharegroup teardown releases the retired worker context"};
            check(beforeTeardown == 2 && afterTeardown == (ownerAlive ? 1 : 0) && afterWorker == 0, names[mode]);
        }
    }

    // GL errors the runtime's own calls raise in a guest context (worker-context
    // sync, frame sampling) never reach the guest's glGetError; an error the guest
    // had pending is still reported. Clumsy Ninja traps on any glGetError.
    {
        struct FakeGL {
            static std::vector<uint32_t>& errors() { thread_local std::vector<uint32_t> v; return v; }
            static std::thread::id& mainId() { static std::thread::id id; return id; }
            static uint32_t getError() {
                auto& q = errors();
                if (q.empty()) return 0;
                const uint32_t e = q.front();
                q.erase(q.begin());
                return e;
            }
            static void useProgram(uint32_t) { if (std::this_thread::get_id() != mainId()) errors().push_back(0x0502); }
            static void bindFramebuffer(uint32_t, uint32_t) { if (std::this_thread::get_id() != mainId()) errors().push_back(0x0502); }
            static void bindTexture(uint32_t, uint32_t t) { if (t >= 99) errors().push_back(0x0500); }
            static void texImage2D(uint32_t, int32_t, int32_t f, int32_t, int32_t, int32_t, uint32_t, uint32_t type, const void*) {
                // Like an ES 3 driver: the unsized depth-stencil format is refused, the sized one accepted.
                // Like the phone's driver: BGRA is not an internal format, and half floats have their ES 3 name only.
                ++texUploads();
                if (f == 0x84F9 || f == 0x7777 || (f == 0x88F0 && refuseSized()) || type == 0x8D61) errors().push_back(0x0502);
                else if (f == 0x80E1) errors().push_back(0x0500);
                else { lastInternalFormat() = f; lastType() = type; }
            }
            static uint32_t& lastType() { static uint32_t t = 0; return t; }
            static int& texUploads() { static int n = 0; return n; }
            static int32_t& lastInternalFormat() { static int32_t f = 0; return f; }
            static bool& refuseSized() { static bool r = false; return r; }
            static void texSubImage2D(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t f, uint32_t type, const void*) {
                if (f == 0x84F9) errors().push_back(0x0500);
                else if (type == 0x8D61) errors().push_back(0x0502);
                else lastType() = type;
            }
            // Buffer objects, with the iOS driver's rule that glBufferData refuses a mapped buffer.
            struct Buffer { std::vector<uint8_t> data; bool mapped = false; };
            static std::map<uint32_t, Buffer>& buffers() { static std::map<uint32_t, Buffer> b; return b; }
            static uint32_t& boundArray() { static uint32_t b = 0; return b; }
            static void bindBuffer(uint32_t target, uint32_t name) { if (target == 0x8892) boundArray() = name; }
            static void bufferData(uint32_t, intptr_t size, const void* data, uint32_t) {
                Buffer& b = buffers()[boundArray()];
                if (!boundArray() || b.mapped) { errors().push_back(0x0502); return; }
                b.data.assign(size_t(size), 0);
                if (data) std::memcpy(b.data.data(), data, size_t(size));
            }
            static void* mapBufferRange(uint32_t, intptr_t offset, intptr_t, uint32_t) {
                Buffer& b = buffers()[boundArray()];
                if (!boundArray() || b.mapped) { errors().push_back(0x0502); return nullptr; }
                b.mapped = true;
                return b.data.data() + offset;
            }
            static uint8_t unmapBuffer(uint32_t) {
                Buffer& b = buffers()[boundArray()];
                if (!b.mapped) { errors().push_back(0x0502); return 0; }
                b.mapped = false;
                return 1;
            }
            static void getBufferParameteriv(uint32_t, uint32_t pname, int32_t* out) {
                const Buffer& b = buffers()[boundArray()];
                *out = pname == 0x8764 ? int32_t(b.data.size()) : pname == 0x88BC ? int32_t(b.mapped) : 0;
            }
            static void getIntegerv(uint32_t pname, int32_t* out) { *out = pname == 0x8894 ? int32_t(boundArray()) : 0; }
        };
        struct ErrPlatform final : HostGL::Platform {
            std::mutex mu;
            std::unordered_map<void*, std::thread::id> owner;
            const char* name() const override { return "gl-error-test"; }
            void* createContext(int, void*, bool& desktop) override { desktop = false; return new uint8_t(1); }
            void destroyContext(void* p) override { std::lock_guard<std::mutex> lk(mu); owner.erase(p); delete static_cast<uint8_t*>(p); }
            bool makeCurrent(void* p) override {
                std::lock_guard<std::mutex> lk(mu);
                if (!p) return true;
                auto it = owner.find(p);
                if (it != owner.end() && it->second != std::this_thread::get_id()) return false;
                owner[p] = std::this_thread::get_id();
                return true;
            }
            void* proc(void*, const char* n) override {
                const std::string s = n;
                if (s == "glGetError") return reinterpret_cast<void*>(&FakeGL::getError);
                if (s == "glUseProgram") return reinterpret_cast<void*>(&FakeGL::useProgram);
                if (s == "glBindFramebuffer") return reinterpret_cast<void*>(&FakeGL::bindFramebuffer);
                if (s == "glBindTexture") return reinterpret_cast<void*>(&FakeGL::bindTexture);
                if (s == "glTexImage2D") return reinterpret_cast<void*>(&FakeGL::texImage2D);
                if (s == "glTexSubImage2D") return reinterpret_cast<void*>(&FakeGL::texSubImage2D);
                if (s == "glBindBuffer") return reinterpret_cast<void*>(&FakeGL::bindBuffer);
                if (s == "glBufferData") return reinterpret_cast<void*>(&FakeGL::bufferData);
                if (s == "glMapBufferRange") return reinterpret_cast<void*>(&FakeGL::mapBufferRange);
                if (s == "glUnmapBuffer") return reinterpret_cast<void*>(&FakeGL::unmapBuffer);
                if (s == "glGetBufferParameteriv") return reinterpret_cast<void*>(&FakeGL::getBufferParameteriv);
                if (s == "glGetIntegerv") return reinterpret_cast<void*>(&FakeGL::getIntegerv);
                return nullptr;
            }
        };
        // A game that makes its drawable again (Minecraft PE lays its GL view out anew when its
        // keyboard view is added) is told once more where the layer sits: the host may be
        // showing it through a new layer, and a landscape game went unrotated without it.
        {
            struct PlacePlatform final : HostGL::Platform {
                std::vector<std::array<float, 6>> told;
                int storages = 0;
                static void getIntegerv(uint32_t pname, int32_t* out) { *out = pname == 0x8CA7 ? 1 : 0; }   // GL_RENDERBUFFER_BINDING
                const char* name() const override { return "placement-test"; }
                void* createContext(int, void*, bool& desktop) override { desktop = false; return new uint8_t(1); }
                void destroyContext(void* p) override { delete static_cast<uint8_t*>(p); }
                bool makeCurrent(void*) override { return true; }
                void* proc(void*, const char* n) override { return std::string(n) == "glGetIntegerv" ? reinterpret_cast<void*>(&PlacePlatform::getIntegerv) : nullptr; }
                bool drawableStorage(void*, uint32_t, int, int, bool& handled) override { handled = true; ++storages; return true; }
                void layerGeometry(uint32_t, const float unit[6]) override { told.push_back({unit[0], unit[1], unit[2], unit[3], unit[4], unit[5]}); }
                bool presentShows = true;
                bool presentDirect(void*, uint32_t) override {
                    std::this_thread::sleep_for(std::chrono::milliseconds(3));   // a driver that takes its time
                    return presentShows;
                }
            };
            auto place = std::make_shared<PlacePlatform>();
            ProcessConfig pcfg;
            pcfg.homePath = "/tmp/hvm_gl_placement";
            pcfg.hostGL = place;
            Process pproc(pcfg);
            auto& pr = ObjC::rt(pproc);
            auto& pcpu = pproc.mainThread().cpu;
            const ObjC::Class pctxCls = pr.getClass("EAGLContext");
            const ObjC::id pctx = ObjC::id(pr.send(pcpu, ObjC::id(pr.send(pcpu, ObjC::id(pctxCls), pr.selAlloc)), "initWithAPI:", {2}));
            pr.send(pcpu, ObjC::id(pctxCls), "setCurrentContext:", {pctx});
            const uint32_t layer = 0x4242;
            const bool first = HostGL::renderbufferStorageFromDrawable(pproc, pctx, layer, 1136, 640);
            const float turned[6] = {0, -2, 2, 0, 0, 1136};   // a landscape game on a 640x1136 screen
            HostGL::layerPlaced(pproc, layer, turned, 0, 0, 568, 320, 640, 1136);
            const size_t afterPlace = place->told.size();
            const bool second = HostGL::renderbufferStorageFromDrawable(pproc, pctx, layer, 1136, 640);
            const bool resent = place->told.size() == afterPlace + 1 && place->told.back() == place->told[afterPlace - 1];
            check(first && second && afterPlace == 1 && resent, "a drawable made again for the same layer is told its placement again");
            // A different size keeps the layer's place on the screen: the unit scale follows the drawable.
            HostGL::renderbufferStorageFromDrawable(pproc, pctx, layer, 568, 320);
            const bool rescaled = place->told.size() == afterPlace + 2 && std::fabs(place->told.back()[1] - 2 * place->told[0][1]) < 1e-6f &&
                                  std::fabs(place->told.back()[2] - 2 * place->told[0][2]) < 1e-6f && place->told.back()[5] == place->told[0][5];
            check(rescaled, "a drawable made again at another size keeps its place on the screen");
            // Present timing counts every direct-present attempt and the time spent inside it.
            {
                uint32_t lay = 0; int w = 0, h = 0; std::vector<uint8_t> rgba; bool direct = false;
                const uint64_t calls0 = pproc.perf().presentCalls.load(), ns0 = pproc.perf().presentCallNs.load();
                const bool shownOk = HostGL::presentRenderbuffer(pproc, pctx, lay, w, h, rgba, direct) && direct;
                place->presentShows = false;
                bool direct2 = true;
                const bool fellBack = !HostGL::presentRenderbuffer(pproc, pctx, lay, w, h, rgba, direct2) && !direct2;   // no readback in the fake
                const uint64_t calls = pproc.perf().presentCalls.load() - calls0, ns = pproc.perf().presentCallNs.load() - ns0;
                check(shownOk && fellBack && calls == 2 && ns >= 6'000'000 && ns < 2'000'000'000,
                      "present timing counts shown and failed direct presents and the time inside the driver");
            }
            pr.send(pcpu, ObjC::id(pctxCls), "setCurrentContext:", {0});
        }
        FakeGL::mainId() = std::this_thread::get_id();
        auto platform = std::make_shared<ErrPlatform>();
        ProcessConfig ecfg;
        ecfg.homePath = "/tmp/hvm_gl_error_isolation";
        ecfg.hostGL = platform;
        ecfg.glTrace = true;   // the trace checks every call; the guest must still see its own errors
        Process eproc(ecfg);
        auto& er = ObjC::rt(eproc);
        auto& ecpu = eproc.mainThread().cpu;
        const ObjC::Class ctxCls = er.getClass("EAGLContext");
        const ObjC::id context = ObjC::id(er.send(ecpu, ObjC::id(er.send(ecpu, ObjC::id(ctxCls), er.selAlloc)), "initWithAPI:", {2}));
        er.send(ecpu, ObjC::id(ctxCls), "setCurrentContext:", {context});
        eproc.callGuest(ecpu, *eproc.lookupSymbol("_glUseProgram"), {7});
        std::atomic<uint32_t> workerErr{0xffffffffu}, workerKept{0xffffffffu}, workerAfter{0xffffffffu};
        const uint32_t fn = eproc.defineFunction("test_gl_error_worker", [&, context](Ctx& x) {
            auto& rt = ObjC::rt(x);
            rt.send(x.cpu, ObjC::id(rt.getClass("EAGLContext")), "setCurrentContext:", {context});   // shared context, synced
            workerErr = uint32_t(x.proc.callGuest(x.cpu, *x.proc.lookupSymbol("_glGetError"), {}));
            // A guest error pending when the runtime syncs the worker again survives the sync.
            x.proc.callGuest(x.cpu, *x.proc.lookupSymbol("_glBindTexture"), {0x0DE1, 99});
            rt.send(x.cpu, ObjC::id(rt.getClass("EAGLContext")), "setCurrentContext:", {0});
            x.proc.callGuest(x.cpu, *x.proc.lookupSymbol("_glUseProgram"), {0});   // no context: ignored
            rt.send(x.cpu, ObjC::id(rt.getClass("EAGLContext")), "setCurrentContext:", {context});
            workerKept = uint32_t(x.proc.callGuest(x.cpu, *x.proc.lookupSymbol("_glGetError"), {}));
            workerAfter = uint32_t(x.proc.callGuest(x.cpu, *x.proc.lookupSymbol("_glGetError"), {}));
            rt.send(x.cpu, ObjC::id(rt.getClass("EAGLContext")), "setCurrentContext:", {0});
            x.cpu.r[0] = 0;
        });
        const uint32_t tid = eproc.startThread(fn, 0, 64u << 10, false);
        eproc.joinThread(tid, nullptr);
        er.send(ecpu, ObjC::id(ctxCls), "setCurrentContext:", {0});
        check(workerErr == 0 && workerKept == 0x0500 && workerAfter == 0,
              "GL errors from the runtime's worker-context sync never reach the guest's glGetError");
        // The trace's error log keeps the first call that raised each error, with its arguments.
        er.send(ecpu, ObjC::id(ctxCls), "setCurrentContext:", {context});
        eproc.callGuest(ecpu, *eproc.lookupSymbol("_glBindTexture"), {0x0DE1, 100});
        eproc.callGuest(ecpu, *eproc.lookupSymbol("_glBindTexture"), {0x0DE1, 101});
        const uint32_t mainErr = uint32_t(eproc.callGuest(ecpu, *eproc.lookupSymbol("_glGetError"), {}));
        er.send(ecpu, ObjC::id(ctxCls), "setCurrentContext:", {0});
        size_t mainLines = 0;
        bool firstArgs = false;
        for (const auto& line : eproc.glErrorLog())
            if (line.rfind("glBindTexture(", 0) == 0 && line.find("-> 0x0500 [main]") != std::string::npos) {
                ++mainLines;
                firstArgs = line.find("100") != std::string::npos && line.find("101") == std::string::npos;
            }
        if (!(mainErr == 0x0500 && mainLines == 1 && firstArgs))
            for (const auto& line : eproc.glErrorLog()) std::printf("    gl log: %s\n", line.c_str());
        check(mainErr == 0x0500 && mainLines == 1 && firstArgs,
              "the GL trace logs the first failing call of each kind with its arguments, and the guest still sees the error");
        // A hand-written GL function (glTexImage2D) checks its own error: the log names it,
        // not the next generated call (glBindTexture(GL_TEXTURE_2D, 0) once took the blame).
        er.send(ecpu, ObjC::id(ctxCls), "setCurrentContext:", {context});
        const uint32_t savedSp = ecpu.r[13];
        ecpu.r[13] = eproc.heap().allocZeroed(4096) + 4096;   // room for the five stack arguments
        eproc.callGuest(ecpu, *eproc.lookupSymbol("_glTexImage2D"), {0x0DE1, 0, 0x7777, 4, 4, 0, 0x7777, 0x1401, 0});
        ecpu.r[13] = savedSp;
        eproc.callGuest(ecpu, *eproc.lookupSymbol("_glBindTexture"), {0x0DE1, 0});
        const uint32_t texErr = uint32_t(eproc.callGuest(ecpu, *eproc.lookupSymbol("_glGetError"), {}));
        er.send(ecpu, ObjC::id(ctxCls), "setCurrentContext:", {0});
        bool texNamed = false, bindBlamed = false;
        for (const auto& line : eproc.glErrorLog()) {
            texNamed = texNamed || (line.rfind("glTexImage2D(", 0) == 0 && line.find("-> 0x0502") != std::string::npos);
            bindBlamed = bindBlamed || (line.rfind("glBindTexture(", 0) == 0 && line.find("-> 0x0502") != std::string::npos);
        }
        if (!(texErr == 0x0502 && texNamed && !bindBlamed))
            for (const auto& line : eproc.glErrorLog()) std::printf("    gl log: %s\n", line.c_str());
        check(texErr == 0x0502 && texNamed && !bindBlamed,
              "a hand-written GL function's error is logged under its own name, and the guest still sees it");
        // A depth-stencil texture asked for the ES 2 way (unsized) on a driver that only takes the
        // sized format: the runtime asks again with DEPTH24_STENCIL8 and the guest sees no error
        // (Minecraft: Story Mode's framebuffer was incomplete on the phone). If the driver refuses
        // both, the guest gets the first error.
        er.send(ecpu, ObjC::id(ctxCls), "setCurrentContext:", {context});
        FakeGL::lastInternalFormat() = 0;
        ecpu.r[13] = eproc.heap().allocZeroed(4096) + 4096;
        eproc.callGuest(ecpu, *eproc.lookupSymbol("_glTexImage2D"), {0x0DE1, 0, 0x84F9, 4, 4, 0, 0x84F9, 0x84FA, 0});
        const uint32_t depthErr = uint32_t(eproc.callGuest(ecpu, *eproc.lookupSymbol("_glGetError"), {}));
        const int32_t depthFormat = FakeGL::lastInternalFormat();
        FakeGL::refuseSized() = true;
        eproc.callGuest(ecpu, *eproc.lookupSymbol("_glTexImage2D"), {0x0DE1, 0, 0x84F9, 4, 4, 0, 0x84F9, 0x84FA, 0});
        const uint32_t bothErr = uint32_t(eproc.callGuest(ecpu, *eproc.lookupSymbol("_glGetError"), {}));
        const uint32_t bothErrAgain = uint32_t(eproc.callGuest(ecpu, *eproc.lookupSymbol("_glGetError"), {}));
        FakeGL::refuseSized() = false;
        ecpu.r[13] = savedSp;
        er.send(ecpu, ObjC::id(ctxCls), "setCurrentContext:", {0});
        if (!(depthErr == 0 && depthFormat == 0x88F0 && bothErr == 0x0502 && bothErrAgain == 0))
            std::printf("    depth texture: err %x format %x both %x again %x\n", depthErr, depthFormat, bothErr, bothErrAgain);
        check(depthErr == 0 && depthFormat == 0x88F0 && bothErr == 0x0502 && bothErrAgain == 0,
              "an unsized depth-stencil texture the driver refuses is created with the sized format; refused twice, the guest sees the error once");
        // Texture formats the phone's driver refuses in the form old iOS took them (a refused
        // upload leaves a texture that samples black):
        //  - BGRA as the internal format (Goat Simulator's HUD, Crossy Road): asked again as RGBA,
        //    and later uploads go straight to the accepted form;
        //  - half-float RGBA by its OES type (Minecraft: Story Mode's 3D scenes): asked again as
        //    RGBA16F with the ES 3 type, which glTexSubImage2D then uses too.
        {
            er.send(ecpu, ObjC::id(ctxCls), "setCurrentContext:", {context});
            ecpu.r[13] = eproc.heap().allocZeroed(4096) + 4096;
            auto gl = [&](const char* n, std::initializer_list<uint32_t> a) { return uint32_t(eproc.callGuest(ecpu, *eproc.lookupSymbol(n), a)); };
            while (gl("_glGetError", {})) {}
            FakeGL::lastInternalFormat() = 0;
            gl("_glTexImage2D", {0x0DE1, 0, 0x80E1, 512, 512, 0, 0x80E1, 0x1401, 0});
            const uint32_t bgraErr = gl("_glGetError", {});
            const int32_t bgraFormat = FakeGL::lastInternalFormat();
            const int before = FakeGL::texUploads();
            gl("_glTexImage2D", {0x0DE1, 0, 0x80E1, 64, 64, 0, 0x80E1, 0x1401, 0});
            const int secondCalls = FakeGL::texUploads() - before;
            const uint32_t bgraErr2 = gl("_glGetError", {});
            FakeGL::lastInternalFormat() = 0;
            FakeGL::lastType() = 0;
            gl("_glTexImage2D", {0x0DE1, 0, 0x1908, 1024, 1, 0, 0x1908, 0x8D61, 0});
            const uint32_t halfErr = gl("_glGetError", {});
            const int32_t halfFormat = FakeGL::lastInternalFormat();
            const uint32_t halfType = FakeGL::lastType();
            FakeGL::lastType() = 0;
            gl("_glTexSubImage2D", {0x0DE1, 0, 0, 0, 1024, 1, 0x1908, 0x8D61, 0});
            const uint32_t subErr = gl("_glGetError", {});
            const uint32_t subType = FakeGL::lastType();
            ecpu.r[13] = savedSp;
            er.send(ecpu, ObjC::id(ctxCls), "setCurrentContext:", {0});
            if (!(bgraErr == 0 && bgraFormat == 0x1908 && secondCalls == 1 && bgraErr2 == 0))
                std::printf("    bgra: err %x format %x second calls %d err %x\n", bgraErr, bgraFormat, secondCalls, bgraErr2);
            check(bgraErr == 0 && bgraFormat == 0x1908 && secondCalls == 1 && bgraErr2 == 0,
                  "a BGRA texture the driver refuses with a BGRA internal format is created as RGBA; the next one is asked once");
            if (!(halfErr == 0 && halfFormat == 0x881A && halfType == 0x140B && subErr == 0 && subType == 0x140B))
                std::printf("    half float: err %x format %x type %x sub err %x type %x\n", halfErr, halfFormat, halfType, subErr, subType);
            check(halfErr == 0 && halfFormat == 0x881A && halfType == 0x140B && subErr == 0 && subType == 0x140B,
                  "a half-float texture the driver refuses by its OES type is created as RGBA16F, and updated with the ES 3 type");
        }
        // Two buffers mapped through the same target, then unmapped in the other order (Clumsy
        // Ninja's vertex setup). Each unmap must write its own data to its own buffer and end the
        // driver's mapping, or the next glBufferData is refused, as it was on the phone.
        {
            er.send(ecpu, ObjC::id(ctxCls), "setCurrentContext:", {context});
            auto gl = [&](const char* n, std::initializer_list<uint32_t> a) { return uint32_t(eproc.callGuest(ecpu, *eproc.lookupSymbol(n), a)); };
            while (gl("_glGetError", {})) {}
            gl("_glBindBuffer", {0x8892, 10});
            gl("_glBufferData", {0x8892, 8, 0, 0x88E8});
            const uint32_t mapA = gl("_glMapBufferOES", {0x8892, 0x88B9});
            gl("_glBindBuffer", {0x8892, 9});
            gl("_glBufferData", {0x8892, 4, 0, 0x88E8});
            const uint32_t mapB = gl("_glMapBufferOES", {0x8892, 0x88B9});
            if (mapA) std::memcpy(eproc.mem().host(mapA), "AAAAAAAA", 8);
            if (mapB) std::memcpy(eproc.mem().host(mapB), "BBBB", 4);
            gl("_glBindBuffer", {0x8892, 10});
            const bool unmapA = (gl("_glUnmapBufferOES", {0x8892}) & 0xFF) == 1;
            gl("_glBindBuffer", {0x8892, 9});
            const bool unmapB = (gl("_glUnmapBufferOES", {0x8892}) & 0xFF) == 1;
            const auto& bufs = FakeGL::buffers();
            const bool contents = bufs.at(10).data == std::vector<uint8_t>(8, 'A') && bufs.at(9).data == std::vector<uint8_t>(4, 'B');
            const uint32_t fresh = eproc.heap().allocZeroed(4);
            std::memcpy(eproc.mem().host(fresh), "CCCC", 4);
            gl("_glBufferData", {0x8892, 4, fresh, 0x88E4});
            const uint32_t afterErr = gl("_glGetError", {});
            // glBufferData on a buffer the app left mapped ends the mapping; a late unmap reports
            // failure and writes nothing.
            FakeGL::buffers()[11] = {};
            gl("_glBindBuffer", {0x8892, 11});
            gl("_glBufferData", {0x8892, 4, 0, 0x88E8});
            gl("_glMapBufferOES", {0x8892, 0x88B9});
            FakeGL::buffers()[11].mapped = false;   // what a real driver's glBufferData does to the old mapping
            gl("_glBufferData", {0x8892, 4, fresh, 0x88E4});
            const bool lateUnmap = (gl("_glUnmapBufferOES", {0x8892}) & 0xFF) == 0 && FakeGL::buffers()[11].data == std::vector<uint8_t>(4, 'C');
            while (gl("_glGetError", {})) {}
            gl("_glBindBuffer", {0x8892, 0});
            er.send(ecpu, ObjC::id(ctxCls), "setCurrentContext:", {0});
            if (!(mapA && mapB && mapA != mapB && unmapA && unmapB && contents && afterErr == 0 && lateUnmap))
                std::printf("    mapped buffers: maps %x %x unmap %d %d contents %d err %x late %d\n", mapA, mapB, unmapA, unmapB, contents, afterErr, lateUnmap);
            check(mapA && mapB && mapA != mapB && unmapA && unmapB && contents && afterErr == 0 && lateUnmap,
                  "two buffers mapped on one target each get their own data back on unmap, and glBufferData works afterwards");
        }
        // glTexSubImage2D has no check of its own: HG_BEGIN's check on return names it.
        er.send(ecpu, ObjC::id(ctxCls), "setCurrentContext:", {context});
        ecpu.r[13] = eproc.heap().allocZeroed(4096) + 4096;
        eproc.callGuest(ecpu, *eproc.lookupSymbol("_glTexSubImage2D"), {0x0DE1, 0, 0, 0, 4, 4, 0x84F9, 0x84FA, 0});
        ecpu.r[13] = savedSp;
        eproc.callGuest(ecpu, *eproc.lookupSymbol("_glBindTexture"), {0x0DE1, 0});
        const uint32_t subErr = uint32_t(eproc.callGuest(ecpu, *eproc.lookupSymbol("_glGetError"), {}));
        er.send(ecpu, ObjC::id(ctxCls), "setCurrentContext:", {0});
        bool subNamed = false, subBindBlamed = false;
        for (const auto& line : eproc.glErrorLog()) {
            subNamed = subNamed || (line.rfind("glTexSubImage2D(", 0) == 0 && line.find("-> 0x0500") != std::string::npos);
            subBindBlamed = subBindBlamed || (line.rfind("glBindTexture(3553, 0)", 0) == 0 && line.find("-> 0x0500") != std::string::npos);
        }
        check(subErr == 0x0500 && subNamed && !subBindBlamed,
              "HG_BEGIN checks a hand-written GL function on return: its error is not blamed on the next call");
    }

    // PR #167: fixed-size heap objects share writable pages, so a successful
    // host mapping check alone is not enough to protect adjacent allocations.
    {
        ProcessConfig cfg;
        cfg.homePath = "/tmp/hvm-format-bounds-tests";
        Process proc(cfg);
        auto& cpu = proc.mainThread().cpu;
        const uint32_t stack = proc.heap().allocZeroed(1024);
        cpu.r[CPU::kSP] = stack + 1024;
        std::string missingSymbols;   // names the tests asked for that are not registered
        auto run = [&](const char* name, std::initializer_list<uint32_t> args) {
            bool fault = false;
            int32_t result = -1;
            try {
                const auto sym = proc.lookupSymbol(name);
                if (!sym) { missingSymbols += std::string(name) + " "; return std::pair<bool, int32_t>{true, result}; }
                result = int32_t(proc.callGuest(cpu, *sym, args));
            } catch (const CPU::GuestFault&) {
                fault = true;
            } catch (const std::exception&) {
                fault = true;
            }
            return std::pair<bool, int32_t>{fault, result};
        };
        const uint32_t exact = proc.allocCString("123456789012345");
        const uint32_t overflow = proc.allocCString("1234567890123456");
        const uint32_t shortText = proc.allocCString("12345");
        const uint32_t small = proc.heap().allocZeroed(16);
        auto r = run("_sprintf", {small, exact});
        check(!r.first && r.second == 15 && proc.mem().read<uint8_t>(small + 15) == 0,
              "sprintf accepts exact heap allocation fit");
        r = run("_sprintf", {small, overflow});
        check(r.first, "sprintf rejects one-byte heap allocation overflow");

        const uint32_t adjacent = proc.heap().allocZeroed(16);
        proc.mem().write<uint8_t>(adjacent, 0x5a);
        r = run("_sprintf", {small, overflow});
        check(r.first && proc.mem().read<uint8_t>(adjacent) == 0x5a,
              "sprintf cannot corrupt adjacent live allocation");

        const uint32_t inside = proc.heap().allocZeroed(16) + 5;
        r = run("_sprintf", {inside, proc.allocCString("1234567890")});
        check(!r.first && r.second == 10, "sprintf accepts interior pointer exact fit");
        r = run("_sprintf", {inside, proc.allocCString("12345678901")});
        check(r.first, "sprintf rejects interior pointer overflow");

        r = run("_snprintf", {small, 4, shortText});
        check(!r.first && r.second == 5 &&
              std::memcmp(proc.mem().host(small), "123\0", 4) == 0,
              "snprintf truncates and reports untruncated length");
        r = run("_snprintf", {0, 0, shortText});
        check(!r.first && r.second == 5, "snprintf allows null destination with zero size");
        r = run("___snprintf_chk", {small, 4, 0, 4, shortText});
        check(!r.first && r.second == 5, "snprintf_chk permits truncation within object bound");
        r = run("___snprintf_chk", {small, 5, 0, 4, shortText});
        check(r.first, "snprintf_chk rejects size exceeding object bound");
        r = run("___snprintf_chk", {0, 0, 0, 0, shortText});
        check(!r.first && r.second == 5, "snprintf_chk accepts zero-size destination");
        r = run("_vsnprintf", {small, 4, shortText, 0});
        check(!r.first && r.second == 5 &&
              std::memcmp(proc.mem().host(small), "123\0", 4) == 0,
              "vsnprintf truncates and reports untruncated length");
        r = run("___vsnprintf_chk", {small, 4, 0, 4, shortText, 0});
        check(!r.first && r.second == 5, "vsnprintf_chk permits bounded truncation");
        r = run("___vsnprintf_chk", {small, 5, 0, 4, shortText, 0});
        check(r.first, "vsnprintf_chk rejects size exceeding object bound");
        r = run("___sprintf_chk", {small, 0, 15, exact});
        check(r.first, "sprintf_chk enforces object size including terminator");
        r = run("___sprintf_chk", {small, 0, 16, exact});
        check(!r.first && r.second == 15, "sprintf_chk accepts exact compiler object bound");
        r = run("___vsprintf_chk", {small, 0, 15, exact});
        check(r.first, "vsprintf_chk exists and enforces object size");

        // The managed heap reserves 0x10000000..0x80000000; use an
        // independent mapping so this is genuinely a stack/global-style
        // destination, not an unallocated address within the heap reserve.
        constexpr uint32_t page = 0x90000000;
        proc.mem().map(page, 4096, Memory::kProtRW);
        const uint32_t edge = page + 4096 - 5;
        r = run("_sprintf", {edge, proc.allocCString("1234")});
        check(!r.first && r.second == 4, "sprintf accepts mapped guest-page exact fit");
        r = run("_sprintf", {edge, shortText});
        check(r.first, "sprintf rejects overflow into unmapped guest page");
        const uint32_t global = proc.allocStatic(32);
        r = run("_sprintf", {global, shortText});
        check(!r.first && r.second == 5 &&
              std::memcmp(proc.mem().host(global), "12345\0", 6) == 0,
              "sprintf permits writable global buffers outside tracked heap");

        // Wide formatting stores 4 bytes per wchar_t. C99 swprintf honours n
        // (including the NUL), returns -1 when the text does not fit, and
        // must throw before any store when that span is not writable.
        struct WideCall { bool failed = false; bool bounds = false; int32_t result = -1; };
        auto wideRun = [&](const char* name, std::initializer_list<uint32_t> args) {
            WideCall out;
            try {
                const auto sym = proc.lookupSymbol(name);
                if (!sym) { missingSymbols += std::string(name) + " "; out.failed = true; return out; }
                out.result = int32_t(proc.callGuest(cpu, *sym, args));
            } catch (const CPU::GuestFault& fault) {
                out.failed = true;
                out.bounds = fault.exit == CPU::Exit::HostError;
            } catch (const std::exception&) {
                out.failed = true;
                out.bounds = true;
            }
            return out;
        };
        auto wideLiteral = [&](const char* text) {
            const size_t n = std::strlen(text);
            const uint32_t p = proc.heap().allocZeroed(uint32_t((n + 1) * 4));
            for (size_t i = 0; i < n; ++i)
                proc.mem().write<uint32_t>(p + uint32_t(i * 4), uint8_t(text[i]));
            return p;
        };
        auto wideEquals = [&](uint32_t addr, const char* text) {
            const size_t n = std::strlen(text);
            for (size_t i = 0; i <= n; ++i) {
                const uint32_t expect = i < n ? uint32_t(uint8_t(text[i])) : 0u;
                if (proc.mem().read<uint32_t>(addr + uint32_t(i * 4)) != expect) return false;
            }
            return true;
        };
        auto paint = [&](uint32_t addr, uint32_t bytes, uint8_t value) {
            for (uint32_t i = 0; i < bytes; ++i) proc.mem().write<uint8_t>(addr + i, value);
        };
        auto painted = [&](uint32_t addr, uint32_t bytes, uint8_t value) {
            for (uint32_t i = 0; i < bytes; ++i)
                if (proc.mem().read<uint8_t>(addr + i) != value) return false;
            return true;
        };
        const uint32_t wideExact = wideLiteral("123");
        const uint32_t wideOver = wideLiteral("1234");
        const uint32_t wideLong = wideLiteral("12345");
        const uint32_t wideBuf = proc.heap().allocZeroed(16);
        const uint32_t wideNext = proc.heap().allocZeroed(16);
        paint(wideNext, 16, 0xa5);

        auto w = wideRun("_swprintf", {wideBuf, 4, wideExact});
        check(!w.failed && w.result == 3 && wideEquals(wideBuf, "123") && painted(wideNext, 16, 0xa5),
              "swprintf accepts an exact four-wchar heap fit");
        w = wideRun("_vswprintf", {wideBuf, 4, wideExact, 0});
        check(!w.failed && w.result == 3 && wideEquals(wideBuf, "123"),
              "vswprintf accepts an exact four-wchar heap fit");

        paint(wideBuf, 16, 0xa5);
        w = wideRun("_swprintf", {wideBuf, 0, wideLong});
        check(!w.failed && w.result == -1 && painted(wideBuf, 16, 0xa5),
              "swprintf with n=0 writes nothing and returns -1");
        w = wideRun("_vswprintf", {0, 0, wideLong, 0});
        check(!w.failed && w.result == -1, "vswprintf with n=0 accepts a null destination");

        w = wideRun("_swprintf", {wideBuf, 4, wideLong});
        check(!w.failed && w.result == -1 && wideEquals(wideBuf, "123") && painted(wideNext, 16, 0xa5),
              "swprintf truncates to n-1 wide characters and returns -1");
        w = wideRun("_vswprintf", {wideBuf, 4, wideLong, 0});
        check(!w.failed && w.result == -1 && wideEquals(wideBuf, "123"),
              "vswprintf truncates to n-1 wide characters and returns -1");

        paint(wideBuf, 16, 0xa5);
        paint(wideNext, 16, 0xa5);
        w = wideRun("_swprintf", {wideBuf, 5, wideOver});
        check(w.failed && w.bounds && painted(wideBuf, 16, 0xa5) && painted(wideNext, 16, 0xa5),
              "swprintf throws before a wide write can leave its heap allocation");
        w = wideRun("_vswprintf", {wideBuf, 5, wideOver, 0});
        check(w.failed && w.bounds && painted(wideBuf, 16, 0xa5) && painted(wideNext, 16, 0xa5),
              "vswprintf throws before a wide write can leave its heap allocation");

        const uint32_t wideInside = proc.heap().allocZeroed(16) + 4;
        paint(wideInside, 12, 0xa5);
        w = wideRun("_swprintf", {wideInside, 3, wideLiteral("12")});
        check(!w.failed && w.result == 2 && wideEquals(wideInside, "12"),
              "swprintf accepts an interior pointer that fits the remaining allocation");
        paint(wideInside, 12, 0xa5);
        w = wideRun("_swprintf", {wideInside, 4, wideExact});
        check(w.failed && w.bounds && painted(wideInside, 12, 0xa5),
              "swprintf throws when an interior wide write leaves its allocation");

        constexpr uint32_t widePage = 0x91000000;
        proc.mem().map(widePage, 4096, Memory::kProtRW);
        const uint32_t wideEdge = widePage + 4096 - 16;
        w = wideRun("_swprintf", {wideEdge, 4, wideExact});
        check(!w.failed && w.result == 3 && wideEquals(wideEdge, "123"),
              "swprintf accepts a mapped guest-page exact wide fit");
        paint(wideEdge, 16, 0xa5);
        w = wideRun("_swprintf", {wideEdge, 5, wideOver});
        check(w.failed && w.bounds && painted(wideEdge, 16, 0xa5),
              "swprintf throws before a wide write runs onto an unmapped page");
        paint(wideEdge, 16, 0xa5);
        w = wideRun("_vswprintf", {wideEdge, 5, wideOver, 0});
        check(w.failed && w.bounds && painted(wideEdge, 16, 0xa5),
              "vswprintf throws before a wide write runs onto an unmapped page");

        const uint32_t wideBig = proc.heap().allocZeroed(64);
        w = wideRun("___swprintf_chk", {wideBig, 4, 0, 16, wideExact});
        check(!w.failed && w.result == 3 && wideEquals(wideBig, "123"),
              "swprintf_chk accepts an exact object-size wide span");
        paint(wideBig, 64, 0xa5);
        w = wideRun("___swprintf_chk", {wideBig, 5, 0, 16, wideExact});
        check(w.failed && w.bounds && painted(wideBig, 64, 0xa5),
              "swprintf_chk throws when the wchar count exceeds the object size");
        w = wideRun("___vswprintf_chk", {wideBig, 5, 0, 16, wideExact, 0});
        check(w.failed && w.bounds && painted(wideBig, 64, 0xa5),
              "vswprintf_chk throws when the wchar count exceeds the object size");
        w = wideRun("___swprintf_chk", {wideBuf, 4, 0, 16, wideLong});
        check(!w.failed && w.result == -1 && wideEquals(wideBuf, "123"),
              "swprintf_chk truncates inside the object size and returns -1");
        check(missingSymbols.empty(), "format-bounds tests only call registered guest functions (a missing symbol would make a rejection check pass without running)");
    }

    // Mapping and writes must be serialized: on 16 KB hosts a committed host
    // page can remain accessible after its 4 KB guest mapping is removed.
    {
        Memory::GuestArena arena;
        constexpr uint32_t page = 0x90000000;
        arena.map(page, 4096, Memory::kProtRW);
        std::atomic<int> attempted{0};
        std::thread writer([&] {
            for (int i = 0; i < 1000; ++i) {
                arena.copyCStringWritable(page, "ok", 2);
                attempted.fetch_add(1, std::memory_order_relaxed);
            }
        });
        for (int i = 0; i < 100; ++i) {
            arena.unmap(page, 4096);
            arena.map(page, 4096, Memory::kProtRW);
        }
        writer.join();
        arena.unmap(page, 4096);
        check(attempted.load() == 1000 && !arena.copyCStringWritable(page, "no", 2),
              "formatted guest-page writes synchronize with concurrent map/unmap");
    }

    // Call ring: the last-N host calls a guest thread made, kept for crash
    // bundles and stuck-quit diagnosis (T1/T2). Checked on its own first, then
    // through the real Engine::callHle hook.
    {
        using Diag::CallKind;
        using Diag::CallRecord;
        const auto inOrder = [](const std::vector<CallRecord>& v, uint64_t firstSeq) {
            for (size_t i = 0; i < v.size(); ++i)
                if (v[i].seq != firstSeq + i || v[i].kind != CallKind::Hle || v[i].id != uint32_t((firstSeq + i) * 7)) return false;
            return true;
        };
        Diag::CallRing<8> small;
        check(small.last(8).empty() && small.total() == 0, "call ring starts empty");
        for (uint32_t i = 1; i <= 5; ++i) small.push(CallKind::Hle, i * 7);
        check(small.total() == 5 && small.last(8).size() == 5 && inOrder(small.last(8), 1),
              "call ring returns every record, oldest first, before it wraps");
        for (uint32_t i = 6; i <= 27; ++i) small.push(CallKind::Hle, i * 7);   // 27 pushes into 8 slots
        const auto lastFull = small.last(8);
        check(small.total() == 27 && lastFull.size() == 8 && inOrder(lastFull, 20),
              "call ring filled past capacity returns exactly the last N records in order");
        check(inOrder(small.last(3), 25) && small.last(3).size() == 3,
              "call ring last(n) for n below capacity returns the newest n records");
        check(small.last(1000).size() == 8 && small.last(0).empty(),
              "call ring caps a request at its capacity");

        // A reader on another thread while the writer wraps the ring many times
        // must see only whole records in strictly increasing order.
        Diag::CallRing<64> shared;
        std::atomic<bool> writing{true};
        std::thread writer([&] {
            for (uint32_t i = 1; i <= 400000; ++i) shared.push(CallKind::Hle, i * 7);
            writing = false;
        });
        bool consistent = true;
        uint64_t reads = 0;
        while (writing.load()) {
            const auto snap = shared.last(64);
            for (size_t i = 0; i < snap.size(); ++i) {
                if (snap[i].kind != CallKind::Hle || snap[i].id != uint32_t(snap[i].seq * 7) ||
                    (i > 0 && snap[i].seq <= snap[i - 1].seq)) consistent = false;
            }
            ++reads;
        }
        writer.join();
        check(consistent && reads > 0 && shared.total() == 400000 && inOrder(shared.last(64), 400000 - 63),
              "call ring never returns a torn or reordered record to a concurrent reader");

        ProcessConfig ringCfg;
        ringCfg.homePath = "/tmp/hvm_call_ring";
        Process ringProc(ringCfg);
        auto& ringCpu = ringProc.mainThread().cpu;
        const uint32_t ringFn = ringProc.defineFunction("test_call_ring_fn", [](Ctx&) {});
        const uint32_t ringIndex = ringProc.engine().hleIndex(ringFn);
        const Diag::ThreadCallRing* before = ringCpu.call_ring.load();
        const uint64_t totalBefore = before ? before->total() : 0;
        for (int i = 0; i < 300; ++i) ringProc.engine().callHle(&ringCpu, ringIndex);
        const Diag::ThreadCallRing* ring = ringCpu.call_ring.load();
        bool hooked = ring && ring->total() == totalBefore + 300;
        if (hooked) {
            const auto tail = ring->last(Diag::ThreadCallRing::kCapacity);
            hooked = tail.size() == Diag::ThreadCallRing::kCapacity;
            for (size_t i = 0; hooked && i < tail.size(); ++i)
                hooked = tail[i].kind == CallKind::Hle && tail[i].id == ringIndex &&
                         tail[i].seq == totalBefore + 300 - tail.size() + 1 + i;
        }
        check(hooked, "Engine::callHle records each host call in the calling thread's ring");
        {   // Total and records come from one read of the head. A push forced between the head read and the record
            // read must not leak into the snapshot; the former two-read order (total(), then last()) does leak it.
            const auto pushOne = [&] { ringProc.engine().callHle(&ringCpu, ringIndex); };
            const Diag::ThreadCallRing* r2 = ringCpu.call_ring.load();
            const auto snap = r2->snapshot(16, pushOne);
            const bool consistent = !snap.calls.empty() && snap.calls.back().seq <= snap.total;
            const uint64_t oldTotal = r2->total();
            pushOne();
            const auto oldCalls = r2->last(16);   // the former order: a push landed between the two reads
            check(consistent && !oldCalls.empty() && oldCalls.back().seq > oldTotal,
                  "a call-ring snapshot never holds a record newer than its total (the former two-read order does)");
        }

        // The crash bundle carries each thread's last calls (T2c, #259): oldest first, exactly the last N,
        // with the support status of a function that was declared a Stub.
        const uint32_t ringStubFn = ringProc.defineFunction("_test_call_ring_stub_fn", [](Ctx&) {});
        const uint32_t ringStubIndex = ringProc.engine().hleIndex(ringStubFn);
        supportRegistry(ringProc).record({SupportKind::Function, "_test_call_ring_stub_fn", SupportStatus::Stub,
                                          "test: a placeholder that must show up as a stub hit"});
        ringProc.engine().callHle(&ringCpu, ringStubIndex);
        const uint64_t ringTotal = ringCpu.call_ring.load()->total();
        const std::string ringJson = ringProc.callRingJson(8);
        auto seqAt = [&](uint64_t seq) { return ringJson.find("\"seq\":" + std::to_string(seq) + ","); };
        size_t hleEntries = 0;
        for (size_t at = ringJson.find("\"kind\":\"hle\""); at != std::string::npos; at = ringJson.find("\"kind\":\"hle\"", at + 1)) ++hleEntries;
        const size_t lastEntry = seqAt(ringTotal);
        const size_t stubAt = ringJson.find("\"status\":\"Stub\"");
        bool ringOrdered = seqAt(ringTotal - 7) != std::string::npos && seqAt(ringTotal - 8) == std::string::npos;
        for (uint64_t q = ringTotal - 7; ringOrdered && q < ringTotal; ++q) ringOrdered = seqAt(q) < seqAt(q + 1);
        check(ringJson.find("\"thread\":" + std::to_string(ringProc.mainThread().id) + ",\"total\":" + std::to_string(ringTotal)) != std::string::npos &&
              hleEntries == 8 && ringOrdered,
              "callRingJson lists exactly the last N calls of a thread, oldest first, with the thread's total");
        check(lastEntry != std::string::npos && stubAt != std::string::npos && stubAt > lastEntry &&
              ringJson.find("\"name\":\"test_call_ring_stub_fn\"") != std::string::npos &&
              ringJson.find("\"status\"") == stubAt && ringJson.find("\"status\"", stubAt + 1) == std::string::npos,
              "callRingJson marks a declared Stub call and leaves unlabelled calls without a status");

        // A worker that has finished (the one that crashed is marked finished before the bundle is written)
        // is still in the dump.
        const uint32_t ringWorkerFn = ringProc.defineFunction("test_call_ring_worker_fn", [&](Ctx& x) {
            ringProc.engine().callHle(&x.cpu, ringStubIndex);   // a host call made on the worker lands in the worker's ring
            x.cpu.r[0] = 0;
        });
        const uint32_t ringWorker = ringProc.startThread(ringWorkerFn, 0, 64u << 10, false);
        uint32_t ringWorkerResult = 1;
        const bool ringWorkerJoined = ringProc.joinThread(ringWorker, &ringWorkerResult);
        const std::string ringAfter = ringProc.callRingJson(8);
        size_t ringThreads = 0;   // the dump names a thread by its id (not the pthread handle startThread returns)
        for (size_t at = ringAfter.find("\"thread\":"); at != std::string::npos; at = ringAfter.find("\"thread\":", at + 1)) ++ringThreads;
        check(ringWorkerJoined && ringThreads == 2,
              "callRingJson keeps a finished worker's ring so a crashed thread is not dropped");

        // Generated placeholders for missing imports and C++ vtable slots are declared Stub, never unlabelled.
        ringProc.makeMissingStub("_hvm_test_missing_import");
        ringProc.makeMissingStub("__ZTV13HVMTestMissing");
        const auto missingFn = supportRegistry(ringProc).find("_hvm_test_missing_import");
        const auto missingSlot = supportRegistry(ringProc).find("__ZTV13HVMTestMissing$hvm_slot0");
        check(missingFn && missingFn->status == SupportStatus::Stub && missingSlot && missingSlot->status == SupportStatus::Stub,
              "a generated missing-function or vtable-slot stub is recorded as Stub");
    }

    // Teardown diagnostics: a guest thread still running (blocked in the real `read` on
    // an empty pipe) is named with the host function it is inside and its last calls.
    {
        int fds[2];
        const bool piped = ::pipe(fds) == 0;
        ProcessConfig diagCfg;
        diagCfg.homePath = "/tmp/hvm_blocked_threads";
        Process diagProc(diagCfg);
        check(diagProc.describeRunningThreads().empty(), "no running guest threads: nothing to describe");
        std::atomic<bool> readerDone{false};
        const uint32_t readerFn = diagProc.defineFunction("test_blocked_reader", [&](Ctx& x) {
            const uint32_t buf = x.proc.heap().allocZeroed(16);
            x.proc.callGuest(x.cpu, *x.proc.lookupSymbol("_read"), {uint32_t(fds[0]), buf, 1});
            readerDone = true;
        });
        if (piped) diagProc.startThread(readerFn, 0, 256u << 10, true);
        std::string described;
        for (int i = 0; piped && i < 300; ++i) {
            described = diagProc.describeRunningThreads();
            if (described.find("_read") != std::string::npos && described.find("last calls:") != std::string::npos) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(piped && described.find("thread ") != std::string::npos && described.find("inside ") != std::string::npos &&
              described.find("_read") != std::string::npos && described.find("last calls:") != std::string::npos,
              "describeRunningThreads names a blocked guest thread, the host function it is inside and its last calls");
        if (piped) {
            const ssize_t unblocked = ::write(fds[1], "x", 1);   // let the reader finish before the Process is destroyed
            for (int i = 0; i < 300 && !readerDone.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            check(unblocked == 1 && readerDone.load(), "the blocked reader thread finishes once its pipe has data");
            for (int i = 0; i < 300 && !diagProc.describeRunningThreads().empty(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            check(diagProc.describeRunningThreads().empty(), "a finished guest thread is no longer described as running");
            ::close(fds[0]);
            ::close(fds[1]);
        }
    }

    // Blocking host reads and locks must notice a stop request (T1/#234). A guest thread
    // parked in read() on an empty pipe or in a contended flock() used to ignore
    // requestStop() forever and outlive the process teardown. Normal behaviour must not change.
    {
        using namespace std::chrono;
        struct Run { bool earlyDone; bool done; double wakeMs; };
        auto callG = [](Ctx& x, const char* sym, std::initializer_list<uint32_t> a) { return uint32_t(x.proc.callGuest(x.cpu, *x.proc.lookupSymbol(sym), a)); };
        // Run `body` on a guest thread. If it finishes within `earlyWaitMs` it was not blocked.
        // Otherwise request a stop and time how long it takes to leave.
        auto runCase = [&](std::function<void(Process&, Ctx&)> body, int earlyWaitMs) {
            ProcessConfig cfg;
            cfg.homePath = "/tmp/hvm_stop_wake";
            auto* proc = new Process(cfg);
            std::atomic<bool> done{false};
            const uint32_t fn = proc->defineFunction("stop_wake_fn", [&](Ctx& x) { try { body(*proc, x); } catch (...) {} done = true; });
            proc->startThread(fn, 0, 256u << 10, true);
            const auto start = steady_clock::now();
            while (!done.load() && steady_clock::now() - start < milliseconds(earlyWaitMs)) std::this_thread::sleep_for(milliseconds(5));
            Run r{done.load(), done.load(), 0};
            if (!r.earlyDone) {
                const auto t0 = steady_clock::now();
                proc->requestStop();
                while (!done.load() && steady_clock::now() - t0 < seconds(3)) std::this_thread::sleep_for(milliseconds(5));
                r.done = done.load();
                r.wakeMs = duration<double, std::milli>(steady_clock::now() - t0).count();
            }
            if (r.done) delete proc;   // a thread that never left still owns the process: leak it rather than free under it
            return r;
        };
        std::atomic<int32_t> ret{99};
        const auto readFd = [&](int fd) { return [&ret, fd, callG](Process& p, Ctx& x) { ret = int32_t(callG(x, "_read", {uint32_t(fd), p.heap().allocZeroed(16), 1})); }; };

        int blockingPipe[2]; ::pipe(blockingPipe);
        const Run readStop = runCase(readFd(blockingPipe[0]), 300);
        check(!readStop.earlyDone && readStop.done && readStop.wakeMs < 1000, "read() on an empty pipe wakes on a stop request");

        char lockPath[] = "/tmp/hvm_stop_wake_lock_XXXXXX";
        const int lockFile = ::mkstemp(lockPath);
        const int heldFd = lockFile, waiterFd = ::open(lockPath, O_RDWR);
        ::flock(heldFd, LOCK_EX);
        const auto lockFd = [&](int fd, int op) { return [&ret, fd, op, callG](Process&, Ctx& x) { ret = int32_t(callG(x, "_flock", {uint32_t(fd), uint32_t(op)})); }; };
        const Run flockStop = runCase(lockFd(waiterFd, 2), 300);
        check(!flockStop.earlyDone && flockStop.done && flockStop.wakeMs < 1000, "a contended flock() wakes on a stop request");

        // Unchanged behaviour.
        int dataPipe[2]; ::pipe(dataPipe); (void)!::write(dataPipe[1], "x", 1);
        ret = 99;
        Run r = runCase(readFd(dataPipe[0]), 1500);
        check(r.earlyDone && ret == 1, "read() on a pipe with data still returns it immediately");

        int eofPipe[2]; ::pipe(eofPipe); ::close(eofPipe[1]);
        ret = 99;
        r = runCase(readFd(eofPipe[0]), 1500);
        check(r.earlyDone && ret == 0, "read() at end of file on a pipe still returns 0");

        int nbPipe[2]; ::pipe(nbPipe); ::fcntl(nbPipe[0], F_SETFL, ::fcntl(nbPipe[0], F_GETFL) | O_NONBLOCK);
        ret = 99;
        r = runCase(readFd(nbPipe[0]), 1500);
        check(r.earlyDone && ret == -1, "read() on an empty non-blocking pipe still fails at once instead of waiting");

        const int regFd = ::open(lockPath, O_RDONLY);
        ret = 99;
        r = runCase(readFd(regFd), 1500);
        check(r.earlyDone && ret == 0, "read() on a regular file is unaffected");

        ret = 99;
        r = runCase(lockFd(waiterFd, 2 | 4), 1500);
        check(r.earlyDone && ret == -1, "flock() with LOCK_NB on a held lock still fails at once");

        char freePath[] = "/tmp/hvm_stop_wake_free_XXXXXX";
        const int freeFd = ::mkstemp(freePath);
        ret = 99;
        r = runCase(lockFd(freeFd, 2), 1500);
        check(r.earlyDone && ret == 0, "an uncontended blocking flock() still succeeds at once");

        std::thread releaser([&] { std::this_thread::sleep_for(milliseconds(150)); ::flock(heldFd, LOCK_UN); });
        ret = 99;
        r = runCase(lockFd(waiterFd, 2), 3000);
        releaser.join();
        check(r.earlyDone && ret == 0, "a blocking flock() still acquires once the holder lets go");

        for (int fd : {blockingPipe[0], blockingPipe[1], dataPipe[0], dataPipe[1], eofPipe[0], nbPipe[0], nbPipe[1], regFd, freeFd, waiterFd, heldFd}) ::close(fd);
        ::unlink(lockPath);
        ::unlink(freePath);
    }
    {
        ProcessConfig cfg;
        cfg.homePath = "/tmp/hvm-dlsym-tests";
        Process proc(cfg);
        auto& cpu = proc.mainThread().cpu;
        const uint32_t stack = proc.heap().allocZeroed(1024);
        cpu.r[CPU::kSP] = stack + 1024;

        auto callDlsym = [&](const char* name) -> uint32_t {
            const uint32_t namePtr = proc.allocCString(name);
            return uint32_t(proc.callGuest(cpu, *proc.lookupSymbol("_dlsym"), {0xFFFFFFFEu, namePtr}));
        };

        const uint32_t addrHttp = callDlsym("kCFHTTPVersion1_1");
        const uint32_t addrProxyFtp = callDlsym("kCFProxyTypeFTP");
        const uint32_t addrProxyJs = callDlsym("kCFProxyTypeAutoConfigurationJavaScript");

        Ctx cx{proc, cpu, proc.mem()};
        auto hasValue = [&](uint32_t addr, const char* expected) {
            return addr != 0 && proc.mem().read<uint32_t>(addr) != 0 &&
                   Foundation::utf8(cx, proc.mem().read<uint32_t>(addr)) == expected;
        };
        bool okHttp = hasValue(addrHttp, "HTTP/1.1");
        bool okProxyFtp = hasValue(addrProxyFtp, "kCFProxyTypeFTP");
        bool okProxyJs = hasValue(addrProxyJs, "kCFProxyTypeAutoConfigurationJavaScript");

        check(okHttp && okProxyFtp && okProxyJs, "dlsym(RTLD_DEFAULT) resolves dynamic CFNetwork and proxy constants");
        check(callDlsym("kHVMDefinitelyUnknownType") == 0, "dlsym returns 0 for an unknown constant-like name");
        check(callDlsym("OBJC_CLASS_$_HVMDefinitelyUnknown") == 0, "dlsym returns 0 for an unknown class");
        check(callDlsym("hvmDefinitelyUnknownFunction") == 0, "dlsym returns 0 for an unknown function");
        const uint32_t accuracy = callDlsym("kCLLocationAccuracyBest");
        check(accuracy && proc.mem().read<double>(accuracy) == -1,
              "dlsym resolves a recognized numeric framework constant");
    }
    {
        ProcessConfig cfg;
        cfg.homePath = "/tmp/hvm-symbol-resolution-tests";
        Process proc(cfg);
        std::atomic<int> calls{0}, ready{0}, misses{0};
        std::atomic<bool> go{false};
        proc.addMissingSymbolResolver([&](const std::string& name, Process::MissingSymbolMode mode) -> uint32_t {
            if (mode != Process::MissingSymbolMode::DynamicLookup) return 0;
            if (name == "_HVMUnresolvedTest") { ++misses; return 0; }
            if (name != "_HVMConcurrentResolutionTest") return 0;
            const int call = ++calls;
            proc.defineData("_HVMResolutionSideEffect", 0x55);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            return 0x12340000u + uint32_t(call);
        });
        constexpr size_t count = 8;
        std::array<uint32_t, count> results{};
        std::array<std::thread, count> workers;
        for (size_t i = 0; i < count; ++i) workers[i] = std::thread([&, i] {
            const auto existing = proc.lookupSymbol("_HVMConcurrentResolutionTest");
            ++ready;
            while (!go.load()) std::this_thread::yield();
            results[i] = existing ? *existing : proc.resolveMissingSymbol("_HVMConcurrentResolutionTest");
        });
        while (ready.load() != int(count)) std::this_thread::yield();
        go = true;
        for (auto& worker : workers) worker.join();
        bool same = results[0] != 0;
        for (auto result : results) same &= result == results[0];
        check(same && calls == 1 && proc.lookupSymbol("_HVMConcurrentResolutionTest") == results[0] &&
              proc.lookupSymbol("_HVMResolutionSideEffect") == 0x55,
              "concurrent first dynamic symbol resolution publishes one address");
        check(proc.resolveMissingSymbol("_HVMUnresolvedTest") == 0 &&
              proc.resolveMissingSymbol("_HVMUnresolvedTest") == 0 && misses == 2 &&
              !proc.lookupSymbol("_HVMUnresolvedTest"),
              "unresolved dynamic symbols remain uncached");
    }
    {
        namespace fs = std::filesystem;
        const fs::path tmpRoot = fs::temp_directory_path() / ("hvm-guest-dylib-fixture-" + std::to_string(::getpid()));
        fs::remove_all(tmpRoot);
        fs::create_directories(tmpRoot);
        fs::path script = fs::path("tools/build_guest_dylib_fixture.py");
        if (!fs::exists(script)) script = fs::path(__FILE__).parent_path().parent_path().parent_path() / "tools/build_guest_dylib_fixture.py";
        const std::string buildCmd = "python3 \"" + script.string() + "\" --out \"" + (tmpRoot / "ok").string() + "\"";
        const int built = std::system(buildCmd.c_str());
        check(built == 0, "build_guest_dylib_fixture.py generates ARMv7 executable and dependent MH_DYLIB chain");

        if (built == 0) {
        const std::string appPath = (tmpRoot / "ok" / "GuestDylibFixture.app").string();
        const std::string libDir = (tmpRoot / "ok" / "guestlibs").string();

        // 1. Default guestLibs = false: does not touch guestLibDir or map dependent dylibs.
        {
            ProcessConfig cfg;
            cfg.bundlePath = appPath;
            cfg.homePath = (tmpRoot / "home-off").string();
            cfg.guestLibDir = (tmpRoot / "nonexistent-dir").string();
            cfg.log = [](LogLevel, const std::string&) {};
            try {
            Process pOff(cfg);
            pOff.load();
            check(!pOff.config().guestLibs && pOff.guestDylibs().empty() && !pOff.guestExports("_guest_a_compute").has_value(),
                  "guestLibs default OFF does not touch disk or map dependent dylibs");
            } catch (const std::exception& e) {
                std::fprintf(stderr, "guestLibs OFF load: %s\n", e.what());
                check(false, "guestLibs OFF fixture loads");
            }
        }

        // 2. guestLibs = true: recursively loads libguest_a.dylib -> libguest_b.dylib, rebases, binds, runs initializers in dependency order, and releases mappings on teardown.
        const uint32_t arenasBefore = Memory::GuestArena::liveCount();
        {
            ProcessConfig cfg;
            cfg.bundlePath = appPath;
            cfg.homePath = (tmpRoot / "home-on").string();
            cfg.guestLibs = true;
            cfg.guestLibDir = libDir;
            cfg.log = [](LogLevel, const std::string&) {};
            try {
            Process pOn(cfg);
            pOn.load();
            const auto expA = pOn.guestExports("_guest_a_compute");
            const auto expB = pOn.guestExports("_guest_b_get");
            const bool loadedOrder = pOn.guestDylibs().size() == 2 &&
                pOn.guestDylibs()[0].installName == "/usr/lib/libguest_b.dylib" &&
                pOn.guestDylibs()[1].installName == "@rpath/libguest_a.dylib" &&
                pOn.guestDylibs()[0].slide != 0 && pOn.guestDylibs()[1].slide != 0 &&
                pOn.guestDylibs()[0].highAddr <= pOn.guestDylibs()[1].lowAddr &&
                expA.has_value() && expB.has_value() &&
                pOn.mem().isMapped(*expA & ~1u) && pOn.mem().isMapped(*expB & ~1u);
            const int exitCode = pOn.run();
            check(loadedOrder && !pOn.crashed() && !pOn.tainted() && exitCode == 47,
                  "guestLibs ON maps, rebases, binds and runs dependent MH_DYLIB initializers in topological order before main");
            } catch (const std::exception& e) {
                std::fprintf(stderr, "guestLibs ON load: %s\n", e.what());
                check(false, "guestLibs ON fixture loads and runs");
            }
        }
        check(Memory::GuestArena::waitForRelease(2.0, arenasBefore),
              "Process teardown releases the guest arena after dylib loading");

        // Mutate project-authored fixtures; expected outcomes follow the load contract.
        auto scenario = [&](const std::string& name, auto mutate, auto verify) {
            const auto root = tmpRoot / name;
            fs::copy(tmpRoot / "ok", root, fs::copy_options::recursive);
            mutate(root);
            ProcessConfig cfg;
            cfg.bundlePath = (root / "GuestDylibFixture.app").string();
            cfg.guestLibDir = (root / "guestlibs").string();
            cfg.homePath = (root / "home").string();
            cfg.guestLibs = true;
            cfg.log = [](LogLevel, const std::string&) {};
            Process proc(cfg);
            std::string error;
            try { proc.load(); } catch (const std::exception& e) { error = e.what(); }
            check(verify(proc, error), name.c_str());
        };
        auto editCommands = [](const fs::path& path, auto edit) {
            auto bytes = Loader::readFile(path.string());
            auto get = [&](size_t off) { uint32_t v; std::memcpy(&v, bytes.data() + off, 4); return v; };
            auto put = [&](size_t off, uint32_t v) { std::memcpy(bytes.data() + off, &v, 4); };
            for (size_t off = 28, i = 0; i < get(16); off += get(off + 4), ++i)
                if (get(off) == 1) edit(bytes, off, get, put);
            std::ofstream out(path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        };
        scenario("guest dylib symlink cannot escape configured roots", [&](const fs::path& root) {
            fs::remove(root / "guestlibs/libguest_a.dylib");
            fs::create_symlink(tmpRoot / "ok/guestlibs/libguest_a.dylib", root / "guestlibs/libguest_a.dylib");
        }, [](Process&, const std::string& error) { return error.find("Missing guest dylib") != std::string::npos; });
        scenario("guest dylib Objective-C metadata reports unsupported feature", [&](const fs::path& root) {
            editCommands(root / "guestlibs/libguest_b.dylib", [](auto& bytes, size_t off, auto get, auto) {
                if (get(off + 48)) {
                    // Replace the first data section name without changing its payload.
                    if (std::memcmp(bytes.data() + off + 8, "__DATA", 6) == 0)
                        std::memcpy(bytes.data() + off + 56, "__objc_selrefs\0\0", 16);
                }
            });
        }, [](Process& proc, const std::string& error) { return error.empty() && proc.tainted(); });
        scenario("large guest dylib avoids static arena using actual image span", [&](const fs::path& root) {
            editCommands(root / "GuestDylibFixture.app/GuestDylibFixture", [](auto& bytes, size_t off, auto, auto put) {
                if (std::memcmp(bytes.data() + off + 8, "__LINKEDIT", 10) == 0) put(off + 24, 0x0d000000u);
            });
            editCommands(root / "guestlibs/libguest_b.dylib", [](auto& bytes, size_t off, auto, auto put) {
                if (std::memcmp(bytes.data() + off + 8, "__LINKEDIT", 10) == 0) put(off + 28, 0x01100000u);
            });
        }, [](Process& proc, const std::string& error) {
            return error.empty() && proc.guestDylibs().size() == 2 && proc.guestDylibs()[0].lowAddr >= 0x80000000u;
        });

        // 3. Missing dependent dylib when guestLibs = true fails with an honest error naming the dylib.
        {
            fs::remove(tmpRoot / "ok" / "guestlibs" / "libguest_b.dylib");
            ProcessConfig cfg;
            cfg.bundlePath = appPath;
            cfg.homePath = (tmpRoot / "home-missing").string();
            cfg.guestLibs = true;
            cfg.guestLibDir = libDir;
            cfg.log = [](LogLevel, const std::string&) {};
            Process pMissing(cfg);
            std::string err;
            try { pMissing.load(); } catch (const std::exception& e) { err = e.what(); }
            check(err.find("libguest_b.dylib") != std::string::npos,
                  "guestLibs ON fails with an honest error naming a missing transitive MH_DYLIB");
        }

        // 4. Malformed dylib when guestLibs = true fails with an honest error naming the dylib.
        {
            const std::string badCmd = "python3 \"" + script.string() + "\" --out \"" + (tmpRoot / "bad").string() + "\" --malformed";
            const int badBuilt = std::system(badCmd.c_str());
            ProcessConfig cfg;
            cfg.bundlePath = (tmpRoot / "bad" / "GuestDylibFixture.app").string();
            cfg.homePath = (tmpRoot / "home-bad").string();
            cfg.guestLibs = true;
            cfg.guestLibDir = (tmpRoot / "bad" / "guestlibs").string();
            cfg.log = [](LogLevel, const std::string&) {};
            Process pBad(cfg);
            std::string err;
            try { pBad.load(); } catch (const std::exception& e) { err = e.what(); }
            check(badBuilt == 0 && err.find("libguest_a.dylib") != std::string::npos,
                  "guestLibs ON fails with an honest error naming a malformed MH_DYLIB");
        }
        }
        fs::remove_all(tmpRoot);
    }

    check(runThreadLifetimeTests() == 0, "Process owns native worker lifetime until arena release");
    check(runHostFaultTests() == 0, "host-fault bundle contains complete thread, arena, HLE and log evidence");
    check(runThreadProgressTests() == 0, "race-safe per-thread instruction and HLE wait snapshots");
    check(runGuestCallBoundaryTests() == 0, "guest-call stack prewrites stay inside writable guest pages");
    check(runSuspendTests() == 0, "a suspended Cpu stays out of guest code, including across back-to-back suspensions");
    std::printf("fulltest: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}

} // namespace


// `hvm_tests stopprobe`: block one guest thread in each blocking primitive, call
// requestStop(), and time how long the thread takes to leave it. A primitive that
// never wakes is reported STUCK (exit 1). Not part of fulltest: it is a measurement
// that names the primitives still ignoring a stop (T1, #234), and it takes a few seconds.
static int runStopProbe() {
    using namespace std::chrono;
    using namespace Hydrogen::Runtime;
    struct Case { const char* name; std::function<void(Process&, Ctx&)> body; };
    int pipeFds[2]; ::pipe(pipeFds);
    int sp[2]; ::socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
    const char* lockPath = "/tmp/hvm_stopprobe.lock";
    const int lockHeld = ::open(lockPath, O_CREAT | O_RDWR, 0600); ::flock(lockHeld, LOCK_EX);
    const int lockWaiter = ::open(lockPath, O_RDWR);
    auto callG = [](Ctx& x, const char* sym, std::initializer_list<uint32_t> a) { return uint32_t(x.proc.callGuest(x.cpu, *x.proc.lookupSymbol(sym), a)); };
    std::vector<Case> cases = {
        {"usleep(5s)",        [&](Process&, Ctx& x) { callG(x, "_usleep", {5000000}); }},
        {"sleep(5)",          [&](Process&, Ctx& x) { callG(x, "_sleep", {5}); }},
        {"nanosleep(5s)",     [&](Process& p, Ctx& x) { const uint32_t ts = p.heap().allocZeroed(16); p.mem().write<uint32_t>(ts, 5); callG(x, "_nanosleep", {ts, 0}); }},
        {"read(pipe)",        [&](Process& p, Ctx& x) { callG(x, "_read", {uint32_t(pipeFds[0]), p.heap().allocZeroed(16), 1}); }},
        {"recv(socketpair)",  [&](Process& p, Ctx& x) { callG(x, "_recv", {uint32_t(sp[0]), p.heap().allocZeroed(16), 1, 0}); }},
        {"poll(pipe,-1)",     [&](Process& p, Ctx& x) { const uint32_t pf = p.heap().allocZeroed(16); p.mem().write<uint32_t>(pf, uint32_t(pipeFds[0])); p.mem().write<uint16_t>(pf + 4, 1); callG(x, "_poll", {pf, 1, 0xFFFFFFFFu}); }},
        {"select(pipe,NULL)", [&](Process& p, Ctx& x) { const uint32_t set = p.heap().allocZeroed(128); p.mem().write<uint32_t>(set, 1u << pipeFds[0]); callG(x, "_select", {uint32_t(pipeFds[0]) + 1, set, 0, 0, 0}); }},
        {"dispatch_sem_wait(forever)", [&](Process&, Ctx& x) { const uint32_t sem = callG(x, "_dispatch_semaphore_create", {0}); callG(x, "_dispatch_semaphore_wait", {sem, 0xFFFFFFFFu, 0xFFFFFFFFu}); }},
        {"mutex self-deadlock", [&](Process& p, Ctx& x) { const uint32_t m = p.heap().allocZeroed(64); callG(x, "_pthread_mutex_init", {m, 0}); callG(x, "_pthread_mutex_lock", {m}); callG(x, "_pthread_mutex_lock", {m}); }},
        {"pthread_cond_wait", [&](Process& p, Ctx& x) { const uint32_t m = p.heap().allocZeroed(64), c = p.heap().allocZeroed(64); callG(x, "_pthread_mutex_init", {m, 0}); callG(x, "_pthread_cond_init", {c, 0}); callG(x, "_pthread_mutex_lock", {m}); callG(x, "_pthread_cond_wait", {c, m}); }},
        {"flock(contended)",  [&](Process&, Ctx& x) { callG(x, "_flock", {uint32_t(lockWaiter), 2}); }},
    };
    int stuck = 0;
    for (auto& c : cases) {
        ProcessConfig cfg; cfg.homePath = "/tmp/hvm_stopprobe";
        auto* proc = new Process(cfg);
        std::atomic<bool> done{false}, entered{false};
        const uint32_t fn = proc->defineFunction("stopprobe_fn", [&](Ctx& x) {
            entered = true;
            try { c.body(*proc, x); } catch (...) {}
            done = true;
        });
        proc->startThread(fn, 0, 256u << 10, true);
        std::this_thread::sleep_for(milliseconds(300));
        const bool blockedBeforeStop = entered.load() && !done.load();
        const auto t0 = steady_clock::now();
        proc->requestStop();
        while (!done.load() && steady_clock::now() - t0 < seconds(3)) std::this_thread::sleep_for(milliseconds(5));
        const double ms = duration<double, std::milli>(steady_clock::now() - t0).count();
        if (!blockedBeforeStop) std::printf("  ?      %-28s returned before stop (did not block)\n", c.name);
        else if (done.load()) std::printf("  ok     %-28s woke in %.0f ms\n", c.name, ms);
        else {
            std::printf("  STUCK  %-28s still blocked 3000 ms after requestStop\n%s", c.name, proc->describeRunningThreads().c_str());
            ++stuck;
            continue;
        }   // leak: a stuck thread still owns the process
        delete proc;
    }
    ::write(pipeFds[1], "x", 1); ::write(sp[1], "x", 1);   // let any stuck threads go before exit
    std::printf("stopprobe: %d stuck of %zu\n", stuck, cases.size());
    return stuck ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc == 3 && (std::string(argv[1]) == "hostfault-child" || std::string(argv[1]) == "hostfault-stress-child" || std::string(argv[1]) == "hostfault-unwind-child"))
        return runHostFaultChild(std::atoi(argv[2]), std::string(argv[1]) == "hostfault-stress-child", std::string(argv[1]) == "hostfault-unwind-child");
    if (argc == 3 && std::string(argv[1]) == "guest-call-boundary-child") return runGuestCallBoundaryChild(std::atoi(argv[2]));
    if (argc == 2 && std::string(argv[1]) == "thread-lifetime-child") return runThreadLifetimeChild();
    if (argc == 4 && std::string(argv[1]) == "audio-decode") {
        std::ifstream in(argv[2], std::ios::binary);
        if (!in) return 2;
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), {});
        auto audio = Hydrogen::Runtime::Audio::decode(data.data(), data.size());
        if (audio.stereoPcm.empty()) { std::fprintf(stderr, "unsupported or invalid audio\n"); return 1; }
        std::ofstream out(argv[3], std::ios::binary);
        out.write(reinterpret_cast<const char*>(audio.stereoPcm.data()), audio.stereoPcm.size() * sizeof(int16_t));
        std::printf("rate=%d channels=%d frames=%zu seconds=%.9f\n", audio.sampleRate, audio.channels,
                    audio.stereoPcm.size()/2, audio.duration);
        return out ? 0 : 2;
    }
    if (argc >= 2 && std::string(argv[1]) == "propertytest") {
        std::string plant = "";
        if (argc != 2) {
            static const char* const kSupported[] = {"strlen", "strnlen", "strlcpy", "memcmp", "sprintf"};
            bool known = false;
            if (argc == 4 && std::string(argv[2]) == "--plant-off-by-one")
                for (const char* name : kSupported) known = known || std::string(argv[3]) == name;
            if (!known) {
                std::fprintf(stderr, "usage: hvm_tests propertytest [--plant-off-by-one strlen|strnlen|strlcpy|memcmp|sprintf]\n");
                return 2;
            }
            plant = argv[3];
        }
        int fails = runPropertyTests(plant);
        return fails == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "selftest") {
        auto r = Hydrogen::runSelfTest();
        std::printf("selftest %s: %s (%.0f MIPS)\n", r.passed ? "passed" : "FAILED", r.detail.c_str(), r.mips);
        return r.passed ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "stopprobe") return runStopProbe();
    if (argc == 2 && std::string(argv[1]) == "fulltest") {
        return runFullSubsystemTests();
    }
    if (argc == 3 && std::string(argv[1]) == "shaders") {
        try {
            int count = 0, failed = 0;
            for (auto& file : std::filesystem::directory_iterator(argv[2])) {
                auto ext = file.path().extension().string();
                if (!file.is_regular_file() || (ext != ".vert" && ext != ".frag")) continue;
                std::ifstream input(file.path());
                if (!input) throw std::runtime_error("cannot read " + file.path().string());
                std::string source((std::istreambuf_iterator<char>(input)), {});
                Hydrogen::Runtime::Shader::Program program;
                bool ok = program.compile(source, ext == ".vert");
                std::printf("%s %s: %s\n", ok ? "ok" : "FAIL", file.path().filename().string().c_str(), program.error.c_str());
                ++count; failed += !ok;
            }
            std::printf("shader corpus: %d passed, %d failed\n", count - failed, failed);
            return count && !failed ? 0 : 1;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "shader corpus error: %s\n", e.what()); return 1;
        }
    }
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s fulltest | shaders <directory> | asm <elf> <src.S>... | c <elf> <expected.txt>\n", argv[0]);
        return 2;
    }
    try {
        std::string mode = argv[1];
        if (mode == "asm") return runAsm(argv[2], std::vector<std::string>(argv + 3, argv + argc));
        if (mode == "c") return runC(argv[2], argv[3]);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 2;
    }
    return 2;
}
