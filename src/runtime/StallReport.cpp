// HydrogenVM — stall reports.
//
// A game that freezes usually leaves nothing in the log: every thread is waiting on another,
// or one is spinning while holding what the others need. Once a second the run's watcher
// looks at the main thread; when it has waited 5 s in one place, or no frame has been
// presented for 20 s after the first, the log gets one "Stall:" report:
//
//   Stall: the main thread has waited 5.0 s in pthread_mutex_lock (0x10b36a00, held by thread 32)
//     wait chain: main -> thread 32 (pthread_mutex_lock 0x10b39df0) -> thread 31 (running)
//     thread 32: waiting 5.0 s in pthread_mutex_lock (0x10b39df0, held by thread 31)
//     thread 31: running guest code, 412M instructions in the last 5 s, near Game+0x1234
//     main backtrace: ...
//
// Waits mark themselves with BlockedScope (only inside the wait, so running code pays nothing).
// A report is written once per stall; a stall that goes on is reported again after 60 s, and
// a run writes at most five.
#include "Process.hpp"
#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <vector>
#include <cstdio>
#include <sstream>
#include <unordered_map>

namespace Hydrogen::Runtime {
namespace {
int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::string hex(uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "0x%08x", v); return b; }
std::string secs(int64_t ms) { char b[24]; std::snprintf(b, sizeof b, "%.1f s", double(ms) / 1000.0); return b; }
}  // namespace

BlockedScope::BlockedScope(Process& p, const char* what, uint32_t object, uint32_t owner) {
    GuestThread* t = p.currentThreadIfGuest();
    if (!t || t->blockedIn.load(std::memory_order_relaxed)) return;   // not a guest thread, or nested
    t_ = t;
    t->blockedObject.store(object, std::memory_order_relaxed);
    t->blockedOwner.store(owner, std::memory_order_relaxed);
    t->blockedSinceMs.store(nowMs(), std::memory_order_relaxed);
    t->blockedIn.store(what, std::memory_order_release);
}
BlockedScope::~BlockedScope() {
    if (t_) t_->blockedIn.store(nullptr, std::memory_order_release);
}

struct Process::StallWatch {
    std::mutex mu;
    struct Seen { uint64_t instructions = 0; int64_t atMs = 0; };
    std::unordered_map<uint32_t, std::deque<Seen>> history;   // per thread, the last ~6 s
    uint64_t lastFrames = 0;
    int64_t lastFrameChangeMs = 0;
    int64_t reportedSinceMs = -1;     // the stall episode already reported (its start)
    int64_t lastReportMs = 0;
    int reports = 0;
};
Process::StallWatch* Process::newStallWatch() { return new StallWatch(); }
void Process::deleteStallWatch(StallWatch* w) { delete w; }

void Process::checkStall(StallWatch& w, const char* snapshot) {
    std::lock_guard<std::mutex> watchLock(w.mu);   // the watcher and a stop snapshot can race
    const int64_t now = nowMs();
    if (!snapshot && (stopping() || terminating() || gracefulStopRequested() || backgrounded() || backgroundRequested())) {
        w.lastFrameChangeMs = now;   // a paused game draws nothing; start counting again after it resumes
        return;
    }
    struct Row {
        GuestThread* t;
        bool main;
        const char* in;
        uint32_t object, owner;
        int64_t sinceMs;
        uint64_t instructions;
    };
    std::vector<Row> rows;
    {
        std::lock_guard<std::mutex> lk(threadsMu_);
        auto add = [&](GuestThread* t) {
            if (t->finished.load(std::memory_order_acquire)) return;
            const char* in = t->blockedIn.load(std::memory_order_acquire);
            rows.push_back({t, t == mainThread_, in, t->blockedObject.load(std::memory_order_relaxed),
                            t->blockedOwner.load(std::memory_order_relaxed), t->blockedSinceMs.load(std::memory_order_relaxed),
                            t->cpu.insn_count});
        };
        add(mainThread_);
        for (auto& [id, t] : threads_) if (t.get() != mainThread_) add(t.get());
    }
    for (const Row& r : rows) {
        auto& h = w.history[r.t->id];
        h.push_back({r.instructions, now});
        while (h.size() > 7) h.pop_front();
    }
    const uint64_t frames = perf_.framesPresented.load();
    if (frames != w.lastFrames || !w.lastFrameChangeMs) { w.lastFrames = frames; w.lastFrameChangeMs = now; }

    if (rows.empty()) return;
    const Row& main = rows.front();
    int64_t episode = -1;
    std::string headline;
    if (snapshot) {
        headline = std::string(snapshot) + "; the main thread is " +
                   (main.in ? "waiting " + secs(now - main.sinceMs) + " in " + main.in : std::string("running guest code"));
    } else if (main.in && now - main.sinceMs >= 5000) {
        episode = main.sinceMs;
        headline = "the main thread has waited " + secs(now - main.sinceMs) + " in " + main.in;
        if (main.object || main.owner) {
            headline += " (";
            if (main.object) headline += hex(main.object);
            if (main.owner) headline += std::string(main.object ? ", " : "") + "held by thread " + std::to_string(main.owner);
            headline += ")";
        }
    } else if (frames > 0 && now - w.lastFrameChangeMs >= 20000) {
        episode = w.lastFrameChangeMs;
        headline = "no frame presented for " + secs(now - w.lastFrameChangeMs) + " (frame " + std::to_string(frames) +
                   " was the last); the main thread is " + (main.in ? std::string("waiting in ") + main.in : "running guest code");
    }
    if (!snapshot) {
        if (episode < 0) return;
        if (episode == w.reportedSinceMs && now - w.lastReportMs < 60000) return;
        if (w.reports >= 5) return;
        w.reportedSinceMs = episode;
        w.lastReportMs = now;
        ++w.reports;
    }

    std::unordered_map<uint32_t, const Row*> byId;
    for (const Row& r : rows) byId[r.t->id] = &r;
    auto rate = [&](const Row& r) -> std::string {
        const auto& h = w.history[r.t->id];
        if (h.size() < 2) return "";
        const double dt = double(h.back().atMs - h.front().atMs) / 1000.0;
        const uint64_t di = h.back().instructions - h.front().instructions;
        char b[96];
        std::snprintf(b, sizeof b, "%.1fM instructions in the last %.0f s", double(di) / 1e6, dt);
        return b;
    };
    auto name = [&](const Row& r) {
        std::string n = r.main ? std::string("main") : "thread " + std::to_string(r.t->id);
        if (const char* role = r.t->role.load(std::memory_order_relaxed)) n += std::string(" (") + role + ")";
        return n;
    };
    std::ostringstream out;
    out << (snapshot ? "Snapshot: " : "Stall: ") << headline << "\n";
    // Who the main thread is waiting for, and who that thread is waiting for, until a thread
    // that runs (holding what the rest need) or a cycle (a deadlock).
    if (main.owner) {
        out << "  wait chain: main";
        std::vector<uint32_t> seen;
        uint32_t next = main.owner;
        for (int hop = 0; next && hop < 8; ++hop) {
            out << " -> thread " << next;
            if (std::find(seen.begin(), seen.end(), next) != seen.end()) { out << " (a cycle: deadlock)"; break; }
            seen.push_back(next);
            auto it = byId.find(next);
            if (it == byId.end()) { out << " (finished or unknown)"; break; }
            const Row& r = *it->second;
            if (r.main) { out << " (main: a cycle, deadlock)"; break; }
            if (!r.in) { out << " (running, " << rate(r) << ")"; break; }
            out << " (" << r.in << (r.object ? " " + hex(r.object) : "") << ")";
            next = r.owner;
        }
        out << "\n";
    }
    // Threads that matter get a line each: main, everyone in the wait chain, threads that hold
    // what someone waits for, and threads executing guest code. The rest (worker pools waiting
    // for work, sleepers) are counted by what they wait in.
    std::vector<uint32_t> relevant;
    for (const Row& r : rows) if (r.owner) relevant.push_back(r.owner);
    auto ran = [&](const Row& r) {
        const auto& h = w.history[r.t->id];
        return h.size() >= 2 && h.back().instructions != h.front().instructions;
    };
    std::map<std::string, int> quiet;
    for (const Row& r : rows) {
        const bool holder = std::find(relevant.begin(), relevant.end(), r.t->id) != relevant.end();
        if (!r.main && !holder && !r.owner && (r.in || !ran(r))) {
            ++quiet[r.in ? "in " + std::string(r.in) : std::string("idle")];
            continue;
        }
        out << "  " << name(r) << ": ";
        if (r.in) {
            out << "waiting " << secs(now - r.sinceMs) << " in " << r.in;
            if (r.object) out << " " << hex(r.object);
            if (r.owner) out << ", held by thread " << r.owner;
        } else if (!ran(r)) {
            out << "in HydrogenVM, no guest instructions in the last 5 s, near " << describeAddress(r.t->cpu.r[CPU::kPC]);
        } else {
            // The thread is running: its registers change under us, so only an approximate place.
            out << "running guest code, " << rate(r) << ", near " << describeAddress(r.t->cpu.r[CPU::kPC]);
        }
        out << "\n";
    }
    if (!quiet.empty()) {
        int n = 0;
        for (auto& [what, count] : quiet) n += count;
        out << "  " << n << " other thread" << (n == 1 ? "" : "s") << ":";
        bool first = true;
        for (auto& [what, count] : quiet) { out << (first ? " " : ", ") << count << " " << what; first = false; }
        out << "\n";
    }
    for (const Row& r : rows) {
        if (!r.in || !(r.main || r.owner || now - r.sinceMs >= 5000)) continue;
        if (snapshot && !r.main && !r.owner) continue;   // a stop snapshot: main and the lock waiters only
        out << "  " << name(r) << " backtrace:\n" << backtrace(r.t->cpu, 8);
    }
    std::string text = out.str();
    while (!text.empty() && text.back() == '\n') text.pop_back();
    log(LogLevel::Warning, text);
}
}  // namespace Hydrogen::Runtime
