// HydrogenVM — session recording and replay.
//
// A recording is what made one play session differ from another: the seed every random source
// in the runtime draws from, the wall-clock time the session started, and the player's input
// (touches, tilt, leaving and returning to the app), each stamped with the time since launch
// and how many frames the game had finished by then.
//
// Replay starts the game with the same seed and the same apparent date, and feeds the input back
// when the game reaches the same frame (or, if it renders fewer frames, a little after the same
// time). Guest threads still run on real host threads, so replay is close rather than exact; it
// is exact enough to bring a crash reported from the phone back on the bench.
//
// File format (text, one record per line, so it can be pasted into an issue):
//   HVMREC 1 seed=<hex64> wall=<ns since epoch> app=<bundle id>
//   <ms> <swaps>/<hooks> touch <phase> <x> <y> <finger>
//   <ms> <swaps>/<hooks> accel <x> <y> <z>
//   <ms> <swaps>/<hooks> background | foreground
// swaps: frames the game had handed to the screen; hooks: main run-loop frame ticks.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace Hydrogen::Runtime {

struct SessionEvent {
    uint64_t ms = 0;
    uint64_t swaps = 0;   // frames the game had handed over (GL presents)
    uint64_t hooks = 0;   // main run-loop frame ticks (UIKit games that don't use GL)
    enum class Kind { Touch, Accel, Background, Foreground } kind = Kind::Touch;
    int phase = 0;
    uint32_t finger = 0;
    float x = 0, y = 0, z = 0;

    static SessionEvent input(Kind k, int phase = 0, uint32_t finger = 0, float x = 0, float y = 0, float z = 0) {
        SessionEvent e;
        e.kind = k; e.phase = phase; e.finger = finger; e.x = x; e.y = y; e.z = z;
        return e;
    }
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
    // Replay: reads the file. Throws std::runtime_error with a readable message on a bad file.
    void startReplay(const std::string& path);

    Mode mode() const { return mode_; }
    const std::vector<SessionEvent>& events() const { return events_; }
    const std::string& app() const { return app_; }

    // Every random value the runtime hands the guest comes from here: a deterministic stream
    // from the session seed (a random seed when not recording or replaying).
    uint64_t entropy();
    // Wall-clock time the guest sees, in ns since 1970: the host's, shifted on replay so the
    // game sees the recorded session's date.
    int64_t wallNowNs() const;

    void note(const SessionEvent& e);   // while recording; thread-safe

    static std::string format(const SessionEvent& e);
    static bool parse(const std::string& line, SessionEvent& out);

private:
    Mode mode_ = Mode::Off;
    uint64_t seed_ = 0;
    bool seeded_ = false;
    std::atomic<uint64_t> draws_{0};
    std::string app_;
    std::vector<SessionEvent> events_;
    std::mutex mu_;
    std::FILE* out_ = nullptr;
    void seedOnce();
public:
    ~Session();
};

} // namespace Hydrogen::Runtime
