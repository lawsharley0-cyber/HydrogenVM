// HydrogenVM — time and sleeping.
#include "runtime/Process.hpp"
#include <chrono>
#include <cstring>
#include <ctime>
#include <sys/time.h>
#include <thread>

namespace Hydrogen::Runtime {
namespace {

using Clock = std::chrono::steady_clock;

// Guest struct tm: nine ints, long tm_gmtoff, char *tm_zone (44 bytes).
void storeTm(Ctx& x, GuestPtr out, const std::tm& t) {
    const int32_t f[9] = {t.tm_sec, t.tm_min, t.tm_hour, t.tm_mday, t.tm_mon, t.tm_year, t.tm_wday, t.tm_yday, t.tm_isdst};
    for (int i = 0; i < 9; ++i) x.mem.write<int32_t>(out + uint32_t(i) * 4, f[i]);
    x.mem.write<int32_t>(out + 36, int32_t(t.tm_gmtoff));
    x.mem.write<uint32_t>(out + 40, x.proc.internCString(t.tm_zone ? t.tm_zone : "UTC"));
}
std::tm loadTm(Ctx& x, GuestPtr in) {
    std::tm t{};
    int32_t f[9];
    for (int i = 0; i < 9; ++i) f[i] = x.mem.read<int32_t>(in + uint32_t(i) * 4);
    t.tm_sec = f[0]; t.tm_min = f[1]; t.tm_hour = f[2]; t.tm_mday = f[3]; t.tm_mon = f[4];
    t.tm_year = f[5]; t.tm_wday = f[6]; t.tm_yday = f[7]; t.tm_isdst = f[8];
    t.tm_gmtoff = x.mem.read<int32_t>(in + 36);
    return t;
}

// Sleep in slices so a stop request isn't held up by a long sleep.
void sleepFor(Ctx& x, std::chrono::nanoseconds d) {
    const auto end = Clock::now() + d;
    while (Clock::now() < end) {
        if (x.proc.stopping()) throw ProcessExit(0, "stopping");
        std::this_thread::sleep_for(std::min<std::chrono::nanoseconds>(end - Clock::now(), std::chrono::milliseconds(50)));
    }
}

int32_t t_time(Ctx& x, GuestPtr out) {
    const int32_t t = int32_t(std::time(nullptr));
    if (out) x.mem.write<int32_t>(out, t);
    return t;
}
int32_t t_gettimeofday(Ctx& x, GuestPtr tv, GuestPtr tz) {
    timeval v;
    ::gettimeofday(&v, nullptr);
    if (tv) { x.mem.write<int32_t>(tv, int32_t(v.tv_sec)); x.mem.write<int32_t>(tv + 4, int32_t(v.tv_usec)); }
    if (tz) { x.mem.write<int32_t>(tz, 0); x.mem.write<int32_t>(tz + 4, 0); }
    return 0;
}
uint32_t t_clock(Ctx&) { return uint32_t(std::clock() * (1000000.0 / CLOCKS_PER_SEC)); }
uint64_t t_mach_absolute_time(Ctx&) {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}
int32_t t_mach_timebase_info(Ctx& x, GuestPtr info) {
    x.mem.write<uint32_t>(info, 1);       // mach_absolute_time() is in nanoseconds here
    x.mem.write<uint32_t>(info + 4, 1);
    return 0;
}
int32_t t_usleep(Ctx& x, uint32_t us) { sleepFor(x, std::chrono::microseconds(us)); return 0; }
uint32_t t_sleep(Ctx& x, uint32_t s) { sleepFor(x, std::chrono::seconds(s)); return 0; }
int32_t t_nanosleep(Ctx& x, GuestPtr req, GuestPtr rem) {
    const int64_t s = x.mem.read<int32_t>(req), ns = x.mem.read<int32_t>(req + 4);
    sleepFor(x, std::chrono::seconds(s) + std::chrono::nanoseconds(ns));
    if (rem) { x.mem.write<uint32_t>(rem, 0); x.mem.write<uint32_t>(rem + 4, 0); }
    return 0;
}
// clock_gettime arrived in iOS 10.
int32_t t_clock_gettime(Ctx& x, uint32_t clk, GuestPtr ts) {
    timespec v;
    ::clock_gettime(clk == 0 ? CLOCK_REALTIME : CLOCK_MONOTONIC, &v);
    x.mem.write<int32_t>(ts, int32_t(v.tv_sec));
    x.mem.write<int32_t>(ts + 4, int32_t(v.tv_nsec));
    return 0;
}

uint32_t t_localtime_r(Ctx& x, GuestPtr tp, GuestPtr out) {
    const time_t t = x.mem.read<int32_t>(tp);
    std::tm r;
    ::localtime_r(&t, &r);
    storeTm(x, out, r);
    return out;
}
uint32_t t_gmtime_r(Ctx& x, GuestPtr tp, GuestPtr out) {
    const time_t t = x.mem.read<int32_t>(tp);
    std::tm r;
    ::gmtime_r(&t, &r);
    storeTm(x, out, r);
    return out;
}
uint32_t tmBuffer(Ctx& x) {
    static thread_local uint32_t buf = 0;
    if (!buf) buf = x.proc.allocStatic(64);
    return buf;
}
uint32_t t_localtime(Ctx& x, GuestPtr tp) { return t_localtime_r(x, tp, tmBuffer(x)); }
uint32_t t_gmtime(Ctx& x, GuestPtr tp) { return t_gmtime_r(x, tp, tmBuffer(x)); }
int32_t t_mktime(Ctx& x, GuestPtr in) {
    std::tm t = loadTm(x, in);
    const time_t r = std::mktime(&t);
    storeTm(x, in, t);
    return int32_t(r);
}
int32_t t_timegm(Ctx& x, GuestPtr in) { std::tm t = loadTm(x, in); return int32_t(::timegm(&t)); }
uint32_t t_strftime(Ctx& x, GuestPtr buf, uint32_t max, GuestPtr fmt, GuestPtr tmp) {
    std::tm t = loadTm(x, tmp);
    std::string out(max ? max : 1, '\0');
    const size_t n = std::strftime(out.data(), out.size(), x.proc.readCString(fmt).c_str(), &t);
    if (n && max) x.mem.copyIn(buf, out.c_str(), n + 1);
    return uint32_t(n);
}
double t_difftime(Ctx&, int32_t a, int32_t b) { return double(a) - double(b); }
uint32_t t_ctime(Ctx& x, GuestPtr tp) {
    const time_t t = x.mem.read<int32_t>(tp);
    char b[64];
    ::ctime_r(&t, b);
    static thread_local uint32_t buf = 0;
    if (!buf) buf = x.proc.allocStatic(64);
    x.mem.copyIn(buf, b, std::strlen(b) + 1);
    return buf;
}
void t_tzset(Ctx&) {}

} // namespace

void registerLibcTime(Process& p) {
    p.def<&t_time>("_time");
    p.def<&t_gettimeofday>("_gettimeofday");
    p.def<&t_clock>("_clock");
    p.def<&t_mach_absolute_time>("_mach_absolute_time");
    p.def<&t_mach_timebase_info>("_mach_timebase_info");
    p.def<&t_usleep>("_usleep");
    p.def<&t_sleep>("_sleep");
    p.def<&t_nanosleep>("_nanosleep");
    p.def<&t_clock_gettime>("_clock_gettime");
    p.def<&t_localtime>("_localtime");
    p.def<&t_localtime_r>("_localtime_r");
    p.def<&t_gmtime>("_gmtime");
    p.def<&t_gmtime_r>("_gmtime_r");
    p.def<&t_mktime>("_mktime");
    p.def<&t_timegm>("_timegm");
    p.def<&t_strftime>("_strftime");
    p.def<&t_difftime>("_difftime");
    p.def<&t_ctime>("_ctime");
    p.def<&t_tzset>("_tzset");
}

} // namespace Hydrogen::Runtime
