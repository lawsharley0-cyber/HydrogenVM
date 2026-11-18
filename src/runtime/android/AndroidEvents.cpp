// Linux descriptor APIs with ARM EABI structures at the guest boundary.
#include "runtime/android/BionicInternal.hpp"
#include "runtime/libc/Network.hpp"
#include "runtime/Support.hpp"
#include <algorithm>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unordered_map>
#include <unistd.h>
#include <vector>
#if defined(__linux__)
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#elif defined(__APPLE__)
#include <sys/event.h>
#endif

namespace Hydrogen::Runtime::Android {

bool guestSpan(Ctx& x, GuestPtr addr, uint32_t size, uint8_t prot) {
    if (!size) return true;
    const uint64_t end = uint64_t(addr) + size;
    if (!addr || end > Memory::GuestArena::kArenaSize) return false;
    for (uint64_t page = addr & ~4095u; page < end; page += 4096)
        if ((x.mem.protAt(uint32_t(page)) & prot) != prot) return false;
    return true;
}

namespace {
constexpr int kNonblock = 0x800, kCloexec = 0x80000;
#ifdef __APPLE__
constexpr uint32_t kEdge = 0x80000000u, kOneshot = 0x40000000u;
struct TimerSpec { timespec it_interval, it_value; };
#else
using TimerSpec = itimerspec;
#endif
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

int linuxPollEvents(short events) {
    return ((events & POLLIN) ? 1 : 0) | ((events & POLLPRI) ? 2 : 0) |
           ((events & POLLOUT) ? 4 : 0) | ((events & POLLERR) ? 8 : 0) |
           ((events & POLLHUP) ? 16 : 0) | ((events & POLLNVAL) ? 32 : 0)
#ifdef POLLRDHUP
           | ((events & POLLRDHUP) ? 0x2000 : 0)
#endif
        ;
}
short hostPollEvents(int events) {
    return short(((events & (1 | 0x40)) ? POLLIN : 0) | ((events & 2) ? POLLPRI : 0) |
                 ((events & (4 | 0x100)) ? POLLOUT : 0)
#ifdef POLLRDHUP
                 | ((events & 0x2000) ? POLLRDHUP : 0)
#endif
    );
}
#ifndef __linux__
bool setDescriptorFlags(int fd, int flags) {
    return ::fcntl(fd, F_SETFD, flags & kCloexec ? FD_CLOEXEC : 0) >= 0 &&
           (!(flags & kNonblock) || ::fcntl(fd, F_SETFL, O_NONBLOCK) >= 0);
}
#endif

#ifdef __APPLE__
struct Counter {
    Counter(int peer, bool semaphore, int clock)
        : peer(peer), semaphore(semaphore), clock(clock) {}
    ~Counter() {
        {
            std::lock_guard lock(mu);
            stopping = true;
        }
        cv.notify_all();
        if (worker.joinable()) worker.join();
        ::close(peer);
    }
    // The peer's one-byte notification tracks the counter's zero transition.
    void signal(int fd, uint64_t previous) {
        char byte = 0;
        if (!previous && value) ::send(peer, &byte, 1, MSG_DONTWAIT);
        if (previous && !value) ::recv(fd, &byte, 1, MSG_DONTWAIT);
        cv.notify_all();
    }
    void run() {
        std::unique_lock lock(mu);
        while (!stopping) {
            if (due == Clock::time_point::max()) { cv.wait(lock); continue; }
            if (Clock::now() < due) { cv.wait_until(lock, due); continue; }
            const auto now = Clock::now();
            uint64_t count = period.count() ? 1 + uint64_t((now - due) / period) : 1;
            due = period.count() ? due + period * int64_t(count) : Clock::time_point::max();
            const auto old = value;
            value += std::min(count, UINT64_MAX - 1 - value);
            if (!old && value) {
                char byte = 0;
                ::send(peer, &byte, 1, MSG_DONTWAIT);
            }
            cv.notify_all();
        }
    }
    int peer;
    bool semaphore;
    int clock;  // -1 for eventfd
    uint64_t value = 0;
    Clock::time_point due = Clock::time_point::max();
    std::chrono::nanoseconds period{};
    std::mutex mu;
    std::condition_variable cv;
    bool stopping = false;
    std::thread worker;
};
struct Kqueue {
    struct Watch { uint32_t events; uint64_t data; bool enabled; };
    std::unordered_map<int, Watch> watches;
    std::mutex mu;
};
#endif

struct Entry {
    bool socket = false;
#ifdef __APPLE__
    std::shared_ptr<Counter> counter;
    std::shared_ptr<Kqueue> queue;
#endif
};
struct Descriptors {
    explicit Descriptors(Process&) {}
    ~Descriptors() {
        // Counters own their peers and workers. The table owns the guest ends.
        for (const auto& [fd, entry] : entries) ::close(fd);
    }
    std::mutex mu;
    std::unordered_map<int, Entry> entries;
};
Entry entryFor(Process& p, int fd) {
    auto& state = p.service<Descriptors>();
    std::lock_guard lock(state.mu);
    const auto it = state.entries.find(fd);
    return it == state.entries.end() ? Entry{} : it->second;
}
void duplicate(Process& p, int from, int to) {
    if (from == to) return;
    auto& state = p.service<Descriptors>();
    std::lock_guard lock(state.mu);
    auto it = state.entries.find(from);
    if (it == state.entries.end()) state.entries.erase(to);
    else {
        const auto entry = it->second;
        state.entries[to] = entry;
    }
}

struct Wait {
    Ctx& x;
    Clock::time_point due;
    explicit Wait(Ctx& x, int timeout)
        : x(x), due(timeout < 0 ? Clock::time_point::max() : Clock::now() + std::chrono::milliseconds(timeout)) {}
    int slice() const {
        if (due == Clock::time_point::max()) return 40;
        return int(std::clamp<int64_t>(std::chrono::ceil<std::chrono::milliseconds>(due - Clock::now()).count(), int64_t(0), int64_t(40)));
    }
    bool interrupted() const { return x.proc.stopping() || x.cpu.async_pending.load(std::memory_order_acquire); }
};

int32_t a_close(Ctx& x, int fd) {
    auto& state = x.proc.service<Descriptors>();
    {
        std::lock_guard lock(state.mu);
#ifdef __APPLE__
        for (const auto& [key, entry] : state.entries) {
            if (entry.queue) {
                std::lock_guard queueLock(entry.queue->mu);
                entry.queue->watches.erase(fd);
            }
        }
#endif
        state.entries.erase(fd);
    }
    return int32_t(callIos(x, "_close", {uint32_t(fd)}));
}
int32_t a_dup(Ctx& x, int fd) {
    const auto result = int32_t(callIos(x, "_dup", {uint32_t(fd)}));
    if (result >= 0) duplicate(x.proc, fd, result);
    return result;
}
int32_t a_dup2(Ctx& x, int from, int to) {
    const auto result = int32_t(callIos(x, "_dup2", {uint32_t(from), uint32_t(to)}));
    if (result > 2) duplicate(x.proc, from, to);
    return result;
}
int32_t a_read(Ctx& x, int fd, GuestPtr ptr, uint32_t count) {
    if (!guestSpan(x, ptr, count, Memory::kProtWrite)) return linuxFailure(x, 14);
#ifdef __APPLE__
    if (auto c = entryFor(x.proc, fd).counter) {
        if (count < 8) return linuxFailure(x, 22);
        std::unique_lock lock(c->mu);
        while (!c->value) {
            const int flags = ::fcntl(fd, F_GETFL);
            if (flags < 0) return failHost<int32_t>(x);
            if (flags & O_NONBLOCK) return linuxFailure(x, 11);
            if (x.proc.stopping() || x.cpu.async_pending.load()) return linuxFailure(x, 4);
            c->cv.wait_for(lock, 40ms);
        }
        const uint64_t old = c->value, value = c->semaphore ? 1 : old;
        c->value -= value;
        c->signal(fd, old);
        x.mem.write<uint64_t>(ptr, value);
        return 8;
    }
#endif
    if (isAndroidHostSocket(x.proc, fd)) return int32_t(callIos(x, "android:recv", {uint32_t(fd), ptr, count, 0}));
    return int32_t(callIos(x, "_read", {uint32_t(fd), ptr, count}));
}
int32_t a_write(Ctx& x, int fd, GuestPtr ptr, uint32_t count) {
    if (!guestSpan(x, ptr, count, Memory::kProtRead)) return linuxFailure(x, 14);
#ifdef __APPLE__
    if (auto c = entryFor(x.proc, fd).counter) {
        if (c->clock >= 0 || count < 8) return linuxFailure(x, 22);
        const uint64_t value = x.mem.read<uint64_t>(ptr);
        if (value == UINT64_MAX) return linuxFailure(x, 22);
        std::unique_lock lock(c->mu);
        while (value > UINT64_MAX - 1 - c->value) {
            const int flags = ::fcntl(fd, F_GETFL);
            if (flags < 0) return failHost<int32_t>(x);
            if (flags & O_NONBLOCK) return linuxFailure(x, 11);
            if (x.proc.stopping() || x.cpu.async_pending.load()) return linuxFailure(x, 4);
            c->cv.wait_for(lock, 40ms);
        }
        const uint64_t old = c->value;
        c->value += value;
        c->signal(fd, old);
        return 8;
    }
#endif
    if (isAndroidHostSocket(x.proc, fd)) return int32_t(callIos(x, "android:send", {uint32_t(fd), ptr, count, 0}));
    return int32_t(callIos(x, "_write", {uint32_t(fd), ptr, count}));
}

int32_t a_pipe2(Ctx& x, GuestPtr output, int flags) {
    if (!guestSpan(x, output, 8, Memory::kProtWrite)) return linuxFailure(x, 14);
    if (flags & ~(kNonblock | kCloexec)) return linuxFailure(x, 22);
    int fds[2];
#ifdef __linux__
    const int hostFlags = (flags & kNonblock ? O_NONBLOCK : 0) | (flags & kCloexec ? O_CLOEXEC : 0);
    if (::pipe2(fds, hostFlags) < 0) return failHost<int32_t>(x);
#else
    if (::pipe(fds) < 0) return failHost<int32_t>(x);
    if (!setDescriptorFlags(fds[0], flags) || !setDescriptorFlags(fds[1], flags)) {
        const int error = errno;
        ::close(fds[0]); ::close(fds[1]);
        return linuxFailure(x, hostErrnoLinux(error));
    }
#endif
    x.mem.write<int32_t>(output, trackAndroidFd(x.proc, fds[0]));
    x.mem.write<int32_t>(output + 4, trackAndroidFd(x.proc, fds[1]));
    return 0;
}
int32_t a_pipe(Ctx& x, GuestPtr output) { return a_pipe2(x, output, 0); }

int32_t a_poll(Ctx& x, GuestPtr array, uint32_t count, int timeout) {
    if (count > 65536) return linuxFailure(x, 22);
    if (!guestSpan(x, array, count * 8, Memory::kProtRW)) return linuxFailure(x, 14);
    std::vector<pollfd> fds(count);
    std::vector<int> masks(count);
    for (uint32_t i = 0; i < count; ++i) {
        const auto at = array + i * 8;
        masks[i] = x.mem.read<uint16_t>(at + 4);
        fds[i] = {x.mem.read<int32_t>(at), hostPollEvents(masks[i]), 0};
    }
    Wait wait(x, timeout);
    BlockedScope blocked(x.proc, "Android poll");
    for (;;) {
        if (wait.interrupted()) return linuxFailure(x, 4);
        const int ready = ::poll(fds.data(), fds.size(), wait.slice());
        if (ready < 0) return failHost<int32_t>(x);
        int total = 0;
        for (uint32_t i = 0; i < count; ++i) {
            int events = linuxPollEvents(fds[i].revents);
#ifdef __APPLE__
            if (entryFor(x.proc, fds[i].fd).counter) events = pollDescriptor(x.proc, fds[i].fd, masks[i]);
#endif
            if (events & 1) events |= masks[i] & 0x40;
            if (events & 4) events |= masks[i] & 0x100;
            events &= masks[i] | 8 | 16 | 32;
            x.mem.write<uint16_t>(array + i * 8 + 6, uint16_t(events));
            if (events) ++total;
        }
        if (total || Clock::now() >= wait.due) return total;
    }
}

#ifdef __APPLE__
int makeCounter(Ctx& x, int flags, uint32_t initial, int clock) {
    int pair[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0) return failHost<int32_t>(x);
    const int enabled = 1;
    ::setsockopt(pair[0], SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled);
    ::setsockopt(pair[1], SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled);
    if (!setDescriptorFlags(pair[0], flags) || !setDescriptorFlags(pair[1], kNonblock | kCloexec)) {
        const int error = errno;
        ::close(pair[0]); ::close(pair[1]);
        return linuxFailure(x, hostErrnoLinux(error));
    }
    auto counter = std::make_shared<Counter>(pair[1], flags & 1, clock);
    counter->value = initial;
    counter->signal(pair[0], 0);
    if (clock >= 0) {
        auto* state = counter.get();
        counter->worker = std::thread([state] { state->run(); });
    }
    auto& state = x.proc.service<Descriptors>();
    std::lock_guard lock(state.mu);
    state.entries[pair[0]].counter = std::move(counter);
    return pair[0];
}
#endif

int32_t a_eventfd(Ctx& x, uint32_t initial, int flags) {
    if (flags & ~(1 | kNonblock | kCloexec)) return linuxFailure(x, 22);
#ifdef __linux__
    const int host = (flags & 1 ? EFD_SEMAPHORE : 0) | (flags & kNonblock ? EFD_NONBLOCK : 0) |
                     (flags & kCloexec ? EFD_CLOEXEC : 0);
    const int fd = ::eventfd(initial, host);
    return fd < 0 ? failHost<int32_t>(x) : trackAndroidFd(x.proc, fd);
#else
    return makeCounter(x, flags, initial, -1);
#endif
}
int32_t oldEventfd(Ctx& x, uint32_t initial) { return a_eventfd(x, initial, 0); }

int32_t a_timerCreate(Ctx& x, int clock, int flags) {
    if ((clock != 0 && clock != 1 && clock != 7) || flags & ~(kNonblock | kCloexec)) return linuxFailure(x, 22);
#ifdef __linux__
    const int hostClock = clock == 0 ? CLOCK_REALTIME : clock == 7 ? CLOCK_BOOTTIME : CLOCK_MONOTONIC;
    const int hostFlags = (flags & kNonblock ? TFD_NONBLOCK : 0) | (flags & kCloexec ? TFD_CLOEXEC : 0);
    const int fd = ::timerfd_create(hostClock, hostFlags);
    return fd < 0 ? failHost<int32_t>(x) : trackAndroidFd(x.proc, fd);
#else
    return makeCounter(x, flags, 0, clock);
#endif
}
bool loadTime(Ctx& x, GuestPtr at, timespec& value) {
    value = {x.mem.read<int32_t>(at), x.mem.read<int32_t>(at + 4)};
    return value.tv_sec >= 0 && value.tv_nsec >= 0 && value.tv_nsec < 1000000000;
}
void storeTime(Ctx& x, GuestPtr at, const timespec& value) {
    x.mem.write<int32_t>(at, int32_t(value.tv_sec));
    x.mem.write<int32_t>(at + 4, int32_t(value.tv_nsec));
}
#ifdef __APPLE__
timespec timespecOf(std::chrono::nanoseconds span) {
    const auto sec = std::chrono::duration_cast<std::chrono::seconds>(span);
    return {time_t(sec.count()), long((span - sec).count())};
}
std::chrono::nanoseconds spanOf(const timespec& ts) {
    return std::chrono::seconds(ts.tv_sec) + std::chrono::nanoseconds(ts.tv_nsec);
}
TimerSpec counterTimes(const Counter& c) {
    const auto remaining = c.due == Clock::time_point::max() ? 0ns :
                           std::max(0ns, std::chrono::duration_cast<std::chrono::nanoseconds>(c.due - Clock::now()));
    return {timespecOf(c.period), timespecOf(remaining)};
}
#endif

int32_t a_timerSet(Ctx& x, int fd, int flags, GuestPtr input, GuestPtr output) {
    if (!guestSpan(x, input, 16, Memory::kProtRead) || (output && !guestSpan(x, output, 16, Memory::kProtWrite)))
        return linuxFailure(x, 14);
    TimerSpec old{}, spec{};
    if (!loadTime(x, input, spec.it_interval) || !loadTime(x, input + 8, spec.it_value) || flags & ~3)
        return linuxFailure(x, 22);
#ifdef __linux__
    const int host = (flags & 1 ? TFD_TIMER_ABSTIME : 0) | (flags & 2 ? TFD_TIMER_CANCEL_ON_SET : 0);
    if (::timerfd_settime(fd, host, &spec, &old) < 0) return failHost<int32_t>(x);
#else
    auto c = entryFor(x.proc, fd).counter;
    if (!c || c->clock < 0) return linuxFailure(x, ::fcntl(fd, F_GETFD) < 0 ? 9 : 22);
    if (flags & 2) return linuxFailure(x, 95);  // cancel on wall-clock adjustment needs a host notification
    std::lock_guard lock(c->mu);
    old = counterTimes(*c);
    const uint64_t previous = c->value;
    c->value = 0;
    c->period = spanOf(spec.it_interval);
    const auto value = spanOf(spec.it_value);
    c->due = Clock::time_point::max();
    if (value.count()) {
        auto remaining = value;
        if (flags & 1) {
            timespec now{};
            ::clock_gettime(c->clock == 0 ? CLOCK_REALTIME : CLOCK_MONOTONIC, &now);
            remaining = value - spanOf(now);
        }
        c->due = Clock::now() + remaining;
    }
    c->signal(fd, previous);
#endif
    if (output) { storeTime(x, output, old.it_interval); storeTime(x, output + 8, old.it_value); }
    return 0;
}
int32_t a_timerGet(Ctx& x, int fd, GuestPtr output) {
    if (!guestSpan(x, output, 16, Memory::kProtWrite)) return linuxFailure(x, 14);
    TimerSpec spec{};
#ifdef __linux__
    if (::timerfd_gettime(fd, &spec) < 0) return failHost<int32_t>(x);
#else
    auto c = entryFor(x.proc, fd).counter;
    if (!c || c->clock < 0) return linuxFailure(x, ::fcntl(fd, F_GETFD) < 0 ? 9 : 22);
    std::lock_guard lock(c->mu);
    spec = counterTimes(*c);
#endif
    storeTime(x, output, spec.it_interval);
    storeTime(x, output + 8, spec.it_value);
    return 0;
}

int32_t a_epollCreate(Ctx& x, int flags) {
    if (flags & ~kCloexec) return linuxFailure(x, 22);
#ifdef __linux__
    const int fd = ::epoll_create1(flags & kCloexec ? EPOLL_CLOEXEC : 0);
    return fd < 0 ? failHost<int32_t>(x) : trackAndroidFd(x.proc, fd);
#else
    const int fd = ::kqueue();
    if (fd < 0) return failHost<int32_t>(x);
    if (!setDescriptorFlags(fd, flags)) {
        const int error = errno; ::close(fd); return linuxFailure(x, hostErrnoLinux(error));
    }
    auto& state = x.proc.service<Descriptors>();
    std::lock_guard lock(state.mu);
    state.entries[fd].queue = std::make_shared<Kqueue>();
    return fd;
#endif
}
int32_t oldEpollCreate(Ctx& x, int size) { return size > 0 ? a_epollCreate(x, 0) : linuxFailure(x, 22); }

// ARM EABI aligns the epoll_data u64 at 8 (the x86_64 Linux struct is packed).
constexpr uint32_t kEventSize = 16;
int32_t a_epollCtl(Ctx& x, int ep, int op, int fd, GuestPtr input) {
    if (op < 1 || op > 3) return linuxFailure(x, 22);
    if (op != 2 && !guestSpan(x, input, kEventSize, Memory::kProtRead)) return linuxFailure(x, 14);
    const uint32_t events = op == 2 ? 0 : x.mem.read<uint32_t>(input);
    const uint64_t data = op == 2 ? 0 : x.mem.read<uint64_t>(input + 8);
#ifdef __linux__
    epoll_event event{}; event.events = events; event.data.u64 = data;
    return ::epoll_ctl(ep, op, fd, op == 2 ? nullptr : &event) < 0 ? failHost<int32_t>(x) : 0;
#else
    const auto queue = entryFor(x.proc, ep).queue;
    if (!queue || fd == ep) return linuxFailure(x, ::fcntl(ep, F_GETFD) < 0 ? 9 : 22);
    if (::fcntl(fd, F_GETFD) < 0) return failHost<int32_t>(x);
    std::lock_guard lock(queue->mu);
    auto it = queue->watches.find(fd);
    if (op == 1 && it != queue->watches.end()) return linuxFailure(x, 17);
    if (op != 1 && it == queue->watches.end()) return linuxFailure(x, 2);
    std::vector<struct kevent> changes;
    const uint32_t previous = it == queue->watches.end() ? 0 : it->second.events;
    for (const auto& [filter, mask] : {std::pair{EVFILT_READ, 1u}, std::pair{EVFILT_WRITE, 4u}}) {
        struct kevent e;
        if (events & mask) EV_SET(&e, fd, filter, EV_ADD | EV_ENABLE | (events & kEdge ? EV_CLEAR : 0), 0, 0, nullptr);
        else if (previous & mask) EV_SET(&e, fd, filter, EV_DELETE, 0, 0, nullptr);
        else continue;
        changes.push_back(e);
    }
    if (!changes.empty() && ::kevent(ep, changes.data(), int(changes.size()), nullptr, 0, nullptr) < 0)
        return failHost<int32_t>(x);
    if (op == 2) queue->watches.erase(it);
    else queue->watches[fd] = {events, data, true};
    return 0;
#endif
}
int32_t a_epollWait(Ctx& x, int fd, GuestPtr output, int maximum, int timeout) {
    if (maximum <= 0 || maximum > INT_MAX / int(kEventSize)) return linuxFailure(x, 22);
    if (!guestSpan(x, output, uint32_t(maximum) * kEventSize, Memory::kProtWrite)) return linuxFailure(x, 14);
    const int capacity = std::min(maximum, 1024);
    auto put = [&](int index, uint32_t events, uint64_t data) {
        const auto at = output + uint32_t(index) * kEventSize;
        x.mem.write<uint32_t>(at, events);
        x.mem.write<uint64_t>(at + 8, data);
    };
    Wait wait(x, timeout);
    BlockedScope blocked(x.proc, "Android epoll_wait");
#ifdef __linux__
    std::vector<epoll_event> events(capacity);
    for (;;) {
        if (wait.interrupted()) return linuxFailure(x, 4);
        const int count = ::epoll_wait(fd, events.data(), capacity, wait.slice());
        if (count < 0) return failHost<int32_t>(x);
        for (int i = 0; i < count; ++i) put(i, events[i].events, events[i].data.u64);
        if (count || Clock::now() >= wait.due) return count;
    }
#else
    const auto queue = entryFor(x.proc, fd).queue;
    if (!queue) return linuxFailure(x, ::fcntl(fd, F_GETFD) < 0 ? 9 : 22);
    std::vector<struct kevent> events(capacity);
    for (;;) {
        if (wait.interrupted()) return linuxFailure(x, 4);
        timespec timeoutSpec{0, wait.slice() * 1000000L};
        const int count = ::kevent(fd, nullptr, 0, events.data(), capacity, &timeoutSpec);
        if (count < 0) return failHost<int32_t>(x);
        std::unordered_map<int, uint32_t> ready;
        for (int i = 0; i < count; ++i) {
            const auto& event = events[i];
            auto& mask = ready[int(event.ident)];
            mask |= event.filter == EVFILT_READ ? 1u : 4u;
            if (event.flags & EV_EOF) mask |= 16;
            if (event.flags & EV_ERROR) mask |= 8;
        }
        int total = 0;
        {
            std::lock_guard lock(queue->mu);
            for (const auto& [key, mask] : ready) {
                auto it = queue->watches.find(key);
                if (it == queue->watches.end() || !it->second.enabled) continue;
                auto& watch = it->second;
                put(total++, mask, watch.data);
                if (watch.events & kOneshot) {
                    watch.enabled = false;
                    for (const auto& [filter, bit] : {std::pair{EVFILT_READ, 1u}, std::pair{EVFILT_WRITE, 4u}}) {
                        if (!(watch.events & bit)) continue;
                        struct kevent change;
                        EV_SET(&change, key, filter, EV_DISABLE, 0, 0, nullptr);
                        ::kevent(fd, &change, 1, nullptr, 0, nullptr);
                    }
                }
            }
        }
        if (total || Clock::now() >= wait.due) return total;
    }
#endif
}
int32_t a_epollPwait(Ctx& x, int fd, GuestPtr output, int maximum, int timeout, GuestPtr sigmask) {
    if (sigmask) return linuxFailure(x, 95);
    return a_epollWait(x, fd, output, maximum, timeout);
}
} // namespace

int trackAndroidFd(Process& p, int fd, bool socket) {
    if (fd >= 0) {
        auto& state = p.service<Descriptors>();
        std::lock_guard lock(state.mu);
        state.entries[fd].socket = socket;
    }
    return fd;
}
bool isAndroidHostSocket(Process& p, int fd) { return entryFor(p, fd).socket; }
int pollDescriptor(Process& p, int fd, int events) {
#ifdef __APPLE__
    if (auto counter = entryFor(p, fd).counter) {
        std::lock_guard lock(counter->mu);
        return ((events & (1 | 0x40)) && counter->value ? 1 : 0) |
               ((events & (4 | 0x100)) && counter->clock < 0 && counter->value < UINT64_MAX - 1 ? 4 : 0);
    }
#else
    (void)p;
#endif
    pollfd descriptor{fd, hostPollEvents(events), 0};
    return ::poll(&descriptor, 1, 0) > 0 ? linuxPollEvents(descriptor.revents) : 0;
}
int32_t androidFcntl(Ctx& x, int fd, int cmd, uint32_t arg) {
    if (cmd == 0 || cmd == 1030) {
        const int copy = ::fcntl(fd, cmd == 0 ? F_DUPFD : F_DUPFD_CLOEXEC, int(arg));
        if (copy < 0) return failHost<int32_t>(x);
        duplicate(x.proc, fd, copy);
        duplicateNetworkSocket(x.proc, fd, copy);
        return copy;
    }
    return int32_t(callIos(x, "_fcntl", {uint32_t(fd), uint32_t(cmd), arg}));
}

void registerAndroidEvents(Process& p) {
#define REG(fn, name, what) supportDef<&fn>(p, "android:" name, SupportStatus::Real, what)
    REG(a_close, "close", "release Android descriptors and use libc close");
    REG(a_read, "read", "read Android descriptors or libc files");
    REG(a_write, "write", "write Android descriptors or libc files");
    REG(a_dup, "dup", "duplicate Android and libc descriptor ownership");
    REG(a_dup2, "dup2", "duplicate Android and libc descriptor ownership");
    REG(a_poll, "poll", "Linux pollfd layout and event bits");
    REG(a_pipe, "pipe", "Linux pipe with inherited descriptor flags");
    REG(a_pipe2, "pipe2", "Linux pipe and NONBLOCK/CLOEXEC flags");
    REG(a_eventfd, "eventfd", "Linux event counter descriptor");
    REG(a_eventfd, "eventfd2", "Linux event counter descriptor");
    REG(oldEventfd, "__eventfd", "Linux event counter descriptor with no flags");
    REG(a_timerCreate, "timerfd_create", "Linux timer counter descriptor");
    REG(a_timerSet, "timerfd_settime", "Linux ARM32 itimerspec");
    REG(a_timerGet, "timerfd_gettime", "Linux ARM32 itimerspec");
    REG(a_epollCreate, "epoll_create1", "Linux event queue descriptor");
    REG(oldEpollCreate, "epoll_create", "Linux event queue descriptor");
    REG(a_epollCtl, "epoll_ctl", "Linux ARM EABI event layout");
    REG(a_epollWait, "epoll_wait", "Linux ARM EABI event layout");
    supportDef<&a_epollPwait>(p, "android:epoll_pwait", SupportStatus::Approx, "epoll_wait with a null temporary mask");
#undef REG
}
} // namespace Hydrogen::Runtime::Android
