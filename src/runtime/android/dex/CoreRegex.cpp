#include "runtime/android/dex/CoreRegex.hpp"
#include "runtime/android/dex/DexFile.hpp"

#include <bit>
#include <utility>

#if !defined(__APPLE__) && !defined(__ANDROID__)
#include <unicode/uregex.h>
#endif
#if defined(__ANDROID__)
#include <regex>
#endif

namespace Hydrogen::Runtime::Android::Dex {
namespace {

Value word(uint32_t bits) { return {ValueKind::Word, bits}; }
Value ref(uint32_t id) { return {ValueKind::Reference, id}; }
int32_t asInt(Value v) { return std::bit_cast<int32_t>(uint32_t(v.bits)); }
uint32_t asRef(Value v) { return uint32_t(v.bits); }

std::u16string lowerAscii(std::u16string s) {
    for (auto& c : s) if (c >= u'A' && c <= u'Z') c = char16_t(c + 32);
    return s;
}

std::u16string upperAscii(std::u16string s) {
    for (auto& c : s) if (c >= u'a' && c <= u'z') c = char16_t(c - 32);
    return s;
}

#if defined(__ANDROID__)
// The NDK's ICU has no regular expressions: java.util.regex runs on std::regex (ECMAScript) over
// UTF-32 text, with match positions mapped back to UTF-16 indices. DOTALL and COMMENTS are not
// available in that grammar; LITERAL escapes the pattern.
class StdRegexPattern final : public HostRegexPattern {
public:
    StdRegexPattern(std::u16string_view pattern, int32_t javaFlags) {
        std::wstring source = widen(pattern, nullptr);
        if (javaFlags & 0x10) {   // LITERAL
            std::wstring escaped;
            for (wchar_t c : source) { if (std::wcschr(L"\\^$.|?*+()[]{}", c)) escaped.push_back(L'\\'); escaped.push_back(c); }
            source = escaped;
        }
        auto flags = std::regex::ECMAScript;
        if (javaFlags & 0x02) flags |= std::regex::icase;
        if (javaFlags & 0x08) flags |= std::regex::multiline;
        try { re_ = std::wregex(source, flags); }
        catch (const std::regex_error&) { throw JavaException{u"Ljava/util/regex/PatternSyntaxException;"}; }
        groups_ = int32_t(re_.mark_count());
    }
    int32_t groupCount() const override { return groups_; }
    bool matches(std::u16string_view input, std::vector<RegexGroup>& groups) const override {
        std::vector<size_t> map;
        const std::wstring text = widen(input, &map);
        std::wsmatch m;
        if (!std::regex_match(text, m, re_)) return false;
        capture(m, map, groups, text.begin());
        return true;
    }
    bool find(std::u16string_view input, size_t startPos, std::vector<RegexGroup>& groups) const override {
        std::vector<size_t> map;
        const std::wstring text = widen(input, &map);
        size_t from = 0;
        while (from < map.size() - 1 && map[from] < startPos) ++from;
        std::wsmatch m;
        if (!std::regex_search(text.begin() + std::ptrdiff_t(from), text.end(), m, re_,
                               from ? std::regex_constants::match_prev_avail : std::regex_constants::match_default))
            return false;
        capture(m, map, groups, text.begin());
        return true;
    }
    std::u16string replace(std::u16string_view input, std::u16string_view replacement, bool firstOnly) const override {
        const std::wstring out = std::regex_replace(widen(input, nullptr), re_, widen(replacement, nullptr),
                                                    firstOnly ? std::regex_constants::format_first_only : std::regex_constants::format_default);
        std::u16string result;
        for (wchar_t c : out) {
            const uint32_t cp = uint32_t(c);
            if (cp > 0xFFFF) { result.push_back(char16_t(0xD800 + ((cp - 0x10000) >> 10))); result.push_back(char16_t(0xDC00 + ((cp - 0x10000) & 0x3FF))); }
            else result.push_back(char16_t(cp));
        }
        return result;
    }
private:
    // UTF-16 to UTF-32; map[i] is the UTF-16 index of code point i (and map[n] the length).
    static std::wstring widen(std::u16string_view s, std::vector<size_t>* map) {
        std::wstring out;
        for (size_t i = 0; i < s.size();) {
            if (map) map->push_back(i);
            uint32_t cp = s[i];
            if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
                i += 2;
            } else {
                ++i;
            }
            out.push_back(wchar_t(cp));
        }
        if (map) map->push_back(s.size());
        return out;
    }
    // Positions are measured from `base`, the start of the whole text.
    void capture(const std::wsmatch& m, const std::vector<size_t>& map, std::vector<RegexGroup>& groups,
                 std::wstring::const_iterator base) const {
        groups.clear();
        for (size_t i = 0; i <= size_t(groups_); ++i) {
            if (i >= m.size() || !m[i].matched) { groups.push_back({false, 0, 0}); continue; }
            const size_t start = size_t(m[i].first - base);
            const size_t end = start + size_t(m[i].length());
            groups.push_back({true, map[start], map[end]});
        }
    }
    std::wregex re_;
    int32_t groups_ = 0;
};
#endif

#if !defined(__APPLE__) && !defined(__ANDROID__)
uint32_t toIcuFlags(int32_t javaFlags) {
    uint32_t out = 0;
    if (javaFlags & 0x01) out |= UREGEX_UNIX_LINES;
    if (javaFlags & 0x02) out |= UREGEX_CASE_INSENSITIVE;
    if (javaFlags & 0x04) out |= UREGEX_COMMENTS;
    if (javaFlags & 0x08) out |= UREGEX_MULTILINE;
    if (javaFlags & 0x10) out |= UREGEX_LITERAL;
    if (javaFlags & 0x20) out |= UREGEX_DOTALL;
    return out;
}

class IcuRegexPattern final : public HostRegexPattern {
public:
    IcuRegexPattern(std::u16string_view pattern, int32_t javaFlags) {
        UErrorCode status = U_ZERO_ERROR;
        UParseError parseErr{};
        re_ = uregex_open(reinterpret_cast<const UChar*>(pattern.data()),
                          int32_t(pattern.size()), toIcuFlags(javaFlags), &parseErr, &status);
        if (U_FAILURE(status) || !re_) {
            if (re_) uregex_close(re_);
            throw JavaException{u"Ljava/util/regex/PatternSyntaxException;"};
        }
        uregex_setTimeLimit(re_, 4000, &status);
        uregex_setStackLimit(re_, 8 << 20, &status);
        status = U_ZERO_ERROR;
        groups_ = uregex_groupCount(re_, &status);
        if (U_FAILURE(status) || groups_ < 0) groups_ = 0;
    }

    ~IcuRegexPattern() override {
        if (re_) uregex_close(re_);
    }

    int32_t groupCount() const override { return groups_; }

    bool matches(std::u16string_view input, std::vector<RegexGroup>& groups) const override {
        std::lock_guard lock(mu_);
        UErrorCode status = U_ZERO_ERROR;
        uregex_setText(re_, reinterpret_cast<const UChar*>(input.data()), int32_t(input.size()), &status);
        if (U_FAILURE(status)) return false;
        const UBool ok = uregex_matches(re_, 0, &status);
        if (U_FAILURE(status) || !ok) return false;
        captureGroupsLocked(groups);
        return true;
    }

    bool find(std::u16string_view input, size_t startPos, std::vector<RegexGroup>& groups) const override {
        std::lock_guard lock(mu_);
        UErrorCode status = U_ZERO_ERROR;
        uregex_setText(re_, reinterpret_cast<const UChar*>(input.data()), int32_t(input.size()), &status);
        if (U_FAILURE(status)) return false;
        const UBool ok = uregex_find(re_, int32_t(startPos), &status);
        if (U_FAILURE(status) || !ok) return false;
        captureGroupsLocked(groups);
        return true;
    }

    std::u16string replace(std::u16string_view input, std::u16string_view replacement,
                           bool firstOnly) const override {
        std::lock_guard lock(mu_);
        UErrorCode status = U_ZERO_ERROR;
        uregex_setText(re_, reinterpret_cast<const UChar*>(input.data()), int32_t(input.size()), &status);
        if (U_FAILURE(status)) return std::u16string(input);
        std::u16string out(std::max<size_t>(input.size() + replacement.size() + 16, 64), u'\0');
        auto runReplace = [&](UChar* dest, int32_t cap, UErrorCode* st) {
            return firstOnly
                ? uregex_replaceFirst(re_, reinterpret_cast<const UChar*>(replacement.data()),
                                      int32_t(replacement.size()), dest, cap, st)
                : uregex_replaceAll(re_, reinterpret_cast<const UChar*>(replacement.data()),
                                    int32_t(replacement.size()), dest, cap, st);
        };
        int32_t len = runReplace(reinterpret_cast<UChar*>(out.data()), int32_t(out.size()), &status);
        if (status == U_BUFFER_OVERFLOW_ERROR) {
            out.resize(size_t(len) + 1);
            status = U_ZERO_ERROR;
            len = runReplace(reinterpret_cast<UChar*>(out.data()), int32_t(out.size()), &status);
        }
        if (U_FAILURE(status) || len < 0) return std::u16string(input);
        out.resize(size_t(len));
        return out;
    }

private:
    void captureGroupsLocked(std::vector<RegexGroup>& groups) const {
        groups.clear();
        groups.reserve(size_t(groups_) + 1);
        for (int32_t i = 0; i <= groups_; ++i) {
            UErrorCode s1 = U_ZERO_ERROR, s2 = U_ZERO_ERROR;
            const int32_t start = uregex_start(re_, i, &s1);
            const int32_t end = uregex_end(re_, i, &s2);
            if (U_SUCCESS(s1) && U_SUCCESS(s2) && start >= 0 && end >= start)
                groups.push_back({true, size_t(start), size_t(end)});
            else
                groups.push_back({false, 0, 0});
        }
    }

    URegularExpression* re_ = nullptr;
    int32_t groups_ = 0;
    mutable std::mutex mu_;
};
#endif

std::vector<std::u16string> splitWithPattern(const HostRegexPattern& re, std::u16string_view input, int32_t limit) {
    std::vector<std::u16string> parts;
    const bool limited = limit > 0;
    size_t index = 0;
    size_t searchPos = 0;
    std::vector<RegexGroup> groups;
    while (searchPos <= input.size()) {
        if (limited && int32_t(parts.size()) + 1 >= limit) {
            parts.emplace_back(input.substr(index));
            return parts;
        }
        if (!re.find(input, searchPos, groups) || groups.empty() || !groups[0].matched) break;
        const size_t matchStart = groups[0].start;
        const size_t matchEnd = groups[0].end;
        // Per Java Pattern.split: ignore a zero-width match at the very beginning of input.
        if (matchStart == 0 && matchEnd == 0 && index == 0) {
            searchPos = 1;
            continue;
        }
        parts.emplace_back(input.substr(index, matchStart - index));
        index = matchEnd;
        searchPos = (matchEnd == matchStart) ? (matchEnd + 1) : matchEnd;
    }
    if (index == 0) return {std::u16string(input)};
    parts.emplace_back(input.substr(index));
    if (limit == 0) {
        while (parts.size() > 1 && parts.back().empty()) parts.pop_back();
    }
    return parts;
}

} // namespace

#if defined(__ANDROID__)
std::shared_ptr<const HostRegexPattern> compileHostRegex(std::u16string_view pattern, int32_t javaFlags) {
    return std::make_shared<StdRegexPattern>(pattern, javaFlags);
}
#elif !defined(__APPLE__)
std::shared_ptr<const HostRegexPattern> compileHostRegex(std::u16string_view pattern, int32_t javaFlags) {
    return std::make_shared<IcuRegexPattern>(pattern, javaFlags);
}
#endif

bool CoreRegex::hasClass(std::u16string_view d) {
    return d == u"Ljava/util/regex/Pattern;" || d == u"Ljava/util/regex/Matcher;" ||
           d == u"Ljava/util/regex/PatternSyntaxException;" || d == u"Ljava/util/Locale;";
}

std::vector<std::u16string> CoreRegex::splitString(std::u16string_view input,
                                                   std::u16string_view pattern, int32_t limit) {
    const auto re = compileHostRegex(pattern, 0);
    return splitWithPattern(*re, input, limit);
}

std::u16string CoreRegex::replaceRegex(std::u16string_view input, std::u16string_view pattern,
                                       std::u16string_view replacement, bool firstOnly) {
    const auto re = compileHostRegex(pattern, 0);
    return re->replace(input, replacement, firstOnly);
}

std::optional<Value> CoreRegex::invoke(Heap& heap, CoreLang& lang, std::u16string_view owner,
                                       std::u16string_view name, std::u16string_view sig,
                                       bool isStatic, std::span<const Value> args) {
    if (owner == u"Ljava/lang/String;" && !isStatic && !args.empty()) {
        const uint32_t self = asRef(args[0]);
        if (!self) throw JavaException{u"Ljava/lang/NullPointerException;"};
        if (name == u"split" && (sig == u"(Ljava/lang/String;)[Ljava/lang/String;" ||
                                 sig == u"(Ljava/lang/String;I)[Ljava/lang/String;")) {
            const uint32_t patRef = asRef(args[1]);
            if (!patRef) throw JavaException{u"Ljava/lang/NullPointerException;"};
            const int32_t limit = args.size() == 3 ? asInt(args[2]) : 0;
            auto parts = splitString(heap.string(self), heap.string(patRef), limit);
            const uint32_t arr = heap.array(u"[Ljava/lang/String;", int32_t(parts.size()));
            for (size_t i = 0; i < parts.size(); ++i)
                heap.setElement(arr, int32_t(i), ref(heap.newString(std::move(parts[i]))));
            return ref(arr);
        }
        if ((name == u"replaceAll" || name == u"replaceFirst") &&
            sig == u"(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;" && args.size() == 3) {
            const uint32_t patRef = asRef(args[1]), replRef = asRef(args[2]);
            if (!patRef || !replRef) throw JavaException{u"Ljava/lang/NullPointerException;"};
            return ref(heap.newString(replaceRegex(heap.string(self), heap.string(patRef),
                                                   heap.string(replRef), name == u"replaceFirst")));
        }
        return std::nullopt;
    }

    if (owner == u"Ljava/util/regex/Pattern;") {
        if (isStatic) {
            if (name == u"compile" && (sig == u"(Ljava/lang/String;)Ljava/util/regex/Pattern;" ||
                                       sig == u"(Ljava/lang/String;I)Ljava/util/regex/Pattern;")) {
                const uint32_t s = asRef(args[0]);
                if (!s) throw JavaException{u"Ljava/lang/NullPointerException;"};
                const std::u16string src = heap.string(s);
                const int32_t flags = args.size() == 2 ? asInt(args[1]) : 0;
                auto compiled = compileHostRegex(src, flags);
                const uint32_t id = heap.object(u"Ljava/util/regex/Pattern;");
                std::lock_guard lock(mutex_);
                patterns_[id] = CompiledPattern{src, flags, std::move(compiled)};
                return ref(id);
            }
            if (name == u"matches" && sig == u"(Ljava/lang/String;Ljava/lang/CharSequence;)Z" && args.size() == 2) {
                const uint32_t p = asRef(args[0]), cs = asRef(args[1]);
                if (!p || !cs) throw JavaException{u"Ljava/lang/NullPointerException;"};
                const auto re = compileHostRegex(heap.string(p), 0);
                std::vector<RegexGroup> groups;
                return word(re->matches(lang.formatObject(heap, cs), groups) ? 1u : 0u);
            }
            if (name == u"quote" && sig == u"(Ljava/lang/String;)Ljava/lang/String;" && args.size() == 1) {
                const uint32_t s = asRef(args[0]);
                if (!s) throw JavaException{u"Ljava/lang/NullPointerException;"};
                return ref(heap.newString(u"\\Q" + heap.string(s) + u"\\E"));
            }
            return std::nullopt;
        }
        if (args.empty()) return std::nullopt;
        const uint32_t self = asRef(args[0]);
        if (!self) throw JavaException{u"Ljava/lang/NullPointerException;"};
        if (name == u"pattern" && sig == u"()Ljava/lang/String;") {
            std::lock_guard lock(mutex_);
            return ref(heap.newString(patterns_.at(self).source));
        }
        if (name == u"flags" && sig == u"()I") {
            std::lock_guard lock(mutex_);
            return word(uint32_t(patterns_.at(self).flags));
        }
        if (name == u"matcher" && sig == u"(Ljava/lang/CharSequence;)Ljava/util/regex/Matcher;" && args.size() == 2) {
            const uint32_t cs = asRef(args[1]);
            if (!cs) throw JavaException{u"Ljava/lang/NullPointerException;"};
            const std::u16string input = lang.formatObject(heap, cs);
            const uint32_t id = heap.object(u"Ljava/util/regex/Matcher;");
            std::lock_guard lock(mutex_);
            matchers_[id] = MatcherState{self, input, 0, false, {}};
            return ref(id);
        }
        if (name == u"split" && (sig == u"(Ljava/lang/CharSequence;)[Ljava/lang/String;" ||
                                 sig == u"(Ljava/lang/CharSequence;I)[Ljava/lang/String;")) {
            const uint32_t cs = asRef(args[1]);
            if (!cs) throw JavaException{u"Ljava/lang/NullPointerException;"};
            const std::u16string input = lang.formatObject(heap, cs);
            const int32_t limit = args.size() == 3 ? asInt(args[2]) : 0;
            std::shared_ptr<const HostRegexPattern> impl;
            {
                std::lock_guard lock(mutex_);
                impl = patterns_.at(self).impl;
            }
            auto parts = splitWithPattern(*impl, input, limit);
            const uint32_t arr = heap.array(u"[Ljava/lang/String;", int32_t(parts.size()));
            for (size_t i = 0; i < parts.size(); ++i)
                heap.setElement(arr, int32_t(i), ref(heap.newString(std::move(parts[i]))));
            return ref(arr);
        }
        return std::nullopt;
    }

    if (owner == u"Ljava/util/regex/Matcher;" && !isStatic && !args.empty()) {
        const uint32_t self = asRef(args[0]);
        if (!self) throw JavaException{u"Ljava/lang/NullPointerException;"};
        if (name == u"matches" && sig == u"()Z") {
            std::shared_ptr<const HostRegexPattern> impl;
            std::u16string input;
            {
                std::lock_guard lock(mutex_);
                const auto& st = matchers_.at(self);
                impl = patterns_.at(st.patternRef).impl;
                input = st.input;
            }
            std::vector<RegexGroup> groups;
            const bool ok = impl->matches(input, groups);
            std::lock_guard lock(mutex_);
            auto& st = matchers_.at(self);
            st.hasMatch = ok;
            st.groups = std::move(groups);
            if (ok) st.searchPos = st.input.size();
            return word(ok ? 1u : 0u);
        }
        if (name == u"find" && (sig == u"()Z" || sig == u"(I)Z")) {
            std::shared_ptr<const HostRegexPattern> impl;
            std::u16string input;
            size_t startPos = 0;
            {
                std::lock_guard lock(mutex_);
                auto& st = matchers_.at(self);
                if (sig == u"(I)Z" && args.size() == 2) {
                    const int32_t start = asInt(args[1]);
                    if (start < 0 || size_t(start) > st.input.size())
                        throw JavaException{u"Ljava/lang/IndexOutOfBoundsException;"};
                    st.searchPos = size_t(start);
                }
                if (st.searchPos > st.input.size()) { st.hasMatch = false; return word(0); }
                impl = patterns_.at(st.patternRef).impl;
                input = st.input;
                startPos = st.searchPos;
            }
            std::vector<RegexGroup> groups;
            const bool ok = impl->find(input, startPos, groups);
            std::lock_guard lock(mutex_);
            auto& st = matchers_.at(self);
            st.hasMatch = ok;
            st.groups = std::move(groups);
            if (ok && !st.groups.empty()) {
                const size_t endPos = st.groups[0].end;
                st.searchPos = (endPos == startPos) ? (endPos + 1) : endPos;
            } else {
                st.searchPos = st.input.size() + 1;
            }
            return word(ok ? 1u : 0u);
        }
        if (name == u"group" && (sig == u"()Ljava/lang/String;" || sig == u"(I)Ljava/lang/String;")) {
            const int32_t idx = (sig == u"(I)Ljava/lang/String;" && args.size() == 2) ? asInt(args[1]) : 0;
            std::optional<std::u16string> text;
            {
                std::lock_guard lock(mutex_);
                const auto& st = matchers_.at(self);
                if (!st.hasMatch) throw JavaException{u"Ljava/lang/IllegalStateException;"};
                if (idx < 0 || size_t(idx) >= st.groups.size())
                    throw JavaException{u"Ljava/lang/IndexOutOfBoundsException;"};
                const auto& g = st.groups[size_t(idx)];
                if (g.matched && g.end <= st.input.size())
                    text = st.input.substr(g.start, g.end - g.start);
            }
            return ref(text ? heap.newString(std::move(*text)) : 0u);
        }
        if (name == u"groupCount" && sig == u"()I") {
            std::lock_guard lock(mutex_);
            return word(uint32_t(patterns_.at(matchers_.at(self).patternRef).impl->groupCount()));
        }
        if ((name == u"start" || name == u"end") && (sig == u"()I" || sig == u"(I)I")) {
            const int32_t idx = (sig == u"(I)I" && args.size() == 2) ? asInt(args[1]) : 0;
            std::lock_guard lock(mutex_);
            const auto& st = matchers_.at(self);
            if (!st.hasMatch) throw JavaException{u"Ljava/lang/IllegalStateException;"};
            if (idx < 0 || size_t(idx) >= st.groups.size())
                throw JavaException{u"Ljava/lang/IndexOutOfBoundsException;"};
            const auto& g = st.groups[size_t(idx)];
            if (!g.matched) return word(uint32_t(-1));
            return word(uint32_t(name == u"start" ? g.start : g.end));
        }
        if (name == u"reset" && sig == u"()Ljava/util/regex/Matcher;") {
            std::lock_guard lock(mutex_);
            auto& st = matchers_.at(self);
            st.searchPos = 0;
            st.hasMatch = false;
            st.groups.clear();
            return ref(self);
        }
        if (name == u"reset" && sig == u"(Ljava/lang/CharSequence;)Ljava/util/regex/Matcher;" && args.size() == 2) {
            const uint32_t cs = asRef(args[1]);
            if (!cs) throw JavaException{u"Ljava/lang/NullPointerException;"};
            const std::u16string input = lang.formatObject(heap, cs);
            std::lock_guard lock(mutex_);
            auto& st = matchers_.at(self);
            st.input = input;
            st.searchPos = 0;
            st.hasMatch = false;
            st.groups.clear();
            return ref(self);
        }
        if ((name == u"replaceAll" || name == u"replaceFirst") &&
            sig == u"(Ljava/lang/String;)Ljava/lang/String;" && args.size() == 2) {
            const uint32_t r = asRef(args[1]);
            if (!r) throw JavaException{u"Ljava/lang/NullPointerException;"};
            const std::u16string repl = heap.string(r);
            std::shared_ptr<const HostRegexPattern> impl;
            std::u16string input;
            {
                std::lock_guard lock(mutex_);
                const auto& st = matchers_.at(self);
                impl = patterns_.at(st.patternRef).impl;
                input = st.input;
            }
            return ref(heap.newString(impl->replace(input, repl, name == u"replaceFirst")));
        }
        return std::nullopt;
    }

    if (owner == u"Ljava/util/Locale;") {
        if (isStatic && name == u"getDefault" && sig == u"()Ljava/util/Locale;") {
            std::lock_guard lock(mutex_);
            if (!defaultLocale_) {
                defaultLocale_ = heap.object(u"Ljava/util/Locale;");
                locales_[defaultLocale_] = {u"en", u"US", u""};
            }
            return ref(defaultLocale_);
        }
        if (isStatic || args.empty()) return std::nullopt;
        const uint32_t self = asRef(args[0]);
        if (!self) throw JavaException{u"Ljava/lang/NullPointerException;"};
        if (name == u"<init>") {
            LocaleData data;
            if (args.size() >= 2) {
                const uint32_t l = asRef(args[1]);
                if (!l) throw JavaException{u"Ljava/lang/NullPointerException;"};
                data.language = lowerAscii(heap.string(l));
            }
            if (args.size() >= 3) {
                const uint32_t c = asRef(args[2]);
                if (!c) throw JavaException{u"Ljava/lang/NullPointerException;"};
                data.country = upperAscii(heap.string(c));
            }
            if (args.size() >= 4) {
                const uint32_t v = asRef(args[3]);
                if (!v) throw JavaException{u"Ljava/lang/NullPointerException;"};
                data.variant = heap.string(v);
            }
            std::lock_guard lock(mutex_);
            locales_[self] = std::move(data);
            return Value{ValueKind::Void};
        }
        LocaleData data;
        {
            std::lock_guard lock(mutex_);
            if (const auto it = locales_.find(self); it != locales_.end()) data = it->second;
        }
        if (name == u"getLanguage" && sig == u"()Ljava/lang/String;") return ref(heap.newString(data.language));
        if (name == u"getCountry" && sig == u"()Ljava/lang/String;") return ref(heap.newString(data.country));
        if (name == u"getVariant" && sig == u"()Ljava/lang/String;") return ref(heap.newString(data.variant));
        if (name == u"toString" && sig == u"()Ljava/lang/String;") {
            std::u16string out = data.language;
            if (!data.country.empty() || !data.variant.empty()) { out += u'_'; out += data.country; }
            if (!data.variant.empty()) { out += u'_'; out += data.variant; }
            return ref(heap.newString(std::move(out)));
        }
        if (name == u"equals" && sig == u"(Ljava/lang/Object;)Z" && args.size() == 2) {
            const uint32_t other = asRef(args[1]);
            if (!other || heap.typeOf(other) != u"Ljava/util/Locale;") return word(0);
            std::lock_guard lock(mutex_);
            const auto it = locales_.find(other);
            const LocaleData empty{};
            const auto& o = (it != locales_.end()) ? it->second : empty;
            return word((data.language == o.language && data.country == o.country && data.variant == o.variant) ? 1u : 0u);
        }
        if (name == u"hashCode" && sig == u"()I") {
            uint32_t h = 0;
            for (const auto* s : {&data.language, &data.country, &data.variant})
                for (char16_t c : *s) h = h * 31u + uint32_t(c);
            return word(h);
        }
    }
    return std::nullopt;
}

} // namespace Hydrogen::Runtime::Android::Dex
