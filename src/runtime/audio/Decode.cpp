// HydrogenVM — audio file decoding: WAV, CAF (LPCM, IMA4), AIFF/AIFC, MP3.
#include "Audio.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

#define MINIMP3_IMPLEMENTATION
#include "thirdparty/minimp3/minimp3.h"

namespace Hydrogen::Runtime::Audio {
namespace {



uint32_t readLE32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
uint16_t readLE16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
uint32_t readBE32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint16_t readBE16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
uint64_t readBE64(const uint8_t* p) { return (uint64_t(readBE32(p)) << 32) | readBE32(p + 4); }

// Linear PCM in any common layout -> interleaved stereo int16.
void pcmToStereo(DecodedAudio& out, const uint8_t* p, size_t bytes, int channels, int bits, bool isFloat, bool bigEndian) {
    const int bps = bits / 8;
    if (channels < 1 || bps < 1) return;
    const size_t frameBytes = size_t(channels) * size_t(bps);
    const size_t frames = bytes / frameBytes;
    auto sample = [&](const uint8_t* s) -> float {
        uint32_t v = 0;
        for (int i = 0; i < bps; ++i) v |= uint32_t(s[bigEndian ? i : bps - 1 - i]) << (8 * (bps - 1 - i));
        if (isFloat) {
            if (bps == 4) { float f; std::memcpy(&f, &v, 4); return f; }
            if (bps == 8) {
                uint64_t w = 0;
                for (int i = 0; i < 8; ++i) w |= uint64_t(s[bigEndian ? i : 7 - i]) << (8 * (7 - i));
                double d; std::memcpy(&d, &w, 8); return float(d);
            }
            return 0;
        }
        if (bps == 1) return (float(v) - 128.0f) / 128.0f;   // 8-bit PCM is unsigned
        const int shift = 32 - bits;
        return float(int32_t(v << shift)) / 2147483648.0f;
    };
    out.channels = channels;
    out.stereoPcm.resize(frames * 2);
    for (size_t f = 0; f < frames; ++f) {
        const uint8_t* fp = p + f * frameBytes;
        const float l = sample(fp), r = channels >= 2 ? sample(fp + bps) : l;
        out.stereoPcm[f * 2] = int16_t(std::clamp(l * 32767.0f, -32768.0f, 32767.0f));
        out.stereoPcm[f * 2 + 1] = int16_t(std::clamp(r * 32767.0f, -32768.0f, 32767.0f));
    }
    out.duration = out.sampleRate ? double(frames) / double(out.sampleRate) : 0;
}

constexpr int16_t kSteps[89] = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724,
    796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086,
    29794, 32767};
constexpr int kIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

// Microsoft IMA WAV is distinct from Apple IMA4: each block starts with a
// complete predictor sample, then groups of four bytes per channel, low nibble first.
bool imaWavToStereo(DecodedAudio& out, const uint8_t* p, size_t bytes, int channels,
                    size_t blockBytes, size_t samplesPerBlock, uint32_t factFrames) {
    if (channels < 1 || channels > 2 || blockBytes < size_t(4 * channels) ||
        (blockBytes - 4 * channels) % (4 * channels) != 0 ||
        samplesPerBlock != 1 + (blockBytes - 4 * channels) * 2 / channels) return false;
    std::vector<int16_t> result;
    for (size_t off = 0; off < bytes;) {
        const size_t count = std::min(blockBytes, bytes - off);
        if (count < size_t(4 * channels) || (count - 4 * channels) % (4 * channels)) return false;
        int pred[2]{}, index[2]{};
        for (int c = 0; c < channels; ++c) {
            pred[c] = int16_t(readLE16(p + off + c * 4));
            index[c] = p[off + c * 4 + 2];
            if (index[c] > 88 || p[off + c * 4 + 3] != 0) return false;
        }
        result.push_back(int16_t(pred[0]));
        result.push_back(int16_t(pred[channels - 1]));
        for (size_t group = 4 * channels; group < count; group += 4 * channels) {
            int16_t decoded[2][8]{};
            for (int c = 0; c < channels; ++c) {
                for (int i = 0; i < 8; ++i) {
                    const int nib = (p[off + group + c * 4 + i / 2] >> (4 * (i & 1))) & 15;
                    const int step = kSteps[index[c]];
                    const int diff = (step >> 3) + ((nib & 1) ? step >> 2 : 0) +
                                     ((nib & 2) ? step >> 1 : 0) + ((nib & 4) ? step : 0);
                    pred[c] = std::clamp(pred[c] + ((nib & 8) ? -diff : diff), -32768, 32767);
                    index[c] = std::clamp(index[c] + kIndex[nib], 0, 88);
                    decoded[c][i] = int16_t(pred[c]);
                }
            }
            for (int i = 0; i < 8; ++i) {
                result.push_back(decoded[0][i]); result.push_back(decoded[channels - 1][i]);
            }
        }
        off += count;
    }
    if (factFrames) {
        if (factFrames > result.size() / 2) return false;
        result.resize(size_t(factFrames) * 2);
    }
    out.stereoPcm = std::move(result);
    out.channels = channels;
    out.duration = double(out.stereoPcm.size() / 2) / out.sampleRate;
    return !out.stereoPcm.empty();
}

// Apple IMA4: packets of 34 bytes per channel (2-byte header + 64 4-bit samples).
void ima4ToStereo(DecodedAudio& out, const uint8_t* p, size_t bytes, int channels) {
    if (channels < 1) return;
    const size_t packetBytes = size_t(34) * size_t(channels);
    const size_t packets = bytes / packetBytes;
    std::vector<int16_t> ch[2];
    for (int c = 0; c < std::min(channels, 2); ++c) ch[c].reserve(packets * 64);
    for (size_t k = 0; k < packets; ++k) {
        for (int c = 0; c < std::min(channels, 2); ++c) {
            const uint8_t* b = p + k * packetBytes + size_t(c) * 34;
            const uint16_t hdr = readBE16(b);
            int pred = int16_t(hdr & 0xFF80);
            int idx = std::clamp(int(hdr & 0x7F), 0, 88);
            for (int i = 0; i < 64; ++i) {
                const int nib = (b[2 + i / 2] >> ((i & 1) * 4)) & 0xF;
                const int step = kSteps[idx];
                int diff = step >> 3;
                if (nib & 1) diff += step >> 2;
                if (nib & 2) diff += step >> 1;
                if (nib & 4) diff += step;
                pred = std::clamp(nib & 8 ? pred - diff : pred + diff, -32768, 32767);
                idx = std::clamp(idx + kIndex[nib], 0, 88);
                ch[c].push_back(int16_t(pred));
            }
        }
    }
    const size_t frames = ch[0].size();
    out.channels = channels;
    out.stereoPcm.resize(frames * 2);
    for (size_t f = 0; f < frames; ++f) {
        out.stereoPcm[f * 2] = ch[0][f];
        out.stereoPcm[f * 2 + 1] = channels >= 2 ? ch[1][f] : ch[0][f];
    }
    out.duration = out.sampleRate ? double(frames) / double(out.sampleRate) : 0;
}

bool decodeCaf(DecodedAudio& out, const uint8_t* data, size_t n) {
    if (n < 8 || std::memcmp(data, "caff", 4) != 0) return false;
    size_t pos = 8;
    double sr = 44100;
    uint32_t fmt = 0, flags = 0, channels = 1, bits = 16;
    const uint8_t* audio = nullptr;
    size_t audioBytes = 0;
    while (pos + 12 <= n) {
        const uint8_t* h = data + pos;
        int64_t size = int64_t(readBE64(h + 4));
        pos += 12;
        if (size < 0 || pos + size_t(size) > n) size = int64_t(n - pos);   // -1: data runs to EOF
        if (std::memcmp(h, "desc", 4) == 0 && size >= 32) {
            uint64_t bitsRate = readBE64(data + pos);
            std::memcpy(&sr, &bitsRate, 8);
            fmt = readBE32(data + pos + 8);
            flags = readBE32(data + pos + 12);
            channels = std::max<uint32_t>(1, readBE32(data + pos + 24));
            bits = readBE32(data + pos + 28);
        } else if (std::memcmp(h, "data", 4) == 0 && size >= 4) {
            audio = data + pos + 4;   // skip the edit count
            audioBytes = size_t(size) - 4;
        }
        pos += size_t(size);
    }
    if (!audio) return false;
    out.sampleRate = int(std::max(8000.0, std::min(sr, 192000.0)));
    if (fmt == 0x6C70636D /* lpcm */) pcmToStereo(out, audio, audioBytes, int(channels), int(bits), flags & 1, !(flags & 2));
    else if (fmt == 0x696D6134 /* ima4 */) ima4ToStereo(out, audio, audioBytes, int(channels));
    else return false;
    return !out.stereoPcm.empty();
}

// IEEE 754 80-bit extended (AIFF sample rate).
double readExtended(const uint8_t* p) {
    const int exp = ((p[0] & 0x7F) << 8) | p[1];
    const uint64_t mant = readBE64(p + 2);
    if (!exp && !mant) return 0;
    const double v = std::ldexp(double(mant), exp - 16383 - 63);
    return (p[0] & 0x80) ? -v : v;
}

bool decodeAiff(DecodedAudio& out, const uint8_t* data, size_t n) {
    if (n < 12 || std::memcmp(data, "FORM", 4) != 0) return false;
    const bool aifc = std::memcmp(data + 8, "AIFC", 4) == 0;
    if (!aifc && std::memcmp(data + 8, "AIFF", 4) != 0) return false;
    size_t pos = 12;
    int channels = 1, bits = 16;
    double sr = 44100;
    uint32_t comp = 0x4E4F4E45;   // 'NONE'
    const uint8_t* audio = nullptr;
    size_t audioBytes = 0;
    while (pos + 8 <= n) {
        const uint8_t* h = data + pos;
        const size_t size = readBE32(h + 4);
        pos += 8;
        if (pos + size > n) break;
        if (std::memcmp(h, "COMM", 4) == 0 && size >= 18) {
            channels = std::max(1, int(readBE16(data + pos)));
            bits = readBE16(data + pos + 6);
            sr = readExtended(data + pos + 8);
            if (aifc && size >= 22) comp = readBE32(data + pos + 18);
        } else if (std::memcmp(h, "SSND", 4) == 0 && size >= 8) {
            const uint32_t off = readBE32(data + pos);
            audio = data + pos + 8 + off;
            audioBytes = size - 8 - std::min<size_t>(off, size - 8);
        }
        pos += (size + 1) & ~size_t(1);
    }
    if (!audio) return false;
    out.sampleRate = int(std::max(8000.0, std::min(sr, 192000.0)));
    if (comp == 0x4E4F4E45 /* NONE */ || comp == 0x74776F73 /* twos */) pcmToStereo(out, audio, audioBytes, channels, bits, false, true);
    else if (comp == 0x736F7774 /* sowt */) pcmToStereo(out, audio, audioBytes, channels, bits, false, false);
    else if (comp == 0x666C3332 /* fl32 */) pcmToStereo(out, audio, audioBytes, channels, 32, true, true);
    else if (comp == 0x696D6134 /* ima4 */) ima4ToStereo(out, audio, audioBytes, channels);
    else return false;
    return !out.stereoPcm.empty();
}

DecodedAudio decodeKnown(const uint8_t* data, size_t n) {
    DecodedAudio out;
    if (!data || n < 12) return out;
    if (decodeCaf(out, data, n) || decodeAiff(out, data, n)) return out;
    out = DecodedAudio{};

    // RIFF / WAVE parser
    if (std::memcmp(data, "RIFF", 4) == 0 && std::memcmp(data + 8, "WAVE", 4) == 0) {
        size_t pos = 12;
        uint16_t audioFmt = 1, numCh = 1, bitsPerSample = 16;
        uint32_t sr = 44100;
        uint16_t blockBytes = 0, samplesPerBlock = 0;
        uint32_t factFrames = 0;
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
                blockBytes = readLE16(data + pos + 12);
                if (chunkSize >= 20 && readLE16(data + pos + 16) >= 2)
                    samplesPerBlock = readLE16(data + pos + 18);
            } else if (std::memcmp(hdr, "fact", 4) == 0 && chunkSize >= 4) {
                factFrames = readLE32(data + pos);
            } else if (std::memcmp(hdr, "data", 4) == 0) {
                pcmPtr = data + pos;
                pcmBytes = chunkSize;
            }
            pos += (chunkSize + 1) & ~size_t(1);
        }
        out.sampleRate = int(sr);
        out.channels = int(numCh);
        if (audioFmt == 0x11) {
            if (pcmPtr && bitsPerSample == 4 && imaWavToStereo(out, pcmPtr, pcmBytes,
                    numCh, blockBytes, samplesPerBlock, factFrames)) return out;
            return DecodedAudio{};
        }
        if (pcmPtr && pcmBytes > 0 && (audioFmt == 3 || audioFmt == 0xFFFE || (audioFmt == 1 && bitsPerSample > 16))) {
            // float, WAVE_FORMAT_EXTENSIBLE (PCM inside), 24/32-bit integer
            pcmToStereo(out, pcmPtr, pcmBytes, int(numCh), int(bitsPerSample), audioFmt == 3, false);
            return out;
        }
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


// MPEG-1/2 layer III (and I/II) via minimp3. Skips an ID3v2 tag first.
bool decodeMp3(DecodedAudio& out, const uint8_t* data, size_t n) {
    size_t off = 0;
    if (n >= 10 && std::memcmp(data, "ID3", 3) == 0) {
        off = 10 + ((size_t(data[6] & 0x7F) << 21) | (size_t(data[7] & 0x7F) << 14) | (size_t(data[8] & 0x7F) << 7) | size_t(data[9] & 0x7F));
        if (data[5] & 0x10) off += 10;   // footer
    }
    if (off >= n) return false;
    auto dec = std::make_unique<mp3dec_t>();
    mp3dec_init(dec.get());
    std::vector<int16_t> pcm(MINIMP3_MAX_SAMPLES_PER_FRAME);
    mp3dec_frame_info_t info{};
    int rate = 0, channels = 0;
    size_t badBytes = 0;
    while (off < n) {
        const int samples = mp3dec_decode_frame(dec.get(), data + off, int(std::min<size_t>(n - off, 1u << 20)), pcm.data(), &info);
        if (info.frame_bytes <= 0) break;
        off += size_t(info.frame_bytes);
        if (samples <= 0) { if ((badBytes += size_t(info.frame_bytes)) > (1u << 20)) break; continue; }
        if (!rate) { rate = info.hz; channels = info.channels; }
        if (info.hz != rate) continue;   // mid-stream format change: skip
        for (int i = 0; i < samples; ++i) {
            const int16_t l = pcm[size_t(i * info.channels)];
            const int16_t r = info.channels >= 2 ? pcm[size_t(i * info.channels + 1)] : l;
            out.stereoPcm.push_back(l);
            out.stereoPcm.push_back(r);
        }
    }
    if (out.stereoPcm.empty() || !rate) return false;
    out.sampleRate = rate;
    out.channels = channels;
    out.duration = double(out.stereoPcm.size() / 2) / double(rate);
    return true;
}

bool looksLikeMp3(const uint8_t* d, size_t n) {
    if (n >= 3 && std::memcmp(d, "ID3", 3) == 0) return true;
    return n >= 2 && d[0] == 0xFF && (d[1] & 0xE0) == 0xE0;
}

} // namespace

DecodedAudio decode(const uint8_t* data, size_t n) {
    if (data && looksLikeMp3(data, n)) {
        DecodedAudio out;
        if (decodeMp3(out, data, n)) return out;
    }
    return decodeKnown(data, n);
}

} // namespace Hydrogen::Runtime::Audio
