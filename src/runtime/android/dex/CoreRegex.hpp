// java.util.regex.Pattern, java.util.regex.Matcher, and java.util.Locale for Dalvik execution.
#pragma once
#include "runtime/android/dex/CoreLang.hpp"
#include "runtime/android/dex/Heap.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Hydrogen::Runtime::Android::Dex {

struct RegexGroup {
    bool matched = false;
    size_t start = 0;
    size_t end = 0;
};

struct HostRegexPattern {
    virtual ~HostRegexPattern() = default;
    virtual int32_t groupCount() const = 0;
    virtual bool matches(std::u16string_view input, std::vector<RegexGroup>& groups) const = 0;
    virtual bool find(std::u16string_view input, size_t startPos, std::vector<RegexGroup>& groups) const = 0;
    virtual std::u16string replace(std::u16string_view input, std::u16string_view replacement,
                                   bool firstOnly) const = 0;
};

// Implemented via ICU uregex on Linux and Foundation NSRegularExpression on Apple.
std::shared_ptr<const HostRegexPattern> compileHostRegex(std::u16string_view pattern, int32_t javaFlags);

class CoreRegex {
public:
    static bool hasClass(std::u16string_view descriptor);

    std::optional<Value> invoke(Heap& heap, CoreLang& lang, std::u16string_view owner,
                                std::u16string_view name, std::u16string_view signature,
                                bool isStatic, std::span<const Value> args);

    static std::vector<std::u16string> splitString(std::u16string_view input,
                                                   std::u16string_view pattern, int32_t limit);
    static std::u16string replaceRegex(std::u16string_view input, std::u16string_view pattern,
                                       std::u16string_view replacement, bool firstOnly);

private:
    struct CompiledPattern {
        std::u16string source;
        int32_t flags = 0;
        std::shared_ptr<const HostRegexPattern> impl;
    };
    struct MatcherState {
        uint32_t patternRef = 0;
        std::u16string input;
        size_t searchPos = 0;
        bool hasMatch = false;
        std::vector<RegexGroup> groups;
    };
    struct LocaleData {
        std::u16string language;
        std::u16string country;
        std::u16string variant;
    };

    std::mutex mutex_;
    std::unordered_map<uint32_t, CompiledPattern> patterns_;
    std::unordered_map<uint32_t, MatcherState> matchers_;
    std::unordered_map<uint32_t, LocaleData> locales_;
    uint32_t defaultLocale_ = 0;
};

} // namespace Hydrogen::Runtime::Android::Dex
