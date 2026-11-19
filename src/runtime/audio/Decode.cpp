// HydrogenVM — audio file decoding: WAV, CAF (LPCM, IMA4), AIFF/AIFC, MP3.
#include "Audio.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <limits>
#include <sys/stat.h>
#include <utility>
#include <unistd.h>

#define MINIMP3_IMPLEMENTATION
#include "thirdparty/minimp3/minimp3.h"

#define HVM_COMPILE_STB_VORBIS
#include "thirdparty/stb/stb_vorbis.c"
#undef HVM_COMPILE_STB_VORBIS

namespace Hydrogen::Runtime::Audio {
bool decodeHostContainer(DecodedAudio&, const uint8_t*, size_t);
std::shared_ptr<AudioSource> openHostStream(const uint8_t*, size_t, std::shared_ptr<const void>);
std::shared_ptr<AudioSource> openHostStream(int fd, size_t size);
bool decodeMsAdpcm(DecodedAudio&, const uint8_t*, size_t);
namespace {



uint32_t readLE32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
uint16_t readLE16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
uint32_t readBE32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint16_t readBE16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
uint64_t readBE64(const uint8_t* p) { return (uint64_t(readBE32(p)) << 32) | readBE32(p + 4); }

struct PcmLayout {
    const uint8_t* data = nullptr;
    size_t bytes = 0;
    int channels = 0, bits = 0;
    bool isFloat = false, bigEndian = false, exactWav = false;

    size_t frameBytes() const {
        const bool supported = isFloat ? (bits == 32 || bits == 64) :
                                        (bits == 8 || bits == 16 || bits == 24 || bits == 32);
        return supported && channels > 0 ? size_t(channels) * (bits / 8) : 0;
    }
};

// Linear PCM in any common layout -> interleaved stereo int16.
void convertPcm(const PcmLayout& pcm, size_t frames, int16_t* output) {
    const int bps = pcm.bits / 8;
    const size_t frameBytes = pcm.frameBytes();
    if (pcm.exactWav) {
        for (size_t frame = 0; frame < frames; ++frame) {
            const uint8_t* at = pcm.data + frame * frameBytes;
            auto sample = [&](int channel) {
                return bps == 2 ? int16_t(readLE16(at + channel * 2)) : int16_t((int(at[channel]) - 128) * 256);
            };
            output[2 * frame] = sample(0);
            output[2 * frame + 1] = sample(pcm.channels >= 2);
        }
        return;
    }
    auto sample = [&](const uint8_t* s) -> float {
        if (pcm.isFloat) {
            if (bps == 4) {
                const uint32_t v = pcm.bigEndian ? readBE32(s) : readLE32(s);
                float f; std::memcpy(&f, &v, 4); return f;
            }
            if (bps == 8) {
                uint64_t w = 0;
                for (int i = 0; i < 8; ++i) w |= uint64_t(s[pcm.bigEndian ? i : 7 - i]) << (8 * (7 - i));
                double d; std::memcpy(&d, &w, 8); return float(d);
            }
            return 0;
        }
        uint32_t v = 0;
        for (int i = 0; i < bps; ++i) v |= uint32_t(s[pcm.bigEndian ? i : bps - 1 - i]) << (8 * (bps - 1 - i));
        if (bps == 1) return (float(v) - 128.0f) / 128.0f;   // 8-bit PCM is unsigned
        const int shift = 32 - pcm.bits;
        return float(int32_t(v << shift)) / 2147483648.0f;
    };
    auto toInt16 = [](float v) -> int16_t {
        // A NaN has no integer representation. Silence it; infinities saturate
        // just like finite samples outside [-1, 1].
        if (std::isnan(v)) return 0;
        return int16_t(std::clamp(v * 32767.0f, -32768.0f, 32767.0f));
    };
    for (size_t f = 0; f < frames; ++f) {
        const uint8_t* fp = pcm.data + f * frameBytes;
        const float l = sample(fp), r = pcm.channels >= 2 ? sample(fp + bps) : l;
        output[f * 2] = toInt16(l);
        output[f * 2 + 1] = toInt16(r);
    }
}

void pcmToStereo(DecodedAudio& out, const PcmLayout& pcm) {
    const size_t frameBytes = pcm.frameBytes();
    if (!frameBytes) return;
    const size_t frames = pcm.bytes / frameBytes;
    out.channels = pcm.channels;
    out.stereoPcm.resize(frames * 2);
    out.duration = out.sampleRate ? double(frames) / out.sampleRate : 0;
    convertPcm(pcm, frames, out.stereoPcm.data());
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

struct CafInfo {
    double rate = 44100;
    uint32_t format = 0, flags = 0, channels = 1, bits = 16, framesPerPacket = 0;
    int64_t validFrames = -1, packets = -1;
    size_t offset = 0, length = 0;

    int sampleRate() const { return int(std::max(8000.0, std::min(rate, 192000.0))); }
    PcmLayout layout(const uint8_t* audio) const {
        return {audio, length, int(channels), int(bits), bool(flags & 1), !(flags & 2)};
    }
};

template<typename Read>
CafInfo cafInfo(size_t n, Read read) {
    CafInfo info;
    // The caller has already checked the file magic.
    for (size_t pos = 8; pos + 12 <= n;) {
        const auto* header = read(pos, 12);
        if (!header) break;
        const auto declared = readBE64(header + 4);
        const uint32_t type = readBE32(header);
        pos += 12;
        const size_t size = size_t(std::min<uint64_t>(declared, n - pos));
        if (type == 0x64657363 /* desc */ && size >= 32) {
            const auto* desc = read(pos, 32);
            if (!desc) break;
            uint64_t rate = readBE64(desc);
            std::memcpy(&info.rate, &rate, 8);
            info.format = readBE32(desc + 8);
            info.flags = readBE32(desc + 12);
            info.framesPerPacket = readBE32(desc + 20);
            info.channels = std::max<uint32_t>(1, readBE32(desc + 24));
            info.bits = readBE32(desc + 28);
        } else if (type == 0x70616B74 /* pakt */ && size >= 24) {
            const auto* table = read(pos, 24);
            if (!table) break;
            info.packets = int64_t(readBE64(table));
            info.validFrames = int64_t(readBE64(table + 8));
        } else if (type == 0x64617461 /* data */ && size >= 4) {
            info.offset = pos + 4;  // skip the edit count
            info.length = size - 4;
        }
        pos += size;
    }
    return info;
}

bool decodeCaf(DecodedAudio& out, const uint8_t* data, size_t n) {
    if (n < 8 || std::memcmp(data, "caff", 4) != 0) return false;
    const auto info = cafInfo(n, [&](size_t offset, size_t) { return data + offset; });
    if (!info.offset) return false;
    const auto* audio = data + info.offset;
    out.sampleRate = info.sampleRate();
    if (info.format == 0x6C70636D /* lpcm */) pcmToStereo(out, info.layout(audio));
    else if (info.format == 0x696D6134 /* ima4 */) ima4ToStereo(out, audio, info.length, int(info.channels));
    else {
        // A compressed codec unavailable on this host still opens with the
        // timing declared by the file. Limit the size of the silence buffer.
        int64_t frames = info.validFrames;
        if (frames < 0 && info.packets >= 0 && info.framesPerPacket &&
            info.packets <= std::numeric_limits<int64_t>::max() / info.framesPerPacket)
            frames = info.packets * int64_t(info.framesPerPacket);
        if (frames <= 0) return false;
        const int64_t realFrames = frames;
        frames = std::min<int64_t>(frames, int64_t(16) << 20);
        out.channels = int(std::min<uint32_t>(info.channels, 8));
        out.stereoPcm.assign(size_t(frames) * 2, 0);
        out.duration = double(realFrames) / out.sampleRate;
        out.silent = true;
    }
    out.codec = info.format;
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
    if (comp == 0x4E4F4E45 /* NONE */ || comp == 0x74776F73 /* twos */) pcmToStereo(out, {audio, audioBytes, channels, bits, false, true});
    else if (comp == 0x736F7774 /* sowt */) pcmToStereo(out, {audio, audioBytes, channels, bits, false, false});
    else if (comp == 0x666C3332 /* fl32 */) pcmToStereo(out, {audio, audioBytes, channels, 32, true, true});
    else if (comp == 0x696D6134 /* ima4 */) ima4ToStereo(out, audio, audioBytes, channels);
    else return false;
    return !out.stereoPcm.empty();
}

struct WaveInfo {
    uint16_t format = 1, channels = 1, bits = 16, blockBytes = 0, samplesPerBlock = 0;
    uint32_t sampleRate = 44100, factFrames = 0;
    size_t offset = 0, length = 0;

    PcmLayout layout(const uint8_t* audio) const {
        if (format != 1 && format != 3 && format != 0xFFFE) return {};
        return {audio, length, channels, bits, format == 3, false, format == 1 && (bits == 8 || bits == 16)};
    }
};

template<typename Read>
WaveInfo waveInfo(size_t n, Read read) {
    WaveInfo info;
    for (size_t pos = 12; pos + 8 <= n;) {
        const auto* header = read(pos, 8);
        if (!header) break;
        const uint32_t type = readBE32(header), size = readLE32(header + 4);
        pos += 8;
        if (size > n - pos) break;
        if (type == 0x666D7420 /* fmt */ && size >= 16) {
            const auto* format = read(pos, std::min<uint32_t>(size, 20));
            if (!format) break;
            info.format = readLE16(format);
            info.channels = std::max<uint16_t>(1, readLE16(format + 2));
            info.sampleRate = std::max<uint32_t>(8000, readLE32(format + 4));
            info.blockBytes = readLE16(format + 12);
            info.bits = readLE16(format + 14);
            if (size >= 20 && readLE16(format + 16) >= 2) info.samplesPerBlock = readLE16(format + 18);
        } else if (type == 0x66616374 /* fact */ && size >= 4) {
            const auto* fact = read(pos, 4);
            if (!fact) break;
            info.factFrames = readLE32(fact);
        } else if (type == 0x64617461 /* data */) {
            info.offset = pos;
            info.length = size;
        }
        pos += (size_t(size) + 1) & ~size_t(1);
    }
    return info;
}

DecodedAudio decodeKnown(const uint8_t* data, size_t n) {
    DecodedAudio out;
    if (!data || n < 12) return out;
    if (decodeCaf(out, data, n) || decodeAiff(out, data, n)) return out;
    out = DecodedAudio{};

    if (std::memcmp(data, "RIFF", 4) == 0 && std::memcmp(data + 8, "WAVE", 4) == 0) {
        const auto info = waveInfo(n, [&](size_t offset, size_t) { return data + offset; });
        out.sampleRate = int(info.sampleRate);
        out.channels = info.channels;
        if (info.format == 2) {
            DecodedAudio decoded;
            if (decodeMsAdpcm(decoded, data, n)) return decoded;
            return DecodedAudio{};
        }
        const auto* pcm = info.offset ? data + info.offset : nullptr;
        if (info.format == 0x11) {
            if (pcm && info.bits == 4 && imaWavToStereo(out, pcm, info.length, info.channels,
                    info.blockBytes, info.samplesPerBlock, info.factFrames)) return out;
            return DecodedAudio{};
        }
        const auto layout = info.layout(pcm);
        if (pcm && info.length && layout.frameBytes()) {
            pcmToStereo(out, layout);
            return out;
        }
    }

    // Fallback for compressed/other containers: estimate duration (~128 kbps)
    out.duration = std::max(0.05, double(n) / 16000.0);
    return out;
}


} // namespace

namespace {
struct Mp3Header { int version = 0, sampleRate = 0, channels = 0; uint32_t size = 0, frames = 0; };
// A layer III frame header at p (4 bytes), or false.
bool parseMp3Header(const uint8_t* p, Mp3Header& h) {
    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return false;
    const int version = (p[1] >> 3) & 3, layer = (p[1] >> 1) & 3;   // 3 = MPEG-1, 2 = MPEG-2, 0 = MPEG-2.5; layer 1 = III
    const int bitrateIndex = p[2] >> 4, rateIndex = (p[2] >> 2) & 3, padding = (p[2] >> 1) & 1;
    if (version == 1 || layer != 1 || bitrateIndex == 0 || bitrateIndex == 15 || rateIndex == 3) return false;
    static const int kbps1[15] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
    static const int kbps2[15] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
    static const int rates[3] = {44100, 48000, 32000};
    const bool v1 = version == 3;
    h.version = version;
    h.sampleRate = rates[rateIndex] >> (version == 3 ? 0 : version == 2 ? 1 : 2);
    h.channels = (p[3] >> 6) == 3 ? 1 : 2;
    h.frames = v1 ? 1152 : 576;
    const int bitrate = (v1 ? kbps1 : kbps2)[bitrateIndex] * 1000;
    h.size = uint32_t((v1 ? 144 : 72) * bitrate / h.sampleRate + padding);
    return h.size >= 24;
}
}
namespace {
template<typename Read, typename PacketHandler>
bool walkMp3(size_t n, Mp3Packets& out, Read read, PacketHandler packet) {
    size_t off = 0;
    const auto* tag = n >= 10 ? read(0, 10) : nullptr;
    if (tag && std::memcmp(tag, "ID3", 3) == 0) {
        off = 10 + ((size_t(tag[6] & 0x7F) << 21) | (size_t(tag[7] & 0x7F) << 14) |
                    (size_t(tag[8] & 0x7F) << 7) | size_t(tag[9] & 0x7F));
        if (tag[5] & 0x10) off += 10;   // footer
    }
    // The first frame: a header whose own length leads to another header of the same stream
    // (or to the end), so stray 0xFF bytes are not taken for audio.
    Mp3Header first;
    const size_t scanEnd = std::min(n, off + (64u << 10));
    for (;; ++off) {
        if (off + 4 > scanEnd) return false;
        const auto* header = read(off, 4);
        if (!header) return false;
        if (!parseMp3Header(header, first)) continue;
        const size_t next = off + first.size;
        Mp3Header second;
        if (next == n) break;
        const auto* following = next + 4 <= n ? read(next, 4) : nullptr;
        if (following && parseMp3Header(following, second) &&
            second.sampleRate == first.sampleRate && second.version == first.version) break;
    }
    // A Xing/Info frame describes the stream and holds no audio: AudioFile does not count it
    // as a packet. A LAME-compatible tag (LAME itself, or ffmpeg's "Lavc") carries the
    // encoder delay and padding that AudioFile exposes as AudioFilePacketTableInfo.
    const size_t side = first.version == 3 ? (first.channels == 1 ? 17 : 32) : (first.channels == 1 ? 9 : 17);
    uint32_t xingFrames = 0;
    int32_t lameDelay = 0, lamePad = 0;
    bool hasLame = false;
    const auto* firstFrame = read(off, first.size);
    if (firstFrame && first.size >= 4 + side + 4 &&
        (std::memcmp(firstFrame + 4 + side, "Xing", 4) == 0 || std::memcmp(firstFrame + 4 + side, "Info", 4) == 0)) {
        const uint8_t* p = firstFrame + 4 + side + 4;
        const uint8_t* const end = firstFrame + first.size;
        if (p + 4 <= end) {
            const uint32_t flags = readBE32(p);
            p += 4;
            if ((flags & 1) && p + 4 <= end) { xingFrames = readBE32(p); p += 4; }
            if ((flags & 2) && p + 4 <= end) p += 4;           // bytes
            if ((flags & 4) && p + 100 <= end) p += 100;       // TOC
            if ((flags & 8) && p + 4 <= end) p += 4;           // quality
            // Encoder delay (12 bits) and padding (12 bits) sit at offset 21 of the 9-byte
            // encoder string. Apple's AudioFile uses these for 'pnfo'.
            if (p + 24 <= end && (std::memcmp(p, "LAME", 4) == 0 || std::memcmp(p, "Lavc", 4) == 0)) {
                const uint32_t d = (uint32_t(p[21]) << 16) | (uint32_t(p[22]) << 8) | uint32_t(p[23]);
                lameDelay = int32_t((d >> 12) & 0xFFF);
                lamePad = int32_t(d & 0xFFF);
                hasLame = true;
            }
        }
        off += first.size;
    }
    out = Mp3Packets{};
    out.sampleRate = first.sampleRate;
    out.channels = first.channels;
    out.framesPerPacket = first.frames;
    out.dataOffset = uint32_t(off);
    size_t packetCount = 0;
    while (off + 4 <= n) {
        Mp3Header h;
        const auto* header = read(off, 4);
        if (!header || !parseMp3Header(header, h) || h.sampleRate != first.sampleRate ||
            h.channels != first.channels || h.frames != first.frames || off + h.size > n) break;
        packet(packetCount++, uint32_t(off), h.size);
        out.maxPacketSize = std::max(out.maxPacketSize, h.size);
        out.dataBytes += h.size;
        off += h.size;
    }
    if (!packetCount) return false;
    // Prefer the Xing frame count when it is present: that is what AudioFile uses.
    const int64_t mpeg = int64_t(xingFrames ? xingFrames : packetCount) * int64_t(out.framesPerPacket);
    if (hasLame && mpeg > int64_t(lameDelay) + int64_t(lamePad)) {
        out.primingFrames = lameDelay;
        out.remainderFrames = lamePad;
        out.validFrames = uint64_t(mpeg - lameDelay - lamePad);
    } else {
        out.validFrames = uint64_t(std::max(int64_t(0), mpeg));
    }
    return true;
}
}

bool indexMp3(const uint8_t* data, size_t n, Mp3Packets& out) {
    if (!data) return false;
    return walkMp3(n, out, [&](size_t offset, size_t) { return data + offset; },
                   [&](size_t, uint32_t offset, uint32_t size) {
        out.packets.emplace_back(offset, size);
    });
}

struct Mp3Stream {
    mp3dec_t dec;
    std::vector<uint8_t> carry;   // the tail of the last call that was not a whole frame
    Mp3Stream() { mp3dec_init(&dec); }
};
std::shared_ptr<Mp3Stream> mp3StreamNew() { return std::make_shared<Mp3Stream>(); }
int mp3StreamDecode(Mp3Stream& s, const uint8_t* data, size_t n, std::vector<int16_t>& stereoOut) {
    std::vector<uint8_t> joined;
    if (!s.carry.empty()) {
        joined = std::move(s.carry);
        joined.insert(joined.end(), data, data + n);
        data = joined.data();
        n = joined.size();
    }
    std::vector<int16_t> pcm(MINIMP3_MAX_SAMPLES_PER_FRAME);
    int rate = 0;
    size_t off = 0;
    while (off + 4 <= n) {
        Mp3Header h;
        if (!parseMp3Header(data + off, h)) { ++off; continue; }
        if (off + h.size > n) break;   // the rest of this frame comes with the next packet
        mp3dec_frame_info_t info{};
        const int samples = mp3dec_decode_frame(&s.dec, data + off, int(h.size), pcm.data(), &info);
        off += h.size;
        if (samples <= 0) continue;    // the first frames of a stream can lack their bit reservoir
        rate = info.hz;
        for (int i = 0; i < samples; ++i) {
            const int16_t l = pcm[size_t(i * info.channels)];
            stereoOut.push_back(l);
            stereoOut.push_back(info.channels >= 2 ? pcm[size_t(i * info.channels + 1)] : l);
        }
    }
    std::vector<uint8_t> rest(data + std::min(off, n), data + n);
    s.carry = std::move(rest);
    if (s.carry.size() > (64u << 10)) s.carry.clear();   // not MP3 at all
    return rate;
}

namespace {
// MPEG-1/2 layer III (and I/II) via minimp3. Skips an ID3v2 tag first.
bool decodeMp3(DecodedAudio& out, const uint8_t* data, size_t n) {
    size_t off = 0;
    if (n >= 10 && std::memcmp(data, "ID3", 3) == 0) {
        off = 10 + ((size_t(data[6] & 0x7F) << 21) | (size_t(data[7] & 0x7F) << 14) | (size_t(data[8] & 0x7F) << 7) | size_t(data[9] & 0x7F));
        if (data[5] & 0x10) off += 10;   // footer
    }
    if (off >= n) return false;
    // Start at the first audio frame: a Xing/Info frame describes the stream and decodes to
    // 26 ms of silence that is not part of it.
    if (Mp3Packets index; indexMp3(data, n, index)) off = index.dataOffset;
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

namespace {
constexpr size_t kStreamThreshold = 256 * 1024;

// Byte ownership for NSData and file players. MP3's sequential header
// scan and decoder share a bounded read-ahead buffer for file input.
struct StreamBytes {
    std::vector<uint8_t> owned;
    size_t length = 0;
    int file = -1;
    std::vector<uint8_t> buffer;
    size_t start = 0, available = 0;

    ~StreamBytes() {
        if (file >= 0) ::close(file);
    }

    static constexpr size_t kReadAhead = 65536;
    size_t size() const { return file >= 0 ? length : owned.size(); }

    const uint8_t* read(size_t offset, size_t needed) {
        if (offset > size() || needed > size() - offset) return nullptr;
        if (file < 0) return owned.data() + offset;
        if (offset < start || offset - start > available || needed > available - (offset - start)) {
            start = offset;
            ssize_t got;
            do { got = ::pread(file, buffer.data(), buffer.size(), off_t(start)); } while (got < 0 && errno == EINTR);
            available = got > 0 ? size_t(got) : 0;
        }
        return needed <= available - (offset - start) ? buffer.data() + (offset - start) : nullptr;
    }
};

class VorbisSource final : public AudioSource {
public:
    static std::shared_ptr<VorbisSource> create(stb_vorbis* handle, std::shared_ptr<StreamBytes> bytes = {}) {
        Handle decoder(handle, stb_vorbis_close);
        if (!decoder) return nullptr;
        const auto info = stb_vorbis_get_info(decoder.get());
        const auto length = stb_vorbis_stream_length_in_samples(decoder.get());
        if (!length || info.sample_rate < 4000 || info.sample_rate > 192000 || info.channels < 1) return nullptr;
        return std::make_shared<VorbisSource>(std::move(decoder), std::move(bytes), info, length);
    }

    using Handle = std::unique_ptr<stb_vorbis, decltype(&stb_vorbis_close)>;
    VorbisSource(Handle decoder, std::shared_ptr<StreamBytes> bytes, stb_vorbis_info info, uint64_t length)
        : AudioSource(int(info.sample_rate), info.channels, length), bytes_(std::move(bytes)), decoder_(std::move(decoder)) {}

    size_t read(uint64_t first, int16_t* stereo, size_t count) override {
        if (first >= frames() || !count) return 0;
        if (first != cursor_ && !stb_vorbis_seek(decoder_.get(), unsigned(first))) return 0;
        cursor_ = first;
        count = std::min({count, size_t(frames() - first), size_t(std::numeric_limits<int>::max() / 2)});
        // Ask for one channel for mono so exactly the same samples go to left and right.
        const int outputChannels = channels() == 1 ? 1 : 2;
        const size_t got = stb_vorbis_get_samples_short_interleaved(decoder_.get(), outputChannels, stereo, int(count) * outputChannels);
        if (outputChannels == 1)
            for (size_t i = got; i != 0; --i) stereo[2 * i - 1] = stereo[2 * i - 2] = stereo[i - 1];
        cursor_ += got;
        return got;
    }

private:
    std::shared_ptr<StreamBytes> bytes_;
    Handle decoder_;
    uint64_t cursor_ = 0;
};

class Mp3FileSource final : public AudioSource {
public:
    static constexpr uint64_t kCheckpoint = 64;

    Mp3FileSource(std::shared_ptr<StreamBytes> bytes, const Mp3Packets& index, size_t packets, std::vector<uint32_t> offsets)
        : AudioSource(index.sampleRate, index.channels, uint64_t(packets) * index.framesPerPacket),
          bytes_(std::move(bytes)), offsets_(std::move(offsets)), packetFrames_(index.framesPerPacket), offset_(index.dataOffset) {
        mp3dec_init(&decoder_);
    }

    size_t read(uint64_t first, int16_t* stereo, size_t count) override {
        if (first >= frames() || !count) return 0;
        count = std::min<uint64_t>(count, frames() - first);
        const uint64_t wantedPacket = first / packetFrames_;
        if (!havePacket_ || wantedPacket < packet_ || wantedPacket > packet_ + 1) {
            // Start before the requested packet so Layer III's bit reservoir,
            // overlap/add and synthesis filter are warmed up before returning PCM.
            const uint64_t checkpoint = wantedPacket / kCheckpoint;
            const uint64_t warmup = checkpoint > 0 ? checkpoint - 1 : 0;
            packet_ = warmup * kCheckpoint;
            offset_ = offsets_[warmup];
            havePacket_ = false;
            mp3dec_init(&decoder_);
        }

        size_t produced = 0;
        while (produced < count) {
            const uint64_t position = first + produced;
            const uint64_t wanted = position / packetFrames_;
            while (!havePacket_ || packet_ < wanted) {
                if (havePacket_) ++packet_;
                if (!readPacket()) return produced;
                havePacket_ = true;
            }
            const size_t skip = position % packetFrames_;
            const size_t take = std::min(count - produced, packetFrames_ - skip);
            std::copy_n(pcm_.data() + skip * 2, take * 2, stereo + produced * 2);
            produced += take;
        }
        return produced;
    }

private:
    bool readPacket() {
        Mp3Header header;
        const auto* bytes = bytes_->read(offset_, 4);
        if (!bytes || !parseMp3Header(bytes, header)) return false;
        bytes = bytes_->read(offset_, header.size);
        if (!bytes) return false;
        mp3dec_frame_info_t info{};
        const int samples = mp3dec_decode_frame(&decoder_, bytes, int(header.size), pcm_.data(), &info);
        offset_ += header.size;
        if (samples != int(packetFrames_)) pcm_.fill(0);
        else if (info.channels == 1)
            for (size_t i = samples; i != 0; --i) pcm_[2 * i - 1] = pcm_[2 * i - 2] = pcm_[i - 1];
        return true;
    }

    std::shared_ptr<StreamBytes> bytes_;
    std::vector<uint32_t> offsets_;
    size_t packetFrames_, offset_;
    uint64_t packet_ = 0;
    mp3dec_t decoder_{};
    std::array<int16_t, MINIMP3_MAX_SAMPLES_PER_FRAME> pcm_;
    bool havePacket_ = false;
};

class PcmSource final : public AudioSource {
public:
    static std::shared_ptr<AudioSource> create(std::shared_ptr<StreamBytes> bytes, bool caf) {
        const auto read = [&](size_t offset, size_t count) { return bytes->read(offset, count); };
        PcmLayout pcm;
        int rate;
        size_t offset;
        if (caf) {
            const auto info = cafInfo(bytes->size(), read);
            if (info.format != 0x6C70636D /* lpcm */) return nullptr;
            offset = info.offset;
            pcm = info.layout(nullptr);
            rate = info.sampleRate();
        } else {
            const auto info = waveInfo(bytes->size(), read);
            offset = info.offset;
            pcm = info.layout(nullptr);
            rate = int(info.sampleRate);
        }
        const size_t stride = pcm.frameBytes();
        if (!offset || !stride || stride > StreamBytes::kReadAhead || pcm.bytes < stride || rate <= 0) return nullptr;
        return std::make_shared<PcmSource>(std::move(bytes), pcm, offset, rate);
    }

    PcmSource(std::shared_ptr<StreamBytes> bytes, PcmLayout layout, size_t offset, int rate)
        : AudioSource(rate, layout.channels, layout.bytes / layout.frameBytes()),
          bytes_(std::move(bytes)), layout_(layout), offset_(offset) {}

    size_t read(uint64_t first, int16_t* stereo, size_t count) override {
        if (first >= frames() || !count) return 0;
        count = std::min<uint64_t>(count, frames() - first);
        const size_t stride = layout_.frameBytes();
        auto chunk = layout_;
        size_t done = 0;
        while (done < count) {
            const size_t take = std::min(count - done, StreamBytes::kReadAhead / stride);
            chunk.data = bytes_->read(offset_ + (first + done) * stride, take * stride);
            if (!chunk.data) break;
            convertPcm(chunk, take, stereo + done * 2);
            done += take;
        }
        return done;
    }

private:
    std::shared_ptr<StreamBytes> bytes_;
    PcmLayout layout_;
    size_t offset_;
};

bool isOgg(const uint8_t* data, size_t size) {
    return size >= 4 && std::memcmp(data, "OggS", 4) == 0;
}

std::shared_ptr<AudioSource> openMp3(std::shared_ptr<StreamBytes> bytes) {
    Mp3Packets index;
    std::vector<uint32_t> checkpoints;
    size_t packets = 0;
    auto read = [&](size_t offset, size_t count) { return bytes->read(offset, count); };
    if (!walkMp3(bytes->size(), index, read, [&](size_t packet, uint32_t offset, uint32_t) {
        if (packet % Mp3FileSource::kCheckpoint == 0) checkpoints.push_back(offset);
        ++packets;
    })) return nullptr;
    return std::make_shared<Mp3FileSource>(std::move(bytes), index, packets, std::move(checkpoints));
}

std::shared_ptr<AudioSource> openMemory(std::shared_ptr<StreamBytes> bytes) {
    const size_t size = bytes->size();
    const uint8_t* data = bytes->owned.data();
    if (looksLikeMp3(data, size)) return openMp3(std::move(bytes));
    if (isOgg(data, size)) return VorbisSource::create(stb_vorbis_open_memory(data, int(size), nullptr, nullptr), std::move(bytes));
    if (size >= 12) {
        if (std::memcmp(data, "RIFF", 4) == 0 && std::memcmp(data + 8, "WAVE", 4) == 0)
            return PcmSource::create(std::move(bytes), false);
        if (std::memcmp(data, "caff", 4) == 0)
            if (auto pcm = PcmSource::create(bytes, true)) return pcm;
    }
    return openHostStream(data, size, bytes);
}
}

std::shared_ptr<AudioSource> openStream(const std::string& path) {
    std::unique_ptr<std::FILE, decltype(&std::fclose)> file(std::fopen(path.c_str(), "rb"), std::fclose);
    if (!file) return nullptr;
    struct stat state{};
    const int fd = fileno(file.get());
    if (::fstat(fd, &state) != 0 || !S_ISREG(state.st_mode) || state.st_size < int64_t(kStreamThreshold) ||
        uint64_t(state.st_size) > uint64_t(std::numeric_limits<int>::max())) return nullptr;
    uint8_t header[12];
    if (std::fread(header, 1, sizeof header, file.get()) != sizeof header) return nullptr;
    if (isOgg(header, sizeof header)) {
        if (std::fseek(file.get(), 0, SEEK_SET) != 0) return nullptr;
        // stb owns the FILE even when it rejects the stream.
        return VorbisSource::create(stb_vorbis_open_file(file.release(), true, nullptr, nullptr));
    }
    const bool m4a = std::memcmp(header + 4, "ftyp", 4) == 0;
    const bool caf = std::memcmp(header, "caff", 4) == 0;
    if (m4a || caf) {
        if (auto source = openHostStream(fd, size_t(state.st_size))) return source;
        if (m4a) return nullptr;
    }
    const bool mp3 = looksLikeMp3(header, sizeof header);
    const bool wav = std::memcmp(header, "RIFF", 4) == 0 && std::memcmp(header + 8, "WAVE", 4) == 0;
    if (!mp3 && !wav && !caf) return nullptr;
    auto bytes = std::make_shared<StreamBytes>();
    bytes->file = ::dup(fd);
    if (bytes->file < 0) return nullptr;
    bytes->length = size_t(state.st_size);
    bytes->buffer.resize(StreamBytes::kReadAhead);
    return mp3 ? openMp3(std::move(bytes)) : PcmSource::create(std::move(bytes), caf);
}

std::shared_ptr<AudioSource> openStream(std::vector<uint8_t>& input) {
    if (input.size() < kStreamThreshold || input.size() > size_t(std::numeric_limits<int>::max())) return nullptr;
    auto bytes = std::make_shared<StreamBytes>();
    bytes->owned = std::move(input);
    auto source = openMemory(bytes);
    if (!source) input = std::move(bytes->owned);
    return source;
}

bool SourceWindow::pair(AudioSource& source, uint64_t first, int16_t (&current)[2], int16_t (&next)[2], bool loop) {
    if (first < start_ || first - start_ >= std::min(count_, kFrames)) {
        const bool adjacent = count_ == kFrames + 1 && first == start_ + kFrames;
        start_ = first;
        if (adjacent) {
            std::copy_n(samples_.data() + kFrames * 2, 2, samples_.data());
            count_ = 1 + source.read(first + 1, samples_.data() + 2, kFrames);
        } else {
            count_ = source.read(first, samples_.data(), kFrames + 1);
        }
    }
    if (start_ == 0 && count_ && !haveFirst_) {
        std::copy_n(samples_.data(), 2, first_);
        haveFirst_ = true;
    }
    const size_t index = size_t(first - start_);
    if (index >= count_) return false;
    const size_t following = std::min(index + 1, count_ - 1);
    std::copy_n(samples_.data() + index * 2, 2, current);
    if (loop && first + 1 == source.frames()) {
        if (!haveFirst_) haveFirst_ = source.read(0, first_, 1) == 1;
        if (haveFirst_) {
            std::copy_n(first_, 2, next);
            return true;
        }
    }
    std::copy_n(samples_.data() + following * 2, 2, next);
    return true;
}

DecodedAudio decode(const uint8_t* data, size_t n) {
    DecodedAudio host;
    if (decodeHostContainer(host, data, n)) return host;
    if (data && looksLikeMp3(data, n)) {
        DecodedAudio out;
        if (decodeMp3(out, data, n)) return out;
    }
    if (data && n <= size_t(std::numeric_limits<int>::max()) && isOgg(data, n)) {
        auto source = VorbisSource::create(stb_vorbis_open_memory(data, int(n), nullptr, nullptr));
        if (source) {
            DecodedAudio out;
            out.sampleRate = source->sampleRate();
            out.channels = source->channels();
            constexpr size_t kMaxFrames = 64u * 1024 * 1024;
            if (source->frames() > kMaxFrames) return out;
            out.stereoPcm.resize(size_t(source->frames()) * 2);
            const size_t frames = source->read(0, out.stereoPcm.data(), size_t(source->frames()));
            out.stereoPcm.resize(frames * 2);
            out.duration = double(frames) / out.sampleRate;
            return out;
        }
    }
    return decodeKnown(data, n);
}

} // namespace Hydrogen::Runtime::Audio
