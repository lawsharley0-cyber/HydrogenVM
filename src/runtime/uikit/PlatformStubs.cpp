// HydrogenVM — Audio emulation (AudioToolbox, AVAudioSession, AVAudioPlayer,
// OpenAL) and platform framework stubs (GameKit, StoreKit, MessageUI,
// SystemConfiguration).
#include "UIKit.hpp"
#include "runtime/audio/Audio.hpp"
#include <algorithm>
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
    bool initialized = false, running = false;
};

struct AudioToolboxState {
    explicit AudioToolboxState(Process&) {}
    uint32_t nextSoundId = 1;
    uint32_t nextQueueId = 1;
    uint32_t nextFileId = 1;
    uint32_t nextUnitId = 1;
    std::unordered_map<uint32_t, SystemSoundEntry> sounds;
    std::unordered_map<uint32_t, AudioQueueInstance> queues;
    std::unordered_map<uint32_t, AudioFileInstance> files;
    std::unordered_map<uint32_t, AudioUnitInstance> units;
};
AudioToolboxState& ATS(Ctx& x) { return x.proc.service<AudioToolboxState>(); }

int32_t as_init(Ctx&, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
int32_t as_setProperty(Ctx&, uint32_t, uint32_t, uint32_t) { return 0; }
int32_t as_getProperty(Ctx& x, uint32_t prop, GuestPtr size, GuestPtr out) {
    if (out && size) {
        const uint32_t n = x.mem.read<uint32_t>(size);
        if (prop == 0x63687372 /* 'chsr' hw sample rate */ && n >= 8) x.mem.write<double>(out, 44100.0);
        else if (prop == 0x6368766C /* 'chvl' output volume */ && n >= 4) x.mem.write<float>(out, 1.0f);
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
int32_t aq_freeBuffer(Ctx& x, uint32_t qid, GuestPtr buf) {
    auto& st = ATS(x);
    if (st.queues.count(qid) && buf) {
        auto& v = st.queues[qid].buffers;
        v.erase(std::remove_if(v.begin(), v.end(), [&](const auto& b) { return b.guestAddr == buf; }), v.end());
        x.proc.heap().free(buf);
    }
    return 0;
}
// Hands finished buffers back to the app's callback (on the main run loop).
void aqPump(Process& p, CPU::Cpu& c, uint32_t qid) {
    auto& st = p.service<AudioToolboxState>();
    auto it = st.queues.find(qid);
    if (it == st.queues.end()) return;
    const uint32_t cb = it->second.callback, ud = it->second.userData;
    for (uint64_t buf : MX(p).takeProcessed(it->second.voice)) {
        if (!cb || !st.queues.count(qid)) break;
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
int32_t aq_setProperty(Ctx&, uint32_t, uint32_t, GuestPtr, uint32_t) { return 0; }   // channel layout, converter quality, ...
int32_t aq_createTimeline(Ctx& x, uint32_t, GuestPtr out) { if (out) x.mem.write<uint32_t>(out, 0x71740001); return 0; }
int32_t aq_disposeTimeline(Ctx&, uint32_t, uint32_t) { return 0; }
// AudioQueueGetCurrentTime(q, timeline, AudioTimeStamp* outTime, Boolean* discontinuity): mSampleTime from the mixer.
int32_t aq_getCurrentTime(Ctx& x, uint32_t qid, uint32_t, GuestPtr outTime, GuestPtr disc) {
    if (outTime) {
        for (uint32_t i = 0; i < 64; i += 4) x.mem.write<uint32_t>(outTime + i, 0);
        double t = 0;
        auto& st = ATS(x);
        if (st.queues.count(qid)) t = MX(x.proc).positionSeconds(st.queues[qid].voice) * 44100.0;
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
    const uint32_t dataPtr = x.mem.read<uint32_t>(buf + 4);
    const uint32_t byteSize = x.mem.read<uint32_t>(buf + 8);
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
int32_t aq_start(Ctx& x, uint32_t qid, GuestPtr) { if (ATS(x).queues.count(qid)) { auto& q = ATS(x).queues[qid]; q.running = true; MX(x.proc).setPaused(q.voice, false); } return 0; }
int32_t aq_pause(Ctx& x, uint32_t qid) { if (ATS(x).queues.count(qid)) { auto& q = ATS(x).queues[qid]; q.running = false; MX(x.proc).setPaused(q.voice, true); } return 0; }
int32_t aq_stop(Ctx& x, uint32_t qid, bool) {
    if (!ATS(x).queues.count(qid)) return 0;
    auto& q = ATS(x).queues[qid];
    q.running = false;
    MX(x.proc).stop(q.voice);
    MX(x.proc).takeProcessed(q.voice);   // stopped buffers are not called back
    MX(x.proc).setPaused(q.voice, true);
    return 0;
}
int32_t aq_flush(Ctx&, uint32_t) { return 0; }
int32_t aq_reset(Ctx& x, uint32_t qid) { if (ATS(x).queues.count(qid)) MX(x.proc).stop(ATS(x).queues[qid].voice); return 0; }
int32_t aq_dispose(Ctx& x, uint32_t qid, bool) {
    auto& st = ATS(x);
    if (st.queues.count(qid)) {
        auto& q = st.queues[qid];
        if (q.pump) q.pump->valid = false;
        MX(x.proc).release(q.voice);
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
int32_t af_getProperty(Ctx& x, uint32_t fid, uint32_t prop, GuestPtr ioSize, GuestPtr outData) {
    auto& st = ATS(x);
    if (!st.files.count(fid) || !outData || !ioSize) return -50;
    const AudioFileInstance& af = st.files[fid];
    const uint32_t cap = x.mem.read<uint32_t>(ioSize);
    if (prop == 0x64666D74 /* 'dfmt' AudioStreamBasicDescription */ && cap >= 40) {
        x.mem.write<double>(outData + 0, double(af.decoded.sampleRate));
        x.mem.write<uint32_t>(outData + 8, 0x6C70636D /* 'lpcm' */);
        x.mem.write<uint32_t>(outData + 12, 0x0C /* signed int | packed */);
        x.mem.write<uint32_t>(outData + 16, 4);
        x.mem.write<uint32_t>(outData + 20, 1);
        x.mem.write<uint32_t>(outData + 24, 4);
        x.mem.write<uint32_t>(outData + 28, 2);
        x.mem.write<uint32_t>(outData + 32, 16);
        x.mem.write<uint32_t>(outData + 36, 0);
    } else if (prop == 0x62636E74 /* 'bcnt' byte count */ && cap >= 8) {
        x.mem.write<uint64_t>(outData, uint64_t(af.decoded.stereoPcm.size() * 2));
    } else if (prop == 0x65647572 /* 'edur' estimated duration */ && cap >= 8) {
        x.mem.write<double>(outData, af.decoded.duration);
    } else if (prop == 0x70636E74 /* 'pcnt' audio data packet count */ && cap >= 8) {
        x.mem.write<uint64_t>(outData, uint64_t(af.decoded.stereoPcm.size() / 2));
    } else if ((prop == 0x70737A65 /* 'psze' */ || prop == 0x706B7562 /* 'pkub' */) && cap >= 4) {
        x.mem.write<uint32_t>(outData, 4);
    } else if (prop == 0x66666D74 /* 'ffmt' file format */ && cap >= 4) {
        x.mem.write<uint32_t>(outData, 0x63616666 /* 'caff' */);
    }
    return 0;
}
int32_t af_readBytes(Ctx& x, uint32_t fid, bool, int64_t inStartingByte, GuestPtr ioNumBytes, GuestPtr outBuffer) {
    auto& st = ATS(x);
    if (!st.files.count(fid) || !ioNumBytes || !outBuffer) return -50;
    const auto& pcm = st.files[fid].decoded.stereoPcm;
    const size_t totalBytes = pcm.size() * 2;
    const uint32_t req = x.mem.read<uint32_t>(ioNumBytes);
    const size_t start = std::min(size_t(std::max<int64_t>(0, inStartingByte)), totalBytes);
    const size_t toCopy = std::min(size_t(req), totalBytes - start);
    if (toCopy > 0) x.mem.copyIn(outBuffer, reinterpret_cast<const uint8_t*>(pcm.data()) + start, toCopy);
    x.mem.write<uint32_t>(ioNumBytes, uint32_t(toCopy));
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
    const auto& pcm = st.files[fid].decoded.stereoPcm;
    const uint64_t frames = pcm.size() / 2;
    uint64_t want = x.mem.read<uint32_t>(ioNumPackets);
    if (numBytesIsInOut && ioNumBytes) want = std::min<uint64_t>(want, x.mem.read<uint32_t>(ioNumBytes) / 4);
    const uint64_t first = uint64_t(std::max<int64_t>(0, start));
    const uint64_t n = first >= frames ? 0 : std::min<uint64_t>(want, frames - first);
    if (n) x.mem.copyIn(out, reinterpret_cast<const uint8_t*>(pcm.data() + first * 2), size_t(n * 4));
    x.mem.write<uint32_t>(ioNumPackets, uint32_t(n));
    if (ioNumBytes) x.mem.write<uint32_t>(ioNumBytes, uint32_t(n * 4));
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
    const uint32_t uid = ATS(x).nextUnitId++;
    ATS(x).units[uid].id = uid;
    if (outUnit) x.mem.write<uint32_t>(outUnit, uid);
    return 0;
}
int32_t ac_dispose(Ctx& x, uint32_t uid) { ATS(x).units.erase(uid); return 0; }
int32_t au_init(Ctx& x, uint32_t uid) { if (ATS(x).units.count(uid)) ATS(x).units[uid].initialized = true; return 0; }
int32_t au_uninit(Ctx& x, uint32_t uid) { if (ATS(x).units.count(uid)) ATS(x).units[uid].initialized = false; return 0; }
int32_t au_start(Ctx& x, uint32_t uid) { if (ATS(x).units.count(uid)) ATS(x).units[uid].running = true; return 0; }
int32_t au_stop(Ctx& x, uint32_t uid) { if (ATS(x).units.count(uid)) ATS(x).units[uid].running = false; return 0; }
int32_t au_setProp(Ctx& x, uint32_t uid, uint32_t propId, uint32_t, uint32_t, GuestPtr data, uint32_t size) {
    if (ATS(x).units.count(uid) && propId == 23 /* kAudioUnitProperty_SetRenderCallback */ && data && size >= 8) {
        ATS(x).units[uid].renderCallback = x.mem.read<uint32_t>(data);
        ATS(x).units[uid].renderUserData = x.mem.read<uint32_t>(data + 4);
    }
    return 0;
}
int32_t au_getProp(Ctx&, uint32_t, uint32_t, uint32_t, uint32_t, GuestPtr, GuestPtr) { return 0; }

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
    float v = param == AL_PITCH ? s.pitch : param == AL_GAIN ? s.gain : 0.0f;
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
    if (s.voice) MX(x.proc).stop(s.voice);
    if (s.state != AL_INITIAL) s.state = AL_STOPPED;
}
void al_sourceRewind(Ctx& x, uint32_t sid) { if (OAL(x).sources.count(sid)) { ALSource& s = OAL(x).sources[sid]; alRelease(x, s); s.state = AL_INITIAL; } }
void al_sourceQueueBuffers(Ctx& x, uint32_t sid, int32_t n, GuestPtr bids) {
    auto& st = OAL(x);
    if (!st.sources.count(sid) || !bids || n <= 0) return;
    ALSource& s = st.sources[sid];
    alRefresh(x, s);
    s.buffer = 0;
    for (int32_t i = 0; i < n; ++i) {
        const uint32_t bid = x.mem.read<uint32_t>(bids + uint32_t(i * 4));
        s.queuedBuffers.push_back(bid);
        if (s.streaming && s.voice && (s.state == AL_PLAYING || s.state == AL_PAUSED) && st.buffers.count(bid))
            MX(x.proc).enqueue(s.voice, st.buffers[bid].pcm, st.buffers[bid].freq, bid);
    }
}
void al_sourceUnqueueBuffers(Ctx& x, uint32_t sid, int32_t n, GuestPtr bids) {
    auto& st = OAL(x);
    if (!st.sources.count(sid) || !bids || n <= 0) return;
    ALSource& s = st.sources[sid];
    const size_t avail = alProcessed(x, s);
    if (size_t(n) > avail) { st.lastError = 0xA004; /* AL_INVALID_VALUE */ return; }
    for (int32_t i = 0; i < n; ++i) {
        x.mem.write<uint32_t>(bids + uint32_t(i * 4), s.queuedBuffers.front());
        s.queuedBuffers.erase(s.queuedBuffers.begin());
    }
    if (s.voice && s.streaming && s.state != AL_STOPPED) MX(x.proc).takeProcessed(s.voice, size_t(n));
}
void al_listenerf(Ctx& x, uint32_t param, float val) { if (param == AL_GAIN) { OAL(x).listenerGain = val; MX(x.proc).setMasterGain(val); } }
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
uint32_t scr_create(Ctx& x, uint32_t, GuestPtr) { noteOnce(x, "SCNetworkReachability (network reported offline)"); return x.proc.heap().allocZeroed(16); }
uint32_t scr_createWithAddress(Ctx& x, uint32_t a, GuestPtr p) { return scr_create(x, a, p); }
bool scr_getFlags(Ctx& x, uint32_t, GuestPtr flags) { if (flags) x.mem.write<uint32_t>(flags, 0); return true; }
bool scr_setCallback(Ctx&, uint32_t, uint32_t, uint32_t) { return true; }
bool scr_schedule(Ctx&, uint32_t, uint32_t, uint32_t) { return true; }

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
    p.def<&aq_allocateBuffer>("_AudioQueueAllocateBuffer");
    p.def<&aq_freeBuffer>("_AudioQueueFreeBuffer");
    p.def<&aq_enqueueBuffer>("_AudioQueueEnqueueBuffer");
    p.defineFunction("_AudioQueueEnqueueBufferWithParameters", aq_enqueueWithParams);
    p.def<&aq_setProperty>("_AudioQueueSetProperty");
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

    // AudioFile & AudioUnit
    p.def<&af_openURL>("_AudioFileOpenURL");
    p.def<&af_getPropertyInfo>("_AudioFileGetPropertyInfo");
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
    p.def<&ac_findNext>("_AudioComponentFindNext");
    p.def<&ac_instanceNew>("_AudioComponentInstanceNew");
    p.def<&ac_dispose>("_AudioComponentInstanceDispose");
    p.def<&au_init>("_AudioUnitInitialize");
    p.def<&au_uninit>("_AudioUnitUninitialize");
    p.def<&au_start>("_AudioOutputUnitStart");
    p.def<&au_stop>("_AudioOutputUnitStop");
    p.def<&au_setProp>("_AudioUnitSetProperty");
    p.def<&au_getProp>("_AudioUnitGetProperty");

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
       .m<&avs_yesDoubleErr>("setPreferredIOBufferDuration:error:", "c@:d^@").m<&avs_volume>("outputVolume", "f@:");
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
    r.defineClass("SKProduct", "NSObject");
    r.defineClass("SKPayment", "NSObject");
    r.defineClass("SKMutablePayment", "SKPayment");

    // MessageUI
    for (const char* n : {"MFMailComposeViewController", "MFMessageComposeViewController"}) {
        ObjC::ClassBuilder mf = r.defineClass(n, "UIViewController");
        mf.cm<&mf_no>(std::string(n) == "MFMailComposeViewController" ? "canSendMail" : "canSendText", "c@:");
    }

    // SystemConfiguration
    p.def<&scr_create>("_SCNetworkReachabilityCreateWithName");
    p.def<&scr_createWithAddress>("_SCNetworkReachabilityCreateWithAddress");
    p.def<&scr_getFlags>("_SCNetworkReachabilityGetFlags");
    p.def<&scr_setCallback>("_SCNetworkReachabilitySetCallback");
    p.def<&scr_schedule>("_SCNetworkReachabilityScheduleWithRunLoop");
    p.def<&scr_schedule>("_SCNetworkReachabilityUnscheduleFromRunLoop");
}

} // namespace Hydrogen::Runtime::UIKit
