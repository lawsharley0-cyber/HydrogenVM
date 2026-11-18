#include "runtime/Session.hpp"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstring>
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
        case SessionEvent::Kind::ClockRead: return "clock";
        case SessionEvent::Kind::FileRead: return "fileread";
        case SessionEvent::Kind::RngSeed: return "rngseed";
        case SessionEvent::Kind::NetAnswer: return "net";
        case SessionEvent::Kind::SchedPoint: return "sched";
        case SessionEvent::Kind::Checkpoint: return "checkpoint";
    }
    return "?";
}

std::string escapeToken(const std::string& in) {
    if (in.empty()) return "-";
    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in) {
        if (c <= 0x20 || c == '%' || c == 0x7f) {
            char b[4];
            std::snprintf(b, sizeof b, "%%%02X", c);
            out += b;
        } else {
            out += char(c);
        }
    }
    return out;
}

std::string unescapeToken(const std::string& in) {
    if (in == "-") return "";
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            unsigned int v = 0;
            if (std::sscanf(in.c_str() + i + 1, "%2x", &v) == 1) {
                out += char(v);
                i += 2;
                continue;
            }
        }
        out += in[i];
    }
    return out;
}

std::string toHex(const uint8_t* data, size_t len) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out += kHex[(data[i] >> 4) & 0xf];
        out += kHex[data[i] & 0xf];
    }
    return out;
}

bool fromHex(const std::string& hex, std::vector<uint8_t>& out) {
    if (hex.empty() || (hex.size() % 2) != 0) return false;
    out.resize(hex.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        unsigned int v = 0;
        if (std::sscanf(hex.c_str() + i * 2, "%2x", &v) != 1) return false;
        out[i] = uint8_t(v);
    }
    return true;
}

std::atomic<int64_t> g_wallOffsetNs{0};

} // namespace

uint64_t Session::fnv1a64(const void* data, size_t len) {
    uint64_t h = 14695981039346656037ull;
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

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
    version_ = 2;
    mode_ = Mode::Record;
    std::fprintf(out_, "HVMREC 2 seed=%016" PRIx64 " wall=%" PRId64 " app=%s\n", seed_, hostWallNs(), app.c_str());
    std::fflush(out_);
}

void Session::startReplay(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("can't read the session recording " + path);
    std::string line;
    if (!std::getline(in, line))
        throw std::runtime_error(path + " isn't a HydrogenVM session recording");
    if (line.rfind("HVMREC 1 ", 0) == 0) {
        version_ = 1;
    } else if (line.rfind("HVMREC 2 ", 0) == 0) {
        version_ = 2;
    } else {
        throw std::runtime_error(path + " isn't a HydrogenVM session recording");
    }
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
        allEvents_.push_back(e);
        if (SessionEvent::isInputKind(e.kind)) {
            events_.push_back(e);
        } else {
            switch (e.kind) {
                case SessionEvent::Kind::ClockRead: clockEvents_.push_back(std::move(e)); break;
                case SessionEvent::Kind::FileRead: fileEvents_.push_back(std::move(e)); break;
                case SessionEvent::Kind::RngSeed: rngEvents_.push_back(std::move(e)); break;
                case SessionEvent::Kind::NetAnswer: netEvents_.push_back(std::move(e)); break;
                case SessionEvent::Kind::SchedPoint: schedEvents_.push_back(std::move(e)); break;
                case SessionEvent::Kind::Checkpoint: checkpointEvents_.push_back(std::move(e)); break;
                default: break;
            }
        }
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

void Session::recordClockRead(const std::string& clockKind, uint64_t valueNs, uint64_t ms, uint64_t swaps, uint64_t hooks) {
    if (mode_ != Mode::Record) return;
    SessionEvent e;
    e.ms = ms; e.swaps = swaps; e.hooks = hooks;
    e.kind = SessionEvent::Kind::ClockRead;
    e.label = clockKind;
    e.value = valueNs;
    note(e);
}

std::optional<uint64_t> Session::replayClockRead(const std::string& clockKind) {
    if (mode_ != Mode::Replay) return std::nullopt;
    std::lock_guard<std::mutex> lk(mu_);
    for (size_t i = clockCursor_; i < clockEvents_.size(); ++i) {
        if (clockEvents_[i].label == clockKind) {
            const uint64_t v = clockEvents_[i].value;
            if (i == clockCursor_) ++clockCursor_;
            else clockEvents_[i].label.clear(); // consumed
            return v;
        }
        if (clockEvents_[i].label.empty() && i == clockCursor_) ++clockCursor_;
    }
    return std::nullopt;
}

void Session::recordFileRead(const std::string& path, uint64_t offset, const void* data, size_t len, bool inBundle,
                             uint64_t ms, uint64_t swaps, uint64_t hooks) {
    if (mode_ != Mode::Record) return;
    SessionEvent e;
    e.ms = ms; e.swaps = swaps; e.hooks = hooks;
    e.kind = SessionEvent::Kind::FileRead;
    e.seq = nextSeq_.fetch_add(1, std::memory_order_relaxed);
    e.aux = offset;
    e.aux2 = uint64_t(len);
    e.hash = (data && len) ? fnv1a64(data, len) : 0;
    e.inBundle = inBundle;
    e.label = path;
    if (!inBundle && data && len > 0 && len <= 65536) {
        const auto* p = static_cast<const uint8_t*>(data);
        e.bytes.assign(p, p + len);
    }
    note(e);
}

std::optional<std::vector<uint8_t>> Session::replayFileRead(const std::string& path, uint64_t offset, size_t maxLen) {
    if (mode_ != Mode::Replay) return std::nullopt;
    std::lock_guard<std::mutex> lk(mu_);
    for (size_t i = fileCursor_; i < fileEvents_.size(); ++i) {
        auto& ev = fileEvents_[i];
        if (ev.label.empty()) {
            if (i == fileCursor_) ++fileCursor_;
            continue;
        }
        if (ev.label == path && ev.aux == offset && !ev.inBundle && !ev.bytes.empty()) {
            std::vector<uint8_t> out = ev.bytes;
            if (out.size() > maxLen) out.resize(maxLen);
            ev.label.clear();
            if (i == fileCursor_) ++fileCursor_;
            return out;
        }
    }
    return std::nullopt;
}

void Session::recordRngSeed(const std::string& fn, uint64_t seed, uint64_t ms, uint64_t swaps, uint64_t hooks) {
    if (mode_ != Mode::Record) return;
    SessionEvent e;
    e.ms = ms; e.swaps = swaps; e.hooks = hooks;
    e.kind = SessionEvent::Kind::RngSeed;
    e.label = fn;
    e.value = seed;
    note(e);
}

std::optional<uint64_t> Session::replayRngSeed(const std::string& fn) {
    if (mode_ != Mode::Replay) return std::nullopt;
    std::lock_guard<std::mutex> lk(mu_);
    for (size_t i = rngCursor_; i < rngEvents_.size(); ++i) {
        if (rngEvents_[i].label == fn) {
            const uint64_t v = rngEvents_[i].value;
            rngEvents_[i].label.clear();
            if (i == rngCursor_) ++rngCursor_;
            return v;
        }
        if (rngEvents_[i].label.empty() && i == rngCursor_) ++rngCursor_;
    }
    return std::nullopt;
}

void Session::recordNetAnswer(const void* data, size_t len, uint64_t ms, uint64_t swaps, uint64_t hooks) {
    if (mode_ != Mode::Record) return;
    SessionEvent e;
    e.ms = ms; e.swaps = swaps; e.hooks = hooks;
    e.kind = SessionEvent::Kind::NetAnswer;
    e.seq = nextSeq_.fetch_add(1, std::memory_order_relaxed);
    e.aux2 = uint64_t(len);
    e.hash = (data && len) ? fnv1a64(data, len) : 0;
    if (data && len > 0 && len <= 65536) {
        const auto* p = static_cast<const uint8_t*>(data);
        e.bytes.assign(p, p + len);
    }
    note(e);
}

std::optional<std::vector<uint8_t>> Session::replayNetAnswer() {
    if (mode_ != Mode::Replay) return std::nullopt;
    std::lock_guard<std::mutex> lk(mu_);
    if (netCursor_ >= netEvents_.size()) return std::nullopt;
    return netEvents_[netCursor_++].bytes;
}

void Session::recordSchedPoint(uint32_t tid, const std::string& op, uint64_t addr, uint64_t ms, uint64_t swaps, uint64_t hooks) {
    if (mode_ != Mode::Record) return;
    SessionEvent e;
    e.ms = ms; e.swaps = swaps; e.hooks = hooks;
    e.kind = SessionEvent::Kind::SchedPoint;
    e.seq = nextSeq_.fetch_add(1, std::memory_order_relaxed);
    e.tid = tid;
    e.label = op;
    e.aux = addr;
    note(e);
}

void Session::waitSchedTurn(uint32_t tid, const std::string& op, uint64_t addr) {
    if (mode_ != Mode::Replay) return;
    std::unique_lock<std::mutex> lk(mu_);
    if (schedCursor_ >= schedEvents_.size()) return;
    const auto matches = [&](const SessionEvent& ev) {
        return ev.tid == tid && ev.label == op && ev.aux == addr;
    };
    if (matches(schedEvents_[schedCursor_])) {
        ++schedCursor_;
        schedCv_.notify_all();
        return;
    }
    // Only enforce ordering against the head event when it contends on the same lock address.
    if (schedEvents_[schedCursor_].aux == addr) {
        schedCv_.wait_for(lk, std::chrono::milliseconds(2), [&] {
            return schedCursor_ >= schedEvents_.size() || matches(schedEvents_[schedCursor_]);
        });
        if (schedCursor_ < schedEvents_.size() && matches(schedEvents_[schedCursor_])) {
            ++schedCursor_;
            schedCv_.notify_all();
            return;
        }
        // Timed out: advance past the stale head entry so replay cannot wedge on one missed turn.
        if (schedCursor_ < schedEvents_.size() && schedEvents_[schedCursor_].aux == addr) {
            ++schedCursor_;
            schedCv_.notify_all();
        }
    }
}

void Session::recordCheckpoint(uint64_t frame, uint64_t frameHash, uint64_t callRingTotal, uint64_t callRingHash,
                               uint64_t ms, uint64_t swaps, uint64_t hooks) {
    if (mode_ != Mode::Record) return;
    SessionEvent e;
    e.ms = ms; e.swaps = swaps; e.hooks = hooks;
    e.kind = SessionEvent::Kind::Checkpoint;
    e.aux2 = frame;
    e.value = frameHash;
    e.seq = callRingTotal;
    e.aux = callRingHash;
    note(e);
}

bool Session::checkCheckpoint(uint64_t frame, uint64_t frameHash, uint64_t callRingTotal, uint64_t callRingHash) {
    if (mode_ != Mode::Replay) return true;
    std::lock_guard<std::mutex> lk(mu_);
    while (checkpointCursor_ < checkpointEvents_.size() && checkpointEvents_[checkpointCursor_].aux2 < frame) {
        ++checkpointCursor_;
    }
    if (checkpointCursor_ >= checkpointEvents_.size()) return true;
    const auto& rec = checkpointEvents_[checkpointCursor_];
    if (rec.aux2 != frame) return true;
    ++checkpointCursor_;
    const bool match = (rec.value == frameHash && rec.seq == callRingTotal && rec.aux == callRingHash);
    if (!match && !firstDivergence_) {
        firstDivergence_ = DivergenceInfo{
            frame, rec.value, frameHash, rec.seq, callRingTotal, rec.aux, callRingHash
        };
    }
    return match;
}

std::optional<DivergenceInfo> Session::firstDivergence() const {
    std::lock_guard<std::mutex> lk(mu_);
    return firstDivergence_;
}

std::string Session::formatDivergence() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!firstDivergence_) return "";
    char buf[256];
    std::snprintf(buf, sizeof buf,
                  "Replay diverged at frame %" PRIu64 " (frameHash %016" PRIx64 " vs %016" PRIx64
                  ", callRing total %" PRIu64 " vs %" PRIu64 ", callHash %016" PRIx64 " vs %016" PRIx64 ")",
                  firstDivergence_->frame,
                  firstDivergence_->recordedFrameHash, firstDivergence_->replayFrameHash,
                  firstDivergence_->recordedCallTotal, firstDivergence_->replayCallTotal,
                  firstDivergence_->recordedCallHash, firstDivergence_->replayCallHash);
    return buf;
}

std::string Session::format(const SessionEvent& e) {
    char b[256];
    switch (e.kind) {
        case SessionEvent::Kind::Touch:
            std::snprintf(b, sizeof b, "%" PRIu64 " %" PRIu64 "/%" PRIu64 " touch %d %.2f %.2f %u",
                          e.ms, e.swaps, e.hooks, e.phase, e.x, e.y, e.finger);
            return b;
        case SessionEvent::Kind::Accel:
            std::snprintf(b, sizeof b, "%" PRIu64 " %" PRIu64 "/%" PRIu64 " accel %.4f %.4f %.4f",
                          e.ms, e.swaps, e.hooks, e.x, e.y, e.z);
            return b;
        case SessionEvent::Kind::Background:
        case SessionEvent::Kind::Foreground:
            std::snprintf(b, sizeof b, "%" PRIu64 " %" PRIu64 "/%" PRIu64 " %s",
                          e.ms, e.swaps, e.hooks, kindName(e.kind));
            return b;
        case SessionEvent::Kind::ClockRead:
            std::snprintf(b, sizeof b, "%" PRIu64 " %" PRIu64 "/%" PRIu64 " clock %s %" PRIu64,
                          e.ms, e.swaps, e.hooks, escapeToken(e.label).c_str(), e.value);
            return b;
        case SessionEvent::Kind::FileRead: {
            std::snprintf(b, sizeof b, "%" PRIu64 " %" PRIu64 "/%" PRIu64 " fileread %" PRIu64 " %" PRIu64 " %" PRIu64 " %016" PRIx64 " %d %s",
                          e.ms, e.swaps, e.hooks, e.seq, e.aux, e.aux2, e.hash, e.inBundle ? 1 : 0, escapeToken(e.label).c_str());
            std::string out(b);
            if (!e.inBundle && !e.bytes.empty()) {
                out += ' ';
                out += toHex(e.bytes.data(), e.bytes.size());
            }
            return out;
        }
        case SessionEvent::Kind::RngSeed:
            std::snprintf(b, sizeof b, "%" PRIu64 " %" PRIu64 "/%" PRIu64 " rngseed %s %016" PRIx64,
                          e.ms, e.swaps, e.hooks, escapeToken(e.label).c_str(), e.value);
            return b;
        case SessionEvent::Kind::NetAnswer: {
            std::snprintf(b, sizeof b, "%" PRIu64 " %" PRIu64 "/%" PRIu64 " net %" PRIu64 " %" PRIu64 " %016" PRIx64,
                          e.ms, e.swaps, e.hooks, e.seq, e.aux2, e.hash);
            std::string out(b);
            if (!e.bytes.empty()) {
                out += ' ';
                out += toHex(e.bytes.data(), e.bytes.size());
            }
            return out;
        }
        case SessionEvent::Kind::SchedPoint:
            std::snprintf(b, sizeof b, "%" PRIu64 " %" PRIu64 "/%" PRIu64 " sched %" PRIu64 " %u %s %" PRIx64,
                          e.ms, e.swaps, e.hooks, e.seq, e.tid, escapeToken(e.label).c_str(), e.aux);
            return b;
        case SessionEvent::Kind::Checkpoint:
            std::snprintf(b, sizeof b, "%" PRIu64 " %" PRIu64 "/%" PRIu64 " checkpoint %" PRIu64 " %016" PRIx64 " %" PRIu64 " %016" PRIx64,
                          e.ms, e.swaps, e.hooks, e.aux2, e.value, e.seq, e.aux);
            return b;
    }
    return "";
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
    if (kind == "clock") {
        e.kind = SessionEvent::Kind::ClockRead;
        std::string tok;
        if (!(in >> tok >> e.value)) return false;
        e.label = unescapeToken(tok);
        return true;
    }
    if (kind == "fileread") {
        e.kind = SessionEvent::Kind::FileRead;
        int inB = 0;
        std::string hashHex, pathTok, hexBytes;
        if (!(in >> e.seq >> e.aux >> e.aux2 >> hashHex >> inB >> pathTok)) return false;
        try { e.hash = std::stoull(hashHex, nullptr, 16); } catch (...) { return false; }
        e.inBundle = (inB != 0);
        e.label = unescapeToken(pathTok);
        if (in >> hexBytes) {
            if (!fromHex(hexBytes, e.bytes)) return false;
        }
        return true;
    }
    if (kind == "rngseed") {
        e.kind = SessionEvent::Kind::RngSeed;
        std::string fnTok, seedHex;
        if (!(in >> fnTok >> seedHex)) return false;
        e.label = unescapeToken(fnTok);
        try { e.value = std::stoull(seedHex, nullptr, 16); } catch (...) { return false; }
        return true;
    }
    if (kind == "net") {
        e.kind = SessionEvent::Kind::NetAnswer;
        std::string hashHex, hexBytes;
        if (!(in >> e.seq >> e.aux2 >> hashHex)) return false;
        try { e.hash = std::stoull(hashHex, nullptr, 16); } catch (...) { return false; }
        if (in >> hexBytes) {
            if (!fromHex(hexBytes, e.bytes)) return false;
        }
        return true;
    }
    if (kind == "sched") {
        e.kind = SessionEvent::Kind::SchedPoint;
        std::string opTok, addrHex;
        if (!(in >> e.seq >> e.tid >> opTok >> addrHex)) return false;
        e.label = unescapeToken(opTok);
        try { e.aux = std::stoull(addrHex, nullptr, 16); } catch (...) { return false; }
        return true;
    }
    if (kind == "checkpoint") {
        e.kind = SessionEvent::Kind::Checkpoint;
        std::string fHashHex, cHashHex;
        if (!(in >> e.aux2 >> fHashHex >> e.seq >> cHashHex)) return false;
        try {
            e.value = std::stoull(fHashHex, nullptr, 16);
            e.aux = std::stoull(cHashHex, nullptr, 16);
        } catch (...) { return false; }
        return true;
    }
    return false;
}

} // namespace Hydrogen::Runtime
