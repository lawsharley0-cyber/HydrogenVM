// HydrogenVM — session recording and replay.
//
// A recording captures what made one play session differ from another: the seed every random source
// in the runtime draws from, the wall-clock time the session started, the player's input
// (touches, tilt, leaving and returning to the app), plus (HVMREC 2) timer/clock reads, file reads
// (path, offset, length, hash; bytes only for files outside the bundle), non-session RNG seeds,
// network answers when "Allow internet" is on, thread scheduling points at locks/waits, and
// frame/call-ring checkpoints for divergence detection.
//
// Replay starts the game with the same seed and the same apparent date, feeds input and recorded
// nondeterministic events back, enforces thread lock/wait ordering where possible, and reports the
// first frame that diverges from the recording.
//
// File format (text, one record per line, backward-compatible with HVMREC 1):
//   HVMREC 2 seed=<hex64> wall=<ns since epoch> app=<bundle id>
//   <ms> <swaps>/<hooks> touch <phase> <x> <y> <finger>
//   <ms> <swaps>/<hooks> accel <x> <y> <z>
//   <ms> <swaps>/<hooks> background | foreground
//   <ms> <swaps>/<hooks> clock <kind> <valueNs>
//   <ms> <swaps>/<hooks> fileread <seq> <offset> <length> <hashHex64> <inBundle> <escapedPath> [hexBytes]
//   <ms> <swaps>/<hooks> rngseed <fn> <seedHex64>
//   <ms> <swaps>/<hooks> net <seq> <length> <hashHex64> [hexBytes]
//   <ms> <swaps>/<hooks> sched <seq> <tid> <op> <addrHex>
//   <ms> <swaps>/<hooks> checkpoint <frame> <frameHashHex64> <callRingTotal> <callRingHashHex64>
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Hydrogen::Runtime {

struct SessionEvent {
    uint64_t ms = 0;
    uint64_t swaps = 0;   // frames the game had handed over (GL presents)
    uint64_t hooks = 0;   // main run-loop frame ticks (UIKit games that don't use GL)
    enum class Kind {
        Touch,
        Accel,
        Background,
        Foreground,
        ClockRead,
        FileRead,
        RngSeed,
        NetAnswer,
        SchedPoint,
        Checkpoint,
    } kind = Kind::Touch;
    int phase = 0;
    uint32_t finger = 0;
    float x = 0, y = 0, z = 0;

    // HVMREC 2 fields
    uint64_t seq = 0;
    uint64_t value = 0;       // clock valueNs, rng seed, or checkpoint frameHash
    uint64_t aux = 0;         // sched addr, checkpoint callRingHash, or file offset
    uint64_t aux2 = 0;        // file/net length, checkpoint frame, or checkpoint callRingTotal
    uint64_t hash = 0;        // file/net FNV-1a 64-bit hash
    uint32_t tid = 0;         // thread id for sched
    bool inBundle = false;    // file read inside app bundle (bytes omitted)
    std::string label;        // clock kind, rng fn, sched op, or escaped file path
    std::vector<uint8_t> bytes; // external file read bytes or network answer bytes

    static SessionEvent input(Kind k, int phase = 0, uint32_t finger = 0, float x = 0, float y = 0, float z = 0) {
        SessionEvent e;
        e.kind = k; e.phase = phase; e.finger = finger; e.x = x; e.y = y; e.z = z;
        return e;
    }

    static bool isInputKind(Kind k) {
        return k == Kind::Touch || k == Kind::Accel || k == Kind::Background || k == Kind::Foreground;
    }
};

struct DivergenceInfo {
    uint64_t frame = 0;
    uint64_t recordedFrameHash = 0;
    uint64_t replayFrameHash = 0;
    uint64_t recordedCallTotal = 0;
    uint64_t replayCallTotal = 0;
    uint64_t recordedCallHash = 0;
    uint64_t replayCallHash = 0;
};

// The wall-clock time the guest sees, ns since 1970 (the host's, shifted while a replay runs so
// the game sees the recorded date). For code with no Process at hand; one process replays at a time.
int64_t guestWallNowNs();

class Session {
public:
    enum class Mode { Off, Record, Replay };

    // Recording: a fresh seed and the current time. Throws std::runtime_error if the file can't
    // be written.
    void startRecording(const std::string& path, const std::string& app);
    // Replay: reads the file (HVMREC 1 or 2). Throws std::runtime_error with a readable message on a bad file.
    void startReplay(const std::string& path);

    Mode mode() const { return mode_; }
    int version() const { return version_; }
    // Input events (Touch, Accel, Background, Foreground) delivered by SessionReplayer.
    const std::vector<SessionEvent>& events() const { return events_; }
    // All parsed events (including HVMREC 2 records).
    const std::vector<SessionEvent>& allEvents() const { return allEvents_; }
    const std::string& app() const { return app_; }

    // Every random value the runtime hands the guest comes from here: a deterministic stream
    // from the session seed (a random seed when not recording or replaying).
    uint64_t entropy();
    // Wall-clock time the guest sees, in ns since 1970: the host's, shifted on replay so the
    // game sees the recorded session's date.
    int64_t wallNowNs() const;

    void note(const SessionEvent& e);   // while recording; thread-safe

    // HVMREC 2 record/replay helpers (thread-safe).
    void recordClockRead(const std::string& clockKind, uint64_t valueNs, uint64_t ms = 0, uint64_t swaps = 0, uint64_t hooks = 0);
    std::optional<uint64_t> replayClockRead(const std::string& clockKind);

    void recordFileRead(const std::string& path, uint64_t offset, const void* data, size_t len, bool inBundle,
                        uint64_t ms = 0, uint64_t swaps = 0, uint64_t hooks = 0);
    // Returns recorded bytes if an outside-bundle read matches (path, offset) during replay.
    std::optional<std::vector<uint8_t>> replayFileRead(const std::string& path, uint64_t offset, size_t maxLen);

    void recordRngSeed(const std::string& fn, uint64_t seed, uint64_t ms = 0, uint64_t swaps = 0, uint64_t hooks = 0);
    std::optional<uint64_t> replayRngSeed(const std::string& fn);

    void recordNetAnswer(const void* data, size_t len, uint64_t ms = 0, uint64_t swaps = 0, uint64_t hooks = 0);
    std::optional<std::vector<uint8_t>> replayNetAnswer();

    void recordSchedPoint(uint32_t tid, const std::string& op, uint64_t addr, uint64_t ms = 0, uint64_t swaps = 0, uint64_t hooks = 0);
    // Enforce recorded lock/wait ordering where possible (bounded wait so diverged replays don't deadlock).
    void waitSchedTurn(uint32_t tid, const std::string& op, uint64_t addr);

    void recordCheckpoint(uint64_t frame, uint64_t frameHash, uint64_t callRingTotal, uint64_t callRingHash,
                          uint64_t ms = 0, uint64_t swaps = 0, uint64_t hooks = 0);
    // Returns true if matches (or no recorded checkpoint for this frame); false on first divergence.
    bool checkCheckpoint(uint64_t frame, uint64_t frameHash, uint64_t callRingTotal, uint64_t callRingHash);
    std::optional<DivergenceInfo> firstDivergence() const;
    std::string formatDivergence() const;

    static uint64_t fnv1a64(const void* data, size_t len);
    static std::string format(const SessionEvent& e);
    static bool parse(const std::string& line, SessionEvent& out);

private:
    Mode mode_ = Mode::Off;
    int version_ = 2;
    uint64_t seed_ = 0;
    bool seeded_ = false;
    std::atomic<uint64_t> draws_{0};
    std::atomic<uint64_t> nextSeq_{1};
    std::string app_;
    std::vector<SessionEvent> events_;
    std::vector<SessionEvent> allEvents_;

    // Replay cursors
    std::vector<SessionEvent> clockEvents_;
    size_t clockCursor_ = 0;
    std::vector<SessionEvent> fileEvents_;
    size_t fileCursor_ = 0;
    std::vector<SessionEvent> rngEvents_;
    size_t rngCursor_ = 0;
    std::vector<SessionEvent> netEvents_;
    size_t netCursor_ = 0;
    std::vector<SessionEvent> schedEvents_;
    size_t schedCursor_ = 0;
    std::vector<SessionEvent> checkpointEvents_;
    size_t checkpointCursor_ = 0;
    std::optional<DivergenceInfo> firstDivergence_;

    mutable std::mutex mu_;
    std::condition_variable schedCv_;
    std::FILE* out_ = nullptr;
    void seedOnce();
public:
    ~Session();
};

} // namespace Hydrogen::Runtime
