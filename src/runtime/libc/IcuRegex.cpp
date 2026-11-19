// ICU regular expressions (libicucore uregex_*) for RegexKitLite and other
// code that calls ICU directly instead of going through NSRegularExpression.
//
// The pattern and the subject text are kept as UTF-16 code units in a host
// wide string, one unit per element, so every index the API reports is a
// UTF-16 offset as ICU defines it. Matching is the host ECMAScript engine
// (std::basic_regex) after a small ICU-to-ECMAScript rewrite: leading inline
// flags (?i) (?s) (?m) (?x)/(?w) become options, \A and \z/\Z become anchors,
// and UREGEX_DOTALL rewrites "." to match line ends. Syntax the host engine
// rejects (lookbehind, possessive quantifiers, \p{...}) returns
// U_REGEX_RULE_SYNTAX, never success.
//
// Like ICU, a failure already in *status makes every call a no-op.
// uregex_setText copies the text; ICU only keeps the pointer, so callers that
// change the buffer without calling setText again see the old text.
#include "runtime/Process.hpp"
#include "runtime/Support.hpp"
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <unordered_map>

namespace Hydrogen::Runtime {
namespace {

using WStr = std::basic_string<wchar_t>;
using WRegex = std::basic_regex<wchar_t>;

constexpr int32_t U_ZERO_ERROR = 0;
constexpr int32_t U_ILLEGAL_ARGUMENT_ERROR = 1;
constexpr int32_t U_INDEX_OUTOFBOUNDS_ERROR = 8;
constexpr int32_t U_MEMORY_ALLOCATION_ERROR = 7;
constexpr int32_t U_BUFFER_OVERFLOW_ERROR = 15;
constexpr int32_t U_STRING_NOT_TERMINATED_WARNING = -124;
constexpr int32_t U_REGEX_INTERNAL_ERROR = 0x10300;
constexpr int32_t U_REGEX_RULE_SYNTAX = 0x10301;
constexpr int32_t U_REGEX_INVALID_STATE = 0x10302;

constexpr uint32_t UREGEX_CASE_INSENSITIVE = 2;
constexpr uint32_t UREGEX_COMMENTS = 4;
constexpr uint32_t UREGEX_MULTILINE = 8;
constexpr uint32_t UREGEX_DOTALL = 32;

struct Matcher {
    WStr pattern;
    uint32_t flags = 0;
    std::shared_ptr<WRegex> re;
    uint32_t groups = 0;
    WStr text;
    int32_t regionStart = 0, regionEnd = 0;
    bool matched = false;
    std::vector<std::pair<int32_t, int32_t>> spans;   // per group; -1 unmatched
    int32_t nextFrom = 0;                              // findNext start
};

struct IcuRegexState {
    explicit IcuRegexState(Process&) {}
    std::mutex mu;
    std::unordered_map<uint32_t, std::shared_ptr<Matcher>> live;
    std::unordered_map<int32_t, uint32_t> errorNames;
};

std::shared_ptr<Matcher> matcherOf(Ctx& x, uint32_t h) {
    auto& s = x.proc.service<IcuRegexState>();
    std::lock_guard<std::mutex> lk(s.mu);
    auto it = s.live.find(h);
    return it == s.live.end() ? nullptr : it->second;
}

// Returns false (and leaves *status alone) when a failure is already there.
bool statusOk(Ctx& x, GuestPtr status) {
    return !status || x.mem.read<int32_t>(status) <= U_ZERO_ERROR;
}
void setStatus(Ctx& x, GuestPtr status, int32_t code) {
    if (status) x.mem.write<int32_t>(status, code);
}

WStr readUChars(Ctx& x, GuestPtr p, int32_t len) {
    WStr out;
    if (!p) return out;
    if (len < 0) {
        for (uint32_t i = 0; i < (1u << 24); ++i) {
            const uint16_t c = x.mem.read<uint16_t>(GuestPtr(p + i * 2));
            if (!c) break;
            out.push_back(wchar_t(c));
        }
        return out;
    }
    out.resize(size_t(len));
    for (int32_t i = 0; i < len; ++i) out[size_t(i)] = wchar_t(x.mem.read<uint16_t>(GuestPtr(p + uint32_t(i) * 2)));
    return out;
}

// ICU syntax that ECMAScript spells differently. Returns false for input the
// rewrite cannot express faithfully.
bool translate(const WStr& in, uint32_t& flags, WStr& out) {
    size_t i = 0;
    // Leading inline flag groups: (?i) (?s) (?m) (?x) (?w) and combinations.
    while (i + 2 < in.size() && in[i] == L'(' && in[i + 1] == L'?') {
        size_t j = i + 2;
        uint32_t f = 0;
        while (j < in.size() && std::wstring(L"ismxw").find(in[j]) != std::wstring::npos) {
            switch (in[j]) {
            case L'i': f |= UREGEX_CASE_INSENSITIVE; break;
            case L's': f |= UREGEX_DOTALL; break;
            case L'm': f |= UREGEX_MULTILINE; break;
            case L'x': f |= UREGEX_COMMENTS; break;
            default: break;
            }
            ++j;
        }
        if (j == i + 2 || j >= in.size() || in[j] != L')') break;
        flags |= f;
        i = j + 1;
    }
    if (flags & UREGEX_COMMENTS) return false;
    bool inClass = false;
    for (; i < in.size(); ++i) {
        const wchar_t c = in[i];
        if (c == L'\\' && i + 1 < in.size()) {
            const wchar_t n = in[i + 1];
            if (!inClass && n == L'A') { out += L"^"; ++i; continue; }
            if (!inClass && (n == L'z' || n == L'Z')) { out += L"$"; ++i; continue; }
            if (n == L'Q') {   // \Q...\E literal run
                size_t e = in.find(L"\\E", i + 2);
                const size_t stop = e == WStr::npos ? in.size() : e;
                for (size_t k = i + 2; k < stop; ++k) {
                    if (std::wstring(L"\\^$.|?*+()[]{}/-").find(in[k]) != std::wstring::npos) out.push_back(L'\\');
                    out.push_back(in[k]);
                }
                i = e == WStr::npos ? in.size() : e + 1;
                continue;
            }
            if (n == L'p' || n == L'P' || n == L'G' || n == L'X' || n == L'N') return false;
            out.push_back(c);
            out.push_back(n);
            ++i;
            continue;
        }
        if (inClass) {
            if (c == L']') inClass = false;
            out.push_back(c);
            continue;
        }
        if (c == L'[') {
            inClass = true;
            out.push_back(c);
            if (i + 1 < in.size() && in[i + 1] == L'^') { out.push_back(L'^'); ++i; }
            if (i + 1 < in.size() && in[i + 1] == L']') { out += L"\\]"; ++i; }
            continue;
        }
        if (c == L'.' && (flags & UREGEX_DOTALL)) { out += L"[\\s\\S]"; continue; }
        if (c == L'(' && i + 2 < in.size() && in[i + 1] == L'?' && (in[i + 2] == L'<' || in[i + 2] == L'>')) return false;
        if ((c == L'+' || c == L'*' || c == L'?' || c == L'}') && i + 1 < in.size() && in[i + 1] == L'+') return false;
        out.push_back(c);
    }
    return true;
}

std::shared_ptr<WRegex> compile(const WStr& pattern, uint32_t& flags, uint32_t& groups) {
    WStr src;
    if (!translate(pattern, flags, src)) return nullptr;
    auto opts = std::regex_constants::ECMAScript;
    if (flags & UREGEX_CASE_INSENSITIVE) opts |= std::regex_constants::icase;
    if (flags & UREGEX_MULTILINE) opts |= std::regex_constants::multiline;
    try {
        auto re = std::make_shared<WRegex>(src, opts);
        groups = uint32_t(re->mark_count());
        return re;
    } catch (...) {
        return nullptr;
    }
}

uint32_t storeMatcher(Ctx& x, std::shared_ptr<Matcher> m) {
    const uint32_t h = x.proc.heap().allocZeroed(16);
    if (!h) return 0;
    auto& s = x.proc.service<IcuRegexState>();
    std::lock_guard<std::mutex> lk(s.mu);
    s.live[h] = std::move(m);
    return h;
}

void clearMatch(Matcher& m) {
    m.matched = false;
    m.spans.clear();
}

// Searches [from, regionEnd). anchorStart: the match must begin at from;
// whole: it must also end at regionEnd.
bool runSearch(Matcher& m, int32_t from, bool anchorStart, bool whole) {
    clearMatch(m);
    if (from < m.regionStart || from > m.regionEnd || !m.re) return false;
    const auto b = m.text.cbegin() + from, e = m.text.cbegin() + m.regionEnd;
    auto mf = std::regex_constants::match_default;
    if (from > 0) mf |= std::regex_constants::match_prev_avail;
    if (anchorStart) mf |= std::regex_constants::match_continuous;
    std::match_results<WStr::const_iterator> r;
    bool ok = false;
    try {
        ok = whole ? std::regex_match(b, e, r, *m.re, mf) : std::regex_search(b, e, r, *m.re, mf);
    } catch (...) {
        ok = false;
    }
    if (!ok) { m.nextFrom = m.regionEnd + 1; return false; }
    m.matched = true;
    for (size_t g = 0; g < r.size(); ++g) {
        if (!r[g].matched) { m.spans.push_back({-1, -1}); continue; }
        const int32_t s = from + int32_t(r.position(g));
        m.spans.push_back({s, s + int32_t(r.length(g))});
    }
    const auto [s0, e0] = m.spans[0];
    m.nextFrom = e0 == s0 ? e0 + 1 : e0;
    return true;
}

// URegularExpression* uregex_open(pattern, patternLength, flags, UParseError* pe, UErrorCode* status)
void ur_open(Ctx& x) {
    const GuestPtr pat = x.cpu.r[0];
    const int32_t patLen = int32_t(x.cpu.r[1]);
    uint32_t flags = x.cpu.r[2];
    const GuestPtr pe = x.cpu.r[3];
    const GuestPtr status = x.mem.read<uint32_t>(x.cpu.r[CPU::kSP]);
    x.cpu.r[0] = 0;
    if (!statusOk(x, status)) return;
    if (pe) {   // UParseError { int32 line, offset; UChar pre[16], post[16]; }
        for (uint32_t i = 0; i < 72; i += 4) x.mem.write<uint32_t>(pe + i, 0);
    }
    if (!pat || patLen < -1) { setStatus(x, status, U_ILLEGAL_ARGUMENT_ERROR); return; }
    auto m = std::make_shared<Matcher>();
    m->pattern = readUChars(x, pat, patLen);
    m->flags = flags;
    m->re = compile(m->pattern, flags, m->groups);
    if (!m->re) {
        if (pe) x.mem.write<int32_t>(pe + 4, -1);
        setStatus(x, status, U_REGEX_RULE_SYNTAX);
        return;
    }
    const uint32_t h = storeMatcher(x, std::move(m));
    if (!h) { setStatus(x, status, U_MEMORY_ALLOCATION_ERROR); return; }
    x.cpu.r[0] = h;
}

void ur_close(Ctx& x, uint32_t h) {
    if (!h) return;
    {
        auto& s = x.proc.service<IcuRegexState>();
        std::lock_guard<std::mutex> lk(s.mu);
        if (!s.live.erase(h)) return;
    }
    x.proc.heap().free(h);
}

uint32_t ur_clone(Ctx& x, uint32_t h, GuestPtr status) {
    if (!statusOk(x, status)) return 0;
    auto m = matcherOf(x, h);
    if (!m) { setStatus(x, status, U_ILLEGAL_ARGUMENT_ERROR); return 0; }
    auto c = std::make_shared<Matcher>();
    c->pattern = m->pattern;
    c->flags = m->flags;
    c->re = m->re;
    c->groups = m->groups;
    const uint32_t r = storeMatcher(x, std::move(c));
    if (!r) setStatus(x, status, U_MEMORY_ALLOCATION_ERROR);
    return r;
}

void ur_setText(Ctx& x, uint32_t h, GuestPtr text, int32_t len, GuestPtr status) {
    if (!statusOk(x, status)) return;
    auto m = matcherOf(x, h);
    if (!m || !text || len < -1) { setStatus(x, status, U_ILLEGAL_ARGUMENT_ERROR); return; }
    m->text = readUChars(x, text, len);
    m->regionStart = 0;
    m->regionEnd = int32_t(m->text.size());
    m->nextFrom = 0;
    clearMatch(*m);
}

void ur_reset(Ctx& x, uint32_t h, int32_t index, GuestPtr status) {
    if (!statusOk(x, status)) return;
    auto m = matcherOf(x, h);
    if (!m) { setStatus(x, status, U_ILLEGAL_ARGUMENT_ERROR); return; }
    if (index < 0 || index > int32_t(m->text.size())) { setStatus(x, status, U_INDEX_OUTOFBOUNDS_ERROR); return; }
    m->regionStart = 0;
    m->regionEnd = int32_t(m->text.size());
    m->nextFrom = index;
    clearMatch(*m);
}

void ur_setRegion(Ctx& x, uint32_t h, int32_t start, int32_t limit, GuestPtr status) {
    if (!statusOk(x, status)) return;
    auto m = matcherOf(x, h);
    if (!m) { setStatus(x, status, U_ILLEGAL_ARGUMENT_ERROR); return; }
    if (start < 0 || limit < start || limit > int32_t(m->text.size())) { setStatus(x, status, U_INDEX_OUTOFBOUNDS_ERROR); return; }
    m->regionStart = start;
    m->regionEnd = limit;
    m->nextFrom = start;
    clearMatch(*m);
}

int32_t ur_regionStart(Ctx& x, uint32_t h, GuestPtr status) {
    auto m = statusOk(x, status) ? matcherOf(x, h) : nullptr;
    return m ? m->regionStart : -1;
}
int32_t ur_regionEnd(Ctx& x, uint32_t h, GuestPtr status) {
    auto m = statusOk(x, status) ? matcherOf(x, h) : nullptr;
    return m ? m->regionEnd : -1;
}

// UBool uregex_find(re, startIndex, status): resets, then searches from
// startIndex (-1: continue like findNext).
uint32_t ur_find(Ctx& x, uint32_t h, int32_t start, GuestPtr status) {
    if (!statusOk(x, status)) return 0;
    auto m = matcherOf(x, h);
    if (!m) { setStatus(x, status, U_ILLEGAL_ARGUMENT_ERROR); return 0; }
    if (start == -1) return runSearch(*m, m->nextFrom, false, false) ? 1 : 0;
    if (start < 0 || start > int32_t(m->text.size())) { setStatus(x, status, U_INDEX_OUTOFBOUNDS_ERROR); return 0; }
    m->regionStart = 0;
    m->regionEnd = int32_t(m->text.size());
    return runSearch(*m, start, false, false) ? 1 : 0;
}
uint32_t ur_findNext(Ctx& x, uint32_t h, GuestPtr status) {
    if (!statusOk(x, status)) return 0;
    auto m = matcherOf(x, h);
    if (!m) { setStatus(x, status, U_ILLEGAL_ARGUMENT_ERROR); return 0; }
    if (m->nextFrom > m->regionEnd) { clearMatch(*m); return 0; }
    return runSearch(*m, m->nextFrom, false, false) ? 1 : 0;
}
uint32_t ur_matches(Ctx& x, uint32_t h, int32_t start, GuestPtr status) {
    if (!statusOk(x, status)) return 0;
    auto m = matcherOf(x, h);
    if (!m) { setStatus(x, status, U_ILLEGAL_ARGUMENT_ERROR); return 0; }
    const int32_t from = start == -1 ? m->regionStart : start;
    return runSearch(*m, from, true, true) ? 1 : 0;
}
uint32_t ur_lookingAt(Ctx& x, uint32_t h, int32_t start, GuestPtr status) {
    if (!statusOk(x, status)) return 0;
    auto m = matcherOf(x, h);
    if (!m) { setStatus(x, status, U_ILLEGAL_ARGUMENT_ERROR); return 0; }
    const int32_t from = start == -1 ? m->regionStart : start;
    return runSearch(*m, from, true, false) ? 1 : 0;
}

int32_t ur_groupCount(Ctx& x, uint32_t h, GuestPtr status) {
    if (!statusOk(x, status)) return 0;
    auto m = matcherOf(x, h);
    if (!m) { setStatus(x, status, U_ILLEGAL_ARGUMENT_ERROR); return 0; }
    return int32_t(m->groups);
}

// Shared by start/end/group: the span of a group of the current match.
bool groupSpan(Ctx& x, uint32_t h, int32_t group, GuestPtr status, std::pair<int32_t, int32_t>& out,
               std::shared_ptr<Matcher>* keep = nullptr) {
    if (!statusOk(x, status)) return false;
    auto m = matcherOf(x, h);
    if (!m) { setStatus(x, status, U_ILLEGAL_ARGUMENT_ERROR); return false; }
    if (!m->matched) { setStatus(x, status, U_REGEX_INVALID_STATE); return false; }
    if (group < 0 || group > int32_t(m->groups)) { setStatus(x, status, U_INDEX_OUTOFBOUNDS_ERROR); return false; }
    out = size_t(group) < m->spans.size() ? m->spans[size_t(group)] : std::pair<int32_t, int32_t>{-1, -1};
    if (keep) *keep = m;
    return true;
}
int32_t ur_start(Ctx& x, uint32_t h, int32_t group, GuestPtr status) {
    std::pair<int32_t, int32_t> s;
    return groupSpan(x, h, group, status, s) ? s.first : -1;
}
int32_t ur_end(Ctx& x, uint32_t h, int32_t group, GuestPtr status) {
    std::pair<int32_t, int32_t> s;
    return groupSpan(x, h, group, status, s) ? s.second : -1;
}
// int32_t uregex_group(re, groupNum, UChar* dest, destCapacity, status)
void ur_group(Ctx& x) {
    const uint32_t h = x.cpu.r[0];
    const int32_t group = int32_t(x.cpu.r[1]);
    const GuestPtr dest = x.cpu.r[2];
    const int32_t cap = int32_t(x.cpu.r[3]);
    const GuestPtr status = x.mem.read<uint32_t>(x.cpu.r[CPU::kSP]);
    std::pair<int32_t, int32_t> s;
    std::shared_ptr<Matcher> m;
    x.cpu.r[0] = 0;
    if (!groupSpan(x, h, group, status, s, &m)) return;
    if (cap < 0 || (cap > 0 && !dest)) { setStatus(x, status, U_ILLEGAL_ARGUMENT_ERROR); return; }
    const int32_t len = s.first < 0 ? 0 : s.second - s.first;
    const int32_t n = std::min(len, cap);
    for (int32_t i = 0; i < n; ++i)
        x.mem.write<uint16_t>(GuestPtr(dest + uint32_t(i) * 2), uint16_t(m->text[size_t(s.first + i)]));
    if (len < cap) x.mem.write<uint16_t>(GuestPtr(dest + uint32_t(len) * 2), 0);
    else if (len == cap) setStatus(x, status, U_STRING_NOT_TERMINATED_WARNING);
    else setStatus(x, status, U_BUFFER_OVERFLOW_ERROR);
    x.cpu.r[0] = uint32_t(len);
}

int32_t ur_flags(Ctx& x, uint32_t h, GuestPtr status) {
    auto m = statusOk(x, status) ? matcherOf(x, h) : nullptr;
    return m ? int32_t(m->flags) : 0;
}

int32_t u_strlenImpl(Ctx& x, GuestPtr s) {
    if (!s) return 0;
    int32_t n = 0;
    while (x.mem.read<uint16_t>(GuestPtr(s + uint32_t(n) * 2))) ++n;
    return n;
}

const char* errorName(int32_t code) {
    switch (code) {
    case U_ZERO_ERROR: return "U_ZERO_ERROR";
    case U_ILLEGAL_ARGUMENT_ERROR: return "U_ILLEGAL_ARGUMENT_ERROR";
    case 2: return "U_MISSING_RESOURCE_ERROR";
    case 3: return "U_INVALID_FORMAT_ERROR";
    case 4: return "U_FILE_ACCESS_ERROR";
    case 5: return "U_INTERNAL_PROGRAM_ERROR";
    case 6: return "U_MESSAGE_PARSE_ERROR";
    case U_MEMORY_ALLOCATION_ERROR: return "U_MEMORY_ALLOCATION_ERROR";
    case U_INDEX_OUTOFBOUNDS_ERROR: return "U_INDEX_OUTOFBOUNDS_ERROR";
    case U_BUFFER_OVERFLOW_ERROR: return "U_BUFFER_OVERFLOW_ERROR";
    case 16: return "U_UNSUPPORTED_ERROR";
    case U_STRING_NOT_TERMINATED_WARNING: return "U_STRING_NOT_TERMINATED_WARNING";
    case U_REGEX_INTERNAL_ERROR: return "U_REGEX_INTERNAL_ERROR";
    case U_REGEX_RULE_SYNTAX: return "U_REGEX_RULE_SYNTAX";
    case U_REGEX_INVALID_STATE: return "U_REGEX_INVALID_STATE";
    case 0x10303: return "U_REGEX_BAD_ESCAPE_SEQUENCE";
    case 0x10304: return "U_REGEX_PROPERTY_SYNTAX";
    case 0x10305: return "U_REGEX_UNIMPLEMENTED";
    case 0x10306: return "U_REGEX_MISMATCHED_PAREN";
    default: return "[BOGUS UErrorCode]";
    }
}
uint32_t u_errorNameImpl(Ctx& x, int32_t code) {
    auto& s = x.proc.service<IcuRegexState>();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        auto it = s.errorNames.find(code);
        if (it != s.errorNames.end()) return it->second;
    }
    const std::string name = errorName(code);
    const uint32_t a = x.proc.allocStatic(uint32_t(name.size() + 1), 4);
    x.mem.copyIn(a, name.c_str(), name.size() + 1);
    std::lock_guard<std::mutex> lk(s.mu);
    return s.errorNames.emplace(code, a).first->second;
}

} // namespace

void registerIcuRegex(Process& p) {
    const char* engine = "Host ECMAScript regex over UTF-16 code units after an ICU syntax rewrite; unsupported ICU syntax returns U_REGEX_RULE_SYNTAX";
    supportDef<&ur_open>(p, "_uregex_open", SupportStatus::Approx, engine);
    supportDef<&ur_close>(p, "_uregex_close", SupportStatus::Real, "Frees the matcher handle");
    supportDef<&ur_clone>(p, "_uregex_clone", SupportStatus::Real, "Shares the compiled pattern; the clone has no text");
    supportDef<&ur_setText>(p, "_uregex_setText", SupportStatus::Approx, "Copies the UTF-16 text (ICU keeps the caller's pointer)");
    supportDef<&ur_reset>(p, "_uregex_reset", SupportStatus::Real, "Clears the match and region; the next findNext starts at index");
    supportDef<&ur_setRegion>(p, "_uregex_setRegion", SupportStatus::Real, "Limits matching to [start, limit) in UTF-16 units");
    supportDef<&ur_regionStart>(p, "_uregex_regionStart", SupportStatus::Real, "Region start in UTF-16 units");
    supportDef<&ur_regionEnd>(p, "_uregex_regionEnd", SupportStatus::Real, "Region end in UTF-16 units");
    supportDef<&ur_find>(p, "_uregex_find", SupportStatus::Approx, engine);
    supportDef<&ur_findNext>(p, "_uregex_findNext", SupportStatus::Approx, engine);
    supportDef<&ur_matches>(p, "_uregex_matches", SupportStatus::Approx, engine);
    supportDef<&ur_lookingAt>(p, "_uregex_lookingAt", SupportStatus::Approx, engine);
    supportDef<&ur_groupCount>(p, "_uregex_groupCount", SupportStatus::Real, "Capture group count of the compiled pattern");
    supportDef<&ur_start>(p, "_uregex_start", SupportStatus::Real, "UTF-16 start of a group of the current match; -1 when it did not take part");
    supportDef<&ur_end>(p, "_uregex_end", SupportStatus::Real, "UTF-16 end of a group of the current match; -1 when it did not take part");
    supportDef<&ur_group>(p, "_uregex_group", SupportStatus::Real, "Copies a group's UTF-16 text with ICU's buffer-overflow and not-terminated rules");
    supportDef<&ur_flags>(p, "_uregex_flags", SupportStatus::Real, "Flags passed to uregex_open plus leading inline flags");
    supportDef<&u_strlenImpl>(p, "_u_strlen", SupportStatus::Real, "Length of a NUL-terminated UTF-16 string in code units");
    supportDef<&u_errorNameImpl>(p, "_u_errorName", SupportStatus::Real, "ICU error code names; unknown codes are \"[BOGUS UErrorCode]\" as in ICU");
}

} // namespace Hydrogen::Runtime
