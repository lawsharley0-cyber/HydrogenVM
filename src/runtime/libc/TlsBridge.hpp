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
                                        "transfer-encoding:", "content-encoding:", "connection:"};
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
            if (lower.rfind(k, 0) == 0 && line.size() < 200) { extra += (extra.empty() ? "" : ", ") + line; break; }
        i = e + 1;
    }
    return extra.empty() ? out : out + " [" + extra + "]";
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
        const std::string tagged = std::string(fromGame ? "<- " : "-> ") + line;
        if (tagged == last_) {
            ++repeats_;
            if (repeats_ == 10 || repeats_ == 100 || repeats_ == 1000) say(last_ + " (" + std::to_string(repeats_) + " times so far)");
            return;
        }
        flushRepeats();
        last_ = tagged;
        say(tagged);
    }
    void ended(const char* how) {
        if (!log_) return;
        flushRepeats();
        log_("Internet: " + host_ + " connection ended, " + how + " (game sent " + std::to_string(game_) +
             " bytes, server sent " + std::to_string(server_) + ")");
    }
private:
    void say(const std::string& s) { if (lines_++ < 400) log_("Internet: " + host_ + " " + s); }
    void flushRepeats() {
        if (repeats_ >= 1 && repeats_ != 10 && repeats_ != 100 && repeats_ != 1000)
            say(last_ + " (" + std::to_string(repeats_ + 1) + " times)");
        repeats_ = 0;
    }
    std::string host_;
    std::function<void(const std::string&)> log_;
    std::string last_;
    int repeats_ = 0, lines_ = 0;
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
