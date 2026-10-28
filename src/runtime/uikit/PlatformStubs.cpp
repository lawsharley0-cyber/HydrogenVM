// HydrogenVM — Audio emulation (AudioToolbox, AVAudioSession, AVAudioPlayer,
// OpenAL) and platform framework stubs (GameKit, StoreKit, MessageUI,
// SystemConfiguration).
#include "UIKit.hpp"
#include "runtime/audio/Audio.hpp"
#include <algorithm>
#include <chrono>
#include <deque>
#include <cmath>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <vector>

namespace Hydrogen::Runtime::UIKit {

using Foundation::autoreleased;
using Foundation::makeString;
using Foundation::utf8;

namespace {

void noteOnce(Ctx& x, const char* what) { x.proc.reportMissing(what); }

// Decoding lives in runtime/audio (WAV, CAF, AIFF, MP3); playback in the mixer.
using Audio::DecodedAudio;
inline DecodedAudio decodeAudioBytes(const uint8_t* data, size_t n) { return Audio::decode(data, n); }

Audio::Mixer& MX(Process& p) { return p.mixer(); }
Audio::Pcm sharePcm(const std::vector<int16_t>& v) { return v.empty() ? nullptr : std::make_shared<const std::vector<int16_t>>(v); }
void audioTrace(const char* operation, uint32_t id = 0, uint64_t detail = 0) {
    static const bool enabled = std::getenv("HVM_AUDIO_TRACE") != nullptr;
    if (enabled) std::fprintf(stderr, "[audio-trace] %s id=%u detail=%llu\n", operation, id, (unsigned long long)detail);
}

// ============================================================= AudioToolbox ===

struct SystemSoundEntry {
    DecodedAudio audio;
    Audio::Pcm pcm;
    uint32_t completionProc = 0;
    uint32_t clientData = 0;
};

struct AudioQueueBufferInfo {
    uint32_t guestAddr = 0;
    uint32_t capacity = 0;
};

struct AudioQueueInstance {
    uint32_t id = 0;
    uint32_t callback = 0;
    uint32_t userData = 0;
    int sampleRate = 44100;
    int channels = 2;
    int bitsPerChannel = 16;
    float volume = 1.0f;
    bool running = false, isFloat = false, nonInterleaved = false;
    uint32_t voice = 0;   // mixer stream; buffers go back to the app as they finish
    std::shared_ptr<Foundation::RunLoop::Timer> pump;
    std::vector<AudioQueueBufferInfo> buffers;
    // Recording queues (AudioQueueNewInput): buffers wait here until they are filled
    // with microphone audio at the queue's rate.
    bool input = false, micClaimed = false;
    std::deque<uint32_t> pending;
    double dueFrames = 0, sampleTime = 0;
    std::chrono::steady_clock::time_point last{};
    uint32_t timeStamp = 0;   // guest AudioTimeStamp handed to the input callback
};

struct AudioFileInstance {
    uint32_t id = 0;
    std::vector<uint8_t> rawBytes;
    DecodedAudio decoded;
};

struct AudioUnitInstance {
    uint32_t id = 0;
    uint32_t renderCallback = 0;
    uint32_t renderUserData = 0;
    uint32_t desc[5] = {0, 0, 0, 0, 0};
    bool initialized = false, running = false;
    double sampleRate = 44100;
    uint32_t formatFlags = 0x0C, bytesPerFrame = 4, channels = 2, bits = 16;
    uint32_t voice = 0;
    uint32_t pumpThread = 0;
    uint32_t bufferList = 0, bufferStorage = 0, bufferBytes = 0;
    uint32_t actionFlags = 0, timeStamp = 0;
    double sampleTime = 0;
    uint64_t nextTag = 1;
};

struct AUGraphInstance {
    uint32_t id;
    std::vector<uint32_t> nodes;
};

struct AudioToolboxState {
    explicit AudioToolboxState(Process&) {}
    std::recursive_mutex unitsMu;
    uint32_t nextSoundId = 1;
    uint32_t nextQueueId = 1;
    uint32_t nextFileId = 1;
    uint32_t nextUnitId = 1;
    uint32_t nextGraphId = 1;
    std::unordered_map<uint32_t, SystemSoundEntry> sounds;
    std::unordered_map<uint32_t, AudioQueueInstance> queues;
    std::unordered_map<uint32_t, AudioFileInstance> files;
    std::unordered_map<uint32_t, AudioUnitInstance> units;
    std::unordered_map<uint32_t, AUGraphInstance> graphs;
};
AudioToolboxState& ATS(Ctx& x) { return x.proc.service<AudioToolboxState>(); }

int32_t as_init(Ctx&, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
int32_t as_setProperty(Ctx&, uint32_t, uint32_t, uint32_t) { return 0; }
int32_t as_getProperty(Ctx& x, uint32_t prop, GuestPtr size, GuestPtr out) {
    if (out && size) {
        const uint32_t n = x.mem.read<uint32_t>(size);
        if (prop == 0x63687372 /* 'chsr' hw sample rate */ && n >= 8) x.mem.write<double>(out, 44100.0);
        else if (prop == 0x6368766C /* 'chvl' output volume */ && n >= 4) x.mem.write<float>(out, 1.0f);
        else if (prop == 0x61696176 /* 'aiav' audio input available */ && n >= 4) x.mem.write<uint32_t>(out, 1);
        else if (prop == 0x63686963 /* 'chic' input channels */ && n >= 4) x.mem.write<uint32_t>(out, 1);
        else if (prop == 0x63686f63 /* 'choc' output channels */ && n >= 4) x.mem.write<uint32_t>(out, 2);
        else if (n >= 4) x.mem.write<uint32_t>(out, 0);
    }
    return 0;
}
int32_t as_getPropertySize(Ctx& x, uint32_t prop, GuestPtr outSize) {
    if (outSize) x.mem.write<uint32_t>(outSize, prop == 0x63687372 ? 8 : 4);
    return 0;
}
int32_t as_setActive(Ctx&, bool) { return 0; }
int32_t as_addListener(Ctx&, uint32_t, uint32_t, uint32_t) { return 0; }

int32_t ss_create(Ctx& x, uint32_t urlRef, GuestPtr outId) {
    auto& st = ATS(x);
    const uint32_t sid = st.nextSoundId++;
    SystemSoundEntry entry;
    if (urlRef) {
        const std::string path = Foundation::urlFilePath(x, urlRef);
        std::ifstream in(path, std::ios::binary);
        if (in) {
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
            entry.audio = decodeAudioBytes(bytes.data(), bytes.size());
            entry.pcm = sharePcm(entry.audio.stereoPcm);
        }
    }
    st.sounds[sid] = std::move(entry);
    if (outId) x.mem.write<uint32_t>(outId, sid);
    return 0;
}
void ss_play(Ctx& x, uint32_t sid) {
    auto& st = ATS(x);
    auto it = st.sounds.find(sid);
    if (it == st.sounds.end()) return;
    MX(x.proc).play(it->second.pcm, it->second.audio.sampleRate, {}, 0, true);
    if (it->second.completionProc) {
        const uint32_t fn = it->second.completionProc, ud = it->second.clientData;
        Process* p = &x.proc;
        const double after = std::max(0.01, it->second.audio.duration);
        Foundation::mainRunLoop(x.proc).addTimer(after, after, false, [p, fn, sid, ud](CPU::Cpu& c) { p->callGuest(c, fn, {sid, ud}); });
    }
}
int32_t ss_dispose(Ctx& x, uint32_t sid) { ATS(x).sounds.erase(sid); return 0; }
int32_t ss_addCompletion(Ctx& x, uint32_t sid, uint32_t, uint32_t, uint32_t proc, uint32_t userData) {
    auto& st = ATS(x);
    st.sounds[sid].completionProc = proc;
    st.sounds[sid].clientData = userData;
    return 0;
}
void ss_removeCompletion(Ctx& x, uint32_t sid) {
    auto& st = ATS(x);
    if (st.sounds.count(sid)) st.sounds[sid].completionProc = 0;
}

// AudioQueue API
int32_t aq_newOutput(Ctx& x, GuestPtr format, uint32_t callbackProc, uint32_t userData, uint32_t, uint32_t, uint32_t, GuestPtr outAQ) {
    auto& st = ATS(x);
    const uint32_t qid = st.nextQueueId++;
    AudioQueueInstance q;
    q.id = qid;
    q.callback = callbackProc;
    q.userData = userData;
    if (format) {
        const double sr = x.mem.read<double>(format);
        const uint32_t ch = x.mem.read<uint32_t>(format + 28);
        const uint32_t bits = x.mem.read<uint32_t>(format + 32);
        if (sr >= 4000 && sr <= 192000) q.sampleRate = int(sr);
        if (ch >= 1 && ch <= 8) q.channels = int(ch);
        if (bits == 8 || bits == 16 || bits == 32) q.bitsPerChannel = int(bits);
        const uint32_t flags = x.mem.read<uint32_t>(format + 12);
        q.isFloat = flags & 1;
        q.nonInterleaved = flags & 0x20;
    }
    q.voice = MX(x.proc).createStream({q.volume, 0, 1, false});
    audioTrace("AudioQueueNewOutput", qid, (uint64_t(q.sampleRate) << 32) | uint32_t(q.channels));
    MX(x.proc).setPaused(q.voice, true);   // until AudioQueueStart
    st.queues[qid] = std::move(q);
    if (outAQ) x.mem.write<uint32_t>(outAQ, qid);
    return 0;
}
int32_t aq_allocateBuffer(Ctx& x, uint32_t qid, uint32_t byteSize, GuestPtr outBuf) {
    auto& st = ATS(x);
    if (!outBuf || !st.queues.count(qid)) return -50;
    // AudioQueueBuffer layout on 32-bit iOS (28 bytes + audio data):
    //   0: uint32_t mAudioDataBytesCapacity
    //   4: void*    mAudioData
    //   8: uint32_t mAudioDataByteSize
    //  12: void*    mUserData
    //  16: uint32_t mPacketDescriptionCapacity
    //  20: void*    mPacketDescriptions
    //  24: uint32_t mPacketDescriptionCount
    const uint32_t cap = std::max(byteSize, 16u);
    const uint32_t hdr = x.proc.heap().allocZeroed(32 + cap);
    const uint32_t dataPtr = hdr + 32;
    x.mem.write<uint32_t>(hdr + 0, cap);
    x.mem.write<uint32_t>(hdr + 4, dataPtr);
    x.mem.write<uint32_t>(hdr + 8, 0);
    st.queues[qid].buffers.push_back({hdr, cap});
    x.mem.write<uint32_t>(outBuf, hdr);
    return 0;
}
// Same buffer with room for packet descriptions (16-byte AudioStreamPacketDescription each).
int32_t aq_allocateBufferWithPacketDescriptions(Ctx& x, uint32_t qid, uint32_t byteSize, uint32_t descs, GuestPtr outBuf) {
    const int32_t r = aq_allocateBuffer(x, qid, byteSize, outBuf);
    if (r != 0 || !descs) return r;
    const uint32_t hdr = x.mem.read<uint32_t>(outBuf);
    x.mem.write<uint32_t>(hdr + 16, descs);
    x.mem.write<uint32_t>(hdr + 20, x.proc.heap().allocZeroed(descs * 16));
    return 0;
}
int32_t aq_freeBuffer(Ctx& x, uint32_t qid, GuestPtr buf) {
    auto& st = ATS(x);
    if (st.queues.count(qid) && buf) {
        if (const uint32_t pd = x.mem.read<uint32_t>(buf + 20)) x.proc.heap().free(pd);
        auto& v = st.queues[qid].buffers;
        v.erase(std::remove_if(v.begin(), v.end(), [&](const auto& b) { return b.guestAddr == buf; }), v.end());
        x.proc.heap().free(buf);
    }
    return 0;
}
// Hands finished buffers back to the app's callback (on the main run loop).
void aqPumpInput(Process& p, CPU::Cpu& c, uint32_t qid);
void aqPump(Process& p, CPU::Cpu& c, uint32_t qid) {
    auto& st = p.service<AudioToolboxState>();
    auto it = st.queues.find(qid);
    if (it == st.queues.end()) return;
    if (it->second.input) { aqPumpInput(p, c, qid); return; }
    const uint32_t cb = it->second.callback, ud = it->second.userData;
    for (uint64_t buf : MX(p).takeProcessed(it->second.voice)) {
        if (p.terminating() || p.stopping() || !cb || !st.queues.count(qid)) break;
        p.callGuest(c, cb, {ud, qid, uint32_t(buf)});
    }
}
int32_t aq_enqueueBuffer(Ctx& x, uint32_t qid, GuestPtr buf, uint32_t, GuestPtr);
// AudioQueueEnqueueBufferWithParameters(q, buf, nDescs, descs, trimStart, trimEnd, nParams, params, startTime, actualStart)
void aq_enqueueWithParams(Ctx& x) {
    const uint32_t sp = x.cpu.r[CPU::kSP];
    const uint32_t actual = x.mem.read<uint32_t>(sp + 20);
    if (actual) for (uint32_t i = 0; i < 64; i += 4) x.mem.write<uint32_t>(actual + i, 0);   // AudioTimeStamp: "now"
    x.cpu.r[0] = uint32_t(aq_enqueueBuffer(x, x.cpu.r[0], x.cpu.r[1], x.cpu.r[2], x.cpu.r[3]));
}
int32_t aq_addPropertyListener(Ctx&, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
int32_t aq_removePropertyListener(Ctx&, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
int32_t aq_getProperty(Ctx& x, uint32_t qid, uint32_t prop, GuestPtr outData, GuestPtr ioSize) {
    if (outData && ioSize) {
        const uint32_t sz = x.mem.read<uint32_t>(ioSize);
        if (prop == 0x61717275 /* 'aqru' kAudioQueueProperty_IsRunning */ && sz >= 4) {
            bool running = false;
            auto& st = ATS(x);
            if (st.queues.count(qid)) running = st.queues[qid].running;
            x.mem.write<uint32_t>(outData, running ? 1 : 0);
        } else if (sz >= 4) {
            x.mem.write<uint32_t>(outData, 0);
        }
    }
    return 0;
}
int32_t aq_getPropertySize(Ctx& x, uint32_t, uint32_t, GuestPtr outSize) {
    if (outSize) x.mem.write<uint32_t>(outSize, 4);
    return 0;
}
// Recording: AudioQueueNewInput(format, callback, userData, runLoop, mode, flags, outAQ). The
// callback is AudioQueueInputCallback(userData, queue, buffer, const AudioTimeStamp*, numPackets,
// packetDescs); buffers are filled with linear PCM from the host microphone in real time.
int32_t aq_newInput(Ctx& x, GuestPtr format, uint32_t callbackProc, uint32_t userData, uint32_t runLoop, uint32_t runLoopMode, uint32_t flags, GuestPtr outAQ) {
    const int32_t r = aq_newOutput(x, format, callbackProc, userData, runLoop, runLoopMode, flags, outAQ);
    if (r != 0 || !outAQ) return r;
    auto& q = ATS(x).queues[x.mem.read<uint32_t>(outAQ)];
    MX(x.proc).release(q.voice);
    q.voice = 0;
    q.input = true;
    q.timeStamp = x.proc.heap().allocZeroed(64);
    return 0;
}
void aqPumpInput(Process& p, CPU::Cpu& c, uint32_t qid) {
    auto& st = p.service<AudioToolboxState>();
    auto it = st.queues.find(qid);
    if (it == st.queues.end() || !it->second.running) return;
    AudioQueueInstance* qp = &it->second;
#define q (*qp)
    const auto now = std::chrono::steady_clock::now();
    q.dueFrames += std::chrono::duration<double>(now - q.last).count() * q.sampleRate;
    q.last = now;
    const int bps = std::max(1, q.bitsPerChannel / 8), ch = std::max(1, q.channels);
    const uint32_t bytesPerFrame = uint32_t(bps * (q.nonInterleaved ? 1 : ch));
    // Never more than two buffers in one go (after a stall, catch up gently).
    for (int filled = 0; filled < 2 && !q.pending.empty(); ++filled) {
        const uint32_t buf = q.pending.front();
        const uint32_t cap = p.mem().read<uint32_t>(buf);
        const uint32_t data = p.mem().read<uint32_t>(buf + 4);
        const uint32_t frames = bytesPerFrame ? cap / bytesPerFrame : 0;
        if (!frames || !data || q.dueFrames < frames) break;
        std::vector<float> mono(frames);
        p.pullMicrophone(mono.data(), frames, q.sampleRate);
        for (uint32_t f = 0; f < frames; ++f) {
            const float v = std::clamp(mono[f], -1.0f, 1.0f);
            for (int k = 0; k < (q.nonInterleaved ? 1 : ch); ++k) {
                const uint32_t a = data + (f * uint32_t(q.nonInterleaved ? 1 : ch) + uint32_t(k)) * uint32_t(bps);
                if (q.isFloat && bps == 4) p.mem().write<float>(a, v);
                else if (bps == 4) p.mem().write<int32_t>(a, int32_t(v * 2147483647.0f));
                else if (bps == 2) p.mem().write<int16_t>(a, int16_t(std::lrint(v * 32767.0f)));
                else p.mem().write<uint8_t>(a, uint8_t(std::lrint(v * 127.0f) + 128));
            }
        }
        p.mem().write<uint32_t>(buf + 8, frames * bytesPerFrame);   // mAudioDataByteSize
        p.mem().write<double>(q.timeStamp, q.sampleTime);         // mSampleTime
        p.mem().write<uint32_t>(q.timeStamp + 56, 1);             // kAudioTimeStampSampleTimeValid
        q.sampleTime += frames;
        q.dueFrames -= frames;
        q.pending.pop_front();
        const uint32_t cb = q.callback, ud = q.userData, ts = q.timeStamp;
        if (p.terminating() || p.stopping() || !cb) break;
        p.callGuest(c, cb, {ud, qid, buf, ts, frames, 0});
        it = st.queues.find(qid);          // the callback may have stopped or disposed the queue
        if (it == st.queues.end() || !it->second.running) return;
        qp = &it->second;
    }
    if (q.pending.empty()) q.dueFrames = std::min(q.dueFrames, double(q.sampleRate) / 10);   // no buffer waiting: don't bank time
}
#undef q
int32_t aq_setProperty(Ctx&, uint32_t, uint32_t, GuestPtr, uint32_t) { return 0; }   // channel layout, converter quality, ...
int32_t aq_createTimeline(Ctx& x, uint32_t, GuestPtr out) { if (out) x.mem.write<uint32_t>(out, 0x71740001); return 0; }
int32_t aq_disposeTimeline(Ctx&, uint32_t, uint32_t) { return 0; }
// AudioQueueGetCurrentTime(q, timeline, AudioTimeStamp* outTime, Boolean* discontinuity): mSampleTime from the mixer.
int32_t aq_getCurrentTime(Ctx& x, uint32_t qid, uint32_t, GuestPtr outTime, GuestPtr disc) {
    if (outTime) {
        for (uint32_t i = 0; i < 64; i += 4) x.mem.write<uint32_t>(outTime + i, 0);
        double t = 0;
        auto& st = ATS(x);
        if (st.queues.count(qid))
            t = st.queues[qid].input ? st.queues[qid].sampleTime : MX(x.proc).positionSeconds(st.queues[qid].voice) * 44100.0;
        x.mem.write<double>(outTime, t);              // mSampleTime
        x.mem.write<uint32_t>(outTime + 56, 1);       // mFlags: kAudioTimeStampSampleTimeValid
    }
    if (disc) x.mem.write<uint8_t>(disc, 0);
    return 0;
}
int32_t aq_enqueueBuffer(Ctx& x, uint32_t qid, GuestPtr buf, uint32_t, GuestPtr) {
    auto& st = ATS(x);
    auto it = st.queues.find(qid);
    if (it == st.queues.end() || !buf) return -50;
    AudioQueueInstance& q = it->second;
    if (q.input) {   // an empty buffer for the recorder to fill
        q.pending.push_back(buf);
        if (!q.pump) {
            Process* p = &x.proc;
            q.pump = Foundation::mainRunLoop(x.proc).addTimer(0.01, 0.01, true, [p, qid](CPU::Cpu& c) { aqPump(*p, c, qid); });
        }
        return 0;
    }
    const uint32_t dataPtr = x.mem.read<uint32_t>(buf + 4);
    const uint32_t byteSize = x.mem.read<uint32_t>(buf + 8);
    audioTrace("AudioQueueEnqueueBuffer", qid, byteSize);
    std::vector<int16_t> stereo;
    if (dataPtr && byteSize > 0) {
        const int bps = std::max(1, q.bitsPerChannel / 8), ch = std::max(1, q.channels);
        const size_t frames = byteSize / size_t(bps * (q.nonInterleaved ? 1 : ch));
        stereo.resize(frames * 2);
        auto sample = [&](size_t frame, int c) -> int16_t {
            const size_t idx = q.nonInterleaved ? frame : frame * size_t(ch) + size_t(std::min(c, ch - 1));
            const uint32_t a = dataPtr + uint32_t(idx * size_t(bps));
            if (q.isFloat && bps == 4) return int16_t(std::clamp(x.mem.read<float>(a) * 32767.0f, -32768.0f, 32767.0f));
            if (bps == 4) return int16_t(x.mem.read<int32_t>(a) >> 16);
            if (bps == 2) return x.mem.read<int16_t>(a);
            return int16_t((int(x.mem.read<uint8_t>(a)) - 128) * 256);
        };
        for (size_t f = 0; f < frames; ++f) { stereo[f * 2] = sample(f, 0); stereo[f * 2 + 1] = sample(f, 1); }
    }
    MX(x.proc).enqueue(q.voice, sharePcm(stereo), q.sampleRate, buf);
    if (!q.pump) {
        Process* p = &x.proc;
        q.pump = Foundation::mainRunLoop(x.proc).addTimer(0.01, 0.01, true, [p, qid](CPU::Cpu& c) { aqPump(*p, c, qid); });
    }
    return 0;
}
void micRelease(Ctx& x, AudioQueueInstance& q) {
    if (q.micClaimed) { q.micClaimed = false; x.proc.microphoneUser(false); }
}
int32_t aq_start(Ctx& x, uint32_t qid, GuestPtr) {
    audioTrace("AudioQueueStart", qid);
    if (!ATS(x).queues.count(qid)) return 0;
    auto& q = ATS(x).queues[qid];
    q.running = true;
    if (q.input) {
        q.last = std::chrono::steady_clock::now();
        q.dueFrames = 0;
        if (!q.micClaimed) { q.micClaimed = true; x.proc.microphoneUser(true); }
        if (!q.pump) {
            Process* p = &x.proc;
            q.pump = Foundation::mainRunLoop(x.proc).addTimer(0.01, 0.01, true, [p, qid](CPU::Cpu& c) { aqPump(*p, c, qid); });
        }
        return 0;
    }
    MX(x.proc).setPaused(q.voice, false);
    return 0;
}
int32_t aq_pause(Ctx& x, uint32_t qid) {
    if (!ATS(x).queues.count(qid)) return 0;
    auto& q = ATS(x).queues[qid];
    q.running = false;
    if (q.input) { micRelease(x, q); return 0; }
    MX(x.proc).setPaused(q.voice, true);
    return 0;
}
int32_t aq_stop(Ctx& x, uint32_t qid, bool) {
    if (!ATS(x).queues.count(qid)) return 0;
    auto& q = ATS(x).queues[qid];
    q.running = false;
    if (q.input) { micRelease(x, q); q.pending.clear(); return 0; }
    MX(x.proc).stop(q.voice);
    MX(x.proc).takeProcessed(q.voice);   // stopped buffers are not called back
    MX(x.proc).setPaused(q.voice, true);
    return 0;
}
int32_t aq_flush(Ctx&, uint32_t) { return 0; }
int32_t aq_reset(Ctx& x, uint32_t qid) {
    if (!ATS(x).queues.count(qid)) return 0;
    auto& q = ATS(x).queues[qid];
    if (q.input) q.pending.clear();
    else MX(x.proc).stop(q.voice);
    return 0;
}
int32_t aq_dispose(Ctx& x, uint32_t qid, bool) {
    auto& st = ATS(x);
    if (st.queues.count(qid)) {
        auto& q = st.queues[qid];
        if (q.pump) q.pump->valid = false;
        micRelease(x, q);
        if (q.timeStamp) x.proc.heap().free(q.timeStamp);
        if (q.voice) MX(x.proc).release(q.voice);
        for (const auto& b : q.buffers) if (b.guestAddr) x.proc.heap().free(b.guestAddr);
        st.queues.erase(qid);
    }
    return 0;
}
int32_t aq_setParam(Ctx& x, uint32_t qid, uint32_t param, float val) {
    if (ATS(x).queues.count(qid) && param == 1 /* kAudioQueueParam_Volume */) {
        auto& q = ATS(x).queues[qid];
        q.volume = val;
        MX(x.proc).setParams(q.voice, {val, 0, 1, false});
    }
    return 0;
}
int32_t aq_getParam(Ctx& x, uint32_t qid, uint32_t, GuestPtr outVal) {
    if (outVal) x.mem.write<float>(outVal, ATS(x).queues.count(qid) ? ATS(x).queues[qid].volume : 1.0f);
    return 0;
}

// Offline rendering isn't supported (from PR #114, Minecraft: Story Mode imports these).
int32_t aq_offlineRender(Ctx&, uint32_t, GuestPtr, GuestPtr, uint32_t) { return -50; }
int32_t aq_setOfflineFormat(Ctx&, uint32_t, GuestPtr, GuestPtr) { return -50; }

// AudioFile & AudioUnit stubs
int32_t af_openURL(Ctx& x, uint32_t urlRef, int32_t, uint32_t, GuestPtr outFile) {
    if (!urlRef || !outFile) return -50;
    const std::string path = Foundation::urlFilePath(x, urlRef);
    std::ifstream in(path, std::ios::binary);
    if (!in) return -43;
    auto& st = ATS(x);
    const uint32_t fid = st.nextFileId++;
    AudioFileInstance af;
    af.id = fid;
    af.rawBytes.assign((std::istreambuf_iterator<char>(in)), {});
    af.decoded = decodeAudioBytes(af.rawBytes.data(), af.rawBytes.size());
    if (af.decoded.stereoPcm.empty()) { x.mem.write<uint32_t>(outFile, 0); return 1954115647; }
    st.files[fid] = std::move(af);
    x.mem.write<uint32_t>(outFile, fid);
    return 0;
}
int32_t af_getPropertyInfo(Ctx& x, uint32_t, uint32_t prop, GuestPtr outSize, GuestPtr outWritable) {
    if (outWritable) x.mem.write<uint32_t>(outWritable, 0);
    if (outSize) {
        uint32_t sz = 8;
        if (prop == 0x64666D74 /* 'dfmt' */) sz = 40;
        x.mem.write<uint32_t>(outSize, sz);
    }
    return 0;
}
// AudioFile presents 16-bit LPCM in the source's own channel count (mono files
// stay mono): apps such as Talking John size their read buffers from the file's
// real layout, and a stereo view overran them by 2x.
uint32_t afChannels(const AudioFileInstance& af) { return af.decoded.channels == 1 ? 1 : 2; }
// Copies frames [first, first+n) of the decoded stereo PCM to guest memory in the file's layout.
void afCopyFrames(Ctx& x, const AudioFileInstance& af, uint64_t first, uint64_t n, GuestPtr out) {
    const auto& pcm = af.decoded.stereoPcm;
    if (afChannels(af) == 2) { x.mem.copyIn(out, reinterpret_cast<const uint8_t*>(pcm.data() + first * 2), size_t(n * 4)); return; }
    std::vector<int16_t> mono(n);
    for (uint64_t i = 0; i < n; ++i) mono[i] = pcm[(first + i) * 2];
    x.mem.copyIn(out, reinterpret_cast<const uint8_t*>(mono.data()), size_t(n * 2));
}
int32_t af_getProperty(Ctx& x, uint32_t fid, uint32_t prop, GuestPtr ioSize, GuestPtr outData) {
    auto& st = ATS(x);
    if (!st.files.count(fid) || !outData || !ioSize) return -50;
    const AudioFileInstance& af = st.files[fid];
    const uint32_t cap = x.mem.read<uint32_t>(ioSize);
    if (prop == 0x64666D74 /* 'dfmt' AudioStreamBasicDescription */ && cap >= 40) {
        x.mem.write<double>(outData + 0, double(af.decoded.sampleRate));
        x.mem.write<uint32_t>(outData + 8, 0x6C70636D /* 'lpcm' */);
        x.mem.write<uint32_t>(outData + 12, 0x0C /* signed int | packed */);
        x.mem.write<uint32_t>(outData + 16, 2 * afChannels(af));
        x.mem.write<uint32_t>(outData + 20, 1);
        x.mem.write<uint32_t>(outData + 24, 2 * afChannels(af));
        x.mem.write<uint32_t>(outData + 28, afChannels(af));
        x.mem.write<uint32_t>(outData + 32, 16);
        x.mem.write<uint32_t>(outData + 36, 0);
    } else if (prop == 0x62636E74 /* 'bcnt' byte count */ && cap >= 8) {
        x.mem.write<uint64_t>(outData, uint64_t(af.decoded.stereoPcm.size() / 2) * 2 * afChannels(af));
    } else if (prop == 0x65647572 /* 'edur' estimated duration */ && cap >= 8) {
        x.mem.write<double>(outData, af.decoded.duration);
    } else if (prop == 0x70636E74 /* 'pcnt' audio data packet count */ && cap >= 8) {
        x.mem.write<uint64_t>(outData, uint64_t(af.decoded.stereoPcm.size() / 2));
    } else if ((prop == 0x70737A65 /* 'psze' */ || prop == 0x706B7562 /* 'pkub' */) && cap >= 4) {
        x.mem.write<uint32_t>(outData, 2 * afChannels(af));
    } else if (prop == 0x66666D74 /* 'ffmt' file format */ && cap >= 4) {
        x.mem.write<uint32_t>(outData, 0x63616666 /* 'caff' */);
    }
    return 0;
}
int32_t af_readBytes(Ctx& x, uint32_t fid, bool, int64_t inStartingByte, GuestPtr ioNumBytes, GuestPtr outBuffer) {
    auto& st = ATS(x);
    if (!st.files.count(fid) || !ioNumBytes || !outBuffer) return -50;
    const AudioFileInstance& af = st.files[fid];
    const uint32_t bpf = 2 * afChannels(af);
    const uint64_t frames = af.decoded.stereoPcm.size() / 2;
    const uint32_t req = x.mem.read<uint32_t>(ioNumBytes);
    const uint64_t first = std::min<uint64_t>(uint64_t(std::max<int64_t>(0, inStartingByte)) / bpf, frames);
    const uint64_t n = std::min<uint64_t>(req / bpf, frames - first);
    if (n) afCopyFrames(x, af, first, n, outBuffer);
    x.mem.write<uint32_t>(ioNumBytes, uint32_t(n * bpf));
    return 0;
}
int32_t af_close(Ctx& x, uint32_t fid) { ATS(x).files.erase(fid); return 0; }

// AudioFileOpenWithCallbacks(clientData, read, write, getSize, setSize, typeHint, outFile):
// pull the whole file through the app's callbacks, then decode it like AudioFileOpenURL.
//   getSize(clientData) -> SInt64
//   read(clientData, SInt64 position, UInt32 count, void* buffer, UInt32* actual) -> OSStatus
// (32-bit iOS passes the SInt64 position in r1:r2, no register pairing.)
void af_openWithCallbacks(Ctx& x) {
    const uint32_t client = x.cpu.r[0], readFn = x.cpu.r[1], getSizeFn = x.cpu.r[3];
    const uint32_t sp = x.cpu.r[CPU::kSP];
    const uint32_t outFile = x.mem.read<uint32_t>(sp + 8);
    if (!readFn || !getSizeFn || !outFile) { x.cpu.r[0] = uint32_t(-50); return; }
    const int64_t size = int64_t(x.proc.callGuest(x.cpu, getSizeFn, {client}));
    if (size <= 0 || size > (256ll << 20)) { x.cpu.r[0] = uint32_t(1954115647); return; }   // 'typ?'
    const uint32_t chunk = 64u << 10;
    const uint32_t buf = x.proc.heap().alloc(chunk), actual = x.proc.heap().allocZeroed(4);
    std::vector<uint8_t> bytes;
    bytes.reserve(size_t(size));
    while (int64_t(bytes.size()) < size) {
        const uint64_t pos = bytes.size();
        const uint32_t want = uint32_t(std::min<int64_t>(chunk, size - int64_t(pos)));
        x.mem.write<uint32_t>(actual, 0);
        const int32_t rc = int32_t(uint32_t(x.proc.callGuest(x.cpu, readFn, {client, uint32_t(pos), uint32_t(pos >> 32), want, buf, actual})));
        const uint32_t got = x.mem.read<uint32_t>(actual);
        if (got) { const uint8_t* p = x.mem.host(buf); bytes.insert(bytes.end(), p, p + got); }
        if (rc != 0 || got == 0) break;
    }
    x.proc.heap().free(buf);
    x.proc.heap().free(actual);
    auto& st = ATS(x);
    AudioFileInstance af;
    af.id = st.nextFileId++;
    af.rawBytes = std::move(bytes);
    af.decoded = decodeAudioBytes(af.rawBytes.data(), af.rawBytes.size());
    if (af.decoded.stereoPcm.empty()) { x.cpu.r[0] = uint32_t(1954115647); return; }
    x.mem.write<uint32_t>(outFile, af.id);
    st.files[af.id] = std::move(af);
    x.cpu.r[0] = 0;
}
// AudioFileReadPacketData / AudioFileReadPackets(fid, useCache, ioNumBytes*, packetDescs, SInt64 startPacket,
// ioNumPackets*, outBuffer). Our files present 16-bit stereo LPCM: one packet = one 4-byte frame.
void af_readPackets(Ctx& x, bool numBytesIsInOut) {
    const uint32_t fid = x.cpu.r[0], ioNumBytes = x.cpu.r[2];
    const uint32_t sp = x.cpu.r[CPU::kSP];
    const int64_t start = int64_t((uint64_t(x.mem.read<uint32_t>(sp + 4)) << 32) | x.mem.read<uint32_t>(sp));
    const uint32_t ioNumPackets = x.mem.read<uint32_t>(sp + 8), out = x.mem.read<uint32_t>(sp + 12);
    auto& st = ATS(x);
    if (!st.files.count(fid) || !ioNumPackets || !out) { x.cpu.r[0] = uint32_t(-50); return; }
    const AudioFileInstance& af = st.files[fid];
    const uint32_t bpf = 2 * afChannels(af);
    const uint64_t frames = af.decoded.stereoPcm.size() / 2;
    uint64_t want = x.mem.read<uint32_t>(ioNumPackets);
    if (numBytesIsInOut && ioNumBytes) want = std::min<uint64_t>(want, x.mem.read<uint32_t>(ioNumBytes) / bpf);
    const uint64_t first = uint64_t(std::max<int64_t>(0, start));
    const uint64_t n = first >= frames ? 0 : std::min<uint64_t>(want, frames - first);
    if (n) afCopyFrames(x, af, first, n, out);
    x.mem.write<uint32_t>(ioNumPackets, uint32_t(n));
    if (ioNumBytes) x.mem.write<uint32_t>(ioNumBytes, uint32_t(n * bpf));
    x.cpu.r[0] = 0;
}

// ------------------------------------------------------------- ExtAudioFile ---
// Reads decoded audio in the app's "client" format: linear PCM of any common
// layout (int16, float32, 8.24 fixed, 8-bit; interleaved or not; mono/stereo),
// resampled linearly when the client rate differs.

struct Asbd {
    double sampleRate = 44100;
    uint32_t formatID = 0x6C70636D, flags = 0x0C, bytesPerPacket = 4, framesPerPacket = 1, bytesPerFrame = 4, channels = 2, bits = 16;
};
Asbd readAsbd(Ctx& x, GuestPtr p) {
    Asbd a;
    a.sampleRate = x.mem.read<double>(p);
    a.formatID = x.mem.read<uint32_t>(p + 8);
    a.flags = x.mem.read<uint32_t>(p + 12);
    a.bytesPerPacket = x.mem.read<uint32_t>(p + 16);
    a.framesPerPacket = x.mem.read<uint32_t>(p + 20);
    a.bytesPerFrame = x.mem.read<uint32_t>(p + 24);
    a.channels = x.mem.read<uint32_t>(p + 28);
    a.bits = x.mem.read<uint32_t>(p + 32);
    return a;
}
void writeAsbd(Ctx& x, GuestPtr p, const Asbd& a) {
    x.mem.write<double>(p, a.sampleRate);
    x.mem.write<uint32_t>(p + 8, a.formatID);
    x.mem.write<uint32_t>(p + 12, a.flags);
    x.mem.write<uint32_t>(p + 16, a.bytesPerPacket);
    x.mem.write<uint32_t>(p + 20, a.framesPerPacket);
    x.mem.write<uint32_t>(p + 24, a.bytesPerFrame);
    x.mem.write<uint32_t>(p + 28, a.channels);
    x.mem.write<uint32_t>(p + 32, a.bits);
    x.mem.write<uint32_t>(p + 36, 0);
}

struct ExtFile {
    DecodedAudio audio;
    Asbd file, client;
    double pos = 0;   // in source frames
};
struct ExtAudioState { explicit ExtAudioState(Process&) {} uint32_t next = 0x45580001; std::unordered_map<uint32_t, ExtFile> files; };
ExtAudioState& EAS(Ctx& x) { return x.proc.service<ExtAudioState>(); }

Asbd fileFormatOf(const DecodedAudio& d) {
    Asbd a;
    a.sampleRate = d.sampleRate;
    a.channels = uint32_t(std::clamp(d.channels, 1, 2));
    a.bits = 16;
    a.flags = 0x0C;   // signed integer, packed
    a.bytesPerFrame = a.bytesPerPacket = 2 * a.channels;
    return a;
}

uint32_t newExtFile(Ctx& x, DecodedAudio audio) {
    ExtFile f;
    f.audio = std::move(audio);
    f.file = f.client = fileFormatOf(f.audio);
    auto& st = EAS(x);
    const uint32_t id = st.next++;
    st.files[id] = std::move(f);
    return id;
}

int32_t eaf_openURL(Ctx& x, uint32_t url, GuestPtr out) {
    if (!url || !out) return -50;
    const std::string path = Foundation::urlFilePath(x, url);
    std::ifstream in(path, std::ios::binary);
    if (!in) return -43;   // fnfErr
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
    DecodedAudio d = decodeAudioBytes(bytes.data(), bytes.size());
    if (d.stereoPcm.empty()) {
        x.proc.reportMissing("ExtAudioFile: audio format of " + path.substr(path.find_last_of('/') + 1) + " (needs a decoder; plays silence)");
        d.stereoPcm.assign(size_t(std::max(1.0, d.duration * d.sampleRate)) * 2, 0);
    }
    x.mem.write<uint32_t>(out, newExtFile(x, std::move(d)));
    return 0;
}
int32_t eaf_wrap(Ctx& x, uint32_t afid, bool, GuestPtr out) {
    auto& st = ATS(x);
    if (!st.files.count(afid) || !out) return -50;
    x.mem.write<uint32_t>(out, newExtFile(x, st.files[afid].decoded));
    return 0;
}
int32_t eaf_dispose(Ctx& x, uint32_t id) { EAS(x).files.erase(id); return 0; }

constexpr uint32_t kEafFileFormat = 0x66666D74 /* ffmt */, kEafClientFormat = 0x63666D74 /* cfmt */,
                   kEafLengthFrames = 0x2366726D /* #frm */, kEafAudioFile = 0x6166696C /* afil */;

int32_t eaf_getPropertyInfo(Ctx& x, uint32_t, uint32_t prop, GuestPtr outSize, GuestPtr outWritable) {
    if (outSize) x.mem.write<uint32_t>(outSize, prop == kEafLengthFrames ? 8 : prop == kEafAudioFile ? 4 : 40);
    if (outWritable) x.mem.write<uint8_t>(outWritable, prop == kEafClientFormat ? 1 : 0);
    return 0;
}
int32_t eaf_getProperty(Ctx& x, uint32_t id, uint32_t prop, GuestPtr ioSize, GuestPtr out) {
    auto it = EAS(x).files.find(id);
    if (it == EAS(x).files.end() || !out) return -50;
    const ExtFile& f = it->second;
    if (prop == kEafFileFormat || prop == kEafClientFormat) {
        writeAsbd(x, out, prop == kEafFileFormat ? f.file : f.client);
        if (ioSize) x.mem.write<uint32_t>(ioSize, 40);
    } else if (prop == kEafLengthFrames) {
        x.mem.write<uint64_t>(out, uint64_t(f.audio.stereoPcm.size() / 2));
        if (ioSize) x.mem.write<uint32_t>(ioSize, 8);
    } else if (prop == kEafAudioFile) {
        x.mem.write<uint32_t>(out, 0);
        if (ioSize) x.mem.write<uint32_t>(ioSize, 4);
    } else {
        return 0x77686174;   // 'what': kAudioFileUnsupportedPropertyError-ish
    }
    return 0;
}
int32_t eaf_setProperty(Ctx& x, uint32_t id, uint32_t prop, uint32_t, GuestPtr data) {
    auto it = EAS(x).files.find(id);
    if (it == EAS(x).files.end() || !data) return -50;
    if (prop == kEafClientFormat) {
        Asbd c = readAsbd(x, data);
        if (c.formatID != 0x6C70636D) return 0x666D743F;   // 'fmt?'
        c.channels = std::clamp<uint32_t>(c.channels, 1, 8);
        if (!c.bits) c.bits = 16;
        if (c.sampleRate <= 0) c.sampleRate = it->second.file.sampleRate;
        it->second.client = c;
    }
    return 0;
}

int32_t eaf_read(Ctx& x, uint32_t id, GuestPtr ioFrames, GuestPtr abl) {
    auto it = EAS(x).files.find(id);
    if (it == EAS(x).files.end() || !ioFrames || !abl) return -50;
    ExtFile& f = it->second;
    const Asbd& c = f.client;
    const bool isFloat = c.flags & 1, nonInterleaved = c.flags & 0x20;
    const uint32_t bps = std::max<uint32_t>(1, c.bits / 8);
    const uint32_t fracBits = (c.flags >> 7) & 0x3F;     // kLinearPCMFormatFlagsSampleFraction (8.24 fixed)
    const uint32_t nbuf = x.mem.read<uint32_t>(abl);
    const uint32_t chPerBuf = nonInterleaved ? 1 : c.channels;
    // Frames that fit in the smallest buffer.
    uint32_t want = x.mem.read<uint32_t>(ioFrames);
    for (uint32_t b = 0; b < nbuf; ++b) {
        const uint32_t cap = x.mem.read<uint32_t>(abl + 4 + b * 12 + 4);
        want = std::min(want, cap / std::max<uint32_t>(1, bps * chPerBuf));
    }
    const size_t srcFrames = f.audio.stereoPcm.size() / 2;
    const double step = double(f.audio.sampleRate) / std::max(1.0, c.sampleRate);
    uint32_t done = 0;
    auto put = [&](uint32_t b, uint32_t index, float v) {
        const uint32_t base = x.mem.read<uint32_t>(abl + 4 + b * 12 + 8);
        const uint32_t a = base + index * bps;
        if (isFloat && bps == 4) x.mem.write<float>(a, v);
        else if (bps == 4) x.mem.write<int32_t>(a, int32_t(std::clamp(double(v), -1.0, 0.9999999) * double(fracBits ? (1u << fracBits) : 2147483648.0)));
        else if (bps == 2) x.mem.write<int16_t>(a, int16_t(std::clamp(v * 32767.0f, -32768.0f, 32767.0f)));
        else if (bps == 1) x.mem.write<uint8_t>(a, uint8_t(std::clamp(v * 127.0f + 128.0f, 0.0f, 255.0f)));
    };
    for (; done < want; ++done) {
        const double sp = f.pos + double(done) * step;
        const size_t i0 = size_t(sp);
        if (i0 >= srcFrames) break;
        const size_t i1 = std::min(i0 + 1, srcFrames - 1);
        const float t = float(sp - double(i0));
        float lr[2];
        for (int ch = 0; ch < 2; ++ch)
            lr[ch] = (float(f.audio.stereoPcm[i0 * 2 + ch]) * (1 - t) + float(f.audio.stereoPcm[i1 * 2 + ch]) * t) / 32768.0f;
        for (uint32_t ch = 0; ch < c.channels; ++ch) {
            const float v = c.channels == 1 ? (lr[0] + lr[1]) * 0.5f : lr[std::min<uint32_t>(ch, 1)];
            if (nonInterleaved) { if (ch < nbuf) put(ch, done, v); }
            else put(0, done * c.channels + ch, v);
        }
    }
    f.pos += double(done) * step;
    for (uint32_t b = 0; b < nbuf; ++b) x.mem.write<uint32_t>(abl + 4 + b * 12 + 4, done * bps * chPerBuf);
    x.mem.write<uint32_t>(ioFrames, done);
    return 0;
}
// ExtAudioFileSeek(ref, SInt64 frame): frame in r1:r2 (iOS passes 64-bit arguments without register pairing).
void eaf_seek(Ctx& x) {
    auto it = EAS(x).files.find(x.cpu.r[0]);
    if (it == EAS(x).files.end()) { x.cpu.r[0] = uint32_t(-50); return; }
    const int64_t frame = int64_t((uint64_t(x.cpu.r[2]) << 32) | x.cpu.r[1]);
    it->second.pos = double(std::max<int64_t>(0, frame)) * double(it->second.audio.sampleRate) / std::max(1.0, it->second.client.sampleRate);
    x.cpu.r[0] = 0;
}
int32_t eaf_tell(Ctx& x, uint32_t id, GuestPtr out) {
    auto it = EAS(x).files.find(id);
    if (it == EAS(x).files.end() || !out) return -50;
    x.mem.write<uint64_t>(out, uint64_t(it->second.pos * std::max(1.0, it->second.client.sampleRate) / double(it->second.audio.sampleRate)));
    return 0;
}

uint32_t ac_findNext(Ctx&, uint32_t, GuestPtr) { return 1; }
int32_t ac_instanceNew(Ctx& x, uint32_t, GuestPtr outUnit) {
    if (!outUnit) return -50;
    auto& st = ATS(x);
    std::lock_guard<std::recursive_mutex> lk(st.unitsMu);
    const uint32_t uid = st.nextUnitId++;
    st.units[uid].id = uid;
    x.mem.write<uint32_t>(outUnit, uid);
    return 0;
}
void auReleaseBuffers(Ctx& x, AudioUnitInstance& u) {
    if (u.bufferList) x.proc.heap().free(u.bufferList);
    if (u.bufferStorage) x.proc.heap().free(u.bufferStorage);
    u.bufferList = u.bufferStorage = u.bufferBytes = 0;
}
void auRelease(Ctx& x, AudioUnitInstance& u) {
    u.running = false;
    if (u.voice) MX(x.proc).release(u.voice);
    u.voice = 0;
    auReleaseBuffers(x, u);
    if (u.actionFlags) x.proc.heap().free(u.actionFlags);
    if (u.timeStamp) x.proc.heap().free(u.timeStamp);
    u.actionFlags = u.timeStamp = 0;
}
int32_t ac_dispose(Ctx& x, uint32_t uid) {
    auto& st = ATS(x);
    std::lock_guard<std::recursive_mutex> lk(st.unitsMu);
    auto it = st.units.find(uid);
    if (it == st.units.end()) return -50;
    auRelease(x, it->second);
    st.units.erase(it);
    return 0;
}
int32_t au_init(Ctx& x, uint32_t uid) {
    auto& st = ATS(x);
    std::lock_guard<std::recursive_mutex> lk(st.unitsMu);
    auto it = st.units.find(uid);
    if (it == st.units.end()) return -50;
    it->second.initialized = true;
    return 0;
}
int32_t au_stop(Ctx& x, uint32_t uid) {
    audioTrace("AudioOutputUnitStop", uid);
    auto& st = ATS(x);
    std::lock_guard<std::recursive_mutex> lk(st.unitsMu);
    auto it = st.units.find(uid);
    if (it == st.units.end()) return -50;
    it->second.running = false;
    if (it->second.voice) MX(x.proc).setPaused(it->second.voice, true);
    return 0;
}
int32_t au_uninit(Ctx& x, uint32_t uid) {
    auto& st = ATS(x);
    std::lock_guard<std::recursive_mutex> lk(st.unitsMu);
    auto it = st.units.find(uid);
    if (it == st.units.end()) return -50;
    au_stop(x, uid);
    it->second.initialized = false;
    return 0;
}
bool auPrepareBuffers(Ctx& x, AudioUnitInstance& u, uint32_t frames) {
    const bool nonInterleaved = (u.formatFlags & 32) != 0;
    const uint32_t bps = u.bits / 8, nbuf = nonInterleaved ? u.channels : 1;
    const uint32_t bytesPerBuffer = frames * (nonInterleaved ? bps : u.bytesPerFrame);
    const uint32_t total = bytesPerBuffer * nbuf;
    if (!bps || !nbuf || !bytesPerBuffer || total / nbuf != bytesPerBuffer) return false;
    if (!u.bufferList) u.bufferList = x.proc.heap().allocZeroed(4 + nbuf * 12);
    if (total > u.bufferBytes) {
        if (u.bufferStorage) x.proc.heap().free(u.bufferStorage);
        u.bufferStorage = x.proc.heap().alloc(total);
        u.bufferBytes = total;
    }
    if (!u.actionFlags) u.actionFlags = x.proc.heap().allocZeroed(4);
    if (!u.timeStamp) u.timeStamp = x.proc.heap().allocZeroed(64);
    std::memset(x.mem.host(u.bufferStorage), 0, total);
    x.mem.write<uint32_t>(u.bufferList, nbuf);
    for (uint32_t b = 0; b < nbuf; ++b) {
        const uint32_t at = u.bufferList + 4 + b * 12;
        x.mem.write<uint32_t>(at, nonInterleaved ? 1 : u.channels);
        x.mem.write<uint32_t>(at + 4, bytesPerBuffer);
        x.mem.write<uint32_t>(at + 8, u.bufferStorage + b * bytesPerBuffer);
    }
    x.mem.write<uint32_t>(u.actionFlags, 0);
    x.mem.write<double>(u.timeStamp, u.sampleTime);
    x.mem.write<uint32_t>(u.timeStamp + 56, 1); // kAudioTimeStampSampleTimeValid
    return true;
}
void auPump(Process& p, CPU::Cpu& c, uint32_t uid) {
    auto& st = p.service<AudioToolboxState>();
    auto it = st.units.find(uid);
    if (it == st.units.end()) { audioTrace("AudioUnitPumpMissing", uid); return; }
    if (!it->second.running) { audioTrace("AudioUnitPumpStopped", uid); return; }
    if (!it->second.renderCallback) { audioTrace("AudioUnitPumpNoCallback", uid); return; }
    AudioUnitInstance& u = it->second;
    const uint32_t frames = std::clamp<uint32_t>(uint32_t(std::lround(u.sampleRate * 0.01)), 1, 4096);
    Ctx x{p, c, p.mem()};
    if (!auPrepareBuffers(x, u, frames)) { audioTrace("AudioUnitPumpBufferFailure", uid); return; }
    const uint32_t cb = u.renderCallback, ud = u.renderUserData;
    const uint32_t flags = u.actionFlags, ts = u.timeStamp, abl = u.bufferList;
    const bool first = u.sampleTime == 0;
    if (first) audioTrace("AudioUnitRenderBegin", uid, cb);
    const int32_t status = int32_t(uint32_t(p.callGuest(c, cb, {ud, flags, ts, 0, frames, abl})));
    if (first) audioTrace("AudioUnitRenderEnd", uid, uint32_t(status));
    it = st.units.find(uid); // the callback may stop, uninitialize or dispose the unit
    if (it == st.units.end()) { audioTrace("AudioUnitPumpDisposed", uid); return; }
    if (status != 0) { audioTrace("AudioUnitPumpCallbackError", uid, uint32_t(status)); return; }
    u = it->second;
    if (!u.running) { audioTrace("AudioUnitPumpStoppedByCallback", uid); return; }
    u.sampleTime += frames;
    const bool nonInterleaved = (u.formatFlags & 32) != 0;
    const uint32_t bps = u.bits / 8, expectedBuffers = nonInterleaved ? u.channels : 1;
    if (x.mem.read<uint32_t>(abl) != expectedBuffers) { audioTrace("AudioUnitPumpBufferCount", uid); return; }
    std::vector<uint32_t> data(expectedBuffers);
    for (uint32_t b = 0; b < expectedBuffers; ++b) {
        const uint32_t at = abl + 4 + b * 12;
        const uint32_t size = x.mem.read<uint32_t>(at + 4);
        data[b] = x.mem.read<uint32_t>(at + 8);
        const uint32_t need = frames * (nonInterleaved ? bps : u.bytesPerFrame);
        if (!data[b] || size < need) { audioTrace("AudioUnitPumpShortBuffer", uid, size); return; }
    }
    const bool isFloat = (u.formatFlags & 1) != 0, isSigned = (u.formatFlags & 4) != 0;
    const bool bigEndian = (u.formatFlags & 2) != 0;
    auto sample = [&](uint32_t frame, uint32_t channel) -> int16_t {
        const uint32_t b = nonInterleaved ? channel : 0;
        const uint32_t offset = nonInterleaved ? frame * bps : frame * u.bytesPerFrame + channel * bps;
        uint64_t raw = 0;
        for (uint32_t i = 0; i < bps; ++i) {
            const uint32_t j = bigEndian ? bps - 1 - i : i;
            raw |= uint64_t(x.mem.read<uint8_t>(data[b] + offset + j)) << (i * 8);
        }
        double v = 0;
        if (isFloat && bps == 4) {
            const uint32_t bits = uint32_t(raw);
            float f;
            std::memcpy(&f, &bits, sizeof f);
            v = f;
        } else if (isFloat && bps == 8) {
            double d;
            std::memcpy(&d, &raw, sizeof d);
            v = d;
        } else {
            const uint32_t sign = u.bits - 1;
            int64_t s = int64_t(raw);
            if (isSigned && (raw & (uint64_t(1) << sign))) s = int64_t(raw | (~uint64_t(0) << u.bits));
            if (!isSigned) s -= int64_t(uint64_t(1) << sign);
            v = double(s) / double(uint64_t(1) << sign);
        }
        return int16_t(std::clamp(v * 32767.0, -32768.0, 32767.0));
    };
    std::vector<int16_t> stereo(size_t(frames) * 2);
    for (uint32_t f = 0; f < frames; ++f) {
        stereo[f * 2] = sample(f, 0);
        stereo[f * 2 + 1] = sample(f, u.channels > 1 ? 1 : 0);
    }
    if (first || (uint64_t(u.sampleTime) / frames) % 100 == 1) {
        audioTrace("AudioUnitFormat", uid, (uint64_t(u.formatFlags) << 32) | u.channels);
        audioTrace("AudioUnitRate", uid, uint32_t(std::lround(u.sampleRate)));
        audioTrace("AudioUnitPcmNonzero", uid, uint64_t(std::count_if(stereo.begin(), stereo.end(), [](int16_t s) { return s != 0; })));
    }
    MX(p).enqueue(u.voice, sharePcm(stereo), int(std::lround(u.sampleRate)), u.nextTag++);
}
void au_thread(Ctx& x, uint32_t uid) {
    // RemoteIO has real-time deadlines independent of the app's main run loop.
    // A guest thread gives its callback a real CPU context without letting a
    // slow Unity/CADisplayLink frame starve music and sound effects.
    auto next = std::chrono::steady_clock::now();
    while (!x.proc.stopping()) {
        {
            auto& st = ATS(x);
            std::lock_guard<std::recursive_mutex> lk(st.unitsMu);
            auto it = st.units.find(uid);
            if (it == st.units.end()) return;
            if (it->second.running) auPump(x.proc, x.cpu, uid);
        }
        next += std::chrono::milliseconds(10);
        const auto now = std::chrono::steady_clock::now();
        if (next + std::chrono::milliseconds(50) < now) next = now;
        std::this_thread::sleep_until(next);
    }
}
int32_t au_start(Ctx& x, uint32_t uid) {
    audioTrace("AudioOutputUnitStart", uid);
    auto& st = ATS(x);
    std::lock_guard<std::recursive_mutex> lk(st.unitsMu);
    auto it = st.units.find(uid);
    if (it == st.units.end()) return -50;
    AudioUnitInstance& u = it->second;
    u.running = true;
    if (!u.voice) u.voice = MX(x.proc).createStream({});
    MX(x.proc).setPaused(u.voice, false);
    if (!u.pumpThread)
        u.pumpThread = x.proc.startThread(*x.proc.lookupSymbol("__hvm_audio_unit_pump"), uid, 256u << 10, true);
    return 0;
}
int32_t au_setProp(Ctx& x, uint32_t uid, uint32_t propId, uint32_t, uint32_t, GuestPtr data, uint32_t size) {
    auto& st = ATS(x);
    std::lock_guard<std::recursive_mutex> lk(st.unitsMu);
    auto it = st.units.find(uid);
    if (it == ATS(x).units.end() || !data) return -50;
    AudioUnitInstance& u = it->second;
    if (propId == 23 /* kAudioUnitProperty_SetRenderCallback */) {
        if (size < 8) return -50;
        u.renderCallback = x.mem.read<uint32_t>(data);
        u.renderUserData = x.mem.read<uint32_t>(data + 4);
    } else if (propId == 8 /* kAudioUnitProperty_StreamFormat */) {
        if (size < 40) return -50;
        const Asbd a = readAsbd(x, data);
        const uint32_t bps = a.bits / 8;
        if (a.formatID != 0x6C70636D || a.framesPerPacket != 1 || !(a.flags & 8) ||
            a.sampleRate < 1000 || a.sampleRate > 192000 || a.channels < 1 || a.channels > 8 ||
            !(a.bits == 8 || a.bits == 16 || a.bits == 24 || a.bits == 32 || ((a.flags & 1) && a.bits == 64)) ||
            !a.bytesPerFrame || a.bytesPerFrame < ((a.flags & 32) ? bps : bps * a.channels)) return 0x666D743F; // 'fmt?'
        u.sampleRate = a.sampleRate;
        u.formatFlags = a.flags;
        u.bytesPerFrame = a.bytesPerFrame;
        u.channels = a.channels;
        u.bits = a.bits;
        auReleaseBuffers(x, u);
    }
    return 0;
}
int32_t au_getProp(Ctx& x, uint32_t unit, uint32_t prop, uint32_t scope, uint32_t element, GuestPtr outData, GuestPtr ioDataSize) {
    auto& st = ATS(x);
    std::lock_guard<std::recursive_mutex> lk(st.unitsMu);
    auto it = st.units.find(unit);
    if (it == ATS(x).units.end()) return -50;
    if (!outData || !ioDataSize) return -50;
    const uint32_t size = x.mem.read<uint32_t>(ioDataSize);
    if (prop == 8 /* kAudioUnitProperty_StreamFormat */) {
        if (size >= 40) {
            const AudioUnitInstance& u = it->second;
            x.mem.write<double>(outData, u.sampleRate);
            x.mem.write<uint32_t>(outData + 8, 0x6c70636d /* 'lpcm' */);
            x.mem.write<uint32_t>(outData + 12, u.formatFlags);
            x.mem.write<uint32_t>(outData + 16, u.bytesPerFrame);
            x.mem.write<uint32_t>(outData + 20, 1);
            x.mem.write<uint32_t>(outData + 24, u.bytesPerFrame);
            x.mem.write<uint32_t>(outData + 28, u.channels);
            x.mem.write<uint32_t>(outData + 32, u.bits);
            x.mem.write<uint32_t>(outData + 36, 0);
            x.mem.write<uint32_t>(ioDataSize, 40);
        }
    } else if (prop == 14 /* kAudioUnitProperty_MaximumFramesPerSlice */) {
        if (size >= 4) {
            x.mem.write<uint32_t>(outData, 4096);
            x.mem.write<uint32_t>(ioDataSize, 4);
        }
    }
    return 0;
}
int32_t au_setParam(Ctx&, uint32_t, uint32_t, uint32_t, uint32_t, float, uint32_t) { return 0; }
int32_t au_getParam(Ctx& x, uint32_t, uint32_t, uint32_t, uint32_t, GuestPtr out) { if (out) x.mem.write<float>(out, 1.0f); return 0; }
int32_t au_render(Ctx& x, uint32_t, GuestPtr, GuestPtr, uint32_t, uint32_t nFrames, GuestPtr ioData) {
    if (ioData) {
        const uint32_t nbuf = x.mem.read<uint32_t>(ioData);
        for (uint32_t b = 0; b < nbuf; ++b) {
            const uint32_t sz = x.mem.read<uint32_t>(ioData + 4 + b * 12 + 4);
            const uint32_t ptr = x.mem.read<uint32_t>(ioData + 4 + b * 12 + 8);
            if (ptr && sz) std::memset(x.mem.host(ptr), 0, std::min<uint32_t>(sz, nFrames * 8));
        }
    }
    return 0;
}
int32_t aug_new(Ctx& x, GuestPtr outGraph) {
    auto& st = ATS(x);
    std::lock_guard<std::recursive_mutex> lk(st.unitsMu);
    const uint32_t gid = st.nextGraphId++;
    st.graphs[gid].id = gid;
    if (outGraph) x.mem.write<uint32_t>(outGraph, gid);
    return 0;
}
int32_t aug_ok1(Ctx&, uint32_t) { return 0; }
int32_t aug_ok2(Ctx&, uint32_t, uint32_t) { return 0; }
int32_t aug_ok3(Ctx&, uint32_t, uint32_t, uint32_t) { return 0; }
int32_t aug_ok4(Ctx&, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
int32_t aug_ok5(Ctx&, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
int32_t aug_addNode(Ctx& x, uint32_t graph, GuestPtr desc, GuestPtr outNode) {
    auto& st = ATS(x);
    std::lock_guard<std::recursive_mutex> lk(st.unitsMu);
    const uint32_t uid = st.nextUnitId++;
    st.units[uid].id = uid;
    if (desc) {
        for (int i = 0; i < 5; ++i) st.units[uid].desc[i] = x.mem.read<uint32_t>(desc + i * 4);
    }
    if (st.graphs.count(graph)) st.graphs[graph].nodes.push_back(uid);
    if (outNode) x.mem.write<int32_t>(outNode, int32_t(uid));
    return 0;
}
int32_t aug_nodeInfo(Ctx& x, uint32_t, int32_t node, GuestPtr outDesc, GuestPtr outUnit) {
    auto& st = ATS(x);
    std::lock_guard<std::recursive_mutex> lk(st.unitsMu);
    const uint32_t uid = node > 0 ? uint32_t(node) : st.nextUnitId++;
    st.units[uid].id = uid;
    if (outDesc) {
        for (int i = 0; i < 5; ++i) x.mem.write<uint32_t>(outDesc + i * 4, st.units[uid].desc[i]);
    }
    if (outUnit) x.mem.write<uint32_t>(outUnit, uid);
    return 0;
}
int32_t aug_isRunning(Ctx& x, uint32_t, GuestPtr outRunning) { if (outRunning) x.mem.write<uint8_t>(outRunning, 1); return 0; }
int32_t aug_update(Ctx& x, uint32_t, GuestPtr outUpdated) { if (outUpdated) x.mem.write<uint8_t>(outUpdated, 1); return 0; }

int32_t aug_getIndNode(Ctx& x, uint32_t, uint32_t, GuestPtr outNode) {
    if (outNode) x.mem.write<int32_t>(outNode, 1);
    return 0;
}
int32_t aug_getNodeCount(Ctx& x, uint32_t, GuestPtr outCount) {
    if (outCount) x.mem.write<uint32_t>(outCount, 1);
    return 0;
}

// ============================================================= AudioFormat ===
// No encoder/decoder catalogue: every property reports kAudioFormatUnsupportedPropertyError,
// which apps treat as "not available" (Talking Bacteria checks for an AAC encoder).
constexpr int32_t kAudioFormatUnsupportedProperty = 0x70726F70;   // 'prop'
int32_t afmt_getPropertyInfo(Ctx& x, uint32_t, uint32_t, GuestPtr, GuestPtr outSize) {
    if (outSize) x.mem.write<uint32_t>(outSize, 0);
    return kAudioFormatUnsupportedProperty;
}
int32_t afmt_getProperty(Ctx& x, uint32_t, uint32_t, GuestPtr, GuestPtr ioSize, GuestPtr) {
    if (ioSize) x.mem.write<uint32_t>(ioSize, 0);
    return kAudioFormatUnsupportedProperty;
}

// =========================================================== AVAudioSession ===

id avs_shared(Ctx& x, id self, SEL) {
    static const char sharedKey = 0; uint32_t& shared = x.proc.processSlot(&sharedKey);
    if (!shared) { shared = rt(x).createInstance(self); rt(x).makeImmortal(shared); }
    return shared;
}
bool avs_yesErr(Ctx&, id, SEL, id, GuestPtr err) { if (err) (void)err; return true; }
bool avs_yesBoolErr(Ctx&, id, SEL, bool, GuestPtr) { return true; }
bool avs_yesErr2(Ctx&, id, SEL, id, uint32_t, GuestPtr) { return true; }
bool avs_no(Ctx&, id, SEL) { return false; }
id avs_category(Ctx& x, id, SEL) { return makeString(x, "AVAudioSessionCategorySoloAmbient"); }
void avs_setDelegate(Ctx&, id, SEL, id) {}
double avs_rate(Ctx&, id, SEL) { return 44100; }
float avs_volume(Ctx&, id, SEL) { return 1; }
bool avs_yesDoubleErr(Ctx&, id, SEL, double, GuestPtr) { return true; }
id avs_mode(Ctx& x, id, SEL) { return makeString(x, "AVAudioSessionModeDefault"); }
uint32_t avs_zero(Ctx&, id, SEL) { return 0; }
uint32_t avs_two(Ctx&, id, SEL) { return 2; }
uint32_t avs_one(Ctx&, id, SEL) { return 1; }
bool avs_yes(Ctx&, id, SEL) { return true; }
double avs_ioDuration(Ctx&, id, SEL) { return 0.0058; }   // 256 frames at 44.1 kHz
id avs_emptyArray(Ctx& x, id, SEL) { return Foundation::makeArray(x, {}); }

// ============================================================ AVAudioPlayer ===

struct PlayerData : ObjC::HostObject {
    id url = 0, data = 0, delegate = 0;
    bool playing = false, metering = false;
    float volume = 1.0f, pan = 0.0f, rate = 1.0f;
    int32_t loops = 0, loopsRemaining = 0;
    double time = 0.0, duration = 0.0;
    uint32_t channels = 2;
    DecodedAudio decoded;
    Audio::Pcm pcm;
    uint32_t voice = 0;      // mixer voice while playing or paused
    bool paused = false;
    std::shared_ptr<Foundation::RunLoop::Timer> timer;
};
Audio::VoiceParams playerParams(const PlayerData& d) { return {d.volume, d.pan, d.rate, d.loops < 0}; }
PlayerData& PD(Ctx& x, id p) { return rt(x).host<PlayerData>(p); }

void loadPlayerDecoded(Ctx& x, PlayerData& d, const uint8_t* bytes, size_t len) {
    d.decoded = decodeAudioBytes(bytes, len);
    if (d.decoded.stereoPcm.empty() && len) x.proc.reportMissing("AVAudioPlayer: audio format not supported (plays silence)");
    d.pcm = sharePcm(d.decoded.stereoPcm);
    d.duration = d.decoded.duration;
    d.channels = uint32_t(std::max(1, d.decoded.channels));
}

id ap_initURL(Ctx& x, id self, SEL, id url, GuestPtr errOut) {
    if (errOut) x.mem.write<uint32_t>(errOut, 0);
    PlayerData& d = PD(x, self);
    d.url = url ? rt(x).retain(x.cpu, url) : 0;
    if (url) {
        const std::string path = Foundation::urlFilePath(x, url);
        std::ifstream in(path, std::ios::binary);
        if (in) {
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
            loadPlayerDecoded(x, d, bytes.data(), bytes.size());
        }
    }
    return self;
}
id ap_initData(Ctx& x, id self, SEL, id data, GuestPtr errOut) {
    if (errOut) x.mem.write<uint32_t>(errOut, 0);
    PlayerData& d = PD(x, self);
    d.data = data ? rt(x).retain(x.cpu, data) : 0;
    if (data) {
        const auto bytes = Foundation::dataBytes(x, data);
        loadPlayerDecoded(x, d, bytes.data(), bytes.size());
    }
    return self;
}
bool ap_play(Ctx& x, id self, SEL) {
    PlayerData& d = PD(x, self);
    audioTrace("AVAudioPlayerPlay", self, d.decoded.stereoPcm.size());
    if (!d.pcm) return false;
    Audio::Mixer& mx = MX(x.proc);
    if (d.voice && d.paused) {
        mx.setPaused(d.voice, false);
    } else {
        if (d.voice) mx.release(d.voice);
        d.loopsRemaining = d.loops;
        d.voice = mx.play(d.pcm, d.decoded.sampleRate, playerParams(d), d.time);
    }
    d.playing = true;
    d.paused = false;
    if (d.timer) d.timer->valid = false;
    Process* p = &x.proc;
    // Finite loop counts and the delegate callback are driven from here.
    d.timer = Foundation::mainRunLoop(x.proc).addTimer(0.05, 0.05, true, [p, self](CPU::Cpu& c) {
        Ctx cx{*p, c, p->mem()};
        PlayerData* pd = rt(cx).hostIf<PlayerData>(self);
        if (!pd || !pd->playing || pd->paused) return;
        Audio::Mixer& m = MX(*p);
        if (m.playing(pd->voice)) { pd->time = m.positionSeconds(pd->voice); return; }
        m.release(pd->voice);
        pd->voice = 0;
        pd->time = 0;
        if (pd->loopsRemaining > 0) {
            --pd->loopsRemaining;
            pd->voice = m.play(pd->pcm, pd->decoded.sampleRate, playerParams(*pd), 0);
            return;
        }
        pd->playing = false;
        if (pd->timer) pd->timer->valid = false;
        if (pd->delegate && rt(cx).respondsTo(pd->delegate, rt(cx).sel("audioPlayerDidFinishPlaying:successfully:")))
            rt(cx).send(c, pd->delegate, "audioPlayerDidFinishPlaying:successfully:", {self, 1});
    });
    return true;
}
bool ap_playAt(Ctx& x, id self, SEL s, double) { return ap_play(x, self, s); }
void ap_pause(Ctx& x, id self, SEL) {
    PlayerData& d = PD(x, self);
    if (!d.playing) return;
    if (d.voice) { MX(x.proc).setPaused(d.voice, true); d.time = MX(x.proc).positionSeconds(d.voice); }
    d.playing = false;
    d.paused = true;
}
void ap_stop(Ctx& x, id self, SEL) {
    PlayerData& d = PD(x, self);
    if (d.voice) { d.time = MX(x.proc).positionSeconds(d.voice); MX(x.proc).release(d.voice); d.voice = 0; }
    d.playing = false;
    d.paused = false;
    if (d.timer) d.timer->valid = false;
}
bool ap_prepare(Ctx&, id, SEL) { return true; }
bool ap_isPlaying(Ctx& x, id self, SEL) { return PD(x, self).playing; }
float ap_volume(Ctx& x, id self, SEL) { return PD(x, self).volume; }
void ap_setVolume(Ctx& x, id self, SEL, float v) { PlayerData& d = PD(x, self); d.volume = std::clamp(v, 0.0f, 1.0f); if (d.voice) MX(x.proc).setParams(d.voice, playerParams(d)); }
float ap_pan(Ctx& x, id self, SEL) { return PD(x, self).pan; }
void ap_setPan(Ctx& x, id self, SEL, float v) { PlayerData& d = PD(x, self); d.pan = std::clamp(v, -1.0f, 1.0f); if (d.voice) MX(x.proc).setParams(d.voice, playerParams(d)); }
float ap_rate(Ctx& x, id self, SEL) { return PD(x, self).rate; }
void ap_setRate(Ctx& x, id self, SEL, float v) { PlayerData& d = PD(x, self); d.rate = std::clamp(v, 0.25f, 4.0f); if (d.voice) MX(x.proc).setParams(d.voice, playerParams(d)); }
int32_t ap_loops(Ctx& x, id self, SEL) { return PD(x, self).loops; }
void ap_setLoops(Ctx& x, id self, SEL, int32_t n) { PlayerData& d = PD(x, self); d.loops = n; d.loopsRemaining = n; if (d.voice) MX(x.proc).setParams(d.voice, playerParams(d)); }
double ap_time(Ctx& x, id self, SEL) { const PlayerData& d = PD(x, self); return d.voice ? MX(x.proc).positionSeconds(d.voice) : d.time; }
void ap_setTime(Ctx& x, id self, SEL, double t) { PlayerData& d = PD(x, self); d.time = std::max(0.0, t); if (d.voice) MX(x.proc).seekSeconds(d.voice, d.time); }
double ap_duration(Ctx& x, id self, SEL) { return PD(x, self).duration; }
id ap_delegate(Ctx& x, id self, SEL) { return PD(x, self).delegate; }
void ap_setDelegate(Ctx& x, id self, SEL, id d) { PD(x, self).delegate = d; }
id ap_url(Ctx& x, id self, SEL) { return PD(x, self).url; }
id ap_data(Ctx& x, id self, SEL) { return PD(x, self).data; }
void ap_setMetering(Ctx& x, id self, SEL, bool b) { PD(x, self).metering = b; }
bool ap_isMetering(Ctx& x, id self, SEL) { return PD(x, self).metering; }
void ap_ignoreBool(Ctx&, id, SEL, bool) {}
void ap_updateMeters(Ctx&, id, SEL) {}
float ap_power(Ctx& x, id self, SEL, uint32_t) { return PD(x, self).playing ? -6.0f : -160.0f; }
uint32_t ap_channels(Ctx& x, id self, SEL) { return PD(x, self).channels; }
void ap_dealloc(Ctx& x, id self, SEL) {
    if (PlayerData* d = rt(x).hostIf<PlayerData>(self)) {
        if (d->timer) d->timer->valid = false;
        if (d->voice) MX(x.proc).release(d->voice);
        for (id* o : {&d->url, &d->data}) { if (*o) rt(x).release(x.cpu, *o); *o = 0; }
    }
    rt(x).disposeInstance(x.cpu, self);
}

// ================================================================== OpenAL ===

enum : uint32_t {
    AL_NONE = 0,
    AL_FALSE = 0,
    AL_TRUE = 1,
    AL_SOURCE_RELATIVE = 0x0202,
    AL_PITCH = 0x1003,
    AL_POSITION = 0x1004,
    AL_VELOCITY = 0x1006,
    AL_LOOPING = 0x1007,
    AL_BUFFER = 0x1009,
    AL_GAIN = 0x100A,
    AL_MIN_GAIN = 0x100D,
    AL_MAX_GAIN = 0x100E,
    AL_SOURCE_STATE = 0x1010,
    AL_INITIAL = 0x1011,
    AL_PLAYING = 0x1012,
    AL_PAUSED = 0x1013,
    AL_STOPPED = 0x1014,
    AL_BUFFERS_QUEUED = 0x1015,
    AL_BUFFERS_PROCESSED = 0x1016,
    AL_FORMAT_MONO8 = 0x1100,
    AL_FORMAT_MONO16 = 0x1101,
    AL_FORMAT_STEREO8 = 0x1102,
    AL_FORMAT_STEREO16 = 0x1103,
    AL_FREQUENCY = 0x2001,
    AL_BITS = 0x2002,
    AL_CHANNELS = 0x2003,
    AL_SIZE = 0x2004,
};

struct ALBuffer {
    uint32_t id = 0;
    uint32_t format = AL_FORMAT_STEREO16;
    int freq = 44100;
    int byteSize = 0;
    std::vector<int16_t> stereoPcm;
    Audio::Pcm pcm;   // shared with the mixer voices playing it
};

struct ALSource {
    uint32_t id = 0;
    uint32_t buffer = 0;
    uint32_t state = AL_INITIAL;
    float pitch = 1.0f, gain = 1.0f;
    float minGain = 0.0f, maxGain = 1.0f;
    float pos[3] = {0, 0, 0}, vel[3] = {0, 0, 0};
    bool looping = false, relative = false;
    std::vector<uint32_t> queuedBuffers;
    uint32_t voice = 0;      // mixer voice while playing or paused
    bool streaming = false;  // voice plays the buffer queue
};

struct OpenALState {
    explicit OpenALState(Process&) {}
    uint32_t nextBuf = 1, nextSrc = 1;
    uint32_t currentDevice = 0, currentContext = 0;
    uint32_t lastError = 0, lastAlcError = 0;
    float listenerGain = 1.0f;
    std::unordered_map<uint32_t, ALBuffer> buffers;
    std::unordered_map<uint32_t, ALSource> sources;
};
OpenALState& OAL(Ctx& x) { return x.proc.service<OpenALState>(); }

uint32_t alc_openDevice(Ctx& x, GuestPtr) { OAL(x).currentDevice = 1; return 1; }
bool alc_closeDevice(Ctx& x, uint32_t) { OAL(x).currentDevice = 0; return true; }
uint32_t alc_createContext(Ctx& x, uint32_t, GuestPtr) { OAL(x).currentContext = 1; return 1; }
bool alc_makeCurrent(Ctx& x, uint32_t ctx) { OAL(x).currentContext = ctx; return true; }
void alc_process(Ctx&, uint32_t) {}
void alc_suspend(Ctx&, uint32_t) {}
void alc_destroy(Ctx& x, uint32_t) { OAL(x).currentContext = 0; }
uint32_t alc_getCurrent(Ctx& x) { return OAL(x).currentContext; }
uint32_t alc_getDevice(Ctx& x, uint32_t) { return OAL(x).currentDevice; }
uint32_t alc_getError(Ctx& x, uint32_t) { const uint32_t e = OAL(x).lastAlcError; OAL(x).lastAlcError = 0; return e; }
bool alc_isExt(Ctx&, uint32_t, GuestPtr) { return false; }
GuestPtr alc_getString(Ctx& x, uint32_t, uint32_t) { return x.proc.internCString("HydrogenVM OpenAL"); }
void alc_macMixerRate(Ctx&, double) {}
double alc_macGetMixerRate(Ctx&) { return 44100.0; }
void alc_macIntSetter(Ctx&, int32_t) {}
int32_t alc_macIntGetter(Ctx&) { return 2; }

uint32_t al_getError(Ctx& x) { const uint32_t e = OAL(x).lastError; OAL(x).lastError = 0; return e; }
GuestPtr al_getString(Ctx& x, uint32_t) { return x.proc.internCString("HydrogenVM OpenAL 1.1"); }

void al_genBuffers(Ctx& x, int32_t n, GuestPtr ids) {
    auto& st = OAL(x);
    if (!ids || n <= 0) return;
    for (int32_t i = 0; i < n; ++i) {
        const uint32_t id = st.nextBuf++;
        st.buffers[id].id = id;
        x.mem.write<uint32_t>(ids + uint32_t(i * 4), id);
    }
}
void al_deleteBuffers(Ctx& x, int32_t n, GuestPtr ids) {
    auto& st = OAL(x);
    if (!ids || n <= 0) return;
    for (int32_t i = 0; i < n; ++i) st.buffers.erase(x.mem.read<uint32_t>(ids + uint32_t(i * 4)));
}
bool al_isBuffer(Ctx& x, uint32_t id) { return id != 0 && OAL(x).buffers.count(id) > 0; }
void al_bufferDataImpl(Ctx& x, uint32_t bid, uint32_t format, GuestPtr data, int32_t size, int32_t freq) {
    auto& st = OAL(x);
    if (!st.buffers.count(bid) || size < 0) return;
    ALBuffer& b = st.buffers[bid];
    b.format = format;
    b.freq = std::max(8000, freq);
    b.byteSize = size;
    b.stereoPcm.clear();
    if (!data || size == 0) return;
    const uint8_t* p = x.mem.host(data);
    if (format == AL_FORMAT_STEREO16) {
        const size_t n = size_t(size) / 2;
        b.stereoPcm.resize(n & ~size_t(1));
        std::memcpy(b.stereoPcm.data(), p, b.stereoPcm.size() * 2);
    } else if (format == AL_FORMAT_MONO16) {
        const size_t frames = size_t(size) / 2;
        b.stereoPcm.resize(frames * 2);
        for (size_t i = 0; i < frames; ++i) {
            int16_t s; std::memcpy(&s, p + i * 2, 2);
            b.stereoPcm[i * 2] = b.stereoPcm[i * 2 + 1] = s;
        }
    } else if (format == AL_FORMAT_MONO8) {
        const size_t frames = size_t(size);
        b.stereoPcm.resize(frames * 2);
        for (size_t i = 0; i < frames; ++i) {
            const int16_t s = int16_t((int(p[i]) - 128) * 256);
            b.stereoPcm[i * 2] = b.stereoPcm[i * 2 + 1] = s;
        }
    } else if (format == AL_FORMAT_STEREO8) {
        const size_t frames = size_t(size) / 2;
        b.stereoPcm.resize(frames * 2);
        for (size_t i = 0; i < frames; ++i) {
            b.stereoPcm[i * 2 + 0] = int16_t((int(p[i * 2 + 0]) - 128) * 256);
            b.stereoPcm[i * 2 + 1] = int16_t((int(p[i * 2 + 1]) - 128) * 256);
        }
    }
}
void al_bufferData(Ctx& x, uint32_t bid, uint32_t format, GuestPtr data, int32_t size, int32_t freq) {
    al_bufferDataImpl(x, bid, format, data, size, freq);
    auto& st = OAL(x);
    if (st.buffers.count(bid)) st.buffers[bid].pcm = sharePcm(st.buffers[bid].stereoPcm);
}
void al_getBufferi(Ctx& x, uint32_t bid, uint32_t param, GuestPtr val) {
    if (!val || !OAL(x).buffers.count(bid)) return;
    const ALBuffer& b = OAL(x).buffers[bid];
    int32_t v = 0;
    if (param == AL_FREQUENCY) v = b.freq;
    else if (param == AL_SIZE) v = b.byteSize;
    else if (param == AL_BITS) v = (b.format == AL_FORMAT_MONO8 || b.format == AL_FORMAT_STEREO8) ? 8 : 16;
    else if (param == AL_CHANNELS) v = (b.format == AL_FORMAT_STEREO8 || b.format == AL_FORMAT_STEREO16) ? 2 : 1;
    x.mem.write<int32_t>(val, v);
}

// Stereo position from the source's direction (the listener is at the origin).
Audio::VoiceParams alParams(const ALSource& s) {
    const float len = std::sqrt(s.pos[0] * s.pos[0] + s.pos[1] * s.pos[1] + s.pos[2] * s.pos[2]);
    return {std::max(0.0f, s.gain), len > 1e-6f ? std::clamp(s.pos[0] / len, -1.0f, 1.0f) * 0.8f : 0.0f, s.pitch, s.looping};
}
void alUpdate(Ctx& x, const ALSource& s) { if (s.voice) MX(x.proc).setParams(s.voice, alParams(s)); }
// A playing source stops when its voice runs out of audio.
void alRefresh(Ctx& x, ALSource& s) {
    if (s.state == AL_PLAYING && (!s.voice || !MX(x.proc).playing(s.voice))) s.state = AL_STOPPED;
}
void alRelease(Ctx& x, ALSource& s) { if (s.voice) MX(x.proc).release(s.voice); s.voice = 0; s.streaming = false; }
size_t alProcessed(Ctx& x, ALSource& s) {
    alRefresh(x, s);
    if (s.state == AL_STOPPED) return s.queuedBuffers.size();
    if (s.state == AL_INITIAL || !s.streaming) return 0;
    return std::min(s.queuedBuffers.size(), MX(x.proc).processedCount(s.voice));
}

void al_genSources(Ctx& x, int32_t n, GuestPtr ids) {
    auto& st = OAL(x);
    if (!ids || n <= 0) return;
    for (int32_t i = 0; i < n; ++i) {
        const uint32_t id = st.nextSrc++;
        st.sources[id].id = id;
        x.mem.write<uint32_t>(ids + uint32_t(i * 4), id);
    }
}
void al_deleteSources(Ctx& x, int32_t n, GuestPtr ids) {
    auto& st = OAL(x);
    if (!ids || n <= 0) return;
    for (int32_t i = 0; i < n; ++i) {
        const uint32_t sid = x.mem.read<uint32_t>(ids + uint32_t(i * 4));
        if (st.sources.count(sid)) alRelease(x, st.sources[sid]);
        st.sources.erase(sid);
    }
}
bool al_isSource(Ctx& x, uint32_t id) { return id != 0 && OAL(x).sources.count(id) > 0; }
void al_sourcef(Ctx& x, uint32_t sid, uint32_t param, float val) {
    if (!OAL(x).sources.count(sid)) return;
    ALSource& s = OAL(x).sources[sid];
    if (param == AL_PITCH) s.pitch = val;
    else if (param == AL_GAIN) s.gain = val;
    else if (param == AL_MIN_GAIN) s.minGain = val;
    else if (param == AL_MAX_GAIN) s.maxGain = val;
    audioTrace("alSourcef", sid, (uint64_t(param) << 32) | uint32_t(std::lround(val * 1000000.0f)));
    alUpdate(x, s);
}
void al_source3f(Ctx& x, uint32_t sid, uint32_t param, float v1, float v2, float v3) {
    if (!OAL(x).sources.count(sid)) return;
    ALSource& s = OAL(x).sources[sid];
    if (param == AL_POSITION) { s.pos[0] = v1; s.pos[1] = v2; s.pos[2] = v3; }
    else if (param == AL_VELOCITY) { s.vel[0] = v1; s.vel[1] = v2; s.vel[2] = v3; }
    alUpdate(x, s);
}
void al_sourcefv(Ctx& x, uint32_t sid, uint32_t param, GuestPtr vals) {
    audioTrace("alSourcefv", sid, param);
    if (vals) al_source3f(x, sid, param, x.mem.read<float>(vals), x.mem.read<float>(vals + 4), x.mem.read<float>(vals + 8));
}
void al_sourcei(Ctx& x, uint32_t sid, uint32_t param, int32_t val) {
    if (!OAL(x).sources.count(sid)) return;
    ALSource& s = OAL(x).sources[sid];
    if (param == AL_BUFFER) { s.buffer = uint32_t(val); s.queuedBuffers.clear(); }
    else if (param == AL_LOOPING) s.looping = val != 0;
    else if (param == AL_SOURCE_RELATIVE) s.relative = val != 0;
    alUpdate(x, s);
}
void al_getSourcef(Ctx& x, uint32_t sid, uint32_t param, GuestPtr val) {
    if (!val || !OAL(x).sources.count(sid)) return;
    const ALSource& s = OAL(x).sources[sid];
    float v = param == AL_PITCH ? s.pitch : param == AL_GAIN ? s.gain :
              param == AL_MIN_GAIN ? s.minGain : param == AL_MAX_GAIN ? s.maxGain : 0.0f;
    if (param == 0x1024 /* AL_SEC_OFFSET */ && s.voice) v = float(MX(x.proc).positionSeconds(s.voice));
    x.mem.write<float>(val, v);
}
void al_getSourcei(Ctx& x, uint32_t sid, uint32_t param, GuestPtr val) {
    if (!val || !OAL(x).sources.count(sid)) return;
    ALSource& s = OAL(x).sources[sid];
    alRefresh(x, s);
    int32_t v = 0;
    if (param == AL_SOURCE_STATE) v = int32_t(s.state);
    else if (param == AL_BUFFER) v = int32_t(s.buffer);
    else if (param == AL_LOOPING) v = s.looping ? 1 : 0;
    else if (param == AL_SOURCE_RELATIVE) v = s.relative ? 1 : 0;
    else if (param == AL_BUFFERS_QUEUED) v = int32_t(s.queuedBuffers.size());
    else if (param == AL_BUFFERS_PROCESSED) v = int32_t(alProcessed(x, s));
    else if (param == 0x1025 /* AL_SAMPLE_OFFSET */ && s.voice && OAL(x).buffers.count(s.buffer))
        v = int32_t(MX(x.proc).positionSeconds(s.voice) * OAL(x).buffers[s.buffer].freq);
    x.mem.write<int32_t>(val, v);
}
void al_sourcePlay(Ctx& x, uint32_t sid) {
    auto& st = OAL(x);
    audioTrace("alSourcePlay", sid);
    if (!st.sources.count(sid)) return;
    ALSource& s = st.sources[sid];
    Audio::Mixer& mx = MX(x.proc);
    if (s.state == AL_PAUSED && s.voice) { mx.setPaused(s.voice, false); s.state = AL_PLAYING; return; }
    alRelease(x, s);   // playing again restarts from the beginning
    if (!s.queuedBuffers.empty()) {
        s.voice = mx.createStream(alParams(s));
        s.streaming = true;
        for (uint32_t bid : s.queuedBuffers)
            if (st.buffers.count(bid)) mx.enqueue(s.voice, st.buffers[bid].pcm, st.buffers[bid].freq, bid);
    } else if (s.buffer && st.buffers.count(s.buffer)) {
        s.voice = mx.play(st.buffers[s.buffer].pcm, st.buffers[s.buffer].freq, alParams(s));
    }
    s.state = AL_PLAYING;
    audioTrace("alSourcePlayState", sid, (uint64_t(s.state) << 32) | s.voice);
    audioTrace("alSourcePlayQueue", sid, s.queuedBuffers.size());
    audioTrace("alSourcePlayGain", sid, uint64_t(std::lround(s.gain * 1000000.0f)));
}
void al_sourcePause(Ctx& x, uint32_t sid) {
    if (!OAL(x).sources.count(sid)) return;
    ALSource& s = OAL(x).sources[sid];
    alRefresh(x, s);
    if (s.state != AL_PLAYING) return;
    if (s.voice) MX(x.proc).setPaused(s.voice, true);
    s.state = AL_PAUSED;
}
void al_sourceStop(Ctx& x, uint32_t sid) {
    if (!OAL(x).sources.count(sid)) return;
    ALSource& s = OAL(x).sources[sid];
    audioTrace("alSourceStop", sid, s.state);
    if (s.voice) MX(x.proc).stop(s.voice);
    if (s.state != AL_INITIAL) s.state = AL_STOPPED;
}
void al_sourceRewind(Ctx& x, uint32_t sid) { if (OAL(x).sources.count(sid)) { ALSource& s = OAL(x).sources[sid]; alRelease(x, s); s.state = AL_INITIAL; } }
void al_sourceQueueBuffers(Ctx& x, uint32_t sid, int32_t n, GuestPtr bids) {
    auto& st = OAL(x);
    if (!st.sources.count(sid) || !bids || n <= 0) return;
    ALSource& s = st.sources[sid];
    alRefresh(x, s);
    audioTrace("alSourceQueueState", sid, (uint64_t(s.state) << 32) | s.voice);
    s.buffer = 0;
    for (int32_t i = 0; i < n; ++i) {
        const uint32_t bid = x.mem.read<uint32_t>(bids + uint32_t(i * 4));
        s.queuedBuffers.push_back(bid);
        if (s.streaming && s.voice && (s.state == AL_PLAYING || s.state == AL_PAUSED) && st.buffers.count(bid))
            MX(x.proc).enqueue(s.voice, st.buffers[bid].pcm, st.buffers[bid].freq, bid);
        audioTrace("alSourceQueueBuffer", sid, (uint64_t(bid) << 32) | uint32_t(s.queuedBuffers.size()));
    }
}
void al_sourceUnqueueBuffers(Ctx& x, uint32_t sid, int32_t n, GuestPtr bids) {
    auto& st = OAL(x);
    if (!st.sources.count(sid) || !bids || n <= 0) return;
    ALSource& s = st.sources[sid];
    const size_t avail = alProcessed(x, s);
    audioTrace("alSourceUnqueue", sid, (uint64_t(avail) << 32) | uint32_t(n));
    if (size_t(n) > avail) { st.lastError = 0xA004; /* AL_INVALID_VALUE */ return; }
    for (int32_t i = 0; i < n; ++i) {
        x.mem.write<uint32_t>(bids + uint32_t(i * 4), s.queuedBuffers.front());
        s.queuedBuffers.erase(s.queuedBuffers.begin());
    }
    if (s.voice && s.streaming && s.state != AL_STOPPED) MX(x.proc).takeProcessed(s.voice, size_t(n));
}
void al_listenerf(Ctx& x, uint32_t param, float val) { if (param == AL_GAIN) { OAL(x).listenerGain = val; MX(x.proc).setMasterGain(val); audioTrace("alListenerGain", param, uint64_t(std::lround(val * 1000000.0f))); } }
void al_listener3f(Ctx&, uint32_t, float, float, float) {}
void al_listenerfv(Ctx&, uint32_t, GuestPtr) {}
void al_getListenerf(Ctx& x, uint32_t param, GuestPtr val) { if (val) x.mem.write<float>(val, param == AL_GAIN ? OAL(x).listenerGain : 0.0f); }
void al_getListenerfv(Ctx& x, uint32_t param, GuestPtr val) {
    if (!val) return;
    const uint32_t n = param == 0x100F /* AL_ORIENTATION */ ? 6 : 3;
    for (uint32_t i = 0; i < n; ++i) x.mem.write<float>(val + i * 4, param == 0x100F && (i == 2 || i == 4) ? (i == 2 ? -1.0f : 1.0f) : 0.0f);
}
void al_getSourcefv(Ctx& x, uint32_t sid, uint32_t param, GuestPtr val) {
    if (!val || !OAL(x).sources.count(sid)) return;
    const ALSource& s = OAL(x).sources[sid];
    const float* v = param == 0x1004 /* AL_POSITION */ ? s.pos : param == 0x1006 /* AL_VELOCITY */ ? s.vel : nullptr;
    for (int i = 0; i < 3; ++i) x.mem.write<float>(val + uint32_t(i) * 4, v ? v[i] : (i == 0 && param == AL_GAIN ? s.gain : i == 0 && param == AL_PITCH ? s.pitch : 0.0f));
}
// alcGetProcAddress / alGetProcAddress: extensions are looked up as exported functions
// (alBufferDataStatic is plain alBufferData here: the data is copied).
uint32_t al_getProcAddress(Ctx& x, GuestPtr name) {
    const std::string n = x.proc.readCString(name);
    if (auto a = x.proc.lookupSymbol("_" + n)) return *a;
    return 0;
}
uint32_t alc_getProcAddress(Ctx& x, uint32_t, GuestPtr name) { return al_getProcAddress(x, name); }
void al_noop1(Ctx&, uint32_t) {}

// --------------------------------------------------------------- StoreKit ---
bool sk_canPay(Ctx& x, id, SEL) { noteOnce(x, "StoreKit (in-app purchases are disabled)"); return false; }
id sk_queue(Ctx& x, id self, SEL) {
    static const char sharedKey = 0; uint32_t& shared = x.proc.processSlot(&sharedKey);
    if (!shared) { shared = rt(x).createInstance(self); rt(x).makeImmortal(shared); }
    return shared;
}
void sk_ignoreObj(Ctx&, id, SEL, id) {}
void sk_noop(Ctx&, id, SEL) {}
id sk_empty(Ctx& x, id, SEL) { return Foundation::makeArray(x, {}); }

// SKProductsRequest: no App Store, so every product id is "invalid" and the
// delegate gets an empty response (not an error — IAP code expects a response).
struct ProductsRequest : ObjC::HostObject { id delegate = 0, ids = 0; };
id skpr_initWithIds(Ctx& x, id self, SEL, id ids) {
    auto h = std::make_unique<ProductsRequest>();
    h->ids = ids ? rt(x).retain(x.cpu, ids) : 0;
    rt(x).setHost(self, std::move(h));
    return self;
}
id skpr_delegate(Ctx& x, id self, SEL) { auto* h = rt(x).hostIf<ProductsRequest>(self); return h ? h->delegate : 0; }
void skpr_setDelegate(Ctx& x, id self, SEL, id d) { if (auto* h = rt(x).hostIf<ProductsRequest>(self)) h->delegate = d; }
void skpr_start(Ctx& x, id self, SEL) {
    auto* h = rt(x).hostIf<ProductsRequest>(self);
    if (!h || !h->delegate) return;
    rt(x).retain(x.cpu, self);
    Process* p = &x.proc;
    Foundation::mainRunLoop(x.proc).post([p, self](CPU::Cpu& c) {
        Ctx cx{*p, c, p->mem()};
        auto* h = rt(cx).hostIf<ProductsRequest>(self);
        if (h && h->delegate && rt(cx).respondsTo(h->delegate, rt(cx).sel("productsRequest:didReceiveResponse:"))) {
            const id response = rt(cx).createInstance(rt(cx).getClass("SKProductsResponse"));
            rt(cx).makeImmortal(response);
            // stash the requested ids on the response for -invalidProductIdentifiers
            if (h->ids) rt(cx).setAssociated(c, response, 1, h->ids, 1);
            rt(cx).send(c, h->delegate, "productsRequest:didReceiveResponse:", {self, response});
        }
        if (h && h->delegate && rt(cx).respondsTo(h->delegate, rt(cx).sel("requestDidFinish:")))
            rt(cx).send(c, h->delegate, "requestDidFinish:", {self});
        rt(cx).release(c, self);
    });
}
void skpr_cancel(Ctx&, id, SEL) {}
void skpr_dealloc(Ctx& x, id self, SEL) {
    if (auto* h = rt(x).hostIf<ProductsRequest>(self)) if (h->ids) rt(x).release(x.cpu, h->ids);
    rt(x).disposeInstance(x.cpu, self);
}
id skresp_products(Ctx& x, id, SEL) { return Foundation::makeArray(x, {}); }
id skresp_invalid(Ctx& x, id self, SEL) {
    const id ids = rt(x).getAssociated(self, 1);
    if (!ids) return Foundation::makeArray(x, {});
    if (rt(x).respondsTo(ids, rt(x).sel("allObjects"))) return id(rt(x).send(x.cpu, ids, "allObjects"));   // NSSet -> array
    return ids;
}

// ------------------------------------------------------------- MessageUI ---
bool mf_no(Ctx&, id, SEL) { return false; }

// ---------------------------------------------------- SystemConfiguration ---
uint32_t scr_create(Ctx& x, uint32_t, GuestPtr) { noteOnce(x, "SCNetworkReachability (network reported offline)"); return rt(x).createInstance(rt(x).nsObject); }
uint32_t scr_createWithAddress(Ctx& x, uint32_t a, GuestPtr p) { return scr_create(x, a, p); }
uint32_t scr_createPair(Ctx& x, uint32_t a, GuestPtr p, GuestPtr) { return scr_create(x, a, p); }
bool scr_getFlags(Ctx& x, uint32_t, GuestPtr flags) { if (flags) x.mem.write<uint32_t>(flags, 0); return true; }
bool scr_setCallback(Ctx&, uint32_t, uint32_t, uint32_t) { return true; }
bool scr_schedule(Ctx&, uint32_t, uint32_t, uint32_t) { return true; }
bool scr_setQueue(Ctx&, uint32_t, uint32_t) { return true; }

} // namespace

void registerPlatformStubs(Process& p, ObjC::Runtime& r) {
    // AudioSession & AudioServices
    p.def<&as_init>("_AudioSessionInitialize");
    p.def<&as_setProperty>("_AudioSessionSetProperty");
    p.def<&as_getProperty>("_AudioSessionGetProperty");
    p.def<&as_getPropertySize>("_AudioSessionGetPropertySize");
    p.def<&as_setActive>("_AudioSessionSetActive");
    p.def<&as_addListener>("_AudioSessionAddPropertyListener");
    p.def<&as_addListener>("_AudioSessionRemovePropertyListenerWithUserData");
    p.def<&ss_create>("_AudioServicesCreateSystemSoundID");
    p.def<&ss_play>("_AudioServicesPlaySystemSound");
    p.def<&ss_play>("_AudioServicesPlayAlertSound");
    p.def<&ss_dispose>("_AudioServicesDisposeSystemSoundID");
    p.def<&ss_addCompletion>("_AudioServicesAddSystemSoundCompletion");
    p.def<&ss_removeCompletion>("_AudioServicesRemoveSystemSoundCompletion");

    // AudioQueue
    p.def<&aq_newOutput>("_AudioQueueNewOutput");
    p.def<&aq_newInput>("_AudioQueueNewInput");
    p.def<&aq_allocateBuffer>("_AudioQueueAllocateBuffer");
    p.def<&aq_allocateBufferWithPacketDescriptions>("_AudioQueueAllocateBufferWithPacketDescriptions");
    p.def<&aq_freeBuffer>("_AudioQueueFreeBuffer");
    p.def<&aq_enqueueBuffer>("_AudioQueueEnqueueBuffer");
    p.defineFunction("_AudioQueueEnqueueBufferWithParameters", aq_enqueueWithParams);
    p.def<&aq_setProperty>("_AudioQueueSetProperty");
    p.def<&aq_getProperty>("_AudioQueueGetProperty");
    p.def<&aq_getPropertySize>("_AudioQueueGetPropertySize");
    p.def<&aq_addPropertyListener>("_AudioQueueAddPropertyListener");
    p.def<&aq_removePropertyListener>("_AudioQueueRemovePropertyListener");
    p.def<&aq_createTimeline>("_AudioQueueCreateTimeline");
    p.def<&aq_disposeTimeline>("_AudioQueueDisposeTimeline");
    p.def<&aq_getCurrentTime>("_AudioQueueGetCurrentTime");
    p.def<&aq_start>("_AudioQueueStart");
    p.def<&aq_pause>("_AudioQueuePause");
    p.def<&aq_stop>("_AudioQueueStop");
    p.def<&aq_flush>("_AudioQueueFlush");
    p.def<&aq_reset>("_AudioQueueReset");
    p.def<&aq_dispose>("_AudioQueueDispose");
    p.def<&aq_setParam>("_AudioQueueSetParameter");
    p.def<&aq_getParam>("_AudioQueueGetParameter");
    p.def<&aq_offlineRender>("_AudioQueueOfflineRender");
    p.def<&aq_setOfflineFormat>("_AudioQueueSetOfflineRenderFormat");

    // AudioFile & AudioUnit
    p.def<&af_openURL>("_AudioFileOpenURL");
    p.def<&af_getPropertyInfo>("_AudioFileGetPropertyInfo");
    p.def<&afmt_getPropertyInfo>("_AudioFormatGetPropertyInfo");
    p.def<&afmt_getProperty>("_AudioFormatGetProperty");
    p.def<&af_getProperty>("_AudioFileGetProperty");
    p.def<&af_readBytes>("_AudioFileReadBytes");
    p.defineFunction("_AudioFileOpenWithCallbacks", af_openWithCallbacks);
    p.defineFunction("_AudioFileReadPacketData", [](Ctx& x) { af_readPackets(x, true); });
    p.defineFunction("_AudioFileReadPackets", [](Ctx& x) { af_readPackets(x, false); });
    p.def<&af_close>("_AudioFileClose");
    p.def<&eaf_openURL>("_ExtAudioFileOpenURL");
    p.def<&eaf_wrap>("_ExtAudioFileWrapAudioFileID");
    p.def<&eaf_dispose>("_ExtAudioFileDispose");
    p.def<&eaf_getPropertyInfo>("_ExtAudioFileGetPropertyInfo");
    p.def<&eaf_getProperty>("_ExtAudioFileGetProperty");
    p.def<&eaf_setProperty>("_ExtAudioFileSetProperty");
    p.def<&eaf_read>("_ExtAudioFileRead");
    p.defineFunction("_ExtAudioFileSeek", [](Ctx& x) { eaf_seek(x); });
    p.def<&eaf_tell>("_ExtAudioFileTell");
    p.def<&au_thread>("__hvm_audio_unit_pump");
    p.def<&ac_findNext>("_AudioComponentFindNext");
    p.def<&ac_instanceNew>("_AudioComponentInstanceNew");
    p.def<&ac_dispose>("_AudioComponentInstanceDispose");
    p.def<&au_init>("_AudioUnitInitialize");
    p.def<&au_uninit>("_AudioUnitUninitialize");
    p.def<&au_start>("_AudioOutputUnitStart");
    p.def<&au_stop>("_AudioOutputUnitStop");
    p.def<&au_setProp>("_AudioUnitSetProperty");
    p.def<&au_getProp>("_AudioUnitGetProperty");
    p.def<&au_setParam>("_AudioUnitSetParameter");
    p.def<&au_getParam>("_AudioUnitGetParameter");
    p.def<&au_render>("_AudioUnitRender");
    p.def<&aug_new>("_NewAUGraph");
    p.def<&aug_ok1>("_DisposeAUGraph");
    p.def<&aug_addNode>("_AUGraphAddNode");
    p.def<&aug_ok2>("_AUGraphRemoveNode");
    p.def<&aug_nodeInfo>("_AUGraphNodeInfo");
    p.def<&aug_ok1>("_AUGraphOpen");
    p.def<&aug_ok1>("_AUGraphClose");
    p.def<&aug_ok1>("_AUGraphInitialize");
    p.def<&aug_ok1>("_AUGraphUninitialize");
    p.def<&aug_ok1>("_AUGraphStart");
    p.def<&aug_ok1>("_AUGraphStop");
    p.def<&aug_isRunning>("_AUGraphIsRunning");
    p.def<&aug_isRunning>("_AUGraphIsOpen");
    p.def<&aug_isRunning>("_AUGraphIsInitialized");
    p.def<&aug_update>("_AUGraphUpdate");
    p.def<&aug_ok4>("_AUGraphSetNodeInputCallback");
    p.def<&aug_ok5>("_AUGraphConnectNodeInput");
    p.def<&aug_ok3>("_AUGraphDisconnectNodeInput");
    p.def<&aug_ok1>("_AUGraphClearConnections");
    p.def<&aug_getIndNode>("_AUGraphGetIndNode");
    p.def<&aug_getNodeCount>("_AUGraphGetNodeCount");
    p.def<&aug_ok3>("_AudioUnitAddRenderNotify");
    p.def<&aug_ok3>("_AudioUnitRemoveRenderNotify");

    // OpenAL
    p.def<&alc_openDevice>("_alcOpenDevice");
    p.def<&alc_closeDevice>("_alcCloseDevice");
    p.def<&alc_createContext>("_alcCreateContext");
    p.def<&alc_makeCurrent>("_alcMakeContextCurrent");
    p.def<&alc_process>("_alcProcessContext");
    p.def<&alc_suspend>("_alcSuspendContext");
    p.def<&alc_destroy>("_alcDestroyContext");
    p.def<&alc_getCurrent>("_alcGetCurrentContext");
    p.def<&alc_getDevice>("_alcGetContextsDevice");
    p.def<&alc_getError>("_alcGetError");
    p.def<&alc_isExt>("_alcIsExtensionPresent");
    p.def<&alc_getString>("_alcGetString");
    p.def<&alc_macMixerRate>("_alcMacOSXMixerOutputRate");
    p.def<&alc_macGetMixerRate>("_alcMacOSXGetMixerOutputRate");
    p.def<&alc_macIntSetter>("_alcMacOSXRenderingQuality");
    p.def<&alc_macIntGetter>("_alcMacOSXGetRenderingQuality");
    p.def<&alc_macIntSetter>("_alcMacOSXMixerMaxiumumBusses");
    p.def<&alc_macIntGetter>("_alcMacOSXGetMixerMaxiumumBusses");
    p.def<&alc_macIntSetter>("_alMacOSXRenderChannelCount");
    p.def<&alc_macIntGetter>("_alMacOSXGetRenderChannelCount");

    p.def<&al_getError>("_alGetError");
    p.def<&al_getString>("_alGetString");
    p.def<&al_genBuffers>("_alGenBuffers");
    p.def<&al_deleteBuffers>("_alDeleteBuffers");
    p.def<&al_isBuffer>("_alIsBuffer");
    p.def<&al_bufferData>("_alBufferData");
    p.def<&al_getBufferi>("_alGetBufferi");
    p.def<&al_genSources>("_alGenSources");
    p.def<&al_deleteSources>("_alDeleteSources");
    p.def<&al_isSource>("_alIsSource");
    p.def<&al_sourcef>("_alSourcef");
    p.def<&al_source3f>("_alSource3f");
    p.def<&al_sourcefv>("_alSourcefv");
    p.def<&al_sourcei>("_alSourcei");
    p.def<&al_getSourcef>("_alGetSourcef");
    p.def<&al_getSourcei>("_alGetSourcei");
    p.def<&al_sourcePlay>("_alSourcePlay");
    p.def<&al_sourcePause>("_alSourcePause");
    p.def<&al_sourceStop>("_alSourceStop");
    p.def<&al_sourceRewind>("_alSourceRewind");
    p.def<&al_sourceQueueBuffers>("_alSourceQueueBuffers");
    p.def<&al_sourceUnqueueBuffers>("_alSourceUnqueueBuffers");
    p.def<&al_listenerf>("_alListenerf");
    p.def<&al_listener3f>("_alListener3f");
    p.def<&al_listenerfv>("_alListenerfv");
    p.def<&al_getListenerf>("_alGetListenerf");
    p.def<&al_getListenerfv>("_alGetListenerfv");
    p.def<&al_getSourcefv>("_alGetSourcefv");
    p.def<&al_getProcAddress>("_alGetProcAddress");
    p.def<&alc_getProcAddress>("_alcGetProcAddress");
    p.def<&al_bufferData>("_alBufferDataStatic");
    p.def<&al_bufferData>("_alBufferDataStaticProc");
    p.def<&al_noop1>("_alDistanceModel");
    p.def<&al_noop1>("_alDopplerFactor");
    p.def<&al_noop1>("_alSpeedOfSound");

    // AVAudioSession
    ObjC::ClassBuilder avs = r.defineClass("AVAudioSession", "NSObject");
    avs.cm<&avs_shared>("sharedInstance");
    avs.m<&avs_yesErr>("setCategory:error:", "c@:@^@").m<&avs_yesErr2>("setCategory:withOptions:error:", "c@:@I^@")
       .m<&avs_yesBoolErr>("setActive:error:", "c@:c^@").m<&avs_yesErr2>("setActive:withOptions:error:", "c@:cI^@")
       .m<&avs_yesErr>("setMode:error:", "c@:@^@").m<&avs_category>("category").m<&avs_no>("isOtherAudioPlaying", "c@:")
       .m<&avs_no>("secondaryAudioShouldBeSilencedHint", "c@:").m<&avs_setDelegate>("setDelegate:", "v@:@")
       .m<&avs_rate>("sampleRate", "d@:").m<&avs_rate>("currentHardwareSampleRate", "d@:").m<&avs_rate>("preferredSampleRate", "d@:")
       .m<&avs_yesDoubleErr>("setPreferredSampleRate:error:", "c@:d^@").m<&avs_yesDoubleErr>("setPreferredHardwareSampleRate:error:", "c@:d^@")
       .m<&avs_yesDoubleErr>("setPreferredIOBufferDuration:error:", "c@:d^@").m<&avs_volume>("outputVolume", "f@:")
       // Read-only state (Hungry Shark Evolution asks for -mode at startup).
       .m<&avs_mode>("mode").m<&avs_zero>("categoryOptions", "I@:").m<&avs_yes>("isInputAvailable", "c@:")
       .m<&avs_yes>("inputIsAvailable", "c@:").m<&avs_two>("outputNumberOfChannels", "i@:").m<&avs_one>("inputNumberOfChannels", "i@:")
       .m<&avs_ioDuration>("IOBufferDuration", "d@:").m<&avs_ioDuration>("preferredIOBufferDuration", "d@:")
       .m<&avs_ioDuration>("outputLatency", "d@:").m<&avs_ioDuration>("inputLatency", "d@:")
       .m<&avs_emptyArray>("availableInputs");
    for (const char* n : {"AVAudioSessionCategoryAmbient", "AVAudioSessionCategorySoloAmbient", "AVAudioSessionCategoryPlayback",
                          "AVAudioSessionCategoryRecord", "AVAudioSessionCategoryPlayAndRecord", "AVAudioSessionModeDefault",
                          "AVAudioSessionInterruptionNotification", "AVAudioSessionRouteChangeNotification"}) {
        const uint32_t cell = p.defineDataBlock(std::string("_") + n, 4);
        p.mem().write<uint32_t>(cell, Foundation::constString(p, n));
    }

    // AVAudioPlayer
    ObjC::ClassBuilder ap = r.defineClass("AVAudioPlayer", "NSObject");
    ap.m<&ap_initURL>("initWithContentsOfURL:error:").m<&ap_initData>("initWithData:error:").m<&ap_play>("play", "c@:")
      .m<&ap_playAt>("playAtTime:", "c@:d").m<&ap_pause>("pause", "v@:").m<&ap_stop>("stop", "v@:").m<&ap_prepare>("prepareToPlay", "c@:")
      .m<&ap_isPlaying>("isPlaying", "c@:").m<&ap_volume>("volume", "f@:").m<&ap_setVolume>("setVolume:", "v@:f")
      .m<&ap_pan>("pan", "f@:").m<&ap_setPan>("setPan:", "v@:f").m<&ap_loops>("numberOfLoops", "i@:").m<&ap_setLoops>("setNumberOfLoops:", "v@:i")
      .m<&ap_time>("currentTime", "d@:").m<&ap_setTime>("setCurrentTime:", "v@:d").m<&ap_duration>("duration", "d@:")
      .m<&ap_delegate>("delegate").m<&ap_setDelegate>("setDelegate:", "v@:@").m<&ap_url>("url").m<&ap_data>("data")
      .m<&ap_setMetering>("setMeteringEnabled:", "v@:c").m<&ap_isMetering>("isMeteringEnabled", "c@:")
      .m<&ap_ignoreBool>("setEnableRate:", "v@:c").m<&ap_setRate>("setRate:", "v@:f").m<&ap_rate>("rate", "f@:")
      .m<&ap_updateMeters>("updateMeters", "v@:").m<&ap_power>("averagePowerForChannel:", "f@:I").m<&ap_power>("peakPowerForChannel:", "f@:I")
      .m<&ap_channels>("numberOfChannels", "I@:").m<&ap_dealloc>("dealloc", "v@:");


    // GameController
    auto zeroId = [](Ctx&, id, SEL) -> id { return 0; };
    auto zeroC = [](Ctx&, id, SEL) -> char { return 0; };
    auto ignoreQ = [](Ctx&, id, SEL, int64_t) {};
    ObjC::ClassBuilder gcc = r.defineClass("GCController", "NSObject");
    gcc.cm<&sk_empty>("controllers", "@@:")
       .cm<&sk_ignoreObj>("startWirelessControllerDiscoveryWithCompletionHandler:", "v@:@?")
       .cm<&sk_noop>("stopWirelessControllerDiscovery", "v@:")
       .m(zeroId, "controllerPausedHandler", "@?@:")
       .m<&sk_ignoreObj>("setControllerPausedHandler:", "v@:@?")
       .m(zeroC, "isAttachedToDevice", "c@:")
       .m(zeroId, "gamepad", "@@:")
       .m(zeroId, "extendedGamepad", "@@:");


    // GameKit: see GameKit.cpp

    // StoreKit
    ObjC::ClassBuilder skq = r.defineClass("SKPaymentQueue", "NSObject");
    skq.cm<&sk_canPay>("canMakePayments", "c@:").cm<&sk_queue>("defaultQueue");
    skq.m<&sk_ignoreObj>("addTransactionObserver:", "v@:@").m<&sk_ignoreObj>("removeTransactionObserver:", "v@:@")
       .m<&sk_ignoreObj>("addPayment:", "v@:@").m<&sk_ignoreObj>("finishTransaction:", "v@:@")
       .m<&sk_noop>("restoreCompletedTransactions", "v@:").m<&sk_empty>("transactions");
    ObjC::ClassBuilder skpr = r.defineClass("SKProductsRequest", "NSObject");
    skpr.m<&skpr_initWithIds>("initWithProductIdentifiers:").m<&skpr_delegate>("delegate").m<&skpr_setDelegate>("setDelegate:", "v@:@")
        .m<&skpr_start>("start", "v@:").m<&skpr_cancel>("cancel", "v@:").m<&skpr_dealloc>("dealloc", "v@:");
    ObjC::ClassBuilder skresp = r.defineClass("SKProductsResponse", "NSObject");
    skresp.m<&skresp_products>("products").m<&skresp_invalid>("invalidProductIdentifiers");
    ObjC::ClassBuilder skprod = r.defineClass("SKProduct", "NSObject");
    skprod.m<&sk_ignoreObj>("setLocalizedTitle:", "v@:@").m<&sk_ignoreObj>("setPrice:", "v@:@").m<&sk_ignoreObj>("setPriceLocale:", "v@:@")
          .m(zeroId, "localizedTitle", "@@:").m(zeroId, "price", "@@:").m(zeroId, "priceLocale", "@@:").m(zeroId, "productIdentifier", "@@:");

    auto allocZero = [](Ctx& x, id, SEL, id) -> id {
        const id inst = rt(x).createInstance(x.cpu.r[0]);
        return autoreleased(x, inst);
    };
    ObjC::ClassBuilder skpay = r.defineClass("SKPayment", "NSObject");
    skpay.cm(allocZero, "paymentWithProductIdentifier:", "@@:@")
         .m(ignoreQ, "setQuantity:", "v@:q").m(zeroId, "productIdentifier", "@@:");

    r.defineClass("SKMutablePayment", "SKPayment");


    auto zeroQ = [](Ctx&, id, SEL) -> int64_t { return 0; };
    ObjC::ClassBuilder sktrans = r.defineClass("SKPaymentTransaction", "NSObject");
    sktrans.m(zeroId, "transactionIdentifier", "@@:").m(zeroId, "transactionDate", "@@:")
           .m(zeroId, "transactionReceipt", "@@:").m(zeroId, "originalTransaction", "@@:")
           .m(zeroId, "payment", "@@:").m(zeroId, "error", "@@:")
           .m(zeroQ, "transactionState", "q@:");


    // MessageUI
    for (const char* n : {"MFMailComposeViewController", "MFMessageComposeViewController"}) {
        ObjC::ClassBuilder mf = r.defineClass(n, "UIViewController");
        mf.cm<&mf_no>(std::string(n) == "MFMailComposeViewController" ? "canSendMail" : "canSendText", "c@:");
    }

    // SystemConfiguration
    p.def<&scr_create>("_SCNetworkReachabilityCreateWithName");
    p.def<&scr_createWithAddress>("_SCNetworkReachabilityCreateWithAddress");
    p.def<&scr_createPair>("_SCNetworkReachabilityCreateWithAddressPair");
    p.def<&scr_getFlags>("_SCNetworkReachabilityGetFlags");
    p.def<&scr_setCallback>("_SCNetworkReachabilitySetCallback");
    p.def<&scr_schedule>("_SCNetworkReachabilityScheduleWithRunLoop");
    p.def<&scr_schedule>("_SCNetworkReachabilityUnscheduleFromRunLoop");
    p.def<&scr_setQueue>("_SCNetworkReachabilitySetDispatchQueue");

    // CoreMedia CMTime & AVFoundation Video Player (AVAsset, AVURLAsset, AVPlayerItem, AVPlayer, AVPlayerLayer)
    auto writeCMTime = [](Ctx& x, GuestPtr out, int64_t val, int32_t scale, uint32_t flags = 1) {
        if (!out) return;
        x.mem.write<int64_t>(out + 0, val);
        x.mem.write<int32_t>(out + 8, scale);
        x.mem.write<uint32_t>(out + 12, flags);
        x.mem.write<int64_t>(out + 16, 0);
    };
    p.defineFunction("_CMTimeGetSeconds", [](Ctx& x) {
        const int64_t val = int64_t(uint64_t(x.cpu.r[0]) | (uint64_t(x.cpu.r[1]) << 32));
        const int32_t scale = int32_t(x.cpu.r[2]);
        const uint32_t flags = x.cpu.r[3];
        const double sec = (scale > 0 && (flags & 1)) ? (double(val) / double(scale)) : 0.0;
        uint64_t bits;
        std::memcpy(&bits, &sec, 8);
        x.cpu.r[0] = uint32_t(bits);
        x.cpu.r[1] = uint32_t(bits >> 32);
    });
    p.defineFunction("_CMTimeMake", [writeCMTime](Ctx& x) {
        const GuestPtr sret = x.cpu.r[0];
        const int64_t val = int64_t(uint64_t(x.cpu.r[1]) | (uint64_t(x.cpu.r[2]) << 32));
        const int32_t scale = int32_t(x.cpu.r[3]);
        writeCMTime(x, sret, val, scale > 0 ? scale : 1, 1);
    });
    p.defineFunction("_CMTimeMakeWithSeconds", [writeCMTime](Ctx& x) {
        const GuestPtr sret = x.cpu.r[0];
        const uint64_t bits = uint64_t(x.cpu.r[1]) | (uint64_t(x.cpu.r[2]) << 32);
        double sec;
        std::memcpy(&sec, &bits, 8);
        const int32_t scale = std::max<int32_t>(1, int32_t(x.cpu.r[3]));
        writeCMTime(x, sret, int64_t(sec * double(scale)), scale, 1);
    });
    p.defineFunction("_CMTimeCompare", [](Ctx& x) { x.cpu.r[0] = 0; });
    auto defCMTimeConst = [&](const char* name, int64_t val, int32_t scale, uint32_t flags) {
        const uint32_t addr = p.defineDataBlock(name, 24);
        p.mem().write<int64_t>(addr + 0, val);
        p.mem().write<int32_t>(addr + 8, scale);
        p.mem().write<uint32_t>(addr + 12, flags);
        p.mem().write<int64_t>(addr + 16, 0);
    };
    defCMTimeConst("_kCMTimeZero", 0, 1, 1);
    defCMTimeConst("_kCMTimeInvalid", 0, 0, 0);
    defCMTimeConst("_kCMTimePositiveInfinity", 0, 0, 5);
    defCMTimeConst("_kCMTimeNegativeInfinity", 0, 0, 9);
    defCMTimeConst("_kCMTimeIndefinite", 0, 0, 17);

    struct AVAssetData : ObjC::HostObject { id url = 0; };
    ObjC::ClassBuilder avAsset = r.defineClass("AVAsset", "NSObject");
    avAsset.classMethod("assetWithURL:", [](Ctx& x) {
        const id inst = rt(x).createInstance(rt(x).getClass("AVURLAsset"));
        rt(x).host<AVAssetData>(inst).url = x.cpu.r[2] ? rt(x).retain(x.cpu, x.cpu.r[2]) : 0;
        x.cpu.r[0] = autoreleased(x, inst);
    }, "@@:@");
    avAsset.method("duration", [writeCMTime](Ctx& x) { writeCMTime(x, x.cpu.r[0], 0, 600, 1); }, "{?=qiIq}@:");
    avAsset.method("tracks", [](Ctx& x) { x.cpu.r[0] = Foundation::makeArray(x, {}); }, "@@:");
    avAsset.method("tracksWithMediaType:", [](Ctx& x) { x.cpu.r[0] = Foundation::makeArray(x, {}); }, "@@:@");
    avAsset.method("isPlayable", [](Ctx& x) { x.cpu.r[0] = 1; }, "c@:");
    avAsset.method("statusOfValueForKey:error:", [](Ctx& x) {
        const GuestPtr errOut = x.cpu.r[3];
        if (errOut) x.mem.write<uint32_t>(errOut, 0);
        x.cpu.r[0] = 2; // AVKeyValueStatusLoaded
    }, "i@:@^@");
    avAsset.method("loadValuesAsynchronouslyForKeys:completionHandler:", [](Ctx& x) {
        const uint32_t handler = x.cpu.r[3];
        if (!handler) return;
        const uint32_t block = ObjC::blockCopy(x, handler);
        Process* proc = &x.proc;
        Foundation::mainRunLoop(x.proc).post([proc, block](CPU::Cpu& c) {
            Ctx cx{*proc, c, proc->mem()};
            ObjC::callBlock(cx, block, {});
            ObjC::blockRelease(cx, block);
        });
    }, "v@:@@?");
    avAsset.method("cancelLoading", [](Ctx&) {}, "v@:");

    ObjC::ClassBuilder avUrlAsset = r.defineClass("AVURLAsset", "AVAsset");
    avUrlAsset.classMethod("URLAssetWithURL:options:", [](Ctx& x) {
        const id inst = rt(x).createInstance(x.cpu.r[0]);
        rt(x).host<AVAssetData>(inst).url = x.cpu.r[2] ? rt(x).retain(x.cpu, x.cpu.r[2]) : 0;
        x.cpu.r[0] = autoreleased(x, inst);
    }, "@@:@@");
    avUrlAsset.method("initWithURL:options:", [](Ctx& x) {
        const id self = x.cpu.r[0];
        rt(x).host<AVAssetData>(self).url = x.cpu.r[2] ? rt(x).retain(x.cpu, x.cpu.r[2]) : 0;
        x.cpu.r[0] = self;
    }, "@@:@@");
    avUrlAsset.method("URL", [](Ctx& x) {
        auto* d = rt(x).hostIf<AVAssetData>(x.cpu.r[0]);
        x.cpu.r[0] = d ? d->url : 0;
    }, "@@:");

    struct AVPlayerItemData : ObjC::HostObject { id asset = 0; };
    ObjC::ClassBuilder avItem = r.defineClass("AVPlayerItem", "NSObject");
    avItem.classMethod("playerItemWithAsset:", [](Ctx& x) {
        const id inst = rt(x).createInstance(x.cpu.r[0]);
        rt(x).host<AVPlayerItemData>(inst).asset = x.cpu.r[2] ? rt(x).retain(x.cpu, x.cpu.r[2]) : 0;
        x.cpu.r[0] = autoreleased(x, inst);
    }, "@@:@");
    avItem.classMethod("playerItemWithURL:", [](Ctx& x) {
        const id asset = id(rt(x).send(x.cpu, rt(x).getClass("AVURLAsset"), "URLAssetWithURL:options:", {x.cpu.r[2], 0}));
        const id inst = rt(x).createInstance(x.cpu.r[0]);
        rt(x).host<AVPlayerItemData>(inst).asset = asset ? rt(x).retain(x.cpu, asset) : 0;
        x.cpu.r[0] = autoreleased(x, inst);
    }, "@@:@");
    avItem.method("initWithAsset:", [](Ctx& x) {
        const id self = x.cpu.r[0];
        rt(x).host<AVPlayerItemData>(self).asset = x.cpu.r[2] ? rt(x).retain(x.cpu, x.cpu.r[2]) : 0;
        x.cpu.r[0] = self;
    }, "@@:@");
    avItem.method("initWithURL:", [](Ctx& x) {
        const id self = x.cpu.r[0];
        const id asset = id(rt(x).send(x.cpu, rt(x).getClass("AVURLAsset"), "URLAssetWithURL:options:", {x.cpu.r[2], 0}));
        rt(x).host<AVPlayerItemData>(self).asset = asset ? rt(x).retain(x.cpu, asset) : 0;
        x.cpu.r[0] = self;
    }, "@@:@");
    avItem.method("asset", [](Ctx& x) {
        auto* d = rt(x).hostIf<AVPlayerItemData>(x.cpu.r[0]);
        x.cpu.r[0] = d ? d->asset : 0;
    }, "@@:");
    avItem.method("status", [](Ctx& x) { x.cpu.r[0] = 1; /* AVPlayerItemStatusReadyToPlay */ }, "i@:");
    avItem.method("error", [](Ctx& x) { x.cpu.r[0] = 0; }, "@@:");
    avItem.method("duration", [writeCMTime](Ctx& x) { writeCMTime(x, x.cpu.r[0], 0, 600, 1); }, "{?=qiIq}@:");
    avItem.method("currentTime", [writeCMTime](Ctx& x) { writeCMTime(x, x.cpu.r[0], 0, 600, 1); }, "{?=qiIq}@:");
    avItem.method("tracks", [](Ctx& x) { x.cpu.r[0] = Foundation::makeArray(x, {}); }, "@@:");
    avItem.method("presentationSize", [](Ctx& x) {
        const GuestPtr sret = x.cpu.r[0];
        x.mem.write<float>(sret + 0, 0.0f);
        x.mem.write<float>(sret + 4, 0.0f);
    }, "{CGSize=ff}@:");
    avItem.method("seekToTime:", [](Ctx&) {}, "v@:{?=qiIq}");
    avItem.method("seekToTime:completionHandler:", [](Ctx& x) {
        const uint32_t sp = x.cpu.r[CPU::kSP];
        const uint32_t handler = x.mem.read<uint32_t>(sp + 16);
        if (handler) ObjC::callBlock(x, handler, {1});
    }, "v@:{?=qiIq}@?");

    struct AVPlayerData : ObjC::HostObject {
        id currentItem = 0;
        float rate = 0.0f, volume = 1.0f;
        int32_t actionAtItemEnd = 0;
        bool muted = false;
    };
    auto notifyFinish = [](Ctx& x, id self) {
        auto* d = rt(x).hostIf<AVPlayerData>(self);
        if (!d || !d->currentItem) return;
        const id item = rt(x).retain(x.cpu, d->currentItem);
        Process* proc = &x.proc;
        Foundation::mainRunLoop(x.proc).addTimer(0.05, 0, false, [proc, item](CPU::Cpu& c) {
            Ctx cx{*proc, c, proc->mem()};
            Foundation::postNotification(cx, "AVPlayerItemDidPlayToEndTimeNotification", item);
            rt(cx).release(c, item);
        });
    };
    ObjC::ClassBuilder avPlayer = r.defineClass("AVPlayer", "NSObject");
    avPlayer.classMethod("playerWithPlayerItem:", [](Ctx& x) {
        const id inst = rt(x).createInstance(x.cpu.r[0]);
        rt(x).host<AVPlayerData>(inst).currentItem = x.cpu.r[2] ? rt(x).retain(x.cpu, x.cpu.r[2]) : 0;
        x.cpu.r[0] = autoreleased(x, inst);
    }, "@@:@");
    avPlayer.classMethod("playerWithURL:", [](Ctx& x) {
        const id item = id(rt(x).send(x.cpu, rt(x).getClass("AVPlayerItem"), "playerItemWithURL:", {x.cpu.r[2]}));
        const id inst = rt(x).createInstance(x.cpu.r[0]);
        rt(x).host<AVPlayerData>(inst).currentItem = item ? rt(x).retain(x.cpu, item) : 0;
        x.cpu.r[0] = autoreleased(x, inst);
    }, "@@:@");
    avPlayer.method("initWithPlayerItem:", [](Ctx& x) {
        const id self = x.cpu.r[0];
        rt(x).host<AVPlayerData>(self).currentItem = x.cpu.r[2] ? rt(x).retain(x.cpu, x.cpu.r[2]) : 0;
        x.cpu.r[0] = self;
    }, "@@:@");
    avPlayer.method("initWithURL:", [](Ctx& x) {
        const id self = x.cpu.r[0];
        const id item = id(rt(x).send(x.cpu, rt(x).getClass("AVPlayerItem"), "playerItemWithURL:", {x.cpu.r[2]}));
        rt(x).host<AVPlayerData>(self).currentItem = item ? rt(x).retain(x.cpu, item) : 0;
        x.cpu.r[0] = self;
    }, "@@:@");
    avPlayer.method("currentItem", [](Ctx& x) {
        auto* d = rt(x).hostIf<AVPlayerData>(x.cpu.r[0]);
        x.cpu.r[0] = d ? d->currentItem : 0;
    }, "@@:");
    avPlayer.method("replaceCurrentItemWithPlayerItem:", [](Ctx& x) {
        auto& d = rt(x).host<AVPlayerData>(x.cpu.r[0]);
        const id next = x.cpu.r[2] ? rt(x).retain(x.cpu, x.cpu.r[2]) : 0;
        if (d.currentItem) rt(x).release(x.cpu, d.currentItem);
        d.currentItem = next;
    }, "v@:@");
    avPlayer.method("play", [notifyFinish](Ctx& x) {
        const id self = x.cpu.r[0];
        rt(x).host<AVPlayerData>(self).rate = 1.0f;
        notifyFinish(x, self);
    }, "v@:");
    avPlayer.method("pause", [](Ctx& x) { rt(x).host<AVPlayerData>(x.cpu.r[0]).rate = 0.0f; }, "v@:");
    avPlayer.method("rate", [](Ctx& x) {
        float v = rt(x).host<AVPlayerData>(x.cpu.r[0]).rate;
        uint32_t bits; std::memcpy(&bits, &v, 4);
        x.cpu.r[0] = bits;
    }, "f@:");
    avPlayer.method("setRate:", [notifyFinish](Ctx& x) {
        const id self = x.cpu.r[0];
        float v; std::memcpy(&v, &x.cpu.r[2], 4);
        rt(x).host<AVPlayerData>(self).rate = v;
        if (v > 0.0f) notifyFinish(x, self);
    }, "v@:f");
    avPlayer.method("status", [](Ctx& x) { x.cpu.r[0] = 1; /* AVPlayerStatusReadyToPlay */ }, "i@:");
    avPlayer.method("error", [](Ctx& x) { x.cpu.r[0] = 0; }, "@@:");
    avPlayer.method("actionAtItemEnd", [](Ctx& x) { x.cpu.r[0] = uint32_t(rt(x).host<AVPlayerData>(x.cpu.r[0]).actionAtItemEnd); }, "i@:");
    avPlayer.method("setActionAtItemEnd:", [](Ctx& x) { rt(x).host<AVPlayerData>(x.cpu.r[0]).actionAtItemEnd = int32_t(x.cpu.r[2]); }, "v@:i");
    avPlayer.method("isMuted", [](Ctx& x) { x.cpu.r[0] = rt(x).host<AVPlayerData>(x.cpu.r[0]).muted ? 1 : 0; }, "c@:");
    avPlayer.method("setMuted:", [](Ctx& x) { rt(x).host<AVPlayerData>(x.cpu.r[0]).muted = (x.cpu.r[2] & 0xFF) != 0; }, "v@:c");
    avPlayer.method("volume", [](Ctx& x) {
        float v = rt(x).host<AVPlayerData>(x.cpu.r[0]).volume;
        uint32_t bits; std::memcpy(&bits, &v, 4);
        x.cpu.r[0] = bits;
    }, "f@:");
    avPlayer.method("setVolume:", [](Ctx& x) {
        float v; std::memcpy(&v, &x.cpu.r[2], 4);
        rt(x).host<AVPlayerData>(x.cpu.r[0]).volume = v;
    }, "v@:f");
    avPlayer.method("allowsExternalPlayback", [](Ctx& x) { x.cpu.r[0] = 0; }, "c@:");
    avPlayer.method("setAllowsExternalPlayback:", [](Ctx&) {}, "v@:c");
    avPlayer.method("allowsAirPlayVideo", [](Ctx& x) { x.cpu.r[0] = 0; }, "c@:");
    avPlayer.method("setAllowsAirPlayVideo:", [](Ctx&) {}, "v@:c");
    avPlayer.method("currentTime", [writeCMTime](Ctx& x) { writeCMTime(x, x.cpu.r[0], 0, 600, 1); }, "{?=qiIq}@:");
    avPlayer.method("seekToTime:", [](Ctx&) {}, "v@:{?=qiIq}");
    avPlayer.method("seekToTime:toleranceBefore:toleranceAfter:", [](Ctx&) {}, "v@:{?=qiIq}{?=qiIq}{?=qiIq}");
    avPlayer.method("addPeriodicTimeObserverForInterval:queue:usingBlock:", [](Ctx& x) { x.cpu.r[0] = rt(x).createInstance(rt(x).nsObject); }, "@@:{?=qiIq}@@?");
    avPlayer.method("removeTimeObserver:", [](Ctx&) {}, "v@:@");

    ObjC::ClassBuilder avLayer = r.defineClass("AVPlayerLayer", "CALayer");
    avLayer.classMethod("playerLayerWithPlayer:", [](Ctx& x) {
        const id inst = id(rt(x).send(x.cpu, id(rt(x).send(x.cpu, x.cpu.r[0], rt(x).selAlloc)), rt(x).selInit));
        if (x.cpu.r[2]) rt(x).setAssociated(x.cpu, inst, 0xA701, x.cpu.r[2], 1);
        x.cpu.r[0] = autoreleased(x, inst);
    }, "@@:@");
    avLayer.method("player", [](Ctx& x) { x.cpu.r[0] = rt(x).getAssociated(x.cpu.r[0], 0xA701); }, "@@:");
    avLayer.method("setPlayer:", [](Ctx& x) { rt(x).setAssociated(x.cpu, x.cpu.r[0], 0xA701, x.cpu.r[2], 1); }, "v@:@");
    avLayer.method("videoGravity", [](Ctx& x) {
        const id g = rt(x).getAssociated(x.cpu.r[0], 0xA702);
        x.cpu.r[0] = g ? g : Foundation::constString(x.proc, "AVLayerVideoGravityResizeAspect");
    }, "@@:");
    avLayer.method("setVideoGravity:", [](Ctx& x) { rt(x).setAssociated(x.cpu, x.cpu.r[0], 0xA702, x.cpu.r[2], 3); }, "v@:@");
    avLayer.method("isReadyForDisplay", [](Ctx& x) { x.cpu.r[0] = 1; }, "c@:");

    for (const char* n : {"AVPlayerItemDidPlayToEndTimeNotification", "AVPlayerItemFailedToPlayToEndTimeNotification",
                          "AVPlayerItemTimeJumpedNotification", "AVPlayerItemPlaybackStalledNotification",
                          "AVLayerVideoGravityResizeAspect", "AVLayerVideoGravityResizeAspectFill", "AVLayerVideoGravityResize",
                          "AVMediaTypeVideo", "AVMediaTypeAudio", "AVMediaTypeText", "AVURLAssetPreferPreciseDurationAndTimingKey"}) {
        const uint32_t cell = p.defineDataBlock(std::string("_") + n, 4);
        p.mem().write<uint32_t>(cell, Foundation::constString(p, n));
    }
}

} // namespace Hydrogen::Runtime::UIKit
