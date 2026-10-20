// HydrogenVM — audio: decoding of the file formats iOS games ship (WAV, CAF,
// AIFF, MP3) and the software mixer every audio API plays through.
//
// The mixer is the single place sound comes out of. OpenAL sources,
// AVAudioPlayer, system sounds and audio queues each own voices in it; the
// platform pulls mixed 44.1 kHz stereo float from Mixer::render() on its audio
// thread (an AVAudioSourceNode on iOS). Every voice resamples on its own, so
// sounds at different rates, pitches and volumes overlap freely.
#pragma once
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace Hydrogen::Runtime::Audio {

struct DecodedAudio {
    int sampleRate = 44100;
    int channels = 2;              // of the source; stereoPcm is always stereo
    double duration = 0.0;
    std::vector<int16_t> stereoPcm;   // interleaved L, R
};

// WAV (PCM 8/16/24/32-bit, float), CAF (LPCM, IMA4), AIFF/AIFC, MP3.
// Unknown formats give no samples and an estimated duration.
DecodedAudio decode(const uint8_t* data, size_t n);

using Pcm = std::shared_ptr<const std::vector<int16_t>>;   // interleaved stereo

struct VoiceParams {
    float gain = 1.0f;
    float pan = 0.0f;     // -1 left .. +1 right
    float pitch = 1.0f;   // playback rate multiplier
    bool loop = false;    // non-stream voices: repeat forever
};

class Mixer {
public:
    static constexpr int kRate = 44100;

    // A voice that plays `pcm` (recorded at `rate`) once, or looping.
    // autoRelease: forget the voice when it ends (fire-and-forget sounds).
    uint32_t play(Pcm pcm, int rate, const VoiceParams& p, double startSeconds = 0, bool autoRelease = false);
    // A voice fed with queued buffers (OpenAL buffer queues, audio queues).
    // It starves (plays silence) when the queue runs out.
    uint32_t createStream(const VoiceParams& p);
    void enqueue(uint32_t voice, Pcm pcm, int rate, uint64_t tag);
    // Tags of buffers that finished playing, oldest first (and forgets them).
    std::vector<uint64_t> takeProcessed(uint32_t voice, size_t max = SIZE_MAX);
    size_t processedCount(uint32_t voice) const;
    size_t queuedCount(uint32_t voice) const;   // not yet finished, including the current one

    void setParams(uint32_t voice, const VoiceParams& p);
    void setPaused(uint32_t voice, bool paused);
    void stop(uint32_t voice);       // drops everything queued (counted as processed)
    void release(uint32_t voice);    // stop and forget
    bool playing(uint32_t voice) const;   // has audio left and isn't paused
    double positionSeconds(uint32_t voice) const;
    void seekSeconds(uint32_t voice, double t);
    void setMasterGain(float g);
    void setAllPaused(bool paused);   // app in the background: silence everything, keep positions
    void stopAll();

    // Mixes `frames` frames at kRate into separate left/right channels.
    void render(float* left, float* right, size_t frames);

private:
    struct Buffer { Pcm pcm; int rate; uint64_t tag; };
    struct Voice {
        std::deque<Buffer> queue;
        double pos = 0;   // in frames of the front buffer
        VoiceParams p;
        bool paused = false, stream = false, autoRelease = false, done = false;
        std::vector<uint64_t> processed;
    };
    mutable std::mutex mu_;
    std::unordered_map<uint32_t, Voice> voices_;
    uint32_t next_ = 1;
    float master_ = 1.0f;
    std::atomic<bool> allPaused_{false};
};

} // namespace Hydrogen::Runtime::Audio
