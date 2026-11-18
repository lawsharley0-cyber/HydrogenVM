// HydrogenVM — AAudio (libaaudio.so) for Android games. See Android.hpp.
//
// Each AAudio output stream is a stream voice in HydrogenVM's shared mixer. Games may push audio
// with AAudioStream_write() or register a data callback on the stream builder (pumped by a guest
// thread while the stream is started).
#include "runtime/android/BionicInternal.hpp"
#include "runtime/audio/Audio.hpp"
#include "runtime/Support.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Hydrogen::Runtime::Android {
namespace {

using namespace std::chrono_literals;

constexpr int32_t kOk = 0;
constexpr int32_t kErrIllegalArg = -898;
constexpr int32_t kErrInvalidState = -895;
constexpr int32_t kErrUnimplemented = -890;
constexpr int32_t kErrNull = -888;

constexpr int32_t kStateUninitialized = 0;
constexpr int32_t kStateOpen = 1;
constexpr int32_t kStateStarted = 3;
constexpr int32_t kStatePaused = 5;
constexpr int32_t kStateFlushed = 7;
constexpr int32_t kStateStopped = 9;
constexpr int32_t kStateClosed = 11;

constexpr int32_t kFormatUnspecified = 0;
constexpr int32_t kFormatPcmI16 = 1;
constexpr int32_t kFormatPcmFloat = 2;

constexpr int32_t kDirectionOutput = 0;
constexpr int32_t kDirectionInput = 1;

constexpr int32_t kCallbackContinue = 0;

struct Builder {
    int32_t deviceId = 0;
    int32_t sampleRate = 44100;
    int32_t channelCount = 2;
    int32_t format = kFormatPcmI16;
    int32_t sharingMode = 0;
    int32_t direction = kDirectionOutput;
    int32_t bufferCapacity = 4096;
    int32_t performanceMode = 0;
    uint32_t dataCallback = 0;
    uint32_t userData = 0;
    uint32_t errorCallback = 0;
    uint32_t errorUserData = 0;
    int32_t framesPerCallback = 256;
};

struct Stream {
    uint32_t voice = 0;
    int32_t deviceId = 1;
    int32_t sampleRate = 44100;
    int32_t channelCount = 2;
    int32_t format = kFormatPcmI16;
    int32_t sharingMode = 0;
    int32_t direction = kDirectionOutput;
    int32_t performanceMode = 0;
    int32_t bufferCapacity = 4096;
    int32_t bufferSize = 2048;
    int32_t framesPerBurst = 256;
    int32_t state = kStateOpen;
    int64_t framesWritten = 0;
    int64_t framesRead = 0;
    uint64_t nextTag = 1;
    uint32_t dataCallback = 0;
    uint32_t userData = 0;
    int32_t framesPerCallback = 256;
    std::atomic<bool> pumpActive{false};
    bool pumpSpawned = false;
    std::chrono::steady_clock::time_point startTime{};
};

struct AAState {
    explicit AAState(Process&) {}
    std::mutex mu;
    std::unordered_map<uint32_t, Builder> builders;
    std::unordered_map<uint32_t, std::shared_ptr<Stream>> streams;
    uint32_t pumpEntry = 0;
    uint32_t textCache = 0;
};
AAState& S(Ctx& x) { return x.proc.service<AAState>(); }

Audio::Pcm convertToStereoI16(const void* src, int32_t numFrames, int32_t channels, int32_t format) {
    auto pcm = std::make_shared<std::vector<int16_t>>();
    if (!src || numFrames <= 0) return pcm;
    pcm->resize(size_t(numFrames) * 2);
    if (format == kFormatPcmFloat) {
        const float* f = static_cast<const float*>(src);
        for (int32_t i = 0; i < numFrames; ++i) {
            const float l = std::clamp(f[i * channels], -1.0f, 1.0f);
            const float r = channels >= 2 ? std::clamp(f[i * channels + 1], -1.0f, 1.0f) : l;
            (*pcm)[size_t(i) * 2] = int16_t(l * 32767.0f);
            (*pcm)[size_t(i) * 2 + 1] = int16_t(r * 32767.0f);
        }
    } else {
        const int16_t* s = static_cast<const int16_t*>(src);
        for (int32_t i = 0; i < numFrames; ++i) {
            const int16_t l = s[i * channels];
            const int16_t r = channels >= 2 ? s[i * channels + 1] : l;
            (*pcm)[size_t(i) * 2] = l;
            (*pcm)[size_t(i) * 2 + 1] = r;
        }
    }
    return pcm;
}

void pumpThread(Ctx& x, uint32_t streamPtr) {
    if (GuestThread* t = x.proc.currentThreadIfGuest()) t->role.store("AAudio callback thread");
    std::shared_ptr<Stream> st;
    {
        auto& s = S(x);
        std::lock_guard<std::mutex> lk(s.mu);
        auto it = s.streams.find(streamPtr);
        if (it != s.streams.end()) st = it->second;
    }
    if (!st) return;
    const int32_t nFrames = std::clamp(st->framesPerCallback, 64, 4096);
    const size_t sampleBytes = st->format == kFormatPcmFloat ? 4 : 2;
    const uint32_t bufBytes = uint32_t(nFrames) * uint32_t(st->channelCount) * uint32_t(sampleBytes);
    const uint32_t guestBuf = x.proc.heap().allocZeroed(bufBytes);
    while (!x.proc.stopping() && st->pumpActive.load()) {
        if (st->state != kStateStarted) {
            std::this_thread::sleep_for(5ms);
            continue;
        }
        auto& mixer = x.proc.mixer();
        if (mixer.queuedCount(st->voice) >= 6) {
            mixer.takeProcessed(st->voice);
            std::this_thread::sleep_for(4ms);
            continue;
        }
        std::memset(x.mem.host(guestBuf), 0, bufBytes);
        const int32_t action = int32_t(x.proc.callGuest(x.cpu, st->dataCallback,
                                                        {streamPtr, st->userData, guestBuf, uint32_t(nFrames)}));
        auto pcm = convertToStereoI16(x.mem.host(guestBuf), nFrames, st->channelCount, st->format);
        mixer.enqueue(st->voice, pcm, st->sampleRate, st->nextTag++);
        mixer.takeProcessed(st->voice);
        st->framesWritten += nFrames;
        if (action != kCallbackContinue) {
            st->pumpActive.store(false);
            break;
        }
    }
    x.proc.heap().free(guestBuf);
    st->pumpSpawned = false;
}

// ----------------------------------------------------------------- builder ---

int32_t createStreamBuilder(Ctx& x, GuestPtr outBuilder) {
    if (!outBuilder) return kErrNull;
    const uint32_t h = x.proc.heap().allocZeroed(16);
    auto& s = S(x);
    std::lock_guard<std::mutex> lk(s.mu);
    s.builders[h] = Builder{};
    x.mem.write<uint32_t>(outBuilder, h);
    return kOk;
}

int32_t deleteBuilder(Ctx& x, GuestPtr b) {
    if (!b) return kErrNull;
    {
        auto& s = S(x);
        std::lock_guard<std::mutex> lk(s.mu);
        if (!s.builders.erase(b)) return kErrIllegalArg;
    }
    x.proc.heap().free(b);
    return kOk;
}

template <typename F>
void withBuilder(Ctx& x, GuestPtr b, F&& fn) {
    auto& s = S(x);
    std::lock_guard<std::mutex> lk(s.mu);
    if (auto it = s.builders.find(b); it != s.builders.end()) fn(it->second);
}

void sb_setDeviceId(Ctx& x, GuestPtr b, int32_t v) { withBuilder(x, b, [&](Builder& B) { B.deviceId = v; }); }
void sb_setSampleRate(Ctx& x, GuestPtr b, int32_t v) { withBuilder(x, b, [&](Builder& B) { if (v > 0) B.sampleRate = v; }); }
void sb_setChannelCount(Ctx& x, GuestPtr b, int32_t v) { withBuilder(x, b, [&](Builder& B) { if (v > 0) B.channelCount = v; }); }
void sb_setFormat(Ctx& x, GuestPtr b, int32_t v) { withBuilder(x, b, [&](Builder& B) { if (v != kFormatUnspecified) B.format = v; }); }
void sb_setSharingMode(Ctx& x, GuestPtr b, int32_t v) { withBuilder(x, b, [&](Builder& B) { B.sharingMode = v; }); }
void sb_setDirection(Ctx& x, GuestPtr b, int32_t v) { withBuilder(x, b, [&](Builder& B) { B.direction = v; }); }
void sb_setBufferCap(Ctx& x, GuestPtr b, int32_t v) { withBuilder(x, b, [&](Builder& B) { if (v > 0) B.bufferCapacity = v; }); }
void sb_setPerfMode(Ctx& x, GuestPtr b, int32_t v) { withBuilder(x, b, [&](Builder& B) { B.performanceMode = v; }); }
void sb_noop(Ctx&, GuestPtr, int32_t) {}
void sb_setDataCallback(Ctx& x, GuestPtr b, GuestPtr cb, GuestPtr ud) {
    withBuilder(x, b, [&](Builder& B) { B.dataCallback = cb; B.userData = ud; });
}
void sb_setErrorCallback(Ctx& x, GuestPtr b, GuestPtr cb, GuestPtr ud) {
    withBuilder(x, b, [&](Builder& B) { B.errorCallback = cb; B.errorUserData = ud; });
}
void sb_setFramesPerCallback(Ctx& x, GuestPtr b, int32_t v) {
    withBuilder(x, b, [&](Builder& B) { if (v > 0) B.framesPerCallback = v; });
}

int32_t sb_openStream(Ctx& x, GuestPtr b, GuestPtr outStream) {
    if (!b || !outStream) return kErrNull;
    Builder cfg;
    {
        auto& s = S(x);
        std::lock_guard<std::mutex> lk(s.mu);
        auto it = s.builders.find(b);
        if (it == s.builders.end()) return kErrIllegalArg;
        cfg = it->second;
    }
    if (cfg.direction == kDirectionInput) return kErrUnimplemented;
    auto st = std::make_shared<Stream>();
    st->deviceId = cfg.deviceId ? cfg.deviceId : 1;
    st->sampleRate = cfg.sampleRate > 0 ? cfg.sampleRate : 44100;
    st->channelCount = cfg.channelCount > 0 ? cfg.channelCount : 2;
    st->format = cfg.format == kFormatPcmFloat ? kFormatPcmFloat : kFormatPcmI16;
    st->sharingMode = cfg.sharingMode;
    st->direction = cfg.direction;
    st->performanceMode = cfg.performanceMode;
    st->bufferCapacity = std::max(cfg.bufferCapacity, 1024);
    st->bufferSize = st->bufferCapacity / 2;
    st->framesPerBurst = std::clamp(cfg.framesPerCallback, 64, 1024);
    st->dataCallback = cfg.dataCallback;
    st->userData = cfg.userData;
    st->framesPerCallback = cfg.framesPerCallback > 0 ? cfg.framesPerCallback : 256;
    st->voice = x.proc.mixer().createStream({});
    x.proc.mixer().setPaused(st->voice, true);

    const uint32_t h = x.proc.heap().allocZeroed(16);
    {
        auto& s = S(x);
        std::lock_guard<std::mutex> lk(s.mu);
        s.streams[h] = std::move(st);
    }
    x.mem.write<uint32_t>(outStream, h);
    return kOk;
}

// ------------------------------------------------------------------ stream ---

std::shared_ptr<Stream> getStream(Ctx& x, GuestPtr h) {
    auto& s = S(x);
    std::lock_guard<std::mutex> lk(s.mu);
    auto it = s.streams.find(h);
    return it == s.streams.end() ? nullptr : it->second;
}

int32_t st_close(Ctx& x, GuestPtr h) {
    std::shared_ptr<Stream> st;
    {
        auto& s = S(x);
        std::lock_guard<std::mutex> lk(s.mu);
        auto it = s.streams.find(h);
        if (it == s.streams.end()) return kErrIllegalArg;
        st = std::move(it->second);
        s.streams.erase(it);
    }
    st->pumpActive.store(false);
    st->state = kStateClosed;
    x.proc.mixer().stop(st->voice);
    x.proc.mixer().release(st->voice);
    x.proc.heap().free(h);
    return kOk;
}

int32_t st_requestStart(Ctx& x, GuestPtr h) {
    auto st = getStream(x, h);
    if (!st) return kErrIllegalArg;
    st->state = kStateStarted;
    st->startTime = std::chrono::steady_clock::now();
    x.proc.mixer().setPaused(st->voice, false);
    if (st->dataCallback && !st->pumpSpawned) {
        st->pumpActive.store(true);
        st->pumpSpawned = true;
        uint32_t entry = 0;
        {
            auto& s = S(x);
            std::lock_guard<std::mutex> lk(s.mu);
            if (!s.pumpEntry) s.pumpEntry = x.proc.registerHostFunction("__hvm_aaudio_pump", [](Ctx& tx) { pumpThread(tx, tx.cpu.r[0]); });
            entry = s.pumpEntry;
        }
        x.proc.startThread(entry, h, 512u << 10, true);
    }
    return kOk;
}

int32_t st_requestPause(Ctx& x, GuestPtr h) {
    auto st = getStream(x, h);
    if (!st) return kErrIllegalArg;
    st->state = kStatePaused;
    x.proc.mixer().setPaused(st->voice, true);
    return kOk;
}

int32_t st_requestFlush(Ctx& x, GuestPtr h) {
    auto st = getStream(x, h);
    if (!st) return kErrIllegalArg;
    if (st->state == kStateStarted) return kErrInvalidState;
    x.proc.mixer().stop(st->voice);
    st->state = kStateFlushed;
    return kOk;
}

int32_t st_requestStop(Ctx& x, GuestPtr h) {
    auto st = getStream(x, h);
    if (!st) return kErrIllegalArg;
    st->pumpActive.store(false);
    st->state = kStateStopped;
    x.proc.mixer().stop(st->voice);
    return kOk;
}

int32_t st_waitForStateChange(Ctx& x, GuestPtr h, int32_t inputState, GuestPtr nextStateOut, int64_t) {
    auto st = getStream(x, h);
    if (!st) return kErrIllegalArg;
    if (nextStateOut) x.mem.write<int32_t>(nextStateOut, st->state);
    (void)inputState;
    return kOk;
}

int32_t st_write(Ctx& x, GuestPtr h, GuestPtr buffer, int32_t numFrames, int64_t) {
    auto st = getStream(x, h);
    if (!st) return kErrIllegalArg;
    if (!buffer || numFrames < 0) return kErrIllegalArg;
    if (numFrames == 0) return 0;
    auto pcm = convertToStereoI16(x.mem.host(buffer), numFrames, st->channelCount, st->format);
    auto& mixer = x.proc.mixer();
    if (mixer.queuedCount(st->voice) < 32) mixer.enqueue(st->voice, pcm, st->sampleRate, st->nextTag++);
    mixer.takeProcessed(st->voice);
    st->framesWritten += numFrames;
    if (st->state == kStateStarted) {
        for (;;) {
            const double played = std::chrono::duration<double>(std::chrono::steady_clock::now() - st->startTime).count();
            if (double(st->framesWritten) / st->sampleRate <= played + 0.1 || x.proc.stopping()) break;
            std::this_thread::sleep_for(5ms);
        }
    }
    return numFrames;
}

int32_t st_read(Ctx& x, GuestPtr h, GuestPtr, int32_t, int64_t) {
    auto st = getStream(x, h);
    return st ? 0 : kErrIllegalArg;
}

int32_t st_getState(Ctx& x, GuestPtr h) { auto st = getStream(x, h); return st ? st->state : kStateUninitialized; }
int32_t st_getSampleRate(Ctx& x, GuestPtr h) { auto st = getStream(x, h); return st ? st->sampleRate : 0; }
int32_t st_getChannelCount(Ctx& x, GuestPtr h) { auto st = getStream(x, h); return st ? st->channelCount : 0; }
int32_t st_getFormat(Ctx& x, GuestPtr h) { auto st = getStream(x, h); return st ? st->format : kFormatUnspecified; }
int32_t st_getDirection(Ctx& x, GuestPtr h) { auto st = getStream(x, h); return st ? st->direction : kDirectionOutput; }
int32_t st_getSharingMode(Ctx& x, GuestPtr h) { auto st = getStream(x, h); return st ? st->sharingMode : 0; }
int32_t st_getPerfMode(Ctx& x, GuestPtr h) { auto st = getStream(x, h); return st ? st->performanceMode : 0; }
int32_t st_getBufferSize(Ctx& x, GuestPtr h) { auto st = getStream(x, h); return st ? st->bufferSize : 0; }
int32_t st_setBufferSize(Ctx& x, GuestPtr h, int32_t frames) {
    auto st = getStream(x, h);
    if (!st) return kErrIllegalArg;
    st->bufferSize = std::clamp(frames, 64, st->bufferCapacity);
    return st->bufferSize;
}
int32_t st_getBufferCap(Ctx& x, GuestPtr h) { auto st = getStream(x, h); return st ? st->bufferCapacity : 0; }
int32_t st_getFramesPerBurst(Ctx& x, GuestPtr h) { auto st = getStream(x, h); return st ? st->framesPerBurst : 0; }
int64_t st_getFramesWritten(Ctx& x, GuestPtr h) { auto st = getStream(x, h); return st ? st->framesWritten : 0; }
int64_t st_getFramesRead(Ctx& x, GuestPtr h) { auto st = getStream(x, h); return st ? st->framesRead : 0; }
int32_t st_getXRunCount(Ctx&) { return 0; }
int32_t st_getDeviceId(Ctx& x, GuestPtr h) { auto st = getStream(x, h); return st ? st->deviceId : 0; }
int32_t st_getTimestamp(Ctx& x, GuestPtr h, int32_t, GuestPtr framePos, GuestPtr timeNs) {
    auto st = getStream(x, h);
    if (!st) return kErrIllegalArg;
    if (framePos) x.mem.write<int64_t>(framePos, st->framesWritten);
    if (timeNs) {
        const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        x.mem.write<int64_t>(timeNs, ns);
    }
    return kOk;
}

uint32_t convertResultToText(Ctx& x, int32_t code) {
    const char* msg = code == kOk ? "AAUDIO_OK" : "AAUDIO_ERROR";
    auto& s = S(x);
    std::lock_guard<std::mutex> lk(s.mu);
    if (!s.textCache) s.textCache = x.proc.allocStatic(32);
    std::memcpy(x.mem.host(s.textCache), msg, std::strlen(msg) + 1);
    return s.textCache;
}

} // namespace

void registerAAudio(Process& p) {
#define R(F, NAME, WHAT) supportDef<&F>(p, std::string("android:") + NAME, SupportStatus::Real, WHAT)
    R(createStreamBuilder, "AAudio_createStreamBuilder", "creates a stream builder");
    R(deleteBuilder, "AAudioStreamBuilder_delete", "frees the builder");
    R(sb_setDeviceId, "AAudioStreamBuilder_setDeviceId", "keeps the device id");
    R(sb_setSampleRate, "AAudioStreamBuilder_setSampleRate", "sets sample rate");
    R(sb_setChannelCount, "AAudioStreamBuilder_setChannelCount", "sets channel count");
    R(sb_setChannelCount, "AAudioStreamBuilder_setSamplesPerFrame", "sets channel count");
    R(sb_setFormat, "AAudioStreamBuilder_setFormat", "PCM I16 or Float");
    R(sb_setSharingMode, "AAudioStreamBuilder_setSharingMode", "keeps sharing mode");
    R(sb_setDirection, "AAudioStreamBuilder_setDirection", "output or input");
    R(sb_setBufferCap, "AAudioStreamBuilder_setBufferCapacityInFrames", "sets buffer capacity");
    R(sb_setPerfMode, "AAudioStreamBuilder_setPerformanceMode", "keeps performance mode");
    R(sb_noop, "AAudioStreamBuilder_setUsage", "accepted");
    R(sb_noop, "AAudioStreamBuilder_setContentType", "accepted");
    R(sb_noop, "AAudioStreamBuilder_setInputPreset", "accepted");
    R(sb_noop, "AAudioStreamBuilder_setSessionId", "accepted");
    R(sb_setDataCallback, "AAudioStreamBuilder_setDataCallback", "registers audio callback");
    R(sb_setErrorCallback, "AAudioStreamBuilder_setErrorCallback", "registers error callback");
    R(sb_setFramesPerCallback, "AAudioStreamBuilder_setFramesPerDataCallback", "sets callback frames");
    R(sb_openStream, "AAudioStreamBuilder_openStream", "opens a mixer stream");

    R(st_close, "AAudioStream_close", "closes the stream");
    R(st_requestStart, "AAudioStream_requestStart", "starts playback");
    R(st_requestPause, "AAudioStream_requestPause", "pauses playback");
    R(st_requestFlush, "AAudioStream_requestFlush", "flushes queued audio");
    R(st_requestStop, "AAudioStream_requestStop", "stops playback");
    R(st_waitForStateChange, "AAudioStream_waitForStateChange", "reports stream state");
    R(st_write, "AAudioStream_write", "queues PCM frames to the mixer");
    R(st_read, "AAudioStream_read", "no microphone");
    R(st_getState, "AAudioStream_getState", "current state");
    R(st_getSampleRate, "AAudioStream_getSampleRate", "sample rate");
    R(st_getChannelCount, "AAudioStream_getChannelCount", "channel count");
    R(st_getChannelCount, "AAudioStream_getSamplesPerFrame", "channel count");
    R(st_getFormat, "AAudioStream_getFormat", "sample format");
    R(st_getDirection, "AAudioStream_getDirection", "stream direction");
    R(st_getSharingMode, "AAudioStream_getSharingMode", "sharing mode");
    R(st_getPerfMode, "AAudioStream_getPerformanceMode", "performance mode");
    R(st_getBufferSize, "AAudioStream_getBufferSizeInFrames", "buffer size");
    R(st_setBufferSize, "AAudioStream_setBufferSizeInFrames", "sets buffer size");
    R(st_getBufferCap, "AAudioStream_getBufferCapacityInFrames", "buffer capacity");
    R(st_getFramesPerBurst, "AAudioStream_getFramesPerBurst", "frames per burst");
    R(st_getFramesWritten, "AAudioStream_getFramesWritten", "frames written");
    R(st_getFramesRead, "AAudioStream_getFramesRead", "frames read");
    R(st_getXRunCount, "AAudioStream_getXRunCount", "0");
    R(st_getDeviceId, "AAudioStream_getDeviceId", "device id");
    R(st_getTimestamp, "AAudioStream_getTimestamp", "frame position and clock");
    R(convertResultToText, "AAudio_convertResultToText", "result text");
    R(convertResultToText, "AAudio_convertStreamStateToText", "state text");
#undef R
}

} // namespace Hydrogen::Runtime::Android
