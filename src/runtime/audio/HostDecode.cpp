// Container/codec work belongs to host libraries. Existing raw decoders stay unchanged.
#include "Audio.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cerrno>
#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#elif defined(HVM_HAVE_FFMPEG)
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/samplefmt.h>
}
#endif

namespace Hydrogen::Runtime::Audio {
namespace {
constexpr size_t kMaxSamples = 128u * 1024 * 1024;
struct Input { const uint8_t* data; size_t size; int64_t position = 0; };
int16_t quantize(double value) {
    if (!std::isfinite(value)) return 0;
    return int16_t(std::clamp(std::lrint(std::clamp(value, -1.0, 1.0) * 32768.0), -32768l, 32767l));
}
#if defined(__APPLE__)
OSStatus readInput(void* context, SInt64 position, UInt32 count, void* buffer, UInt32* actual) {
    auto& input = *static_cast<Input*>(context);
    if (position < 0 || uint64_t(position) > input.size) { *actual = 0; return kAudio_ParamError; }
    *actual = UInt32(std::min<size_t>(count, input.size - size_t(position)));
    std::memcpy(buffer, input.data + position, *actual);
    return noErr;
}
SInt64 inputSize(void* context) { return SInt64(static_cast<Input*>(context)->size); }
#elif defined(HVM_HAVE_FFMPEG)
int readInput(void* context, uint8_t* buffer, int count) {
    auto& input = *static_cast<Input*>(context);
    if (count <= 0) return AVERROR(EINVAL);
    const size_t available = input.size - size_t(input.position);
    const size_t copied = std::min<size_t>(count, available);
    if (!copied) return AVERROR_EOF;
    std::memcpy(buffer, input.data + input.position, copied);
    input.position += int64_t(copied);
    return int(copied);
}
int64_t seekInput(void* context, int64_t offset, int mode) {
    auto& input = *static_cast<Input*>(context);
    mode &= ~AVSEEK_FORCE;
    if (mode == AVSEEK_SIZE) return int64_t(input.size);
    int64_t base;
    if (mode == SEEK_SET) base = 0;
    else if (mode == SEEK_CUR) base = input.position;
    else if (mode == SEEK_END) base = int64_t(input.size);
    else return AVERROR(EINVAL);
    if (offset < -base || offset > int64_t(input.size) - base) return AVERROR(EINVAL);
    input.position = base + offset;
    return input.position;
}
struct Decoder {
    AVFormatContext* format = nullptr;
    AVIOContext* io = nullptr;
    AVCodecContext* codec = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    ~Decoder() {
        av_frame_free(&frame); av_packet_free(&packet); avcodec_free_context(&codec);
        avformat_close_input(&format);
        if (io) av_freep(&io->buffer);
        avio_context_free(&io);
    }
};
#endif
}

// MP4/M4A, and CAF holding a codec the built-in decoders don't read (AAC in
// Jetpack Joyride's music archive). CAF with LPCM or IMA4 stays built in.
bool hostContainer(const uint8_t* data, size_t size) {
    if (std::memcmp(data + 4, "ftyp", 4) == 0) return true;
    if (std::memcmp(data, "caff", 4) != 0) return false;
    for (size_t pos = 8; pos + 12 + 12 <= size;) {
        const uint8_t* h = data + pos;
        uint64_t chunk = 0;
        for (int i = 0; i < 8; ++i) chunk = (chunk << 8) | h[4 + i];
        if (std::memcmp(h, "desc", 4) == 0 && chunk >= 32) {
            const uint8_t* f = h + 12 + 8;
            const uint32_t fmt = (uint32_t(f[0]) << 24) | (uint32_t(f[1]) << 16) | (uint32_t(f[2]) << 8) | f[3];
            return fmt != 0x6C70636D /* lpcm */ && fmt != 0x696D6134 /* ima4 */;
        }
        if (chunk > size - pos - 12) break;
        pos += 12 + size_t(chunk);
    }
    return false;
}

bool decodeHostContainer(DecodedAudio& out, const uint8_t* data, size_t size) {
    if (!data || size < 12 || size > 256u * 1024 * 1024 || !hostContainer(data, size)) return false;
    DecodedAudio decoded;
#if defined(__APPLE__)
    Input input{data, size};
    struct Files {
        AudioFileID file = nullptr;
        ExtAudioFileRef extended = nullptr;
        ~Files() { if (extended) ExtAudioFileDispose(extended); if (file) AudioFileClose(file); }
    } files;
    if (AudioFileOpenWithCallbacks(&input, readInput, nullptr, inputSize, nullptr, 0, &files.file) != noErr ||
        ExtAudioFileWrapAudioFileID(files.file, false, &files.extended) != noErr) return false;
    AudioStreamBasicDescription source{};
    UInt32 bytes = sizeof(source);
    if (ExtAudioFileGetProperty(files.extended, kExtAudioFileProperty_FileDataFormat, &bytes, &source) != noErr ||
        !std::isfinite(source.mSampleRate) || source.mSampleRate < 4000 || source.mSampleRate > 192000 ||
        source.mChannelsPerFrame < 1 || source.mChannelsPerFrame > 8) return false;
    decoded.sampleRate = int(source.mSampleRate);
    decoded.channels = int(source.mChannelsPerFrame);
    AudioStreamBasicDescription client{};
    client.mSampleRate = source.mSampleRate;
    client.mFormatID = kAudioFormatLinearPCM;
    client.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
    client.mChannelsPerFrame = source.mChannelsPerFrame;
    client.mBitsPerChannel = 32;
    client.mFramesPerPacket = 1;
    client.mBytesPerFrame = client.mBytesPerPacket = 4 * client.mChannelsPerFrame;
    if (ExtAudioFileSetProperty(files.extended, kExtAudioFileProperty_ClientDataFormat, sizeof(client), &client) != noErr) return false;
    std::vector<float> samples(4096 * decoded.channels);
    for (;;) {
        UInt32 frames = 4096;
        AudioBufferList buffers{};
        buffers.mNumberBuffers = 1;
        buffers.mBuffers[0] = {client.mChannelsPerFrame, UInt32(samples.size() * sizeof(float)), samples.data()};
        if (ExtAudioFileRead(files.extended, &frames, &buffers) != noErr) return false;
        if (!frames) break;
        if (frames > 4096 || frames * 2 > kMaxSamples - decoded.stereoPcm.size()) return false;
        for (UInt32 i = 0; i < frames; ++i) {
            decoded.stereoPcm.push_back(quantize(samples[i * decoded.channels]));
            decoded.stereoPcm.push_back(quantize(samples[i * decoded.channels + (decoded.channels > 1 ? 1 : 0)]));
        }
    }
#elif defined(HVM_HAVE_FFMPEG)
    Input input{data, size};
    Decoder decoder;
    auto* buffer = static_cast<uint8_t*>(av_malloc(32768));
    if (!buffer) return false;
    decoder.io = avio_alloc_context(buffer, 32768, 0, &input, readInput, nullptr, seekInput);
    if (!decoder.io) { av_free(buffer); return false; }
    decoder.format = avformat_alloc_context();
    if (!decoder.format) return false;
    decoder.format->pb = decoder.io;
    decoder.format->flags |= AVFMT_FLAG_CUSTOM_IO;
    // Only MOV/MP4 with our memory reader. Never allow external data references.
    AVDictionary* options = nullptr;
    av_dict_set(&options, "enable_drefs", "0", 0);
    const int opened = avformat_open_input(&decoder.format, nullptr, av_find_input_format("mov"), &options);
    av_dict_free(&options);
    if (opened < 0 || avformat_find_stream_info(decoder.format, nullptr) < 0) return false;
    int stream = -1;
    for (unsigned i = 0; i < decoder.format->nb_streams; ++i)
        if (decoder.format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) { stream = int(i); break; }
    if (stream < 0) return false;
    const auto* parameters = decoder.format->streams[stream]->codecpar;
    const auto* codec = avcodec_find_decoder(parameters->codec_id);
    if (!codec) return false;
    decoder.codec = avcodec_alloc_context3(codec);
    if (!decoder.codec || avcodec_parameters_to_context(decoder.codec, parameters) < 0 ||
        avcodec_open2(decoder.codec, codec, nullptr) < 0) return false;
    decoded.sampleRate = decoder.codec->sample_rate;
    decoded.channels = decoder.codec->ch_layout.nb_channels;
    if (decoded.sampleRate < 4000 || decoded.sampleRate > 192000 || decoded.channels < 1 || decoded.channels > 8) return false;
    decoder.packet = av_packet_alloc(); decoder.frame = av_frame_alloc();
    if (!decoder.packet || !decoder.frame) return false;
    auto drain = [&]() {
        for (;;) {
            const int result = avcodec_receive_frame(decoder.codec, decoder.frame);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return true;
            if (result < 0) return false;
            auto* frame = decoder.frame;
            const auto format = AVSampleFormat(frame->format);
            const auto packed = av_get_packed_sample_fmt(format);
            const int stride = av_get_bytes_per_sample(format);
            if (frame->sample_rate != decoded.sampleRate || frame->ch_layout.nb_channels != decoded.channels ||
                stride <= 0 || frame->nb_samples < 0 || size_t(frame->nb_samples) * 2 > kMaxSamples - decoded.stereoPcm.size()) return false;
            auto sample = [&](int i, int channel) -> double {
                const bool planar = av_sample_fmt_is_planar(format);
                const uint8_t* at = frame->extended_data[planar ? channel : 0] + size_t(planar ? i : i * decoded.channels + channel) * stride;
                switch (packed) {
                    case AV_SAMPLE_FMT_U8: return (double(*at) - 128) / 128;
                    case AV_SAMPLE_FMT_S16: { int16_t v; std::memcpy(&v, at, 2); return double(v) / 32768; }
                    case AV_SAMPLE_FMT_S32: { int32_t v; std::memcpy(&v, at, 4); return double(v >> 16) / 32768; }
                    case AV_SAMPLE_FMT_S64: { int64_t v; std::memcpy(&v, at, 8); return double(v >> 48) / 32768; }
                    case AV_SAMPLE_FMT_FLT: { float v; std::memcpy(&v, at, 4); return v; }
                    case AV_SAMPLE_FMT_DBL: { double v; std::memcpy(&v, at, 8); return v; }
                    default: return 0;
                }
            };
            if (packed < AV_SAMPLE_FMT_U8 || packed > AV_SAMPLE_FMT_S64) return false;
            for (int i = 0; i < frame->nb_samples; ++i) {
                decoded.stereoPcm.push_back(quantize(sample(i, 0)));
                decoded.stereoPcm.push_back(quantize(sample(i, decoded.channels > 1 ? 1 : 0)));
            }
            av_frame_unref(frame);
        }
    };
    int result;
    while ((result = av_read_frame(decoder.format, decoder.packet)) >= 0) {
        const bool accepted = decoder.packet->stream_index != stream ||
            (avcodec_send_packet(decoder.codec, decoder.packet) >= 0 && drain());
        av_packet_unref(decoder.packet);
        if (!accepted) return false;
    }
    if (result != AVERROR_EOF || avcodec_send_packet(decoder.codec, nullptr) < 0 || !drain()) return false;
#else
    return false;
#endif
    if (decoded.stereoPcm.empty()) return false;
    decoded.duration = double(decoded.stereoPcm.size() / 2) / decoded.sampleRate;
    out = std::move(decoded);
    return true;
}
} // namespace Hydrogen::Runtime::Audio
