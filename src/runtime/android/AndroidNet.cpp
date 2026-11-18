// Bionic uses a Linux socket ABI. IPv4 internet and loopback UDP use the
// shared libc policy and TLS bridge. Other transports here stay on loopback.
#include "runtime/android/BionicInternal.hpp"
#include "runtime/libc/Network.hpp"
#include "runtime/Support.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace Hydrogen::Runtime::Android {
namespace {
constexpr int kIPv4 = 2, kIPv6 = 10, kNonblock = 0x800, kCloexec = 0x80000;
constexpr int kSockFlags = kNonblock | kCloexec;
bool owned(Ctx& x, int fd) { return isNetworkSocket(x.proc, fd) || isAndroidHostSocket(x.proc, fd); }

struct Address {
    sockaddr_storage native{};
    uint32_t length = 0;
    int family = 0;
    bool loopback = false, any = false;
    uint8_t bsd[28]{};
};
bool loadAddress(Ctx& x, Address& a, GuestPtr addr, uint32_t size) {
    if (size < 2 || !guestSpan(x, addr, 2, Memory::kProtRead)) return false;
    a.family = x.mem.read<uint16_t>(addr);
    a.length = a.family == kIPv4 ? 16 : a.family == kIPv6 ? 28 : 0;
    if (!a.length || size < a.length || !guestSpan(x, addr, a.length, Memory::kProtRead)) return false;
    x.mem.copyOut(a.bsd, addr, a.length);
    a.bsd[0] = uint8_t(a.length);
    a.bsd[1] = a.family == kIPv4 ? 2 : 30;
    if (a.family == kIPv4) {
        auto* in = reinterpret_cast<sockaddr_in*>(&a.native);
        in->sin_family = AF_INET;
#ifdef __APPLE__
        in->sin_len = sizeof(*in);
#endif
        std::memcpy(&in->sin_port, a.bsd + 2, 2);
        std::memcpy(&in->sin_addr, a.bsd + 4, 4);
        a.any = in->sin_addr.s_addr == 0;
        a.loopback = (ntohl(in->sin_addr.s_addr) >> 24) == 127;
    } else {
        auto* in = reinterpret_cast<sockaddr_in6*>(&a.native);
        in->sin6_family = AF_INET6;
#ifdef __APPLE__
        in->sin6_len = sizeof(*in);
#endif
        std::memcpy(&in->sin6_port, a.bsd + 2, 2);
        std::memcpy(&in->sin6_flowinfo, a.bsd + 4, 4);
        std::memcpy(&in->sin6_addr, a.bsd + 8, 16);
        std::memcpy(&in->sin6_scope_id, a.bsd + 24, 4);
        a.any = IN6_IS_ADDR_UNSPECIFIED(&in->sin6_addr);
        a.loopback = IN6_IS_ADDR_LOOPBACK(&in->sin6_addr);
    }
    return true;
}
void localBind(Address& a) {
    if (!a.any) return;
    if (a.family == kIPv4)
        reinterpret_cast<sockaddr_in*>(&a.native)->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    else
        reinterpret_cast<sockaddr_in6*>(&a.native)->sin6_addr = in6addr_loopback;
}
uint32_t storeAddress(Ctx& x, GuestPtr out, uint32_t capacity, const sockaddr* address) {
    uint8_t bytes[28]{};
    uint32_t length = 2;
    if (address->sa_family == AF_INET) {
        const auto* a = reinterpret_cast<const sockaddr_in*>(address);
        bytes[0] = kIPv4; length = 16;
        std::memcpy(bytes + 2, &a->sin_port, 2);
        std::memcpy(bytes + 4, &a->sin_addr, 4);
    } else if (address->sa_family == AF_INET6) {
        const auto* a = reinterpret_cast<const sockaddr_in6*>(address);
        bytes[0] = kIPv6; length = 28;
        std::memcpy(bytes + 2, &a->sin6_port, 2);
        std::memcpy(bytes + 4, &a->sin6_flowinfo, 4);
        std::memcpy(bytes + 8, &a->sin6_addr, 16);
        std::memcpy(bytes + 24, &a->sin6_scope_id, 4);
    } else if (address->sa_family == AF_UNIX) bytes[0] = 1;
    x.mem.copyIn(out, bytes, std::min(capacity, length));
    return length;
}
bool outputAddress(Ctx& x, GuestPtr out, GuestPtr len) {
    return !out || (guestSpan(x, len, 4, Memory::kProtRW) &&
                    guestSpan(x, out, std::min(x.mem.read<uint32_t>(len), 28u), Memory::kProtWrite));
}
struct Scratch {
    Ctx& x;
    uint32_t addr;
    explicit Scratch(Ctx& x) : x(x), addr(x.proc.heap().allocZeroed(64)) {}
    ~Scratch() { if (addr) x.proc.heap().free(addr); }
};
bool socketFlags(int fd, int flags) {
    return ::fcntl(fd, F_SETFD, flags & kCloexec ? FD_CLOEXEC : 0) >= 0 &&
           (!(flags & kNonblock) || ::fcntl(fd, F_SETFL, O_NONBLOCK) >= 0);
}
bool waitIo(Ctx& x, int fd, int events) {
    using namespace std::chrono_literals;
    BlockedScope blocked(x.proc, "Android socket wait");
    for (;;) {
        if (x.proc.stopping() || x.cpu.async_pending.load(std::memory_order_acquire)) {
            setLinuxErrno(x, 4);
            return false;
        }
        const auto ready = pollDescriptor(x.proc, fd, events);
        if (ready & 32) { setLinuxErrno(x, 9); return false; }
        if (ready) return true;
        std::this_thread::sleep_for(5ms);
    }
}

int32_t a_socket(Ctx& x, int family, int type, int protocol) {
    if (family != kIPv4 && family != kIPv6) return linuxFailure(x, 97);
    const int flags = type & kSockFlags;
    const int style = type & ~kSockFlags;
    if ((style != SOCK_DGRAM && style != SOCK_STREAM) ||
        (protocol && protocol != (style == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP))) return linuxFailure(x, 93);
    const bool shared = family == kIPv4 && (style == SOCK_DGRAM || x.proc.config().internetAccess ||
                                             x.proc.config().loopbackNetwork);
    const int fd = shared ? int32_t(callIos(x, "_socket", {2, uint32_t(style), uint32_t(protocol)})) :
                            ::socket(family == kIPv4 ? AF_INET : AF_INET6, style, protocol);
    if (fd < 0) return shared ? -1 : failHost<int32_t>(x);
#ifdef SO_NOSIGPIPE
    int one = 1;
    if (!shared) ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    if (!socketFlags(fd, flags)) {
        const int error = errno;
        if (shared) closeNetworkSocket(x.proc, fd); else ::close(fd);
        return linuxFailure(x, hostErrnoLinux(error));
    }
    return shared ? fd : trackAndroidFd(x.proc, fd, true);
}
int32_t a_socketpair(Ctx& x, int family, int type, int protocol, GuestPtr out) {
    if (family != 1) return linuxFailure(x, 97);
    const int32_t r = int32_t(callIos(x, "_socketpair", {1, uint32_t(type & ~kSockFlags), uint32_t(protocol), out}));
    if (r < 0) return r;
    const int first = x.mem.read<int32_t>(out), second = x.mem.read<int32_t>(out + 4);
    if (socketFlags(first, type) && socketFlags(second, type)) return 0;
    const int error = errno;
    closeNetworkSocket(x.proc, first); closeNetworkSocket(x.proc, second);
    return linuxFailure(x, hostErrnoLinux(error));
}
int32_t a_bind(Ctx& x, int fd, GuestPtr address, uint32_t length) {
    if (!owned(x, fd)) return linuxFailure(x, 9);
    Address a;
    if (!loadAddress(x, a, address, length)) return linuxFailure(x, 22);
    if (!a.any && !a.loopback) return linuxFailure(x, 99);
    localBind(a);
    return ::bind(fd, reinterpret_cast<const sockaddr*>(&a.native), a.length) < 0 ? failHost<int32_t>(x) : 0;
}
int32_t a_connect(Ctx& x, int fd, GuestPtr address, uint32_t length) {
    if (!owned(x, fd)) return linuxFailure(x, 9);
    Address a;
    if (!loadAddress(x, a, address, length)) return linuxFailure(x, 22);
    const bool shared = isNetworkSocket(x.proc, fd) && a.family == kIPv4;
    if (!a.loopback && !(shared && x.proc.config().internetAccess)) return linuxFailure(x, 101);
    if (shared) {
        Scratch temp(x);
        if (!temp.addr) return linuxFailure(x, 12);
        x.mem.copyIn(temp.addr, a.bsd, a.length);
        return int32_t(callIos(x, "_connect", {uint32_t(fd), temp.addr, a.length}));
    }
    const int mode = ::fcntl(fd, F_GETFL);
    if (mode < 0) return failHost<int32_t>(x);
    const bool blocking = !(mode & O_NONBLOCK);
    if (blocking && ::fcntl(fd, F_SETFL, mode | O_NONBLOCK) < 0) return failHost<int32_t>(x);
    const int result = ::connect(fd, reinterpret_cast<const sockaddr*>(&a.native), a.length);
    int error = result < 0 ? errno : 0;
    if (blocking && error == EINPROGRESS) {
        if (!waitIo(x, fd, 4)) {
            ::fcntl(fd, F_SETFL, mode);
            return -1;
        }
        socklen_t size = sizeof error;
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0) error = errno;
    }
    if (blocking) ::fcntl(fd, F_SETFL, mode);
    return error ? linuxFailure(x, hostErrnoLinux(error)) : 0;
}

int32_t a_listen(Ctx& x, int fd, int backlog) {
    if (!owned(x, fd)) return linuxFailure(x, 9);
    // listen() can implicitly bind a new socket to INADDR_ANY. Permit only
    // descriptors already explicitly bound to a loopback address.
    sockaddr_storage addr{};
    socklen_t size = sizeof addr;
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &size) < 0) return failHost<int32_t>(x);
    if (addr.ss_family == AF_INET && (ntohl(reinterpret_cast<sockaddr_in*>(&addr)->sin_addr.s_addr) >> 24) != 127)
        return linuxFailure(x, 99);
    if (addr.ss_family == AF_INET6 && !IN6_IS_ADDR_LOOPBACK(&reinterpret_cast<sockaddr_in6*>(&addr)->sin6_addr))
        return linuxFailure(x, 99);
    return ::listen(fd, backlog) < 0 ? failHost<int32_t>(x) : 0;
}
int32_t a_accept4(Ctx& x, int fd, GuestPtr out, GuestPtr len, int flags) {
    if (!owned(x, fd)) return linuxFailure(x, 9);
    if (flags & ~kSockFlags) return linuxFailure(x, 22);
    if (!outputAddress(x, out, len)) return linuxFailure(x, 14);
    const int mode = ::fcntl(fd, F_GETFL);
    if (mode < 0) return failHost<int32_t>(x);
    if (!(mode & O_NONBLOCK) && !waitIo(x, fd, 1)) return -1;
    sockaddr_storage address{}; socklen_t size = sizeof address;
    const int client = ::accept(fd, reinterpret_cast<sockaddr*>(&address), &size);
    if (client < 0) return failHost<int32_t>(x);
    if (!socketFlags(client, flags)) { const int e = errno; ::close(client); return linuxFailure(x, hostErrnoLinux(e)); }
#ifdef SO_NOSIGPIPE
    int one = 1; ::setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    if (out) x.mem.write<uint32_t>(len, storeAddress(x, out, x.mem.read<uint32_t>(len), reinterpret_cast<sockaddr*>(&address)));
    return trackAndroidFd(x.proc, client, true);
}
int32_t a_accept(Ctx& x, int fd, GuestPtr out, GuestPtr len) { return a_accept4(x, fd, out, len, 0); }
int32_t a_shutdown(Ctx& x, int fd, int how) {
    if (!owned(x, fd)) return linuxFailure(x, 9);
    return ::shutdown(fd, how) < 0 ? failHost<int32_t>(x) : 0;
}
template<bool Peer>
int32_t a_sockName(Ctx& x, int fd, GuestPtr out, GuestPtr len) {
    if (!owned(x, fd)) return linuxFailure(x, 9);
    if (!out || !outputAddress(x, out, len)) return linuxFailure(x, 14);
    sockaddr_storage address{}; socklen_t size = sizeof address;
    if ((Peer ? ::getpeername : ::getsockname)(fd, reinterpret_cast<sockaddr*>(&address), &size) < 0)
        return failHost<int32_t>(x);
    x.mem.write<uint32_t>(len, storeAddress(x, out, x.mem.read<uint32_t>(len), reinterpret_cast<sockaddr*>(&address)));
    return 0;
}

int msgFlags(uint32_t flags, bool receive) {
    constexpr uint32_t allowed = 1 | 2 | 4 | 0x20 | 0x40 | 0x80 | 0x100 | 0x4000 | 0x8000;
    if (flags & ~allowed) return -1;
    int host = 0;
    if (flags & 1) host |= MSG_OOB;
    if (receive && (flags & 2)) host |= MSG_PEEK;
    if (!receive && (flags & 4)) host |= MSG_DONTROUTE;
    if (receive && (flags & 0x20)) host |= MSG_TRUNC;
    if (receive && (flags & 0x100)) host |= MSG_WAITALL;
    if (flags & 0x40) host |= MSG_DONTWAIT;
    if (!receive && (flags & 0x80)) host |= MSG_EOR;
#ifdef MSG_NOSIGNAL
    if (!receive) host |= MSG_NOSIGNAL;
#endif
#ifdef MSG_MORE
    if (!receive && (flags & 0x8000)) host |= MSG_MORE;
#endif
    return host;
}
int32_t a_sendto(Ctx& x, int fd, GuestPtr buf, uint32_t count, uint32_t flags, GuestPtr address, uint32_t length) {
    if (!owned(x, fd)) return linuxFailure(x, 9);
    const int host = msgFlags(flags, false);
    if (host < 0) return linuxFailure(x, 95);
    if (count > INT32_MAX || !guestSpan(x, buf, count, Memory::kProtRead)) return linuxFailure(x, 14);
    const bool existing = isNetworkSocket(x.proc, fd);
    if (!address && existing && !(flags & ~(1u | 4u | 0x40u | 0x4000u))) {
        const auto bsd = (flags & (1 | 4)) | ((flags & 0x40) ? 128u : 0u);
        return int32_t(callIos(x, "_send", {uint32_t(fd), buf, count, bsd}));
    }
    Address a;
    if (address) {
        if (!loadAddress(x, a, address, length)) return linuxFailure(x, 22);
        const bool online = x.proc.config().internetAccess && existing && a.family == kIPv4;
        if (!a.loopback && !online) return linuxFailure(x, 101);
        if (existing && a.family == kIPv4 && !flags) {
            Scratch temp(x);
            if (!temp.addr) return linuxFailure(x, 12);
            x.mem.copyIn(temp.addr, a.bsd, a.length);
            return int32_t(callIos(x, "_sendto", {uint32_t(fd), buf, count, 0, temp.addr, a.length}));
        }
    }
    for (;;) {
        const auto result = ::sendto(fd, x.mem.host(buf), count, host | MSG_DONTWAIT,
                                     address ? reinterpret_cast<const sockaddr*>(&a.native) : nullptr,
                                     address ? a.length : 0);
        if (result >= 0) return int32_t(result);
        if (errno != EAGAIN || (flags & 0x40) || (::fcntl(fd, F_GETFL) & O_NONBLOCK)) return failHost<int32_t>(x);
        if (!waitIo(x, fd, 4)) return -1;
    }
}
int32_t a_send(Ctx& x, int fd, GuestPtr buf, uint32_t count, uint32_t flags) {
    return a_sendto(x, fd, buf, count, flags, 0, 0);
}
int32_t a_recvfrom(Ctx& x, int fd, GuestPtr buf, uint32_t count, uint32_t flags, GuestPtr out, GuestPtr len) {
    if (!owned(x, fd)) return linuxFailure(x, 9);
    const int host = msgFlags(flags, true);
    if (host < 0) return linuxFailure(x, 95);
    if (count > INT32_MAX || !guestSpan(x, buf, count, Memory::kProtWrite) || !outputAddress(x, out, len))
        return linuxFailure(x, 14);
    if (!out && isNetworkSocket(x.proc, fd) && !(flags & ~(1u | 2u | 0x40u | 0x100u))) {
        const auto bsd = (flags & 3) | ((flags & 0x40) ? 128u : 0u) | ((flags & 0x100) ? 64u : 0u);
        return int32_t(callIos(x, "_recv", {uint32_t(fd), buf, count, bsd}));
    }
    const bool nonblock = (flags & 0x40) || (::fcntl(fd, F_GETFL) & O_NONBLOCK);
    uint32_t done = 0;
    for (;;) {
        sockaddr_storage address{}; socklen_t size = sizeof address;
        const auto result = ::recvfrom(fd, x.mem.host(buf + done), count - done, host | MSG_DONTWAIT,
                                       reinterpret_cast<sockaddr*>(&address), &size);
        if (result >= 0) {
            if (out) x.mem.write<uint32_t>(len, storeAddress(x, out, x.mem.read<uint32_t>(len), reinterpret_cast<sockaddr*>(&address)));
            done += uint32_t(result);
            if (!result || done >= count || out || nonblock || !(flags & 0x100) || (flags & 2)) return int32_t(done);
        } else if (errno != EAGAIN || nonblock) return done ? int32_t(done) : failHost<int32_t>(x);
        if (!waitIo(x, fd, 1)) return done ? int32_t(done) : -1;
    }
}
int32_t a_recv(Ctx& x, int fd, GuestPtr buf, uint32_t count, uint32_t flags) {
    return a_recvfrom(x, fd, buf, count, flags, 0, 0);
}

int socketOption(int& level, int name) {
    if (level == 1) {
        level = SOL_SOCKET;
        switch (name) {
            case 2: return SO_REUSEADDR; case 3: return SO_TYPE; case 4: return SO_ERROR;
            case 5: return SO_DONTROUTE; case 6: return SO_BROADCAST; case 7: return SO_SNDBUF;
            case 8: return SO_RCVBUF; case 9: return SO_KEEPALIVE; case 10: return SO_OOBINLINE;
            case 13: return SO_LINGER; case 15: return SO_REUSEPORT;
            case 18: return SO_RCVLOWAT; case 19: return SO_SNDLOWAT;
            case 20: return SO_RCVTIMEO; case 21: return SO_SNDTIMEO; case 30: return SO_ACCEPTCONN;
            default: return -1;
        }
    }
    if (level == 0) {
        level = IPPROTO_IP;
        if (name == 1) return IP_TOS;
        if (name == 2) return IP_TTL;
    } else if (level == 6) {
        level = IPPROTO_TCP;
        if (name == 1) return TCP_NODELAY;
    } else if (level == 41) {
        level = IPPROTO_IPV6;
        if (name == 26) return IPV6_V6ONLY;
    }
    return -1;
}
int32_t a_getsockopt(Ctx& x, int fd, int level, int option, GuestPtr out, GuestPtr len) {
    if (!owned(x, fd)) return linuxFailure(x, 9);
    if (!guestSpan(x, len, 4, Memory::kProtRW)) return linuxFailure(x, 14);
    const int name = socketOption(level, option);
    if (name < 0) return linuxFailure(x, 92);
    const uint32_t cap = x.mem.read<uint32_t>(len);
    uint8_t bytes[8]{};
    uint32_t result = 4;
    if (level == SOL_SOCKET && (name == SO_RCVTIMEO || name == SO_SNDTIMEO)) {
        timeval value{}; socklen_t size = sizeof value;
        if (::getsockopt(fd, level, name, &value, &size) < 0) return failHost<int32_t>(x);
        const int32_t pair[2]{int32_t(value.tv_sec), int32_t(value.tv_usec)};
        std::memcpy(bytes, pair, sizeof pair); result = 8;
    } else if (level == SOL_SOCKET && name == SO_LINGER) {
        linger value{}; socklen_t size = sizeof value;
        if (::getsockopt(fd, level, name, &value, &size) < 0) return failHost<int32_t>(x);
        std::memcpy(bytes, &value, 8); result = 8;
    } else {
        int value = 0; socklen_t size = sizeof value;
        if (::getsockopt(fd, level, name, &value, &size) < 0) return failHost<int32_t>(x);
        if (level == SOL_SOCKET && name == SO_ERROR) value = hostErrnoLinux(value);
        std::memcpy(bytes, &value, 4);
    }
    const uint32_t copied = std::min(cap, result);
    if (!guestSpan(x, out, copied, Memory::kProtWrite)) return linuxFailure(x, 14);
    x.mem.copyIn(out, bytes, copied);
    x.mem.write<uint32_t>(len, copied);
    return 0;
}
int32_t a_setsockopt(Ctx& x, int fd, int level, int option, GuestPtr in, uint32_t len) {
    if (!owned(x, fd)) return linuxFailure(x, 9);
    const int name = socketOption(level, option);
    if (name < 0) return linuxFailure(x, 92);
    const bool time = level == SOL_SOCKET && (name == SO_RCVTIMEO || name == SO_SNDTIMEO);
    if (len < ((time || (level == SOL_SOCKET && name == SO_LINGER)) ? 8u : 4u)) return linuxFailure(x, 22);
    if (!guestSpan(x, in, (time || (level == SOL_SOCKET && name == SO_LINGER)) ? 8u : 4u, Memory::kProtRead)) return linuxFailure(x, 14);
    int result;
    if (time) {
        timeval value{x.mem.read<int32_t>(in), x.mem.read<int32_t>(in + 4)};
        if (value.tv_sec < 0 || value.tv_usec < 0 || value.tv_usec >= 1000000) return linuxFailure(x, 22);
        result = ::setsockopt(fd, level, name, &value, sizeof value);
    } else if (level == SOL_SOCKET && name == SO_LINGER) {
        linger value{x.mem.read<int32_t>(in), x.mem.read<int32_t>(in + 4)};
        result = ::setsockopt(fd, level, name, &value, sizeof value);
    } else {
        const int value = x.mem.read<int32_t>(in);
        result = ::setsockopt(fd, level, name, &value, sizeof value);
    }
    return result < 0 ? failHost<int32_t>(x) : 0;
}

int bionicEai(int error) {
    switch (error) {
        case 0: return 0; case EAI_AGAIN: return 2; case EAI_BADFLAGS: return 3;
        case EAI_FAIL: return 4; case EAI_FAMILY: return 5; case EAI_MEMORY: return 6;
        case EAI_NONAME: return 8; case EAI_SERVICE: return 9; case EAI_SOCKTYPE: return 10;
        case EAI_SYSTEM: return 11; case EAI_OVERFLOW: return 14;
        default: return 8;
    }
}
void a_freeaddrinfo(Ctx& x, GuestPtr result) {
    if (result) x.proc.heap().free(result);
}
int32_t a_getaddrinfo(Ctx& x, GuestPtr node, GuestPtr service, GuestPtr hints, GuestPtr result) {
    if (!guestSpan(x, result, 4, Memory::kProtWrite)) { setLinuxErrno(x, 14); return 11; }
    x.mem.write<uint32_t>(result, 0);
    addrinfo opts{};
    int linuxFlags = 0, family = 0;
    if (hints) {
        if (!guestSpan(x, hints, 32, Memory::kProtRead)) return 3;
        linuxFlags = x.mem.read<int32_t>(hints);
        family = x.mem.read<int32_t>(hints + 4);
        opts.ai_socktype = x.mem.read<int32_t>(hints + 8);
        opts.ai_protocol = x.mem.read<int32_t>(hints + 12);
    }
    if (family && family != kIPv4 && family != kIPv6) return 5;
    // bionic netdb.h (AI_NUMERICSERV is 8, unlike the Darwin bridge).
    if (linuxFlags & ~0xF0F) return 3;
    if (opts.ai_socktype && opts.ai_socktype != SOCK_STREAM && opts.ai_socktype != SOCK_DGRAM) return 10;
    std::string name = node ? x.proc.readCString(node, 256) : "";
    const std::string serviceName = service ? x.proc.readCString(service, 64) : "";
    if (!node && !service) return 8;
    if (linuxFlags & 8 && service && serviceName.find_first_not_of("0123456789") != std::string::npos) return 8;
    const bool numeric = (linuxFlags & 4) != 0;
    const auto& aliases = x.proc.config().loopbackAliases;
    const bool alias = std::find(aliases.begin(), aliases.end(), name) != aliases.end();
    const bool localName = name == "localhost" || alias;
    if (numeric && localName) return 8;

    // Reuse the shared IPv4 resolver online so HTTPS keeps the DNS name for
    // the TLS bridge. Numeric and local IPv6 resolution never queries DNS.
    if (node && family != kIPv6 && !localName && x.proc.config().internetAccess &&
        name.find(':') == std::string::npos) {
        Scratch tmp(x);
        if (!tmp.addr) return 6;
        const uint32_t darwinFlags = (linuxFlags & 7) | ((linuxFlags & 8) ? 0x1000 : 0);
        x.mem.write<uint32_t>(tmp.addr, darwinFlags);
        x.mem.write<uint32_t>(tmp.addr + 4, family);
        x.mem.write<uint32_t>(tmp.addr + 8, opts.ai_socktype);
        x.mem.write<uint32_t>(tmp.addr + 12, opts.ai_protocol);
        x.mem.write<uint32_t>(tmp.addr + 32, 0);
        const int r = int32_t(callIos(x, "_getaddrinfo", {node, service, tmp.addr, tmp.addr + 32}));
        if (r) return r;   // Darwin and bionic EAI numbers used by the IPv4 resolver agree
        const uint32_t head = x.mem.read<uint32_t>(tmp.addr + 32);
        uint32_t count = 0;
        for (uint32_t next = head; next; next = x.mem.read<uint32_t>(next + 28)) {
            ++count;
        }
        const uint32_t memory = x.proc.heap().allocZeroed(count * 64 + 256);
        if (!memory) { callIos(x, "_freeaddrinfo", {head}); return 6; }
        uint32_t at = memory;
        for (uint32_t next = head; next; next = x.mem.read<uint32_t>(next + 28), at += 64) {
            uint32_t src = x.mem.read<uint32_t>(next + 24);
            for (uint32_t field = 0; field < 20; field += 4)
                x.mem.write<uint32_t>(at + field, x.mem.read<uint32_t>(next + field));
            x.mem.write<uint32_t>(at, linuxFlags);
            x.mem.copyIn(at + 32, x.mem.host(src), 16);
            x.mem.write<uint16_t>(at + 32, kIPv4);
            x.mem.write<uint32_t>(at + 24, at + 32);
            if (uint32_t canonical = x.mem.read<uint32_t>(next + 20); canonical && at == memory) {
                const auto text = x.proc.readCString(canonical, 255);
                x.mem.copyIn(memory + count * 64, text.c_str(), text.size() + 1);
                x.mem.write<uint32_t>(at + 20, memory + count * 64);
            }
            x.mem.write<uint32_t>(at + 28, x.mem.read<uint32_t>(next + 28) ? at + 64 : 0);
        }
        callIos(x, "_freeaddrinfo", {head});
        x.mem.write<uint32_t>(result, memory);
        return 0;
    }

    addrinfo* found = nullptr;
    opts.ai_family = family == kIPv4 || alias ? AF_INET : family == kIPv6 ? AF_INET6 : AF_UNSPEC;
    if (linuxFlags & 1) opts.ai_flags |= AI_PASSIVE;
    if (linuxFlags & 2) opts.ai_flags |= AI_CANONNAME;
    if (linuxFlags & 8) opts.ai_flags |= AI_NUMERICSERV;
    if (linuxFlags & 0x100) opts.ai_flags |= AI_ALL;
    if (linuxFlags & 0x400) opts.ai_flags |= AI_ADDRCONFIG;
    if (linuxFlags & 0x800) opts.ai_flags |= AI_V4MAPPED;
    opts.ai_flags |= AI_NUMERICHOST;
    if (localName) name = family == kIPv6 ? "::1" : "127.0.0.1";
    const int err = ::getaddrinfo(node ? name.c_str() : nullptr, service ? serviceName.c_str() : nullptr, &opts, &found);
    if (err) {
        if (err == EAI_SYSTEM) setLinuxErrno(x, hostErrnoLinux(errno));
        return bionicEai(err);
    }
    uint32_t count = 0;
    for (auto* a = found; a; a = a->ai_next)
        if (a->ai_family == AF_INET || a->ai_family == AF_INET6) ++count;
    if (!count) { ::freeaddrinfo(found); return 8; }
    const uint32_t memory = x.proc.heap().allocZeroed(count * 64 + 256);
    if (!memory) { ::freeaddrinfo(found); return 6; }
    uint32_t at = memory;
    for (auto* a = found; a; a = a->ai_next) {
        if (a->ai_family != AF_INET && a->ai_family != AF_INET6) continue;
        const auto length = storeAddress(x, at + 32, 28, a->ai_addr);
        x.mem.write<uint32_t>(at, linuxFlags);
        x.mem.write<uint32_t>(at + 4, a->ai_family == AF_INET ? kIPv4 : kIPv6);
        x.mem.write<uint32_t>(at + 8, a->ai_socktype);
        x.mem.write<uint32_t>(at + 12, a->ai_protocol);
        x.mem.write<uint32_t>(at + 16, length);
        x.mem.write<uint32_t>(at + 24, at + 32);     // bionic places ai_canonname first
        if (at == memory && linuxFlags & 2) {
            const auto canonical = localName ? x.proc.readCString(node, 255) :
                                   a->ai_canonname ? std::string(a->ai_canonname).substr(0, 255) : name;
            x.mem.copyIn(memory + count * 64, canonical.c_str(), canonical.size() + 1);
            x.mem.write<uint32_t>(at + 20, memory + count * 64);
        }
        x.mem.write<uint32_t>(at + 28, at + 64 < memory + count * 64 ? at + 64 : 0);
        at += 64;
    }
    ::freeaddrinfo(found);
    x.mem.write<uint32_t>(result, memory);
    return 0;
}
int32_t a_getnameinfo(Ctx& x, GuestPtr addr, uint32_t length, GuestPtr node, uint32_t nodesize,
                      GuestPtr service, uint32_t servicesize, int flags) {
    Address address;
    if (!loadAddress(x, address, addr, length)) return 5;
    if (!guestSpan(x, node, nodesize, Memory::kProtWrite) ||
        !guestSpan(x, service, servicesize, Memory::kProtWrite)) return 11;
    if (flags & ~31) return 3;
    if ((flags & 4) && !(flags & 2) && !address.loopback) return 8;
    int hostFlags = NI_NUMERICHOST | NI_NUMERICSERV;
    if (flags & 16) hostFlags |= NI_DGRAM;
    return bionicEai(::getnameinfo(reinterpret_cast<const sockaddr*>(&address.native), address.length,
                                  reinterpret_cast<char*>(x.mem.hostOrNull(node)), nodesize,
                                  reinterpret_cast<char*>(x.mem.hostOrNull(service)), servicesize, hostFlags));
}
int32_t a_inetPton(Ctx& x, int family, GuestPtr name, GuestPtr out) {
    return int32_t(callIos(x, "_inet_pton", {uint32_t(family == kIPv6 ? 30 : family), name, out}));
}
uint32_t a_inetNtop(Ctx& x, int family, GuestPtr address, GuestPtr out, uint32_t length) {
    return uint32_t(callIos(x, "_inet_ntop", {uint32_t(family == kIPv6 ? 30 : family), address, out, length}));
}
}  // namespace

void registerAndroidNet(Process& p) {
#define REG(fn, name, description) supportDef<&fn>(p, "android:" name, SupportStatus::Real, description)
    REG(a_socket, "socket", "Linux flags; shared IPv4 policy and local sockets");
    REG(a_socketpair, "socketpair", "local AF_UNIX sockets");
    REG(a_bind, "bind", "bind only on loopback");
    REG(a_connect, "connect", "Linux socket addresses; per-game IPv4 network policy");
    REG(a_listen, "listen", "listeners bound to local addresses");
    REG(a_accept, "accept", "accept a loopback connection");
    REG(a_accept4, "accept4", "accept with Linux descriptor flags");
    REG(a_shutdown, "shutdown", "host socket shutdown");
    REG(a_sockName<false>, "getsockname", "Linux address layout");
    REG(a_sockName<true>, "getpeername", "Linux address layout");
    REG(a_send, "send", "Linux message flags");
    REG(a_sendto, "sendto", "Linux message flags, addresses and per-game network policy");
    REG(a_recv, "recv", "Linux message flags");
    REG(a_recvfrom, "recvfrom", "Linux message flags and addresses");
    REG(a_getsockopt, "getsockopt", "Linux level, option and errno values");
    REG(a_setsockopt, "setsockopt", "Linux level, option and timeval layout");
    REG(a_getaddrinfo, "getaddrinfo", "Linux addrinfo with local addressing and shared opt-in IPv4 DNS");
    REG(a_freeaddrinfo, "freeaddrinfo", "free the Linux addrinfo allocation");
    supportDef<&a_getnameinfo>(p, "android:getnameinfo", SupportStatus::Approx, "numeric host and service names");
    REG(a_inetPton, "inet_pton", "Linux IPv4 and IPv6 families");
    REG(a_inetNtop, "inet_ntop", "Linux IPv4 and IPv6 families");
#undef REG
}
} // namespace Hydrogen::Runtime::Android
