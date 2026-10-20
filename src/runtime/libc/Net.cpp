// HydrogenVM — BSD sockets, name resolution and network interfaces.
//
// HydrogenVM runs apps offline (SCNetworkReachability reports no network), so
// the socket layer behaves like a device with networking down: sockets can't
// be created, names don't resolve, and the only interface is lo0. Address
// conversion helpers (inet_*) are real, and select()/poll() work on the file
// descriptors that do exist (files, pipes) and as sleeps.
#include "runtime/Process.hpp"
#include <algorithm>
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
constexpr int kAF_INET = 2, kAF_INET6 = 30, kAF_LINK = 18;

int32_t failWith(Ctx& x, int e) { setErrno(x, e); return -1; }

int32_t n_socket(Ctx& x, int32_t, int32_t, int32_t) { return failWith(x, kENETDOWN); }
int32_t n_badf(Ctx& x) { return failWith(x, kEBADF); }
int32_t n_socketpair(Ctx& x, int32_t, int32_t, int32_t, GuestPtr) { return failWith(x, kENETDOWN); }
int32_t n_zero(Ctx&) { return 0; }

// getaddrinfo: EAI_NONAME (8 on Darwin) for everything; gethostbyname: NULL.
int32_t n_getaddrinfo(Ctx& x, GuestPtr, GuestPtr, GuestPtr, GuestPtr res) { if (res) x.mem.write<uint32_t>(res, 0); return 8; }
void n_freeaddrinfo(Ctx&, GuestPtr) {}
uint32_t n_gaiStrerror(Ctx& x, int32_t e) {
    static uint32_t s = 0;
    if (!s) s = x.proc.allocCString("nodename nor servname provided, or not known");
    (void)e;
    return s;
}
uint32_t n_nullHost(Ctx&, GuestPtr) { return 0; }
uint32_t n_nullHost3(Ctx&, GuestPtr, uint32_t, int32_t) { return 0; }
uint32_t n_ifNameToIndex(Ctx& x, GuestPtr name) { return x.proc.readCString(name) == "lo0" ? 1 : 0; }

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
    static thread_local uint32_t buf = 0;
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
    return r < 0 ? failWith(x, kEBADF) : r;
}

// ---- execinfo
int32_t n_backtrace(Ctx&, GuestPtr, int32_t) { return 0; }
uint32_t n_backtraceSymbols(Ctx& x, GuestPtr, int32_t) { return x.proc.heap().alloc(4); }

} // namespace

void registerLibcNet(Process& p) {
    p.def<&n_socket>("_socket");
    p.def<&n_socketpair>("_socketpair");
    for (const char* f : {"_connect", "_bind", "_listen", "_accept", "_send", "_sendto", "_sendmsg", "_recv", "_recvfrom",
                          "_recvmsg", "_shutdown", "_getsockopt", "_setsockopt", "_getpeername", "_getsockname", "_sendfile"})
        p.def<&n_badf>(f);
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
