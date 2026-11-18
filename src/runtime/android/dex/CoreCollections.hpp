// Core Java collections backed by DEX heap identities.
#pragma once
#include "runtime/android/dex/Heap.hpp"

#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace Hydrogen::Runtime::Android::Dex {

class CoreCollections {
public:
    static bool hasClass(std::u16string_view type);
    std::optional<Value> invoke(Heap& heap, std::u16string_view type,
                                std::u16string_view name, std::u16string_view signature,
                                bool isStatic, std::span<const Value> args);

private:
    std::mutex mutex_;
    std::map<uint32_t, std::vector<uint32_t>> lists_;
};

} // namespace Hydrogen::Runtime::Android::Dex
