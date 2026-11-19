// HydrogenVM — HTTP client on OpenSSL for the desktop bench. See HttpClient.hpp.
// HTTP/1.1, one connection per request ("Connection: close"), redirects followed (up to 5),
// chunked bodies decoded, gzip/deflate bodies inflated. The certificate is checked against the
// system's trust store, as NSURLSession does.
#include "HttpClient.hpp"

#if defined(HVM_HAVE_OPENSSL) && !defined(__APPLE__)
#include <netdb.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <zlib.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <thread>

namespace Hydrogen::Runtime {
namespace {

struct Url { bool https = false; std::string host, path = "/"; std::string port; bool ok = false; };
Url parseUrl(const std::string& s) {
    Url u;
    size_t p = 0;
    if (s.rfind("https://", 0) == 0) { u.https = true; p = 8; }
    else if (s.rfind("http://", 0) == 0) p = 7;
    else return u;
    const size_t slash = s.find('/', p);
    std::string hostPort = s.substr(p, slash == std::string::npos ? std::string::npos : slash - p);
    if (const size_t at = hostPort.rfind('@'); at != std::string::npos) hostPort = hostPort.substr(at + 1);
    if (const size_t colon = hostPort.rfind(':'); colon != std::string::npos && hostPort.find(']') == std::string::npos) {
        u.port = hostPort.substr(colon + 1);
        hostPort = hostPort.substr(0, colon);
    }
    u.host = hostPort;
    if (u.port.empty()) u.port = u.https ? "443" : "80";
    if (slash != std::string::npos) u.path = s.substr(slash);
    if (const size_t h = u.path.find('#'); h != std::string::npos) u.path = u.path.substr(0, h);
    u.ok = !u.host.empty();
    return u;
}
std::string lower(std::string s) { for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }

struct Conn {
    int fd = -1;
    SSL_CTX* ctx = nullptr;
    SSL* ssl = nullptr;
    ~Conn() {
        if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
        if (ctx) SSL_CTX_free(ctx);
        if (fd >= 0) ::close(fd);
    }
    bool send(const std::string& d) {
        size_t off = 0;
        while (off < d.size()) {
            const int n = ssl ? SSL_write(ssl, d.data() + off, int(d.size() - off)) : int(::send(fd, d.data() + off, d.size() - off, MSG_NOSIGNAL));
            if (n <= 0) return false;
            off += size_t(n);
        }
        return true;
    }
    // Everything until the server closes, or `deadline`.
    bool readAll(std::string& out, std::chrono::steady_clock::time_point deadline) {
        char buf[16384];
        for (;;) {
            if (!ssl || SSL_pending(ssl) == 0) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
                if (left <= 0) return false;
                pollfd p{fd, POLLIN, 0};
                if (::poll(&p, 1, int(std::min<long long>(left, 1000))) == 0) continue;
            }
            const int n = ssl ? SSL_read(ssl, buf, sizeof buf) : int(::recv(fd, buf, sizeof buf, 0));
            if (n > 0) { out.append(buf, size_t(n)); continue; }
            if (ssl) {
                const int e = SSL_get_error(ssl, n);
                if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) continue;
            }
            return true;   // closed (a TLS close without close_notify still ends the body)
        }
    }
};

bool inflateBody(std::string& body, bool gzip) {
    z_stream z{};
    if (inflateInit2(&z, gzip ? 16 + MAX_WBITS : MAX_WBITS) != Z_OK) return false;
    std::string out;
    z.next_in = reinterpret_cast<Bytef*>(body.data());
    z.avail_in = uInt(body.size());
    char buf[32768];
    int r;
    do {
        z.next_out = reinterpret_cast<Bytef*>(buf);
        z.avail_out = sizeof buf;
        r = inflate(&z, Z_NO_FLUSH);
        if (r != Z_OK && r != Z_STREAM_END) { inflateEnd(&z); return false; }
        out.append(buf, sizeof buf - z.avail_out);
    } while (r != Z_STREAM_END && z.avail_in > 0);
    inflateEnd(&z);
    body = std::move(out);
    return true;
}

HttpResponse once(const HttpRequest& rq, const std::string& url, std::string& location) {
    HttpResponse r;
    r.url = url;
    const Url u = parseUrl(url);
    if (!u.ok) { r.errorCode = -1002; r.error = "unsupported URL"; return r; }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(int64_t(std::max(1.0, rq.timeoutSeconds) * 1000));
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(u.host.c_str(), u.port.c_str(), &hints, &res) != 0 || !res) { r.errorCode = -1003; r.error = "A server with the specified hostname could not be found."; return r; }
    Conn c;
    for (addrinfo* a = res; a; a = a->ai_next) {
        c.fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (c.fd < 0) continue;
        timeval tv{std::max<long>(1, long(rq.timeoutSeconds)), 0};
        setsockopt(c.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (::connect(c.fd, a->ai_addr, a->ai_addrlen) == 0) break;
        ::close(c.fd);
        c.fd = -1;
    }
    freeaddrinfo(res);
    if (c.fd < 0) { r.errorCode = -1004; r.error = "Could not connect to the server."; return r; }
    if (u.https) {
        c.ctx = SSL_CTX_new(TLS_client_method());
        SSL_CTX_set_default_verify_paths(c.ctx);
        SSL_CTX_set_verify(c.ctx, SSL_VERIFY_PEER, nullptr);
        c.ssl = SSL_new(c.ctx);
        SSL_set_fd(c.ssl, c.fd);
        SSL_set_tlsext_host_name(c.ssl, u.host.c_str());
        SSL_set1_host(c.ssl, u.host.c_str());
        if (SSL_connect(c.ssl) != 1) { r.errorCode = -1200; r.error = "An SSL error has occurred and a secure connection to the server cannot be made."; return r; }
    }
    std::string head = rq.method + " " + u.path + " HTTP/1.1\r\nHost: " + u.host +
                       ((u.https && u.port != "443") || (!u.https && u.port != "80") ? ":" + u.port : "") +
                       "\r\nConnection: close\r\nAccept-Encoding: gzip, deflate\r\n";
    bool hasAgent = false;
    for (const auto& [k, v] : rq.headers) {
        const std::string lk = lower(k);
        if (lk == "host" || lk == "connection" || lk == "accept-encoding" || lk == "content-length") continue;
        if (lk == "user-agent") hasAgent = true;
        head += k + ": " + v + "\r\n";
    }
    if (!hasAgent) head += "User-Agent: HydrogenVM\r\n";
    if (!rq.body.empty() || rq.method == "POST" || rq.method == "PUT") head += "Content-Length: " + std::to_string(rq.body.size()) + "\r\n";
    head += "\r\n";
    if (!c.send(head + rq.body)) { r.errorCode = -1005; r.error = "The network connection was lost."; return r; }
    std::string raw;
    if (!c.readAll(raw, deadline)) { r.errorCode = -1001; r.error = "The request timed out."; return r; }
    const size_t end = raw.find("\r\n\r\n");
    if (end == std::string::npos || raw.rfind("HTTP/", 0) != 0) { r.errorCode = -1011; r.error = "bad server response"; return r; }
    r.status = std::atoi(raw.c_str() + raw.find(' ') + 1);
    bool chunked = false;
    std::string encoding;
    for (size_t i = raw.find("\r\n") + 2; i < end;) {
        const size_t e = raw.find("\r\n", i);
        const std::string line = raw.substr(i, e - i);
        i = e + 2;
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string k = line.substr(0, colon), v = line.substr(colon + 1);
        while (!v.empty() && v.front() == ' ') v.erase(0, 1);
        const std::string lk = lower(k);
        if (lk == "transfer-encoding" && lower(v).find("chunked") != std::string::npos) { chunked = true; continue; }
        if (lk == "content-encoding") { encoding = lower(v); continue; }   // the body is handed over decoded
        if (lk == "content-length") continue;                               // set again for the decoded body
        if (lk == "location") location = v;
        r.headers.push_back({k, v});
    }
    std::string body = raw.substr(end + 4);
    if (chunked) {
        std::string out;
        size_t i = 0;
        while (i < body.size()) {
            const size_t e = body.find("\r\n", i);
            if (e == std::string::npos) break;
            const size_t n = std::strtoul(body.substr(i, e - i).c_str(), nullptr, 16);
            if (n == 0) break;
            out += body.substr(e + 2, n);
            i = e + 2 + n + 2;
        }
        body = std::move(out);
    }
    if (encoding == "gzip" || encoding == "x-gzip") inflateBody(body, true);
    else if (encoding == "deflate") inflateBody(body, false);
    r.headers.push_back({"Content-Length", std::to_string(body.size())});
    r.body = std::move(body);
    return r;
}

std::string resolve(const std::string& base, const std::string& loc) {
    if (loc.rfind("http://", 0) == 0 || loc.rfind("https://", 0) == 0) return loc;
    const Url u = parseUrl(base);
    const std::string origin = (u.https ? "https://" : "http://") + u.host +
                               ((u.https && u.port != "443") || (!u.https && u.port != "80") ? ":" + u.port : "");
    if (!loc.empty() && loc[0] == '/') return origin + loc;
    const size_t slash = u.path.rfind('/');
    return origin + u.path.substr(0, slash + 1) + loc;
}

struct OpenSslHttpClient final : HttpClient {
    void fetch(HttpRequest rq, std::function<void(HttpResponse)> done) override {
        std::thread([rq = std::move(rq), done = std::move(done)]() mutable {
            std::string url = rq.url;
            HttpResponse r;
            for (int hop = 0; hop < 6; ++hop) {
                std::string location;
                r = once(rq, url, location);
                if (location.empty() || (r.status != 301 && r.status != 302 && r.status != 303 && r.status != 307 && r.status != 308)) break;
                url = resolve(url, location);
                if (r.status == 303 || ((r.status == 301 || r.status == 302) && rq.method == "POST")) { rq.method = "GET"; rq.body.clear(); }
            }
            done(std::move(r));
        }).detach();
    }
};

}  // namespace

std::shared_ptr<HttpClient> makeOpenSslHttpClient() { return std::make_shared<OpenSslHttpClient>(); }

}  // namespace Hydrogen::Runtime

#else
namespace Hydrogen::Runtime {
std::shared_ptr<HttpClient> makeOpenSslHttpClient() { return nullptr; }
}  // namespace Hydrogen::Runtime
#endif
