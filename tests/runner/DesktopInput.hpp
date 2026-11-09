// Parser for the `touch PHASE X Y [FINGER]` line of `hvm run --input-file` (see
// docs/LOCAL_TESTING.md). Header-only so hvm_tests checks the same code hvm runs.
#pragma once
#include <cctype>
#include <cerrno>
#include <cmath>
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
        if (errno || *end || v < lo || v > hi) return std::nullopt;
        return v;
    };
    auto real = [](const std::string& s) -> std::optional<float> {
        errno = 0;
        char* end = nullptr;
        const float v = std::strtof(s.c_str(), &end);
        if (errno || end == s.c_str() || *end || !std::isfinite(v)) return std::nullopt;
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

} // namespace Hydrogen::DesktopInput
