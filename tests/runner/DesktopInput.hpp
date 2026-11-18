// Parser for the `touch PHASE X Y [FINGER]` and `accel X Y Z` lines of
// `hvm run --input-file` (see docs/LOCAL_TESTING.md). Header-only so hvm_tests
// checks the same code hvm runs.
#pragma once
#include "runtime/Process.hpp"
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Hydrogen::DesktopInput {

struct Touch { int phase; float x, y; unsigned finger; };

// Accepts exactly "touch", a phase (0 down, 1 move, 3 up, 4 cancel), finite X and Y and an
// optional finger 0-10 (default 1), separated by whitespace. Every field must be a whole
// token: "touch 3 1 2 two" or "touch 3 1 2 2x" is rejected rather than read as finger 1.
inline std::optional<Touch> parseTouch(std::string_view line) {
    std::vector<std::string> tok;
    for (size_t i = 0; i < line.size();) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        size_t j = i;
        while (j < line.size() && !std::isspace(static_cast<unsigned char>(line[j]))) ++j;
        if (j > i) tok.emplace_back(line.substr(i, j - i));
        i = j;
    }
    if ((tok.size() != 4 && tok.size() != 5) || tok[0] != "touch") return std::nullopt;
    auto whole = [](const std::string& s, long lo, long hi) -> std::optional<long> {
        if (s.empty() || !(std::isdigit(static_cast<unsigned char>(s[0])))) return std::nullopt;   // no sign
        errno = 0;
        char* end = nullptr;
        const long v = std::strtol(s.c_str(), &end, 10);
        if (errno || end != s.c_str() + s.size() || v < lo || v > hi) return std::nullopt;   // whole token, even past a NUL
        return v;
    };
    auto real = [](const std::string& s) -> std::optional<float> {
        errno = 0;
        char* end = nullptr;
        const float v = std::strtof(s.c_str(), &end);
        if (errno || s.empty() || end != s.c_str() + s.size() || !std::isfinite(v)) return std::nullopt;
        return v;
    };
    const auto phase = whole(tok[1], 0, 4);
    const auto x = real(tok[2]), y = real(tok[3]);
    if (!phase || *phase == 2 || !x || !y) return std::nullopt;   // 2 (stationary) is not an input phase
    unsigned finger = 1;
    if (tok.size() == 5) {
        const auto f = whole(tok[4], 0, 10);
        if (!f) return std::nullopt;
        finger = unsigned(*f);
    }
    return Touch{int(*phase), *x, *y, finger};
}

// The `hvm run --input-file` loop calls this for each line: a valid touch line is sent to
// the Process with its own finger id. Returns false when the line is not a valid touch.
inline bool applyTouch(Runtime::Process& proc, std::string_view line, bool quiet) {
    const auto t = parseTouch(line);
    if (!t) return false;
    proc.injectTouch(Runtime::TouchPhase(t->phase), t->x, t->y, t->finger);
    if (!quiet) std::fprintf(stderr, "[hvm] desktop touch %d %.1f %.1f finger %u\n", t->phase, t->x, t->y, t->finger);
    return true;
}

struct Accel { float x, y, z; };

// Accepts exactly "accel" and three finite floats (UIAcceleration, in g: x right,
// y up, z toward the user), separated by whitespace. Every field must be a whole
// token: "accel 0 -1 0 extra" or "accel 0 -1 0x" is rejected. Signs are allowed
// (a tilt is often negative). This is what Process::setAcceleration reports
// through UIAccelerometer once the host has a real reading (until then gravity
// follows the interface orientation).
inline std::optional<Accel> parseAccel(std::string_view line) {
    std::vector<std::string> tok;
    for (size_t i = 0; i < line.size();) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        size_t j = i;
        while (j < line.size() && !std::isspace(static_cast<unsigned char>(line[j]))) ++j;
        if (j > i) tok.emplace_back(line.substr(i, j - i));
        i = j;
    }
    if (tok.size() != 4 || tok[0] != "accel") return std::nullopt;
    auto real = [](const std::string& s) -> std::optional<float> {
        errno = 0;
        char* end = nullptr;
        const float v = std::strtof(s.c_str(), &end);
        if (errno || s.empty() || end != s.c_str() + s.size() || !std::isfinite(v)) return std::nullopt;
        return v;
    };
    const auto x = real(tok[1]), y = real(tok[2]), z = real(tok[3]);
    if (!x || !y || !z) return std::nullopt;
    return Accel{*x, *y, *z};
}

// The `hvm run --input-file` loop calls this for each line: a valid accel line is
// sent to the Process as a host accelerometer reading. Returns false when the
// line is not a valid accel command (so the loop can try the other commands).
inline bool applyAccel(Runtime::Process& proc, std::string_view line, bool quiet) {
    const auto a = parseAccel(line);
    if (!a) return false;
    proc.setAcceleration(a->x, a->y, a->z);
    if (!quiet) std::fprintf(stderr, "[hvm] desktop accel %.3f %.3f %.3f\n", a->x, a->y, a->z);
    return true;
}

} // namespace Hydrogen::DesktopInput
