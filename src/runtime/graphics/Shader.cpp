#include "Shader.hpp"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace Hydrogen::Runtime::Shader {
namespace {

// GLSL ES 1.00 preprocessor. Directives end at a newline or at end of input, so a
// final directive with no trailing newline is still processed (the '<end>' case).
struct PpTok {
    std::string text;
    bool space = false;
    std::shared_ptr<const std::unordered_set<std::string>> skip;
};
struct Macro {
    bool functionLike = false;
    std::vector<std::string> params;
    std::vector<PpTok> body;
    bool operator==(const Macro& o) const {
        if (functionLike != o.functionLike || params != o.params || body.size() != o.body.size()) return false;
        for (size_t i = 0; i < body.size(); ++i) if (body[i].text != o.body[i].text) return false;
        return true;
    }
};

[[noreturn]] void ppFail(const std::string& why, int line) {
    throw std::runtime_error(why + " at line " + std::to_string(line));
}
bool takeUtf8Space(const std::string& s, size_t& i) {
    const auto n = s.size();
    auto b = [&](size_t o) -> unsigned char { return o < n ? static_cast<unsigned char>(s[o]) : 0; };
    const unsigned char c = b(i);
    if (c < 0x80) return false;
    if (c == 0xC2 && b(i + 1) == 0xA0) { i += 2; return true; }
    if (c == 0xE1 && b(i + 1) == 0x9A && b(i + 2) == 0x80) { i += 3; return true; }
    if (c == 0xE2 && b(i + 1) == 0x80) {
        const unsigned char d = b(i + 2);
        if ((d >= 0x80 && d <= 0x8A) || d == 0xAF) { i += 3; return true; }
    }
    if (c == 0xE2 && b(i + 1) == 0x81 && b(i + 2) == 0x9F) { i += 3; return true; }
    if (c == 0xE3 && b(i + 1) == 0x80 && b(i + 2) == 0x80) { i += 3; return true; }
    if (c == 0xEF && b(i + 1) == 0xBB && b(i + 2) == 0xBF) { i += 3; return true; }
    return false;
}
bool reservedMacroName(const std::string& name) {
    return name.starts_with("GL_") || name.find("__") != std::string::npos;
}
bool ppIdent(const std::string& s) {
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
    for (unsigned char c : s) if (!(std::isalnum(c) || c == '_')) return false;
    return true;
}
std::string spliceLines(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\\') {
            size_t j = i + 1;
            if (j < in.size() && in[j] == '\r') ++j;
            if (j < in.size() && in[j] == '\n') { i = j; continue; }
        }
        if (in[i] == '\r') {
            out.push_back('\n');
            if (i + 1 < in.size() && in[i + 1] == '\n') ++i;
            continue;
        }
        out.push_back(in[i]);
    }
    return out;
}
std::string stripComments(const std::string& source) {
    std::string clean;
    for (size_t i = 0; i < source.size();) {
        if (source.compare(i, 2, "//") == 0) { while (i < source.size() && source[i] != '\n') ++i; }
        else if (source.compare(i, 2, "/*") == 0) {
            auto end = source.find("*/", i + 2);
            if (end == std::string::npos) throw std::runtime_error("unterminated comment");
            for (; i < end + 2; ++i) clean += source[i] == '\n' ? '\n' : ' ';
        } else clean += source[i++];
    }
    return clean;
}
std::vector<PpTok> lexPp(const std::string& s) {
    std::vector<PpTok> out;
    bool space = false;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '\n') { out.push_back({"\n", false, {}}); space = false; ++i; continue; }
        if (std::isspace(c) || takeUtf8Space(s, i)) { space = true; if (std::isspace(c)) ++i; continue; }
        PpTok t; t.space = space; space = false;
        if (std::isalpha(c) || c == '_') {
            size_t st = i++;
            while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_')) ++i;
            t.text = s.substr(st, i - st);
        } else if (std::isdigit(c) || (c == '.' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
            size_t st = i++;
            bool integer = true;
            if (c == '0' && i < s.size() && (s[i] == 'x' || s[i] == 'X')) {
                ++i; while (i < s.size() && std::isxdigit(static_cast<unsigned char>(s[i]))) ++i;
            } else {
                while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.')) {
                    if (s[i] == '.') integer = false;
                    ++i;
                }
                if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
                    size_t e = i + 1;
                    if (e < s.size() && (s[e] == '+' || s[e] == '-')) ++e;
                    if (e < s.size() && std::isdigit(static_cast<unsigned char>(s[e]))) {
                        integer = false;
                        i = e; while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
                    }
                }
            }
            if (integer) while (i < s.size() && (s[i] == 'u' || s[i] == 'U' || s[i] == 'l' || s[i] == 'L')) ++i;
            t.text = s.substr(st, i - st);
        } else {
            const char* multi[] = {"##", "&&", "||", "^^", "<<", ">>", "<=", ">=", "==", "!=", "+=", "-=", "*=", "/=", "++", "--"};
            bool found = false;
            for (const char* m : multi) {
                size_t n = std::char_traits<char>::length(m);
                if (s.compare(i, n, m) == 0) { t.text = m; i += n; found = true; break; }
            }
            if (!found) { t.text.assign(1, s[i]); ++i; }
        }
        out.push_back(std::move(t));
    }
    return out;
}

struct Preprocessor {
    std::vector<PpTok> toks;
    size_t i = 0;
    int line = 1;
    bool lineStart = true;
    bool forceLine = false;
    int forced = 1;
    bool emitted = false;
    bool started = false;
    int steps = 0;
    int sourceFile = 0;
    int braces = 0;
    bool derivatives = false;
    std::vector<std::string> warnings;
    struct Frame { bool parent = true, on = true, any = false, hadElse = false; };
    std::vector<Frame> stack;
    std::unordered_map<std::string, Macro> macros;
    std::vector<std::string> out;

    [[noreturn]] void fail(const std::string& why) const { ppFail(why, line); }
    void warn(const std::string& why) { warnings.push_back(why + " at line " + std::to_string(line)); }
    bool active() const { return stack.empty() || stack.back().on; }
    bool defined(const std::string& name) const {
        return name == "__LINE__" || name == "__FILE__" || macros.count(name) > 0;
    }
    void newline() {
        if (forceLine) { line = forced; forceLine = false; }
        else ++line;
        lineStart = true;
    }
    void emit(const PpTok& t) {
        if (t.text == "\n" || t.text.empty()) return;
        out.push_back(t.text);
        emitted = true;
        if (out.size() > 65536) fail("shader source too large");
    }
    int paramIndex(const Macro& m, const std::string& name) const {
        for (int n = 0; n < int(m.params.size()); ++n) if (m.params[size_t(n)] == name) return n;
        return -1;
    }
    std::vector<std::vector<PpTok>> collectArgs(const std::vector<PpTok>& in, size_t& k, int nparams) {
        if (k >= in.size() || in[k].text != "(") fail("unterminated macro invocation");
        ++k;
        std::vector<std::vector<PpTok>> args;
        if (nparams == 0) {
            if (k >= in.size() || in[k].text != ")") fail("macro argument count");
            ++k; return args;
        }
        std::vector<PpTok> cur;
        int depth = 0;
        for (; k < in.size(); ++k) {
            const auto& t = in[k].text;
            if (t == "\n") continue;
            // ES 1.00 / C: only parentheses group macro arguments. Commas inside
            // [] and {} separate arguments.
            if (t == "(") { ++depth; cur.push_back(in[k]); continue; }
            if (t == ")") {
                if (depth == 0) {
                    args.push_back(std::move(cur)); ++k;
                    if (int(args.size()) != nparams) fail("macro argument count");
                    return args;
                }
                --depth; cur.push_back(in[k]); continue;
            }
            if (t == "," && depth == 0) {
                args.push_back(std::move(cur)); cur.clear();
                if (int(args.size()) >= nparams) fail("macro argument count");
                continue;
            }
            cur.push_back(in[k]);
        }
        fail("unterminated macro invocation");
    }
    std::vector<PpTok> substitute(const Macro& m, const std::vector<std::vector<PpTok>>& raw, const std::vector<std::vector<PpTok>>& expanded) {
        std::vector<PpTok> outToks;
        for (size_t n = 0; n < m.body.size();) {
            if (m.body[n].text == "##") {
                if (n + 1 >= m.body.size()) fail("invalid ##");
                ++n;
                int p = paramIndex(m, m.body[n].text);
                const auto& rhs = p >= 0 ? raw[size_t(p)] : std::vector<PpTok>{m.body[n]};
                if (!rhs.empty()) {
                    if (outToks.empty()) outToks = rhs;
                    else {
                        outToks.back().text += rhs.front().text;
                        outToks.insert(outToks.end(), rhs.begin() + 1, rhs.end());
                    }
                }
                ++n; continue;
            }
            if (m.body[n].text == "#" && n + 1 < m.body.size() && paramIndex(m, m.body[n + 1].text) >= 0)
                fail("stringification is not supported");
            int p = paramIndex(m, m.body[n].text);
            if (p >= 0) {
                bool rawSide = n + 1 < m.body.size() && m.body[n + 1].text == "##";
                const auto& src = rawSide ? raw[size_t(p)] : expanded[size_t(p)];
                outToks.insert(outToks.end(), src.begin(), src.end());
                ++n; continue;
            }
            outToks.push_back(m.body[n]); ++n;
        }
        return outToks;
    }
    bool blocked(const PpTok& t, const std::string& name) const { return t.skip && t.skip->count(name) > 0; }
    std::vector<PpTok> paint(const std::vector<PpTok>& repl, const std::shared_ptr<const std::unordered_set<std::string>>& parent, const std::string& name) {
        auto skip = std::make_shared<std::unordered_set<std::string>>();
        if (parent) *skip = *parent;
        skip->insert(name);
        std::shared_ptr<const std::unordered_set<std::string>> frozen = skip;
        std::vector<PpTok> painted;
        painted.reserve(repl.size());
        for (auto t : repl) { t.skip = frozen; painted.push_back(std::move(t)); }
        return painted;
    }
    // Replacement lists are expanded one level deeper, then rescanned with the
    // following tokens at the current depth. Sibling expansions therefore share
    // a depth, so a long flat expansion hits the step limit instead of the depth limit.
    void expandSeq(const std::vector<PpTok>& in, std::vector<PpTok>& dest, int depth, int atLine) {
        if (depth > 256) fail("macro expansion too deep");
        std::vector<PpTok> stream = in;
        for (size_t k = 0; k < stream.size();) {
            if (++steps > 1000000) fail("macro expansion too long");
            if (stream[k].text == "\n") { ++atLine; ++k; continue; }
            const std::string& name = stream[k].text;
            if (name == "__LINE__") { dest.push_back({std::to_string(atLine), false, {}}); ++k; continue; }
            if (name == "__FILE__") { dest.push_back({std::to_string(sourceFile), false, {}}); ++k; continue; }
            auto it = macros.find(name);
            if (it != macros.end() && ppIdent(name) && !blocked(stream[k], name)) {
                auto splice = [&](std::vector<PpTok> repl, size_t end) {
                    std::vector<PpTok> produced;
                    expandSeq(repl, produced, depth + 1, atLine);
                    stream.erase(stream.begin() + long(k), stream.begin() + long(end));
                    stream.insert(stream.begin() + long(k), produced.begin(), produced.end());
                };
                if (it->second.functionLike) {
                    size_t j = k + 1;
                    while (j < stream.size() && stream[j].text == "\n") { ++atLine; ++j; }
                    if (j >= stream.size() || stream[j].text != "(") { dest.push_back(stream[k]); ++k; continue; }
                    size_t pos = j;
                    auto raw = collectArgs(stream, pos, int(it->second.params.size()));
                    std::vector<std::vector<PpTok>> expanded;
                    for (auto& arg : raw) {
                        std::vector<PpTok> one;
                        expandSeq(arg, one, depth + 1, atLine);
                        expanded.push_back(std::move(one));
                    }
                    splice(paint(substitute(it->second, raw, expanded), stream[k].skip, name), pos);
                    continue;
                }
                splice(paint(it->second.body, stream[k].skip, name), k + 1);
                continue;
            }
            dest.push_back(stream[k]); ++k;
        }
    }
    long parseNumber(const std::string& s) {
        if (s.empty() || s.find('.') != std::string::npos) fail("invalid preprocessor expression");
        std::string digits = s;
        while (!digits.empty() && (digits.back() == 'u' || digits.back() == 'U' || digits.back() == 'l' || digits.back() == 'L')) digits.pop_back();
        int base = 10;
        if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) base = 16;
        else if (digits.size() > 1 && digits[0] == '0') base = 8;
        if (base == 10 && (digits.find('e') != std::string::npos || digits.find('E') != std::string::npos)) fail("invalid preprocessor expression");
        char* end = nullptr;
        errno = 0;
        long v = std::strtol(digits.c_str(), &end, base);
        if (!end || *end || end == digits.c_str() || errno == ERANGE) fail("invalid preprocessor expression");
        return v;
    }
    std::vector<PpTok> replaceDefined(const std::vector<PpTok>& in) {
        std::vector<PpTok> r;
        for (size_t k = 0; k < in.size();) {
            if (in[k].text == "defined") {
                ++k; bool paren = false;
                if (k < in.size() && in[k].text == "(") { paren = true; ++k; }
                if (k >= in.size() || !ppIdent(in[k].text)) fail("invalid defined");
                bool yes = defined(in[k].text); ++k;
                if (paren) { if (k >= in.size() || in[k].text != ")") fail("invalid defined"); ++k; }
                r.push_back({yes ? "1" : "0", false, {}});
            } else r.push_back(in[k++]);
        }
        return r;
    }
    // Both operands of && and || are parsed. `live` is false for the operand that
    // C short-circuit would not evaluate, so undefined names and div/mod by zero
    // in that operand are not errors. Arithmetic wraps in uint64_t; >> is arithmetic.
    struct Eval {
        Preprocessor& pp;
        const std::vector<PpTok>& t;
        size_t k = 0;
        bool live = true;
        int depth = 0;
        [[noreturn]] void bad() { pp.fail("invalid preprocessor expression"); }
        void nest() { if (++depth > 256) pp.fail("preprocessor expression too deep"); }
        bool more() const { return k < t.size(); }
        const std::string& peek() { if (!more()) bad(); return t[k].text; }
        bool take(const char* s) { if (more() && t[k].text == s) { ++k; return true; } return false; }
        static long wrapNeg(long v) { return static_cast<long>(-static_cast<uint64_t>(v)); }
        long primary() {
            if (take("(")) { nest(); long v = expr(); --depth; if (!take(")")) bad(); return v; }
            auto s = peek(); ++k;
            if (ppIdent(s)) {
                if (live) pp.fail("undefined preprocessor identifier '" + s + "'");
                return 0;
            }
            return pp.parseNumber(s);
        }
        long unary() {
            if (take("+")) { nest(); long v = unary(); --depth; return live ? v : 0; }
            if (take("-")) { nest(); long v = unary(); --depth; return live ? wrapNeg(v) : 0; }
            if (take("!")) { nest(); long v = unary(); --depth; return live ? (v ? 0 : 1) : 0; }
            if (take("~")) { nest(); long v = unary(); --depth; return live ? static_cast<long>(~static_cast<uint64_t>(v)) : 0; }
            return primary();
        }
        long mul() {
            long v = unary();
            while (more() && (t[k].text == "*" || t[k].text == "/" || t[k].text == "%")) {
                auto op = t[k].text; ++k; long r = unary();
                if (!live) { v = 0; continue; }
                if (op == "*") v = static_cast<long>(static_cast<uint64_t>(v) * static_cast<uint64_t>(r));
                else if (r == 0) bad();
                else if (v == std::numeric_limits<long>::min() && r == -1) v = op == "/" ? v : 0;
                else if (op == "/") v /= r;
                else v %= r;
            }
            return v;
        }
        long add() {
            long v = mul();
            while (more() && (t[k].text == "+" || t[k].text == "-")) {
                auto op = t[k].text; ++k; long r = mul();
                if (!live) { v = 0; continue; }
                auto uv = static_cast<uint64_t>(v), ur = static_cast<uint64_t>(r);
                v = static_cast<long>(op == "+" ? uv + ur : uv - ur);
            }
            return v;
        }
        long shift() {
            long v = add();
            while (more() && (t[k].text == "<<" || t[k].text == ">>")) {
                auto op = t[k].text; ++k; long r = add();
                if (!live) { v = 0; continue; }
                if (r < 0 || r >= 63) bad();
                if (op == "<<") v = static_cast<long>(static_cast<uint64_t>(v) << static_cast<unsigned>(r));
                else v = v >> r;
            }
            return v;
        }
        long rel() {
            long v = shift();
            while (more() && (t[k].text == "<" || t[k].text == ">" || t[k].text == "<=" || t[k].text == ">=")) {
                auto op = t[k].text; ++k; long r = shift();
                if (!live) { v = 0; continue; }
                v = op == "<" ? v < r : op == ">" ? v > r : op == "<=" ? v <= r : v >= r;
            }
            return v;
        }
        long eq() {
            long v = rel();
            while (more() && (t[k].text == "==" || t[k].text == "!=")) {
                auto op = t[k].text; ++k; long r = rel();
                if (!live) { v = 0; continue; }
                v = op == "==" ? v == r : v != r;
            }
            return v;
        }
        long bitAnd() {
            long v = eq();
            while (take("&")) { long r = eq(); v = live ? static_cast<long>(static_cast<uint64_t>(v) & static_cast<uint64_t>(r)) : 0; }
            return v;
        }
        long bitXor() {
            long v = bitAnd();
            while (take("^")) { long r = bitAnd(); v = live ? static_cast<long>(static_cast<uint64_t>(v) ^ static_cast<uint64_t>(r)) : 0; }
            return v;
        }
        long bitOr() {
            long v = bitXor();
            while (take("|")) { long r = bitXor(); v = live ? static_cast<long>(static_cast<uint64_t>(v) | static_cast<uint64_t>(r)) : 0; }
            return v;
        }
        long land() {
            long v = bitOr();
            while (take("&&")) {
                bool saved = live;
                if (live && !v) live = false;
                long r = bitOr();
                live = saved;
                if (saved) v = (v && r) ? 1 : 0;
            }
            return v;
        }
        long lor() {
            long v = land();
            while (take("||")) {
                bool saved = live;
                if (live && v) live = false;
                long r = land();
                live = saved;
                if (saved) v = (v || r) ? 1 : 0;
            }
            return v;
        }
        long expr() { return lor(); }
    };
    long evalExpr(const std::vector<PpTok>& raw) {
        auto replaced = replaceDefined(raw);
        std::vector<PpTok> expanded; expandSeq(replaced, expanded, 0, line);
        if (expanded.empty()) fail("invalid preprocessor expression");
        Eval ev{*this, expanded, 0, true, 0};
        long value = ev.expr();
        if (ev.more()) fail("invalid preprocessor expression");
        return value;
    }
    // One integer constant expression, leaving any further tokens for the caller.
    long evalOne(const std::vector<PpTok>& expanded, size_t& k) {
        Eval ev{*this, expanded, k, true, 0};
        long value = ev.expr();
        k = ev.k;
        return value;
    }
    void pushIf(bool parent, bool cond) { stack.push_back({parent, parent && cond, parent && cond, false}); }
    void directive(const std::string& op, const std::vector<PpTok>& rest) {
        if (op == "version") {
            if (started || emitted) fail("version directive must be first");
            std::string shown;
            for (size_t n = 0; n < rest.size(); ++n) {
                if (n) shown += " ";
                shown += rest[n].text;
            }
            if (rest.size() == 1 && rest[0].text == "100") { started = true; return; }
            fail("unsupported version '" + shown + "'");
        }
        started = true;
        if (op == "if") { bool parent = active(); pushIf(parent, parent && evalExpr(rest)); return; }
        if (op == "ifdef" || op == "ifndef") {
            if (rest.size() != 1 || !ppIdent(rest[0].text)) fail("invalid #" + op);
            bool parent = active();
            bool cond = defined(rest[0].text) == (op == "ifdef");
            pushIf(parent, parent && cond); return;
        }
        if (op == "elif") {
            if (stack.empty() || stack.back().hadElse) fail("unmatched #elif");
            auto& f = stack.back();
            bool cond = f.parent && !f.any ? evalExpr(rest) != 0 : false;
            f.on = f.parent && !f.any && cond;
            if (f.on) f.any = true;
            return;
        }
        if (op == "else") {
            if (stack.empty() || stack.back().hadElse || !rest.empty()) fail("unmatched #else");
            auto& f = stack.back(); f.hadElse = true; f.on = f.parent && !f.any; f.any = true; return;
        }
        if (op == "endif") {
            if (stack.empty() || !rest.empty()) fail("unmatched #endif");
            stack.pop_back(); return;
        }
        if (!active()) return;
        if (op == "define") {
            if (rest.empty() || !ppIdent(rest[0].text) || rest[0].text == "defined") fail("invalid macro name");
            if (reservedMacroName(rest[0].text)) fail("reserved macro name");
            Macro m;
            size_t n = 1;
            if (rest.size() > 1 && rest[1].text == "(" && !rest[1].space) {
                m.functionLike = true; n = 2;
                if (n >= rest.size()) fail("invalid macro");
                if (rest[n].text != ")") {
                    while (n < rest.size()) {
                        if (!ppIdent(rest[n].text)) fail("invalid macro parameter");
                        if (std::find(m.params.begin(), m.params.end(), rest[n].text) != m.params.end()) fail("duplicate macro parameter");
                        m.params.push_back(rest[n].text); ++n;
                        if (n < rest.size() && rest[n].text == ",") { ++n; continue; }
                        break;
                    }
                }
                if (n >= rest.size() || rest[n].text != ")") fail("invalid macro");
                ++n;
            }
            m.body.assign(rest.begin() + long(n), rest.end());
            auto it = macros.find(rest[0].text);
            if (it != macros.end() && !(it->second == m)) fail("macro redefinition");
            macros[rest[0].text] = std::move(m); return;
        }
        if (op == "undef") {
            if (rest.size() != 1 || !ppIdent(rest[0].text)) fail("invalid #undef");
            if (reservedMacroName(rest[0].text)) fail("reserved macro name");
            macros.erase(rest[0].text); return;
        }
        if (op == "error") {
            std::string msg = "error";
            for (auto& t : rest) { msg += " "; msg += t.text; }
            fail(msg);
        }
        if (op == "pragma") {
            if (!rest.empty() && (rest[0].text == "optimize" || rest[0].text == "debug") && braces > 0)
                fail("pragma " + rest[0].text + " inside function");
            return;
        }
        if (op == "line") {
            std::vector<PpTok> expanded; expandSeq(rest, expanded, 0, line);
            if (expanded.empty()) fail("invalid #line");
            size_t k = 0;
            long ln = evalOne(expanded, k);
            long file = sourceFile;
            if (k < expanded.size()) file = evalOne(expanded, k);
            if (k != expanded.size() || ln < 1 || file < 0) fail("invalid #line");
            forceLine = true; forced = int(ln); sourceFile = int(file); return;
        }
        if (op == "extension") {
            if (emitted) fail("extension directive after code");
            if (rest.size() != 3 || rest[1].text != ":") fail("invalid #extension");
            const auto& name = rest[0].text; const auto& behavior = rest[2].text;
            if (behavior != "require" && behavior != "enable" && behavior != "warn" && behavior != "disable") fail("invalid extension behavior");
            if (name == "all" && (behavior == "require" || behavior == "enable")) fail("invalid extension");
            const bool deriv = name == "GL_OES_standard_derivatives";
            if (behavior == "require" && !deriv) fail("unsupported extension " + name);
            if (behavior == "warn") warn("warning: #extension " + name + " : warn");
            if (name == "all" && behavior == "disable") derivatives = false;
            else if (deriv) derivatives = behavior != "disable";
            return;
        }
        fail("unsupported preprocessor directive '#" + op + "'");
    }
    void flush(std::vector<PpTok>& chunk, int chunkLine) {
        if (chunk.empty()) return;
        std::vector<PpTok> dest;
        expandSeq(chunk, dest, 0, chunkLine);
        for (auto& t : dest) emit(t);
        chunk.clear();
    }
    std::vector<std::string> run(const std::string& source) {
        if (source.size() > 65536) fail("shader source too large");
        toks = lexPp(stripComments(spliceLines(source)));
        macros["GL_ES"] = Macro{false, {}, {{"1", false, {}}}};
        macros["__VERSION__"] = Macro{false, {}, {{"100", false, {}}}};
        // ES 1.00 section 4.5.4: defined as 1 when the fragment language supports
        // highp, in both vertex and fragment shaders. This implementation does.
        macros["GL_FRAGMENT_PRECISION_HIGH"] = Macro{false, {}, {{"1", false, {}}}};
        std::vector<PpTok> chunk;
        int chunkLine = 1;
        int paren = 0;
        while (i < toks.size()) {
            if (lineStart && toks[i].text == "#") {
                if (paren == 0) flush(chunk, chunkLine);
                ++i;
                if (i >= toks.size() || toks[i].text == "\n") {
                    started = true;
                    if (i < toks.size() && toks[i].text == "\n") { newline(); ++i; }
                    lineStart = true; continue;
                }
                std::string op = toks[i].text; ++i;
                std::vector<PpTok> rest;
                while (i < toks.size() && toks[i].text != "\n") rest.push_back(toks[i++]);
                directive(op, rest);
                if (i < toks.size() && toks[i].text == "\n") { newline(); ++i; }
                lineStart = true; continue;
            }
            if (toks[i].text == "\n") {
                if (paren == 0) flush(chunk, chunkLine);
                else if (active()) chunk.push_back(toks[i]);
                newline(); ++i; continue;
            }
            lineStart = false;
            if (!active()) { ++i; continue; }
            if (toks[i].text == "{") ++braces;
            else if (toks[i].text == "}" && braces > 0) --braces;
            if (chunk.empty()) chunkLine = line;
            if (toks[i].text == "(") ++paren;
            else if (toks[i].text == ")" && paren > 0) --paren;
            chunk.push_back(toks[i++]);
        }
        flush(chunk, chunkLine);
        if (!stack.empty()) fail("unterminated conditional");
        return out;
    }
};

std::vector<std::string> preprocessShader(const std::string& source, bool vertex, bool& derivatives, std::vector<std::string>& warnings) {
    (void)vertex;
    Preprocessor pp;
    auto tokens = pp.run(source);
    derivatives = pp.derivatives;
    warnings = std::move(pp.warnings);
    return tokens;
}

struct Expression {
    Type type;
    std::vector<int> slots;
    bool lvalue = false;
    bool isConst = false;
    float constValue = 0;
    bool needsIndex = false;
    bool direct = false;
    bool contig = false;
    bool dynamic = false;
    int origin = 0;
    int indexSlot = -1;
    int elemStride = 0;
    int elemCount = 0;
    int fragTag = 0;
    std::vector<int> dynComp;
};

struct Compiler {
    explicit Compiler(Program& program) : p(program) {}
    Program& p;
    std::vector<std::string> tokens;
    size_t at = 0;
    std::vector<std::unordered_map<std::string, size_t>> scopes;
    std::unordered_map<std::string, Type> structs;
    bool wroteOutput = false;
    int expressionDepth = 0;
    struct Param { std::string name; Type type; int qual = 0; int count = 1; bool isArray = false; };
    struct Func { std::string name; Type ret; std::vector<Param> params; size_t begin = 0, end = 0; bool defined = false; };
    struct Loop { std::vector<int> breaks, continues; int continuePc = -1; };
    struct Frame { Type ret; int result = -1; std::vector<int> returns; bool isMain = false; };
    std::vector<Func> funcs;
    std::vector<size_t> inlineStack;
    std::vector<Loop> loops;
    std::vector<Frame> frames;
    size_t mainBegin = 0, mainEnd = 0;
    bool hasMain = false;
    int stmtDepth = 0, structDepth = 0;
    bool wroteFragColor = false, wroteFragData = false;
    bool derivatives = false;
    int constDepth = 0;
    std::vector<std::string> warnings;
    // A fully folded constant. `width` is the component count (n*n for a matrix).
    struct CVal {
        Kind kind = Kind::Int;
        int width = 1;
        int matrix = 0;
        double c[16] = {};
        bool scalar() const { return matrix == 0 && width == 1; }
    };
    std::unordered_map<size_t, CVal> constVals;

    static constexpr float kPi = 3.14159265358979323846f;

    [[noreturn]] void fail(const std::string& why) const {
        throw std::runtime_error(why + " near token " + std::to_string(at) + " ('" + peek() + "')");
    }
    std::string peek() const { return at < tokens.size() ? tokens[at] : "<end>"; }
    bool take(const std::string& s) { if (peek() != s) return false; ++at; return true; }
    void need(const std::string& s) { if (!take(s)) fail("expected " + s); }
    static bool precision(const std::string& s) { return s == "lowp" || s == "mediump" || s == "highp"; }
    static bool keyword(const std::string& s) {
        static const std::unordered_set<std::string> k = {
            "attribute","const","uniform","varying","break","continue","do","for","while","if","else",
            "in","out","inout","float","int","void","bool","true","false","discard","return","mat2","mat3","mat4",
            "vec2","vec3","vec4","ivec2","ivec3","ivec4","bvec2","bvec3","bvec4","sampler2D","samplerCube",
            "struct","invariant","precision","lowp","mediump","highp"};
        return k.count(s) > 0;
    }
    static bool typeName(const std::string& s) {
        return s == "float" || s == "int" || s == "bool" || s == "vec2" || s == "vec3" || s == "vec4" ||
               s == "ivec2" || s == "ivec3" || s == "ivec4" || s == "bvec2" || s == "bvec3" || s == "bvec4" ||
               s == "mat2" || s == "mat3" || s == "mat4" || s == "sampler2D" || s == "samplerCube";
    }
    static Type ty(Kind k, int w) { Type t; t.kind = k; t.width = w; return t; }
    static Type tyMat(int n) { Type t; t.width = n * n; t.matrix = n; return t; }
    static Type tySam(bool cube) { Type t; t.sampler = true; t.cube = cube; return t; }
    static Type tyVoid() { Type t; t.width = 0; t.kind = Kind::Void; return t; }
    static Type fieldType(const Type::Field& f) {
        Type t; t.width = f.width; t.matrix = f.matrix; t.sampler = f.sampler; t.fields = f.fields; t.kind = f.kind; t.cube = f.cube; return t;
    }
    std::string identifier() {
        auto s = peek();
        if (!ppIdent(s) || keyword(s)) fail("expected identifier");
        ++at; return s;
    }
    int alloc(int n = 1) {
        if (n < 1 || p.slots + n > MaxSlots) fail("shader register limit exceeded");
        int out = p.slots; p.slots += n; return out;
    }
    // width * count, formed in 64 bits. A nested array of 2048 can overflow
    // signed int; reject that before the product is narrowed or allocated.
    int checkedSpan(int width, int count) {
        const int64_t n = int64_t(std::max(1, width)) * int64_t(count);
        if (count < 1 || n < 1 || n > int64_t(MaxSlots)) fail("shader register limit exceeded");
        return int(n);
    }
    int emit(Op op, int a = 0, int b = 0, int c = 0, float value = 0, int d = 0) {
        int dst = alloc();
        p.code.push_back({op, dst, a, b, c, value, d});
        return dst;
    }
    void mov(int dst, int src) { p.code.push_back({Op::Move, dst, src, 0, 0, 0.f, 0}); }
    int cst(float v) { return emit(Op::Constant, 0, 0, 0, v); }
    int jump(int target) { p.code.push_back({Op::Jump, 0, target, 0, 0, 0.f, 0}); return int(p.code.size()) - 1; }
    int jif(int cond) { p.code.push_back({Op::JumpIfFalse, 0, cond, 0, 0, 0.f, 0}); return int(p.code.size()) - 1; }
    void patchJ(int inst, int target) { p.code[size_t(inst)].a = target; }
    void patchF(int inst, int target) { p.code[size_t(inst)].b = target; }
    void pushScope() { scopes.push_back({}); }
    void popScope() { if (scopes.size() <= 1) fail("scope underflow"); scopes.pop_back(); }
    std::optional<size_t> findName(const std::string& n) const {
        for (int i = int(scopes.size()) - 1; i >= 0; --i) {
            auto it = scopes[size_t(i)].find(n);
            if (it != scopes[size_t(i)].end()) return it->second;
        }
        return std::nullopt;
    }
    bool funcExists(const std::string& n) const {
        for (auto& f : funcs) if (f.name == n) return true;
        return false;
    }
    Variable& declare(const std::string& name, Type t, Storage storage, int count, bool ro, bool isArray = false, bool invariant = false, bool reservedOk = false) {
        if (scopes.empty()) scopes.push_back({});
        if (scopes.back().count(name) || structs.count(name) || funcExists(name) || (name.starts_with("gl_") && !reservedOk))
            fail("duplicate/reserved variable " + name);
        if (count < 1 || count > MaxSlots) fail("array size outside supported range");
        int slot = alloc(checkedSpan(t.width, count));
        scopes.back()[name] = p.variables.size();
        p.variables.push_back(Variable{name, t, storage, slot, count, ro, isArray, invariant});
        return p.variables.back();
    }
    // Integral constant expressions (ES 1.00 §4.3.3): literals, const values,
    // parentheses, operators other than assignment and inc/dec, constructors,
    // and built-ins other than texture lookups. A non-constant leaves `at`
    // unspecified; callers rewind. Array sizes must fold to a scalar int.
    std::optional<double> convertComp(double v, Kind from, Kind to) const {
        if (to == Kind::Int && !std::isfinite(v)) return std::nullopt;
        if (from == to) return v;
        if (to == Kind::Bool) return v != 0.0 ? 1.0 : 0.0;
        if (from == Kind::Bool) return v != 0.0 ? 1.0 : 0.0;
        if (from == Kind::Float && to == Kind::Int) return std::trunc(v);
        if (from == Kind::Int && to == Kind::Float) return v;
        return std::nullopt;
    }
    std::optional<CVal> foldConstruct(const Type& t, const std::vector<CVal>& args) const {
        if (t.sampler || t.kind == Kind::Void || t.isStruct() || args.empty()) return std::nullopt;
        if (t.matrix && args.size() == 1 && args[0].scalar()) {
            auto s = convertComp(args[0].c[0], args[0].kind, Kind::Float);
            if (!s) return std::nullopt;
            CVal m; m.kind = Kind::Float; m.matrix = t.matrix; m.width = t.width;
            for (int i = 0; i < t.width; ++i) m.c[i] = (i % (t.matrix + 1) == 0) ? *s : 0;
            return m;
        }
        if (t.matrix && args.size() == 1 && args[0].matrix) {
            int n = t.matrix, m = args[0].matrix;
            CVal o; o.kind = Kind::Float; o.matrix = n; o.width = t.width;
            for (int col = 0; col < n; ++col) for (int row = 0; row < n; ++row) {
                int dst = col * n + row;
                o.c[dst] = (col < m && row < m) ? args[0].c[col * m + row] : (col == row ? 1.0 : 0.0);
            }
            return o;
        }
        if (!t.matrix && args.size() == 1 && args[0].scalar() && t.width > 1) {
            auto s = convertComp(args[0].c[0], args[0].kind, t.kind);
            if (!s) return std::nullopt;
            CVal o; o.kind = t.kind; o.width = t.width;
            for (int i = 0; i < t.width; ++i) o.c[i] = *s;
            return o;
        }
        std::vector<std::pair<double, Kind>> comps;
        for (const auto& arg : args) {
            if (arg.matrix) {
                for (int i = 0; i < arg.width; ++i) comps.push_back({arg.c[i], Kind::Float});
            } else {
                for (int i = 0; i < arg.width; ++i) comps.push_back({arg.c[i], arg.kind});
            }
        }
        if (int(comps.size()) != t.width) return std::nullopt;
        Kind want = t.matrix ? Kind::Float : t.kind;
        CVal o; o.kind = want; o.width = t.width; o.matrix = t.matrix;
        for (int i = 0; i < t.width; ++i) {
            auto c = convertComp(comps[size_t(i)].first, comps[size_t(i)].second, want);
            if (!c) return std::nullopt;
            o.c[i] = *c;
        }
        return o;
    }
    std::optional<CVal> applySwizzle(const CVal& v, const std::string& sel) const {
        if (v.matrix || sel.empty() || sel.size() > 4) return std::nullopt;
        const char* sets[] = {"xyzw", "rgba", "stpq"};
        int set = -1;
        int idx[4] = {};
        for (size_t i = 0; i < sel.size(); ++i) {
            int found = -1, which = -1;
            for (int s = 0; s < 3 && found < 0; ++s) {
                for (int k = 0; k < 4; ++k) if (sets[s][k] == sel[i]) { found = k; which = s; }
            }
            if (found < 0 || found >= v.width || (set >= 0 && set != which)) return std::nullopt;
            set = which; idx[i] = found;
        }
        CVal o; o.kind = v.kind; o.width = int(sel.size());
        for (size_t i = 0; i < sel.size(); ++i) o.c[i] = v.c[idx[i]];
        return o;
    }
    std::optional<CVal> applyIndex(const CVal& v, const CVal& idx) const {
        if (!idx.scalar() || idx.kind != Kind::Int || !std::isfinite(idx.c[0])) return std::nullopt;
        const double truncated = std::trunc(idx.c[0]);
        if (truncated != idx.c[0]) return std::nullopt;
        const int limit = v.matrix ? v.matrix : v.width;
        // Cast only after the index is known to fit. int(1e20) and int(2^31) are undefined.
        if (limit < 1 || truncated < 0.0 || truncated >= double(limit)) return std::nullopt;
        const int i = int(truncated);
        if (v.matrix) {
            CVal o; o.kind = Kind::Float; o.width = v.matrix;
            for (int row = 0; row < v.matrix; ++row) o.c[row] = v.c[i * v.matrix + row];
            return o;
        }
        CVal o; o.kind = v.kind; o.width = 1; o.c[0] = v.c[i];
        return o;
    }
    std::optional<CVal> foldBin(const CVal& a, const CVal& b, const std::string& op) const {
        if (a.kind != b.kind || a.kind == Kind::Bool || a.matrix || b.matrix) return std::nullopt;
        if (a.width != b.width && a.width != 1 && b.width != 1) return std::nullopt;
        const int n = std::max(a.width, b.width);
        CVal o; o.kind = a.kind; o.width = n;
        for (int i = 0; i < n; ++i) {
            const double x = a.c[a.width == 1 ? 0 : i];
            const double y = b.c[b.width == 1 ? 0 : i];
            double r = 0;
            if (op == "+") r = x + y;
            else if (op == "-") r = x - y;
            else if (op == "*") r = x * y;
            else {
                if (y == 0.0) return std::nullopt;
                r = a.kind == Kind::Int ? std::trunc(x / y) : x / y;
            }
            if (!std::isfinite(r)) return std::nullopt;
            o.c[i] = r;
        }
        return o;
    }
    std::optional<CVal> foldMinMax(const CVal& a, const CVal& b, bool isMin) const {
        if (a.matrix || b.matrix || a.kind != b.kind || (a.kind != Kind::Float && a.kind != Kind::Int)) return std::nullopt;
        if (a.width != b.width && b.width != 1) return std::nullopt;
        CVal o; o.kind = a.kind; o.width = a.width;
        for (int i = 0; i < a.width; ++i) {
            const double x = a.c[i], y = b.c[b.width == 1 ? 0 : i];
            o.c[i] = isMin ? std::min(x, y) : std::max(x, y);
        }
        return o;
    }
    std::optional<CVal> foldBuiltin(const std::string& name, const std::vector<CVal>& args) const {
        auto arity = [&](size_t n) { return args.size() == n; };
        auto map1 = [&](auto fn) -> std::optional<CVal> {
            if (!arity(1) || args[0].kind != Kind::Float || args[0].matrix) return std::nullopt;
            CVal o = args[0];
            for (int i = 0; i < o.width; ++i) {
                const double r = fn(o.c[i]);
                if (!std::isfinite(r)) return std::nullopt;
                o.c[i] = r;
            }
            return o;
        };
        auto zip = [&](auto fn) -> std::optional<CVal> {
            if (!arity(2) || args[0].kind != Kind::Float || args[1].kind != Kind::Float || args[0].matrix || args[1].matrix ||
                args[0].width != args[1].width) return std::nullopt;
            CVal o = args[0];
            for (int i = 0; i < o.width; ++i) {
                const double r = fn(args[0].c[i], args[1].c[i]);
                if (!std::isfinite(r)) return std::nullopt;
                o.c[i] = r;
            }
            return o;
        };
        if (name == "dFdx" || name == "dFdy" || name == "fwidth") {
            if (!derivatives || !arity(1) || args[0].kind != Kind::Float || args[0].matrix) return std::nullopt;
            CVal o = args[0];
            for (int i = 0; i < o.width; ++i) o.c[i] = 0;
            return o;
        }
        if (name == "radians") return map1([](double v) { return v * 3.14159265358979323846 / 180.0; });
        if (name == "degrees") return map1([](double v) { return v * 180.0 / 3.14159265358979323846; });
        if (name == "sin") return map1([](double v) { return std::sin(v); });
        if (name == "cos") return map1([](double v) { return std::cos(v); });
        if (name == "tan") return map1([](double v) { return std::tan(v); });
        if (name == "asin") return map1([](double v) { return std::asin(v); });
        if (name == "acos") return map1([](double v) { return std::acos(v); });
        if (name == "atan") {
            if (arity(1)) return map1([](double v) { return std::atan(v); });
            return zip([](double y, double x) { return std::atan2(y, x); });
        }
        if (name == "pow") return zip([](double x, double y) { return std::pow(x, y); });
        if (name == "exp") return map1([](double v) { return std::exp(v); });
        if (name == "log") return map1([](double v) { return v > 0 ? std::log(v) : 0; });
        if (name == "exp2") return map1([](double v) { return std::exp2(v); });
        if (name == "log2") return map1([](double v) { return v > 0 ? std::log2(v) : 0; });
        if (name == "sqrt") return map1([](double v) { return v > 0 ? std::sqrt(v) : 0; });
        if (name == "inversesqrt") return map1([](double v) { return v > 0 ? 1.0 / std::sqrt(v) : 0; });
        if (name == "floor") return map1([](double v) { return std::floor(v); });
        if (name == "ceil") return map1([](double v) { return std::ceil(v); });
        if (name == "fract") return map1([](double v) { return v - std::floor(v); });
        if (name == "abs") {
            if (!arity(1) || args[0].matrix || (args[0].kind != Kind::Float && args[0].kind != Kind::Int)) return std::nullopt;
            CVal o = args[0];
            for (int i = 0; i < o.width; ++i) o.c[i] = std::fabs(o.c[i]);
            return o;
        }
        if (name == "sign") {
            if (!arity(1) || args[0].matrix || (args[0].kind != Kind::Float && args[0].kind != Kind::Int)) return std::nullopt;
            CVal o = args[0];
            for (int i = 0; i < o.width; ++i) o.c[i] = o.c[i] > 0 ? 1 : o.c[i] < 0 ? -1 : 0;
            return o;
        }
        if (name == "mod") {
            if (!arity(2) || args[0].kind != Kind::Float || args[0].matrix || args[1].kind != Kind::Float || args[1].matrix) return std::nullopt;
            if (args[1].width != args[0].width && args[1].width != 1) return std::nullopt;
            CVal o = args[0];
            for (int i = 0; i < o.width; ++i) {
                const double y = args[1].c[args[1].width == 1 ? 0 : i];
                if (y == 0.0) return std::nullopt;
                o.c[i] = o.c[i] - y * std::floor(o.c[i] / y);
                if (!std::isfinite(o.c[i])) return std::nullopt;
            }
            return o;
        }
        if (name == "min" || name == "max") {
            if (!arity(2)) return std::nullopt;
            return foldMinMax(args[0], args[1], name == "min");
        }
        if (name == "clamp") {
            if (!arity(3)) return std::nullopt;
            const bool scalarEnds = args[1].scalar() && args[2].scalar();
            const bool same = !args[1].matrix && !args[2].matrix && args[1].width == args[0].width && args[2].width == args[0].width;
            if (!scalarEnds && !same) return std::nullopt;
            auto hi = foldMinMax(args[0], args[1], false);
            if (!hi) return std::nullopt;
            return foldMinMax(*hi, args[2], true);
        }
        if (name == "mix") {
            if (!arity(3) || args[0].kind != Kind::Float || args[0].matrix || args[0].kind != args[1].kind || args[0].width != args[1].width ||
                args[1].matrix || args[2].kind != Kind::Float || args[2].matrix) return std::nullopt;
            if (args[2].width != 1 && args[2].width != args[0].width) return std::nullopt;
            CVal o = args[0];
            for (int i = 0; i < o.width; ++i) {
                const double a = args[2].c[args[2].width == 1 ? 0 : i];
                o.c[i] = args[0].c[i] * (1.0 - a) + args[1].c[i] * a;
                if (!std::isfinite(o.c[i])) return std::nullopt;
            }
            return o;
        }
        if (name == "step") {
            if (!arity(2) || args[1].kind != Kind::Float || args[1].matrix || args[0].kind != Kind::Float || args[0].matrix) return std::nullopt;
            if (args[0].width != args[1].width && args[0].width != 1) return std::nullopt;
            CVal o; o.kind = Kind::Float; o.width = args[1].width;
            for (int i = 0; i < o.width; ++i) o.c[i] = args[1].c[i] < args[0].c[args[0].width == 1 ? 0 : i] ? 0.0 : 1.0;
            return o;
        }
        if (name == "smoothstep") {
            if (!arity(3) || args[2].kind != Kind::Float || args[2].matrix) return std::nullopt;
            const bool scalarEdge = args[0].scalar() && args[1].scalar() && args[0].kind == Kind::Float && args[1].kind == Kind::Float;
            const bool same = args[0].kind == Kind::Float && args[1].kind == Kind::Float && !args[0].matrix && !args[1].matrix &&
                              args[0].width == args[2].width && args[1].width == args[2].width;
            if (!scalarEdge && !same) return std::nullopt;
            CVal o; o.kind = Kind::Float; o.width = args[2].width;
            for (int i = 0; i < o.width; ++i) {
                const double e0 = args[0].c[args[0].width == 1 ? 0 : i];
                const double e1 = args[1].c[args[1].width == 1 ? 0 : i];
                if (e1 == e0) return std::nullopt;
                double t = (args[2].c[i] - e0) / (e1 - e0);
                t = std::clamp(t, 0.0, 1.0);
                o.c[i] = t * t * (3.0 - 2.0 * t);
            }
            return o;
        }
        if (name == "length" || name == "distance" || name == "dot") {
            if (name == "length" && arity(1) && args[0].kind == Kind::Float && !args[0].matrix) {
                double s = 0;
                for (int i = 0; i < args[0].width; ++i) s += args[0].c[i] * args[0].c[i];
                CVal o; o.kind = Kind::Float; o.c[0] = std::sqrt(s);
                return o;
            }
            if ((name == "distance" || name == "dot") && arity(2) && args[0].kind == Kind::Float && args[1].kind == Kind::Float &&
                !args[0].matrix && !args[1].matrix && args[0].width == args[1].width) {
                double s = 0;
                for (int i = 0; i < args[0].width; ++i) {
                    const double d = name == "dot" ? args[0].c[i] * args[1].c[i] : args[0].c[i] - args[1].c[i];
                    s += name == "dot" ? d : d * d;
                }
                CVal o; o.kind = Kind::Float; o.c[0] = name == "dot" ? s : std::sqrt(s);
                return o;
            }
        }
        return std::nullopt;
    }
    std::optional<CVal> withPostfix(CVal v) {
        while (peek() == "." || peek() == "[") {
            if (take(".")) {
                const std::string sel = peek();
                if (sel.empty() || !std::isalpha(static_cast<unsigned char>(sel[0]))) return std::nullopt;
                ++at;
                auto next = applySwizzle(v, sel);
                if (!next) return std::nullopt;
                v = *next;
            } else {
                ++at;
                auto idx = constTernary();
                if (!idx || !take("]")) return std::nullopt;
                auto next = applyIndex(v, *idx);
                if (!next) return std::nullopt;
                v = *next;
            }
        }
        return v;
    }
    std::vector<CVal> constArgs() {
        std::vector<CVal> args;
        if (!take("(")) return {};
        if (take(")")) return args;
        do {
            auto a = constTernary();
            if (!a) return {};
            args.push_back(*a);
        } while (take(","));
        if (!take(")")) return {};
        return args;
    }
    std::optional<CVal> constPrimary() {
        if (take("(")) {
            auto v = constTernary();
            if (!v || !take(")")) return std::nullopt;
            return withPostfix(*v);
        }
        if (take("true")) return CVal{Kind::Bool, 1, 0, {1}};
        if (take("false")) return CVal{Kind::Bool, 1, 0, {0}};
        const std::string tok = peek();
        if (!tok.empty() && (std::isdigit(static_cast<unsigned char>(tok[0])) || tok[0] == '.')) {
            const bool isFloat = tok.find('.') != std::string::npos || tok.find('e') != std::string::npos || tok.find('E') != std::string::npos;
            char* end = nullptr;
            if (!isFloat) {
                errno = 0;
                const long n = std::strtol(tok.c_str(), &end, 10);
                if (!end || *end || end == tok.c_str() || errno == ERANGE) return std::nullopt;
                ++at;
                CVal v; v.kind = Kind::Int; v.c[0] = double(n);
                return v;
            }
            const double n = std::strtod(tok.c_str(), &end);
            if (!end || *end || !std::isfinite(n)) return std::nullopt;
            ++at;
            CVal v; v.kind = Kind::Float; v.c[0] = n;
            return v;
        }
        if ((typeName(tok) || structs.count(tok)) && at + 1 < tokens.size() && tokens[at + 1] == "(") {
            Type tyBuilt = type();
            auto args = constArgs();
            // constArgs yields an empty vector for both `()` and a failed argument.
            // A failed argument does not leave the cursor on the closing paren.
            if (at == 0 || tokens[at - 1] != ")") return std::nullopt;
            auto built = foldConstruct(tyBuilt, args);
            if (!built) return std::nullopt;
            return withPostfix(*built);
        }
        if (ppIdent(tok) && at + 1 < tokens.size() && tokens[at + 1] == "(") {
            const bool derivCall = derivatives && (tok == "dFdx" || tok == "dFdy" || tok == "fwidth");
            if (!builtinName(tok) && !derivCall) return std::nullopt;
            ++at;
            const size_t before = at;
            auto args = constArgs();
            if (at == before || tokens[at - 1] != ")") return std::nullopt;
            auto built = foldBuiltin(tok, args);
            if (!built) return std::nullopt;
            return withPostfix(*built);
        }
        if (ppIdent(tok) && !keyword(tok)) {
            auto id = findName(tok);
            if (!id) return std::nullopt;
            auto it = constVals.find(*id);
            if (it == constVals.end()) return std::nullopt;
            ++at;
            return withPostfix(it->second);
        }
        return std::nullopt;
    }
    std::optional<CVal> constUnary() {
        if (++constDepth > 64) return std::nullopt;
        struct Guard { int& d; ~Guard() { --d; } } guard{constDepth};
        if (take("!") || take("+") || take("-")) {
            const bool neg = tokens[at - 1] == "-";
            const bool lnot = tokens[at - 1] == "!";
            auto v = constUnary();
            if (!v) return std::nullopt;
            if (lnot) {
                if (!v->scalar() || v->kind != Kind::Bool) return std::nullopt;
                v->c[0] = v->c[0] == 0.0 ? 1.0 : 0.0;
                return v;
            }
            if (v->kind == Kind::Bool) return std::nullopt;
            if (neg) for (int i = 0; i < v->width; ++i) v->c[i] = -v->c[i];
            return v;
        }
        return constPrimary();
    }
    std::optional<CVal> constMul() {
        auto v = constUnary();
        if (!v) return std::nullopt;
        while (peek() == "*" || peek() == "/") {
            const std::string op = peek();
            ++at;
            auto r = constUnary();
            if (!r) return std::nullopt;
            v = foldBin(*v, *r, op);
            if (!v) return std::nullopt;
        }
        return v;
    }
    std::optional<CVal> constAdd() {
        auto v = constMul();
        if (!v) return std::nullopt;
        while (peek() == "+" || peek() == "-") {
            const std::string op = peek();
            ++at;
            auto r = constMul();
            if (!r) return std::nullopt;
            v = foldBin(*v, *r, op);
            if (!v) return std::nullopt;
        }
        return v;
    }
    std::optional<CVal> constRel() {
        auto v = constAdd();
        if (!v) return std::nullopt;
        while (peek() == "<" || peek() == ">" || peek() == "<=" || peek() == ">=") {
            const std::string op = peek();
            ++at;
            auto r = constAdd();
            if (!r || !v->scalar() || !r->scalar() || v->kind != r->kind || v->kind == Kind::Bool) return std::nullopt;
            const bool b = op == "<" ? v->c[0] < r->c[0] : op == ">" ? v->c[0] > r->c[0] : op == "<=" ? v->c[0] <= r->c[0] : v->c[0] >= r->c[0];
            v = CVal{Kind::Bool, 1, 0, {b ? 1.0 : 0.0}};
        }
        return v;
    }
    std::optional<CVal> constEq() {
        auto v = constRel();
        if (!v) return std::nullopt;
        while (peek() == "==" || peek() == "!=") {
            const bool ne = peek() == "!=";
            ++at;
            auto r = constRel();
            if (!r || v->kind != r->kind || v->width != r->width || v->matrix != r->matrix) return std::nullopt;
            bool eq = true;
            for (int i = 0; i < v->width; ++i) if (v->c[i] != r->c[i]) eq = false;
            v = CVal{Kind::Bool, 1, 0, {(eq != ne) ? 1.0 : 0.0}};
        }
        return v;
    }
    std::optional<CVal> constAnd() {
        auto v = constEq();
        if (!v) return std::nullopt;
        while (take("&&")) {
            auto r = constEq();
            if (!r || !v->scalar() || !r->scalar() || v->kind != Kind::Bool || r->kind != Kind::Bool) return std::nullopt;
            v->c[0] = (v->c[0] != 0.0 && r->c[0] != 0.0) ? 1.0 : 0.0;
        }
        return v;
    }
    std::optional<CVal> constXor() {
        auto v = constAnd();
        if (!v) return std::nullopt;
        while (take("^^")) {
            auto r = constAnd();
            if (!r || !v->scalar() || !r->scalar() || v->kind != Kind::Bool || r->kind != Kind::Bool) return std::nullopt;
            v->c[0] = ((v->c[0] != 0.0) != (r->c[0] != 0.0)) ? 1.0 : 0.0;
        }
        return v;
    }
    std::optional<CVal> constOr() {
        auto v = constXor();
        if (!v) return std::nullopt;
        while (take("||")) {
            auto r = constXor();
            if (!r || !v->scalar() || !r->scalar() || v->kind != Kind::Bool || r->kind != Kind::Bool) return std::nullopt;
            v->c[0] = (v->c[0] != 0.0 || r->c[0] != 0.0) ? 1.0 : 0.0;
        }
        return v;
    }
    std::optional<CVal> constTernary() {
        auto c = constOr();
        if (!c) return std::nullopt;
        if (!take("?")) return c;
        auto a = constTernary();
        if (!a || !take(":")) return std::nullopt;
        auto b = constTernary();
        if (!b || !c->scalar() || c->kind != Kind::Bool || a->kind != b->kind || a->width != b->width || a->matrix != b->matrix) return std::nullopt;
        return c->c[0] != 0.0 ? a : b;
    }
    int arraySize() {
        const size_t saved = at;
        auto k = constTernary();
        const double raw = k ? k->c[0] : 0.0;
        const double truncated = std::trunc(raw);
        // Compare the double before any integer cast. 1e20 and 2^63 do not fit in a long.
        const bool integral = k && k->scalar() && k->kind == Kind::Int && std::isfinite(raw) &&
                              truncated == raw && truncated >= 1.0 && truncated <= double(MaxSlots);
        if (!integral) { at = saved; fail("invalid array size"); }
        return int(truncated);
    }
    bool containsSampler(const Type& t) const {
        if (t.sampler) return true;
        for (auto& f : t.fields) if (f.sampler || containsSampler(fieldType(f))) return true;
        return false;
    }
    bool nonFloat(const Type& t) const {
        if (!t.isStruct() && t.kind != Kind::Float) return true;
        for (auto& f : t.fields) if (nonFloat(fieldType(f))) return true;
        return false;
    }
    Type parseStruct() {
        if (++structDepth > 256) fail("struct nesting too deep");
        struct Guard { int& d; ~Guard() { --d; } } guard{structDepth};
        std::string name;
        if (peek() != "{") name = identifier();
        if (!name.empty() && (structs.count(name) || (findName(name) && scopes.size() == 1))) fail("duplicate struct " + name);
        need("{");
        std::vector<Type::Field> fields;
        int offset = 0;
        while (!take("}")) {
            if (at >= tokens.size()) fail("unterminated struct");
            if (precision(peek())) ++at;
            if (peek() == "const" || peek() == "attribute" || peek() == "uniform" || peek() == "varying" || peek() == "invariant")
                fail("unsupported struct qualifier");
            Type ft = type();
            if (ft.kind == Kind::Void) fail("void field");
            do {
                std::string fname = identifier();
                for (auto& existing : fields) if (existing.name == fname) fail("duplicate struct field " + fname);
                int count = 1; bool isArray = false;
                if (take("[")) { count = arraySize(); need("]"); isArray = true; }
                if (peek() == "=") fail("unsupported struct field initializer");
                Type::Field field;
                field.name = fname; field.offset = offset; field.width = ft.width; field.count = count;
                field.matrix = ft.matrix; field.sampler = ft.sampler; field.array = isArray; field.fields = ft.fields;
                field.kind = ft.kind; field.cube = ft.cube;
                const int span = checkedSpan(ft.width, count);
                const int64_t next = int64_t(offset) + span;
                if (next > int64_t(MaxSlots)) fail("shader register limit exceeded");
                offset = int(next);
                fields.push_back(std::move(field));
            } while (take(","));
            need(";");
        }
        if (fields.empty() || offset < 1) fail("empty struct");
        Type t; t.width = offset; t.fields = std::move(fields);
        if (!name.empty()) structs.emplace(name, t);
        return t;
    }
    Type type() {
        if (take("struct")) return parseStruct();
        if (auto it = structs.find(peek()); it != structs.end()) { ++at; return it->second; }
        std::string t = peek();
        if (t == "void") { ++at; return tyVoid(); }
        if (!typeName(t)) fail("unsupported type");
        ++at;
        if (t == "float") return ty(Kind::Float, 1);
        if (t == "int") return ty(Kind::Int, 1);
        if (t == "bool") return ty(Kind::Bool, 1);
        if (t == "sampler2D") return tySam(false);
        if (t == "samplerCube") return tySam(true);
        Kind k = t[0] == 'i' ? Kind::Int : t[0] == 'b' ? Kind::Bool : Kind::Float;
        int n = t.back() - '0';
        return t[0] == 'm' ? tyMat(n) : ty(k, n);
    }
    void rvalue(Expression& e) {
        if (e.needsIndex) fail("array requires literal index");
        if (e.type.kind != Kind::Void && e.slots.empty()) fail("invalid expression");
    }
    Expression lit(float v, Kind k) {
        Expression e; e.type = ty(k, 1); e.slots = {cst(v)}; e.isConst = true; e.constValue = v; e.contig = true; e.direct = true; e.origin = e.slots[0];
        return e;
    }
    bool scalar(const Expression& e) const {
        return !e.needsIndex && !e.type.matrix && !e.type.sampler && !e.type.isStruct() && e.type.width == 1;
    }
    bool vecLike(const Expression& e, Kind k) const {
        return !e.needsIndex && !e.type.matrix && !e.type.sampler && !e.type.isStruct() && e.type.kind == k && e.type.width >= 1 && e.type.width <= 4;
    }
    bool boolScalar(const Expression& e) const { return scalar(e) && e.type.kind == Kind::Bool; }
    int slotAt(const Expression& e, int i) const { return e.slots[size_t(e.type.width == 1 ? 0 : i)]; }
    Expression named(Type t, std::vector<int> slots, bool lv) {
        Expression e; e.type = t; e.slots = std::move(slots); e.lvalue = lv;
        e.contig = !e.slots.empty();
        for (size_t i = 1; i < e.slots.size(); ++i) if (e.slots[i] != e.slots[0] + int(i)) e.contig = false;
        if (e.contig && !e.slots.empty()) { e.direct = true; e.origin = e.slots[0]; }
        return e;
    }
    void noteWrite(const Expression& dst) {
        if (dst.fragTag == 1) wroteFragColor = true;
        if (dst.fragTag == 2) wroteFragData = true;
        bool hit = dst.fragTag != 0;
        if (dst.dynamic) {
            int span = dst.elemCount * std::max(1, dst.elemStride);
            if (dst.origin < p.output + 4 && dst.origin + span > p.output) hit = true;
        } else for (int s : dst.slots) if (s >= p.output && s < p.output + 4) hit = true;
        if (hit) wroteOutput = true;
    }
    void assignSlots(const std::vector<int>& dst, const std::vector<int>& src) {
        std::vector<int> temp;
        for (int s : src) { int t = alloc(); mov(t, s); temp.push_back(t); }
        for (size_t i = 0; i < dst.size(); ++i) mov(dst[i], temp[i]);
    }
    void store(Expression dst, Expression src) {
        if (dst.needsIndex) fail("array requires literal index");
        if (!dst.lvalue) fail("assignment to read-only variable");
        rvalue(src);
        if (dst.type != src.type) fail("assignment type mismatch");
        if (dst.dynamic) {
            auto comps = dst.dynComp;
            auto sorted = comps; std::sort(sorted.begin(), sorted.end());
            if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) fail("duplicate destination swizzle");
            for (size_t j = 0; j < comps.size(); ++j) {
                for (int i = 0; i < dst.elemCount; ++i) {
                    int addr = dst.origin + i * dst.elemStride + comps[j];
                    int cond = emit(Op::Equal, dst.indexSlot, cst(float(i)));
                    int sel = emit(Op::Select, cond, src.slots[j], addr);
                    mov(addr, sel);
                }
            }
            noteWrite(dst); return;
        }
        auto sorted = dst.slots; std::sort(sorted.begin(), sorted.end());
        if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) fail("duplicate destination swizzle");
        assignSlots(dst.slots, src.slots);
        noteWrite(dst);
    }
    Expression varRef(size_t index) {
        const Variable& v = p.variables[index];
        Expression e; e.type = v.type; e.lvalue = !v.readOnly; e.direct = true; e.origin = v.slot;
        if (v.name == "gl_FragColor") e.fragTag = 1;
        if (v.name == "gl_FragData") e.fragTag = 2;
        if (v.array || v.count != 1) {
            e.needsIndex = true; e.elemCount = v.count; e.elemStride = std::max(1, v.type.width); return e;
        }
        e.contig = true;
        for (int i = 0; i < v.type.width; ++i) e.slots.push_back(v.slot + i);
        return e;
    }
    Expression takeConst(const Expression& base, int index, Type elem, int stride) {
        if (index < 0) fail("array index must be an in-range literal");
        Expression o; o.type = elem; o.lvalue = base.lvalue; o.fragTag = base.fragTag;
        int n = elem.kind == Kind::Void ? 0 : std::max(1, elem.width);
        if (elem.sampler) n = 1;
        if (base.direct) {
            o.direct = true; o.contig = true; o.origin = base.origin + index * stride;
            for (int c = 0; c < n; ++c) o.slots.push_back(o.origin + c);
        } else {
            if (index * stride + n > int(base.slots.size())) fail("array index must be an in-range literal");
            for (int c = 0; c < n; ++c) o.slots.push_back(base.slots[size_t(index * stride + c)]);
            o = named(elem, o.slots, base.lvalue); o.fragTag = base.fragTag;
        }
        return o;
    }
    Expression takeDynamic(const Expression& base, const Expression& idx, Type elem, int stride, int count) {
        if (elem.sampler) fail("sampler index must be a constant");
        if (!base.direct) fail("dynamic index requires an addressable value");
        int frozen = alloc(); mov(frozen, idx.slots[0]);
        int n = std::max(1, elem.width);
        Expression o; o.type = elem; o.lvalue = base.lvalue; o.dynamic = true; o.fragTag = base.fragTag;
        o.origin = base.origin; o.elemStride = stride; o.elemCount = count; o.indexSlot = frozen;
        for (int c = 0; c < n; ++c) o.dynComp.push_back(c);
        for (int c = 0; c < n; ++c) {
            int cur = base.origin + c;
            for (int i = 1; i < count; ++i) {
                int cond = emit(Op::Equal, frozen, cst(float(i)));
                cur = emit(Op::Select, cond, base.origin + i * stride + c, cur);
            }
            o.slots.push_back(cur);
        }
        return o;
    }
    Expression doIndex(Expression base) {
        auto idx = expression();
        need("]");
        rvalue(idx);
        if (!scalar(idx) || idx.type.kind != Kind::Int) fail("index must be a scalar int");
        if (base.needsIndex) {
            if (idx.isConst) {
                int i = int(idx.constValue);
                if (i < 0 || i >= base.elemCount) fail("array index must be an in-range literal");
                return takeConst(base, i, base.type, base.elemStride);
            }
            return takeDynamic(base, idx, base.type, base.elemStride, base.elemCount);
        }
        if (base.type.matrix) {
            int n = base.type.matrix;
            Type col = ty(Kind::Float, n);
            if (!base.direct && base.slots.size() == size_t(n * n)) {
                Expression tmp = named(base.type, base.slots, base.lvalue);
                tmp.fragTag = base.fragTag; base = tmp;
            }
            if (idx.isConst) {
                int i = int(idx.constValue);
                if (i < 0 || i >= n) fail("array index must be an in-range literal");
                return takeConst(base, i, col, n);
            }
            return takeDynamic(base, idx, col, n, n);
        }
        if (!base.type.isStruct() && !base.type.sampler && base.type.width > 1) {
            Type elem = ty(base.type.kind, 1);
            if (idx.isConst) {
                int i = int(idx.constValue);
                if (i < 0 || i >= base.type.width) fail("array index must be an in-range literal");
                if (base.direct) return takeConst(base, i, elem, 1);
                return named(elem, {base.slots[size_t(i)]}, base.lvalue);
            }
            if (base.direct) return takeDynamic(base, idx, elem, 1, base.type.width);
            int frozen = alloc(); mov(frozen, idx.slots[0]);
            int cur = base.slots[0];
            for (int i = 1; i < base.type.width; ++i) cur = emit(Op::Select, emit(Op::Equal, frozen, cst(float(i))), base.slots[size_t(i)], cur);
            return named(elem, {cur}, false);
        }
        fail("cannot index this value");
    }
    Expression doMember(Expression e) {
        if (e.needsIndex) fail("array requires literal index");
        if (e.type.isStruct()) {
            std::string name = identifier();
            const Type::Field* field = nullptr;
            for (auto& f : e.type.fields) if (f.name == name) field = &f;
            if (!field) fail("unknown struct field " + name);
            Type ft = fieldType(*field);
            if (field->array || field->count != 1) {
                Expression o; o.type = ft; o.lvalue = e.lvalue; o.fragTag = e.fragTag; o.needsIndex = true;
                o.elemCount = field->count; o.elemStride = std::max(1, field->width);
                if (e.direct) { o.direct = true; o.origin = e.origin + field->offset; }
                else fail("array requires literal index");
                return o;
            }
            if (e.dynamic) {
                Expression o = e; o.type = ft; o.slots.clear(); o.dynComp.clear();
                for (int c = 0; c < ft.width; ++c) {
                    int src = field->offset + c;
                    if (src < 0 || src >= int(e.dynComp.size())) fail("struct field out of range");
                    o.dynComp.push_back(e.dynComp[size_t(src)]);
                    o.slots.push_back(e.slots[size_t(src)]);
                }
                return o;
            }
            std::vector<int> slots;
            if (e.direct) for (int c = 0; c < ft.width; ++c) slots.push_back(e.origin + field->offset + c);
            else {
                if (field->offset + ft.width > int(e.slots.size())) fail("struct field out of range");
                for (int c = 0; c < ft.width; ++c) slots.push_back(e.slots[size_t(field->offset + c)]);
            }
            auto o = named(ft, slots, e.lvalue); o.fragTag = e.fragTag; return o;
        }
        if (e.dynamic) {
            if (e.type.matrix || e.type.sampler || e.dynComp.size() <= 1) fail("swizzle requires vector");
            std::string mask = identifier();
            if (mask.empty() || mask.size() > 4) fail("invalid swizzle length");
            const std::string sets[] = {"xyzw", "rgba", "stpq"};
            int set = -1; std::vector<int> comps;
            for (char ch : mask) {
                int component = -1;
                for (int s = 0; s < 3; ++s) {
                    auto k = sets[s].find(ch);
                    if (k != std::string::npos) {
                        if (set != -1 && set != s) fail("mixed swizzle alphabets");
                        set = s; component = int(k); break;
                    }
                }
                if (component < 0 || component >= int(e.dynComp.size())) fail("swizzle component out of range");
                comps.push_back(component);
            }
            Expression o = e; o.type = ty(e.type.kind, int(mask.size())); o.slots.clear(); o.dynComp.clear();
            for (int c : comps) { o.dynComp.push_back(e.dynComp[size_t(c)]); o.slots.push_back(e.slots[size_t(c)]); }
            return o;
        }
        if (e.type.matrix || e.type.sampler || e.type.width <= 1 || e.type.isStruct()) fail("swizzle requires vector");
        std::string mask = identifier();
        if (mask.empty() || mask.size() > 4) fail("invalid swizzle length");
        const std::string sets[] = {"xyzw", "rgba", "stpq"};
        int set = -1; std::vector<int> slots;
        for (char ch : mask) {
            int component = -1;
            for (int s = 0; s < 3; ++s) {
                auto k = sets[s].find(ch);
                if (k != std::string::npos) {
                    if (set != -1 && set != s) fail("mixed swizzle alphabets");
                    set = s; component = int(k); break;
                }
            }
            if (component < 0 || component >= e.type.width) fail("swizzle component out of range");
            slots.push_back(e.direct ? e.origin + component : e.slots[size_t(component)]);
        }
        auto o = named(ty(e.type.kind, int(mask.size())), slots, e.lvalue); o.fragTag = e.fragTag; return o;
    }
    Expression postfix(Expression e) {
        while (true) {
            if (take("[")) e = doIndex(std::move(e));
            else if (take(".")) e = doMember(std::move(e));
            else if (take("++")) e = inc(std::move(e), false, 1);
            else if (take("--")) e = inc(std::move(e), false, -1);
            else break;
        }
        return e;
    }
    Expression neg(Expression e) {
        rvalue(e);
        if (e.type.sampler || e.type.isStruct() || e.type.matrix || e.needsIndex || (e.type.kind != Kind::Float && e.type.kind != Kind::Int))
            fail("type mismatch");
        Expression m = lit(e.type.kind == Kind::Int ? -1.f : -1.f, e.type.kind);
        return arithmetic("*", std::move(e), std::move(m));
    }
    Expression inc(Expression e, bool prefix, int sign) {
        if (!e.lvalue || e.needsIndex || e.type.matrix || e.type.sampler || e.type.isStruct() || e.type.kind == Kind::Bool)
            fail("lvalue required");
        Expression read = e; rvalue(read); read.lvalue = false; read.dynamic = false;
        std::vector<int> old;
        for (int s : read.slots) { int t = alloc(); mov(t, s); old.push_back(t); }
        Expression sum = arithmetic("+", named(e.type, old, false), lit(float(sign), e.type.kind));
        store(e, sum);
        return named(e.type, prefix ? sum.slots : old, false);
    }
    Expression logicNot(Expression e) {
        rvalue(e);
        if (e.type.kind != Kind::Bool || e.type.matrix || e.type.isStruct() || e.needsIndex) fail("logical not requires bool");
        Expression o; o.type = e.type;
        for (int s : e.slots) o.slots.push_back(emit(Op::Not, s));
        return o;
    }
    Expression unary() {
        if (take("++")) return inc(unary(), true, 1);
        if (take("--")) return inc(unary(), true, -1);
        if (take("!")) return logicNot(unary());
        if (take("-")) return neg(unary());
        if (take("+")) { auto e = unary(); if (!vecLike(e, Kind::Float) && !vecLike(e, Kind::Int) && !e.type.matrix) fail("type mismatch"); return e; }
        return postfix(primary());
    }
    Expression zipBool(Op op, Expression a, Expression b, bool scalarOnly) {
        rvalue(a); rvalue(b);
        if (a.type.kind != b.type.kind || a.type.matrix || b.type.matrix || a.type.sampler || b.type.sampler || a.type.isStruct() || b.type.isStruct())
            fail("type mismatch");
        if (a.type.kind == Kind::Bool && op != Op::Equal && op != Op::NotEqual) fail("type mismatch");
        if (scalarOnly && (a.type.width != 1 || b.type.width != 1)) fail("vector comparison requires a relational function");
        if (!scalarOnly && a.type != b.type) fail("type mismatch");
        if (scalarOnly && a.type.width == 1 && b.type.width == 1) {
            return named(ty(Kind::Bool, 1), {emit(op, a.slots[0], b.slots[0])}, false);
        }
        int acc = cst(1);
        for (int i = 0; i < a.type.width; ++i) {
            int cmp = emit(op == Op::NotEqual ? Op::Equal : op, a.slots[size_t(i)], b.slots[size_t(i)]);
            acc = emit(Op::Select, cmp, acc, cst(0));
        }
        if (op == Op::NotEqual) acc = emit(Op::Not, acc);
        return named(ty(Kind::Bool, 1), {acc}, false);
    }
    Expression mulDiv(Expression lhs) {
        while (peek() == "*" || peek() == "/") {
            auto op = peek(); ++at;
            auto rhs = unary();
            lhs = arithmetic(op, std::move(lhs), std::move(rhs));
        }
        return lhs;
    }
    Expression addSub(Expression lhs) {
        lhs = mulDiv(std::move(lhs));
        while (peek() == "+" || peek() == "-") {
            auto op = peek(); ++at;
            lhs = arithmetic(op, std::move(lhs), mulDiv(unary()));
        }
        return lhs;
    }
    Expression relational() {
        auto lhs = addSub(unary());
        auto op = peek();
        if (op != "<" && op != ">" && op != "<=" && op != ">=") return lhs;
        ++at;
        Op code = op == "<" ? Op::Less : op == ">" ? Op::Greater : op == "<=" ? Op::LessEqual : Op::GreaterEqual;
        return zipBool(code, std::move(lhs), addSub(unary()), true);
    }
    Expression equality() {
        auto lhs = relational();
        while (peek() == "==" || peek() == "!=") {
            auto op = peek(); ++at;
            lhs = zipBool(op == "==" ? Op::Equal : Op::NotEqual, std::move(lhs), relational(), false);
        }
        return lhs;
    }
    Expression land() {
        auto lhs = equality();
        while (take("&&")) {
            rvalue(lhs);
            if (!boolScalar(lhs)) fail("logical operator requires bool");
            int jf = jif(lhs.slots[0]);
            auto rhs = equality();
            if (!boolScalar(rhs)) fail("logical operator requires bool");
            int result = alloc(); mov(result, rhs.slots[0]);
            int j = jump(0); patchF(jf, int(p.code.size())); mov(result, cst(0)); patchJ(j, int(p.code.size()));
            lhs = named(ty(Kind::Bool, 1), {result}, false);
        }
        return lhs;
    }
    Expression lxor() {
        auto lhs = land();
        while (take("^^")) {
            auto rhs = land();
            if (!boolScalar(lhs) || !boolScalar(rhs)) fail("logical operator requires bool");
            int cmp = emit(Op::NotEqual, lhs.slots[0], rhs.slots[0]);
            lhs = named(ty(Kind::Bool, 1), {cmp}, false);
        }
        return lhs;
    }
    Expression lor() {
        auto lhs = lxor();
        while (take("||")) {
            rvalue(lhs);
            if (!boolScalar(lhs)) fail("logical operator requires bool");
            int jf = jif(lhs.slots[0]);
            int result = alloc(); mov(result, cst(1));
            int j = jump(0); patchF(jf, int(p.code.size()));
            auto rhs = lxor();
            if (!boolScalar(rhs)) fail("logical operator requires bool");
            mov(result, rhs.slots[0]); patchJ(j, int(p.code.size()));
            lhs = named(ty(Kind::Bool, 1), {result}, false);
        }
        return lhs;
    }
    Expression ternary() {
        auto cond = lor();
        if (!take("?")) return cond;
        rvalue(cond);
        if (!boolScalar(cond)) fail("condition must be bool");
        int jf = jif(cond.slots[0]);
        auto yes = expression();
        need(":");
        if (yes.needsIndex || yes.type.kind == Kind::Void) fail("ternary operands differ");
        int base = alloc(std::max(1, yes.type.width));
        for (int i = 0; i < yes.type.width; ++i) mov(base + i, yes.slots[size_t(i)]);
        int j = jump(0);
        patchF(jf, int(p.code.size()));
        auto no = assignment();
        if (no.type != yes.type || no.needsIndex) fail("ternary operands differ");
        for (int i = 0; i < no.type.width; ++i) mov(base + i, no.slots[size_t(i)]);
        patchJ(j, int(p.code.size()));
        std::vector<int> slots;
        for (int i = 0; i < yes.type.width; ++i) slots.push_back(base + i);
        return named(yes.type, std::move(slots), false);
    }
    Expression assignment() {
        auto lhs = ternary();
        auto op = peek();
        if (op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=") {
            ++at;
            auto rhs = assignment();
            if (op != "=") {
                Expression read = lhs; rvalue(read); read.lvalue = false; read.dynamic = false;
                rhs = arithmetic(op.substr(0, 1), std::move(read), std::move(rhs));
            }
            store(lhs, rhs);
            rhs.lvalue = false; rhs.dynamic = false; rhs.fragTag = 0; rhs.needsIndex = false;
            return rhs;
        }
        return lhs;
    }
    Expression expression() {
        if (++expressionDepth > 64) fail("expression nesting limit exceeded");
        struct Guard { int& d; ~Guard() { --d; } } guard{expressionDepth};
        auto e = assignment();
        while (take(",")) e = assignment();
        return e;
    }
    Expression arithmetic(const std::string& op, Expression a, Expression b) {
        rvalue(a); rvalue(b);
        if (a.needsIndex || b.needsIndex || a.type.sampler || b.type.sampler || a.type.isStruct() || b.type.isStruct()) fail("type mismatch");
        if (a.type.kind == Kind::Bool || b.type.kind == Kind::Bool || a.type.kind == Kind::Void || b.type.kind == Kind::Void) fail("type mismatch");
        auto asMatVec = [&](const Expression& m, const Expression& v) {
            int n = m.type.matrix;
            if (v.type.matrix || v.type.width != n || v.type.kind != Kind::Float || m.type.kind != Kind::Float) fail("type mismatch");
            Expression out = named(ty(Kind::Float, n), {}, false);
            for (int row = 0; row < n; ++row) {
                int sum = emit(Op::Multiply, m.slots[size_t(row)], v.slots[0]);
                for (int col = 1; col < n; ++col)
                    sum = emit(Op::Add, sum, emit(Op::Multiply, m.slots[size_t(col * n + row)], v.slots[size_t(col)]));
                out.slots.push_back(sum);
            }
            return out;
        };
        if (op == "*" && a.type.matrix && scalar(b)) {
            std::vector<int> slots;
            for (int s : a.slots) slots.push_back(emit(Op::Multiply, s, b.slots[0]));
            return named(a.type, slots, false);
        }
        if (op == "*" && b.type.matrix && scalar(a)) {
            std::vector<int> slots;
            for (int s : b.slots) slots.push_back(emit(Op::Multiply, a.slots[0], s));
            return named(b.type, slots, false);
        }
        if (op == "*" && a.type.matrix && !b.type.matrix) return asMatVec(a, b);
        if (op == "*" && b.type.matrix && !a.type.matrix) {
            int n = b.type.matrix;
            if (a.type.width != n || a.type.kind != Kind::Float || b.type.kind != Kind::Float) fail("type mismatch");
            Expression out = named(ty(Kind::Float, n), {}, false);
            for (int col = 0; col < n; ++col) {
                int sum = emit(Op::Multiply, a.slots[0], b.slots[size_t(col * n)]);
                for (int row = 1; row < n; ++row)
                    sum = emit(Op::Add, sum, emit(Op::Multiply, a.slots[size_t(row)], b.slots[size_t(col * n + row)]));
                out.slots.push_back(sum);
            }
            return out;
        }
        if (a.type.matrix || b.type.matrix) {
            if (a.type.kind != Kind::Float || b.type.kind != Kind::Float) fail("type mismatch");
            if (op == "*" && a.type.matrix && b.type.matrix && a.type.matrix == b.type.matrix) {
                int n = a.type.matrix;
                std::vector<int> slots(size_t(n * n));
                for (int col = 0; col < n; ++col) for (int row = 0; row < n; ++row) {
                    int sum = emit(Op::Multiply, a.slots[size_t(row)], b.slots[size_t(col * n)]);
                    for (int k = 1; k < n; ++k)
                        sum = emit(Op::Add, sum, emit(Op::Multiply, a.slots[size_t(k * n + row)], b.slots[size_t(col * n + k)]));
                    slots[size_t(col * n + row)] = sum;
                }
                return named(tyMat(n), slots, false);
            }
            if ((op == "+" || op == "-") && a.type.matrix && a.type == b.type) {
                Op code = op == "+" ? Op::Add : Op::Subtract;
                std::vector<int> slots;
                for (size_t i = 0; i < a.slots.size(); ++i) slots.push_back(emit(code, a.slots[i], b.slots[i]));
                return named(a.type, slots, false);
            }
            fail("unsupported matrix arithmetic");
        }
        if (a.type.kind != b.type.kind) fail("type mismatch");
        if (a.type.width != b.type.width && a.type.width != 1 && b.type.width != 1) fail("incompatible operand widths");
        int n = std::max(a.type.width, b.type.width);
        Op code = op == "+" ? Op::Add : op == "-" ? Op::Subtract : op == "*" ? Op::Multiply : Op::Divide;
        if (op == "/" && a.type.kind == Kind::Int) code = Op::IntDivide;
        Expression out = named(ty(a.type.kind, n), {}, false);
        for (int i = 0; i < n; ++i) out.slots.push_back(emit(code, slotAt(a, i), slotAt(b, i)));
        return out;
    }
    int convertSlot(int slot, Kind from, Kind to) {
        if (from == to) return slot;
        if (to == Kind::Bool) return emit(Op::Not, emit(Op::Equal, slot, cst(0)));
        if (from == Kind::Bool) return slot;
        if (from == Kind::Float && to == Kind::Int) {
            int ge = emit(Op::GreaterEqual, slot, cst(0));
            return emit(Op::Select, ge, emit(Op::Floor, slot), emit(Op::Ceil, slot));
        }
        if (from == Kind::Int && to == Kind::Float) return slot;
        fail("cannot convert");
    }
    Expression pack(Type t, const std::vector<int>& slots) {
        int base = alloc(std::max(1, t.width));
        for (int i = 0; i < t.width; ++i) mov(base + i, slots[size_t(i)]);
        std::vector<int> out;
        for (int i = 0; i < t.width; ++i) out.push_back(base + i);
        auto e = named(t, out, false); e.direct = true; e.contig = true; e.origin = base; return e;
    }
    std::vector<std::pair<int, Kind>> flatten(Expression arg) {
        rvalue(arg);
        if (arg.needsIndex || arg.type.sampler || arg.type.isStruct() || arg.type.kind == Kind::Void) fail("unsupported constructor argument");
        std::vector<std::pair<int, Kind>> comps;
        Kind k = arg.type.matrix ? Kind::Float : arg.type.kind;
        for (int s : arg.slots) comps.push_back({s, k});
        return comps;
    }
    Expression construct(Type t, std::vector<Expression> args) {
        if (t.sampler || t.kind == Kind::Void || args.empty()) fail("unsupported constructor");
        if (t.isStruct()) {
            if (args.size() != t.fields.size()) fail("struct constructor field count");
            int base = alloc(t.width);
            for (size_t n = 0; n < args.size(); ++n) {
                if (t.fields[n].count != 1 || t.fields[n].array) fail("unsupported array field constructor");
                if (args[n].type != fieldType(t.fields[n])) fail("struct constructor type mismatch");
                rvalue(args[n]);
                for (int c = 0; c < t.fields[n].width; ++c) mov(base + t.fields[n].offset + c, args[n].slots[size_t(c)]);
            }
            std::vector<int> slots;
            for (int i = 0; i < t.width; ++i) slots.push_back(base + i);
            auto e = named(t, slots, false); e.direct = true; e.origin = base; return e;
        }
        if (t.matrix && args.size() == 1 && scalar(args[0])) {
            rvalue(args[0]);
            int s = convertSlot(args[0].slots[0], args[0].type.kind, Kind::Float);
            int n = t.matrix, base = alloc(t.width);
            for (int i = 0; i < t.width; ++i) mov(base + i, (i % (n + 1) == 0) ? s : cst(0));
            std::vector<int> slots; for (int i = 0; i < t.width; ++i) slots.push_back(base + i);
            auto e = named(t, slots, false); e.direct = true; e.origin = base; return e;
        }
        if (t.matrix && args.size() == 1 && args[0].type.matrix) {
            rvalue(args[0]);
            int n = t.matrix, m = args[0].type.matrix, base = alloc(t.width);
            for (int col = 0; col < n; ++col) for (int row = 0; row < n; ++row) {
                int dst = base + col * n + row;
                if (col < m && row < m) mov(dst, args[0].slots[size_t(col * m + row)]);
                else mov(dst, cst(col == row ? 1.f : 0.f));
            }
            std::vector<int> slots; for (int i = 0; i < t.width; ++i) slots.push_back(base + i);
            auto e = named(t, slots, false); e.direct = true; e.origin = base; return e;
        }
        if (!t.matrix && args.size() == 1 && scalar(args[0]) && t.width > 1) {
            rvalue(args[0]);
            int s = convertSlot(args[0].slots[0], args[0].type.kind, t.kind);
            std::vector<int> slots(size_t(t.width), s);
            return pack(t, slots);
        }
        std::vector<std::pair<int, Kind>> comps;
        for (auto& arg : args) {
            auto part = flatten(arg);
            comps.insert(comps.end(), part.begin(), part.end());
        }
        if (int(comps.size()) != t.width) fail("constructor component count mismatch");
        std::vector<int> slots;
        Kind want = t.matrix ? Kind::Float : t.kind;
        for (auto& c : comps) slots.push_back(convertSlot(c.first, c.second, want));
        return pack(t, slots);
    }
    std::vector<Expression> parseArgs() {
        need("(");
        std::vector<Expression> args;
        if (!take(")")) {
            do { args.push_back(assignment()); } while (take(","));
            need(")");
        }
        return args;
    }
    bool sameParamTypes(const std::vector<Param>& a, const std::vector<Param>& b, bool qual) const {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (a[i].type != b[i].type || a[i].isArray != b[i].isArray || a[i].count != b[i].count) return false;
            if (qual && a[i].qual != b[i].qual) return false;
        }
        return true;
    }
    bool argMatches(const Param& param, const Expression& arg) const {
        if (param.isArray) return arg.needsIndex && arg.type == param.type && arg.elemCount == param.count;
        return !arg.needsIndex && arg.type == param.type;
    }
    int findUser(const std::string& name, const std::vector<Expression>& args) const {
        int found = -1;
        for (size_t i = 0; i < funcs.size(); ++i) {
            if (funcs[i].name != name || funcs[i].params.size() != args.size()) continue;
            bool ok = true;
            for (size_t p = 0; p < args.size(); ++p) if (!argMatches(funcs[i].params[p], args[p])) ok = false;
            if (!ok) continue;
            if (found >= 0) fail("ambiguous function call");
            found = int(i);
        }
        return found;
    }
    Expression callUser(int fi, std::vector<Expression> args) {
        if (inlineStack.size() >= 256) fail("function call depth exceeded");
        for (auto id : inlineStack) if (int(id) == fi) fail("recursion is not allowed");
        if (!funcs[size_t(fi)].defined) fail("function is not defined");
        inlineStack.push_back(size_t(fi));
        pushScope();
        struct OutCopy { Expression dst; int origin; int n; bool block; };
        std::vector<OutCopy> outs;
        for (size_t i = 0; i < args.size(); ++i) {
            const Param param = funcs[size_t(fi)].params[i];
            if (!argMatches(param, args[i])) fail("argument type mismatch");
            if (param.qual != 0 && !args[i].lvalue) fail("out parameter requires lvalue");
            if (param.qual != 0 && args[i].dynamic) {
                int frozen = alloc(); mov(frozen, args[i].indexSlot); args[i].indexSlot = frozen;
            }
            auto& local = declare(param.name, param.type, Storage::Local, param.count, false, param.isArray);
            int slot = local.slot;
            if (param.isArray) {
                if (!args[i].direct) fail("array argument must be addressable");
                int n = param.count * std::max(1, param.type.width);
                if (param.qual != 1) for (int c = 0; c < n; ++c) mov(slot + c, args[i].origin + c);
                if (param.qual != 0) outs.push_back({args[i], slot, n, true});
            } else if (param.qual != 1) {
                rvalue(args[i]);
                std::vector<int> dst;
                for (int c = 0; c < param.type.width; ++c) dst.push_back(slot + c);
                assignSlots(dst, args[i].slots);
                if (param.qual == 2) outs.push_back({args[i], slot, param.type.width, false});
            } else outs.push_back({args[i], slot, param.type.width, false});
        }
        Frame frame;
        frame.ret = funcs[size_t(fi)].ret;
        frame.isMain = false;
        if (frame.ret.kind != Kind::Void) frame.result = alloc(std::max(1, frame.ret.width));
        frames.push_back(frame);
        size_t saved = at;
        int savedExpr = expressionDepth, savedStmt = stmtDepth;
        expressionDepth = 0;
        stmtDepth = 0;
        at = funcs[size_t(fi)].begin;
        while (at < funcs[size_t(fi)].end) statement();
        expressionDepth = savedExpr;
        stmtDepth = savedStmt;
        if (at != funcs[size_t(fi)].end) fail("invalid function body");
        int epi = int(p.code.size());
        for (int j : frames.back().returns) patchJ(j, epi);
        for (auto& o : outs) {
            if (o.block) {
                std::vector<int> src, dst;
                for (int c = 0; c < o.n; ++c) { src.push_back(o.origin + c); dst.push_back(o.dst.origin + c); }
                assignSlots(dst, src);
            } else {
                std::vector<int> slots;
                for (int c = 0; c < o.n; ++c) slots.push_back(o.origin + c);
                store(o.dst, named(o.dst.type, slots, false));
            }
        }
        Expression ret;
        ret.type = frame.ret;
        if (frame.result >= 0) {
            for (int c = 0; c < frame.ret.width; ++c) ret.slots.push_back(frame.result + c);
            ret = named(frame.ret, ret.slots, false);
        }
        frames.pop_back();
        popScope();
        at = saved;
        inlineStack.pop_back();
        return ret;
    }
    bool builtinName(const std::string& s) const {
        static const std::unordered_set<std::string> names = {
            "radians","degrees","sin","cos","tan","asin","acos","atan","pow","exp","log","exp2","log2","sqrt","inversesqrt",
            "abs","sign","floor","ceil","fract","mod","min","max","clamp","mix","step","smoothstep","length","distance","dot",
            "cross","normalize","faceforward","reflect","refract","matrixCompMult","lessThan","lessThanEqual","greaterThan",
            "greaterThanEqual","equal","notEqual","any","all","not","texture2D","texture2DProj","texture2DLod","texture2DProjLod",
            "textureCube","textureCubeLod"};
        return names.count(s) > 0;
    }
    Expression map1(Op op, Expression a) {
        rvalue(a); Expression o; o.type = a.type;
        for (int s : a.slots) o.slots.push_back(emit(op, s));
        return named(o.type, o.slots, false);
    }
    Expression scale(Expression a, float k) {
        rvalue(a); Expression o; o.type = a.type;
        int c = cst(k);
        for (int s : a.slots) o.slots.push_back(emit(Op::Multiply, s, c));
        return named(o.type, o.slots, false);
    }
    bool floatVec(const Expression& e) const { return vecLike(e, Kind::Float); }
    bool intVec(const Expression& e) const { return vecLike(e, Kind::Int); }
    Expression zipFloat(Op op, Expression a, Expression b) {
        if (a.type != b.type || !floatVec(a)) fail("invalid builtin arguments");
        rvalue(a); rvalue(b); Expression o; o.type = a.type;
        for (size_t i = 0; i < a.slots.size(); ++i) o.slots.push_back(emit(op, a.slots[i], b.slots[i]));
        return named(o.type, o.slots, false);
    }
    bool broadcast(const Expression& v, const Expression& s, Kind k) const {
        return vecLike(v, k) && scalar(s) && s.type.kind == k;
    }
    Expression minMax(Expression a, Expression b, bool isMin) {
        Kind k = a.type.kind;
        if ((k != Kind::Float && k != Kind::Int) || b.type.kind != k) fail("invalid builtin arguments");
        if (!vecLike(a, k) || !vecLike(b, k)) fail("invalid builtin arguments");
        if (a.type.width != b.type.width && a.type.width != 1 && b.type.width != 1) fail("invalid builtin arguments");
        rvalue(a); rvalue(b);
        int n = std::max(a.type.width, b.type.width);
        Expression o; o.type = ty(k, n);
        for (int i = 0; i < n; ++i) {
            int as = slotAt(a, i), bs = slotAt(b, i);
            int cmp = emit(isMin ? Op::Less : Op::Greater, as, bs);
            o.slots.push_back(emit(Op::Select, cmp, as, bs));
        }
        return named(o.type, o.slots, false);
    }
    Expression clampOf(Expression x, Expression a, Expression b) { return minMax(minMax(x, a, false), b, true); }
    Expression mixOf(Expression x, Expression y, Expression a) {
        if (!floatVec(x) || x.type != y.type) fail("invalid builtin arguments");
        if (!(a.type == x.type || (scalar(a) && a.type.kind == Kind::Float))) fail("invalid builtin arguments");
        rvalue(x); rvalue(y); rvalue(a);
        int n = x.type.width;
        Expression o; o.type = x.type;
        for (int i = 0; i < n; ++i) {
            int ai = slotAt(a, a.type.width == 1 ? 0 : i);
            int one = cst(1);
            int inv = emit(Op::Subtract, one, ai);
            int l = emit(Op::Multiply, x.slots[size_t(i)], inv);
            int r = emit(Op::Multiply, y.slots[size_t(i)], ai);
            o.slots.push_back(emit(Op::Add, l, r));
        }
        return named(o.type, o.slots, false);
    }
    Expression stepOf(Expression edge, Expression x) {
        if (!floatVec(x) || !(edge.type == x.type || (scalar(edge) && edge.type.kind == Kind::Float))) fail("invalid builtin arguments");
        rvalue(edge); rvalue(x);
        Expression o; o.type = x.type;
        for (int i = 0; i < x.type.width; ++i) {
            int cmp = emit(Op::Less, x.slots[size_t(i)], slotAt(edge, i));
            o.slots.push_back(emit(Op::Select, cmp, cst(0), cst(1)));
        }
        return named(o.type, o.slots, false);
    }
    Expression smoothOf(Expression e0, Expression e1, Expression x) {
        if (!floatVec(x)) fail("invalid builtin arguments");
        bool scalarEdge = scalar(e0) && scalar(e1) && e0.type.kind == Kind::Float && e1.type.kind == Kind::Float;
        if (!scalarEdge && (e0.type != x.type || e1.type != x.type)) fail("invalid builtin arguments");
        rvalue(e0); rvalue(e1); rvalue(x);
        Expression o; o.type = x.type;
        for (int i = 0; i < x.type.width; ++i) {
            int a = slotAt(e0, i), b = slotAt(e1, i), xv = x.slots[size_t(i)];
            int denom = emit(Op::Subtract, b, a);
            int t = emit(Op::Divide, emit(Op::Subtract, xv, a), denom);
            int ge0 = emit(Op::GreaterEqual, t, cst(0));
            int le1 = emit(Op::LessEqual, t, cst(1));
            t = emit(Op::Select, ge0, t, cst(0));
            t = emit(Op::Select, le1, t, cst(1));
            int tt = emit(Op::Multiply, t, t);
            int poly = emit(Op::Subtract, cst(3), emit(Op::Multiply, cst(2), t));
            o.slots.push_back(emit(Op::Multiply, tt, poly));
        }
        return named(o.type, o.slots, false);
    }
    Expression dotOf(Expression a, Expression b) {
        if (a.type != b.type || !floatVec(a)) fail("invalid builtin arguments");
        rvalue(a); rvalue(b);
        int sum = emit(Op::Multiply, a.slots[0], b.slots[0]);
        for (int i = 1; i < a.type.width; ++i) sum = emit(Op::Add, sum, emit(Op::Multiply, a.slots[size_t(i)], b.slots[size_t(i)]));
        return named(ty(Kind::Float, 1), {sum}, false);
    }
    Expression lengthOf(Expression a) {
        if (!floatVec(a)) fail("invalid builtin arguments");
        return named(ty(Kind::Float, 1), {emit(Op::Sqrt, dotOf(a, a).slots[0])}, false);
    }
    Expression normOf(Expression a) {
        auto len = lengthOf(a);
        return arithmetic("/", a, len);
    }
    Expression sampleUV(const Expression& sampler, int u, int v) {
        int dst = alloc(4);
        p.code.push_back({Op::Texture, dst, sampler.slots[0], u, v, 0.f, 0});
        return named(ty(Kind::Float, 4), {dst, dst + 1, dst + 2, dst + 3}, false);
    }
    Expression sampleCube(const Expression& sampler, Expression dir) {
        rvalue(dir);
        int dst = alloc(4);
        p.code.push_back({Op::TextureCube, dst, sampler.slots[0], dir.slots[0], dir.slots[1], 0.f, dir.slots[2]});
        return named(ty(Kind::Float, 4), {dst, dst + 1, dst + 2, dst + 3}, false);
    }
    Expression projSample(const Expression& sampler, Expression coord) {
        rvalue(coord);
        int w = coord.slots.back();
        return sampleUV(sampler, emit(Op::Divide, coord.slots[0], w), emit(Op::Divide, coord.slots[1], w));
    }
    bool isSam2D(const Expression& e) const { return e.type.sampler && !e.type.cube && !e.needsIndex; }
    bool isSamCube(const Expression& e) const { return e.type.sampler && e.type.cube && !e.needsIndex; }
    void needStage(bool vertexOnly, const char* name) {
        if (vertexOnly && !p.vertex) fail(std::string(name) + " is only available in a vertex shader");
        if (!vertexOnly && p.vertex) fail(std::string(name) + " is not available in a vertex shader");
    }
    Expression emitBuiltin(const std::string& name, std::vector<Expression>& args) {
        auto arity = [&](size_t n) { if (args.size() != n) fail("invalid arguments to " + name); };
        if (name == "dFdx" || name == "dFdy" || name == "fwidth") {
            if (!derivatives) fail("unknown variable/function " + name);
            if (p.vertex) fail(name + " is not available in a vertex shader");
            arity(1);
            if (!floatVec(args[0])) fail("invalid arguments to " + name);
            rvalue(args[0]);
            p.usesDerivatives = true;
            if (name == "fwidth") {
                Expression dx = map1(Op::DFdx, args[0]);
                Expression dy = map1(Op::DFdy, args[0]);
                Expression o; o.type = args[0].type;
                for (size_t i = 0; i < dx.slots.size(); ++i)
                    o.slots.push_back(emit(Op::Add, emit(Op::Abs, dx.slots[i]), emit(Op::Abs, dy.slots[i])));
                return named(o.type, o.slots, false);
            }
            return map1(name == "dFdx" ? Op::DFdx : Op::DFdy, args[0]);
        }
        if (name == "radians" || name == "degrees") {
            arity(1); if (!floatVec(args[0])) fail("invalid arguments to " + name);
            return scale(args[0], name == "radians" ? kPi / 180.f : 180.f / kPi);
        }
        if (name == "sin" || name == "cos" || name == "tan" || name == "asin" || name == "acos" || name == "exp" || name == "log" ||
            name == "exp2" || name == "log2" || name == "sqrt" || name == "inversesqrt" || name == "floor" || name == "ceil" || name == "abs") {
            arity(1);
            bool intOk = name == "abs";
            if (intOk && intVec(args[0])) return map1(Op::Abs, args[0]);
            if (!floatVec(args[0])) fail("invalid arguments to " + name);
            Op op = name == "sin" ? Op::Sin : name == "cos" ? Op::Cos : name == "tan" ? Op::Tan : name == "asin" ? Op::Asin :
                    name == "acos" ? Op::Acos : name == "exp" ? Op::Exp : name == "log" ? Op::Log : name == "exp2" ? Op::Exp2 :
                    name == "log2" ? Op::Log2 : name == "sqrt" ? Op::Sqrt : name == "inversesqrt" ? Op::InverseSqrt :
                    name == "floor" ? Op::Floor : name == "ceil" ? Op::Ceil : Op::Abs;
            return map1(op, args[0]);
        }
        if (name == "atan") {
            if (args.size() == 1) { if (!floatVec(args[0])) fail("invalid arguments to atan"); return map1(Op::Atan, args[0]); }
            if (args.size() == 2) return zipFloat(Op::Atan2, args[0], args[1]);
            fail("invalid arguments to atan");
        }
        if (name == "pow") { arity(2); return zipFloat(Op::Pow, args[0], args[1]); }
        if (name == "sign") {
            arity(1);
            if (!floatVec(args[0]) && !intVec(args[0])) fail("invalid arguments to sign");
            rvalue(args[0]); Expression o; o.type = args[0].type;
            for (int s : args[0].slots) {
                int gt = emit(Op::Greater, s, cst(0)), lt = emit(Op::Less, s, cst(0));
                o.slots.push_back(emit(Op::Select, gt, cst(1), emit(Op::Select, lt, cst(-1), cst(0))));
            }
            return named(o.type, o.slots, false);
        }
        if (name == "fract") {
            arity(1); if (!floatVec(args[0])) fail("invalid arguments to fract");
            rvalue(args[0]); Expression o; o.type = args[0].type;
            for (int s : args[0].slots) o.slots.push_back(emit(Op::Subtract, s, emit(Op::Floor, s)));
            return named(o.type, o.slots, false);
        }
        if (name == "mod") {
            arity(2);
            if (!floatVec(args[0]) || !(args[1].type == args[0].type || (scalar(args[1]) && args[1].type.kind == Kind::Float)))
                fail("invalid arguments to mod");
            rvalue(args[0]); rvalue(args[1]); Expression o; o.type = args[0].type;
            for (int i = 0; i < args[0].type.width; ++i) {
                int x = args[0].slots[size_t(i)], y = slotAt(args[1], i);
                int q = emit(Op::Floor, emit(Op::Divide, x, y));
                o.slots.push_back(emit(Op::Subtract, x, emit(Op::Multiply, y, q)));
            }
            return named(o.type, o.slots, false);
        }
        if (name == "min" || name == "max") { arity(2); return minMax(args[0], args[1], name == "min"); }
        if (name == "clamp") { arity(3); return clampOf(args[0], args[1], args[2]); }
        if (name == "mix") { arity(3); return mixOf(args[0], args[1], args[2]); }
        if (name == "step") { arity(2); return stepOf(args[0], args[1]); }
        if (name == "smoothstep") { arity(3); return smoothOf(args[0], args[1], args[2]); }
        if (name == "length") { arity(1); return lengthOf(args[0]); }
        if (name == "distance") { arity(2); if (args[0].type != args[1].type) fail("invalid arguments to distance"); return lengthOf(arithmetic("-", args[0], args[1])); }
        if (name == "dot") { arity(2); return dotOf(args[0], args[1]); }
        if (name == "cross") {
            arity(2);
            if (args[0].type != ty(Kind::Float, 3) || args[1].type != args[0].type) fail("invalid arguments to cross");
            rvalue(args[0]); rvalue(args[1]);
            auto cm = [&](int a, int b, int c, int d) { return emit(Op::Subtract, emit(Op::Multiply, a, b), emit(Op::Multiply, c, d)); };
            auto &A = args[0].slots, &B = args[1].slots;
            return named(ty(Kind::Float, 3), {cm(A[1], B[2], A[2], B[1]), cm(A[2], B[0], A[0], B[2]), cm(A[0], B[1], A[1], B[0])}, false);
        }
        if (name == "normalize") { arity(1); return normOf(args[0]); }
        if (name == "faceforward") {
            arity(3);
            if (args[0].type != args[1].type || args[0].type != args[2].type || !floatVec(args[0])) fail("invalid arguments to faceforward");
            int d = dotOf(args[2], args[1]).slots[0];
            int lt = emit(Op::Less, d, cst(0));
            auto negN = neg(args[0]);
            rvalue(args[0]); Expression o; o.type = args[0].type;
            for (int i = 0; i < args[0].type.width; ++i) o.slots.push_back(emit(Op::Select, lt, args[0].slots[size_t(i)], negN.slots[size_t(i)]));
            return named(o.type, o.slots, false);
        }
        if (name == "reflect") {
            arity(2);
            if (args[0].type != args[1].type || !floatVec(args[0])) fail("invalid arguments to reflect");
            int d = dotOf(args[1], args[0]).slots[0];
            int two = emit(Op::Multiply, cst(2), d);
            rvalue(args[0]); rvalue(args[1]); Expression o; o.type = args[0].type;
            for (int i = 0; i < args[0].type.width; ++i)
                o.slots.push_back(emit(Op::Subtract, args[0].slots[size_t(i)], emit(Op::Multiply, two, args[1].slots[size_t(i)])));
            return named(o.type, o.slots, false);
        }
        if (name == "refract") {
            arity(3);
            if (args[0].type != args[1].type || !floatVec(args[0]) || !scalar(args[2]) || args[2].type.kind != Kind::Float)
                fail("invalid arguments to refract");
            int d = dotOf(args[1], args[0]).slots[0];
            rvalue(args[2]); int eta = args[2].slots[0];
            int eta2 = emit(Op::Multiply, eta, eta);
            int d2 = emit(Op::Multiply, d, d);
            int k = emit(Op::Subtract, cst(1), emit(Op::Multiply, eta2, emit(Op::Subtract, cst(1), d2)));
            int neg = emit(Op::Less, k, cst(0));
            int scaleN = emit(Op::Add, emit(Op::Multiply, eta, d), emit(Op::Sqrt, k));
            rvalue(args[0]); rvalue(args[1]); Expression o; o.type = args[0].type;
            for (int i = 0; i < args[0].type.width; ++i) {
                int term = emit(Op::Subtract, emit(Op::Multiply, eta, args[0].slots[size_t(i)]), emit(Op::Multiply, scaleN, args[1].slots[size_t(i)]));
                o.slots.push_back(emit(Op::Select, neg, cst(0), term));
            }
            return named(o.type, o.slots, false);
        }
        if (name == "matrixCompMult") {
            arity(2);
            if (!args[0].type.matrix || args[0].type != args[1].type) fail("invalid arguments to matrixCompMult");
            rvalue(args[0]); rvalue(args[1]); std::vector<int> slots;
            for (size_t i = 0; i < args[0].slots.size(); ++i) slots.push_back(emit(Op::Multiply, args[0].slots[i], args[1].slots[i]));
            return named(args[0].type, slots, false);
        }
        if (name == "lessThan" || name == "lessThanEqual" || name == "greaterThan" || name == "greaterThanEqual" || name == "equal" || name == "notEqual") {
            arity(2);
            if (args[0].type != args[1].type || args[0].type.width < 2 || args[0].type.matrix || args[0].type.sampler || args[0].type.isStruct())
                fail("invalid arguments to " + name);
            Kind k = args[0].type.kind;
            bool rel = name != "equal" && name != "notEqual";
            if (rel && k != Kind::Float && k != Kind::Int) fail("invalid arguments to " + name);
            if (!rel && k != Kind::Float && k != Kind::Int && k != Kind::Bool) fail("invalid arguments to " + name);
            rvalue(args[0]); rvalue(args[1]);
            Op op = name == "lessThan" ? Op::Less : name == "lessThanEqual" ? Op::LessEqual : name == "greaterThan" ? Op::Greater :
                    name == "greaterThanEqual" ? Op::GreaterEqual : name == "equal" ? Op::Equal : Op::NotEqual;
            Expression o; o.type = ty(Kind::Bool, args[0].type.width);
            for (int i = 0; i < args[0].type.width; ++i) o.slots.push_back(emit(op, args[0].slots[size_t(i)], args[1].slots[size_t(i)]));
            return named(o.type, o.slots, false);
        }
        if (name == "any" || name == "all" || name == "not") {
            arity(1);
            if (args[0].type.kind != Kind::Bool || args[0].type.width < 2 || args[0].type.matrix) fail("invalid arguments to " + name);
            rvalue(args[0]);
            if (name == "not") return map1(Op::Not, args[0]);
            int acc = cst(name == "all" ? 1.f : 0.f);
            for (int s : args[0].slots) acc = name == "all" ? emit(Op::Select, s, acc, cst(0)) : emit(Op::Select, s, cst(1), acc);
            return named(ty(Kind::Bool, 1), {acc}, false);
        }
        if (name == "texture2D" || name == "texture2DLod") {
            bool lod = name == "texture2DLod";
            if (lod) arity(3); else if (args.size() != 2 && args.size() != 3) fail("invalid arguments to texture2D");
            if (!isSam2D(args[0]) || args[1].type != ty(Kind::Float, 2)) fail("invalid arguments to " + name);
            if (args.size() == 3 && !scalar(args[2])) fail("invalid arguments to " + name);
            needStage(lod, name.c_str());
            rvalue(args[0]); rvalue(args[1]);
            return sampleUV(args[0], args[1].slots[0], args[1].slots[1]);
        }
        if (name == "texture2DProj" || name == "texture2DProjLod") {
            bool lod = name == "texture2DProjLod";
            if (!isSam2D(args[0])) fail("invalid arguments to " + name);
            bool vec3 = args.size() >= 2 && args[1].type == ty(Kind::Float, 3);
            bool vec4 = args.size() >= 2 && args[1].type == ty(Kind::Float, 4);
            if (!vec3 && !vec4) fail("invalid arguments to " + name);
            if (lod) { if (args.size() != 3 || !scalar(args[2])) fail("invalid arguments to " + name); }
            else if (args.size() == 3) { if (!scalar(args[2]) || args[2].type.kind != Kind::Float) { /* bias or not */ } if (!scalar(args[2])) fail("invalid arguments to " + name); }
            else if (args.size() != 2) fail("invalid arguments to " + name);
            needStage(lod, name.c_str());
            rvalue(args[0]);
            return projSample(args[0], args[1]);
        }
        if (name == "textureCube" || name == "textureCubeLod") {
            bool lod = name == "textureCubeLod";
            if (!isSamCube(args[0]) || args.size() < 2 || args[1].type != ty(Kind::Float, 3)) fail("invalid arguments to " + name);
            if (lod) { if (args.size() != 3 || !scalar(args[2])) fail("invalid arguments to " + name); }
            else if (args.size() == 3) { if (!scalar(args[2])) fail("invalid arguments to " + name); }
            else if (args.size() != 2) fail("invalid arguments to " + name);
            needStage(lod, name.c_str());
            rvalue(args[0]);
            return sampleCube(args[0], args[1]);
        }
        fail("invalid arguments to " + name);
    }
    Expression primary() {
        if (take("true")) return lit(1, Kind::Bool);
        if (take("false")) return lit(0, Kind::Bool);
        if (take("(")) { auto e = expression(); need(")"); return e; }
        std::string tok = peek();
        if (!tok.empty() && (std::isdigit(static_cast<unsigned char>(tok[0])) || tok[0] == '.')) {
            bool isFloat = tok.find('.') != std::string::npos || tok.find('e') != std::string::npos || tok.find('E') != std::string::npos;
            char* end = nullptr;
            if (!isFloat) {
                errno = 0;
                long v = std::strtol(tok.c_str(), &end, 10);
                if (!end || *end || errno == ERANGE) fail("invalid numeric literal");
                ++at; return lit(float(v), Kind::Int);
            }
            float v = std::strtof(tok.c_str(), &end);
            if (!end || *end) fail("invalid numeric literal");
            ++at; return lit(v, Kind::Float);
        }
        if ((typeName(tok) || structs.count(tok)) && at + 1 < tokens.size() && tokens[at + 1] == "(") {
            Type t = type();
            if (t.kind == Kind::Void) fail("unsupported constructor");
            return construct(t, parseArgs());
        }
        std::string name = identifier();
        if (peek() == "(") {
            auto args = parseArgs();
            int user = findUser(name, args);
            const bool derivCall = derivatives && (name == "dFdx" || name == "dFdy" || name == "fwidth");
            if (user >= 0) return callUser(user, std::move(args));
            if (builtinName(name) || derivCall) return emitBuiltin(name, args);
            fail("unknown variable/function " + name);
        }
        auto id = findName(name);
        if (!id) fail("unknown variable/function " + name);
        return varRef(*id);
    }
    struct Qualifiers { bool invariant = false, isConst = false, storageSet = false, precisionSet = false; Storage storage = Storage::Local; };
    // ES 1.00 orders invariant, then storage, then precision. Shaders from real
    // applications put precision first (`mediump varying`, `highp attribute`), so any order is accepted.
    Qualifiers qualifiers() {
        Qualifiers q;
        while (true) {
            auto s = peek();
            if (s == "invariant") { if (q.invariant) fail("duplicate qualifier"); q.invariant = true; ++at; }
            else if (s == "const") { if (q.isConst) fail("duplicate qualifier"); q.isConst = true; ++at; }
            else if (s == "attribute" || s == "uniform" || s == "varying") {
                if (q.storageSet) fail("duplicate qualifier");
                q.storageSet = true;
                q.storage = s == "attribute" ? Storage::Attribute : s == "uniform" ? Storage::Uniform : Storage::Varying;
                ++at;
            } else if (precision(s)) { if (q.precisionSet) fail("duplicate qualifier"); q.precisionSet = true; ++at; }
            else break;
        }
        return q;
    }
    void declaration(bool local) {
        Qualifiers q = qualifiers();
        if (local && (q.storageSet || q.invariant)) fail("unsupported local qualifier");
        if (q.isConst && q.storageSet) fail("invalid qualifier combination");
        if (q.invariant && q.storage != Storage::Varying) fail("invariant requires varying");
        Type t = type();
        if (t.kind == Kind::Void) fail("void variable");
        if (peek() == ";" && !q.storageSet && !q.isConst && !q.invariant && !q.precisionSet) {
            if (!t.isStruct()) fail("expected identifier");
            ++at; return;
        }
        Storage storage = q.storageSet ? q.storage : Storage::Local;
        do {
            if (peek() == "(") fail(local ? "nested functions are not allowed" : "expected ;");
            std::string name = identifier();
            int count = 1; bool isArray = false;
            if (take("[")) { count = arraySize(); need("]"); isArray = true; }
            if (storage == Storage::Varying && ((t.matrix && !t.isStruct()) || t.sampler || containsSampler(t) || nonFloat(t))) fail("unsupported varying type");
            if (storage == Storage::Attribute && (t.matrix || t.sampler || t.isStruct() || count != 1 || isArray || !p.vertex || t.kind != Kind::Float))
                fail("unsupported attribute type/stage");
            bool ro = q.isConst || storage == Storage::Uniform || storage == Storage::Attribute || (storage == Storage::Varying && !p.vertex);
            auto& v = declare(name, t, storage, count, ro, isArray, q.invariant);
            int slot = v.slot;
            if (take("=")) {
                if (storage != Storage::Local) fail("unsupported global initializer");
                if (count != 1 || isArray) fail("unsupported array initializer");
                const size_t varIndex = p.variables.size() - 1;
                const bool foldable = q.isConst && !isArray && count == 1 && !t.sampler && !t.isStruct();
                size_t saved = at;
                std::optional<CVal> folded = foldable ? constTernary() : std::nullopt;
                const bool complete = folded && folded->kind == t.kind && folded->width == t.width && folded->matrix == t.matrix &&
                                      (peek() == "," || peek() == ";");
                at = saved;
                Expression dst = named(t, {}, true); dst.lvalue = true; dst.direct = true; dst.origin = slot;
                for (int n = 0; n < t.width; ++n) dst.slots.push_back(slot + n);
                store(dst, assignment());
                if (complete) constVals[varIndex] = *folded;
            } else if (q.isConst) fail("const requires initializer");
        } while (take(","));
        need(";");
    }
    bool isDeclStart() const {
        if (peek() == "const" || precision(peek()) || peek() == "struct") return true;
        if (typeName(peek()) || structs.count(peek())) return !(at + 1 < tokens.size() && tokens[at + 1] == "(");
        return false;
    }
    bool isGlobalStart() const {
        auto s = peek();
        return s == "const" || s == "attribute" || s == "uniform" || s == "varying" || s == "invariant" || precision(s) ||
               s == "struct" || typeName(s) || structs.count(s) > 0;
    }
    bool looksLikeFunction() const {
        size_t i = at;
        auto tok = [&](size_t n) { return n < tokens.size() ? tokens[n] : std::string(); };
        while (precision(tok(i))) ++i;
        if (tok(i) == "void" || typeName(tok(i)) || structs.count(tok(i))) ++i;
        else return false;
        if (!ppIdent(tok(i)) || keyword(tok(i))) return false;
        ++i;
        return tok(i) == "(";
    }
    void skipBody(size_t& begin, size_t& end) {
        need("{");
        begin = at;
        int depth = 1;
        while (at < tokens.size() && depth) {
            if (tokens[at] == "{") ++depth;
            else if (tokens[at] == "}") { if (--depth == 0) break; }
            ++at;
        }
        if (depth) fail("unterminated function");
        end = at; ++at;
    }
    void parseParams(std::vector<Param>& params) {
        need("(");
        if (take(")")) return;
        if (peek() == "void" && at + 1 < tokens.size() && tokens[at + 1] == ")") { ++at; need(")"); return; }
        do {
            Param param;
            if (take("inout")) param.qual = 2;
            else if (take("out")) param.qual = 1;
            else if (take("in")) param.qual = 0;
            if (precision(peek())) ++at;
            param.type = type();
            if (param.type.kind == Kind::Void) fail("void parameter");
            param.name = identifier();
            if (take("[")) { param.isArray = true; param.count = arraySize(); need("]"); }
            for (auto& existing : params) if (existing.name == param.name) fail("duplicate parameter " + param.name);
            params.push_back(std::move(param));
        } while (take(","));
        need(")");
    }
    bool paramIs(const Param& param, Kind k, int w) const {
        return !param.isArray && !param.type.matrix && !param.type.sampler && !param.type.isStruct() &&
               param.type.kind == k && param.type.width == w;
    }
    bool paramMat(const Param& param, int n) const {
        return !param.isArray && param.type.matrix == n && !param.type.isStruct();
    }
    bool paramSam(const Param& param, bool cube) const {
        return !param.isArray && param.type.sampler && param.type.cube == cube;
    }
    // Exact parameter-type match with a built-in overload. Return type and
    // parameter qualifiers are not part of an ES 1.00 signature. A different
    // parameter list is a legal overload.
    bool redefinesBuiltin(const std::string& name, const std::vector<Param>& ps) const {
        const bool deriv = name == "dFdx" || name == "dFdy" || name == "fwidth";
        if (deriv) { if (!derivatives) return false; }
        else if (!builtinName(name)) return false;
        const int n = int(ps.size());
        auto allWidth = [&](Kind k, int w) {
            for (int i = 0; i < n; ++i) if (!paramIs(ps[size_t(i)], k, w)) return false;
            return n > 0;
        };
        auto anyGen = [&](Kind k, int count) {
            if (n != count) return false;
            for (int w = 1; w <= 4; ++w) if (allWidth(k, w)) return true;
            return false;
        };
        auto genThenScalar = [&](Kind k, int vecs, int scalars) {
            if (n != vecs + scalars) return false;
            for (int w = 1; w <= 4; ++w) {
                bool ok = true;
                for (int i = 0; i < vecs; ++i) if (!paramIs(ps[size_t(i)], k, w)) ok = false;
                for (int i = 0; i < scalars; ++i) if (!paramIs(ps[size_t(vecs + i)], k, 1)) ok = false;
                if (ok) return true;
            }
            return false;
        };
        if (name == "radians" || name == "degrees" || name == "sin" || name == "cos" || name == "tan" || name == "asin" ||
            name == "acos" || name == "exp" || name == "log" || name == "exp2" || name == "log2" || name == "sqrt" ||
            name == "inversesqrt" || name == "floor" || name == "ceil" || name == "fract" || name == "length" ||
            name == "normalize" || deriv)
            return anyGen(Kind::Float, 1);
        if (name == "abs" || name == "sign") return anyGen(Kind::Float, 1) || anyGen(Kind::Int, 1);
        if (name == "atan") return anyGen(Kind::Float, 1) || anyGen(Kind::Float, 2);
        if (name == "pow" || name == "distance" || name == "dot" || name == "reflect") return anyGen(Kind::Float, 2);
        if (name == "mod") return anyGen(Kind::Float, 2) || genThenScalar(Kind::Float, 1, 1);
        if (name == "min" || name == "max")
            return anyGen(Kind::Float, 2) || anyGen(Kind::Int, 2) || genThenScalar(Kind::Float, 1, 1) || genThenScalar(Kind::Int, 1, 1);
        if (name == "clamp")
            return anyGen(Kind::Float, 3) || anyGen(Kind::Int, 3) || genThenScalar(Kind::Float, 1, 2) || genThenScalar(Kind::Int, 1, 2);
        if (name == "mix") return anyGen(Kind::Float, 3) || genThenScalar(Kind::Float, 2, 1);
        if (name == "step") {
            if (anyGen(Kind::Float, 2)) return true;
            if (n != 2) return false;
            for (int w = 1; w <= 4; ++w)
                if (paramIs(ps[0], Kind::Float, 1) && paramIs(ps[1], Kind::Float, w)) return true;
            return false;
        }
        if (name == "smoothstep") {
            if (anyGen(Kind::Float, 3)) return true;
            if (n != 3) return false;
            for (int w = 1; w <= 4; ++w)
                if (paramIs(ps[0], Kind::Float, 1) && paramIs(ps[1], Kind::Float, 1) && paramIs(ps[2], Kind::Float, w)) return true;
            return false;
        }
        if (name == "cross") return n == 2 && paramIs(ps[0], Kind::Float, 3) && paramIs(ps[1], Kind::Float, 3);
        if (name == "faceforward") return anyGen(Kind::Float, 3);
        if (name == "refract") {
            if (n != 3) return false;
            for (int w = 1; w <= 4; ++w)
                if (paramIs(ps[0], Kind::Float, w) && paramIs(ps[1], Kind::Float, w) && paramIs(ps[2], Kind::Float, 1)) return true;
            return false;
        }
        if (name == "matrixCompMult") {
            if (n != 2) return false;
            for (int m = 2; m <= 4; ++m) if (paramMat(ps[0], m) && paramMat(ps[1], m)) return true;
            return false;
        }
        if (name == "lessThan" || name == "lessThanEqual" || name == "greaterThan" || name == "greaterThanEqual" ||
            name == "equal" || name == "notEqual") {
            if (n != 2) return false;
            const bool eq = name == "equal" || name == "notEqual";
            for (int w = 2; w <= 4; ++w) {
                if (paramIs(ps[0], Kind::Float, w) && paramIs(ps[1], Kind::Float, w)) return true;
                if (paramIs(ps[0], Kind::Int, w) && paramIs(ps[1], Kind::Int, w)) return true;
                if (eq && paramIs(ps[0], Kind::Bool, w) && paramIs(ps[1], Kind::Bool, w)) return true;
            }
            return false;
        }
        if (name == "any" || name == "all" || name == "not") {
            if (n != 1) return false;
            for (int w = 2; w <= 4; ++w) if (paramIs(ps[0], Kind::Bool, w)) return true;
            return false;
        }
        auto vec = [&](int i, int w) { return i < n && paramIs(ps[size_t(i)], Kind::Float, w); };
        if (name == "texture2D")
            return (n == 2 && paramSam(ps[0], false) && vec(1, 2)) || (n == 3 && paramSam(ps[0], false) && vec(1, 2) && vec(2, 1));
        if (name == "texture2DLod") return n == 3 && paramSam(ps[0], false) && vec(1, 2) && vec(2, 1);
        if (name == "texture2DProj")
            return (n == 2 && paramSam(ps[0], false) && (vec(1, 3) || vec(1, 4))) ||
                   (n == 3 && paramSam(ps[0], false) && (vec(1, 3) || vec(1, 4)) && vec(2, 1));
        if (name == "texture2DProjLod")
            return n == 3 && paramSam(ps[0], false) && (vec(1, 3) || vec(1, 4)) && vec(2, 1);
        if (name == "textureCube")
            return (n == 2 && paramSam(ps[0], true) && vec(1, 3)) || (n == 3 && paramSam(ps[0], true) && vec(1, 3) && vec(2, 1));
        if (name == "textureCubeLod") return n == 3 && paramSam(ps[0], true) && vec(1, 3) && vec(2, 1);
        return false;
    }
    void parseFunction() {
        while (precision(peek())) ++at;
        Type ret = peek() == "void" ? (take("void"), tyVoid()) : type();
        std::string name = identifier();
        std::vector<Param> params;
        parseParams(params);
        if (redefinesBuiltin(name, params)) fail("cannot redefine built-in function");
        if (name == "main") {
            if (ret.kind != Kind::Void || !params.empty()) fail("main must be void main()");
            if (hasMain) fail("duplicate main");
            if (peek() == ";") fail("main must be defined");
            skipBody(mainBegin, mainEnd);
            hasMain = true; return;
        }
        if (findName(name)) fail("duplicate/reserved variable " + name);
        Func proto; proto.name = name; proto.ret = ret; proto.params = params;
        int match = -1, clash = -1;
        for (size_t i = 0; i < funcs.size(); ++i) {
            if (funcs[i].name != name) continue;
            if (sameParamTypes(funcs[i].params, params, true) && funcs[i].ret == ret) match = int(i);
            else if (sameParamTypes(funcs[i].params, params, false)) clash = int(i);
        }
        if (take(";")) {
            if (clash >= 0) fail("function prototype mismatch");
            if (match >= 0) return;
            funcs.push_back(std::move(proto)); return;
        }
        size_t begin = 0, end = 0;
        skipBody(begin, end);
        if (clash >= 0) fail("function prototype mismatch");
        if (match >= 0) {
            if (funcs[size_t(match)].defined) fail("duplicate function definition");
            funcs[size_t(match)].defined = true; funcs[size_t(match)].begin = begin; funcs[size_t(match)].end = end;
            funcs[size_t(match)].params = params; return;
        }
        proto.defined = true; proto.begin = begin; proto.end = end;
        funcs.push_back(std::move(proto));
    }
    void statement() {
        if (++stmtDepth > 256) fail("statement nesting too deep");
        struct Guard { int& d; ~Guard() { --d; } } guard{stmtDepth};
        if (at >= tokens.size()) fail("unterminated block");
        if (take(";")) return;
        if (take("{")) { pushScope(); while (!take("}")) statement(); popScope(); return; }
        if (take("if")) {
            need("("); auto c = expression(); need(")"); rvalue(c);
            if (!boolScalar(c)) fail("condition must be bool");
            int jf = jif(c.slots[0]);
            statement();
            if (take("else")) { int j = jump(0); patchF(jf, int(p.code.size())); statement(); patchJ(j, int(p.code.size())); }
            else patchF(jf, int(p.code.size()));
            return;
        }
        if (take("while")) {
            int condPc = int(p.code.size());
            need("("); auto c = expression(); need(")"); rvalue(c);
            if (!boolScalar(c)) fail("condition must be bool");
            int jf = jif(c.slots[0]);
            loops.push_back(Loop{}); loops.back().continuePc = condPc;
            statement();
            jump(condPc);
            int end = int(p.code.size()); patchF(jf, end);
            for (int b : loops.back().breaks) patchJ(b, end);
            loops.pop_back(); return;
        }
        if (take("do")) {
            int body = int(p.code.size());
            loops.push_back(Loop{});
            statement();
            need("while"); need("(");
            int condPc = int(p.code.size());
            for (int c : loops.back().continues) patchJ(c, condPc);
            auto cond = expression(); need(")"); need(";"); rvalue(cond);
            if (!boolScalar(cond)) fail("condition must be bool");
            int jf = jif(cond.slots[0]); jump(body);
            int end = int(p.code.size()); patchF(jf, end);
            for (int b : loops.back().breaks) patchJ(b, end);
            loops.pop_back(); return;
        }
        if (take("for")) {
            need("("); pushScope();
            if (!take(";")) { if (isDeclStart()) declaration(true); else { expression(); need(";"); } }
            int condPc = int(p.code.size()); int jf = -1;
            if (!take(";")) {
                auto c = expression(); rvalue(c);
                if (!boolScalar(c)) fail("condition must be bool");
                jf = jif(c.slots[0]); need(";");
            }
            size_t stepB = at; int depth = 0;
            while (at < tokens.size()) {
                if (peek() == "(") { ++depth; ++at; }
                else if (peek() == ")") { if (depth == 0) break; --depth; ++at; }
                else ++at;
            }
            size_t stepE = at; need(")");
            loops.push_back(Loop{});
            statement();
            int stepPc = int(p.code.size());
            for (int c : loops.back().continues) patchJ(c, stepPc);
            size_t saved = at; at = stepB;
            if (stepB != stepE) { expression(); if (at != stepE) fail("invalid for step"); }
            at = saved;
            jump(condPc);
            int end = int(p.code.size());
            if (jf >= 0) patchF(jf, end);
            for (int b : loops.back().breaks) patchJ(b, end);
            loops.pop_back(); popScope(); return;
        }
        if (take("break")) { if (loops.empty()) fail("break outside loop"); need(";"); loops.back().breaks.push_back(jump(0)); return; }
        if (take("continue")) {
            if (loops.empty()) fail("continue outside loop");
            need(";");
            if (loops.back().continuePc >= 0) jump(loops.back().continuePc);
            else loops.back().continues.push_back(jump(0));
            return;
        }
        if (take("discard")) {
            if (p.vertex) fail("discard is not allowed in a vertex shader");
            need(";"); p.code.push_back({Op::Discard, 0, 0, 0, 0, 0.f, 0}); return;
        }
        if (take("return")) {
            if (frames.empty()) fail("return outside function");
            size_t fi = frames.size() - 1;
            if (frames[fi].ret.kind == Kind::Void || frames[fi].isMain) {
                if (peek() != ";") fail("void function cannot return a value");
                need(";");
            } else {
                auto e = expression(); need(";");
                if (e.type != frames[fi].ret) fail("return type mismatch");
                rvalue(e);
                for (int c = 0; c < e.type.width; ++c) mov(frames[fi].result + c, e.slots[size_t(c)]);
            }
            frames[fi].returns.push_back(jump(0)); return;
        }
        if (isDeclStart()) { declaration(true); return; }
        expression(); need(";");
    }
    void declareBuiltins() {
        auto& output = declare(p.vertex ? "gl_Position" : "gl_FragColor", ty(Kind::Float, 4), Storage::Output, 1, false, false, false, true);
        p.output = output.slot;
        if (p.vertex) {
            auto& ps = declare("gl_PointSize", ty(Kind::Float, 1), Storage::Output, 1, false, false, false, true);
            p.code.push_back({Op::Constant, ps.slot, 0, 0, 0, 1.f, 0});
        } else {
            declare("gl_FragCoord", ty(Kind::Float, 4), Storage::Local, 1, true, false, false, true);
            declare("gl_FrontFacing", ty(Kind::Bool, 1), Storage::Local, 1, true, false, false, true);
            declare("gl_PointCoord", ty(Kind::Float, 2), Storage::Local, 1, true, false, false, true);
            Variable fd;
            fd.name = "gl_FragData"; fd.type = ty(Kind::Float, 4); fd.storage = Storage::Output; fd.slot = p.output;
            fd.count = 1; fd.array = true;
            scopes.back()["gl_FragData"] = p.variables.size();
            p.variables.push_back(std::move(fd));
        }
        Type dr; dr.width = 3;
        Type::Field near, far, diff;
        near.name = "near"; far.name = "far"; far.offset = 1; diff.name = "diff"; diff.offset = 2;
        dr.fields = {near, far, diff};
        declare("gl_DepthRange", dr, Storage::Local, 1, true, false, false, true);
        auto iconst = [&](const char* n, int v) {
            auto& var = declare(n, ty(Kind::Int, 1), Storage::Local, 1, true, false, false, true);
            p.code.push_back({Op::Constant, var.slot, 0, 0, 0, float(v), 0});
            CVal c; c.kind = Kind::Int; c.c[0] = v;
            constVals[p.variables.size() - 1] = c;
        };
        iconst("gl_MaxVertexAttribs", 16);
        iconst("gl_MaxVertexUniformVectors", 128);
        iconst("gl_MaxVaryingVectors", 8);
        iconst("gl_MaxVertexTextureImageUnits", 8);
        iconst("gl_MaxCombinedTextureImageUnits", 8);
        iconst("gl_MaxTextureImageUnits", 8);
        iconst("gl_MaxFragmentUniformVectors", 16);
        iconst("gl_MaxDrawBuffers", 1);
    }
    void tokenize(const std::string& source) {
        if (source.size() > 65536) fail("shader source too large");
        tokens = preprocessShader(source, p.vertex, derivatives, warnings);
        if (tokens.size() > 65536) fail("shader source too large");
    }
    void run(const std::string& source) {
        tokenize(source);
        pushScope();
        declareBuiltins();
        while (at < tokens.size()) {
            if (take("precision")) {
                if (!precision(peek())) fail("expected precision");
                ++at;
                auto kind = peek();
                if (kind != "float" && kind != "int" && kind != "sampler2D" && kind != "samplerCube") fail("unsupported precision type");
                ++at; need(";"); continue;
            }
            if (peek() == "invariant" && at + 1 < tokens.size()) {
                const auto& next = tokens[at + 1];
                bool qualified = next == "varying" || next == "attribute" || next == "uniform" || next == "const" ||
                    precision(next) || next == "struct" || typeName(next) || structs.count(next);
                if (!qualified) {
                    ++at; auto name = identifier(); need(";");
                    auto id = findName(name);
                    if (!id) fail("unknown invariant target " + name);
                    auto storage = p.variables[*id].storage;
                    if (storage != Storage::Output && storage != Storage::Varying) fail("invariant requires an output");
                    p.variables[*id].invariant = true; continue;
                }
            }
            if (looksLikeFunction()) { parseFunction(); continue; }
            if (!isGlobalStart()) fail("unsupported global declaration");
            declaration(false);
        }
        if (!hasMain) fail("missing main");
        at = mainBegin;
        pushScope();
        frames.push_back(Frame{tyVoid(), -1, {}, true});
        while (at < mainEnd) statement();
        if (at != mainEnd) fail("invalid main");
        int end = int(p.code.size());
        for (int j : frames.back().returns) patchJ(j, end);
        frames.pop_back();
        popScope();
        if (wroteFragColor && wroteFragData) fail("cannot write both gl_FragColor and gl_FragData");
        if (!wroteOutput) fail("shader never writes stage output");
    }
};
} // namespace

bool Program::compile(const std::string& source, bool isVertex) {
    *this = {}; vertex = isVertex;
    try {
        Compiler compiler{*this};
        compiler.run(source);
        for (const auto& w : compiler.warnings) {
            if (!warnings.empty()) warnings.push_back('\n');
            warnings += w;
        }
        return true;
    }
    catch (const std::exception& e) { error = e.what(); code.clear(); return false; }
}
const Variable* Program::find(const std::string& name) const {
    for (auto& v : variables) if (v.name == name) return &v;
    return nullptr;
}
void Program::initialize(Values& v) const { std::fill_n(v.begin(), slots, 0.0f); }

namespace {
void execOp(Values& v, const Instruction& i, Sample sample, void* context) {
    auto unitOf = [&](int slot) {
        float u = v[slot];
        return std::isfinite(u) && u >= 0 && u < 8 ? int(u) : -1;
    };
    auto sampleAt = [&](int dst, int unitSlot, float s, float t) {
        if (sample) sample(context, unitOf(unitSlot), s, t, &v[dst]);
        else std::fill_n(&v[dst], 4, 0.0f);
    };
    switch (i.op) {
        case Op::Constant: v[i.dst] = i.value; break;
        case Op::Move: v[i.dst] = v[i.a]; break;
        case Op::Add: v[i.dst] = v[i.a] + v[i.b]; break;
        case Op::Subtract: v[i.dst] = v[i.a] - v[i.b]; break;
        case Op::Multiply: v[i.dst] = v[i.a] * v[i.b]; break;
        case Op::Divide: v[i.dst] = v[i.a] / v[i.b]; break;
        case Op::IntDivide: {
            float d = v[i.b];
            v[i.dst] = d == 0.f ? 0.f : std::trunc(v[i.a] / d);
            break;
        }
        case Op::Texture: sampleAt(i.dst, i.a, v[i.b], v[i.c]); break;
        case Op::TextureCube: {
            float x = v[i.b], y = v[i.c], z = v[i.d];
            float ax = std::fabs(x), ay = std::fabs(y), az = std::fabs(z), sc, tc, ma;
            if (ax >= ay && ax >= az) { ma = ax; sc = x >= 0 ? -z : z; tc = -y; }
            else if (ay >= az) { ma = ay; sc = x; tc = y >= 0 ? z : -z; }
            else { ma = az; sc = z >= 0 ? x : -x; tc = -y; }
            float s = ma != 0.f ? 0.5f * (sc / ma + 1.f) : 0.5f;
            float t = ma != 0.f ? 0.5f * (tc / ma + 1.f) : 0.5f;
            sampleAt(i.dst, i.a, s, t);
            break;
        }
        case Op::Select: v[i.dst] = v[i.a] != 0.f ? v[i.b] : v[i.c]; break;
        case Op::Abs: v[i.dst] = std::fabs(v[i.a]); break;
        case Op::Floor: v[i.dst] = std::floor(v[i.a]); break;
        case Op::Ceil: v[i.dst] = std::ceil(v[i.a]); break;
        case Op::Sqrt: v[i.dst] = v[i.a] > 0.f ? std::sqrt(v[i.a]) : 0.f; break;
        case Op::InverseSqrt: v[i.dst] = v[i.a] > 0.f ? 1.f / std::sqrt(v[i.a]) : 0.f; break;
        case Op::Pow: v[i.dst] = std::pow(v[i.a], v[i.b]); break;
        case Op::Exp: v[i.dst] = std::exp(v[i.a]); break;
        case Op::Log: v[i.dst] = v[i.a] > 0.f ? std::log(v[i.a]) : 0.f; break;
        case Op::Exp2: v[i.dst] = std::exp2(v[i.a]); break;
        case Op::Log2: v[i.dst] = v[i.a] > 0.f ? std::log2(v[i.a]) : 0.f; break;
        case Op::Sin: v[i.dst] = std::sin(v[i.a]); break;
        case Op::Cos: v[i.dst] = std::cos(v[i.a]); break;
        case Op::Tan: v[i.dst] = std::tan(v[i.a]); break;
        case Op::Asin: v[i.dst] = std::asin(v[i.a]); break;
        case Op::Acos: v[i.dst] = std::acos(v[i.a]); break;
        case Op::Atan: v[i.dst] = std::atan(v[i.a]); break;
        case Op::Atan2: v[i.dst] = std::atan2(v[i.a], v[i.b]); break;
        case Op::Less: v[i.dst] = v[i.a] < v[i.b] ? 1.f : 0.f; break;
        case Op::LessEqual: v[i.dst] = v[i.a] <= v[i.b] ? 1.f : 0.f; break;
        case Op::Greater: v[i.dst] = v[i.a] > v[i.b] ? 1.f : 0.f; break;
        case Op::GreaterEqual: v[i.dst] = v[i.a] >= v[i.b] ? 1.f : 0.f; break;
        case Op::Equal: v[i.dst] = v[i.a] == v[i.b] ? 1.f : 0.f; break;
        case Op::NotEqual: v[i.dst] = v[i.a] != v[i.b] ? 1.f : 0.f; break;
        case Op::Not: v[i.dst] = v[i.a] == 0.f ? 1.f : 0.f; break;
        case Op::DFdx:
        case Op::DFdy:
            v[i.dst] = 0.f;
            break;
        case Op::Jump:
        case Op::JumpIfFalse:
        case Op::Discard:
            break;
    }
}
} // namespace

bool Program::execute(Values& v, Sample sample, void* context, int64_t* budget) const {
    int64_t local = 1000000;
    int64_t* left = budget ? budget : &local;
    for (size_t pc = 0; pc < code.size();) {
        if (*left <= 0) {
            if (budget) *budget = -1;
            return budget == nullptr;
        }
        --*left;
        const Instruction& i = code[pc];
        if (i.op == Op::Jump) { pc = size_t(i.a); continue; }
        if (i.op == Op::JumpIfFalse) {
            if (v[i.a] == 0.f) { pc = size_t(i.b); continue; }
        } else if (i.op == Op::Discard) return false;
        else execOp(v, i, sample, context);
        ++pc;
    }
    return true;
}

bool Program::executeQuad(std::array<Values, 4>& lanes, Sample sample, void* context, int64_t* budget,
                          int activeMask, int& discardedMask) const {
    size_t pc[4] = {};
    bool live[4] = {};
    discardedMask = 0;
    for (int lane = 0; lane < 4; ++lane) live[lane] = (activeMask & (1 << lane)) != 0;
    int64_t local = 1000000;
    int64_t* left = budget ? budget : &local;
    while (true) {
        size_t target = ~size_t(0);
        bool any = false;
        for (int lane = 0; lane < 4; ++lane) {
            if (!live[lane]) continue;
            if (pc[lane] >= code.size()) { live[lane] = false; continue; }
            any = true;
            if (pc[lane] < target) target = pc[lane];
        }
        if (!any) return true;
        int mask = 0, count = 0;
        for (int lane = 0; lane < 4; ++lane) if (live[lane] && pc[lane] == target) { mask |= 1 << lane; ++count; }
        if (*left < count) {
            if (budget) *budget = -1;
            return budget == nullptr;
        }
        *left -= count;
        const Instruction& ins = code[target];
        if (ins.op == Op::DFdx || ins.op == Op::DFdy) {
            for (int lane = 0; lane < 4; ++lane) if (mask & (1 << lane)) {
                const int hi = ins.op == Op::DFdx ? (lane | 1) : (lane | 2);
                const int lo = ins.op == Op::DFdx ? (lane & ~1) : (lane & ~2);
                lanes[lane][ins.dst] = lanes[hi][ins.a] - lanes[lo][ins.a];
                ++pc[lane];
            }
            continue;
        }
        if (ins.op == Op::Discard) {
            for (int lane = 0; lane < 4; ++lane) if (mask & (1 << lane)) {
                live[lane] = false;
                discardedMask |= 1 << lane;
            }
            continue;
        }
        if (ins.op == Op::Jump) {
            for (int lane = 0; lane < 4; ++lane) if (mask & (1 << lane)) pc[lane] = size_t(ins.a);
            continue;
        }
        if (ins.op == Op::JumpIfFalse) {
            for (int lane = 0; lane < 4; ++lane) if (mask & (1 << lane)) {
                if (lanes[lane][ins.a] == 0.f) pc[lane] = size_t(ins.b);
                else ++pc[lane];
            }
            continue;
        }
        for (int lane = 0; lane < 4; ++lane) if (mask & (1 << lane)) {
            execOp(lanes[lane], ins, sample, context);
            ++pc[lane];
        }
    }
}
} // namespace Hydrogen::Runtime::Shader
