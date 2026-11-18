// HydrogenVM — a stand-in for Linux's /proc, for Android games.
//
// Mono refuses to start without /proc/self/maps ("Mono requires /proc to be mounted"), Unity's
// loader reads /proc/self/auxv and /proc/cpuinfo to check for NEON, and Boehm GC counts CPUs in
// /proc/stat. The phone has no /proc (and a Linux host's own must not leak into the game), so
// guest paths under /proc go to a folder of generated files in the game's tmp/. Files that
// change (maps, stat, status, uptime) are written again each time the game looks them up.
//
// /proc/self, /proc/thread-self and /proc/<pid> (the game is pid 1000) are the same folder.
#include "runtime/android/Android.hpp"
#include "runtime/android/BionicInternal.hpp"
#include "runtime/Process.hpp"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace Hydrogen::Runtime::Android {
namespace {

constexpr int kPid = 1000;
constexpr int kCpus = 4;

std::string packageOf(Process& p) {
    const auto* info = p.infoPlist();
    return info ? info->getString("CFBundleIdentifier", "app") : std::string("app");
}

void writeFile(const fs::path& file, const std::string& text) {
    const fs::path tmp = file.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(text.data(), std::streamsize(text.size()));
    }
    std::error_code ec;
    fs::rename(tmp, file, ec);
}

double uptimeSeconds() {
    static const auto start = std::chrono::steady_clock::now();
    return 3600.0 + std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

std::string cpuinfo() {
    std::ostringstream o;
    o << "Processor\t: ARMv7 Processor rev 0 (v7l)\n";
    for (int i = 0; i < kCpus; ++i)
        o << "processor\t: " << i << "\nBogoMIPS\t: 38.40\n\n";
    o << "Features\t: half thumb fastmult vfp edsp neon vfpv3 tls vfpv4\n"
         "CPU implementer\t: 0x41\nCPU architecture: 7\nCPU variant\t: 0x0\nCPU part\t: 0xc0f\nCPU revision\t: 0\n\n"
         "Hardware\t: HydrogenVM\nRevision\t: 0000\nSerial\t\t: 0000000000000000\n";
    return o.str();
}

std::string statAll() {
    std::ostringstream o;
    o << "cpu  1000 0 1000 100000 0 0 0 0 0 0\n";
    for (int i = 0; i < kCpus; ++i) o << "cpu" << i << " 250 0 250 25000 0 0 0 0 0 0\n";
    o << "intr 0\nctxt 0\nbtime 1700000000\nprocesses " << kPid << "\nprocs_running 1\nprocs_blocked 0\n";
    return o.str();
}

std::string meminfo() {
    return "MemTotal:        2953216 kB\nMemFree:          524288 kB\nMemAvailable:    1572864 kB\n"
           "Buffers:           65536 kB\nCached:           786432 kB\nSwapTotal:             0 kB\nSwapFree:              0 kB\n";
}

// AT_HWCAP: half thumb fastmult vfp edsp neon vfpv3 tls vfpv4; then the page size.
std::string auxv() {
    const uint32_t hwcap = 2 | 4 | 16 | 64 | 128 | 4096 | 8192 | 32768 | 65536;
    const uint32_t words[] = {16, hwcap, 26, 0, 6, 4096, 0, 0};
    return std::string(reinterpret_cast<const char*>(words), sizeof words);
}

std::string maps(Process& p) {
    std::ostringstream o;
    char line[512];
    auto row = [&](uint32_t lo, uint32_t hi, const char* perms, const std::string& name) {
        std::snprintf(line, sizeof line, "%08x-%08x %s 00000000 00:00 0          %s\n", lo, hi, perms, name.c_str());
        o << line;
    };
    auto& lib = p.service<Library>();
    {
        std::lock_guard<std::recursive_mutex> lk(lib.mu);
        for (const auto& l : lib.libs) {
            const uint32_t lo = l->so.image.lowest & ~0xFFFu, hi = (l->so.image.highest + 0xFFFu) & ~0xFFFu;
            if (hi > lo) row(lo, hi, "r-xp", p.guestPath(l->path));
        }
    }
    for (const uint32_t id : p.liveThreadIds()) {
        GuestThread* t = p.findThreadById(id);
        if (!t || !t->stackSize) continue;
        const uint32_t lo = t->stackBase & ~0xFFFu, hi = (t->stackBase + t->stackSize + 0xFFFu) & ~0xFFFu;
        row(lo, hi, "rw-p", id == 0 ? "[stack]" : "[stack:" + std::to_string(1000 + id) + "]");
    }
    row(0xFFFF0000u, 0xFFFF1000u, "r-xp", "[vectors]");
    return o.str();
}

std::string selfStat(Process& p, const std::string& pkg) {
    uint32_t startStack = 0;
    if (GuestThread* main = p.findThreadById(0)) startStack = main->stackBase + main->stackSize - 16;
    const auto ticks = uint64_t(uptimeSeconds() * 100);
    std::ostringstream o;
    o << kPid << " (" << pkg.substr(0, 15) << ") R 1 " << kPid << " 0 0 -1 4194624 0 0 0 0 "
      << ticks / 2 << " " << ticks / 8 << " 0 0 20 0 " << p.liveThreadIds().size() << " 0 100 "
      << 1073741824u << " 65536 4294967295 " << kLibraryBase << " " << kLibraryBase + 0x01000000u << " "
      << startStack << " 0 0 0 0 4612 0 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 0\n";
    return o.str();
}

std::string status(Process& p, const std::string& pkg) {
    std::ostringstream o;
    o << "Name:\t" << pkg.substr(0, 15) << "\nState:\tR (running)\nTgid:\t" << kPid << "\nPid:\t" << kPid
      << "\nPPid:\t1\nTracerPid:\t0\nUid:\t10050\t10050\t10050\t10050\nGid:\t10050\t10050\t10050\t10050\n"
         "VmPeak:\t 1048576 kB\nVmSize:\t 1048576 kB\nVmRSS:\t  262144 kB\nThreads:\t"
      << p.liveThreadIds().size() << "\nSigQ:\t0/0\nSigPnd:\t0000000000000000\nCpus_allowed:\tf\n";
    return o.str();
}

struct ProcFs {
    explicit ProcFs(Process&) {}
    std::mutex mu;
    fs::path root;
};

// The host path for one /proc path; regenerates what it names.
std::string resolve(Process& p, const std::string& guest) {
    auto& pf = p.service<ProcFs>();
    std::lock_guard<std::mutex> lk(pf.mu);
    std::error_code ec;
    const std::string pkg = packageOf(p);
    if (pf.root.empty()) {
        pf.root = fs::path(p.homePath()) / "tmp" / ".hvm-proc";
        fs::create_directories(pf.root / "self" / "fd", ec);
        fs::create_directories(pf.root / "self" / "task", ec);
        fs::create_directories(pf.root / "net", ec);
        writeFile(pf.root / "cpuinfo", cpuinfo());
        writeFile(pf.root / "meminfo", meminfo());
        writeFile(pf.root / "self" / "auxv", auxv());
        writeFile(pf.root / "self" / "cmdline", pkg + std::string(1, '\0'));
        writeFile(pf.root / "self" / "comm", pkg.substr(0, 15) + "\n");
        writeFile(pf.root / "net" / "dev", "Inter-|   Receive |  Transmit\n face |bytes packets|bytes packets\n");
        fs::remove(pf.root / "self" / "exe", ec);
        fs::create_symlink("/system/bin/app_process", pf.root / "self" / "exe", ec);
        fs::remove(pf.root / std::to_string(kPid), ec);
        fs::create_directory_symlink("self", pf.root / std::to_string(kPid), ec);
    }
    // /proc/self, /proc/thread-self, /proc/<number> -> self.
    std::string rest = guest.size() > 5 ? guest.substr(6) : std::string();   // after "/proc/"
    if (!rest.empty()) {
        const size_t slash = rest.find('/');
        const std::string first = rest.substr(0, slash);
        const bool number = !first.empty() && first.find_first_not_of("0123456789") == std::string::npos;
        if (first == "thread-self" || number) rest = "self" + (slash == std::string::npos ? std::string() : rest.substr(slash));
    }
    if (rest == "self/maps") writeFile(pf.root / "self" / "maps", maps(p));
    else if (rest == "self/stat") writeFile(pf.root / "self" / "stat", selfStat(p, pkg));
    else if (rest == "self/status") writeFile(pf.root / "self" / "status", status(p, pkg));
    else if (rest == "stat") writeFile(pf.root / "stat", statAll());
    else if (rest == "uptime") {
        char text[64];
        std::snprintf(text, sizeof text, "%.2f %.2f\n", uptimeSeconds(), uptimeSeconds() * 3);
        writeFile(pf.root / "uptime", text);
    }
    return rest.empty() ? pf.root.string() : (pf.root / rest).string();
}

} // namespace

void installProcFs(Process& p) {
    p.procPath = [&p](const std::string& guest) { return resolve(p, guest); };
}

} // namespace Hydrogen::Runtime::Android
