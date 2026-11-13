// HydrogenVM — TLS bridge for games the player allowed online.
//
// Old games carry their own TLS library (Minion Rush: OpenSSL from 2013, TLS 1.0
// only). Servers refuse TLS 1.0 today, so the game's secure connections fail and it
// retries forever. With a bridge, a guest TCP connection to port 443 goes to a local
// port instead: the bridge accepts the game's old TLS there (with its own throwaway
// certificate; these games do not check certificates) and opens a modern TLS
// connection to the real server with the host's TLS library, checking the server's
// certificate. Bytes in between pass unchanged. No cipher code lives here: the host
// library (OpenSSL on Linux, Security.framework on Apple) does all of it.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <algorithm>
#include <zlib.h>
#include <cctype>
#include <utility>

namespace Hydrogen::Runtime {

struct TlsBridge {
    virtual ~TlsBridge() = default;
    // Starts a bridge for one connection to `host` (the name the game looked up, or the
    // dotted address) at `ipNetworkOrder`:`port`. Returns the local 127.0.0.1 port the
    // guest should connect to instead, or 0 when no bridge could be set up.
    virtual uint16_t open(const std::string& host, uint32_t ipNetworkOrder, uint16_t port,
                          std::function<void(const std::string&)> log) = 0;
};

// For the play log: the first line of an HTTP request or response that starts this chunk
// ("GET /path", "HTTP/1.1 404 Not Found"), query strings dropped; empty otherwise.
inline std::string httpFirstLine(const char* p, size_t n) {
    static const char* const kStarts[] = {"GET ", "POST ", "PUT ", "HEAD ", "DELETE ", "HTTP/1."};
    bool http = false;
    for (const char* s : kStarts) {
        const size_t l = std::char_traits<char>::length(s);
        if (n >= l && std::string(p, l) == s) { http = true; break; }
    }
    if (!http) return {};
    size_t end = 0;
    while (end < n && end < 160 && p[end] != '\r' && p[end] != '\n') ++end;
    std::string line(p, end);
    if (const size_t q = line.find('?'); q != std::string::npos) {
        const size_t sp = line.find(' ', q);
        line = line.substr(0, q) + (sp == std::string::npos ? "" : line.substr(sp));
    }
    return line;
}

// The first line plus the headers that say what is being fetched and how big it is.
inline std::string httpSummary(const char* p, size_t n) {
    std::string out = httpFirstLine(p, n);
    if (out.empty()) return out;
    static const char* const kKeep[] = {"content-length:", "content-range:", "range:", "location:", "content-type:",
                                        "transfer-encoding:", "content-encoding:", "connection:", "etag:", "last-modified:"};
    std::string extra;
    size_t i = 0;
    while (i < n) {
        size_t e = i;
        while (e < n && p[e] != '\n') ++e;
        std::string line(p + i, e - i);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (i && line.empty()) break;   // end of the headers
        std::string lower = line;
        for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
        for (const char* k : kKeep)
            if (lower.rfind(k, 0) == 0 && line.size() < 200) {
                if (lower.rfind("location:", 0) == 0) line = line.substr(0, line.find('?'));   // signed URLs stay out of shared logs
                extra += (extra.empty() ? "" : ", ") + line;
                break;
            }
        i = e + 1;
    }
    return extra.empty() ? out : out + " [" + extra + "]";
}

// Today's servers and CDNs often send header names in lower case ("etag:", "content-encoding:").
// HTTP says case does not matter, but parsers in old games look for "ETag:" exactly (Minion
// Rush's asset check asks its CDN for an ETag over and over). In a response that starts in this
// chunk, header names get their usual spelling; values, sizes and byte counts are unchanged.
// Returns how many names were respelled.
inline int canonicalHeaderNames(char* p, size_t n) {
    if (n < 8 || std::string(p, 7) != "HTTP/1.") return 0;
    int changed = 0;
    size_t i = 0;
    while (i < n && p[i] != '\n') ++i;   // past the status line
    for (++i; i < n;) {
        if (p[i] == '\r' || p[i] == '\n') break;   // end of the headers
        size_t colon = i;
        while (colon < n && p[colon] != ':' && p[colon] != '\n') ++colon;
        if (colon >= n || p[colon] != ':') break;
        std::string name(p + i, colon - i), lower = name;
        for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
        std::string want;
        if (lower == "etag") want = "ETag";
        else if (lower == "www-authenticate") want = "WWW-Authenticate";
        else if (lower == "content-md5") want = "Content-MD5";
        else {
            want = lower;
            bool up = true;
            for (char& c : want) { if (up) c = char(std::toupper(static_cast<unsigned char>(c))); up = c == '-'; }
        }
        if (want != name) { std::copy(want.begin(), want.end(), p + i); ++changed; }
        while (i < n && p[i] != '\n') ++i;
        ++i;
    }
    return changed;
}

// A short look at a small response body for the play log (so a game's exchange with its
// servers can be rebuilt offline): gzip undone, values of secret-looking JSON keys and
// query strings blanked, other control bytes shown as dots. Empty for large or absent bodies.
inline std::string httpBodyPreview(const char* p, size_t n) {
    const std::string all(p, n);
    const size_t split = all.find("\r\n\r\n");
    if (split == std::string::npos) return {};
    std::string lowerHead = all.substr(0, split);
    for (char& c : lowerHead) c = char(std::tolower(static_cast<unsigned char>(c)));
    std::string body = all.substr(split + 4);
    if (body.empty() || body.size() > 4096) return {};
    if (lowerHead.find("content-encoding: gzip") != std::string::npos) {
        std::string out(16384, '\0');
        z_stream z{};
        if (inflateInit2(&z, 16 + MAX_WBITS) != Z_OK) return {};
        z.next_in = reinterpret_cast<Bytef*>(body.data()); z.avail_in = uInt(body.size());
        z.next_out = reinterpret_cast<Bytef*>(out.data()); z.avail_out = uInt(out.size());
        const int r = inflate(&z, Z_FINISH);
        out.resize(z.total_out);
        inflateEnd(&z);
        if (r != Z_STREAM_END && r != Z_BUF_ERROR) return {};
        body = out;
    }
    // Blank secrets: "...token...": "value", password=..., and URL queries.
    static const char* const kSecret[] = {"token", "password", "secret", "credential", "session", "signature", "sig", "key", "email", "name"};
    std::string lower = body;
    for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
    for (size_t i = 0; i < body.size(); ++i) {
        if (body[i] != '"') continue;
        const size_t end = body.find('"', i + 1);
        if (end == std::string::npos) break;
        const std::string k = lower.substr(i + 1, end - i - 1);
        size_t v = end + 1;
        while (v < body.size() && (body[v] == ' ' || body[v] == ':')) ++v;
        bool secret = false;
        for (const char* s : kSecret) if (k.find(s) != std::string::npos) { secret = true; break; }
        if (secret && v > end + 1 && v < body.size() && body[v] == '"') {
            const size_t ve = body.find('"', v + 1);
            if (ve == std::string::npos) break;
            for (size_t j = v + 1; j < ve; ++j) body[j] = '*';
            i = ve;
        } else i = end;
    }
    for (size_t q = body.find('?'); q != std::string::npos; q = body.find('?', q + 1))
        for (size_t j = q + 1; j < body.size() && body[j] != '"' && body[j] != ' ' && body[j] != '\n' && body[j] != '<'; ++j) body[j] = '*';
    if (body.size() > 600) body = body.substr(0, 600) + "...";
    for (char& c : body) if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) >= 0x7F) c = '.';
    return body;
}

// One bridged connection's lines for the play log: each request and response with its key
// headers, repeats folded into a count (a game that polls would otherwise flood the log),
// and the bytes each side sent when it ends.
class HttpLog {
public:
    HttpLog(std::string host, std::function<void(const std::string&)> log) : host_(std::move(host)), log_(std::move(log)) {}
    void chunk(bool fromGame, const char* p, size_t n) {
        (fromGame ? game_ : server_) += n;
        if (!log_) return;
        const std::string line = httpSummary(p, n);
        if (line.empty()) return;
        Side& side = fromGame ? toServer_ : toGame_;   // a poll alternates request and reply: fold each side alone
        const std::string tagged = std::string(fromGame ? "<- " : "-> ") + line;
        if (tagged == side.last) {
            ++side.count;
            if (side.count == 10 || side.count == 100 || side.count == 1000) say(side.last + " (" + std::to_string(side.count) + " times so far)");
            return;
        }
        flush(side);
        side.last = tagged;
        side.count = 1;
        say(tagged);
        if (!fromGame) if (const std::string body = httpBodyPreview(p, n); !body.empty()) say("   body: " + body);
    }
    void ended(const char* how) {
        if (!log_) return;
        flush(toServer_);
        flush(toGame_);
        log_("Internet: " + host_ + " connection ended, " + how + " (game sent " + std::to_string(game_) +
             " bytes, server sent " + std::to_string(server_) + ")");
    }
private:
    struct Side { std::string last; int count = 0; };   // count includes the first time
    void say(const std::string& s) { if (lines_++ < 400) log_("Internet: " + host_ + " " + s); }
    void flush(Side& side) {
        if (side.count > 1 && side.count != 10 && side.count != 100 && side.count != 1000)
            say(side.last + " (" + std::to_string(side.count) + " times)");
        side.count = 0;
    }
    std::string host_;
    std::function<void(const std::string&)> log_;
    Side toServer_, toGame_;
    int lines_ = 0;
    uint64_t game_ = 0, server_ = 0;
};

// The version a TLS ClientHello asks for (0x0301 TLS 1.0, 0x0302 TLS 1.1, 0x0303 TLS 1.2 and
// later), or 0 when the bytes are not a ClientHello. A game that already speaks TLS 1.2 or
// later is passed through untouched, so a game that checks certificates keeps working;
// only older TLS is terminated at the bridge.
inline uint16_t clientHelloVersion(const unsigned char* p, size_t n) {
    if (n < 11 || p[0] != 0x16 || p[5] != 0x01) return 0;
    return uint16_t(p[9] << 8 | p[10]);
}
constexpr uint16_t kModernTls = 0x0303;

// Linux with OpenSSL 3; nullptr elsewhere (`why` says why). verifyServer=false is for
// tests against a local server with a self-signed certificate.
std::shared_ptr<TlsBridge> makeOpenSslTlsBridge(bool verifyServer, std::string* why);

}  // namespace Hydrogen::Runtime
