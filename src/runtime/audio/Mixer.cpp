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
    it->second.pos = 0;
    it->second.done = true;
}

void Mixer::release(uint32_t voice) {
    std::lock_guard<std::mutex> lk(mu_);
    voices_.erase(voice);
}

bool Mixer::playing(uint32_t voice) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = voices_.find(voice);
    return it != voices_.end() && !it->second.queue.empty() && !it->second.paused;
}

double Mixer::positionSeconds(uint32_t voice) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = voices_.find(voice);
    if (it == voices_.end() || it->second.queue.empty()) return 0;
    return it->second.pos / double(it->second.queue.front().rate);
}

void Mixer::seekSeconds(uint32_t voice, double t) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = voices_.find(voice);
    if (it == voices_.end() || it->second.queue.empty()) return;
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
        if (v.paused || v.queue.empty()) { ++it; continue; }
        const bool music = v.stream || v.p.loop ||
                           double(v.queue.front().pcm->size() / 2) >= 8.0 * double(v.queue.front().rate);
        const float appGain = std::abs(v.p.gain);   // the app's volume, not the player's
        int16_t voicePeak = 0;
        const float g = v.p.gain * master_;
        const float pan = std::clamp(v.p.pan, -1.0f, 1.0f);
        const float gl = g * (pan > 0 ? 1.0f - pan : 1.0f) * kScale;
        const float gr = g * (pan < 0 ? 1.0f + pan : 1.0f) * kScale;
        const float pitch = std::clamp(v.p.pitch, 0.05f, 8.0f);
        size_t i = 0;
        while (i < frames && !v.queue.empty()) {
            const Buffer& b = v.queue.front();
            const int16_t* s = b.pcm->data();
            const size_t n = b.pcm->size() / 2;
            const double step = double(b.rate) * pitch / double(kRate);
            if (n == 0) { v.processed.push_back(b.tag); v.queue.pop_front(); v.pos = 0; continue; }
            for (; i < frames; ++i) {
                if (v.pos >= double(n)) break;
                const size_t i0 = size_t(v.pos);
                const float t = float(v.pos - double(i0));
                size_t i1 = i0 + 1;
                const int16_t* s1 = s;   // where the interpolation neighbour lives
                if (i1 >= n) {
                    if (!v.stream && v.p.loop) i1 = 0;
                    else if (v.queue.size() > 1 && v.queue[1].pcm && v.queue[1].pcm->size() >= 2) {
                        s1 = v.queue[1].pcm->data();   // continue into the next queued buffer, not a repeat of this one's last frame
                        i1 = 0;
                    } else i1 = n - 1;
                }
                voicePeak = std::max<int16_t>(voicePeak, int16_t(std::min(32767, std::abs(int(s[i0 * 2])))));
                left[i] += (float(s[i0 * 2]) * (1 - t) + float(s1[i1 * 2]) * t) * gl;
                right[i] += (float(s[i0 * 2 + 1]) * (1 - t) + float(s1[i1 * 2 + 1]) * t) * gr;
                v.pos += step;
            }
            if (v.pos >= double(n)) {
                if (!v.stream && v.p.loop) { v.pos = std::fmod(v.pos, double(n)); continue; }
                const double carry = (v.pos - double(n)) / double(b.rate);   // seconds into the next buffer
                v.processed.push_back(b.tag);
                v.queue.pop_front();
                v.pos = v.queue.empty() ? 0 : carry * double(v.queue.front().rate);
            }
        }
        const float level = float(voicePeak) * kScale * appGain;
        if (level > kAudible) {
            (music ? musicHeard : effectHeard) = true;
            callPeak = std::max(callPeak, std::min(level, 1.0f));
            if (!v.heard) { v.heard = true; (music ? stats_.musicVoices : stats_.effectVoices)++; }
        }
        if (v.queue.empty()) v.done = true;
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
