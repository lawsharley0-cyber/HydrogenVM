#include "Shader.hpp"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <memory>
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
        if (std::isspace(c)) { space = true; ++i; continue; }
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
            const char* multi[] = {"##", "&&", "||", "<<", ">>", "<=", ">=", "==", "!=", "+=", "-=", "*=", "/="};
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
    struct Frame { bool parent = true, on = true, any = false, hadElse = false; };
    std::vector<Frame> stack;
    std::unordered_map<std::string, Macro> macros;
    std::vector<std::string> out;

    [[noreturn]] void fail(const std::string& why) const { ppFail(why, line); }
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
            if (t == "(" || t == "[" || t == "{") { ++depth; cur.push_back(in[k]); continue; }
            if (t == ")" || t == "]" || t == "}") {
                if (t == ")" && depth == 0) {
                    args.push_back(std::move(cur)); ++k;
                    if (int(args.size()) != nparams) fail("macro argument count");
                    return args;
                }
                if (depth == 0) fail("macro argument count");
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
            if (m.body[n].text == "#" && n + 1 < m.body.size() && paramIndex(m, m.body[n + 1].text) >= 0) {
                int p = paramIndex(m, m.body[n + 1].text);
                std::string spelling;
                for (size_t a = 0; a < raw[size_t(p)].size(); ++a) {
                    if (a) spelling += " ";
                    spelling += raw[size_t(p)][a].text;
                }
                if (!spelling.empty()) outToks.push_back({spelling, false, {}});
                n += 2; continue;
            }
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
    void expandSeq(const std::vector<PpTok>& in, std::vector<PpTok>& dest, int depth, int atLine) {
        if (depth > 256 || ++steps > 1000000) fail("macro expansion too deep");
        for (size_t k = 0; k < in.size();) {
            if (in[k].text == "\n") { ++atLine; ++k; continue; }
            const std::string& name = in[k].text;
            if (name == "__LINE__") { dest.push_back({std::to_string(atLine), false, {}}); ++k; continue; }
            if (name == "__FILE__") { dest.push_back({"0", false, {}}); ++k; continue; }
            auto it = macros.find(name);
            if (it != macros.end() && ppIdent(name) && !blocked(in[k], name)) {
                auto rescan = [&](std::vector<PpTok> repl, size_t rest) {
                    repl.insert(repl.end(), in.begin() + long(rest), in.end());
                    expandSeq(repl, dest, depth + 1, atLine);
                };
                if (it->second.functionLike) {
                    size_t j = k + 1;
                    while (j < in.size() && in[j].text == "\n") { ++atLine; ++j; }
                    if (j >= in.size() || in[j].text != "(") { dest.push_back(in[k]); ++k; continue; }
                    size_t pos = j;
                    auto raw = collectArgs(in, pos, int(it->second.params.size()));
                    std::vector<std::vector<PpTok>> expanded;
                    for (auto& arg : raw) {
                        std::vector<PpTok> one;
                        expandSeq(arg, one, depth + 1, atLine);
                        expanded.push_back(std::move(one));
                    }
                    rescan(paint(substitute(it->second, raw, expanded), in[k].skip, name), pos);
                    return;
                }
                rescan(paint(it->second.body, in[k].skip, name), k + 1);
                return;
            }
            dest.push_back(in[k]); ++k;
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
        long v = std::strtol(digits.c_str(), &end, base);
        if (!end || *end || end == digits.c_str()) fail("invalid preprocessor expression");
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
    long evalExpr(const std::vector<PpTok>& raw) {
        auto replaced = replaceDefined(raw);
        std::vector<PpTok> expanded; expandSeq(replaced, expanded, 0, line);
        if (expanded.empty()) fail("invalid preprocessor expression");
        struct Eval {
            Preprocessor& pp;
            const std::vector<PpTok>& t;
            size_t k = 0;
            [[noreturn]] void bad() { pp.fail("invalid preprocessor expression"); }
            bool more() const { return k < t.size(); }
            const std::string& peek() { if (!more()) bad(); return t[k].text; }
            bool take(const char* s) { if (more() && t[k].text == s) { ++k; return true; } return false; }
            long primary() {
                if (take("(")) { long v = expr(); if (!take(")")) bad(); return v; }
                auto s = peek(); ++k;
                if (ppIdent(s)) return 0;
                return pp.parseNumber(s);
            }
            long unary() {
                if (take("+")) return unary();
                if (take("-")) return -unary();
                if (take("!")) return unary() ? 0 : 1;
                if (take("~")) return ~unary();
                return primary();
            }
            long mul() {
                long v = unary();
                while (more() && (t[k].text == "*" || t[k].text == "/" || t[k].text == "%")) {
                    auto op = t[k].text; ++k; long r = unary();
                    if (op == "*") v *= r;
                    else if (r == 0) bad();
                    else if (op == "/") v /= r;
                    else v %= r;
                }
                return v;
            }
            long add() {
                long v = mul();
                while (more() && (t[k].text == "+" || t[k].text == "-")) { auto op = t[k].text; ++k; long r = mul(); v = op == "+" ? v + r : v - r; }
                return v;
            }
            long shift() {
                long v = add();
                while (more() && (t[k].text == "<<" || t[k].text == ">>")) {
                    auto op = t[k].text; ++k; long r = add();
                    if (r < 0 || r >= 63) bad();
                    auto bits = static_cast<uint64_t>(v);
                    v = static_cast<long>(op == "<<" ? bits << static_cast<unsigned>(r) : bits >> static_cast<unsigned>(r));
                }
                return v;
            }
            long rel() {
                long v = shift();
                while (more() && (t[k].text == "<" || t[k].text == ">" || t[k].text == "<=" || t[k].text == ">=")) {
                    auto op = t[k].text; ++k; long r = shift();
                    v = op == "<" ? v < r : op == ">" ? v > r : op == "<=" ? v <= r : v >= r;
                }
                return v;
            }
            long eq() {
                long v = rel();
                while (more() && (t[k].text == "==" || t[k].text == "!=")) { auto op = t[k].text; ++k; long r = rel(); v = op == "==" ? v == r : v != r; }
                return v;
            }
            long bitAnd() { long v = eq(); while (take("&")) v = static_cast<long>(static_cast<uint64_t>(v) & static_cast<uint64_t>(eq())); return v; }
            long bitXor() { long v = bitAnd(); while (take("^")) v = static_cast<long>(static_cast<uint64_t>(v) ^ static_cast<uint64_t>(bitAnd())); return v; }
            long bitOr() { long v = bitXor(); while (take("|")) v = static_cast<long>(static_cast<uint64_t>(v) | static_cast<uint64_t>(bitXor())); return v; }
            long land() { long v = bitOr(); while (take("&&")) v = (v && bitOr()) ? 1 : 0; return v; }
            long lor() { long v = land(); while (take("||")) v = (v || land()) ? 1 : 0; return v; }
            long expr() {
                long v = lor();
                if (take("?")) { long a = expr(); if (!take(":")) bad(); long b = expr(); v = v ? a : b; }
                return v;
            }
        };
        Eval ev{*this, expanded, 0};
        long value = ev.expr();
        if (ev.more()) fail("invalid preprocessor expression");
        return value;
    }
    void pushIf(bool parent, bool cond) { stack.push_back({parent, parent && cond, parent && cond, false}); }
    void directive(const std::string& op, const std::vector<PpTok>& rest) {
        if (op == "version") {
            if (started || emitted) fail("version directive must be first");
            if (rest.size() != 1 || rest[0].text != "100") fail("unsupported version");
            started = true; return;
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
            macros.erase(rest[0].text); return;
        }
        if (op == "error") {
            std::string msg = "error";
            for (auto& t : rest) { msg += " "; msg += t.text; }
            fail(msg);
        }
        if (op == "pragma") return;
        if (op == "line") {
            std::vector<PpTok> expanded; expandSeq(rest, expanded, 0, line);
            if (expanded.empty() || expanded.size() > 2) fail("invalid #line");
            long ln = parseNumber(expanded[0].text);
            if (expanded.size() == 2) parseNumber(expanded[1].text);
            if (ln < 1) fail("invalid #line");
            forceLine = true; forced = int(ln); return;
        }
        if (op == "extension") {
            if (emitted) fail("extension directive after code");
            if (rest.size() != 3 || rest[1].text != ":") fail("invalid #extension");
            const auto& name = rest[0].text; const auto& behavior = rest[2].text;
            if (behavior != "require" && behavior != "enable" && behavior != "warn" && behavior != "disable") fail("invalid extension behavior");
            if (name == "all" && (behavior == "require" || behavior == "enable")) fail("invalid extension");
            if (behavior == "require") fail("unsupported extension " + name);
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

std::vector<std::string> preprocessShader(const std::string& source) { return Preprocessor{}.run(source); }

struct Expression { Type type; std::vector<int> slots; };
struct Compiler {
    Program& p;
    std::vector<std::string> tokens;
    size_t at = 0;
    std::unordered_map<std::string, size_t> names;
    std::unordered_map<std::string, Type> structs;
    bool wroteOutput = false;
    int expressionDepth = 0;
    [[noreturn]] void fail(const std::string& why) const {
        throw std::runtime_error(why + " near token " + std::to_string(at) + " ('" + peek() + "')");
    }
    std::string peek() const { return at < tokens.size() ? tokens[at] : "<end>"; }
    bool take(const std::string& s) { if (peek() != s) return false; ++at; return true; }
    void need(const std::string& s) { if (!take(s)) fail("expected " + s); }
    std::string identifier() {
        auto s = peek();
        if (s.empty() || !(std::isalpha(uint8_t(s[0])) || s[0] == '_')) fail("expected identifier");
        ++at; return s;
    }
    int alloc(int n = 1) {
        if (n < 1 || p.slots + n > MaxSlots) fail("shader register limit exceeded");
        int out = p.slots; p.slots += n; return out;
    }
    int constant(float f) {
        int out = alloc(); p.code.push_back({Op::Constant, out, 0, 0, 0, f}); return out;
    }
    int binary(Op op, int a, int b) {
        int out = alloc(); p.code.push_back({op, out, a, b}); return out;
    }
    static bool precision(const std::string& s) { return s == "lowp" || s == "mediump" || s == "highp"; }
    static bool typeName(const std::string& s) {
        return s == "float" || s == "vec2" || s == "vec3" || s == "vec4" ||
               s == "mat2" || s == "mat3" || s == "mat4" || s == "sampler2D";
    }
    int arraySize() {
        auto n = peek(); char* end = nullptr;
        long k = std::strtol(n.c_str(), &end, 10);
        if (!end || *end || k < 1 || k > 32) fail("invalid array size");
        ++at; return int(k);
    }
    static Type typeFromField(const Type::Field& f) {
        Type t; t.width = f.width; t.matrix = f.matrix; t.sampler = f.sampler; t.fields = f.fields; return t;
    }
    bool containsSampler(const Type& t) const {
        if (t.sampler) return true;
        for (auto& f : t.fields) if (f.sampler || containsSampler(typeFromField(f))) return true;
        return false;
    }
    Type parseStruct() {
        std::string name;
        if (peek() != "{") name = identifier();
        if (!name.empty() && (structs.count(name) || names.count(name))) fail("duplicate struct " + name);
        need("{");
        std::vector<Type::Field> fields;
        int offset = 0;
        while (!take("}")) {
            if (at >= tokens.size()) fail("unterminated struct");
            if (precision(peek())) ++at;
            if (peek() == "const" || peek() == "attribute" || peek() == "uniform" || peek() == "varying" || peek() == "invariant")
                fail("unsupported struct qualifier");
            Type ft = type();
            do {
                std::string fname = identifier();
                for (auto& existing : fields) if (existing.name == fname) fail("duplicate struct field " + fname);
                int count = 1;
                bool isArray = false;
                if (take("[")) { count = arraySize(); need("]"); isArray = true; }
                if (peek() == "=") fail("unsupported struct field initializer");
                Type::Field field;
                field.name = fname; field.offset = offset; field.width = ft.width; field.count = count;
                field.matrix = ft.matrix; field.sampler = ft.sampler; field.array = isArray; field.fields = ft.fields;
                offset += ft.width * count;
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
        if (!typeName(t)) fail("unsupported type");
        ++at;
        if (t == "sampler2D") return {1, 0, true, {}};
        if (t == "float") return {};
        int n = t.back() - '0';
        return t[0] == 'm' ? Type{n * n, n, false, {}} : Type{n, 0, false, {}};
    }
    Variable& declare(const std::string& name, Type t, Storage storage, int count, bool ro, bool array = false) {
        if (names.count(name) || structs.count(name) || (name.starts_with("gl_") && storage != Storage::Output)) fail("duplicate/reserved variable " + name);
        if (count < 1 || count > 32) fail("array size outside supported range");
        int slot = alloc(t.width * count);
        names[name] = p.variables.size();
        p.variables.push_back({name, t, storage, slot, count, ro, array});
        return p.variables.back();
    }
    Expression reference(const Variable& v) {
        int base = v.slot;
        if (take("[")) {
            auto n = peek(); char* end = nullptr;
            long idx = std::strtol(n.c_str(), &end, 10);
            if (!end || *end || idx < 0 || idx >= v.count) fail("array index must be an in-range literal");
            ++at; need("]"); base += int(idx) * v.type.width;
        } else if (v.count != 1 || v.array) fail("array requires literal index");
        Expression e{v.type, {}};
        for (int i = 0; i < v.type.width; ++i) e.slots.push_back(base + i);
        return e;
    }
    Expression swizzle(Expression e) {
        while (take(".")) {
            if (!e.type.fields.empty()) {
                std::string name = identifier();
                const Type::Field* field = nullptr;
                for (auto& f : e.type.fields) if (f.name == name) field = &f;
                if (!field) fail("unknown struct field " + name);
                int element = 0;
                if (take("[")) {
                    auto n = peek(); char* end = nullptr;
                    long idx = std::strtol(n.c_str(), &end, 10);
                    if (!end || *end || idx < 0 || idx >= field->count) fail("array index must be an in-range literal");
                    ++at; need("]"); element = int(idx);
                } else if (field->count != 1 || field->array) fail("array requires literal index");
                int begin = field->offset + element * field->width;
                if (begin < 0 || begin + field->width > int(e.slots.size())) fail("struct field out of range");
                Expression out{typeFromField(*field), {}};
                for (int n = 0; n < field->width; ++n) out.slots.push_back(e.slots[size_t(begin + n)]);
                e = std::move(out);
                continue;
            }
            if (e.type.matrix || e.type.sampler || e.type.width == 1) fail("swizzle requires vector");
            std::string mask = identifier();
            if (mask.empty() || mask.size() > 4) fail("invalid swizzle length");
            const std::string sets[] = {"xyzw", "rgba", "stpq"};
            int set = -1;
            Expression out{{int(mask.size()), 0, false, {}}, {}};
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
                out.slots.push_back(e.slots[size_t(component)]);
            }
            e = std::move(out);
        }
        return e;
    }
    Expression arithmetic(const std::string& op, Expression a, Expression b) {
        if (a.type.sampler || b.type.sampler) fail("arithmetic on sampler");
        if (op == "*" && a.type.matrix && b.type.width == a.type.matrix && !b.type.matrix) {
            int n = a.type.matrix;
            Expression out{{n, 0, false, {}}, {}};
            for (int row = 0; row < n; ++row) {
                int sum = binary(Op::Multiply, a.slots[size_t(row)], b.slots[0]);
                for (int col = 1; col < n; ++col)
                    sum = binary(Op::Add, sum, binary(Op::Multiply, a.slots[size_t(col * n + row)], b.slots[size_t(col)]));
                out.slots.push_back(sum);
            }
            return out;
        }
        if (op == "*" && b.type.matrix && a.type.width == b.type.matrix && !a.type.matrix) {
            int n = b.type.matrix;
            Expression out{{n, 0, false, {}}, {}};
            for (int col = 0; col < n; ++col) {
                int sum = binary(Op::Multiply, a.slots[0], b.slots[size_t(col * n)]);
                for (int row = 1; row < n; ++row)
                    sum = binary(Op::Add, sum, binary(Op::Multiply, a.slots[size_t(row)], b.slots[size_t(col * n + row)]));
                out.slots.push_back(sum);
            }
            return out;
        }
        if (a.type.matrix || b.type.matrix) fail("unsupported matrix arithmetic");
        if (a.type.width != b.type.width && a.type.width != 1 && b.type.width != 1) fail("incompatible operand widths");
        int n = std::max(a.type.width, b.type.width);
        Expression out{{n, 0, false, {}}, {}};
        Op code = op == "+" ? Op::Add : op == "-" ? Op::Subtract : op == "*" ? Op::Multiply : Op::Divide;
        for (int i = 0; i < n; ++i)
            out.slots.push_back(binary(code, a.slots[size_t(a.type.width == 1 ? 0 : i)], b.slots[size_t(b.type.width == 1 ? 0 : i)]));
        return out;
    }
    Expression primary() {
        Expression e;
        if (take("-")) return arithmetic("*", {{}, {constant(-1)}}, expression(3));
        if (take("+")) return expression(3);
        if (take("(")) { e = expression(); need(")"); return swizzle(std::move(e)); }
        std::string tok = peek();
        if (!tok.empty() && (std::isdigit(uint8_t(tok[0])) || tok[0] == '.')) {
            char* end = nullptr;
            float v = std::strtof(tok.c_str(), &end);
            if (!end || *end) fail("invalid numeric literal");
            ++at; return {{}, {constant(v)}};
        }
        if (typeName(tok) || tok == "struct" || structs.count(tok) || tok == "texture2D" || tok == "dot") {
            Type t{};
            bool ctor = typeName(tok) || tok == "struct" || structs.count(tok);
            if (ctor) t = type(); else ++at;
            need("(");
            std::vector<Expression> args;
            if (!take(")")) {
                do { args.push_back(expression()); } while (take(","));
                need(")");
            }
            if (t.isStruct()) {
                if (args.size() != t.fields.size()) fail("struct constructor field count");
                e.type = t;
                for (size_t n = 0; n < args.size(); ++n) {
                    if (t.fields[n].count != 1) fail("unsupported array field constructor");
                    if (args[n].type != typeFromField(t.fields[n])) fail("struct constructor type mismatch");
                    e.slots.insert(e.slots.end(), args[n].slots.begin(), args[n].slots.end());
                }
            } else if (ctor) {
                if (t.sampler || args.empty()) fail("unsupported constructor");
                e.type = t;
                for (auto& a : args) {
                    if (a.type.sampler || a.type.matrix) fail("unsupported constructor argument");
                    e.slots.insert(e.slots.end(), a.slots.begin(), a.slots.end());
                }
                if (e.slots.size() == 1 && t.width > 1) {
                    int scalar = e.slots[0];
                    e.slots.assign(size_t(t.width), scalar);
                    if (t.matrix) for (int i = 0; i < t.width; ++i)
                        if (i / t.matrix != i % t.matrix) e.slots[size_t(i)] = constant(0);
                }
                if (int(e.slots.size()) != t.width) fail("constructor component count mismatch");
            } else if (tok == "dot") {
                if (args.size() != 2 || args[0].type != args[1].type || args[0].type.matrix || args[0].type.sampler) fail("invalid dot arguments");
                int sum = binary(Op::Multiply, args[0].slots[0], args[1].slots[0]);
                for (int i = 1; i < args[0].type.width; ++i)
                    sum = binary(Op::Add, sum, binary(Op::Multiply, args[0].slots[size_t(i)], args[1].slots[size_t(i)]));
                e = {{}, {sum}};
            } else {
                if (args.size() != 2 || !args[0].type.sampler || args[1].type != Type{2, 0, false, {}}) fail("invalid texture2D arguments");
                int out = alloc(4);
                p.code.push_back({Op::Texture, out, args[0].slots[0], args[1].slots[0], args[1].slots[1]});
                e = {{4, 0, false, {}}, {out, out + 1, out + 2, out + 3}};
            }
        } else {
            std::string name = identifier();
            if (!names.count(name)) fail("unknown variable/function " + name);
            e = reference(p.variables[names.at(name)]);
        }
        return swizzle(std::move(e));
    }
    Expression expression(int minPrec = 0) {
        if (++expressionDepth > 64) fail("expression nesting limit exceeded");
        auto lhs = primary();
        while (true) {
            auto op = peek();
            int prec = op == "+" || op == "-" ? 1 : op == "*" || op == "/" ? 2 : -1;
            if (prec < minPrec) break;
            ++at;
            lhs = arithmetic(op, std::move(lhs), expression(prec + 1));
        }
        --expressionDepth;
        return lhs;
    }
    void assign(Expression dst, Expression src) {
        if (dst.type != src.type) fail("assignment type mismatch");
        // Snapshot RHS before any stores, including overlapping swizzles.
        std::vector<int> temp;
        for (int s : src.slots) { int t = alloc(); p.code.push_back({Op::Move, t, s}); temp.push_back(t); }
        for (size_t i = 0; i < dst.slots.size(); ++i) p.code.push_back({Op::Move, dst.slots[i], temp[i]});
    }
    struct Qualifiers {
        bool invariant = false, isConst = false, storageSet = false, precisionSet = false;
        Storage storage = Storage::Local;
    };
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
        if (peek() == ";" && !q.storageSet && !q.isConst && !q.invariant && !q.precisionSet) {
            if (!t.isStruct()) fail("expected identifier");
            ++at; return;
        }
        Storage storage = q.storageSet ? q.storage : Storage::Local;
        do {
            std::string name = identifier();
            int count = 1;
            bool isArray = false;
            if (take("[")) { count = arraySize(); need("]"); isArray = true; }
            if (storage == Storage::Varying && ((t.matrix && !t.isStruct()) || t.sampler || containsSampler(t))) fail("unsupported varying type");
            if (storage == Storage::Attribute && (t.matrix || t.sampler || t.isStruct() || count != 1 || isArray || !p.vertex)) fail("unsupported attribute type/stage");
            bool ro = q.isConst || storage == Storage::Uniform || storage == Storage::Attribute || (storage == Storage::Varying && !p.vertex);
            auto& v = declare(name, t, storage, count, ro, isArray);
            int slot = v.slot;
            if (take("=")) {
                if (storage != Storage::Local) fail("unsupported global initializer");
                if (count != 1) fail("unsupported array initializer");
                Expression dst{t, {}};
                for (int n = 0; n < t.width; ++n) dst.slots.push_back(slot + n);
                assign(dst, expression());
            } else if (q.isConst) fail("const requires initializer");
        } while (take(","));
        need(";");
    }
    void tokenize(const std::string& source) {
        if (source.size() > 65536) fail("shader source too large");
        tokens = preprocessShader(source);
        if (tokens.size() > 65536) fail("shader source too large");
    }
    void run(const std::string& source) {
        tokenize(source);
        auto& output = declare(p.vertex ? "gl_Position" : "gl_FragColor", {4, 0, false, {}}, Storage::Output, 1, false);
        p.output = output.slot;
        while (peek() != "void" && at < tokens.size()) {
            if (take("precision")) {
                if (!precision(peek())) fail("expected precision");
                ++at;
                auto kind = peek();
                if (kind != "float" && kind != "int" && kind != "sampler2D") fail("unsupported precision type");
                ++at; need(";"); continue;
            }
            if (peek() == "invariant" && at + 1 < tokens.size()) {
                const auto& next = tokens[at + 1];
                bool qualified = next == "varying" || next == "attribute" || next == "uniform" || next == "const" ||
                    precision(next) || next == "struct" || typeName(next) || structs.count(next);
                if (!qualified) {
                    ++at; auto name = identifier(); need(";");
                    auto it = names.find(name);
                    if (it == names.end()) fail("unknown invariant target " + name);
                    auto storage = p.variables[it->second].storage;
                    if (storage != Storage::Output && storage != Storage::Varying) fail("invariant requires an output");
                    continue;
                }
            }
            if (peek() != "const" && peek() != "attribute" && peek() != "uniform" && peek() != "varying" && peek() != "invariant" &&
                !precision(peek()) && peek() != "struct" && !typeName(peek()) && !structs.count(peek()))
                fail("unsupported global declaration");
            declaration(false);
        }
        need("void"); need("main"); need("("); take("void"); need(")"); need("{");
        while (!take("}")) {
            if (at >= tokens.size()) fail("unterminated main");
            if (peek() == "const" || precision(peek()) || typeName(peek()) || peek() == "struct" || structs.count(peek())) { declaration(true); continue; }
            if (take(";")) continue;
            auto name = identifier();
            if (!names.count(name)) fail("unknown assignment target " + name);
            auto v = p.variables[names.at(name)];
            if (v.readOnly) fail("assignment to read-only variable");
            auto dst = swizzle(reference(v));
            auto sorted = dst.slots; std::sort(sorted.begin(), sorted.end());
            if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) fail("duplicate destination swizzle");
            std::string op = peek();
            if (op != "=" && op != "+=" && op != "-=" && op != "*=" && op != "/=") fail("unsupported statement");
            ++at; auto rhs = expression();
            if (op != "=") rhs = arithmetic(op.substr(0, 1), dst, rhs);
            assign(dst, rhs); need(";");
            if (v.storage == Storage::Output) wroteOutput = true;
        }
        if (at != tokens.size()) fail("unsupported code after main");
        if (!wroteOutput) fail("shader never writes stage output");
    }
};
} // namespace

bool Program::compile(const std::string& source, bool isVertex) {
    *this = {}; vertex = isVertex;
    try { Compiler{*this, {}, 0, {}, {}, false, 0}.run(source); return true; }
    catch (const std::exception& e) { error = e.what(); code.clear(); return false; }
}
const Variable* Program::find(const std::string& name) const {
    for (auto& v : variables) if (v.name == name) return &v;
    return nullptr;
}
void Program::initialize(Values& v) const { std::fill_n(v.begin(), slots, 0.0f); }
void Program::execute(Values& v, Sample sample, void* context) const {
    for (auto& i : code) {
        switch (i.op) {
            case Op::Constant: v[i.dst] = i.value; break;
            case Op::Move: v[i.dst] = v[i.a]; break;
            case Op::Add: v[i.dst] = v[i.a] + v[i.b]; break;
            case Op::Subtract: v[i.dst] = v[i.a] - v[i.b]; break;
            case Op::Multiply: v[i.dst] = v[i.a] * v[i.b]; break;
            case Op::Divide: v[i.dst] = v[i.a] / v[i.b]; break;
            case Op::Texture:
                if (sample) sample(context, std::isfinite(v[i.a]) && v[i.a] >= 0 && v[i.a] < 8 ? int(v[i.a]) : -1, v[i.b], v[i.c], &v[i.dst]);
                else std::fill_n(&v[i.dst], 4, 0.0f);
                break;
        }
    }
}
} // namespace Hydrogen::Runtime::Shader
