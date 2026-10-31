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
}

void Mixer::setAllPaused(bool paused) { allPaused_.store(paused); }

Mixer::Stats Mixer::stats() const {
    std::lock_guard<std::mutex> lk(mu_);
    return stats_;
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
                if (i1 >= n) i1 = (!v.stream && v.p.loop) ? 0 : n - 1;
                voicePeak = std::max<int16_t>(voicePeak, int16_t(std::min(32767, std::abs(int(s[i0 * 2])))));
                left[i] += (float(s[i0 * 2]) * (1 - t) + float(s[i1 * 2]) * t) * gl;
                right[i] += (float(s[i0 * 2 + 1]) * (1 - t) + float(s[i1 * 2 + 1]) * t) * gr;
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
    for (size_t i = 0; i < frames; ++i) {
        left[i] = std::clamp(left[i], -1.0f, 1.0f);
        right[i] = std::clamp(right[i], -1.0f, 1.0f);
    }
}

} // namespace Hydrogen::Runtime::Audio
