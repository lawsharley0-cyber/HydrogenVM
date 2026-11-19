#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

namespace Hydrogen::Runtime::Android::Dex {

inline std::optional<uint32_t> dexOrdinal(std::string_view name) {
    if (name == "classes.dex") return 1;
    if (!name.starts_with("classes") || !name.ends_with(".dex")) return {};
    name = name.substr(7, name.size() - 11);
    if (name.empty() || name.front() == '0') return {};
    uint32_t ordinal = 0;
    const auto result = std::from_chars(name.data(), name.data() + name.size(), ordinal);
    if (result.ec != std::errc{} || result.ptr != name.data() + name.size() || ordinal < 2) return {};
    return ordinal;
}

} // namespace Hydrogen::Runtime::Android::Dex
