// Core java.lang.* classes for Dalvik execution (wrappers, String, StringBuilder, System, Runtime, Enum, Throwable).
#pragma once
#include "runtime/android/dex/Heap.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Hydrogen::Runtime::Android::Dex {

class CoreLang {
public:
    static bool hasClass(std::u16string_view descriptor);

    std::optional<Value> invoke(Heap& heap, std::u16string_view owner, std::u16string_view name,
                                std::u16string_view signature, bool isStatic,
                                std::span<const Value> args);

    // Optional formatting helper for Object.toString / StringBuilder.append(Object).
    std::u16string formatObject(Heap& heap, uint32_t reference);
    bool sameBoxed(Heap& heap, uint32_t lhs, uint32_t rhs);

private:
    struct EnumData {
        uint32_t nameRef = 0;
        int32_t ordinal = 0;
    };
    struct ThrowableData {
        uint32_t messageRef = 0;
        uint32_t causeRef = 0;
        bool causeSet = false;
    };

    std::optional<Value> boxedMethod(Heap& heap, std::u16string_view owner, std::u16string_view name,
                                     std::u16string_view signature, bool isStatic,
                                     std::span<const Value> args);
    std::optional<Value> stringMethod(Heap& heap, std::u16string_view name,
                                      std::u16string_view signature, bool isStatic,
                                      std::span<const Value> args);
    std::optional<Value> builderMethod(Heap& heap, std::u16string_view name,
                                       std::u16string_view signature, std::span<const Value> args);
    std::optional<Value> systemMethod(Heap& heap, std::u16string_view name,
                                      std::u16string_view signature, bool isStatic,
                                      std::span<const Value> args);
    std::optional<Value> enumMethod(Heap& heap, std::u16string_view name,
                                    std::u16string_view signature, std::span<const Value> args);
    std::optional<Value> throwableMethod(Heap& heap, std::u16string_view name,
                                         std::u16string_view signature, std::span<const Value> args);

    std::u16string charSequenceText(Heap& heap, uint32_t reference);
    std::optional<uint64_t> boxedValueLocked(uint32_t reference) const;

    std::mutex mutex_;
    std::unordered_map<uint32_t, uint64_t> boxed_;
    std::unordered_map<uint32_t, std::u16string> builders_;
    std::unordered_map<uint32_t, EnumData> enums_;
    std::unordered_map<uint32_t, ThrowableData> throwables_;
    uint32_t runtimeSingleton_ = 0;
};

} // namespace Hydrogen::Runtime::Android::Dex
