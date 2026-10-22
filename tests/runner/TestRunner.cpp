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
#include <filesystem>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
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
#include "AudioOutput.hpp"
#include <functional>
#include <map>
#include <random>
#include <vector>
namespace Hydrogen::Runtime { std::vector<uint8_t> hvmDigest(int alg, const std::string& s); }
#include "runtime/uikit/UIKit.hpp"
#include "runtime/foundation/Foundation.hpp"
#include "runtime/uikit/CoreGraphics.hpp"
#include "runtime/graphics/TrueType.hpp"
#include "runtime/graphics/Shader.hpp"
#include "TestFont.hpp"
#include <bit>
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
    {
        int sockets[2];
        const bool opened = socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets) == 0;
        bool received = false, stopped = false;
        if (opened) {
            int capacity = 8192;
            setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &capacity, sizeof capacity);
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
            stopped = output.droppedBlocks > 0 && output.nonSilentFrames > 0 && output.error == 0 &&
                      std::chrono::steady_clock::now() - began < std::chrono::milliseconds(200);
            close(sockets[0]); close(sockets[1]);
        }
        check(received && stopped, "Desktop audio transports actual mixer PCM and bounds a stalled consumer without blocking shutdown");
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
                          v[lights->slot + 3] = 2;          // lights[0].intensity
                          v[lights->slot + 5] = 0.25f;      // lights[1].color.r
                          v[lights->slot + 9] = 0.125f;     // lights[1].extra.z
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
                      if (auto* pal = p.find("pal")) { v[pal->slot] = 0.25f; v[pal->slot + 5] = 0.5f; }
                      if (auto* vf = p.find("vf")) v[vf->slot + 1] = 0.125f;
                      if (auto* bones = p.find("bones")) v[bones->slot + 16 + 2] = 0.125f; // column 0, row 2 of element 1
                  }, o) && near4(o, 0.375f, 0.5f, 0.125f, 0),
              "GLSL uniform arrays, varying arrays and matrix array elements");
        {
            bool sampled = false;
            Shader::Program program;
            const bool compiled = program.compile("uniform sampler2D tex[2];\nvoid main() { gl_FragColor = texture2D(tex[1], vec2(0.5, 0.25)); }", false);
            Shader::Values values;
            if (compiled) {
                program.initialize(values);
                if (auto* tex = program.find("tex")) values[tex->slot + 1] = 4;
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
                      if (auto* u = p.find("u")) v[u->slot + 44 * 4] = 0.5f;
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
        auto call = [&](const char* sym, std::initializer_list<uint32_t> args) { return uint32_t(proc.callGuest(cpu, *proc.lookupSymbol(sym), args)); };
        const uint32_t out = proc.heap().allocZeroed(32);
        auto refs = [&](uint32_t port) { return uint32_t(call("_mach_port_get_refs", {0x103, port, 0, out})) == 0 ? proc.mem().read<uint32_t>(out) : 0; };
        check(call("_mach_port_mod_refs", {0x103, 0x103, 0, 1}) == 0 && refs(0x103) == 2 &&
              call("_mach_port_deallocate", {0x103, 0x103}) == 0 && refs(0x103) == 1,
              "Mach task send references increment and deallocate symmetrically");
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
        auto call = [&](const char* sym, std::initializer_list<uint32_t> args) { return uint32_t(proc.callGuest(cpu, *proc.lookupSymbol(sym), args)); };
        const auto face = Graphics::TrueTypeFont::load(std::vector<uint8_t>(std::begin(kTestFont), std::end(kTestFont)));
        check(face && face->glyph('A') == 2 && face->glyph('Z') == 0 && face->ascent(20) == 16 &&
              face->descent(20) == 4 && face->advance(2, 20) == 12,
              "TrueType cmap and metrics use the original font's units per em");
        check(!Graphics::TrueTypeFont::load({0,1,0,0}) && !Graphics::TrueTypeFont::load(std::vector<uint8_t>(100,0)),
              "TrueType loading rejects truncated and invalid font headers");
        const auto data = Foundation::makeData(cx, kTestFont, sizeof kTestFont, true);
        const uint32_t provider = uint32_t(call("_CGDataProviderCreateWithCFData", {data}));
        const uint32_t cg = uint32_t(call("_CGFontCreateWithDataProvider", {provider}));
        const uint32_t ct = uint32_t(call("_CTFontCreateWithGraphicsFont", {cg, std::bit_cast<uint32_t>(20.0f), 0, 0}));
        call("_CGDataProviderRelease", {provider}); call("_CFRelease", {data}); call("_CGFontRelease", {cg});
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
        const std::string userPath = cfg.homePath + "/Library/Caches";
        const std::string localPath = cfg.homePath + "/LocalDomain/Library/Caches";
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
    r.poolPop(cpu, outerPool);
    std::printf("fulltest: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
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
    if (argc == 2 && std::string(argv[1]) == "selftest") {
        auto r = Hydrogen::runSelfTest();
        std::printf("selftest %s: %s (%.0f MIPS)\n", r.passed ? "passed" : "FAILED", r.detail.c_str(), r.mips);
        return r.passed ? 0 : 1;
    }
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
