// HydrogenVM — BSD sockets, name resolution and network interfaces.
//
// By default HydrogenVM runs apps offline (reachability reports no network), so
// the socket layer behaves like a device with networking down: sockets can't
// be created, names don't resolve, and the only interface is lo0. Address
// conversion helpers (inet_*) are real, and select()/poll() work on the file
// descriptors that do exist (files, pipes) and as sleeps.
// An explicit ProcessConfig capability permits outbound IPv4 TCP on loopback.
// No external DNS, external connects or listeners are enabled by it.
//
// IPv4 UDP is always available, but only inside the device: a phone with no
// network still has lo0, and games run their local multiplayer server over it
// (Minecraft PE's RakNet). Sockets are bound to 127.0.0.1 whatever address the
// app asks for, broadcasts go to 127.0.0.1, and any other destination fails
// with ENETUNREACH, so nothing can leave the machine.
#include "runtime/Process.hpp"
#include "Network.hpp"
#include "DarwinErrno.hpp"
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <algorithm>
#include <atomic>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace Hydrogen::Runtime {
namespace {

void setErrno(Ctx& x, int e) { x.mem.write<uint32_t>(x.proc.currentThread().errnoAddr, uint32_t(e)); }
constexpr int kEBADF = 9, kEINVAL = 22, kENETDOWN = 50, kEAFNOSUPPORT = 47, kENOSPC = 28, kEMFILE = 24;
constexpr int kAF_UNIX = 1, kAF_INET = 2, kAF_INET6 = 30, kAF_LINK = 18;

int32_t failWith(Ctx& x, int e) { setErrno(x, e); return -1; }

struct AddrInfoState {
    explicit AddrInfoState(Process&) {}
    std::mutex mu;
    std::map<uint32_t, std::vector<uint32_t>> allocations;
};
// Explicit local TCP capability; never resolves or connects to external hosts.
struct SocketState {
    explicit SocketState(Process& p) { p.onExit([this] { clear(); }); }
    std::mutex mu;
    std::set<int> fds;
    std::set<int> udp;   // subset of fds: loopback-only datagram sockets
    void clear() { std::lock_guard lock(mu); for (int fd : fds) { ::shutdown(fd, SHUT_RDWR); ::close(fd); } fds.clear(); udp.clear(); }
    ~SocketState() { clear(); }
};
bool owns(Ctx& x, int fd) { return isNetworkSocket(x.proc, fd); }
int32_t pollHost(Ctx&, std::vector<pollfd>&, int32_t);
bool waitSocket(Ctx& x, int fd, short events) {
    std::vector<pollfd> fds{{fd, events, 0}};
    int r = pollHost(x, fds, -1);
    if (r < 0) return false;
    if (r == 0) { setErrno(x, 4); return false; } // interrupted by process stop
    if (fds[0].revents & POLLNVAL) { setErrno(x, kEBADF); return false; }
    return true;
}
int32_t hostFailure(Ctx& x) { return failWith(x, darwinErrno(errno)); }
bool span(Ctx& x, GuestPtr ptr, uint32_t n, uint8_t prot) {
    if (!n) return true;
    uint64_t end = uint64_t(ptr) + n;
    if (!ptr || end > (uint64_t(1) << 32)) return false;
    for (uint64_t a = ptr; a < end; a = (a & ~uint64_t(4095)) + 4096)
        if ((x.mem.protAt(uint32_t(a)) & prot) != prot) return false;
    return true;
}
bool isUdp(Ctx& x, int fd) { auto& s = x.proc.service<SocketState>(); std::lock_guard lock(s.mu); return s.udp.count(fd) != 0; }
int32_t n_socket(Ctx& x, int32_t family, int32_t type, int32_t protocol) {
    if (family == kAF_INET && type == SOCK_DGRAM && (protocol == 0 || protocol == IPPROTO_UDP)) {
        int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd < 0) return hostFailure(x);
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        auto& state = x.proc.service<SocketState>();
        std::lock_guard lock(state.mu); state.fds.insert(fd); state.udp.insert(fd);
        return fd;
    }
    if (!x.proc.config().loopbackNetwork) return failWith(x, kENETDOWN);
    if (family != kAF_INET) return failWith(x, kEAFNOSUPPORT);
    if (type != SOCK_STREAM || (protocol != 0 && protocol != IPPROTO_TCP)) return failWith(x, 43);
    int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return hostFailure(x);
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int one = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one) < 0) { int e = errno; ::close(fd); errno = e; return hostFailure(x); }
#endif
    auto& state = x.proc.service<SocketState>();
    std::lock_guard lock(state.mu); state.fds.insert(fd);
    return fd;
}
// Darwin sockaddr_in: len, family, port (network order), address.
bool readSockaddr(Ctx& x, GuestPtr addr, uint32_t len, sockaddr_in& out) {
    if (!addr || len < 8 || !span(x, addr, 8, Memory::kProtRead)) return false;
    out = sockaddr_in{}; out.sin_family = AF_INET;
#ifdef __APPLE__
    out.sin_len = sizeof out;
#endif
    x.mem.copyOut(&out.sin_port, addr + 2, 2); x.mem.copyOut(&out.sin_addr, addr + 4, 4);
    return x.mem.read<uint8_t>(addr + 1) == kAF_INET;
}
void writeSockaddr(Ctx& x, GuestPtr addr, GuestPtr lenp, const sockaddr_in& in) {
    if (!addr || !lenp || !span(x, lenp, 4, Memory::kProtRW)) return;
    uint8_t d[16] = {16, uint8_t(kAF_INET)};
    std::memcpy(d + 2, &in.sin_port, 2); std::memcpy(d + 4, &in.sin_addr, 4);
    const uint32_t cap = x.mem.read<uint32_t>(lenp);
    if (span(x, addr, std::min<uint32_t>(cap, 16), Memory::kProtWrite)) x.mem.copyIn(addr, d, std::min<uint32_t>(cap, 16));
    x.mem.write<uint32_t>(lenp, 16);
}
// Where a datagram may go: loopback as is; broadcasts and "this host" (0.0.0.0)
// to 127.0.0.1. Anything else would leave the device.
bool udpDestination(Ctx& x, sockaddr_in& a) {
    const uint32_t ip = ntohl(a.sin_addr.s_addr);
    if ((ip >> 24) == 127) return true;
    if (ip == 0 || ip == 0xFFFFFFFFu || (ip & 0xFF) == 0xFF) { a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); return true; }
    static std::atomic<bool> told{false};
    if (!told.exchange(true)) {
        char b[INET_ADDRSTRLEN] = "";
        inet_ntop(AF_INET, &a.sin_addr, b, sizeof b);
        x.proc.logf(LogLevel::Info, "UDP to %s refused: only the device's own loopback is reachable offline", b);
    }
    return false;
}
int32_t n_bind(Ctx& x, int32_t fd, GuestPtr addr, uint32_t len) {
    if (!owns(x, fd)) return failWith(x, kEBADF);
    if (!isUdp(x, fd)) return failWith(x, 45);
    sockaddr_in a{};
    if (!readSockaddr(x, addr, len, a)) return failWith(x, kEINVAL);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // never listen beyond the device
    return ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0 ? hostFailure(x) : 0;
}
int32_t n_getsockname(Ctx& x, int32_t fd, GuestPtr addr, GuestPtr lenp) {
    if (!owns(x, fd)) return failWith(x, kEBADF);
    sockaddr_in a{}; socklen_t n = sizeof a;
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &n) < 0) return hostFailure(x);
    writeSockaddr(x, addr, lenp, a);
    return 0;
}
int32_t n_getpeername(Ctx& x, int32_t fd, GuestPtr addr, GuestPtr lenp) {
    if (!owns(x, fd)) return failWith(x, kEBADF);
    sockaddr_in a{}; socklen_t n = sizeof a;
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&a), &n) < 0) return hostFailure(x);
    writeSockaddr(x, addr, lenp, a);
    return 0;
}
int32_t n_send(Ctx& x, int32_t fd, GuestPtr buf, uint32_t n, uint32_t flags);
int32_t n_sendto(Ctx& x, int32_t fd, GuestPtr buf, uint32_t n, uint32_t flags, GuestPtr addr, uint32_t alen) {
    if (!owns(x, fd)) return failWith(x, kEBADF);
    if (!addr) return n_send(x, fd, buf, n, flags);
    if (!isUdp(x, fd)) return failWith(x, 56);   // EISCONN: a TCP socket ignores the address
    sockaddr_in a{};
    if (!readSockaddr(x, addr, alen, a)) return failWith(x, kEINVAL);
    if (!udpDestination(x, a)) return failWith(x, 51);
    if (n > 65507 || !span(x, buf, n, Memory::kProtRead)) return failWith(x, n > 65507 ? 40 : 14);   // EMSGSIZE / EFAULT
    int hostFlags = 0;
#ifdef MSG_NOSIGNAL
    hostFlags |= MSG_NOSIGNAL;
#endif
    const ssize_t out = ::sendto(fd, x.mem.host(buf), n, hostFlags, reinterpret_cast<sockaddr*>(&a), sizeof a);
    return out < 0 ? hostFailure(x) : int32_t(out);
}
int32_t n_recvfrom(Ctx& x, int32_t fd, GuestPtr buf, uint32_t n, uint32_t flags, GuestPtr addr, GuestPtr lenp) {
    if (!owns(x, fd)) return failWith(x, kEBADF);
    if ((flags & ~(2u | 128u)) != 0) return failWith(x, 45);   // MSG_PEEK, MSG_DONTWAIT
    if (n > INT32_MAX || !span(x, buf, n, Memory::kProtWrite)) return failWith(x, 14);
    const bool nonblocking = (flags & 128) || (::fcntl(fd, F_GETFL) & O_NONBLOCK);
    const int hostFlags = (flags & 2) ? MSG_PEEK : 0;
    for (;;) {
        sockaddr_in a{}; socklen_t alen = sizeof a;
        const ssize_t got = ::recvfrom(fd, x.mem.host(buf), n, hostFlags | MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&a), &alen);
        if (got >= 0) { writeSockaddr(x, addr, lenp, a); return int32_t(got); }
        if (errno != EAGAIN || nonblocking) return hostFailure(x);
        if (!waitSocket(x, fd, POLLIN)) return -1;
    }
}
int32_t n_connect(Ctx& x, int32_t fd, GuestPtr addr, uint32_t len) {
    if (!owns(x, fd)) return failWith(x, kEBADF);
    if (len < 16 || !span(x, addr, 16, Memory::kProtRead)) return failWith(x, kEINVAL);
    if (x.mem.read<uint8_t>(addr + 1) != kAF_INET) return failWith(x, kEAFNOSUPPORT);
    sockaddr_in host{}; host.sin_family = AF_INET;
#ifdef __APPLE__
    host.sin_len = sizeof host;
#endif
    x.mem.copyOut(&host.sin_port, addr + 2, 2); x.mem.copyOut(&host.sin_addr, addr + 4, 4);
    // Preserve main's UDP destination policy separately from the local TCP
    // transport and its explicit port overrides.
    if (isUdp(x, fd)) {
        if (!udpDestination(x, host)) return failWith(x, 51);
        return ::connect(fd, reinterpret_cast<sockaddr*>(&host), sizeof host) < 0 ? hostFailure(x) : 0;
    }
    char ipBuf[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &host.sin_addr, ipBuf, sizeof(ipBuf));
    uint16_t origPort = ntohs(host.sin_port);
    x.proc.logf(LogLevel::Info, "guest connect fd=%d to %s:%d", fd, ipBuf, origPort);
    if ((ntohl(host.sin_addr.s_addr) >> 24) != 127) {
        x.proc.logf(LogLevel::Warning, "connect rejected: non-loopback %s:%d", ipBuf, origPort);
        return failWith(x, 51);
    }
    if(host.sin_addr.s_addr==htonl(INADDR_LOOPBACK)) {
        auto mapped=x.proc.config().loopbackPortOverrides.find(ntohs(host.sin_port));
        if(mapped!=x.proc.config().loopbackPortOverrides.end()) {
            x.proc.logf(LogLevel::Info, "port override %d -> %d", origPort, mapped->second);
            host.sin_port=htons(mapped->second);
        }
    }
    int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0) return hostFailure(x);
    bool blocking = !(flags & O_NONBLOCK);
    if (blocking && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return hostFailure(x);
    int r = ::connect(fd, reinterpret_cast<sockaddr*>(&host), sizeof host), saved = errno;
    if (blocking && r < 0 && saved == EINPROGRESS) {
        if (!waitSocket(x, fd, POLLOUT)) { ::fcntl(fd, F_SETFL, flags); return -1; }
        socklen_t size = sizeof saved;
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &saved, &size) < 0) saved = errno;
        r = saved ? -1 : 0;
    }
    if (blocking) ::fcntl(fd, F_SETFL, flags);
    x.proc.logf(LogLevel::Info, "guest connect fd=%d finished r=%d saved_errno=%d", fd, r, saved);
    errno = saved;
    return r < 0 ? hostFailure(x) : 0;
}
int hostMsgFlags(uint32_t flags, bool receive) {
    uint32_t allowed = receive ? (1u | 2u | 64u | 128u) : (1u | 4u | 128u | 0x80000u);
    if (flags & ~allowed) return -1;
    int out = flags & 1 ? MSG_OOB : 0;
    if (receive && (flags & 2)) out |= MSG_PEEK;
    if (receive && (flags & 64)) out |= MSG_WAITALL;
    if (!receive && (flags & 4)) out |= MSG_DONTROUTE;
    if (flags & 128) out |= MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
    if (!receive) out |= MSG_NOSIGNAL;
#endif
    return out;
}
int32_t n_send(Ctx& x, int32_t fd, GuestPtr buf, uint32_t n, uint32_t flags) {
    if (!owns(x, fd)) return failWith(x, kEBADF);
    x.proc.logf(LogLevel::Info, "guest send fd=%d n=%u flags=%u", fd, n, flags);
    int hostFlags = hostMsgFlags(flags, false);
    if (hostFlags < 0) return failWith(x, 45);
    if (n > INT32_MAX || !span(x, buf, n, Memory::kProtRead)) return failWith(x, 14);
    bool nonblocking = (flags & 128) || (::fcntl(fd, F_GETFL) & O_NONBLOCK);
    for (;;) {
        ssize_t out = ::send(fd, x.mem.host(buf), n, hostFlags | MSG_DONTWAIT);
        if (out >= 0) return int32_t(out);
        if (errno != EAGAIN || nonblocking) return hostFailure(x);
        if (!waitSocket(x, fd, POLLOUT)) return -1;
    }
}
int32_t n_recv(Ctx& x, int32_t fd, GuestPtr buf, uint32_t n, uint32_t flags) {
    if (!owns(x, fd)) return failWith(x, kEBADF);
    int hostFlags = hostMsgFlags(flags, true);
    if (hostFlags < 0) return failWith(x, 45);
    if (n > INT32_MAX || !span(x, buf, n, Memory::kProtWrite)) return failWith(x, 14);
    if ((flags & 66) == 66) return failWith(x, 45); // WAITALL + PEEK not implemented
    bool nonblocking = (flags & 128) || (::fcntl(fd, F_GETFL) & O_NONBLOCK);
    uint32_t received = 0;
    for (;;) {
        ssize_t out = ::recv(fd, x.mem.host(buf + received), n - received, hostFlags | MSG_DONTWAIT);
        if (out >= 0) {
            received += uint32_t(out);
            if (!out || received == n || nonblocking || !(flags & 64)) return int32_t(received);
        } else if (errno != EAGAIN || nonblocking) return received ? int32_t(received) : hostFailure(x);
        if (!waitSocket(x, fd, POLLIN)) return received ? int32_t(received) : -1;
    }
}
int32_t n_shutdown(Ctx& x, int32_t fd, int32_t how) {
    if (!owns(x, fd)) return failWith(x, kEBADF);
    if (how < 0 || how > 2) return failWith(x, kEINVAL);
    return ::shutdown(fd, how) < 0 ? hostFailure(x) : 0;
}
int32_t n_setsockopt(Ctx& x, int32_t fd, int32_t level, int32_t opt, GuestPtr value, uint32_t len) {
    if (!owns(x, fd)) return failWith(x, kEBADF);
    if (len != 4 || !span(x, value, 4, Memory::kProtRead)) return failWith(x, kEINVAL);
    int v = x.mem.read<int32_t>(value), hostLevel = SOL_SOCKET, hostOpt;
    if (level == 0 && isUdp(x, fd)) return 0;   // IPPROTO_IP (TTL, TOS, multicast): nothing to change on loopback
    if (level == 6 && opt == 1) { hostLevel = IPPROTO_TCP; hostOpt = TCP_NODELAY; }
    else if (level == 0xffff && opt == 0x20) hostOpt = SO_BROADCAST;
    else if (level == 0xffff && opt == 0x200) {
#ifdef SO_REUSEPORT
        hostOpt = SO_REUSEPORT;
#else
        return 0;
#endif
    }
    else if (level != 0xffff) return failWith(x, 42);
    else if (opt == 4) hostOpt = SO_REUSEADDR;
    else if (opt == 8) hostOpt = SO_KEEPALIVE;
    else if (opt == 0x1001) hostOpt = SO_SNDBUF;
    else if (opt == 0x1002) hostOpt = SO_RCVBUF;
    else if (opt == 0x1022 && v == 1) {
        // macOS sets SO_NOSIGPIPE on creation; Linux applies MSG_NOSIGNAL to
        // every send (including write through System.cpp) for owned sockets.
        return 0;
    } else return failWith(x, 42);
    return ::setsockopt(fd, hostLevel, hostOpt, &v, sizeof v) < 0 ? hostFailure(x) : 0;
}
int32_t n_getsockopt(Ctx& x, int32_t fd, int32_t level, int32_t opt, GuestPtr value, GuestPtr len) {
    if (!owns(x, fd)) return failWith(x, kEBADF);
    if (!span(x, len, 4, Memory::kProtRW) || x.mem.read<uint32_t>(len) < 4 || !span(x, value, 4, Memory::kProtWrite)) return failWith(x, kEINVAL);
    int v = 0, hostOpt;
    if (level != 0xffff) return failWith(x, 42);
    if (opt == 0x1007) hostOpt = SO_ERROR;
    else if (opt == 0x1008) hostOpt = SO_TYPE;
    else if (opt == 0x1001) hostOpt = SO_SNDBUF;
    else if (opt == 0x1002) hostOpt = SO_RCVBUF;
    else return failWith(x, 42);
    socklen_t size = sizeof v;
    if (::getsockopt(fd, SOL_SOCKET, hostOpt, &v, &size) < 0) return hostFailure(x);
    if (hostOpt == SO_ERROR) v = darwinErrno(v);
    x.mem.write<int32_t>(value, v); x.mem.write<uint32_t>(len, 4); return 0;
}
// Darwin addrinfo layout differs from the host (including sockaddr length byte).
int32_t n_getaddrinfo(Ctx& x, GuestPtr node, GuestPtr service, GuestPtr hints, GuestPtr res) {
    if (!span(x, res, 4, Memory::kProtWrite)) return 8;
    x.mem.write<uint32_t>(res, 0);
    if (!x.proc.config().loopbackNetwork) return 8;
    std::string name = node ? x.proc.readCString(node, 256) : "";
    x.proc.logf(LogLevel::Info, "getaddrinfo node=%s", name.c_str());
    in_addr ip{};
    bool alias = std::find(x.proc.config().loopbackAliases.begin(), x.proc.config().loopbackAliases.end(), name) != x.proc.config().loopbackAliases.end();
    if (name == "localhost" || alias) ip.s_addr = htonl(INADDR_LOOPBACK);
    else if (inet_pton(AF_INET, name.c_str(), &ip) != 1 || (ntohl(ip.s_addr) >> 24) != 127) {
        x.proc.logf(LogLevel::Info, "Loopback resolver refused host: %s", name.c_str()); return 8;
    }
    uint32_t flags = 0;
    if (hints) {
        if (!span(x, hints, 32, Memory::kProtRead)) return 3;
        flags = x.mem.read<uint32_t>(hints);
        int family = x.mem.read<int32_t>(hints + 4), type = x.mem.read<int32_t>(hints + 8), protocol = x.mem.read<int32_t>(hints + 12);
        if (family && family != kAF_INET) return 5;
        if ((type && type != SOCK_STREAM) || (protocol && protocol != IPPROTO_TCP)) return 10;
        if (flags & ~uint32_t(2 | 4 | 0x1000)) return 3; // CANONNAME / NUMERICHOST / NUMERICSERV
        if ((flags & 4) && (name == "localhost" || alias)) return 8;
    }
    uint32_t port = 0;
    if (service) {
        std::string s = x.proc.readCString(service, 16);
        if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos) return 9;
        for (char c : s) { port = port * 10 + unsigned(c - '0'); if (port > 65535) return 9; }
    }
    auto& heap = x.proc.heap();
    uint32_t result = heap.allocZeroed(32), addr = heap.allocZeroed(16);
    x.mem.write<uint32_t>(result + 4, kAF_INET); x.mem.write<uint32_t>(result + 8, SOCK_STREAM);
    x.mem.write<uint32_t>(result + 12, IPPROTO_TCP); x.mem.write<uint32_t>(result + 16, 16); x.mem.write<uint32_t>(result + 24, addr);
    if (flags & 2) x.mem.write<uint32_t>(result + 20, x.proc.allocCString(name));
    x.mem.write<uint8_t>(addr, 16); x.mem.write<uint8_t>(addr + 1, kAF_INET);
    x.mem.write<uint16_t>(addr + 2, htons(uint16_t(port))); x.mem.write<uint32_t>(addr + 4, ip.s_addr);
    auto& state = x.proc.service<AddrInfoState>();
    { std::lock_guard lock(state.mu); state.allocations[result] = {addr, x.mem.read<uint32_t>(result + 20), result}; }
    x.mem.write<uint32_t>(res, result); return 0;
}
void n_freeaddrinfo(Ctx& x, GuestPtr addr) {
    // Only allocations produced by this implementation are accepted below.
    if (!addr) return;
    auto& state = x.proc.service<AddrInfoState>();
    std::lock_guard lock(state.mu);
    auto it = state.allocations.find(addr);
    if (it == state.allocations.end()) return;
    for (uint32_t ptr : it->second) if (ptr) x.proc.heap().free(ptr);
    state.allocations.erase(it);
}
int32_t n_unsupportedSocket(Ctx& x, int32_t fd) { return failWith(x, owns(x, fd) ? 45 : kEBADF); }
int32_t n_socketpair(Ctx& x, int32_t domain, int32_t type, int32_t protocol, GuestPtr sv) {
    if (domain != kAF_UNIX) return failWith(x, kEAFNOSUPPORT);
    int cleanType = type & ~0x80000000u;
    if (cleanType != SOCK_STREAM && cleanType != SOCK_DGRAM) return failWith(x, 43);
    if (!span(x, sv, 8, Memory::kProtWrite)) return failWith(x, 14);
    int fds[2]{-1, -1};
    if (::socketpair(AF_UNIX, cleanType, protocol, fds) < 0) return hostFailure(x);
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int one = 1;
    ::setsockopt(fds[0], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    ::setsockopt(fds[1], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    auto& state = x.proc.service<SocketState>();
    {
        std::lock_guard lock(state.mu);
        state.fds.insert(fds[0]);
        state.fds.insert(fds[1]);
    }
    x.mem.write<int32_t>(sv, fds[0]);
    x.mem.write<int32_t>(sv + 4, fds[1]);
    x.proc.logf(LogLevel::Info, "guest socketpair -> %d, %d", fds[0], fds[1]);
    return 0;
}
int32_t n_zero(Ctx&) { return 0; }
uint32_t n_gaiStrerror(Ctx& x, int32_t e) {
    static const char sKey = 0; uint32_t& s = x.proc.processSlot(&sKey);
    if (!s) s = x.proc.allocCString("nodename nor servname provided, or not known");
    (void)e;
    return s;
}
uint32_t n_nullHost(Ctx&, GuestPtr) { return 0; }
uint32_t n_nullHost3(Ctx&, GuestPtr, uint32_t, int32_t) { return 0; }
// Interfaces a phone has: lo0 and en0 (Wi-Fi). Apps read en0's MAC address
// through sysctl(NET_RT_IFLIST) (System.cpp) and fail when en0 is missing.
uint32_t n_ifNameToIndex(Ctx& x, GuestPtr name) {
    if (!name) return 0;
    const std::string n = x.proc.readCString(name);
    return n == "lo0" ? 1 : n == "en0" ? 2 : 0;
}

// ---- inet_*
uint32_t n_inetAddr(Ctx& x, GuestPtr s) {
    in_addr a{};
    return inet_aton(x.proc.readCString(s).c_str(), &a) ? a.s_addr : 0xFFFFFFFFu;
}
int32_t n_inetAton(Ctx& x, GuestPtr s, GuestPtr out) {
    in_addr a{};
    if (!inet_aton(x.proc.readCString(s).c_str(), &a)) return 0;
    if (out) x.mem.write<uint32_t>(out, a.s_addr);
    return 1;
}
uint32_t n_inetNtoa(Ctx& x, uint32_t addr) {
    static const char bufKey = 0; uint32_t& buf = x.proc.threadSlot(&bufKey);
    if (!buf) buf = x.proc.heap().alloc(16);
    in_addr a{};
    a.s_addr = addr;
    const char* s = inet_ntoa(a);
    x.mem.copyIn(buf, s, std::strlen(s) + 1);
    return buf;
}
int32_t n_inetPton(Ctx& x, int32_t af, GuestPtr src, GuestPtr dst) {
    const std::string s = x.proc.readCString(src);
    if (af == kAF_INET) { in_addr a{}; if (inet_pton(AF_INET, s.c_str(), &a) != 1) return 0; x.mem.copyIn(dst, &a, 4); return 1; }
    if (af == kAF_INET6) { in6_addr a{}; if (inet_pton(AF_INET6, s.c_str(), &a) != 1) return 0; x.mem.copyIn(dst, &a, 16); return 1; }
    return failWith(x, kEAFNOSUPPORT);
}
uint32_t n_inetNtop(Ctx& x, int32_t af, GuestPtr src, GuestPtr dst, uint32_t size) {
    char buf[64];
    const char* r = nullptr;
    if (af == kAF_INET) r = inet_ntop(AF_INET, x.mem.host(src), buf, sizeof buf);
    else if (af == kAF_INET6) r = inet_ntop(AF_INET6, x.mem.host(src), buf, sizeof buf);
    else { failWith(x, kEAFNOSUPPORT); return 0; }
    if (!r || std::strlen(buf) + 1 > size) { failWith(x, kENOSPC); return 0; }
    x.mem.copyIn(dst, buf, std::strlen(buf) + 1);
    return dst;
}

// ---- getifaddrs: lo0 only (AF_LINK + AF_INET 127.0.0.1).
// struct ifaddrs { next, name, flags, addr, netmask, dstaddr, data } (28 bytes).
int32_t n_getifaddrs(Ctx& x, GuestPtr out) {
    auto& h = x.proc.heap();
    const uint32_t name = h.alloc(4);
    x.mem.copyIn(name, "lo0", 4);
    auto sockIn = [&](uint32_t ip) {
        const uint32_t a = h.alloc(16);
        for (uint32_t i = 0; i < 16; i += 4) x.mem.write<uint32_t>(a + i, 0);
        x.mem.write<uint8_t>(a, 16);
        x.mem.write<uint8_t>(a + 1, kAF_INET);
        x.mem.write<uint32_t>(a + 4, htonl(ip));
        return a;
    };
    // sockaddr_dl { len, family, index(2), type, nlen, alen, slen, data[12] }
    const uint32_t dl = h.alloc(20);
    for (uint32_t i = 0; i < 20; i += 4) x.mem.write<uint32_t>(dl + i, 0);
    x.mem.write<uint8_t>(dl, 20);
    x.mem.write<uint8_t>(dl + 1, kAF_LINK);
    x.mem.write<uint16_t>(dl + 2, 1);
    x.mem.write<uint8_t>(dl + 4, 24);   // IFT_LOOP
    x.mem.write<uint8_t>(dl + 5, 3);
    x.mem.copyIn(dl + 8, "lo0", 3);
    const uint32_t flags = 0x1 | 0x8 | 0x40 | 0x8000;   // UP | LOOPBACK | RUNNING | MULTICAST
    auto node = [&](uint32_t next, uint32_t addr, uint32_t mask) {
        const uint32_t n = h.alloc(28);
        const uint32_t v[7] = {next, name, flags, addr, mask, 0, 0};
        for (uint32_t i = 0; i < 7; ++i) x.mem.write<uint32_t>(n + i * 4, v[i]);
        return n;
    };
    const uint32_t inet = node(0, sockIn(0x7F000001), sockIn(0xFF000000));
    x.mem.write<uint32_t>(out, node(inet, dl, 0));
    return 0;
}
void n_freeifaddrs(Ctx& x, GuestPtr p) {
    auto& h = x.proc.heap();
    bool first = true;
    while (p) {
        const uint32_t next = x.mem.read<uint32_t>(p);
        if (first) h.free(x.mem.read<uint32_t>(p + 4));   // the shared name
        first = false;
        for (uint32_t off : {12u, 16u}) if (uint32_t a = x.mem.read<uint32_t>(p + off)) h.free(a);
        h.free(p);
        p = next;
    }
}

// ---- select / poll over host fds (files, pipes); with nothing to watch they sleep.
void sleepFor(Ctx& x, int64_t us) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
    while (!x.proc.stopping() && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(end - std::chrono::steady_clock::now(), std::chrono::milliseconds(50)));
}
int32_t pollHost(Ctx& x, std::vector<pollfd>& fds, int32_t timeoutMs) {
    if (fds.empty()) {
        if (timeoutMs < 0) { while (!x.proc.stopping()) std::this_thread::sleep_for(std::chrono::milliseconds(50)); return 0; }
        sleepFor(x, int64_t(timeoutMs) * 1000);
        return 0;
    }
    // Poll in slices so a stopping app isn't stuck here.
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs < 0 ? 0 : timeoutMs);
    for (;;) {
        int slice = 50;
        if (timeoutMs >= 0) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count();
            slice = int(std::max<int64_t>(0, std::min<int64_t>(left, 50)));
        }
        const int r = ::poll(fds.data(), fds.size(), slice);
        if (r != 0) return r < 0 ? failWith(x, errno == EBADF ? kEBADF : kEINVAL) : r;
        if (x.proc.stopping() || (timeoutMs >= 0 && std::chrono::steady_clock::now() >= end)) return 0;
    }
}
constexpr short kPollMask = POLLIN | POLLPRI | POLLOUT | POLLERR | POLLHUP | POLLNVAL;
int32_t n_poll(Ctx& x, GuestPtr fds, uint32_t n, int32_t timeout) {
    std::vector<pollfd> h(n);
    for (uint32_t i = 0; i < n; ++i) {
        h[i].fd = int32_t(x.mem.read<uint32_t>(fds + i * 8));
        const uint16_t ev = x.mem.read<uint16_t>(fds + i * 8 + 4);
        h[i].events = short((ev & kPollMask) | ((ev & 0x40) ? POLLIN : 0));   // POLLRDNORM
        h[i].revents = 0;
    }
    const int32_t r = pollHost(x, h, timeout);
    for (uint32_t i = 0; i < n; ++i) x.mem.write<uint16_t>(fds + i * 8 + 6, uint16_t(h[i].revents & kPollMask));
    return r;
}
// select(nfds, readfds, writefds, errorfds, timeval*): Darwin fd_set is 1024 bits in 32-bit words.
int32_t n_select(Ctx& x, int32_t nfds, GuestPtr rs, GuestPtr ws, GuestPtr es, GuestPtr tv) {
    int32_t timeoutMs = -1;
    if (tv) timeoutMs = int32_t(x.mem.read<uint32_t>(tv)) * 1000 + int32_t(x.mem.read<uint32_t>(tv + 4)) / 1000;
    auto isSet = [&](GuestPtr set, int fd) { return set && (x.mem.read<uint32_t>(set + uint32_t(fd / 32) * 4) >> (fd % 32) & 1); };
    std::vector<pollfd> h;
    std::vector<int> which;
    nfds = std::min(nfds, 1024);
    for (int fd = 0; fd < nfds; ++fd) {
        short ev = 0;
        if (isSet(rs, fd)) ev |= POLLIN;
        if (isSet(ws, fd)) ev |= POLLOUT;
        if (isSet(es, fd)) ev |= POLLPRI;
        if (ev) h.push_back(pollfd{fd, ev, 0});
    }
    const int32_t r = pollHost(x, h, timeoutMs);
    if (r < 0) return r;
    for (auto& fd : h) if (fd.revents & POLLNVAL) return failWith(x, kEBADF);
    auto clear = [&](GuestPtr set) { if (set) for (int w = 0; w < (nfds + 31) / 32; ++w) x.mem.write<uint32_t>(set + uint32_t(w) * 4, 0); };
    clear(rs); clear(ws); clear(es);
    int32_t count = 0;
    auto mark = [&](GuestPtr set, int fd) {
        if (!set) return;
        const uint32_t a = set + uint32_t(fd / 32) * 4;
        x.mem.write<uint32_t>(a, x.mem.read<uint32_t>(a) | (1u << (fd % 32)));
        ++count;
    };
    for (auto& p : h) {
        if (p.revents & (POLLIN | POLLHUP | POLLERR)) mark(rs, p.fd);
        if (p.revents & POLLOUT) mark(ws, p.fd);
        if (p.revents & POLLPRI) mark(es, p.fd);
    }
    return count;
}

// ---- pipe / dup2 (host fds pass through)
int32_t n_pipe(Ctx& x, GuestPtr fds) {
    int f[2];
    if (::pipe(f) != 0) return failWith(x, kEMFILE);   // (no pipe2 on iOS)
    ::fcntl(f[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(f[1], F_SETFD, FD_CLOEXEC);
    x.mem.write<uint32_t>(fds, uint32_t(f[0]));
    x.mem.write<uint32_t>(fds + 4, uint32_t(f[1]));
    return 0;
}
int32_t n_dup2(Ctx& x, int32_t from, int32_t to) {
    if (to <= 2) return to;   // stdout/stderr feed the app log; keep them
    const int r = ::dup2(from, to);
    if (r >= 0) duplicateNetworkSocket(x.proc, from, to);
    return r < 0 ? failWith(x, kEBADF) : r;
}

// ---- execinfo
int32_t n_backtrace(Ctx&, GuestPtr, int32_t) { return 0; }
uint32_t n_backtraceSymbols(Ctx& x, GuestPtr, int32_t) { return x.proc.heap().alloc(4); }

} // namespace

bool isNetworkSocket(Process& p, int fd) {
    auto& s = p.service<SocketState>(); std::lock_guard lock(s.mu); return s.fds.count(fd) != 0;
}
int networkRead(Ctx& x, int fd, unsigned int buffer, unsigned int count) { return n_recv(x, fd, buffer, count, 0); }
int networkWrite(Ctx& x, int fd, unsigned int buffer, unsigned int count) { return n_send(x, fd, buffer, count, 0); }
int closeNetworkSocket(Process& p, int fd) {
    p.logf(LogLevel::Info, "guest closeNetworkSocket fd=%d", fd);
    auto& s = p.service<SocketState>(); std::lock_guard lock(s.mu);
    if (!s.fds.erase(fd)) return -2;
    s.udp.erase(fd);
    return ::close(fd);
}
void duplicateNetworkSocket(Process& p, int from, int to) {
    auto& s = p.service<SocketState>(); std::lock_guard lock(s.mu);
    bool owned = s.fds.count(from) != 0, udp = s.udp.count(from) != 0;
    s.fds.erase(to); s.udp.erase(to);
    if (owned) s.fds.insert(to);
    if (udp) s.udp.insert(to);
}

void registerLibcNet(Process& p) {
    p.def<&n_socket>("_socket");
    p.def<&n_socketpair>("_socketpair");
    p.service<SocketState>();
    p.def<&n_connect>("_connect"); p.def<&n_send>("_send"); p.def<&n_recv>("_recv");
    p.def<&n_shutdown>("_shutdown"); p.def<&n_getsockopt>("_getsockopt"); p.def<&n_setsockopt>("_setsockopt");
    p.def<&n_bind>("_bind"); p.def<&n_sendto>("_sendto"); p.def<&n_recvfrom>("_recvfrom");
    p.def<&n_getsockname>("_getsockname"); p.def<&n_getpeername>("_getpeername");
    for (const char* f : {"_listen", "_accept", "_sendmsg", "_recvmsg", "_sendfile"})
        p.def<&n_unsupportedSocket>(f);
    p.def<&n_getaddrinfo>("_getaddrinfo");
    p.def<&n_freeaddrinfo>("_freeaddrinfo");
    p.def<&n_gaiStrerror>("_gai_strerror");
    p.def<&n_nullHost>("_gethostbyname");
    p.def<&n_nullHost3>("_gethostbyaddr");
    p.def<&n_ifNameToIndex>("_if_nametoindex");
    p.def<&n_inetAddr>("_inet_addr");
    p.def<&n_inetAton>("_inet_aton");
    p.def<&n_inetNtoa>("_inet_ntoa");
    p.def<&n_inetPton>("_inet_pton");
    p.def<&n_inetNtop>("_inet_ntop");
    p.def<&n_getifaddrs>("_getifaddrs");
    p.def<&n_freeifaddrs>("_freeifaddrs");
    p.def<&n_poll>("_poll");
    p.def<&n_select>("_select");
    p.def<&n_select>("_select$DARWIN_EXTSN");
    p.def<&n_pipe>("_pipe");
    p.def<&n_dup2>("_dup2");
    p.def<&n_backtrace>("_backtrace");
    p.def<&n_backtraceSymbols>("_backtrace_symbols");
    (void)n_zero;
}

} // namespace Hydrogen::Runtime
