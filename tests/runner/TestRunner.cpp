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
#include "selftest/SelfTest.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
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
            for (const auto& ch : t.expect) if (ch.key == "fault") faultWant = &ch;
            const bool ok = h.call(c, it->second, err);
            if (faultWant) {
                if (!ok && c.exit == CPU::Exit::MemFault && c.exit_info == faultWant->value) ++pass;
                else { std::printf("FAIL %-28s expected fault at 0x%X: %s\n", t.name.c_str(), faultWant->value, ok ? "returned normally" : err.c_str()); ++fail; }
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
#include "runtime/audio/Audio.hpp"
#include <functional>
#include <map>
#include <random>
#include <vector>
namespace Hydrogen::Runtime { std::vector<uint8_t> hvmDigest(int alg, const std::string& s); }
#include "runtime/uikit/UIKit.hpp"
#include "runtime/foundation/Foundation.hpp"
#include <cmath>
#include <cstring>

namespace {

int runFullSubsystemTests() {
    using namespace Hydrogen;
    using namespace Hydrogen::Runtime;
    int pass = 0, fail = 0;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    auto check = [&](bool ok, const char* what) {
        if (ok) { std::printf("  ok   %s\n", what); ++pass; }
        else { std::printf("  FAIL %s\n", what); ++fail; }
    };

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

    // 2. Full Process + Objective-C + UIKit + OpenGL ES + Audio verification
    ProcessConfig cfg;
    cfg.bundlePath = "/tmp/hvm_test_bundle.app";
    cfg.homePath = "/tmp/hvm_test_home";
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
    const uint32_t outerPool = r.poolPush();

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

        callGL("_glGenRenderbuffers", {1, idsAddr + 4});
        const uint32_t rbo = proc.mem().read<uint32_t>(idsAddr + 4);
        callGL("_glBindRenderbuffer", {0x8D41 /* GL_RENDERBUFFER */, rbo});
        const bool rbOk = (r.send(cpu, glCtx, "renderbufferStorage:fromDrawable:", {0x8D41, glLayer}) & 0xFF) != 0;
        callGL("_glFramebufferRenderbuffer", {0x8D40, 0x8CE0 /* GL_COLOR_ATTACHMENT0 */, 0x8D41, rbo});
        const uint32_t fbStatus = uint32_t(callGL("_glCheckFramebufferStatus", {0x8D40}));
        check(rbOk && fbStatus == 0x8CD5 /* GL_FRAMEBUFFER_COMPLETE */, "EAGLContext renderbufferStorage:fromDrawable: & FBO complete");

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

        const bool presOk = (r.send(cpu, glCtx, "presentRenderbuffer:", {0x8D41}) & 0xFF) != 0;
        UIKit::compositeFrame(cx);
        check(presOk && !UIKit::ui(proc).framebuffer.rgba.empty(), "CAEAGLLayer presentRenderbuffer composites into UIKit framebuffer");
    }

    // 2f2. libstdc++ containers (GCC 4.2 ABI): red-black tree, COW std::string
    {
        auto call = [&](const char* sym, std::initializer_list<uint32_t> args) -> uint32_t {
            auto a = proc.lookupSymbol(sym);
            return a ? uint32_t(proc.callGuest(cpu, *a, args)) : 0xDEADu;
        };
        auto& m = proc.mem();
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
        const bool ok = shared && str(a) == "abc" && str(c) == "hello" && str(b) == ">> hello world" && pos == 9 && str(sub) == "world" && cmp < 0;
        for (uint32_t s : {a, b, c, sub}) call("__ZNSsD1Ev", {s});
        check(ok, "std::string (COW): sharing on copy, append, insert, find, substr, compare, swap");
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

        const ObjC::id nsData = Foundation::makeData(cx, wav.data(), uint32_t(wav.size()));
        const ObjC::Class apCls = r.getClass("AVAudioPlayer");
        const ObjC::id player = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, apCls, r.selAlloc)), "initWithData:error:", {nsData, 0}));
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

        // CommonCrypto digests against the standard "abc" vectors.
        auto hex = [](const std::vector<uint8_t>& v) { std::string h; char b[3]; for (uint8_t c : v) { std::snprintf(b, 3, "%02x", c); h += b; } return h; };
        check(hex(Runtime::hvmDigest(0, "abc")) == "900150983cd24fb0d6963f7d28e17f72" &&
              hex(Runtime::hvmDigest(1, "abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d" &&
              hex(Runtime::hvmDigest(2, "abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" &&
              hex(Runtime::hvmDigest(1, std::string(1000, 'a'))).size() == 40,
              "CC_MD5 / CC_SHA1 / CC_SHA256 match the reference digests");
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
            call("__ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEED1Ev", {oss});
            check(v == "score=-42 0xff     7", "std::ostringstream formatting (int, hex+showbase, width) and str()");
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
    }

    r.poolPop(cpu, outerPool);
    std::printf("fulltest: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "selftest") {
        auto r = Hydrogen::runSelfTest();
        std::printf("selftest %s: %s (%.0f MIPS)\n", r.passed ? "passed" : "FAILED", r.detail.c_str(), r.mips);
        return r.passed ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "fulltest") {
        return runFullSubsystemTests();
    }
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s asm <elf> <src.S>... | c <elf> <expected.txt>\n", argv[0]);
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
