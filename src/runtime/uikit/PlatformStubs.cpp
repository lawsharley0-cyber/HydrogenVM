// HydrogenVM — Audio emulation (AudioToolbox, AVAudioSession, AVAudioPlayer,
// OpenAL) and platform framework stubs (GameKit, StoreKit, MessageUI,
// SystemConfiguration).
#include "UIKit.hpp"
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

// ====================================================== PCM / WAV Decoder ===

struct DecodedAudio {
    int sampleRate = 44100;
    int channels = 2;
    double duration = 0.0;
    std::vector<int16_t> stereoPcm;   // interleaved L, R
};

uint32_t readLE32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
uint16_t readLE16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }

DecodedAudio decodeAudioBytes(const uint8_t* data, size_t n) {
    DecodedAudio out;
    if (!data || n < 12) return out;

    // RIFF / WAVE parser
    if (std::memcmp(data, "RIFF", 4) == 0 && std::memcmp(data + 8, "WAVE", 4) == 0) {
        size_t pos = 12;
        uint16_t audioFmt = 1, numCh = 1, bitsPerSample = 16;
        uint32_t sr = 44100;
        const uint8_t* pcmPtr = nullptr;
        size_t pcmBytes = 0;

        while (pos + 8 <= n) {
            const uint8_t* hdr = data + pos;
            const uint32_t chunkSize = readLE32(hdr + 4);
            pos += 8;
            if (pos + chunkSize > n) break;
            if (std::memcmp(hdr, "fmt ", 4) == 0 && chunkSize >= 16) {
                audioFmt = readLE16(data + pos);
                numCh = std::max<uint16_t>(1, readLE16(data + pos + 2));
                sr = std::max<uint32_t>(8000, readLE32(data + pos + 4));
                bitsPerSample = readLE16(data + pos + 14);
            } else if (std::memcmp(hdr, "data", 4) == 0) {
                pcmPtr = data + pos;
                pcmBytes = chunkSize;
            }
            pos += (chunkSize + 1) & ~size_t(1);
        }
        out.sampleRate = int(sr);
        out.channels = int(numCh);
        if (pcmPtr && pcmBytes > 0 && audioFmt == 1 && (bitsPerSample == 8 || bitsPerSample == 16)) {
            const size_t bytesPerFrame = size_t(numCh) * (bitsPerSample / 8);
            const size_t frames = bytesPerFrame ? (pcmBytes / bytesPerFrame) : 0;
            out.duration = double(frames) / double(sr);
            out.stereoPcm.resize(frames * 2);
            for (size_t f = 0; f < frames; ++f) {
                const uint8_t* fp = pcmPtr + f * bytesPerFrame;
                int16_t l = 0, r = 0;
                if (bitsPerSample == 16) {
                    std::memcpy(&l, fp, 2);
                    r = (numCh >= 2) ? (std::memcpy(&r, fp + 2, 2), r) : l;
                } else {
                    l = int16_t((int(fp[0]) - 128) * 256);
                    r = (numCh >= 2) ? int16_t((int(fp[1]) - 128) * 256) : l;
                }
                out.stereoPcm[f * 2 + 0] = l;
                out.stereoPcm[f * 2 + 1] = r;
            }
            return out;
        }
    }

    // Fallback for compressed/other containers: estimate duration (~128 kbps)
    out.duration = std::max(0.05, double(n) / 16000.0);
    return out;
}

void emitPcmToHost(Process& proc, const std::vector<int16_t>& stereoPcm, int sampleRate, float volume, float pan) {
    if (!proc.config().audioOutput || stereoPcm.empty()) return;
    const float lGain = std::clamp(volume * (pan <= 0 ? 1.0f : (1.0f - pan)), 0.0f, 1.0f);
    const float rGain = std::clamp(volume * (pan >= 0 ? 1.0f : (1.0f + pan)), 0.0f, 1.0f);
    if (std::abs(lGain - 1.0f) < 1e-3f && std::abs(rGain - 1.0f) < 1e-3f) {
        proc.config().audioOutput(stereoPcm.data(), stereoPcm.size() / 2, sampleRate);
        return;
    }
    std::vector<int16_t> scaled(stereoPcm.size());
    for (size_t i = 0; i + 1 < stereoPcm.size(); i += 2) {
        scaled[i + 0] = int16_t(std::clamp(float(stereoPcm[i + 0]) * lGain, -32768.0f, 32767.0f));
        scaled[i + 1] = int16_t(std::clamp(float(stereoPcm[i + 1]) * rGain, -32768.0f, 32767.0f));
    }
    proc.config().audioOutput(scaled.data(), scaled.size() / 2, sampleRate);
}

// ============================================================= AudioToolbox ===

struct SystemSoundEntry {
    DecodedAudio audio;
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
    bool running = false;
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
        }
    }
    st.sounds[sid] = std::move(entry);
    if (outId) x.mem.write<uint32_t>(outId, sid);
    return 0;
}
void ss_play(Ctx& x, uint32_t sid) {
    auto& st = ATS(x);
    auto it = st.sounds.find(sid);
    if (it != st.sounds.end()) {
        emitPcmToHost(x.proc, it->second.audio.stereoPcm, it->second.audio.sampleRate, 1.0f, 0.0f);
        if (it->second.completionProc) {
            const uint32_t fn = it->second.completionProc, ud = it->second.clientData;
            Process* p = &x.proc;
            Foundation::mainRunLoop(x.proc).post([p, fn, sid, ud](CPU::Cpu& c) { p->callGuest(c, fn, {sid, ud}); });
        }
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
    }
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
int32_t aq_enqueueBuffer(Ctx& x, uint32_t qid, GuestPtr buf, uint32_t, GuestPtr) {
    auto& st = ATS(x);
    auto it = st.queues.find(qid);
    if (it == st.queues.end() || !buf) return -50;
    AudioQueueInstance& q = it->second;
    const uint32_t dataPtr = x.mem.read<uint32_t>(buf + 4);
    const uint32_t byteSize = x.mem.read<uint32_t>(buf + 8);
    if (dataPtr && byteSize > 0 && q.bitsPerChannel == 16) {
        const size_t samples = byteSize / 2;
        const int16_t* src = reinterpret_cast<const int16_t*>(x.mem.host(dataPtr));
        std::vector<int16_t> stereo;
        if (q.channels >= 2) {
            stereo.assign(src, src + (samples & ~size_t(1)));
        } else {
            stereo.resize(samples * 2);
            for (size_t i = 0; i < samples; ++i) stereo[i * 2] = stereo[i * 2 + 1] = src[i];
        }
        emitPcmToHost(x.proc, stereo, q.sampleRate, q.volume, 0.0f);
    }
    if (q.callback) {
        const uint32_t cb = q.callback, ud = q.userData;
        Process* p = &x.proc;
        Foundation::mainRunLoop(x.proc).post([p, cb, ud, qid, buf](CPU::Cpu& c) {
            if (p->service<AudioToolboxState>().queues.count(qid))
                p->callGuest(c, cb, {ud, qid, buf});
        });
    }
    return 0;
}
int32_t aq_start(Ctx& x, uint32_t qid, GuestPtr) { if (ATS(x).queues.count(qid)) ATS(x).queues[qid].running = true; return 0; }
int32_t aq_pause(Ctx& x, uint32_t qid) { if (ATS(x).queues.count(qid)) ATS(x).queues[qid].running = false; return 0; }
int32_t aq_stop(Ctx& x, uint32_t qid, bool) { if (ATS(x).queues.count(qid)) ATS(x).queues[qid].running = false; return 0; }
int32_t aq_flush(Ctx&, uint32_t) { return 0; }
int32_t aq_reset(Ctx&, uint32_t) { return 0; }
int32_t aq_dispose(Ctx& x, uint32_t qid, bool) {
    auto& st = ATS(x);
    if (st.queues.count(qid)) {
        for (const auto& b : st.queues[qid].buffers) if (b.guestAddr) x.proc.heap().free(b.guestAddr);
        st.queues.erase(qid);
    }
    return 0;
}
int32_t aq_setParam(Ctx& x, uint32_t qid, uint32_t param, float val) {
    if (ATS(x).queues.count(qid) && param == 1 /* kAudioQueueParam_Volume */) ATS(x).queues[qid].volume = val;
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
    static id shared = 0;
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
    std::shared_ptr<Foundation::RunLoop::Timer> timer;
};
PlayerData& PD(Ctx& x, id p) { return rt(x).host<PlayerData>(p); }

void loadPlayerDecoded(PlayerData& d, const uint8_t* bytes, size_t len) {
    d.decoded = decodeAudioBytes(bytes, len);
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
            loadPlayerDecoded(d, bytes.data(), bytes.size());
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
        loadPlayerDecoded(d, bytes.data(), bytes.size());
    }
    return self;
}
bool ap_play(Ctx& x, id self, SEL) {
    PlayerData& d = PD(x, self);
    d.playing = true;
    d.loopsRemaining = d.loops;
    emitPcmToHost(x.proc, d.decoded.stereoPcm, d.decoded.sampleRate, d.volume, d.pan);
    if (d.timer) d.timer->valid = false;
    const double step = 0.05;
    Process* p = &x.proc;
    d.timer = Foundation::mainRunLoop(x.proc).addTimer(step, step, true, [p, self, step](CPU::Cpu& c) {
        Ctx cx{*p, c, p->mem()};
        PlayerData* pd = rt(cx).hostIf<PlayerData>(self);
        if (!pd || !pd->playing) return;
        pd->time += step * std::max(0.25f, pd->rate);
        if (pd->duration <= 0 || pd->time >= pd->duration) {
            if (pd->loops < 0 || pd->loopsRemaining > 0) {
                if (pd->loopsRemaining > 0) --pd->loopsRemaining;
                pd->time = 0;
                emitPcmToHost(*p, pd->decoded.stereoPcm, pd->decoded.sampleRate, pd->volume, pd->pan);
            } else {
                pd->playing = false;
                pd->time = 0;
                if (pd->timer) pd->timer->valid = false;
                if (pd->delegate && rt(cx).respondsTo(pd->delegate, rt(cx).sel("audioPlayerDidFinishPlaying:successfully:")))
                    rt(cx).send(c, pd->delegate, "audioPlayerDidFinishPlaying:successfully:", {self, 1});
            }
        }
    });
    return true;
}
bool ap_playAt(Ctx& x, id self, SEL s, double) { return ap_play(x, self, s); }
void ap_pause(Ctx& x, id self, SEL) {
    PlayerData& d = PD(x, self);
    d.playing = false;
    if (d.timer) d.timer->valid = false;
}
void ap_stop(Ctx& x, id self, SEL) {
    PlayerData& d = PD(x, self);
    d.playing = false;
    if (d.timer) d.timer->valid = false;
}
bool ap_prepare(Ctx&, id, SEL) { return true; }
bool ap_isPlaying(Ctx& x, id self, SEL) { return PD(x, self).playing; }
float ap_volume(Ctx& x, id self, SEL) { return PD(x, self).volume; }
void ap_setVolume(Ctx& x, id self, SEL, float v) { PD(x, self).volume = std::clamp(v, 0.0f, 1.0f); }
float ap_pan(Ctx& x, id self, SEL) { return PD(x, self).pan; }
void ap_setPan(Ctx& x, id self, SEL, float v) { PD(x, self).pan = std::clamp(v, -1.0f, 1.0f); }
float ap_rate(Ctx& x, id self, SEL) { return PD(x, self).rate; }
void ap_setRate(Ctx& x, id self, SEL, float v) { PD(x, self).rate = std::clamp(v, 0.25f, 4.0f); }
int32_t ap_loops(Ctx& x, id self, SEL) { return PD(x, self).loops; }
void ap_setLoops(Ctx& x, id self, SEL, int32_t n) { PD(x, self).loops = n; }
double ap_time(Ctx& x, id self, SEL) { return PD(x, self).time; }
void ap_setTime(Ctx& x, id self, SEL, double t) { PD(x, self).time = std::max(0.0, t); }
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
};

struct ALSource {
    uint32_t id = 0;
    uint32_t buffer = 0;
    uint32_t state = AL_INITIAL;
    float pitch = 1.0f, gain = 1.0f;
    float pos[3] = {0, 0, 0}, vel[3] = {0, 0, 0};
    bool looping = false, relative = false;
    std::vector<uint32_t> queuedBuffers;
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
void al_bufferData(Ctx& x, uint32_t bid, uint32_t format, GuestPtr data, int32_t size, int32_t freq) {
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
    for (int32_t i = 0; i < n; ++i) st.sources.erase(x.mem.read<uint32_t>(ids + uint32_t(i * 4)));
}
bool al_isSource(Ctx& x, uint32_t id) { return id != 0 && OAL(x).sources.count(id) > 0; }
void al_sourcef(Ctx& x, uint32_t sid, uint32_t param, float val) {
    if (!OAL(x).sources.count(sid)) return;
    ALSource& s = OAL(x).sources[sid];
    if (param == AL_PITCH) s.pitch = val;
    else if (param == AL_GAIN) s.gain = val;
}
void al_source3f(Ctx& x, uint32_t sid, uint32_t param, float v1, float v2, float v3) {
    if (!OAL(x).sources.count(sid)) return;
    ALSource& s = OAL(x).sources[sid];
    if (param == AL_POSITION) { s.pos[0] = v1; s.pos[1] = v2; s.pos[2] = v3; }
    else if (param == AL_VELOCITY) { s.vel[0] = v1; s.vel[1] = v2; s.vel[2] = v3; }
}
void al_sourcefv(Ctx& x, uint32_t sid, uint32_t param, GuestPtr vals) {
    if (vals) al_source3f(x, sid, param, x.mem.read<float>(vals), x.mem.read<float>(vals + 4), x.mem.read<float>(vals + 8));
}
void al_sourcei(Ctx& x, uint32_t sid, uint32_t param, int32_t val) {
    if (!OAL(x).sources.count(sid)) return;
    ALSource& s = OAL(x).sources[sid];
    if (param == AL_BUFFER) s.buffer = uint32_t(val);
    else if (param == AL_LOOPING) s.looping = val != 0;
    else if (param == AL_SOURCE_RELATIVE) s.relative = val != 0;
}
void al_getSourcef(Ctx& x, uint32_t sid, uint32_t param, GuestPtr val) {
    if (!val || !OAL(x).sources.count(sid)) return;
    const ALSource& s = OAL(x).sources[sid];
    x.mem.write<float>(val, param == AL_PITCH ? s.pitch : param == AL_GAIN ? s.gain : 0.0f);
}
void al_getSourcei(Ctx& x, uint32_t sid, uint32_t param, GuestPtr val) {
    if (!val || !OAL(x).sources.count(sid)) return;
    const ALSource& s = OAL(x).sources[sid];
    int32_t v = 0;
    if (param == AL_SOURCE_STATE) v = int32_t(s.state);
    else if (param == AL_BUFFER) v = int32_t(s.buffer);
    else if (param == AL_LOOPING) v = s.looping ? 1 : 0;
    else if (param == AL_BUFFERS_QUEUED || param == AL_BUFFERS_PROCESSED) v = int32_t(s.queuedBuffers.size());
    x.mem.write<int32_t>(val, v);
}
void al_sourcePlay(Ctx& x, uint32_t sid) {
    auto& st = OAL(x);
    if (!st.sources.count(sid)) return;
    ALSource& s = st.sources[sid];
    s.state = AL_PLAYING;
    if (s.buffer && st.buffers.count(s.buffer)) {
        const ALBuffer& b = st.buffers[s.buffer];
        emitPcmToHost(x.proc, b.stereoPcm, b.freq, s.gain * st.listenerGain, std::clamp(s.pos[0], -1.0f, 1.0f));
    }
}
void al_sourcePause(Ctx& x, uint32_t sid) { if (OAL(x).sources.count(sid)) OAL(x).sources[sid].state = AL_PAUSED; }
void al_sourceStop(Ctx& x, uint32_t sid) { if (OAL(x).sources.count(sid)) OAL(x).sources[sid].state = AL_STOPPED; }
void al_sourceRewind(Ctx& x, uint32_t sid) { if (OAL(x).sources.count(sid)) OAL(x).sources[sid].state = AL_INITIAL; }
void al_sourceQueueBuffers(Ctx& x, uint32_t sid, int32_t n, GuestPtr bids) {
    auto& st = OAL(x);
    if (!st.sources.count(sid) || !bids || n <= 0) return;
    ALSource& s = st.sources[sid];
    for (int32_t i = 0; i < n; ++i) {
        const uint32_t bid = x.mem.read<uint32_t>(bids + uint32_t(i * 4));
        s.queuedBuffers.push_back(bid);
        if (st.buffers.count(bid)) emitPcmToHost(x.proc, st.buffers[bid].stereoPcm, st.buffers[bid].freq, s.gain * st.listenerGain, 0.0f);
    }
}
void al_sourceUnqueueBuffers(Ctx& x, uint32_t sid, int32_t n, GuestPtr bids) {
    auto& st = OAL(x);
    if (!st.sources.count(sid) || !bids || n <= 0) return;
    ALSource& s = st.sources[sid];
    for (int32_t i = 0; i < n && !s.queuedBuffers.empty(); ++i) {
        x.mem.write<uint32_t>(bids + uint32_t(i * 4), s.queuedBuffers.front());
        s.queuedBuffers.erase(s.queuedBuffers.begin());
    }
}
void al_listenerf(Ctx& x, uint32_t param, float val) { if (param == AL_GAIN) OAL(x).listenerGain = val; }
void al_listener3f(Ctx&, uint32_t, float, float, float) {}
void al_listenerfv(Ctx&, uint32_t, GuestPtr) {}
void al_noop1(Ctx&, uint32_t) {}

// ---------------------------------------------------------------- GameKit ---
id gk_local(Ctx& x, id self, SEL) {
    static id shared = 0;
    if (!shared) { shared = rt(x).createInstance(self); rt(x).makeImmortal(shared); }
    return shared;
}
bool gk_no(Ctx&, id, SEL) { return false; }
void gk_authenticate(Ctx& x, id, SEL, uint32_t handler) {
    noteOnce(x, "Game Center (not available: sign-in fails)");
    if (!handler) return;
    const uint32_t b = ObjC::blockCopy(x, handler);
    Process* p = &x.proc;
    Foundation::mainRunLoop(x.proc).post([p, b](CPU::Cpu& c) {
        Ctx cx{*p, c, p->mem()};
        const id err = id(rt(cx).send(c, rt(cx).getClass("NSError"), "errorWithDomain:code:userInfo:",
                                      {makeString(cx, "GKErrorDomain"), 15, 0}));
        ObjC::callBlock(cx, b, {err});
        ObjC::blockRelease(cx, b);
    });
}
void gk_setHandler(Ctx& x, id self, SEL s, uint32_t handler) {
    if (!handler) return;
    noteOnce(x, "Game Center (not available: sign-in fails)");
    const uint32_t b = ObjC::blockCopy(x, handler);
    Process* p = &x.proc;
    Foundation::mainRunLoop(x.proc).post([p, b](CPU::Cpu& c) {
        Ctx cx{*p, c, p->mem()};
        const id err = id(rt(cx).send(c, rt(cx).getClass("NSError"), "errorWithDomain:code:userInfo:", {makeString(cx, "GKErrorDomain"), 2, 0}));
        ObjC::callBlock(cx, b, {0, err});
        ObjC::blockRelease(cx, b);
    });
    (void)self; (void)s;
}
id gk_nil(Ctx&, id, SEL) { return 0; }

// --------------------------------------------------------------- StoreKit ---
bool sk_canPay(Ctx& x, id, SEL) { noteOnce(x, "StoreKit (in-app purchases are disabled)"); return false; }
id sk_queue(Ctx& x, id self, SEL) {
    static id shared = 0;
    if (!shared) { shared = rt(x).createInstance(self); rt(x).makeImmortal(shared); }
    return shared;
}
void sk_ignoreObj(Ctx&, id, SEL, id) {}
void sk_noop(Ctx&, id, SEL) {}
id sk_empty(Ctx& x, id, SEL) { return Foundation::makeArray(x, {}); }

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
    p.def<&af_close>("_AudioFileClose");
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

    // GameKit
    ObjC::ClassBuilder gk = r.defineClass("GKLocalPlayer", "NSObject");
    gk.cm<&gk_local>("localPlayer");
    gk.m<&gk_no>("isAuthenticated", "c@:").m<&gk_authenticate>("authenticateWithCompletionHandler:", "v@:@?")
      .m<&gk_setHandler>("setAuthenticateHandler:", "v@:@?").m<&gk_nil>("playerID").m<&gk_nil>("alias").m<&gk_nil>("displayName")
      .m<&gk_no>("isUnderage", "c@:");

    // StoreKit
    ObjC::ClassBuilder skq = r.defineClass("SKPaymentQueue", "NSObject");
    skq.cm<&sk_canPay>("canMakePayments", "c@:").cm<&sk_queue>("defaultQueue");
    skq.m<&sk_ignoreObj>("addTransactionObserver:", "v@:@").m<&sk_ignoreObj>("removeTransactionObserver:", "v@:@")
       .m<&sk_ignoreObj>("addPayment:", "v@:@").m<&sk_ignoreObj>("finishTransaction:", "v@:@")
       .m<&sk_noop>("restoreCompletedTransactions", "v@:").m<&sk_empty>("transactions");

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
