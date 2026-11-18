#include "runtime/Session.hpp"

#include <chrono>
#include <cinttypes>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>

namespace Hydrogen::Runtime {
namespace {

int64_t hostWallNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

uint64_t splitmix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

const char* kindName(SessionEvent::Kind k) {
    switch (k) {
        case SessionEvent::Kind::Touch: return "touch";
        case SessionEvent::Kind::Accel: return "accel";
        case SessionEvent::Kind::Background: return "background";
        case SessionEvent::Kind::Foreground: return "foreground";
    }
    return "?";
}

std::atomic<int64_t> g_wallOffsetNs{0};

} // namespace

int64_t guestWallNowNs() { return hostWallNs() + g_wallOffsetNs.load(std::memory_order_relaxed); }

Session::~Session() {
    if (out_) std::fclose(out_);
    if (mode_ == Mode::Replay) g_wallOffsetNs = 0;
}

void Session::seedOnce() {
    if (seeded_) return;
    std::random_device rd;
    seed_ = (uint64_t(rd()) << 32) ^ rd();
    seeded_ = true;
}

void Session::startRecording(const std::string& path, const std::string& app) {
    out_ = std::fopen(path.c_str(), "w");
    if (!out_) throw std::runtime_error("can't write the session recording " + path);
    seedOnce();
    app_ = app;
    mode_ = Mode::Record;
    std::fprintf(out_, "HVMREC 1 seed=%016" PRIx64 " wall=%" PRId64 " app=%s\n", seed_, hostWallNs(), app.c_str());
    std::fflush(out_);
}

void Session::startReplay(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("can't read the session recording " + path);
    std::string line;
    if (!std::getline(in, line) || line.rfind("HVMREC 1 ", 0) != 0)
        throw std::runtime_error(path + " isn't a HydrogenVM session recording");
    std::istringstream head(line.substr(9));
    std::string field;
    int64_t wall = 0;
    bool haveSeed = false;
    while (head >> field) {
        if (field.rfind("seed=", 0) == 0) { seed_ = std::stoull(field.substr(5), nullptr, 16); haveSeed = true; }
        else if (field.rfind("wall=", 0) == 0) wall = std::stoll(field.substr(5));
        else if (field.rfind("app=", 0) == 0) app_ = field.substr(4);
    }
    if (!haveSeed) throw std::runtime_error(path + ": the recording has no seed");
    seeded_ = true;
    if (wall) g_wallOffsetNs = wall - hostWallNs();
    int n = 1;
    while (std::getline(in, line)) {
        ++n;
        if (line.empty()) continue;
        SessionEvent e;
        if (!parse(line, e)) throw std::runtime_error(path + ": line " + std::to_string(n) + " isn't an input event");
        events_.push_back(e);
    }
    mode_ = Mode::Replay;
}

uint64_t Session::entropy() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        seedOnce();
    }
    return splitmix(seed_ + draws_.fetch_add(1, std::memory_order_relaxed) * 0x9E3779B97F4A7C15ull);
}

int64_t Session::wallNowNs() const { return guestWallNowNs(); }

void Session::note(const SessionEvent& e) {
    if (mode_ != Mode::Record) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (!out_) return;
    const std::string line = format(e);
    std::fputs(line.c_str(), out_);
    std::fputc('\n', out_);
    std::fflush(out_);   // a crash right after still leaves the input that led to it
}

std::string Session::format(const SessionEvent& e) {
    char b[160];
    switch (e.kind) {
        case SessionEvent::Kind::Touch:
            std::snprintf(b, sizeof b, "%" PRIu64 " %" PRIu64 "/%" PRIu64 " touch %d %.2f %.2f %u", e.ms, e.swaps, e.hooks, e.phase, e.x, e.y, e.finger);
            break;
        case SessionEvent::Kind::Accel:
            std::snprintf(b, sizeof b, "%" PRIu64 " %" PRIu64 "/%" PRIu64 " accel %.4f %.4f %.4f", e.ms, e.swaps, e.hooks, e.x, e.y, e.z);
            break;
        default:
            std::snprintf(b, sizeof b, "%" PRIu64 " %" PRIu64 "/%" PRIu64 " %s", e.ms, e.swaps, e.hooks, kindName(e.kind));
    }
    return b;
}

bool Session::parse(const std::string& line, SessionEvent& e) {
    std::istringstream in(line);
    std::string kind, frames;
    if (!(in >> e.ms >> frames >> kind)) return false;
    const auto slash = frames.find('/');
    if (slash == std::string::npos) return false;
    try { e.swaps = std::stoull(frames.substr(0, slash)); e.hooks = std::stoull(frames.substr(slash + 1)); }
    catch (...) { return false; }
    if (kind == "touch") { e.kind = SessionEvent::Kind::Touch; return bool(in >> e.phase >> e.x >> e.y >> e.finger); }
    if (kind == "accel") { e.kind = SessionEvent::Kind::Accel; return bool(in >> e.x >> e.y >> e.z); }
    if (kind == "background") { e.kind = SessionEvent::Kind::Background; return true; }
    if (kind == "foreground") { e.kind = SessionEvent::Kind::Foreground; return true; }
    return false;
}

} // namespace Hydrogen::Runtime
