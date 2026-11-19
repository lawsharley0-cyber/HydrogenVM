// Java SE java.lang.Math and java.lang.StrictMath static operations.
#pragma once
#include "runtime/android/dex/Heap.hpp"

#include <optional>
#include <span>
#include <string_view>

namespace Hydrogen::Runtime::Android::Dex {

class CoreMath {
public:
    static bool hasClass(std::u16string_view type);
    static std::optional<Value> invoke(std::u16string_view type,
                                       std::u16string_view name,
                                       std::u16string_view signature,
                                       bool isStatic,
                                       std::span<const Value> args);
};

} // namespace Hydrogen::Runtime::Android::Dex
