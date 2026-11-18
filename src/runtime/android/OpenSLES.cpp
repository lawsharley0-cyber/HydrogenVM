// HydrogenVM — OpenSL ES (libOpenSLES.so) for Android games. See Android.hpp.
//
// What games of this era use: an engine, an output mix, and audio players fed PCM through an
// Android simple buffer queue (Wwise, FMOD, Unity, Cocos2d-x, most custom engines). Each player
// is a stream voice in HydrogenVM's mixer; a guest thread delivers the buffer-queue callbacks as
// the mixer finishes buffers, as OpenSL's own callback thread does. Players of URIs or asset
// files, MIDI, 3D audio and recorders are reported as unsupported.
//
// An OpenSL interface is a pointer to a pointer to a table of functions; the first argument of
// every function is that interface. Here each interface is a guest cell holding the address of a
// guest table of host functions, and the cell's address identifies the object.
#include "runtime/android/Android.hpp"
#include "runtime/android/BionicInternal.hpp"
#include "runtime/audio/Audio.hpp"
#include "runtime/Support.hpp"
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Hydrogen::Runtime::Android {
namespace {

constexpr uint32_t kOk = 0, kParameterInvalid = 2, kMemoryFailure = 3, kPreconditions = 1, kContentUnsupported = 9,
                   kFeatureUnsupported = 12;
constexpr uint32_t kStateUnrealized = 1, kStateRealized = 2;
constexpr uint32_t kPlayStopped = 1, kPlayPaused = 2, kPlayPlaying = 3;
constexpr uint32_t kLocatorAndroidSimpleBQ = 0x800007BD, kLocatorBufferQueue = 0x6, kLocatorOutputMix = 0x4;
constexpr uint32_t kFormatPcm = 0x2;

enum class Itf { Object, Engine, OutputMix, Play, BufferQueue, Volume, AndroidConfiguration, Seek, PrefetchStatus, Record, Unknown };

struct Player {
    uint32_t voice = 0;
    uint32_t channels = 2, rate = 44100, bits = 16;
    uint32_t state = kPlayStopped;
    uint32_t callback = 0, context = 0;
    uint32_t queued = 0, played = 0;      // buffers enqueued and not yet finished; total finished
    uint64_t nextTag = 1;
    float gain = 1.0f, pan = 0.0f;
    bool muted = false;
};

struct Object {
    enum Kind { Engine, OutputMix, AudioPlayer } kind = Engine;
    uint32_t state = kStateUnrealized;
    std::map<Itf, uint32_t> itfs;         // interface -> its guest cell
    Player player;
};

struct Sl {
    explicit Sl(Process&) {}
    std::recursive_mutex mu;
    std::unordered_map<uint32_t, uint32_t> cellObject;   // interface cell -> object id
    std::unordered_map<uint32_t, Object> objects;
    std::map<Itf, uint32_t> tables;                      // interface -> guest function table
    std::map<uint32_t, Itf> iids;                        // guest SLInterfaceID (16-byte block) -> interface
    uint32_t nextId = 1;
    uint32_t pumpThread = 0;
};
Sl& S(Ctx& x) { return x.proc.service<Sl>(); }

// --------------------------------------------------------- objects and interfaces ---

Itf itfOf(Ctx& x, uint32_t iid) {
    auto& s = S(x);
    if (auto it = s.iids.find(iid); it != s.iids.end()) return it->second;
    // The same IID from another table: compare its 16 bytes.
    for (const auto& [p, kind] : s.iids)
        if (iid && !std::memcmp(x.mem.host(iid), x.mem.host(p), 16)) return kind;
    return Itf::Unknown;
}
Object* objectOf(Ctx& x, uint32_t cell, uint32_t* id = nullptr) {
    auto& s = S(x);
    auto it = s.cellObject.find(cell);
    if (it == s.cellObject.end()) return nullptr;
    if (id) *id = it->second;
    auto o = s.objects.find(it->second);
    return o == s.objects.end() ? nullptr : &o->second;
}
uint32_t cellFor(Ctx& x, uint32_t objId, Object& o, Itf kind) {
    auto& s = S(x);
    if (auto it = o.itfs.find(kind); it != o.itfs.end()) return it->second;
    const uint32_t cell = x.proc.allocStatic(4);
    x.mem.write<uint32_t>(cell, s.tables.at(kind));
    o.itfs[kind] = cell;
    s.cellObject[cell] = objId;
    return cell;
}
// Which interfaces an object of `kind` has.
bool supports(Object::Kind kind, Itf i) {
    switch (kind) {
        case Object::Engine: return i == Itf::Object || i == Itf::Engine;
        case Object::OutputMix: return i == Itf::Object || i == Itf::Volume;
        case Object::AudioPlayer:
            return i == Itf::Object || i == Itf::Play || i == Itf::BufferQueue || i == Itf::Volume ||
                   i == Itf::AndroidConfiguration;
    }
    return false;
}
uint32_t newObject(Ctx& x, Object::Kind kind, GuestPtr out, uint32_t numItfs, GuestPtr ids, GuestPtr required) {
    auto& s = S(x);
    std::lock_guard<std::recursive_mutex> lk(s.mu);
    // A required interface the object doesn't have fails the creation, as the spec says.
    for (uint32_t i = 0; i < numItfs; ++i) {
        const Itf want = itfOf(x, x.mem.read<uint32_t>(ids + i * 4));
        const bool req = required && x.mem.read<uint32_t>(required + i * 4) != 0;
        if (req && !supports(kind, want)) return kFeatureUnsupported;
    }
    const uint32_t id = s.nextId++;
    Object& o = s.objects[id];
    o.kind = kind;
    x.mem.write<uint32_t>(out, cellFor(x, id, o, Itf::Object));
    return kOk;
}

// --------------------------------------------------------------- SLObjectItf ---

uint32_t o_realize(Ctx& x, GuestPtr self, uint32_t) {
    auto& s = S(x);
    std::lock_guard<std::recursive_mutex> lk(s.mu);
    Object* o = objectOf(x, self);
    if (!o) return kParameterInvalid;
    o->state = kStateRealized;
    return kOk;
}
uint32_t o_resume(Ctx&, GuestPtr, uint32_t) { return kOk; }
uint32_t o_getState(Ctx& x, GuestPtr self, GuestPtr out) {
    auto& s = S(x);
    std::lock_guard<std::recursive_mutex> lk(s.mu);
    Object* o = objectOf(x, self);
    if (!o) return kParameterInvalid;
    x.mem.write<uint32_t>(out, o->state);
    return kOk;
}
uint32_t o_getInterface(Ctx& x, GuestPtr self, GuestPtr iid, GuestPtr out) {
    auto& s = S(x);
    std::lock_guard<std::recursive_mutex> lk(s.mu);
    uint32_t id = 0;
    Object* o = objectOf(x, self, &id);
    if (!o) return kParameterInvalid;
    if (o->state != kStateRealized) return kPreconditions;
    const Itf want = itfOf(x, iid);
    if (!supports(o->kind, want)) {
        x.proc.reportDefaulted("OpenSL ES interface the object doesn't have (" + std::to_string(int(want)) + ")");
        return kFeatureUnsupported;
    }
    x.mem.write<uint32_t>(out, cellFor(x, id, *o, want));
    return kOk;
}
uint32_t o_registerCallback(Ctx&, GuestPtr, GuestPtr, GuestPtr) { return kOk; }
void o_abort(Ctx&, GuestPtr) {}
void o_destroy(Ctx& x, GuestPtr self) {
    auto& s = S(x);
    std::lock_guard<std::recursive_mutex> lk(s.mu);
    uint32_t id = 0;
    Object* o = objectOf(x, self, &id);
    if (!o) return;
    if (o->kind == Object::AudioPlayer && o->player.voice) x.proc.mixer().release(o->player.voice);
    for (const auto& [k, cell] : o->itfs) s.cellObject.erase(cell);   // the cells themselves are never reused
    s.objects.erase(id);
}
uint32_t o_setPriority(Ctx&, GuestPtr, int32_t, uint32_t) { return kOk; }
uint32_t o_getPriority(Ctx& x, GuestPtr, GuestPtr p, GuestPtr pre) {
    if (p) x.mem.write<int32_t>(p, 0);
    if (pre) x.mem.write<uint32_t>(pre, 0);
    return kOk;
}
uint32_t o_setLossOfControl(Ctx&, GuestPtr, int16_t, GuestPtr, uint32_t) { return kOk; }

// --------------------------------------------------------------- SLEngineItf ---

uint32_t e_unsupported(Ctx&, GuestPtr) { return kFeatureUnsupported; }
uint32_t e_createOutputMix(Ctx& x, GuestPtr, GuestPtr out, uint32_t n, GuestPtr ids, GuestPtr req) {
    return newObject(x, Object::OutputMix, out, n, ids, req);
}
// CreateAudioPlayer(self, pPlayer, pAudioSrc, pAudioSnk, numInterfaces, pInterfaceIds, pInterfaceRequired)
uint32_t e_createAudioPlayer(Ctx& x, GuestPtr, GuestPtr out, GuestPtr src, GuestPtr sink, uint32_t n, GuestPtr ids, GuestPtr req) {
    if (!src || !sink) return kParameterInvalid;
    const uint32_t loc = x.mem.read<uint32_t>(src), fmt = x.mem.read<uint32_t>(src + 4);
    const uint32_t locType = loc ? x.mem.read<uint32_t>(loc) : 0;
    if (locType != kLocatorAndroidSimpleBQ && locType != kLocatorBufferQueue) {
        x.proc.reportDefaulted("OpenSL ES audio player of data locator 0x" + [&] { char b[16]; std::snprintf(b, sizeof b, "%x", locType); return std::string(b); }() +
                               " (only PCM buffer queues play)");
        return kContentUnsupported;
    }
    if (!fmt || x.mem.read<uint32_t>(fmt) != kFormatPcm) return kContentUnsupported;
    Player pl;
    pl.channels = x.mem.read<uint32_t>(fmt + 4);
    pl.rate = x.mem.read<uint32_t>(fmt + 8) / 1000;   // milliHertz
    pl.bits = x.mem.read<uint32_t>(fmt + 12);
    if ((pl.channels != 1 && pl.channels != 2) || (pl.bits != 8 && pl.bits != 16) || pl.rate < 1000) return kContentUnsupported;
    const uint32_t r = newObject(x, Object::AudioPlayer, out, n, ids, req);
    if (r != kOk) return r;
    auto& s = S(x);
    std::lock_guard<std::recursive_mutex> lk(s.mu);
    Object* o = objectOf(x, x.mem.read<uint32_t>(out));
    pl.voice = x.proc.mixer().createStream({});
    x.proc.mixer().setPaused(pl.voice, true);
    o->player = pl;
    if (!s.pumpThread)
        s.pumpThread = x.proc.startThread(*x.proc.lookupSymbol("__hvm_opensl_pump"), 0, 512u << 10, true);
    return kOk;
}
uint32_t e_queryNum(Ctx& x, GuestPtr, uint32_t, GuestPtr out) { if (out) x.mem.write<uint32_t>(out, 0); return kOk; }
uint32_t e_queryNumExt(Ctx& x, GuestPtr, GuestPtr out) { if (out) x.mem.write<uint32_t>(out, 0); return kOk; }
uint32_t e_isExtensionSupported(Ctx& x, GuestPtr, GuestPtr, GuestPtr out) { if (out) x.mem.write<uint32_t>(out, 0); return kOk; }

// ----------------------------------------------------------------- SLPlayItf ---

Player* playerOf(Ctx& x, GuestPtr self) {
    Object* o = objectOf(x, self);
    return o && o->kind == Object::AudioPlayer ? &o->player : nullptr;
}
void applyParams(Ctx& x, Player& p) {
    Audio::VoiceParams v;
    v.gain = p.muted ? 0.0f : p.gain;
    v.pan = p.pan;
    x.proc.mixer().setParams(p.voice, v);
}
uint32_t p_setPlayState(Ctx& x, GuestPtr self, uint32_t state) {
    auto& s = S(x);
    std::lock_guard<std::recursive_mutex> lk(s.mu);
    Player* p = playerOf(x, self);
    if (!p) return kParameterInvalid;
    if (state == kPlayStopped) {
        x.proc.mixer().stop(p->voice);
        x.proc.mixer().takeProcessed(p->voice);
        p->queued = 0;   // stopping clears the queue
    }
    x.proc.mixer().setPaused(p->voice, state != kPlayPlaying);
    p->state = state;
    return kOk;
}
uint32_t p_getPlayState(Ctx& x, GuestPtr self, GuestPtr out) {
    auto& s = S(x);
    std::lock_guard<std::recursive_mutex> lk(s.mu);
    Player* p = playerOf(x, self);
    if (!p) return kParameterInvalid;
    x.mem.write<uint32_t>(out, p->state);
    return kOk;
}
uint32_t p_getDuration(Ctx& x, GuestPtr, GuestPtr out) { x.mem.write<uint32_t>(out, 0xFFFFFFFFu); return kOk; }   // unknown
uint32_t p_getPosition(Ctx& x, GuestPtr self, GuestPtr out) {
    auto& s = S(x);
    std::lock_guard<std::recursive_mutex> lk(s.mu);
    Player* p = playerOf(x, self);
    x.mem.write<uint32_t>(out, p ? uint32_t(x.proc.mixer().positionSeconds(p->voice) * 1000.0) : 0);
    return kOk;
}
uint32_t p_ok2(Ctx&, GuestPtr, uint32_t) { return kOk; }
uint32_t p_ok3(Ctx&, GuestPtr, uint32_t, uint32_t) { return kOk; }
uint32_t p_get0(Ctx& x, GuestPtr, GuestPtr out) { if (out) x.mem.write<uint32_t>(out, 0); return kOk; }
uint32_t p_ok1(Ctx&, GuestPtr) { return kOk; }

// ------------------------------------------------------ SLAndroidSimpleBufferQueueItf ---

// Interleaved stereo 16-bit from the player's PCM format.
Audio::Pcm toStereo16(Ctx& x, const Player& p, GuestPtr data, uint32_t bytes) {
    auto out = std::make_shared<std::vector<int16_t>>();
    const uint32_t bps = p.bits / 8, frames = bytes / (bps * p.channels);
    out->resize(size_t(frames) * 2);
    const uint8_t* src = x.mem.host(data);
    for (uint32_t f = 0; f < frames; ++f)
        for (uint32_t c = 0; c < 2; ++c) {
            const uint32_t ch = p.channels == 1 ? 0 : c;
            const uint8_t* s = src + (f * p.channels + ch) * bps;
            (*out)[size_t(f) * 2 + c] = bps == 1 ? int16_t((int(s[0]) - 128) << 8) : int16_t(s[0] | (s[1] << 8));
        }
    return out;
}
uint32_t q_enqueue(Ctx& x, GuestPtr self, GuestPtr data, uint32_t bytes) {
    auto& s = S(x);
    std::lock_guard<std::recursive_mutex> lk(s.mu);
    Player* p = playerOf(x, self);
    if (!p) return kParameterInvalid;
    if (!data || !bytes) return kParameterInvalid;
    x.proc.mixer().enqueue(p->voice, toStereo16(x, *p, data, bytes), int(p->rate), p->nextTag++);
    ++p->queued;
    return kOk;
}
uint32_t q_clear(Ctx& x, GuestPtr self) {
    auto& s = S(x);
    std::lock_guard<std::recursive_mutex> lk(s.mu);
    Player* p = playerOf(x, self);
    if (!p) return kParameterInvalid;
    x.proc.mixer().stop(p->voice);
    x.proc.mixer().takeProcessed(p->voice);
    p->queued = 0;
    return kOk;
}
uint32_t q_getState(Ctx& x, GuestPtr self, GuestPtr out) {
    auto& s = S(x);
    std::lock_guard<std::recursive_mutex> lk(s.mu);
    Player* p = playerOf(x, self);
    if (!p) return kParameterInvalid;
    x.mem.write<uint32_t>(out, p->queued);
    x.mem.write<uint32_t>(out + 4, p->played);
    return kOk;
}
uint32_t q_registerCallback(Ctx& x, GuestPtr self, GuestPtr cb, GuestPtr ctx) {
    auto& s = S(x);
    std::lock_guard<std::recursive_mutex> lk(s.mu);
    Player* p = playerOf(x, self);
    if (!p) return kParameterInvalid;
    p->callback = cb;
    p->context = ctx;
    return kOk;
}

// The callback thread: one call per buffer the mixer finished, in order.
void pumpThread(Ctx& x, uint32_t) {
    if (GuestThread* t = x.proc.currentThreadIfGuest()) t->role.store("OpenSL ES callback thread: refills the game's buffer queues");
    auto& s = S(x);
    while (!x.proc.stopping()) {
        std::vector<std::pair<uint32_t, uint32_t>> calls;   // (callback, buffer-queue interface), context below
        std::vector<uint32_t> contexts;
        {
            std::lock_guard<std::recursive_mutex> lk(s.mu);
            for (auto& [id, o] : s.objects) {
                if (o.kind != Object::AudioPlayer) continue;
                Player& p = o.player;
                const size_t done = x.proc.mixer().takeProcessed(p.voice).size();
                for (size_t i = 0; i < done && p.queued; ++i) {
                    --p.queued;
                    ++p.played;
                    auto bq = o.itfs.find(Itf::BufferQueue);
                    if (p.callback && bq != o.itfs.end()) { calls.emplace_back(p.callback, bq->second); contexts.push_back(p.context); }
                }
            }
        }
        for (size_t i = 0; i < calls.size(); ++i) x.proc.callGuest(x.cpu, calls[i].first, {calls[i].second, contexts[i]});
        std::this_thread::sleep_for(std::chrono::milliseconds(calls.empty() ? 4 : 1));
    }
}

// -------------------------------------------------------------- SLVolumeItf ---
// Levels are in millibels (0 = full, negative = quieter).

uint32_t v_setLevel(Ctx& x, GuestPtr self, int16_t mb) {
    auto& s = S(x);
    std::lock_guard<std::recursive_mutex> lk(s.mu);
    if (Player* p = playerOf(x, self)) { p->gain = float(std::pow(10.0, double(mb) / 2000.0)); applyParams(x, *p); }
    return kOk;
}
uint32_t v_getLevel(Ctx& x, GuestPtr self, GuestPtr out) {
    auto& s = S(x);
    std::lock_guard<std::recursive_mutex> lk(s.mu);
    const Player* p = playerOf(x, self);
    const float g = p ? p->gain : 1.0f;
    x.mem.write<int16_t>(out, int16_t(g > 0 ? std::max(-32768.0, 2000.0 * std::log10(double(g))) : -32768));
    return kOk;
}
uint32_t v_getMax(Ctx& x, GuestPtr, GuestPtr out) { x.mem.write<int16_t>(out, 0); return kOk; }
uint32_t v_setMute(Ctx& x, GuestPtr self, uint32_t mute) {
    auto& s = S(x);
    std::lock_guard<std::recursive_mutex> lk(s.mu);
    if (Player* p = playerOf(x, self)) { p->muted = mute != 0; applyParams(x, *p); }
    return kOk;
}
uint32_t v_setStereoPosition(Ctx& x, GuestPtr self, int16_t permille) {
    auto& s = S(x);
    std::lock_guard<std::recursive_mutex> lk(s.mu);
    if (Player* p = playerOf(x, self)) { p->pan = std::clamp(float(permille) / 1000.0f, -1.0f, 1.0f); applyParams(x, *p); }
    return kOk;
}

// ------------------------------------------------------ SLAndroidConfigurationItf ---
uint32_t c_set(Ctx&, GuestPtr, GuestPtr, GuestPtr, uint32_t) { return kOk; }
uint32_t c_get(Ctx& x, GuestPtr, GuestPtr, GuestPtr size, GuestPtr) { if (size) x.mem.write<uint32_t>(size, 0); return kOk; }

// ------------------------------------------------------------------ the entry ---

// slCreateEngine(pEngine, numOptions, pEngineOptions, numInterfaces, pInterfaceIds, pInterfaceRequired)
uint32_t sl_createEngine(Ctx& x, GuestPtr out, uint32_t, GuestPtr, uint32_t n, GuestPtr ids, GuestPtr req) {
    if (!out) return kParameterInvalid;
    return newObject(x, Object::Engine, out, n, ids, req);
}
uint32_t sl_queryNum(Ctx& x, GuestPtr out) { if (out) x.mem.write<uint32_t>(out, 0); return kOk; }

template <auto F> uint32_t fn(Process& p, const char* name) {
    return p.registerHostFunction(std::string("OpenSL ") + name, [](Ctx& x) { callWrapped<F>(x); });
}

} // namespace

void registerOpenSLES(Process& p) {
    auto& s = p.service<Sl>();
    auto table = [&](Itf kind, std::vector<uint32_t> fns) {
        const uint32_t t = p.allocStatic(uint32_t(fns.size()) * 4);
        for (size_t i = 0; i < fns.size(); ++i) p.mem().write<uint32_t>(t + uint32_t(i) * 4, fns[i]);
        s.tables[kind] = t;
    };
    const uint32_t unsupported = fn<&e_unsupported>(p, "unsupported");
    table(Itf::Object, {fn<&o_realize>(p, "Realize"), fn<&o_resume>(p, "Resume"), fn<&o_getState>(p, "GetState"),
                        fn<&o_getInterface>(p, "GetInterface"), fn<&o_registerCallback>(p, "RegisterCallback"),
                        fn<&o_abort>(p, "AbortAsyncOperation"), fn<&o_destroy>(p, "Destroy"), fn<&o_setPriority>(p, "SetPriority"),
                        fn<&o_getPriority>(p, "GetPriority"), fn<&o_setLossOfControl>(p, "SetLossOfControlInterfaces")});
    table(Itf::Engine, {unsupported /*LED*/, unsupported /*Vibra*/, fn<&e_createAudioPlayer>(p, "CreateAudioPlayer"),
                        unsupported /*AudioRecorder*/, unsupported /*MidiPlayer*/, unsupported /*Listener*/,
                        unsupported /*3DGroup*/, fn<&e_createOutputMix>(p, "CreateOutputMix"), unsupported /*MetadataExtractor*/,
                        unsupported /*ExtensionObject*/, fn<&e_queryNum>(p, "QueryNumSupportedInterfaces"), unsupported,
                        fn<&e_queryNumExt>(p, "QueryNumSupportedExtensions"), unsupported,
                        fn<&e_isExtensionSupported>(p, "IsExtensionSupported")});
    const uint32_t ok2 = fn<&p_ok2>(p, "ok"), ok3 = fn<&p_ok3>(p, "ok"), get0 = fn<&p_get0>(p, "get0"), ok1 = fn<&p_ok1>(p, "ok");
    table(Itf::Play, {fn<&p_setPlayState>(p, "SetPlayState"), fn<&p_getPlayState>(p, "GetPlayState"),
                      fn<&p_getDuration>(p, "GetDuration"), fn<&p_getPosition>(p, "GetPosition"), ok3 /*RegisterCallback*/,
                      ok2 /*SetCallbackEventsMask*/, get0 /*GetCallbackEventsMask*/, ok2 /*SetMarkerPosition*/,
                      ok1 /*ClearMarkerPosition*/, get0 /*GetMarkerPosition*/, ok2 /*SetPositionUpdatePeriod*/,
                      get0 /*GetPositionUpdatePeriod*/});
    table(Itf::BufferQueue, {fn<&q_enqueue>(p, "Enqueue"), fn<&q_clear>(p, "Clear"), fn<&q_getState>(p, "GetState"),
                             fn<&q_registerCallback>(p, "RegisterCallback")});
    table(Itf::Volume, {fn<&v_setLevel>(p, "SetVolumeLevel"), fn<&v_getLevel>(p, "GetVolumeLevel"),
                        fn<&v_getMax>(p, "GetMaxVolumeLevel"), fn<&v_setMute>(p, "SetMute"), get0 /*GetMute*/,
                        ok2 /*EnableStereoPosition*/, get0 /*IsEnabledStereoPosition*/,
                        fn<&v_setStereoPosition>(p, "SetStereoPosition"), get0 /*GetStereoPosition*/});
    table(Itf::AndroidConfiguration, {fn<&c_set>(p, "SetConfiguration"), fn<&c_get>(p, "GetConfiguration")});

    // Interface ids: each SL_IID_x is a variable holding the address of a 16-byte id.
    const std::pair<const char*, Itf> ids[] = {
        {"SL_IID_OBJECT", Itf::Object}, {"SL_IID_ENGINE", Itf::Engine}, {"SL_IID_OUTPUTMIX", Itf::OutputMix},
        {"SL_IID_PLAY", Itf::Play}, {"SL_IID_BUFFERQUEUE", Itf::BufferQueue},
        {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE", Itf::BufferQueue}, {"SL_IID_VOLUME", Itf::Volume},
        {"SL_IID_ANDROIDCONFIGURATION", Itf::AndroidConfiguration}, {"SL_IID_SEEK", Itf::Seek},
        {"SL_IID_PREFETCHSTATUS", Itf::PrefetchStatus}, {"SL_IID_RECORD", Itf::Record},
    };
    uint32_t serial = 1;
    for (const auto& [name, kind] : ids) {
        const uint32_t block = p.allocStatic(16);
        p.mem().write<uint32_t>(block, 0x48564D00u + serial++);   // distinct ids ("HVM" + n)
        p.mem().write<uint32_t>(block + 4, uint32_t(kind));
        s.iids[block] = kind;
        p.mem().write<uint32_t>(p.defineDataBlock(std::string("android:") + name, 4), block);
    }
    supportDef<&sl_createEngine>(p, "android:slCreateEngine", SupportStatus::Real, "OpenSL ES engine: PCM buffer-queue players on the mixer");
    supportDef<&sl_queryNum>(p, "android:slQueryNumSupportedEngineInterfaces", SupportStatus::Approx, "none listed");
    p.def<&pumpThread>("__hvm_opensl_pump");
}

} // namespace Hydrogen::Runtime::Android
