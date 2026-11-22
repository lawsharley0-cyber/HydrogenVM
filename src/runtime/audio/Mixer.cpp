// HydrogenVM — the software mixer (see Audio.hpp).
#include "Audio.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace Hydrogen::Runtime::Audio {

uint32_t Mixer::play(Pcm pcm, int rate, const VoiceParams& p, double startSeconds, bool autoRelease) {
    std::lock_guard<std::mutex> lk(mu_);
    const uint32_t id = next_++;
    Voice& v = voices_[id];
    v.p = p;
    v.autoRelease = autoRelease;
    if (pcm && !pcm->empty()) {
        v.queue.push_back({std::move(pcm), std::max(1, rate), 0});
        v.pos = std::max(0.0, startSeconds) * double(std::max(1, rate));
    }
    v.done = v.queue.empty();
    return id;
}

uint32_t Mixer::playSource(std::shared_ptr<AudioSource> source, const VoiceParams& p, double startSeconds) {
    auto samples = std::make_unique<SourceWindow>();
    std::lock_guard<std::mutex> lk(mu_);
    const uint32_t id = next_++;
    Voice& v = voices_[id];
    v.p = p;
    v.source = std::move(source);
    v.samples = std::move(samples);
    if (v.source)
        v.pos = std::clamp(std::max(0.0, startSeconds) * v.source->sampleRate(), 0.0, double(v.source->frames()));
    v.done = !v.source || v.pos >= double(v.source->frames());
    return id;
}

uint32_t Mixer::createStream(const VoiceParams& p) {
    std::lock_guard<std::mutex> lk(mu_);
    const uint32_t id = next_++;
    Voice& v = voices_[id];
    v.p = p;
    v.stream = true;
    return id;
}

void Mixer::enqueue(uint32_t voice, Pcm pcm, int rate, uint64_t tag) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = voices_.find(voice);
    if (it == voices_.end()) return;
    if (!pcm || pcm->empty()) { it->second.processed.push_back(tag); return; }   // empty buffers finish at once
    it->second.queue.push_back({std::move(pcm), std::max(1, rate), tag});
    it->second.done = false;
}

std::vector<uint64_t> Mixer::takeProcessed(uint32_t voice, size_t max) {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<uint64_t> out;
    auto it = voices_.find(voice);
    if (it == voices_.end()) return out;
    auto& p = it->second.processed;
    const size_t n = std::min(max, p.size());
    out.assign(p.begin(), p.begin() + long(n));
    p.erase(p.begin(), p.begin() + long(n));
    return out;
}

size_t Mixer::processedCount(uint32_t voice) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = voices_.find(voice);
    return it == voices_.end() ? 0 : it->second.processed.size();
}

size_t Mixer::queuedCount(uint32_t voice) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = voices_.find(voice);
    return it == voices_.end() ? 0 : it->second.queue.size();
}

void Mixer::setParams(uint32_t voice, const VoiceParams& p) {
    std::lock_guard<std::mutex> lk(mu_);
    if (auto it = voices_.find(voice); it != voices_.end()) it->second.p = p;
}

void Mixer::setPaused(uint32_t voice, bool paused) {
    std::lock_guard<std::mutex> lk(mu_);
    if (auto it = voices_.find(voice); it != voices_.end()) it->second.paused = paused;
}

void Mixer::stop(uint32_t voice) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = voices_.find(voice);
    if (it == voices_.end()) return;
    for (const Buffer& b : it->second.queue) it->second.processed.push_back(b.tag);
    it->second.queue.clear();
    it->second.samples.reset();
    it->second.source.reset();
    it->second.pos = 0;
    it->second.playedSeconds = 0;
    it->second.done = true;
}

void Mixer::release(uint32_t voice) {
    std::lock_guard<std::mutex> lk(mu_);
    voices_.erase(voice);
}

bool Mixer::playing(uint32_t voice) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = voices_.find(voice);
    if (it == voices_.end() || it->second.paused) return false;
    const Voice& v = it->second;
    return v.source ? !v.done : !v.queue.empty();
}

double Mixer::positionSeconds(uint32_t voice) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = voices_.find(voice);
    if (it == voices_.end()) return 0;
    const Voice& v = it->second;
    if (v.source) return v.pos / v.source->sampleRate();
    return v.queue.empty() ? 0 : v.pos / v.queue.front().rate;
}

double Mixer::streamSeconds(uint32_t voice) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = voices_.find(voice);
    if (it == voices_.end()) return 0;
    const Voice& v = it->second;
    if (v.source) return v.pos / v.source->sampleRate();
    return v.playedSeconds + (v.queue.empty() ? 0 : v.pos / v.queue.front().rate);
}

void Mixer::seekSeconds(uint32_t voice, double t) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = voices_.find(voice);
    if (it == voices_.end() || !std::isfinite(t)) return;
    Voice& v = it->second;
    if (v.source) {
        v.pos = std::clamp(t * v.source->sampleRate(), 0.0, double(v.source->frames()));
        v.done = v.pos >= double(v.source->frames());
        return;
    }
    if (v.queue.empty()) return;
    const Buffer& b = it->second.queue.front();
    const double frames = double(b.pcm->size() / 2);
    it->second.pos = std::clamp(t * double(b.rate), 0.0, std::max(0.0, frames - 1));
}

void Mixer::setMasterGain(float g) {
    std::lock_guard<std::mutex> lk(mu_);
    master_ = std::max(0.0f, g);
}

void Mixer::stopAll() {
    std::lock_guard<std::mutex> lk(mu_);
    voices_.clear();
    limiter_ = 1.0f;   // the next sound starts at full level, not at the previous one's reduction
}

void Mixer::setAllPaused(bool paused) { allPaused_.store(paused); }

Mixer::Stats Mixer::stats() const {
    std::lock_guard<std::mutex> lk(mu_);
    return stats_;
}

Mixer::MemoryStats Mixer::memoryStats() const {
    std::lock_guard<std::mutex> lk(mu_);
    MemoryStats out;
    out.voiceCount = voices_.size();
    for (const auto& [id, voice] : voices_) {
        out.queuedBufferCount += voice.queue.size();
        out.processedTagCount += voice.processed.size();
        out.processedTagCapacityBytes += voice.processed.capacity() * sizeof(uint64_t);
        for (const auto& buffer : voice.queue)
            if (buffer.pcm) out.referencedPcmBytes += buffer.pcm->capacity() * sizeof(int16_t);
    }
    return out;
}

namespace {
// Butterworth stage Qs for order 8: 1 / (2 cos((2k - 1) pi / 16)), k = 1..4.
constexpr double kButterQ[4] = {0.50979558, 0.60134489, 0.89997622, 2.56291545};
}
void Mixer::designImageFilter(Voice& v, double effectiveRate) {
    if (effectiveRate == v.imageRate) return;
    v.imageRate = effectiveRate;
    const double fc = 0.46 * effectiveRate;
    // Played at or above the mixer's rate, there are no images under 20 kHz to remove.
    v.imageFilter = fc < 0.45 * double(kRate);
    if (!v.imageFilter) return;
    constexpr double kPi = 3.14159265358979323846;
    const double w0 = 2.0 * kPi * fc / double(kRate), cw = std::cos(w0), sw = std::sin(w0);
    for (int k = 0; k < 4; ++k) {
        const double alpha = sw / (2.0 * kButterQ[k]);
        const double a0 = 1.0 + alpha;
        Voice::Biquad& q = v.image[k];
        q.b0 = float((1.0 - cw) / 2.0 / a0);
        q.b1 = float((1.0 - cw) / a0);
        q.b2 = q.b0;
        q.a1 = float(-2.0 * cw / a0);
        q.a2 = float((1.0 - alpha) / a0);
    }
}
float Mixer::filterImage(Voice& v, int channel, float x) {
    for (int k = 0; k < 4; ++k) {
        const Voice::Biquad& q = v.image[k];
        float* z = v.imageZ[channel][k];
        const float y = q.b0 * x + z[0];
        z[0] = q.b1 * x - q.a1 * y + z[1];
        z[1] = q.b2 * x - q.a2 * y;
        x = y;
    }
    return x;
}

void Mixer::render(float* left, float* right, size_t frames) {
    std::memset(left, 0, frames * sizeof(float));
    std::memset(right, 0, frames * sizeof(float));
    if (allPaused_.load(std::memory_order_relaxed)) return;   // silence; voices don't advance
    std::lock_guard<std::mutex> lk(mu_);
    constexpr float kScale = 1.0f / 32768.0f;
    constexpr float kAudible = 1.0f / 1000.0f;   // about -60 dBFS
    bool musicHeard = false, effectHeard = false;
    float callPeak = 0;
    for (auto it = voices_.begin(); it != voices_.end();) {
        Voice& v = it->second;
        const auto* source = v.source.get();
        if (v.paused || (source ? v.done : v.queue.empty())) { ++it; continue; }
        const bool music = v.stream || v.p.loop ||
                           (source ? double(source->frames()) >= 8.0 * source->sampleRate() :
                            double(v.queue.front().pcm->size() / 2) >= 8.0 * v.queue.front().rate);
        const float appGain = std::abs(v.p.gain);   // the app's volume, not the player's
        int16_t voicePeak = 0;
        const float g = v.p.gain * master_;
        const float pan = std::clamp(v.p.pan, -1.0f, 1.0f);
        const float gl = g * (pan > 0 ? 1.0f - pan : 1.0f) * kScale;
        const float gr = g * (pan < 0 ? 1.0f + pan : 1.0f) * kScale;
        const float pitch = std::clamp(v.p.pitch, 0.05f, 8.0f);
        size_t i = 0;
        while (i < frames && (source ? !v.done : !v.queue.empty())) {
            const Buffer* b = source ? nullptr : &v.queue.front();
            const int rate = source ? source->sampleRate() : b->rate;
            const size_t n = source ? size_t(source->frames()) : b->pcm->size() / 2;
            const double step = double(rate) * pitch / double(kRate);
            designImageFilter(v, double(rate) * pitch);
            if (n == 0) {
                if (source) v.done = true;
                else { v.processed.push_back(b->tag); v.queue.pop_front(); }
                v.pos = 0;
                continue;
            }
            for (; i < frames; ++i) {
                if (v.pos >= double(n)) break;
                const size_t i0 = size_t(v.pos);
                const float t = float(v.pos - double(i0));
                int16_t current[2], next[2];
                if (source) {
                    if (!v.samples->pair(*v.source, i0, current, next, v.p.loop)) {
                        v.done = true;
                        break;
                    }
                } else {
                    const int16_t* s = b->pcm->data();
                    const int16_t* following = s + (i0 + 1) * 2;
                    if (i0 + 1 == n) {
                        if (!v.stream && v.p.loop) following = s;
                        else if (v.queue.size() > 1 && v.queue[1].pcm && v.queue[1].pcm->size() >= 2)
                            following = v.queue[1].pcm->data();
                        else following = s + i0 * 2;
                    }
                    current[0] = s[2 * i0];
                    current[1] = s[2 * i0 + 1];
                    next[0] = following[0];
                    next[1] = following[1];
                }
                voicePeak = std::max<int16_t>(voicePeak, int16_t(std::min(32767, std::abs(int(current[0])))));
                float mixedL = float(current[0]) * (1 - t) + float(next[0]) * t;
                float mixedR = float(current[1]) * (1 - t) + float(next[1]) * t;
                if (v.imageFilter) {
                    mixedL = filterImage(v, 0, mixedL);
                    mixedR = filterImage(v, 1, mixedR);
                }
                left[i] += mixedL * gl;
                right[i] += mixedR * gr;
                // Reporting only: 10 ms RMS of the audible (post-gain) level, for onset checks.
                const float audibleL = mixedL * kScale * appGain;
                const float audibleR = mixedR * kScale * appGain;
                v.onsetSumSq += double(audibleL) * double(audibleL) + double(audibleR) * double(audibleR);
                v.onsetSamples++;
                if (v.onsetSamples >= Voice::kOnsetWindow) {
                    const float rms = float(std::sqrt(v.onsetSumSq / double(2 * v.onsetSamples)));
                    if (v.onsetFilled >= 20) {   // 200 ms of already-audible history
                        float tmp[Voice::kOnsetHist];
                        const int n = v.onsetFilled;
                        for (int k = 0; k < n; ++k) tmp[k] = v.onsetRms[k];
                        std::nth_element(tmp, tmp + n / 2, tmp + n);
                        const float bg = tmp[n / 2];
                        constexpr float kOnsetRatio = 5.0f;
                        if (bg > kAudible && rms >= kOnsetRatio * bg) {
                            v.effectHoldSamples = Voice::kOnsetWindow * 20;   // ~200 ms
                            if (!v.onsetCounted) { v.onsetCounted = true; stats_.effectVoices++; }
                        }
                    }
                    v.onsetRms[v.onsetIndex] = rms;
                    v.onsetIndex = (v.onsetIndex + 1) % Voice::kOnsetHist;
                    if (v.onsetFilled < Voice::kOnsetHist) v.onsetFilled++;
                    v.onsetSumSq = 0;
                    v.onsetSamples = 0;
                }
                if (v.effectHoldSamples > 0) v.effectHoldSamples--;
                v.pos += step;
            }
            if (v.pos >= double(n)) {
                if (!v.stream && v.p.loop) { v.pos = std::fmod(v.pos, double(n)); continue; }
                if (source) { v.done = true; v.pos = double(n); break; }
                const double carry = (v.pos - double(n)) / double(rate);   // seconds into the next buffer
                v.playedSeconds += double(n) / double(rate);
                v.processed.push_back(b->tag);
                v.queue.pop_front();
                v.pos = v.queue.empty() ? 0 : carry * double(v.queue.front().rate);
            }
        }
        // A fading voice drives the filter state toward zero through subnormal floats, which
        // are many times slower to compute with: flush them once per block.
        if (v.imageFilter)
            for (auto& ch : v.imageZ) for (auto& st : ch) for (float& z : st) if (std::abs(z) < 1e-20f) z = 0;
        const float level = float(voicePeak) * kScale * appGain;
        // A music stream that just had a sharp rise also tallies as an effect (hold
        // is counted down per output sample above). Hold may extend into a later call.
        if (v.onsetCounted && v.effectHoldSamples > 0) effectHeard = true;
        if (level > kAudible) {
            (music ? musicHeard : effectHeard) = true;
            callPeak = std::max(callPeak, std::min(level, 1.0f));
            if (!v.heard) { v.heard = true; (music ? stats_.musicVoices : stats_.effectVoices)++; }
        }
        if (!source && v.queue.empty()) v.done = true;
        if (v.done && v.autoRelease) it = voices_.erase(it);
        else ++it;
    }
    stats_.framesRendered += frames;
    if (musicHeard || effectHeard) stats_.audibleFrames += frames;
    if (musicHeard) stats_.musicFrames += frames;
    if (effectHeard) stats_.effectFrames += frames;
    stats_.peak = std::max(stats_.peak, callPeak);
    // Summed voices can exceed full scale (two loud tracks, or a voice with gain above 1). Clamping
    // that flattens every crest, which is harsh crackle on a speaker. Instead a peak limiter scales
    // the bus: it reacts at once to a peak over the ceiling (so no sample gets past it) and lets go
    // slowly, so a sustained loud passage keeps one steady gain and its waveform stays intact. Both
    // channels share the gain to keep the stereo image. Under the ceiling the gain is exactly 1.
    constexpr float kCeiling = 0.98f;                  // about -0.18 dBFS, clear of the clamp below
    constexpr float kRelease = 1.0f / (0.1f * float(kRate));   // ~100 ms time constant
    float g = limiter_;
    for (size_t i = 0; i < frames; ++i) {
        const float peak = std::max(std::abs(left[i]), std::abs(right[i]));
        const float need = peak > kCeiling ? kCeiling / peak : 1.0f;
        if (need < g) g = need;
        else {
            g += (need - g) * kRelease;
            if (g > 0.9998f) g = 1.0f;   // float rounding would otherwise park it just below 1
        }
        left[i] = std::clamp(left[i] * g, -1.0f, 1.0f);    // the clamp is a safety net, not the limiter
        right[i] = std::clamp(right[i] * g, -1.0f, 1.0f);
    }
    limiter_ = g;
}

} // namespace Hydrogen::Runtime::Audio
