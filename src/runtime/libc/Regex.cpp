// POSIX regcomp/regexec/regerror/regfree for the 32-bit Darwin ABI.
//
// Layout verified against Apple Libc-583 include/regex.h and xnu-1456.1.26
// bsd/sys/_types.h (the headers contemporary with the iOS 3/4 SDK these apps
// were built with; the same layout is still in current Libc include/_regex.h):
//
//   typedef __int64_t __darwin_off_t;          // unconditional, including ILP32
//   typedef __darwin_off_t regoff_t;           // so regoff_t is 64-bit on armv7
//   regex_t {                                  // 16 bytes, align 4
//       int    re_magic;                       // 0
//       size_t re_nsub;                        // 4   (size_t is 4 bytes)
//       const char*    re_endp;                // 8
//       struct re_guts* re_g;                  // 12  (cookie, not a host pointer)
//   };
//   regmatch_t { regoff_t rm_so; regoff_t rm_eo; };  // 16 bytes, align 4
//
// armv7 Darwin aligns this struct to 4 (clang -target armv7-apple-ios:
// sizeof 16, alignof 4). A lone int64 prefers 8; the struct does not.
// The stride is still 16. Host glibc regoff_t is 32-bit and is not used.
//
// Unbounded repeats are a Thompson NFA: the leftmost-longest span is the
// furthest accept in an O(states * length) simulation, then a greedy walk
// over a reverse-reachability table fills POSIX submatches. Backreferences
// cannot be expressed that way and use memoized backtracking only. There is
// no fixed repeat ceiling and no step limit on the NFA path.
//
// REG_STARTEND (an eflag) is the Darwin/BSD flag, value 4. The region
// pmatch[0] is the whole string: ^ matches at rm_so unless REG_NOTBOL, and
// $ matches at rm_eo unless REG_NOTEOL. Reported offsets are from the start
// of the guest pointer. (GNU libc anchors ^ at the original start instead.)
//
// The matcher is original. The regerror strings are the BSD-3-Clause messages
// from FreeBSD lib/libc/regex/regerror.c, which is the text Darwin publishes:
//
//   Copyright (c) 1992, 1993, 1994 Henry Spencer.
//   Copyright (c) 1992, 1993, 1994
//   The Regents of the University of California. All rights reserved.
//
//   Redistribution and use in source and binary forms, with or without
//   modification, are permitted provided that the following conditions
//   are met:
//   1. Redistributions of source code must retain the above copyright
//      notice, this list of conditions and the following disclaimer.
//   2. Redistributions in binary form must reproduce the above copyright
//      notice, this list of conditions and the following disclaimer in the
//      documentation and/or other materials provided with the distribution.
//   3. Neither the name of the University nor the names of its contributors
//      may be used to endorse or promote products derived from this software
//      without specific prior written permission.
//
//   THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
//   ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
//   IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
//   ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
//   FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
//   DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
//   SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
//   CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
//   LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
//   OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
//   SUCH DAMAGE.
#include "runtime/Process.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <locale>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Hydrogen::Runtime {
namespace {

constexpr int32_t REG_EXTENDED = 1, REG_ICASE = 2, REG_NOSUB = 4, REG_NEWLINE = 010;
constexpr int32_t REG_NOTBOL = 1, REG_NOTEOL = 2, REG_STARTEND = 4;
constexpr int32_t REG_NOMATCH = 1, REG_BADPAT = 2, REG_ECOLLATE = 3, REG_ECTYPE = 4;
constexpr int32_t REG_EESCAPE = 5, REG_ESUBREG = 6, REG_EBRACK = 7, REG_EPAREN = 8;
constexpr int32_t REG_EBRACE = 9, REG_BADBR = 10, REG_ERANGE = 11, REG_ESPACE = 12;
constexpr int32_t REG_BADRPT = 13, REG_EMPTY = 14, REG_ASSERT = 15, REG_INVARG = 16;
constexpr int32_t REG_ILLSEQ = 17, REG_ATOI = 255, REG_ITOA = 0400;
constexpr int32_t kMagic = 0x52454758; // 'REGX'
constexpr int kDupMax = 255;

struct ErrText { int code; const char* name; const char* explain; };
// BSD/Darwin regerror table, including the unknown-code sentinel wording.
constexpr ErrText kErrs[] = {
    {REG_NOMATCH, "REG_NOMATCH", "regexec() failed to match"},
    {REG_BADPAT, "REG_BADPAT", "invalid regular expression"},
    {REG_ECOLLATE, "REG_ECOLLATE", "invalid collating element"},
    {REG_ECTYPE, "REG_ECTYPE", "invalid character class"},
    {REG_EESCAPE, "REG_EESCAPE", "trailing backslash (\\)"},
    {REG_ESUBREG, "REG_ESUBREG", "invalid backreference number"},
    {REG_EBRACK, "REG_EBRACK", "brackets ([ ]) not balanced"},
    {REG_EPAREN, "REG_EPAREN", "parentheses not balanced"},
    {REG_EBRACE, "REG_EBRACE", "braces not balanced"},
    {REG_BADBR, "REG_BADBR", "invalid repetition count(s)"},
    {REG_ERANGE, "REG_ERANGE", "invalid character range"},
    {REG_ESPACE, "REG_ESPACE", "out of memory"},
    {REG_BADRPT, "REG_BADRPT", "repetition-operator operand invalid"},
    {REG_EMPTY, "REG_EMPTY", "empty (sub)expression"},
    {REG_ASSERT, "REG_ASSERT", "\"can't happen\" -- you found a bug"},
    {REG_INVARG, "REG_INVARG", "invalid argument to regex routine"},
    {REG_ILLSEQ, "REG_ILLSEQ", "illegal byte sequence"},
};
constexpr const char* kUnknown = "*** unknown regexp error code ***";

const ErrText* findErr(int code) {
    for (const ErrText& e : kErrs) if (e.code == code) return &e;
    return nullptr;
}

enum class K : uint8_t { Empty, Lit, Any, Class, Concat, Alt, Rep, Cap, Bol, Eol, Ref };

struct Node {
    K k = K::Empty;
    uint8_t ch = 0;
    bool neg = false;
    int min = 0, max = -1;
    int group = 0;
    int a = -1, b = -1;
    uint32_t bits[8]{};
};

enum class Op : uint8_t { Char, Any, Class, Split, Jmp, Match, Save, Bol, Eol };

struct Inst {
    Op op = Op::Jmp;
    int x = 0;
    int y = 0;
    int aux = 0;     // Save group, or class index
    bool open = false;
    bool neg = false;
    uint8_t ch = 0;
};

struct Prog {
    std::vector<Node> nodes;
    int root = -1;
    int nsub = 0;
    bool icase = false, newline = false, nosub = false, hasRef = false;
    std::vector<Inst> inst;
    std::vector<std::array<uint32_t, 8>> classes;
    int entry = 0;
    int matchPc = 0;
};

struct RegexState {
    explicit RegexState(Process&) {}
    std::mutex mu;
    uint32_t next = 1;
    std::unordered_map<uint32_t, std::shared_ptr<Prog>> live;
};

// ctype<char> of the classic locale. ctype<unsigned char> is not installed and
// std::tolower(unsigned char, locale) throws std::bad_cast.
const std::ctype<char>& cctype() {
    static const std::ctype<char>& ct = std::use_facet<std::ctype<char>>(std::locale::classic());
    return ct;
}
int fold(unsigned char c) { return static_cast<unsigned char>(cctype().tolower(static_cast<char>(c))); }

class Parser {
public:
    Parser(const std::string& in, int cflags) : in_(in), ere_(cflags & REG_EXTENDED), icase_(cflags & REG_ICASE), newline_(cflags & REG_NEWLINE) {}
    int compile(Prog& out) {
        prog_ = &out;
        out.icase = icase_;
        out.newline = newline_;
        out.nosub = false;
        const int root = parseAlt(true);
        if (!err_ && more()) err_ = atGroupEnd() ? REG_EPAREN : REG_BADPAT;
        if (err_) return err_;
        out.root = root;
        out.nsub = nsub_;
        return 0;
    }

private:
    const std::string& in_;
    bool ere_, icase_, newline_;
    size_t i_ = 0;
    int err_ = 0, nsub_ = 0;
    Prog* prog_ = nullptr;

    bool more() const { return i_ < in_.size(); }
    char peek() const { return more() ? in_[i_] : 0; }
    char get() { return in_[i_++]; }
    bool atGroupEnd() const {
        if (!more()) return false;
        if (ere_) return in_[i_] == ')';
        return i_ + 1 < in_.size() && in_[i_] == '\\' && in_[i_ + 1] == ')';
    }
    bool atAlt() const {
        if (!more()) return false;
        if (ere_) return in_[i_] == '|';
        return i_ + 1 < in_.size() && in_[i_] == '\\' && in_[i_ + 1] == '|';
    }
    int mk(K k) { prog_->nodes.push_back(Node{}); prog_->nodes.back().k = k; return int(prog_->nodes.size() - 1); }
    void setBit(int node, unsigned char c) { prog_->nodes[node].bits[c >> 5] |= 1u << (c & 31); }
    void addChar(int node, unsigned char c) {
        setBit(node, c);
        if (icase_) {
            const auto& ct = cctype();
            setBit(node, uint8_t(static_cast<unsigned char>(ct.tolower(static_cast<char>(c)))));
            setBit(node, uint8_t(static_cast<unsigned char>(ct.toupper(static_cast<char>(c)))));
        }
    }
    bool namedClass(int node, const std::string& name) {
        // POSIX classes follow LC_CTYPE. The guest has no locale object; use the
        // C locale so a host setlocale cannot change [[:alpha:]] and the rest.
        const auto& ct = cctype();
        auto pred = [&](auto fn) { for (int c = 0; c < 256; ++c) if (fn(uint8_t(c))) addChar(node, uint8_t(c)); };
        using M = std::ctype_base;
        if (name == "alnum") pred([&](unsigned char c) { return ct.is(M::alnum, static_cast<char>(c)); });
        else if (name == "alpha") pred([&](unsigned char c) { return ct.is(M::alpha, static_cast<char>(c)); });
        else if (name == "blank") pred([](unsigned char c) { return c == ' ' || c == '\t'; });
        else if (name == "cntrl") pred([&](unsigned char c) { return ct.is(M::cntrl, static_cast<char>(c)); });
        else if (name == "digit") pred([&](unsigned char c) { return ct.is(M::digit, static_cast<char>(c)); });
        else if (name == "graph") pred([&](unsigned char c) { return ct.is(M::graph, static_cast<char>(c)); });
        else if (name == "lower") pred([&](unsigned char c) { return ct.is(M::lower, static_cast<char>(c)); });
        else if (name == "print") pred([&](unsigned char c) { return ct.is(M::print, static_cast<char>(c)); });
        else if (name == "punct") pred([&](unsigned char c) { return ct.is(M::punct, static_cast<char>(c)); });
        else if (name == "space") pred([&](unsigned char c) { return ct.is(M::space, static_cast<char>(c)); });
        else if (name == "upper") pred([&](unsigned char c) { return ct.is(M::upper, static_cast<char>(c)); });
        else if (name == "xdigit") pred([&](unsigned char c) { return ct.is(M::xdigit, static_cast<char>(c)); });
        else return false;
        return true;
    }
    unsigned char classChar() {
        if (!more()) { err_ = REG_EBRACK; return 0; }
        char c = get();
        if (c == '\\') {
            if (!more()) { err_ = REG_EESCAPE; return 0; }
            return uint8_t(get());
        }
        return uint8_t(c);
    }
    int parseClass() {
        const int node = mk(K::Class);
        bool neg = false;
        if (peek() == '^') { neg = true; get(); }
        if (peek() == ']') addChar(node, uint8_t(get())); // a leading ] is literal
        while (more() && peek() != ']') {
            if (peek() == '[' && i_ + 1 < in_.size() && (in_[i_ + 1] == ':' || in_[i_ + 1] == '.' || in_[i_ + 1] == '=')) {
                const char kind = in_[i_ + 1];
                if (kind != ':') { err_ = REG_ECOLLATE; return -1; }
                i_ += 2;
                const size_t start = i_;
                while (more() && !(peek() == ':' && i_ + 1 < in_.size() && in_[i_ + 1] == ']')) get();
                if (!(more() && peek() == ':' && i_ + 1 < in_.size() && in_[i_ + 1] == ']')) { err_ = REG_ECTYPE; return -1; }
                const std::string name = in_.substr(start, i_ - start);
                i_ += 2;
                if (!namedClass(node, name)) { err_ = REG_ECTYPE; return -1; }
                continue;
            }
            const unsigned char a = classChar();
            if (err_) return -1;
            if (peek() == '-' && i_ + 1 < in_.size() && in_[i_ + 1] != ']') {
                get();
                const unsigned char b = classChar();
                if (err_) return -1;
                if (b < a) { err_ = REG_ERANGE; return -1; }
                for (int c = a; c <= b; ++c) addChar(node, uint8_t(c));
            } else addChar(node, a);
        }
        if (peek() != ']') { err_ = REG_EBRACK; return -1; }
        get();
        prog_->nodes[node].neg = neg;
        return node;
    }
    int readNum() {
        if (!std::isdigit(static_cast<unsigned char>(peek()))) { err_ = REG_BADBR; return 0; }
        int v = 0;
        while (std::isdigit(static_cast<unsigned char>(peek()))) {
            v = v * 10 + (get() - '0');
            if (v > kDupMax) { err_ = REG_BADBR; return v; }
        }
        return v;
    }
    bool hasQuant() const {
        if (!more()) return false;
        if (ere_ && (peek() == '*' || peek() == '+' || peek() == '?' || peek() == '{')) return true;
        if (!ere_ && peek() == '*') return true;
        if (!ere_ && i_ + 1 < in_.size() && in_[i_] == '\\' && (in_[i_ + 1] == '{' || in_[i_ + 1] == '+' || in_[i_ + 1] == '?')) return true;
        return false;
    }
    int parseAtom() {
        if (!more()) { err_ = REG_BADPAT; return -1; }
        if (ere_ && (peek() == '*' || peek() == '+' || peek() == '?' || peek() == '{')) { err_ = REG_BADRPT; return -1; }
        if (!ere_ && peek() == '*') { err_ = REG_BADRPT; return -1; }
        const char c = get();
        if (c == '.') return mk(K::Any);
        if (c == '[') return parseClass();
        if (ere_ && c == '(') return parseGroup();
        if (c == '\\') {
            if (!more()) { err_ = REG_EESCAPE; return -1; }
            const char e = get();
            if (!ere_ && e == '(') return parseGroup();
            if (e >= '1' && e <= '9') {
                const int g = e - '0';
                if (g > nsub_) { err_ = REG_ESUBREG; return -1; }
                const int n = mk(K::Ref);
                prog_->nodes[n].group = g;
                return n;
            }
            const int n = mk(K::Lit);
            prog_->nodes[n].ch = uint8_t(e);
            return n;
        }
        const int n = mk(K::Lit);
        prog_->nodes[n].ch = uint8_t(c);
        return n;
    }
    int parseGroup() {
        const int id = ++nsub_;
        const int inner = parseAlt(true);
        if (err_) return -1;
        if (ere_) {
            if (peek() != ')') { err_ = REG_EPAREN; return -1; }
            get();
        } else if (!(more() && peek() == '\\' && i_ + 1 < in_.size() && in_[i_ + 1] == ')')) {
            err_ = REG_EPAREN;
            return -1;
        } else i_ += 2;
        const int n = mk(K::Cap);
        prog_->nodes[n].group = id;
        prog_->nodes[n].a = inner;
        return n;
    }
    int add(int acc, bool& any, int node) {
        if (!any) { any = true; return node; }
        const int c = mk(K::Concat);
        prog_->nodes[c].a = acc;
        prog_->nodes[c].b = node;
        return c;
    }
    int parseConcat(bool bolOk) {
        int acc = mk(K::Empty);
        bool any = false;
        if (peek() == '^' && (ere_ || bolOk)) {
            get();
            acc = add(acc, any, mk(K::Bol));
        }
        while (more() && !atGroupEnd() && !atAlt()) {
            if (peek() == '$' && (ere_ || dollarAnchor())) {
                get();
                acc = add(acc, any, mk(K::Eol));
                continue;
            }
            if (ere_ && peek() == '^') {
                get();
                acc = add(acc, any, mk(K::Bol));
                continue;
            }
            if ((ere_ && (peek() == '*' || peek() == '+' || peek() == '?' || peek() == '{')) || (!ere_ && peek() == '*')) {
                err_ = REG_BADRPT;
                return -1;
            }
            int atom = parseAtom();
            if (err_) return -1;
            if (hasQuant()) {
                const K kind = prog_->nodes[atom].k;
                if (kind == K::Bol || kind == K::Eol || kind == K::Rep || kind == K::Empty) { err_ = REG_BADRPT; return -1; }
                int min = 0, max = -1;
                if (!parseQuant(min, max)) return -1;
                const int r = mk(K::Rep);
                prog_->nodes[r].a = atom;
                prog_->nodes[r].min = min;
                prog_->nodes[r].max = max;
                atom = r;
            }
            acc = add(acc, any, atom);
        }
        return err_ ? -1 : acc;
    }
    bool dollarAnchor() const {
        // BRE: '$' is an anchor at the end, or just before \) or \|.
        if (i_ + 1 >= in_.size()) return true;
        if (in_[i_ + 1] == '\\' && i_ + 2 < in_.size() && (in_[i_ + 2] == ')' || in_[i_ + 2] == '|')) return true;
        return false;
    }
    bool parseQuant(int& min, int& max) {
        if (peek() == '*') { get(); min = 0; max = -1; return true; }
        if (ere_ && peek() == '+') { get(); min = 1; max = -1; return true; }
        if (ere_ && peek() == '?') { get(); min = 0; max = 1; return true; }
        if (!ere_ && peek() == '\\' && i_ + 1 < in_.size() && in_[i_ + 1] == '+') { i_ += 2; min = 1; max = -1; return true; }
        if (!ere_ && peek() == '\\' && i_ + 1 < in_.size() && in_[i_ + 1] == '?') { i_ += 2; min = 0; max = 1; return true; }
        if (ere_ && peek() == '{') get();
        else if (!ere_ && peek() == '\\' && i_ + 1 < in_.size() && in_[i_ + 1] == '{') i_ += 2;
        else { err_ = REG_BADRPT; return false; }
        // '{' without a count is an unclosed brace (REG_EBRACE), not a bad count.
        if (!std::isdigit(static_cast<unsigned char>(peek()))) { err_ = REG_EBRACE; return false; }
        min = readNum();
        if (err_) return false;
        if (peek() == ',') {
            get();
            if (std::isdigit(static_cast<unsigned char>(peek()))) {
                max = readNum();
                if (err_) return false;
            } else max = -1;
        } else max = min;
        if (ere_) {
            if (peek() != '}') { err_ = REG_EBRACE; return false; }
            get();
        } else if (!(peek() == '\\' && i_ + 1 < in_.size() && in_[i_ + 1] == '}')) {
            err_ = REG_EBRACE;
            return false;
        } else i_ += 2;
        if (max >= 0 && min > max) { err_ = REG_BADBR; return false; }
        return true;
    }
    int parseAlt(bool bolOk) {
        int left = parseConcat(bolOk);
        if (err_) return -1;
        while (atAlt()) {
            if (ere_) get();
            else i_ += 2;
            const int right = parseConcat(true);
            if (err_) return -1;
            const int n = mk(K::Alt);
            prog_->nodes[n].a = left;
            prog_->nodes[n].b = right;
            left = n;
        }
        return left;
    }
};

struct Caps { std::vector<int64_t> so, eo; };

bool treeHasRef(const Prog& p, int ni) {
    if (ni < 0) return false;
    const Node& n = p.nodes[ni];
    if (n.k == K::Ref) return true;
    return treeHasRef(p, n.a) || treeHasRef(p, n.b);
}

struct Frag {
    int start = 0;
    std::vector<std::pair<int, char>> out; // instruction index, 0 = x, 1 = y
};

// Thompson NFA. Splits prefer x (the greedy / left alternative). Finite
// intervals are unrolled so {m,n} cannot run past n; unbounded repeats are
// real loops, so a long a* is one state, not a 4096-capped recursion.
void compileNfa(Prog& prog) {
    std::vector<Inst>& I = prog.inst;
    auto emit = [&](Op op) {
        Inst in;
        in.op = op;
        in.x = -1;
        in.y = -1;
        I.push_back(in);
        return int(I.size() - 1);
    };
    auto patch = [&](Frag& f, int t) {
        for (auto [i, slot] : f.out) {
            if (slot == 0) I[i].x = t;
            else I[i].y = t;
        }
    };
    auto concat = [&](Frag a, Frag b) {
        patch(a, b.start);
        return Frag{a.start, std::move(b.out)};
    };
    std::function<Frag(int)> compile = [&](int ni) -> Frag {
        const Node& n = prog.nodes[ni];
        switch (n.k) {
        case K::Empty: {
            const int j = emit(Op::Jmp);
            return {j, {{j, 0}}};
        }
        case K::Lit: {
            const int j = emit(Op::Char);
            I[j].ch = n.ch;
            return {j, {{j, 0}}};
        }
        case K::Any: {
            const int j = emit(Op::Any);
            return {j, {{j, 0}}};
        }
        case K::Class: {
            const int j = emit(Op::Class);
            I[j].aux = int(prog.classes.size());
            I[j].neg = n.neg;
            std::array<uint32_t, 8> bits{};
            for (int b = 0; b < 8; ++b) bits[b] = n.bits[b];
            prog.classes.push_back(bits);
            return {j, {{j, 0}}};
        }
        case K::Bol: {
            const int j = emit(Op::Bol);
            return {j, {{j, 0}}};
        }
        case K::Eol: {
            const int j = emit(Op::Eol);
            return {j, {{j, 0}}};
        }
        case K::Concat:
            return concat(compile(n.a), compile(n.b));
        case K::Alt: {
            Frag a = compile(n.a);
            Frag b = compile(n.b);
            const int s = emit(Op::Split);
            I[s].x = a.start;
            I[s].y = b.start;
            auto outs = std::move(a.out);
            outs.insert(outs.end(), b.out.begin(), b.out.end());
            return {s, std::move(outs)};
        }
        case K::Cap: {
            Frag inner = compile(n.a);
            const int o = emit(Op::Save);
            I[o].aux = n.group;
            I[o].open = true;
            I[o].x = inner.start;
            const int c = emit(Op::Save);
            I[c].aux = n.group;
            I[c].open = false;
            patch(inner, c);
            return {o, {{c, 0}}};
        }
        case K::Ref: {
            const int j = emit(Op::Jmp);
            return {j, {{j, 0}}};
        }
        case K::Rep: {
            const int body = n.a;
            const int mn = n.min;
            const int mx = n.max;
            auto star = [&](Frag b) {
                const int s = emit(Op::Split);
                const int j = emit(Op::Jmp);
                patch(b, j);
                I[j].x = s;
                I[s].x = b.start;
                return Frag{s, {{s, 1}}};
            };
            auto opt = [&](Frag b) {
                const int s = emit(Op::Split);
                I[s].x = b.start;
                auto outs = std::move(b.out);
                outs.push_back({s, 1});
                return Frag{s, std::move(outs)};
            };
            if (mx < 0) {
                Frag tail = star(compile(body));
                if (mn <= 0) return tail;
                Frag head = compile(body);
                for (int i = 1; i < mn; ++i) head = concat(std::move(head), compile(body));
                return concat(std::move(head), std::move(tail));
            }
            if (mx == 0) {
                const int j = emit(Op::Jmp);
                return {j, {{j, 0}}};
            }
            if (mn <= 0) {
                Frag acc = opt(compile(body));
                for (int i = 1; i < mx; ++i) acc = concat(std::move(acc), opt(compile(body)));
                return acc;
            }
            Frag head = compile(body);
            for (int i = 1; i < mn; ++i) head = concat(std::move(head), compile(body));
            for (int i = mn; i < mx; ++i) head = concat(std::move(head), opt(compile(body)));
            return head;
        }
        }
        const int j = emit(Op::Jmp);
        return {j, {{j, 0}}};
    };
    Frag f = compile(prog.root);
    const int match = emit(Op::Match);
    patch(f, match);
    prog.entry = f.start;
    prog.matchPc = match;
}

struct Engine {
    const Prog& prog;
    const std::string& s;
    size_t lo = 0, hi = 0;
    bool notbol = false, noteol = false;
    std::vector<uint8_t> reach;
    size_t origin = 0;
    int bend = 0;

    bool isBol(size_t pos) const {
        if (pos == lo) return !notbol;
        return prog.newline && pos > lo && s[pos - 1] == '\n';
    }
    bool isEol(size_t pos) const {
        if (pos == hi) return !noteol;
        return prog.newline && pos < hi && s[pos] == '\n';
    }
    bool classBit(const Inst& in, unsigned char c) const {
        const auto& bits = prog.classes[size_t(in.aux)];
        return (bits[c >> 5] >> (c & 31)) & 1u;
    }
    bool consumes(const Inst& in, size_t pos) const {
        if (pos >= hi) return false;
        const unsigned char c = uint8_t(s[pos]);
        switch (in.op) {
        case Op::Char: return prog.icase ? fold(c) == fold(in.ch) : c == in.ch;
        case Op::Any: return !(prog.newline && c == '\n');
        case Op::Class: {
            bool inn = classBit(in, c);
            if (in.neg) inn = !inn;
            if (prog.newline && in.neg && c == '\n') inn = false;
            return inn;
        }
        default: return false;
        }
    }
    void closure(std::vector<int>& states, size_t pos) const {
        std::vector<char> seen(prog.inst.size(), 0);
        std::vector<int> stack;
        stack.reserve(states.size());
        for (int s0 : states) if (s0 >= 0 && !seen[s0]) { seen[s0] = 1; stack.push_back(s0); }
        states.clear();
        states.reserve(stack.size());
        for (int s0 : stack) states.push_back(s0);
        for (size_t sp = 0; sp < stack.size(); ++sp) {
            const Inst& in = prog.inst[stack[sp]];
            auto add = [&](int nxt) {
                if (nxt < 0 || seen[nxt]) return;
                seen[nxt] = 1;
                stack.push_back(nxt);
                states.push_back(nxt);
            };
            switch (in.op) {
            case Op::Split: add(in.x); add(in.y); break;
            case Op::Jmp: case Op::Save: add(in.x); break;
            case Op::Bol: if (isBol(pos)) add(in.x); break;
            case Op::Eol: if (isEol(pos)) add(in.x); break;
            default: break;
            }
        }
    }
    static bool hasMatch(const Prog& prog, const std::vector<int>& states) {
        for (int pc : states) if (prog.inst[pc].op == Op::Match) return true;
        return false;
    }
    // Furthest accept reachable from `start`, or -1. O(states * remaining length).
    int longestEnd(size_t start) const {
        const int ninst = int(prog.inst.size());
        std::vector<int> cur{prog.entry};
        closure(cur, start);
        int best = hasMatch(prog, cur) ? int(start) : -1;
        std::vector<char> mark(ninst);
        for (size_t pos = start; pos < hi; ++pos) {
            std::fill(mark.begin(), mark.end(), 0);
            std::vector<int> nxt;
            for (int pc : cur) {
                const Inst& in = prog.inst[pc];
                if (!consumes(in, pos) || in.x < 0 || mark[in.x]) continue;
                mark[in.x] = 1;
                nxt.push_back(in.x);
            }
            if (nxt.empty()) break;
            cur.swap(nxt);
            closure(cur, pos + 1);
            if (hasMatch(prog, cur)) best = int(pos + 1);
        }
        return best;
    }
    bool okAt(size_t pos, int pc) const {
        if (pc < 0 || pos < origin || int(pos) > bend) return false;
        return reach[(pos - origin) * prog.inst.size() + size_t(pc)] != 0;
    }
    // From each instruction at each offset in [start, bestEnd], can a path
    // still reach the accept at exactly bestEnd? Computed back-to-front so
    // the greedy walk can ask the question in O(1).
    void buildReach(size_t start, int bestEnd) {
        origin = start;
        bend = bestEnd;
        const int ninst = int(prog.inst.size());
        const int span = bestEnd - int(start) + 1;
        reach.assign(size_t(span) * size_t(ninst), 0);
        std::vector<std::vector<int>> rev(ninst);
        for (int i = 0; i < ninst; ++i) {
            const Inst& in = prog.inst[i];
            auto link = [&](int succ) { if (succ >= 0 && succ < ninst) rev[succ].push_back(i); };
            switch (in.op) {
            case Op::Split: link(in.x); link(in.y); break;
            case Op::Jmp: case Op::Save: case Op::Bol: case Op::Eol: link(in.x); break;
            default: break;
            }
        }
        auto at = [&](int pos, int pc) -> uint8_t& {
            return reach[size_t(pos - int(origin)) * size_t(ninst) + size_t(pc)];
        };
        for (int pos = bestEnd; pos >= int(start); --pos) {
            std::vector<int> q;
            auto mark = [&](int pc) {
                if (pc < 0 || pc >= ninst || at(pos, pc)) return;
                at(pos, pc) = 1;
                q.push_back(pc);
            };
            if (pos == bestEnd) mark(prog.matchPc);
            if (pos < bestEnd) {
                for (int i = 0; i < ninst; ++i) {
                    const Inst& in = prog.inst[i];
                    if ((in.op == Op::Char || in.op == Op::Any || in.op == Op::Class) &&
                        consumes(in, size_t(pos)) && in.x >= 0 && at(pos + 1, in.x))
                        mark(i);
                }
            }
            for (size_t qi = 0; qi < q.size(); ++qi) {
                for (int pred : rev[q[qi]]) {
                    const Inst& in = prog.inst[pred];
                    if (in.op == Op::Bol && !isBol(size_t(pos))) continue;
                    if (in.op == Op::Eol && !isEol(size_t(pos))) continue;
                    mark(pred);
                }
            }
        }
    }
    // True when every successful epsilon path from pc falls back to `home`
    // without consuming input. That is an empty iteration; POSIX does not
    // repeat it, and taking it would spin.
    bool spins(int pc, size_t pos, int home, std::vector<char>& seen) const {
        if (pc == home || pc < 0 || seen[pc]) return true;
        seen[pc] = 1;
        const Inst& in = prog.inst[pc];
        switch (in.op) {
        case Op::Char: case Op::Any: case Op::Class: case Op::Match:
            return false;
        case Op::Split: {
            const bool cx = okAt(pos, in.x);
            const bool cy = okAt(pos, in.y);
            if (cx && cy) {
                std::vector<char> nested(prog.inst.size(), 0);
                if (!spins(in.x, pos, pc, nested)) return false;
                return spins(in.y, pos, home, seen);
            }
            if (cx) return spins(in.x, pos, home, seen);
            if (cy) return spins(in.y, pos, home, seen);
            return true;
        }
        default:
            return spins(in.x, pos, home, seen);
        }
    }
    void walk(size_t start, int bestEnd, Caps& caps) {
        const int ninst = int(prog.inst.size());
        std::vector<int> seenAt(ninst, -1);
        int pc = prog.entry;
        size_t pos = start;
        const int guardMax = ninst * (bestEnd - int(start) + 3) + 8;
        for (int guard = 0; guard < guardMax; ++guard) {
            const Inst& in = prog.inst[pc];
            switch (in.op) {
            case Op::Match:
                caps.so[0] = int64_t(start);
                caps.eo[0] = int64_t(bestEnd);
                return;
            case Op::Save:
                if (in.aux > 0 && in.aux < int(caps.so.size())) {
                    if (in.open) caps.so[in.aux] = int64_t(pos);
                    else caps.eo[in.aux] = int64_t(pos);
                }
                pc = in.x;
                break;
            case Op::Jmp:
            case Op::Bol:
            case Op::Eol:
                if (seenAt[pc] == int(pos)) return;
                seenAt[pc] = int(pos);
                pc = in.x;
                break;
            case Op::Char:
            case Op::Any:
            case Op::Class:
                ++pos;
                pc = in.x;
                break;
            case Op::Split: {
                if (seenAt[pc] == int(pos)) { pc = in.y >= 0 ? in.y : in.x; break; }
                seenAt[pc] = int(pos);
                const bool cx = okAt(pos, in.x);
                const bool cy = okAt(pos, in.y);
                if (cx && cy) {
                    std::vector<char> seen(ninst, 0);
                    pc = spins(in.x, pos, pc, seen) ? in.y : in.x;
                } else if (cx) pc = in.x;
                else pc = in.y;
                break;
            }
            }
            if (pc < 0) return;
        }
    }
};

struct Hit { size_t end = 0; Caps caps; int count = 0; };

struct RefKey {
    int node = 0;
    uint32_t pos = 0;
    std::vector<int64_t> so, eo;
    bool operator==(const RefKey& o) const { return node == o.node && pos == o.pos && so == o.so && eo == o.eo; }
};
struct RefHash {
    size_t operator()(const RefKey& k) const {
        size_t h = (size_t(k.node) << 16) ^ k.pos;
        for (auto v : k.so) h = h * 1315423911u + size_t(v);
        for (auto v : k.eo) h = h * 1315423911u + size_t(v + 1);
        return h;
    }
};

// Memoized backtracking, used only when the pattern contains a backreference.
struct RefEngine {
    const Prog& prog;
    const std::string& s;
    size_t lo = 0, hi = 0;
    bool notbol = false, noteol = false;
    std::unordered_map<RefKey, std::vector<Hit>, RefHash> memo;
    std::unordered_map<RefKey, char, RefHash> stacking;

    bool isBol(size_t pos) const {
        if (pos == lo) return !notbol;
        return prog.newline && pos > lo && s[pos - 1] == '\n';
    }
    bool isEol(size_t pos) const {
        if (pos == hi) return !noteol;
        return prog.newline && pos < hi && s[pos] == '\n';
    }
    std::vector<Hit> all(int ni, size_t pos, const Caps& caps) {
        if (ni < 0 || pos > hi) return {};
        RefKey key{ni, uint32_t(pos), caps.so, caps.eo};
        if (auto it = memo.find(key); it != memo.end()) return it->second;
        if (stacking[key]) return {};
        stacking[key] = 1;
        const Node& n = prog.nodes[ni];
        std::vector<Hit> out;
        auto add = [&](Hit h) {
            for (const auto& e : out) if (e.end == h.end) return;
            out.push_back(std::move(h));
        };
        switch (n.k) {
        case K::Empty: add({pos, caps, 0}); break;
        case K::Lit:
            if (pos < hi && (prog.icase ? fold(uint8_t(s[pos])) == fold(n.ch) : uint8_t(s[pos]) == n.ch))
                add({pos + 1, caps, 0});
            break;
        case K::Any:
            if (pos < hi && !(prog.newline && s[pos] == '\n')) add({pos + 1, caps, 0});
            break;
        case K::Class:
            if (pos < hi) {
                const unsigned char c = uint8_t(s[pos]);
                bool inn = (n.bits[c >> 5] >> (c & 31)) & 1u;
                if (n.neg) inn = !inn;
                if (prog.newline && n.neg && c == '\n') inn = false;
                if (inn) add({pos + 1, caps, 0});
            }
            break;
        case K::Bol: if (isBol(pos)) add({pos, caps, 0}); break;
        case K::Eol: if (isEol(pos)) add({pos, caps, 0}); break;
        case K::Cap:
            for (auto h : all(n.a, pos, caps)) {
                if (n.group > 0 && n.group < int(h.caps.so.size())) {
                    h.caps.so[n.group] = int64_t(pos);
                    h.caps.eo[n.group] = int64_t(h.end);
                }
                add(std::move(h));
            }
            break;
        case K::Ref: {
            const int g = n.group;
            if (g > 0 && g < int(caps.so.size()) && caps.so[g] >= 0 && caps.eo[g] >= caps.so[g]) {
                const size_t len = size_t(caps.eo[g] - caps.so[g]);
                if (pos + len <= hi) {
                    bool eq = true;
                    for (size_t k = 0; k < len && eq; ++k) {
                        const unsigned char a = uint8_t(s[pos + k]);
                        const unsigned char b = uint8_t(s[size_t(caps.so[g]) + k]);
                        eq = prog.icase ? fold(a) == fold(b) : a == b;
                    }
                    if (eq) add({pos + len, caps, 0});
                }
            }
            break;
        }
        case K::Alt:
            for (auto h : all(n.a, pos, caps)) add(std::move(h));
            for (auto h : all(n.b, pos, caps)) add(std::move(h));
            break;
        case K::Concat: {
            auto left = all(n.a, pos, caps);
            std::sort(left.begin(), left.end(), [](const Hit& a, const Hit& b) { return a.end > b.end; });
            for (const auto& h : left)
                for (auto r : all(n.b, h.end, h.caps)) add(std::move(r));
            break;
        }
        case K::Rep: {
            const int limit = n.max < 0 ? int(hi - pos) : n.max;
            struct Frame { size_t p; int count; Caps c; };
            std::vector<Frame> stack{{pos, 0, caps}};
            std::unordered_map<size_t, Hit> best;
            std::unordered_map<uint64_t, char> seen;
            while (!stack.empty()) {
                Frame fr = std::move(stack.back());
                stack.pop_back();
                const uint64_t sk = (uint64_t(fr.count) << 32) ^ fr.p;
                if (seen[sk]) continue;
                seen[sk] = 1;
                if (fr.count >= n.min) {
                    auto it = best.find(fr.p);
                    if (it == best.end() || fr.count > it->second.count) best[fr.p] = Hit{fr.p, fr.c, fr.count};
                }
                if (fr.count >= limit) continue;
                auto parts = all(n.a, fr.p, fr.c);
                std::sort(parts.begin(), parts.end(), [](const Hit& a, const Hit& b) { return a.end < b.end; });
                for (auto& h : parts) {
                    if (h.end <= fr.p) continue;
                    stack.push_back(Frame{h.end, fr.count + 1, std::move(h.caps)});
                }
            }
            for (auto& [_, h] : best) add(std::move(h));
            break;
        }
        }
        stacking[key] = 0;
        memo.emplace(std::move(key), out);
        return out;
    }
};

bool matchProgram(const Prog& prog, const std::string& s, size_t lo, size_t hi, bool notbol, bool noteol, Caps& caps) {
    caps.so.assign(size_t(prog.nsub) + 1, -1);
    caps.eo.assign(size_t(prog.nsub) + 1, -1);
    if (prog.hasRef) {
        RefEngine eng{prog, s, lo, hi, notbol, noteol, {}, {}};
        for (size_t start = lo; start <= hi; ++start) {
            Caps init = caps;
            auto hits = eng.all(prog.root, start, init);
            bool any = false;
            size_t best = 0;
            Hit chosen;
            for (auto& h : hits) if (!any || h.end > best) { any = true; best = h.end; chosen = std::move(h); }
            if (!any) continue;
            caps = std::move(chosen.caps);
            caps.so.resize(size_t(prog.nsub) + 1, -1);
            caps.eo.resize(size_t(prog.nsub) + 1, -1);
            caps.so[0] = int64_t(start);
            caps.eo[0] = int64_t(best);
            return true;
        }
        return false;
    }
    Engine eng{prog, s, lo, hi, notbol, noteol, {}, 0, 0};
    for (size_t start = lo; start <= hi; ++start) {
        const int best = eng.longestEnd(start);
        if (best < 0) continue;
        eng.buildReach(start, best);
        caps.so.assign(size_t(prog.nsub) + 1, -1);
        caps.eo.assign(size_t(prog.nsub) + 1, -1);
        eng.walk(start, best, caps);
        caps.so[0] = int64_t(start);
        caps.eo[0] = int64_t(best);
        return true;
    }
    return false;
}

void clearGuest(Ctx& x, GuestPtr preg) {
    if (!preg) return;
    for (uint32_t i = 0; i < 16; i += 4) x.mem.write<uint32_t>(preg + i, 0);
}

int32_t r_regcomp(Ctx& x, GuestPtr preg, GuestPtr pattern, int32_t cflags) {
    if (!preg) return REG_INVARG;
    clearGuest(x, preg);
    if (!pattern) return REG_INVARG;
    Prog prog;
    prog.nosub = (cflags & REG_NOSUB) != 0;
    const int err = Parser(x.proc.readCString(pattern), cflags).compile(prog);
    if (err) return err;
    prog.nosub = (cflags & REG_NOSUB) != 0;
    prog.hasRef = treeHasRef(prog, prog.root);
    if (!prog.hasRef) compileNfa(prog);
    const uint32_t nsub = uint32_t(prog.nsub);
    auto& st = x.proc.service<RegexState>();
    std::lock_guard<std::mutex> lk(st.mu);
    uint32_t id = st.next++;
    if (!id) id = st.next++;
    st.live.emplace(id, std::make_shared<Prog>(std::move(prog)));
    x.mem.write<int32_t>(preg + 0, kMagic);
    x.mem.write<uint32_t>(preg + 4, nsub);
    x.mem.write<uint32_t>(preg + 8, 0);
    x.mem.write<uint32_t>(preg + 12, id);
    return 0;
}

void r_regfree(Ctx& x, GuestPtr preg) {
    if (!preg || x.mem.read<int32_t>(preg) != kMagic) return;
    const uint32_t id = x.mem.read<uint32_t>(preg + 12);
    {
        auto& st = x.proc.service<RegexState>();
        std::lock_guard<std::mutex> lk(st.mu);
        st.live.erase(id);
    }
    clearGuest(x, preg);
}

uint32_t r_regerror(Ctx& x, int32_t errcode, GuestPtr preg, GuestPtr buf, uint32_t size) {
    const char* text = kUnknown;
    char num[32];
    if (errcode == REG_ATOI) {
        std::string name = preg ? x.proc.readCString(x.mem.read<uint32_t>(preg + 8)) : std::string();
        const ErrText* e = nullptr;
        for (const ErrText& row : kErrs) if (name == row.name) { e = &row; break; }
        std::snprintf(num, sizeof num, "%d", e ? e->code : 0);
        text = num;
    } else if (errcode & REG_ITOA) {
        const ErrText* e = findErr(errcode & ~REG_ITOA);
        if (e) text = e->name;
        else { std::snprintf(num, sizeof num, "REG_0x%x", errcode & ~REG_ITOA); text = num; }
    } else if (const ErrText* e = findErr(errcode)) text = e->explain;
    const size_t need = std::strlen(text) + 1;
    if (buf && size > 0) {
        const size_t copy = std::min(need - 1, size_t(size - 1));
        x.mem.copyIn(buf, text, copy);
        x.mem.write<uint8_t>(buf + uint32_t(copy), 0);
    }
    return uint32_t(need);
}

int32_t r_regexec(Ctx& x, GuestPtr preg, GuestPtr text, uint32_t nmatch, GuestPtr pmatch, int32_t eflags) {
    if (!preg || x.mem.read<int32_t>(preg) != kMagic) return REG_INVARG;
    // REG_STARTEND reads pmatch[0] even when nmatch is 0.
    if ((eflags & REG_STARTEND) && !pmatch) return REG_INVARG;
    if (nmatch && !pmatch) return REG_INVARG;
    const uint32_t id = x.mem.read<uint32_t>(preg + 12);
    std::shared_ptr<Prog> prog;
    {
        auto& st = x.proc.service<RegexState>();
        std::lock_guard<std::mutex> lk(st.mu);
        auto it = st.live.find(id);
        if (it == st.live.end()) return REG_INVARG;
        prog = it->second;
    }
    if (!text) return REG_INVARG;
    std::string s;
    size_t lo = 0, hi = 0;
    if (eflags & REG_STARTEND) {
        const int64_t so = x.mem.read<int64_t>(pmatch);
        const int64_t eo = x.mem.read<int64_t>(pmatch + 8);
        if (so < 0 || eo < so || eo > 16 * 1024 * 1024) return REG_INVARG;
        s.resize(size_t(eo));
        if (eo) x.mem.copyOut(s.data(), text, size_t(eo));
        lo = size_t(so);
        hi = size_t(eo);
    } else {
        s = x.proc.readCString(text);
        hi = s.size();
    }
    Caps caps;
    const bool matched = matchProgram(*prog, s, lo, hi, (eflags & REG_NOTBOL) != 0, (eflags & REG_NOTEOL) != 0, caps);
    if (!matched) return REG_NOMATCH;
    if (!prog->nosub && pmatch && nmatch) {
        // Slots past re_nsub + 1 are defined to be -1, up through nmatch.
        for (uint32_t i = 0; i < nmatch; ++i) {
            int64_t so = -1, eo = -1;
            if (i <= uint32_t(prog->nsub)) {
                if (i < caps.so.size()) so = caps.so[i];
                if (i < caps.eo.size()) eo = caps.eo[i];
            }
            x.mem.write<int64_t>(pmatch + i * 16, so);
            x.mem.write<int64_t>(pmatch + i * 16 + 8, eo);
        }
    }
    return 0;
}

} // namespace

void registerLibcRegex(Process& p) {
    p.def<&r_regcomp>("_regcomp");
    p.def<&r_regexec>("_regexec");
    p.def<&r_regerror>("_regerror");
    p.def<&r_regfree>("_regfree");
}

} // namespace Hydrogen::Runtime
