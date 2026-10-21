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
//   regmatch_t { regoff_t rm_so; regoff_t rm_eo; };  // 16 bytes, align 8
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
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Hydrogen::Runtime {
namespace {

constexpr int32_t REG_EXTENDED = 1, REG_ICASE = 2, REG_NOSUB = 4, REG_NEWLINE = 010;
constexpr int32_t REG_NOTBOL = 1, REG_NOTEOL = 2;
constexpr int32_t REG_NOMATCH = 1, REG_BADPAT = 2, REG_ECOLLATE = 3, REG_ECTYPE = 4;
constexpr int32_t REG_EESCAPE = 5, REG_ESUBREG = 6, REG_EBRACK = 7, REG_EPAREN = 8;
constexpr int32_t REG_EBRACE = 9, REG_BADBR = 10, REG_ERANGE = 11, REG_ESPACE = 12;
constexpr int32_t REG_BADRPT = 13, REG_EMPTY = 14, REG_ASSERT = 15, REG_INVARG = 16;
constexpr int32_t REG_ILLSEQ = 17, REG_ATOI = 255, REG_ITOA = 0400;
constexpr int32_t kMagic = 0x52454758; // 'REGX'
constexpr int kStepLimit = 2000000;
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

struct Prog {
    std::vector<Node> nodes;
    int root = -1;
    int nsub = 0;
    bool icase = false, newline = false, nosub = false;
};

struct RegexState {
    explicit RegexState(Process&) {}
    std::mutex mu;
    uint32_t next = 1;
    std::unordered_map<uint32_t, std::shared_ptr<Prog>> live;
};

int fold(unsigned char c) { return std::tolower(c); }

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
        if (icase_) { setBit(node, uint8_t(std::tolower(c))); setBit(node, uint8_t(std::toupper(c))); }
    }
    bool namedClass(int node, const std::string& name) {
        auto pred = [&](auto fn) { for (int c = 0; c < 256; ++c) if (fn(uint8_t(c))) addChar(node, uint8_t(c)); };
        if (name == "alnum") pred([](unsigned char c) { return std::isalnum(c); });
        else if (name == "alpha") pred([](unsigned char c) { return std::isalpha(c); });
        else if (name == "blank") pred([](unsigned char c) { return c == ' ' || c == '\t'; });
        else if (name == "cntrl") pred([](unsigned char c) { return std::iscntrl(c); });
        else if (name == "digit") pred([](unsigned char c) { return std::isdigit(c); });
        else if (name == "graph") pred([](unsigned char c) { return std::isgraph(c); });
        else if (name == "lower") pred([](unsigned char c) { return std::islower(c); });
        else if (name == "print") pred([](unsigned char c) { return std::isprint(c); });
        else if (name == "punct") pred([](unsigned char c) { return std::ispunct(c); });
        else if (name == "space") pred([](unsigned char c) { return std::isspace(c); });
        else if (name == "upper") pred([](unsigned char c) { return std::isupper(c); });
        else if (name == "xdigit") pred([](unsigned char c) { return std::isxdigit(c); });
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

struct Matcher {
    const Prog& prog;
    const std::string& s;
    bool notbol = false, noteol = false;
    int steps = 0;
    bool gaveUp = false;
    using Cont = std::function<void(size_t)>;

    bool bit(const Node& n, unsigned char c) const { return (n.bits[c >> 5] >> (c & 31)) & 1; }
    bool same(unsigned char a, unsigned char b) const {
        if (prog.icase) return fold(a) == fold(b);
        return a == b;
    }
    void emit(int ni, size_t pos, Caps& caps, const Cont& cont) {
        if (gaveUp || ni < 0) return;
        if (++steps > kStepLimit) { gaveUp = true; return; }
        const Node& n = prog.nodes[ni];
        switch (n.k) {
        case K::Empty: cont(pos); break;
        case K::Lit:
            if (pos < s.size() && same(uint8_t(s[pos]), n.ch)) cont(pos + 1);
            break;
        case K::Any:
            if (pos < s.size() && !(prog.newline && s[pos] == '\n')) cont(pos + 1);
            break;
        case K::Class: {
            if (pos >= s.size()) break;
            const unsigned char c = uint8_t(s[pos]);
            bool in = bit(n, c);
            if (n.neg) in = !in;
            if (prog.newline && c == '\n' && n.neg) in = false;
            if (in) cont(pos + 1);
            break;
        }
        case K::Bol: {
            const bool ok = (pos == 0 && !notbol) || (prog.newline && pos > 0 && s[pos - 1] == '\n');
            if (ok) cont(pos);
            break;
        }
        case K::Eol: {
            const bool ok = (pos == s.size() && !noteol) || (prog.newline && pos < s.size() && s[pos] == '\n');
            if (ok) cont(pos);
            break;
        }
        case K::Concat:
            emit(n.a, pos, caps, [&](size_t mid) { emit(n.b, mid, caps, cont); });
            break;
        case K::Alt:
            emit(n.a, pos, caps, cont);
            emit(n.b, pos, caps, cont);
            break;
        case K::Cap: {
            const int g = n.group;
            const int64_t oldSo = caps.so[g], oldEo = caps.eo[g];
            caps.so[g] = int64_t(pos);
            emit(n.a, pos, caps, [&](size_t np) {
                caps.eo[g] = int64_t(np);
                cont(np);
            });
            caps.so[g] = oldSo;
            caps.eo[g] = oldEo;
            break;
        }
        case K::Ref: {
            const int g = n.group;
            if (g < 0 || g >= int(caps.so.size()) || caps.so[g] < 0 || caps.eo[g] < caps.so[g]) break;
            const size_t len = size_t(caps.eo[g] - caps.so[g]);
            if (pos + len > s.size()) break;
            bool eq = true;
            for (size_t k = 0; k < len && eq; ++k) eq = same(uint8_t(s[pos + k]), uint8_t(s[size_t(caps.so[g]) + k]));
            if (eq) cont(pos + len);
            break;
        }
        case K::Rep: {
            const int hi = n.max < 0 ? int(std::min<size_t>(s.size() + 1, 4096)) : n.max;
            std::function<void(int, size_t)> rec = [&](int count, size_t p) {
                if (gaveUp) return;
                if (count < hi) {
                    emit(n.a, p, caps, [&](size_t np) {
                        if (np == p) return; // an empty iteration would not advance; zero is handled below
                        rec(count + 1, np);
                    });
                }
                if (!gaveUp && count >= n.min) cont(p);
            };
            rec(0, pos);
            break;
        }
        }
    }
};

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
    const std::string s = x.proc.readCString(text);
    const int n = prog->nsub + 1;
    Matcher m{*prog, s, (eflags & REG_NOTBOL) != 0, (eflags & REG_NOTEOL) != 0};
    bool any = false;
    Caps snap;
    for (size_t start = 0; start <= s.size() && !m.gaveUp; ++start) {
        Caps cur;
        cur.so.assign(n, -1);
        cur.eo.assign(n, -1);
        bool local = false;
        int64_t best = -1;
        Caps localSnap;
        m.emit(prog->root, start, cur, [&](size_t end) {
            cur.so[0] = int64_t(start);
            cur.eo[0] = int64_t(end);
            if (!local || int64_t(end) > best) {
                local = true;
                best = int64_t(end);
                localSnap = cur;
            }
        });
        if (m.gaveUp) return REG_ESPACE;
        if (local) { any = true; snap = std::move(localSnap); break; }
    }
    if (!any) return REG_NOMATCH;
    if (!prog->nosub && pmatch && nmatch) {
        const uint32_t count = std::min<uint32_t>(nmatch, uint32_t(n));
        for (uint32_t i = 0; i < count; ++i) {
            const int64_t so = i < snap.so.size() ? snap.so[i] : -1;
            const int64_t eo = i < snap.eo.size() ? snap.eo[i] : -1;
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
