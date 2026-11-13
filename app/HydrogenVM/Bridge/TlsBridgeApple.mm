// HydrogenVM — TLS bridge on Security.framework (iPhone). See src/runtime/libc/TlsBridge.hpp.
//
// The game's side: SecureTransport as a TLS server that still accepts TLS 1.0 (what a
// 2013 game's own OpenSSL speaks), with a self-signed RSA identity made once on this
// device and kept in the app's keychain. The server's side: SecureTransport as a TLS
// 1.2 client that checks the server's certificate and name the usual way. All of the
// cryptography is the system's; this file only moves bytes and builds the
// certificate's DER structure (signed with SecKeyCreateSignature).
#import <Foundation/Foundation.h>
#import <Security/Security.h>
#import <Security/SecureTransport.h>

#include "TlsBridgeApple.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"   // SecureTransport: the system TLS that still speaks TLS 1.0

namespace {

using Bytes = std::vector<uint8_t>;

// ---- a minimal DER writer for one self-signed certificate
void derLength(Bytes& o, size_t n) {
    if (n < 128) { o.push_back(uint8_t(n)); return; }
    Bytes l;
    while (n) { l.insert(l.begin(), uint8_t(n & 0xFF)); n >>= 8; }
    o.push_back(uint8_t(0x80 | l.size()));
    o.insert(o.end(), l.begin(), l.end());
}
Bytes der(uint8_t tag, const Bytes& body) {
    Bytes o{tag};
    derLength(o, body.size());
    o.insert(o.end(), body.begin(), body.end());
    return o;
}
Bytes join(std::initializer_list<Bytes> parts) {
    Bytes o;
    for (const auto& p : parts) o.insert(o.end(), p.begin(), p.end());
    return o;
}
Bytes text(uint8_t tag, const char* s) { return der(tag, Bytes(s, s + strlen(s))); }
Bytes bytesOf(CFDataRef d) {
    const auto* p = CFDataGetBytePtr(d);
    return Bytes(p, p + CFDataGetLength(d));
}
const Bytes kSha256Rsa = der(0x30, join({der(0x06, {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0B}), Bytes{0x05, 0x00}}));
const Bytes kRsa = der(0x30, join({der(0x06, {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01}), Bytes{0x05, 0x00}}));

NSString* const kLabel = @"HydrogenVM TLS bridge";

SecIdentityRef findIdentity() {
    NSDictionary* q = @{(id)kSecClass: (id)kSecClassIdentity, (id)kSecReturnRef: @YES, (id)kSecMatchLimit: (id)kSecMatchLimitAll};
    CFTypeRef found = nullptr;
    if (SecItemCopyMatching((__bridge CFDictionaryRef)q, &found) != errSecSuccess || !found) return nullptr;
    NSArray* all = (__bridge_transfer NSArray*)found;
    for (id item in all) {
        SecIdentityRef ident = (__bridge SecIdentityRef)item;
        SecCertificateRef cert = nullptr;
        if (SecIdentityCopyCertificate(ident, &cert) != errSecSuccess || !cert) continue;
        NSString* summary = (__bridge_transfer NSString*)SecCertificateCopySubjectSummary(cert);
        CFRelease(cert);
        if ([summary isEqualToString:kLabel]) return (SecIdentityRef)CFRetain(ident);
    }
    return nullptr;
}

SecIdentityRef makeIdentity(std::string& why) {
    NSData* tag = [@"com.lfull.HydrogenVM.tlsbridge" dataUsingEncoding:NSUTF8StringEncoding];
    NSDictionary* attrs = @{(id)kSecAttrKeyType: (id)kSecAttrKeyTypeRSA, (id)kSecAttrKeySizeInBits: @2048,
                            (id)kSecPrivateKeyAttrs: @{(id)kSecAttrIsPermanent: @YES, (id)kSecAttrApplicationTag: tag,
                                                       (id)kSecAttrLabel: kLabel}};
    CFErrorRef err = nullptr;
    SecKeyRef priv = SecKeyCreateRandomKey((__bridge CFDictionaryRef)attrs, &err);
    if (!priv) { why = "key: " + std::string(err ? [[(__bridge NSError*)err description] UTF8String] : "failed"); if (err) CFRelease(err); return nullptr; }
    SecKeyRef pub = SecKeyCopyPublicKey(priv);
    CFDataRef pubData = pub ? SecKeyCopyExternalRepresentation(pub, nullptr) : nullptr;   // PKCS#1 RSAPublicKey
    if (pub) CFRelease(pub);
    if (!pubData) { CFRelease(priv); why = "public key export failed"; return nullptr; }
    Bytes bits{0x00};
    const Bytes pk = bytesOf(pubData);
    bits.insert(bits.end(), pk.begin(), pk.end());
    CFRelease(pubData);
    const Bytes name = der(0x30, der(0x31, der(0x30, join({der(0x06, {0x55, 0x04, 0x03}), text(0x0C, "HydrogenVM TLS bridge")}))));
    const Bytes validity = der(0x30, join({text(0x17, "250101000000Z"), text(0x17, "491231235959Z")}));
    const Bytes spki = der(0x30, join({kRsa, der(0x03, bits)}));
    const Bytes tbs = der(0x30, join({Bytes{0xA0, 0x03, 0x02, 0x01, 0x02}, Bytes{0x02, 0x01, 0x01}, kSha256Rsa, name, validity, name, spki}));
    NSData* tbsData = [NSData dataWithBytes:tbs.data() length:tbs.size()];
    CFDataRef sig = SecKeyCreateSignature(priv, kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA256, (__bridge CFDataRef)tbsData, &err);
    CFRelease(priv);
    if (!sig) { why = "signature failed"; if (err) CFRelease(err); return nullptr; }
    Bytes sigBits{0x00};
    const Bytes s = bytesOf(sig);
    sigBits.insert(sigBits.end(), s.begin(), s.end());
    CFRelease(sig);
    const Bytes certDer = der(0x30, join({tbs, kSha256Rsa, der(0x03, sigBits)}));
    SecCertificateRef cert = SecCertificateCreateWithData(nullptr, (__bridge CFDataRef)[NSData dataWithBytes:certDer.data() length:certDer.size()]);
    if (!cert) { why = "certificate not accepted"; return nullptr; }
    const OSStatus added = SecItemAdd((__bridge CFDictionaryRef)@{(id)kSecClass: (id)kSecClassCertificate, (id)kSecValueRef: (__bridge id)cert,
                                                                  (id)kSecAttrLabel: kLabel}, nullptr);
    CFRelease(cert);
    if (added != errSecSuccess && added != errSecDuplicateItem) { why = "keychain " + std::to_string(int(added)); return nullptr; }
    SecIdentityRef ident = findIdentity();
    if (!ident) why = "identity not found after adding";
    return ident;
}

// ---- nonblocking sockets for SecureTransport
OSStatus readFn(SSLConnectionRef c, void* data, size_t* len) {
    const int fd = int(intptr_t(c));
    size_t want = *len, got = 0;
    while (got < want) {
        const ssize_t n = ::recv(fd, static_cast<char*>(data) + got, want - got, 0);
        if (n > 0) { got += size_t(n); continue; }
        *len = got;
        if (n == 0) return errSSLClosedGraceful;
        if (errno == EINTR) continue;
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? errSSLWouldBlock : errSSLClosedAbort;
    }
    *len = got;
    return noErr;
}
OSStatus writeFn(SSLConnectionRef c, const void* data, size_t* len) {
    const int fd = int(intptr_t(c));
    size_t want = *len, put = 0;
    while (put < want) {
        const ssize_t n = ::send(fd, static_cast<const char*>(data) + put, want - put, 0);
        if (n > 0) { put += size_t(n); continue; }
        *len = put;
        if (n < 0 && errno == EINTR) continue;
        return (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) ? errSSLWouldBlock : errSSLClosedAbort;
    }
    *len = put;
    return noErr;
}
void prepare(int fd) {
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}
bool waitFd(int fd, short events, int ms) {
    pollfd p{fd, events, 0};
    return ::poll(&p, 1, ms) > 0;
}
OSStatus handshake(SSLContextRef ctx, int fd) {
    OSStatus s;
    while ((s = SSLHandshake(ctx)) == errSSLWouldBlock)
        if (!waitFd(fd, POLLIN | POLLOUT, 20000)) return errSSLClosedAbort;
    return s;
}
bool writeAll(SSLContextRef to, int fd, const uint8_t* p, size_t n) {
    size_t done = 0;
    OSStatus s = noErr;
    while (done < n) {
        size_t w = 0;
        s = SSLWrite(to, p + done, n - done, &w);
        done += w;
        if (s == errSSLWouldBlock) { if (!waitFd(fd, POLLOUT, 20000)) return false; continue; }
        if (s != noErr) return false;
    }
    while (s == errSSLWouldBlock) {   // a queued record still waiting for the socket
        size_t w = 0;
        if (!waitFd(fd, POLLOUT, 20000)) return false;
        s = SSLWrite(to, nullptr, 0, &w);
    }
    return true;
}
// One read from `from` passed on to `to`; false when `from` closed or failed.
bool transfer(SSLContextRef from, SSLContextRef to, int toFd) {
    uint8_t buf[16384];
    size_t n = 0;
    const OSStatus s = SSLRead(from, buf, sizeof buf, &n);
    if (n && !writeAll(to, toFd, buf, n)) return false;
    return s == noErr || s == errSSLWouldBlock;
}
std::string status(OSStatus s) { return std::to_string(int(s)); }

class AppleTlsBridge final : public Hydrogen::Runtime::TlsBridge {
public:
    uint16_t open(const std::string& host, uint32_t ip, uint16_t port, std::function<void(const std::string&)> log) override {
        const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener < 0) return 0;
        sockaddr_in a{}; a.sin_len = sizeof a; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t n = sizeof a;
        if (::bind(listener, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0 || ::listen(listener, 1) < 0 ||
            ::getsockname(listener, reinterpret_cast<sockaddr*>(&a), &n) < 0) { ::close(listener); return 0; }
        std::thread([this, listener, host, ip, port, log] { run(listener, host, ip, port, log); }).detach();
        return ntohs(a.sin_port);
    }

private:
    SecIdentityRef identity(const std::function<void(const std::string&)>& log) {
        std::lock_guard lock(mu_);
        if (!identity_ && !failed_) {
            identity_ = findIdentity();
            std::string why;
            if (!identity_) identity_ = makeIdentity(why);
            if (!identity_) { failed_ = true; if (log) log("Internet: TLS bridge identity unavailable (" + why + ")"); }
        }
        return identity_;
    }
    void run(int listener, std::string host, uint32_t ip, uint16_t port, std::function<void(const std::string&)> log) {
        @autoreleasepool {
            const int guest = waitFd(listener, POLLIN, 30000) ? ::accept(listener, nullptr, nullptr) : -1;
            ::close(listener);
            if (guest < 0) return;
            prepare(guest);
            SecIdentityRef ident = identity(log);
            SSLContextRef g = ident ? SSLCreateContext(nullptr, kSSLServerSide, kSSLStreamType) : nullptr;
            SSLContextRef u = nullptr;
            int up = -1;
            if (g) {
                SSLSetIOFuncs(g, readFn, writeFn);
                SSLSetConnection(g, SSLConnectionRef(intptr_t(guest)));
                SSLSetProtocolVersionMin(g, kTLSProtocol1);
                SSLSetCertificate(g, (__bridge CFArrayRef)@[(__bridge id)ident]);
                const OSStatus hs = handshake(g, guest);
                if (hs != noErr) {
                    if (log) log("Internet: the game's secure connection to " + host + " could not start (" + status(hs) + ")");
                } else if ((up = connectUpstream(ip, port)) < 0) {
                    if (log) log("Internet: could not reach " + host + ":" + std::to_string(port));
                } else {
                    u = SSLCreateContext(nullptr, kSSLClientSide, kSSLStreamType);
                    SSLSetIOFuncs(u, readFn, writeFn);
                    SSLSetConnection(u, SSLConnectionRef(intptr_t(up)));
                    SSLSetProtocolVersionMin(u, kTLSProtocol12);
                    if (inet_addr(host.c_str()) == INADDR_NONE) SSLSetPeerDomainName(u, host.c_str(), host.size());
                    const OSStatus hu = handshake(u, up);
                    if (hu != noErr) {
                        if (log) log("Internet: modern TLS to " + host + " failed (" + status(hu) + ")");
                    } else {
                        if (log) log("Internet: secure connection to " + host + " bridged");
                        pump(g, guest, u, up);
                    }
                }
            }
            if (u) { SSLClose(u); CFRelease(u); }
            if (g) { SSLClose(g); CFRelease(g); }
            if (up >= 0) ::close(up);
            ::close(guest);
        }
    }
    static int connectUpstream(uint32_t ip, uint16_t port) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        prepare(fd);
        sockaddr_in a{}; a.sin_len = sizeof a; a.sin_family = AF_INET; a.sin_addr.s_addr = ip; a.sin_port = htons(port);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0 && errno != EINPROGRESS) { ::close(fd); return -1; }
        int err = 0; socklen_t len = sizeof err;
        if (!waitFd(fd, POLLOUT, 20000) || ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err) { ::close(fd); return -1; }
        return fd;
    }
    static void pump(SSLContextRef g, int gfd, SSLContextRef u, int ufd) {
        for (;;) {
            size_t ga = 0, ua = 0;
            SSLGetBufferedReadSize(g, &ga);
            SSLGetBufferedReadSize(u, &ua);
            if (!ga && !ua) {
                pollfd fds[2] = {{gfd, POLLIN, 0}, {ufd, POLLIN, 0}};
                if (::poll(fds, 2, 120000) <= 0) return;   // two quiet minutes: the game has moved on
            }
            if (!transfer(g, u, ufd) || !transfer(u, g, gfd)) return;
        }
    }
    std::mutex mu_;
    SecIdentityRef identity_ = nullptr;
    bool failed_ = false;
};

}  // namespace

namespace Hydrogen::App {
std::shared_ptr<Hydrogen::Runtime::TlsBridge> makeAppleTlsBridge() {
    static auto bridge = std::make_shared<AppleTlsBridge>();   // one identity for the app's lifetime
    return bridge;
}
}  // namespace Hydrogen::App
