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
#include <chrono>
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
#include "runtime/FrameHistogram.hpp"
#include "runtime/audio/Audio.hpp"
#include "AudioOutput.hpp"
#include <functional>
#include <map>
#include <random>
#include <vector>
namespace Hydrogen::Runtime { std::vector<uint8_t> hvmDigest(int alg, const std::string& s); }
#include "runtime/libc/HostCrypto.hpp"
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
            check(maxVaryings == 8 && linksWith(maxVaryings) && !linksWith(maxVaryings + 1),
                  "GL_MAX_VARYING_VECTORS (8) matches the linker: 8 vec4 varyings link, 9 do not");
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
        call("__ZNSs6resizeEm", {a, 2});
        const uint32_t emptyRep = call("__ZNSs4_Rep12_S_empty_repEv", {});
        const uint32_t emptyRc = m.read<uint32_t>(emptyRep + 8);
        const bool ok = shared && str(a) == "ab" && str(c) == "hello" && str(b) == ">> hello world" && pos == 9 && str(sub) == "world" && cmp < 0 && emptyRc == 0xFFFFFFFFu;
        for (uint32_t s : {a, b, c, sub}) call("__ZNSsD1Ev", {s});
        check(ok, "std::string (COW): sharing on copy, append, insert, find, substr, compare, swap, resize, emptyRep");

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
        std::filesystem::remove(tmpPath);
        check(opened != 0 && isOpen && fd >= 0 && written == 17 && closed && readBytes == 17 && readStr == "Hello basic_file!",
              "libstdc++ __basic_file<char> open, write, read, close, fd");
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
        bool mach = bound({"_mach_port_allocate", "_mach_port_insert_right", "_mach_msg", "_task_swap_exception_ports", "_thread_swap_exception_ports", "_vm_map", "_vm_protect"});
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
            call("_vm_protect", {0x103, address, 4096, 0, 3}); call("_vm_deallocate", {0x103, address, 4096});
            call("_mach_port_destroy", {0x103, port});
        }
        check(mach, "startup Mach endpoints exchange bounded messages, time out, map aligned memory and retain page permissions");
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
        audioCall("_AudioFileClose", {fileID});
        std::filesystem::remove(filePath);

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
