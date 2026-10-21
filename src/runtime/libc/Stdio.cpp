// HydrogenVM — <stdio.h>. Guest FILE* values are addresses of small guest
// blocks (sizeof(FILE) == 88 on iOS) mapped to host FILE objects. stdout and
// stderr go to the process log, line by line.
#include "Format.hpp"
#include "runtime/Process.hpp"
#include "Network.hpp"
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <unistd.h>

namespace Hydrogen::Runtime {
namespace {

constexpr uint32_t kFileSize = 88;

struct Stdio {
    explicit Stdio(Process& p) : proc(p) {}
    Process& proc;
    std::mutex mu;
    struct File { std::FILE* f = nullptr; int stream = 0; /*1 stdout 2 stderr*/ std::string pending; int unget = -1; bool eof = false; };
    std::unordered_map<uint32_t, File> files;
    uint32_t sF = 0;   // __sF[3]

    File* get(uint32_t fp) {
        auto it = files.find(fp);
        return it == files.end() ? nullptr : &it->second;
    }
    void emit(File& f, const std::string& s) {
        f.pending += s;
        size_t nl;
        while ((nl = f.pending.find('\n')) != std::string::npos) {
            proc.log(f.stream == 2 ? LogLevel::GuestStderr : LogLevel::GuestStdout, f.pending.substr(0, nl + 1));
            f.pending.erase(0, nl + 1);
        }
        if (f.pending.size() > 4096) flush(f);
    }
    void flush(File& f) {
        if (f.stream && !f.pending.empty()) {
            proc.log(f.stream == 2 ? LogLevel::GuestStderr : LogLevel::GuestStdout, f.pending);
            f.pending.clear();
        }
        if (f.f) std::fflush(f.f);
    }
    // Returns bytes written.
    size_t write(uint32_t fp, const void* data, size_t n) {
        std::lock_guard<std::mutex> lk(mu);
        File* f = get(fp);
        if (!f) return 0;
        if (f->stream) { emit(*f, std::string(static_cast<const char*>(data), n)); return n; }
        if (!f->f) return 0;
        return std::fwrite(data, 1, n, f->f);
    }
};

Stdio& S(Ctx& x) { return x.proc.service<Stdio>(); }
inline void setErrno(Ctx& x, int e) { x.mem.write<uint32_t>(x.proc.currentThread().errnoAddr, uint32_t(e)); }

uint32_t newFile(Ctx& x, std::FILE* f) {
    const uint32_t fp = x.proc.heap().allocZeroed(kFileSize);
    std::lock_guard<std::mutex> lk(S(x).mu);
    S(x).files[fp].f = f;
    return fp;
}

int32_t outString(Ctx& x, uint32_t fp, const std::string& s) {
    const size_t n = S(x).write(fp, s.data(), s.size());
    return n == s.size() ? int32_t(n) : -1;
}

// ---------------------------------------------------------------- printf ---

int32_t f_printf(Ctx& x, GuestPtr fmt, VarArgs va) {
    return outString(x, S(x).sF + kFileSize, formatGuest(x.proc, x.proc.readCString(fmt), va.args));
}
int32_t f_vprintf(Ctx& x, GuestPtr fmt, VaList ap) {
    ArgReader a(x.mem, ap.ptr);
    return outString(x, S(x).sF + kFileSize, formatGuest(x.proc, x.proc.readCString(fmt), a));
}
int32_t f_fprintf(Ctx& x, GuestPtr fp, GuestPtr fmt, VarArgs va) { return outString(x, fp, formatGuest(x.proc, x.proc.readCString(fmt), va.args)); }
int32_t f_vfprintf(Ctx& x, GuestPtr fp, GuestPtr fmt, VaList ap) {
    ArgReader a(x.mem, ap.ptr);
    return outString(x, fp, formatGuest(x.proc, x.proc.readCString(fmt), a));
}
int32_t storeFormatted(Ctx& x, GuestPtr buf, uint32_t size, const std::string& s) {
    if (size && buf) {
        const size_t n = std::min<size_t>(s.size(), size - 1);
        x.mem.copyIn(buf, s.data(), n);
        x.mem.write<uint8_t>(buf + uint32_t(n), 0);
    }
    return int32_t(s.size());
}
int32_t f_sprintf(Ctx& x, GuestPtr buf, GuestPtr fmt, VarArgs va) { return storeFormatted(x, buf, 0x7FFFFFFF, formatGuest(x.proc, x.proc.readCString(fmt), va.args)); }
int32_t f_sprintf_chk(Ctx& x, GuestPtr buf, int32_t, uint32_t, GuestPtr fmt, VarArgs va) { return storeFormatted(x, buf, 0x7FFFFFFF, formatGuest(x.proc, x.proc.readCString(fmt), va.args)); }
int32_t f_snprintf(Ctx& x, GuestPtr buf, uint32_t n, GuestPtr fmt, VarArgs va) { return storeFormatted(x, buf, n, formatGuest(x.proc, x.proc.readCString(fmt), va.args)); }
int32_t f_snprintf_chk(Ctx& x, GuestPtr buf, uint32_t n, int32_t, uint32_t, GuestPtr fmt, VarArgs va) { return storeFormatted(x, buf, n, formatGuest(x.proc, x.proc.readCString(fmt), va.args)); }
int32_t f_vsprintf(Ctx& x, GuestPtr buf, GuestPtr fmt, VaList ap) { ArgReader a(x.mem, ap.ptr); return storeFormatted(x, buf, 0x7FFFFFFF, formatGuest(x.proc, x.proc.readCString(fmt), a)); }
int32_t f_vsnprintf(Ctx& x, GuestPtr buf, uint32_t n, GuestPtr fmt, VaList ap) { ArgReader a(x.mem, ap.ptr); return storeFormatted(x, buf, n, formatGuest(x.proc, x.proc.readCString(fmt), a)); }
int32_t f_vsnprintf_chk(Ctx& x, GuestPtr buf, uint32_t n, int32_t, uint32_t, GuestPtr fmt, VaList ap) { ArgReader a(x.mem, ap.ptr); return storeFormatted(x, buf, n, formatGuest(x.proc, x.proc.readCString(fmt), a)); }
int32_t f_asprintf(Ctx& x, GuestPtr out, GuestPtr fmt, VarArgs va) {
    const std::string s = formatGuest(x.proc, x.proc.readCString(fmt), va.args);
    x.mem.write<uint32_t>(out, x.proc.allocCString(s));
    return int32_t(s.size());
}
int32_t f_vasprintf(Ctx& x, GuestPtr out, GuestPtr fmt, VaList ap) {
    ArgReader a(x.mem, ap.ptr);
    const std::string s = formatGuest(x.proc, x.proc.readCString(fmt), a);
    x.mem.write<uint32_t>(out, x.proc.allocCString(s));
    return int32_t(s.size());
}

int32_t f_puts(Ctx& x, GuestPtr s) { return outString(x, S(x).sF + kFileSize, x.proc.readCString(s) + "\n") < 0 ? -1 : 1; }
int32_t f_putchar(Ctx& x, int32_t c) { const char ch = char(c); return S(x).write(S(x).sF + kFileSize, &ch, 1) ? (c & 0xFF) : -1; }
int32_t f_fputs(Ctx& x, GuestPtr s, GuestPtr fp) { return outString(x, fp, x.proc.readCString(s)) < 0 ? -1 : 1; }
int32_t f_fputc(Ctx& x, int32_t c, GuestPtr fp) { const char ch = char(c); return S(x).write(fp, &ch, 1) ? (c & 0xFF) : -1; }
int32_t f_swbuf(Ctx& x, int32_t c, GuestPtr fp) { return f_fputc(x, c, fp); }
uint32_t f_fwrite(Ctx& x, GuestPtr ptr, uint32_t size, uint32_t n, GuestPtr fp) {
    const uint64_t total = uint64_t(size) * n;
    if (!total) return 0;
    return uint32_t(S(x).write(fp, x.mem.host(ptr), size_t(total)) / size);
}
void f_perror(Ctx& x, GuestPtr s) {
    const int e = int(x.mem.read<uint32_t>(x.proc.currentThread().errnoAddr));
    std::string msg = s ? x.proc.readCString(s) + ": " : "";
    outString(x, S(x).sF + 2 * kFileSize, msg + std::strerror(e) + "\n");
}

// ----------------------------------------------------------------- files ---

uint32_t f_fopen(Ctx& x, GuestPtr path, GuestPtr mode) {
    const std::string p = x.proc.readCString(path), m = x.proc.readCString(mode);
    std::FILE* f = std::fopen(p.c_str(), m.c_str());
    if (!f) { setErrno(x, errno); return 0; }
    return newFile(x, f);
}
uint32_t f_fdopen(Ctx& x, int32_t fd, GuestPtr mode) {
    // Host stdio would bypass interruptible socket IO and transfer close
    // ownership out of the socket registry. Reject it until integrated.
    if (isNetworkSocket(x.proc, fd)) { setErrno(x, 45); return 0; }
    std::FILE* f = ::fdopen(fd, x.proc.readCString(mode).c_str());
    if (!f) { setErrno(x, errno); return 0; }
    return newFile(x, f);
}
uint32_t f_freopen(Ctx& x, GuestPtr path, GuestPtr mode, GuestPtr fp) {
    std::lock_guard<std::mutex> lk(S(x).mu);
    auto* f = S(x).get(fp);
    if (!f) return 0;
    if (f->stream) return fp;   // redirecting stdout/stderr: keep logging
    std::FILE* nf = std::freopen(x.proc.readCString(path).c_str(), x.proc.readCString(mode).c_str(), f->f);
    if (!nf) { setErrno(x, errno); return 0; }
    f->f = nf;
    return fp;
}
int32_t f_fclose(Ctx& x, GuestPtr fp) {
    std::unique_lock<std::mutex> lk(S(x).mu);
    auto* f = S(x).get(fp);
    if (!f) return -1;
    if (f->stream) { S(x).flush(*f); return 0; }
    const int r = f->f ? std::fclose(f->f) : 0;
    S(x).files.erase(fp);
    lk.unlock();
    x.proc.heap().free(fp);
    return r;
}
uint32_t f_fread(Ctx& x, GuestPtr ptr, uint32_t size, uint32_t n, GuestPtr fp) {
    std::lock_guard<std::mutex> lk(S(x).mu);
    auto* f = S(x).get(fp);
    if (!f || !f->f || !size || !n) return 0;
    char* dst = reinterpret_cast<char*>(x.mem.host(ptr));
    size_t total = size_t(size) * n, got = 0;
    if (f->unget >= 0) { dst[got++] = char(f->unget); f->unget = -1; }
    got += std::fread(dst + got, 1, total - got, f->f);
    if (got < total) f->eof = std::feof(f->f);
    return uint32_t(got / size);
}
int32_t f_fgetc(Ctx& x, GuestPtr fp) {
    std::lock_guard<std::mutex> lk(S(x).mu);
    auto* f = S(x).get(fp);
    if (!f || !f->f) return -1;
    if (f->unget >= 0) { const int c = f->unget; f->unget = -1; return c; }
    const int c = std::fgetc(f->f);
    if (c == EOF) f->eof = true;
    return c;
}
int32_t f_srget(Ctx& x, GuestPtr fp) { return f_fgetc(x, fp); }
int32_t f_getchar(Ctx&) { return -1; }
int32_t f_ungetc(Ctx& x, int32_t c, GuestPtr fp) {
    std::lock_guard<std::mutex> lk(S(x).mu);
    auto* f = S(x).get(fp);
    if (!f || c == EOF) return -1;
    f->unget = c & 0xFF;
    f->eof = false;
    return c & 0xFF;
}
uint32_t f_fgets(Ctx& x, GuestPtr buf, int32_t n, GuestPtr fp) {
    if (n <= 0) return 0;
    int32_t i = 0;
    while (i < n - 1) {
        const int c = f_fgetc(x, fp);
        if (c == EOF) break;
        x.mem.write<uint8_t>(buf + uint32_t(i++), uint8_t(c));
        if (c == '\n') break;
    }
    if (i == 0) return 0;
    x.mem.write<uint8_t>(buf + uint32_t(i), 0);
    return buf;
}
int32_t f_fseek(Ctx& x, GuestPtr fp, int32_t off, int32_t whence) {
    std::lock_guard<std::mutex> lk(S(x).mu);
    auto* f = S(x).get(fp);
    if (!f || !f->f) return -1;
    f->unget = -1; f->eof = false;
    return std::fseek(f->f, off, whence);
}
int32_t f_fseeko(Ctx& x, GuestPtr fp, int64_t off, int32_t whence) {
    std::lock_guard<std::mutex> lk(S(x).mu);
    auto* f = S(x).get(fp);
    if (!f || !f->f) return -1;
    f->unget = -1; f->eof = false;
    return fseeko(f->f, off_t(off), whence);
}
int32_t f_ftell(Ctx& x, GuestPtr fp) {
    std::lock_guard<std::mutex> lk(S(x).mu);
    auto* f = S(x).get(fp);
    if (!f || !f->f) return -1;
    return int32_t(std::ftell(f->f) - (f->unget >= 0 ? 1 : 0));
}
int64_t f_ftello(Ctx& x, GuestPtr fp) {
    std::lock_guard<std::mutex> lk(S(x).mu);
    auto* f = S(x).get(fp);
    if (!f || !f->f) return -1;
    return int64_t(ftello(f->f)) - (f->unget >= 0 ? 1 : 0);
}
void f_rewind(Ctx& x, GuestPtr fp) { f_fseek(x, fp, 0, SEEK_SET); }
int32_t f_feof(Ctx& x, GuestPtr fp) {
    std::lock_guard<std::mutex> lk(S(x).mu);
    auto* f = S(x).get(fp);
    return f && f->unget < 0 && (f->eof || (f->f && std::feof(f->f))) ? 1 : 0;
}
int32_t f_ferror(Ctx& x, GuestPtr fp) {
    std::lock_guard<std::mutex> lk(S(x).mu);
    auto* f = S(x).get(fp);
    return f && f->f ? std::ferror(f->f) : 0;
}
void f_clearerr(Ctx& x, GuestPtr fp) {
    std::lock_guard<std::mutex> lk(S(x).mu);
    if (auto* f = S(x).get(fp)) { f->eof = false; if (f->f) std::clearerr(f->f); }
}
int32_t f_fflush(Ctx& x, GuestPtr fp) {
    std::lock_guard<std::mutex> lk(S(x).mu);
    if (!fp) { for (auto& [k, f] : S(x).files) S(x).flush(f); return 0; }
    if (auto* f = S(x).get(fp)) S(x).flush(*f);
    return 0;
}
int32_t f_fileno(Ctx& x, GuestPtr fp) {
    std::lock_guard<std::mutex> lk(S(x).mu);
    auto* f = S(x).get(fp);
    if (!f) return -1;
    if (f->stream) return f->stream;
    return f->f ? ::fileno(f->f) : -1;
}
int32_t f_setvbuf(Ctx&, GuestPtr, GuestPtr, int32_t, uint32_t) { return 0; }
void f_setbuf(Ctx&, GuestPtr, GuestPtr) {}
int32_t f_remove(Ctx& x, GuestPtr path) { const int r = std::remove(x.proc.readCString(path).c_str()); if (r) setErrno(x, errno); return r; }
int32_t f_rename(Ctx& x, GuestPtr a, GuestPtr b) { const int r = std::rename(x.proc.readCString(a).c_str(), x.proc.readCString(b).c_str()); if (r) setErrno(x, errno); return r; }
uint32_t f_tmpfile(Ctx& x) { std::FILE* f = std::tmpfile(); return f ? newFile(x, f) : 0; }

// ----------------------------------------------------------------- scanf ---

struct Source {
    virtual int get() = 0;
    virtual void unget(int c) = 0;
    virtual ~Source() = default;
    int count = 0;
};
struct StringSource : Source {
    std::string s; size_t p = 0;
    explicit StringSource(std::string v) : s(std::move(v)) {}
    int get() override { if (p >= s.size()) return EOF; ++count; return uint8_t(s[p++]); }
    void unget(int c) override { if (c != EOF && p) { --p; --count; } }
};
struct FileSource : Source {
    Ctx& x; uint32_t fp;
    FileSource(Ctx& c, uint32_t f) : x(c), fp(f) {}
    int get() override { const int c = f_fgetc(x, fp); if (c != EOF) ++count; return c; }
    void unget(int c) override { if (c != EOF) { f_ungetc(x, c, fp); --count; } }
};

int32_t scanGuest(Ctx& x, Source& in, const std::string& fmt, ArgReader& args) {
    int assigned = 0;
    bool anyInput = false;
    size_t i = 0;
    auto skipWs = [&] { int c; while ((c = in.get()) != EOF && std::isspace(c)) {} in.unget(c); return c; };
    while (i < fmt.size()) {
        const char f = fmt[i];
        if (std::isspace(uint8_t(f))) { skipWs(); ++i; continue; }
        if (f != '%') {
            const int c = in.get();
            if (c != uint8_t(f)) { in.unget(c); break; }
            ++i;
            continue;
        }
        ++i;
        if (i < fmt.size() && fmt[i] == '%') { skipWs(); if (in.get() != '%') break; ++i; continue; }
        bool suppress = false;
        if (i < fmt.size() && fmt[i] == '*') { suppress = true; ++i; }
        int width = 0;
        while (i < fmt.size() && std::isdigit(uint8_t(fmt[i]))) width = width * 10 + (fmt[i++] - '0');
        std::string len;
        while (i < fmt.size() && std::strchr("hlqjztL", fmt[i])) len += fmt[i++];
        if (i >= fmt.size()) break;
        const char conv = fmt[i++];
        if (conv == 'n') { if (!suppress) x.mem.write<uint32_t>(args.word(), uint32_t(in.count)); continue; }
        if (conv != 'c' && conv != '[') { if (skipWs() == EOF) return anyInput ? assigned : EOF; }
        const int maxw = width ? width : 0x7FFFFFFF;
        std::string tok;
        if (conv == 'c') {
            const int n = width ? width : 1;
            for (int k = 0; k < n; ++k) { const int c = in.get(); if (c == EOF) break; tok += char(c); }
            if (tok.empty()) return anyInput ? assigned : EOF;
            anyInput = true;
            if (!suppress) { x.mem.copyIn(args.word(), tok.data(), tok.size()); ++assigned; }
            continue;
        }
        if (conv == 's' || conv == '[') {
            std::string set;
            bool negate = false;
            if (conv == '[') {
                if (i < fmt.size() && fmt[i] == '^') { negate = true; ++i; }
                if (i < fmt.size() && fmt[i] == ']') set += fmt[i++];
                while (i < fmt.size() && fmt[i] != ']') {
                    if (fmt[i] == '-' && !set.empty() && i + 1 < fmt.size() && fmt[i + 1] != ']') {
                        for (char ch = char(set.back() + 1); ch <= fmt[i + 1]; ++ch) set += ch;
                        i += 2;
                    } else set += fmt[i++];
                }
                if (i < fmt.size()) ++i;
            }
            int c;
            while (int(tok.size()) < maxw && (c = in.get()) != EOF) {
                const bool ok = conv == 's' ? !std::isspace(c) : ((set.find(char(c)) != std::string::npos) != negate);
                if (!ok) { in.unget(c); break; }
                tok += char(c);
            }
            if (tok.empty()) break;
            anyInput = true;
            if (!suppress) { const uint32_t d = args.word(); x.mem.copyIn(d, tok.c_str(), tok.size() + 1); ++assigned; }
            continue;
        }
        // numbers
        const bool isFloat = std::strchr("eEfFgGaA", conv) != nullptr;
        int c;
        while (int(tok.size()) < maxw && (c = in.get()) != EOF) {
            const bool ok = isFloat ? (std::isdigit(c) || std::strchr("+-.eEinfatyINFATYxXpP", c))
                                    : (std::isxdigit(c) || c == '+' || c == '-' || c == 'x' || c == 'X');
            if (!ok) { in.unget(c); break; }
            tok += char(c);
        }
        if (tok.empty()) break;
        char* end = nullptr;
        if (isFloat) {
            const double d = std::strtod(tok.c_str(), &end);
            for (size_t k = tok.size(); k > size_t(end - tok.c_str()); --k) in.unget(uint8_t(tok[k - 1]));
            if (end == tok.c_str()) break;
            anyInput = true;
            if (!suppress) {
                const uint32_t dst = args.word();
                if (len == "l" || len == "L") { uint64_t b; std::memcpy(&b, &d, 8); x.mem.write<uint64_t>(dst, b); }
                else { const float fl = float(d); uint32_t b; std::memcpy(&b, &fl, 4); x.mem.write<uint32_t>(dst, b); }
                ++assigned;
            }
            continue;
        }
        const int base = conv == 'd' || conv == 'u' ? 10 : conv == 'o' ? 8 : (conv == 'x' || conv == 'X' || conv == 'p') ? 16 : 0;
        const long long v = std::strtoll(tok.c_str(), &end, base);
        for (size_t k = tok.size(); k > size_t(end - tok.c_str()); --k) in.unget(uint8_t(tok[k - 1]));
        if (end == tok.c_str()) break;
        anyInput = true;
        if (!suppress) {
            const uint32_t dst = args.word();
            if (len == "hh") x.mem.write<uint8_t>(dst, uint8_t(v));
            else if (len == "h") x.mem.write<uint16_t>(dst, uint16_t(v));
            else if (len == "ll" || len == "q" || len == "j") x.mem.write<uint64_t>(dst, uint64_t(v));
            else x.mem.write<uint32_t>(dst, uint32_t(v));
            ++assigned;
        }
    }
    return assigned;
}

int32_t f_sscanf(Ctx& x, GuestPtr s, GuestPtr fmt, VarArgs va) { StringSource src(x.proc.readCString(s)); return scanGuest(x, src, x.proc.readCString(fmt), va.args); }
int32_t f_vsscanf(Ctx& x, GuestPtr s, GuestPtr fmt, VaList ap) { StringSource src(x.proc.readCString(s)); ArgReader a(x.mem, ap.ptr); return scanGuest(x, src, x.proc.readCString(fmt), a); }
int32_t f_fscanf(Ctx& x, GuestPtr fp, GuestPtr fmt, VarArgs va) { FileSource src(x, fp); return scanGuest(x, src, x.proc.readCString(fmt), va.args); }
int32_t f_scanf(Ctx&, GuestPtr, VarArgs) { return -1; }

std::string readWString(Ctx& x, GuestPtr ws, size_t maxChars = 65536) {
    std::string out;
    if (!ws) return out;
    for (size_t i = 0; i < maxChars; ++i) {
        const uint32_t wc = x.mem.read<uint32_t>(ws + uint32_t(i) * 4);
        if (!wc) break;
        out.push_back(wc < 0x80 ? char(wc) : '?');
    }
    return out;
}
int32_t storeWideFormatted(Ctx& x, GuestPtr buf, uint32_t size, const std::string& s) {
    if (!size) return -1;
    if (buf) {
        const size_t n = std::min<size_t>(s.size(), size - 1);
        for (size_t i = 0; i < n; ++i) x.mem.write<uint32_t>(buf + uint32_t(i) * 4, uint8_t(s[i]));
        x.mem.write<uint32_t>(buf + uint32_t(n) * 4, 0);
    }
    return s.size() < size ? int32_t(s.size()) : -1;
}
int32_t f_wprintf(Ctx& x, GuestPtr fmt, VarArgs va) { return outString(x, S(x).sF + kFileSize, formatGuest(x.proc, readWString(x, fmt), va.args)); }
int32_t f_fwprintf(Ctx& x, GuestPtr fp, GuestPtr fmt, VarArgs va) { return outString(x, fp, formatGuest(x.proc, readWString(x, fmt), va.args)); }
int32_t f_swprintf(Ctx& x, GuestPtr buf, uint32_t n, GuestPtr fmt, VarArgs va) { return storeWideFormatted(x, buf, n, formatGuest(x.proc, readWString(x, fmt), va.args)); }
int32_t f_vswprintf(Ctx& x, GuestPtr buf, uint32_t n, GuestPtr fmt, VaList ap) { ArgReader a(x.mem, ap.ptr); return storeWideFormatted(x, buf, n, formatGuest(x.proc, readWString(x, fmt), a)); }
int32_t f_swscanf(Ctx& x, GuestPtr s, GuestPtr fmt, VarArgs va) { StringSource src(readWString(x, s)); return scanGuest(x, src, readWString(x, fmt), va.args); }
int32_t f_vswscanf(Ctx& x, GuestPtr s, GuestPtr fmt, VaList ap) { StringSource src(readWString(x, s)); ArgReader a(x.mem, ap.ptr); return scanGuest(x, src, readWString(x, fmt), a); }

// char *tmpnam(char *s) — a unique path inside the guest sandbox tmp directory.
// Darwin L_tmpnam is 1024. A null s returns one guest static buffer, overwritten
// on the next null call, matching the C library.
struct TmpnamState {
    explicit TmpnamState(Process&) {}
    std::mutex mu;
    uint32_t seq = 0;
    uint32_t staticBuf = 0;
};
uint32_t f_tmpnam(Ctx& x, GuestPtr s) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(x.proc.homePath()) / "tmp";
    std::error_code ec;
    fs::create_directories(dir, ec);
    auto& st = x.proc.service<TmpnamState>();
    std::lock_guard<std::mutex> lk(st.mu);
    std::string chosen;
    for (int n = 0; n < 10000 && chosen.empty(); ++n) {
        const uint32_t seq = ++st.seq;
        char leaf[64];
        std::snprintf(leaf, sizeof leaf, "tmpnam.%u.%u", uint32_t(::getpid()), seq);
        const fs::path path = dir / leaf;
        if (!fs::exists(path, ec)) chosen = path.string();
    }
    if (chosen.empty()) {
        if (x.proc.currentThread().errnoAddr) x.mem.write<uint32_t>(x.proc.currentThread().errnoAddr, 17); // Darwin EEXIST
        return 0;
    }
    if (chosen.size() >= 1024) chosen.resize(1023);
    GuestPtr dest = s;
    if (!dest) {
        if (!st.staticBuf) st.staticBuf = x.proc.allocStatic(1024);
        dest = st.staticBuf;
    }
    x.mem.copyIn(dest, chosen.c_str(), chosen.size() + 1);
    return dest;
}

} // namespace

void registerLibcRegex(Process& p);

void flushGuestStdio(Process& p) {
    auto& s = p.service<Stdio>();
    std::lock_guard<std::mutex> lk(s.mu);
    for (auto& [k, f] : s.files) s.flush(f);
}

void registerLibcStdio(Process& p) {
    auto& st = p.service<Stdio>();
    st.sF = p.defineDataBlock("___sF", kFileSize * 3);
    for (int i = 0; i < 3; ++i) st.files[st.sF + uint32_t(i) * kFileSize].stream = i;   // 0 = stdin (no stream)
    st.files[st.sF].stream = 0;
    const char* ptrNames[] = {"___stdinp", "___stdoutp", "___stderrp"};
    for (uint32_t i = 0; i < 3; ++i) {
        const uint32_t cell = p.defineDataBlock(ptrNames[i], 4);
        p.mem().write<uint32_t>(cell, st.sF + i * kFileSize);
    }
    p.def<&f_printf>("_printf");
    p.def<&f_vprintf>("_vprintf");
    p.def<&f_fprintf>("_fprintf");
    p.def<&f_vfprintf>("_vfprintf");
    p.def<&f_sprintf>("_sprintf");
    p.def<&f_sprintf_chk>("___sprintf_chk");
    p.def<&f_snprintf>("_snprintf");
    p.def<&f_snprintf_chk>("___snprintf_chk");
    p.def<&f_vsprintf>("_vsprintf");
    p.def<&f_vsnprintf>("_vsnprintf");
    p.def<&f_vsnprintf_chk>("___vsnprintf_chk");
    p.def<&f_asprintf>("_asprintf");
    p.def<&f_vasprintf>("_vasprintf");
    p.def<&f_puts>("_puts");
    p.def<&f_putchar>("_putchar");
    p.def<&f_fputs>("_fputs");
    p.def<&f_fputc>("_fputc");
    p.def<&f_fputc>("_putc");
    p.def<&f_swbuf>("___swbuf");
    p.def<&f_fwrite>("_fwrite");
    p.def<&f_perror>("_perror");
    p.def<&f_fopen>("_fopen");
    p.def<&f_fdopen>("_fdopen");
    p.def<&f_freopen>("_freopen");
    p.def<&f_fclose>("_fclose");
    p.def<&f_fread>("_fread");
    p.def<&f_fgetc>("_fgetc");
    p.def<&f_fgetc>("_getc");
    p.def<&f_srget>("___srget");
    p.def<&f_getchar>("_getchar");
    p.def<&f_ungetc>("_ungetc");
    p.def<&f_fgets>("_fgets");
    p.def<&f_fseek>("_fseek");
    p.def<&f_fseeko>("_fseeko");
    p.def<&f_ftell>("_ftell");
    p.def<&f_ftello>("_ftello");
    p.def<&f_rewind>("_rewind");
    p.def<&f_feof>("_feof");
    p.def<&f_ferror>("_ferror");
    p.def<&f_clearerr>("_clearerr");
    p.def<&f_fflush>("_fflush");
    p.def<&f_fileno>("_fileno");
    p.def<&f_setvbuf>("_setvbuf");
    p.def<&f_setbuf>("_setbuf");
    p.def<&f_remove>("_remove");
    p.def<&f_rename>("_rename");
    p.def<&f_tmpfile>("_tmpfile");
    p.def<&f_sscanf>("_sscanf");
    p.def<&f_vsscanf>("_vsscanf");
    p.def<&f_fscanf>("_fscanf");
    p.def<&f_scanf>("_scanf");
    p.def<&f_wprintf>("_wprintf");
    p.def<&f_fwprintf>("_fwprintf");
    p.def<&f_swprintf>("_swprintf");
    p.def<&f_vswprintf>("_vswprintf");
    p.def<&f_swscanf>("_swscanf");
    p.def<&f_vswscanf>("_vswscanf");
    p.def<&f_tmpnam>("_tmpnam");
    registerLibcRegex(p);
}

} // namespace Hydrogen::Runtime
