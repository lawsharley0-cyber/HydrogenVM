// HydrogenVM — TLS bridge on OpenSSL (Linux bench). See TlsBridge.hpp.
#include "TlsBridge.hpp"

#if defined(HVM_HAVE_OPENSSL) && !defined(__APPLE__)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <pthread.h>
#include <csignal>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

namespace Hydrogen::Runtime {
namespace {

std::string sslError() {
    char b[256] = "";
    const unsigned long e = ERR_get_error();
    if (e) ERR_error_string_n(e, b, sizeof b);
    return e ? b : "connection closed";
}

// Listens on 127.0.0.1 with an ephemeral port; returns the fd and fills port.
int localListener(uint16_t& port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t n = sizeof a;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0 || ::listen(fd, 1) < 0 ||
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &n) < 0) { ::close(fd); return -1; }
    port = ntohs(a.sin_port);
    return fd;
}

// Every socket the bridge touches gets send and receive deadlines, so a peer that stops
// talking ends the connection instead of holding a worker forever.
void deadlines(int fd, int seconds) {
    timeval tv{seconds, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}
// Waits until the first 11 bytes (enough for a ClientHello's version) can be peeked.
uint16_t peekHello(int fd) {
    unsigned char b[11];
    for (int i = 0; i < 200; ++i) {
        const ssize_t n = ::recv(fd, b, sizeof b, MSG_PEEK);
        if (n <= 0) return 0;
        if (n >= 11) return clientHelloVersion(b, size_t(n));
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, 100) < 0) return 0;
    }
    return 0;
}
// Bytes both ways, unchanged, for a game whose own TLS is new enough.
void relay(int a, int b) {
    char buf[16384];
    for (;;) {
        pollfd fds[2] = {{a, POLLIN, 0}, {b, POLLIN, 0}};
        if (::poll(fds, 2, 120000) <= 0) return;
        for (int i = 0; i < 2; ++i) {
            if (!fds[i].revents) continue;
            const ssize_t n = ::recv(fds[i].fd, buf, sizeof buf, 0);
            if (n <= 0) return;
            for (ssize_t done = 0; done < n;) {
                const ssize_t w = ::send(i ? a : b, buf + done, size_t(n - done), MSG_NOSIGNAL);
                if (w <= 0) return;
                done += w;
            }
        }
    }
}

int connectUpstream(uint32_t ip, uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    timeval tv{20, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);   // bounds connect too
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = ip; a.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) { ::close(fd); return -1; }
    timeval none{0, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &none, sizeof none);
    int one = 1; ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

class OpenSslBridge final : public TlsBridge, public std::enable_shared_from_this<OpenSslBridge> {
public:
    explicit OpenSslBridge(bool verify) : verify_(verify) {}
    ~OpenSslBridge() override {
        if (server_) SSL_CTX_free(server_);
        if (client_) SSL_CTX_free(client_);
    }
    bool init(std::string* why) {
        // Guest side: accept the oldest TLS a game may speak, with a fresh self-signed key.
        server_ = SSL_CTX_new(TLS_server_method());
        client_ = SSL_CTX_new(TLS_client_method());
        if (!server_ || !client_) { if (why) *why = sslError(); return false; }
        SSL_CTX_set_security_level(server_, 0);
        SSL_CTX_set_min_proto_version(server_, TLS1_VERSION);
        SSL_CTX_set_cipher_list(server_, "ALL:@SECLEVEL=0");
        EVP_PKEY* key = EVP_RSA_gen(2048);
        X509* cert = X509_new();
        if (!key || !cert) { if (why) *why = sslError(); EVP_PKEY_free(key); X509_free(cert); return false; }
        ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
        X509_gmtime_adj(X509_getm_notBefore(cert), -86400);
        X509_gmtime_adj(X509_getm_notAfter(cert), 3650L * 86400);
        X509_set_pubkey(cert, key);
        X509_NAME* name = X509_get_subject_name(cert);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("HydrogenVM TLS bridge"), -1, -1, 0);
        X509_set_issuer_name(cert, name);
        X509_sign(cert, key, EVP_sha256());
        const bool ok = SSL_CTX_use_certificate(server_, cert) == 1 && SSL_CTX_use_PrivateKey(server_, key) == 1;
        X509_free(cert); EVP_PKEY_free(key);
        if (!ok) { if (why) *why = sslError(); return false; }
        // Server side: modern TLS, the server's certificate checked against the system's roots.
        SSL_CTX_set_min_proto_version(client_, TLS1_2_VERSION);
        if (verify_) {
            SSL_CTX_set_default_verify_paths(client_);
            SSL_CTX_set_verify(client_, SSL_VERIFY_PEER, nullptr);
        }
        return true;
    }

    uint16_t open(const std::string& host, uint32_t ip, uint16_t port, std::function<void(const std::string&)> log) override {
        uint16_t local = 0;
        const int listener = localListener(local);
        if (listener < 0) return 0;
        // The worker keeps the bridge (and its TLS contexts) alive until it returns.
        std::thread([self = shared_from_this(), listener, host, ip, port, log] { self->run(listener, host, ip, port, log); }).detach();
        return local;
    }

private:
    void run(int listener, std::string host, uint32_t ip, uint16_t port, std::function<void(const std::string&)> log) {
        // OpenSSL writes with write(): a peer that hung up must not kill the process with SIGPIPE.
        sigset_t pipe; sigemptyset(&pipe); sigaddset(&pipe, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &pipe, nullptr);
        pollfd p{listener, POLLIN, 0};
        const int guest = ::poll(&p, 1, 30000) == 1 ? ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC) : -1;
        ::close(listener);
        if (guest < 0) return;
        deadlines(guest, 30);
        const uint16_t asked = peekHello(guest);
        if (asked >= kModernTls) {
            const int up = connectUpstream(ip, port);
            if (up < 0) { if (log) log("Internet: could not reach " + host + ":" + std::to_string(port)); ::close(guest); return; }
            if (log) log("Internet: secure connection to " + host + " passed through (the game's own TLS is modern)");
            relay(guest, up);
            ::close(up); ::close(guest);
            return;
        }
        SSL* g = SSL_new(server_);
        SSL* u = nullptr;
        int up = -1;
        SSL_set_fd(g, guest);
        if (SSL_accept(g) != 1) {
            if (log) log("Internet: the game's secure connection to " + host + " could not start (" + sslError() + ")");
        } else if ((up = connectUpstream(ip, port)) < 0) {
            if (log) log("Internet: could not reach " + host + ":" + std::to_string(port));
        } else {
            deadlines(up, 30);
            u = SSL_new(client_);
            SSL_set_fd(u, up);
            const bool named = inet_addr(host.c_str()) == INADDR_NONE;
            if (named) {
                SSL_set_tlsext_host_name(u, host.c_str());
                if (verify_) SSL_set1_host(u, host.c_str());
            } else if (verify_) {
                X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(u), host.c_str());   // an address: its certificate must name it
            }
            if (SSL_connect(u) != 1) {
                if (log) log("Internet: modern TLS to " + host + " failed (" + sslError() + ")");
            } else {
                if (log) log("Internet: secure connection to " + host + " bridged (game TLS " + SSL_get_version(g) +
                             ", server " + SSL_get_version(u) + ")");
                pump(g, guest, u, up, host, log);
            }
        }
        if (u) { SSL_shutdown(u); SSL_free(u); }
        SSL_shutdown(g); SSL_free(g);
        if (up >= 0) ::close(up);
        ::close(guest);
    }
    // Copies application data both ways until either side closes.
    static void pump(SSL* g, int gfd, SSL* u, int ufd, const std::string& host, const std::function<void(const std::string&)>& log) {
        char buf[16384];
        HttpLog http(host, log);
        for (;;) {
            bool gr = SSL_pending(g) > 0, ur = SSL_pending(u) > 0;
            if (!gr && !ur) {
                pollfd fds[2] = {{gfd, POLLIN, 0}, {ufd, POLLIN, 0}};
                const int r = ::poll(fds, 2, 120000);
                if (r <= 0) { http.ended("idle for two minutes"); return; }
                gr = fds[0].revents != 0; ur = fds[1].revents != 0;
            }
            if (gr) {
                const int n = SSL_read(g, buf, sizeof buf);
                if (n <= 0) { if (SSL_get_error(g, n) == SSL_ERROR_WANT_READ) continue; http.ended("closed by the game"); return; }
                http.chunk(true, buf, size_t(n));
                if (SSL_write(u, buf, n) != n) { http.ended("server stopped taking data"); return; }
            }
            if (ur) {
                const int n = SSL_read(u, buf, sizeof buf);
                if (n <= 0) { if (SSL_get_error(u, n) == SSL_ERROR_WANT_READ) continue; http.ended("closed by the server"); return; }
                http.chunk(false, buf, size_t(n));
                if (SSL_write(g, buf, n) != n) { http.ended("game stopped taking data"); return; }
            }
        }
    }
    bool verify_;
    SSL_CTX* server_ = nullptr;
    SSL_CTX* client_ = nullptr;
};

}  // namespace

std::shared_ptr<TlsBridge> makeOpenSslTlsBridge(bool verifyServer, std::string* why) {
    auto b = std::make_shared<OpenSslBridge>(verifyServer);
    if (!b->init(why)) return nullptr;
    return b;
}

}  // namespace Hydrogen::Runtime

#else
namespace Hydrogen::Runtime {
std::shared_ptr<TlsBridge> makeOpenSslTlsBridge(bool, std::string* why) {
    if (why) *why = "no OpenSSL in this build";
    return nullptr;
}
}  // namespace Hydrogen::Runtime
#endif
