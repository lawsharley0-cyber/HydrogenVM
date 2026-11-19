// Container/codec work belongs to host libraries. Existing raw decoders stay unchanged.
#include "Audio.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <limits>
#include <utility>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
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
struct Input {
    explicit Input(const uint8_t* data, size_t size, std::shared_ptr<const void> owner = {})
        : data(data), size(size), owner(std::move(owner)) {}
    explicit Input(int fd, size_t size) : size(size), file(::dup(fd)) {}
    ~Input() { if (file >= 0) ::close(file); }
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;

    int64_t readAt(uint64_t at, void* dest, size_t count) const {
        if (at >= size) return 0;
        count = std::min(count, size - size_t(at));
        if (data) {
            std::memcpy(dest, data + at, count);
            return int64_t(count);
        }
        ssize_t read;
        do { read = ::pread(file, dest, count, off_t(at)); } while (read < 0 && errno == EINTR);
        return read;
    }

    const uint8_t* data = nullptr;
    size_t size;
    int64_t position = 0;
    int file = -1;
    std::shared_ptr<const void> owner;
};
int16_t quantize(double value) {
    if (!std::isfinite(value)) return 0;
    return int16_t(std::clamp(std::lrint(std::clamp(value, -1.0, 1.0) * 32768.0), -32768l, 32767l));
}
#if defined(__APPLE__)
OSStatus readInput(void* context, SInt64 position, UInt32 count, void* buffer, UInt32* actual) {
    auto& input = *static_cast<Input*>(context);
    if (position < 0 || uint64_t(position) > input.size) { *actual = 0; return kAudio_ParamError; }
    const auto read = input.readAt(uint64_t(position), buffer, count);
    *actual = read >= 0 ? UInt32(read) : 0;
    return read >= 0 ? noErr : kAudio_ParamError;
}
SInt64 inputSize(void* context) { return SInt64(static_cast<Input*>(context)->size); }
struct AppleFiles {
    AudioFileID file = nullptr;
    ExtAudioFileRef extended = nullptr;
    ~AppleFiles() { if (extended) ExtAudioFileDispose(extended); if (file) AudioFileClose(file); }
};

bool openAppleDecoder(AppleFiles& files, Input& input, AudioStreamBasicDescription& source,
                      AudioStreamBasicDescription& client) {
    if (AudioFileOpenWithCallbacks(&input, readInput, nullptr, inputSize, nullptr, 0, &files.file) != noErr ||
        ExtAudioFileWrapAudioFileID(files.file, false, &files.extended) != noErr) return false;
    UInt32 bytes = sizeof(source);
    if (ExtAudioFileGetProperty(files.extended, kExtAudioFileProperty_FileDataFormat, &bytes, &source) != noErr ||
        !std::isfinite(source.mSampleRate) || source.mSampleRate < 4000 || source.mSampleRate > 192000 ||
        source.mChannelsPerFrame < 1 || source.mChannelsPerFrame > 8) return false;
    client.mSampleRate = source.mSampleRate;
    client.mFormatID = kAudioFormatLinearPCM;
    client.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
    client.mChannelsPerFrame = source.mChannelsPerFrame;
    client.mBitsPerChannel = 32;
    client.mFramesPerPacket = 1;
    client.mBytesPerFrame = client.mBytesPerPacket = 4 * client.mChannelsPerFrame;
    return ExtAudioFileSetProperty(files.extended, kExtAudioFileProperty_ClientDataFormat, sizeof(client), &client) == noErr;
}

#elif defined(HVM_HAVE_FFMPEG)
int readInput(void* context, uint8_t* buffer, int count) {
    auto& input = *static_cast<Input*>(context);
    if (count <= 0) return AVERROR(EINVAL);
    const int64_t copied = input.readAt(uint64_t(input.position), buffer, size_t(count));
    if (copied <= 0) return copied == 0 ? AVERROR_EOF : AVERROR(EIO);
    input.position += copied;
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

bool openDecoder(Decoder& decoder, Input& input, int& stream) {
    auto* buffer = static_cast<uint8_t*>(av_malloc(32768));
    if (!buffer) return false;
    decoder.io = avio_alloc_context(buffer, 32768, 0, &input, readInput, nullptr, seekInput);
    if (!decoder.io) { av_free(buffer); return false; }
    decoder.format = avformat_alloc_context();
    if (!decoder.format) return false;
    decoder.format->pb = decoder.io;
    decoder.format->flags |= AVFMT_FLAG_CUSTOM_IO;
    // Only selected local audio containers with our memory reader. Never allow external data references.
    AVDictionary* options = nullptr;
    av_dict_set(&options, "enable_drefs", "0", 0);
    char magic[4]{};
    input.readAt(0, magic, sizeof magic);
    const auto* format = av_find_input_format(std::memcmp(magic, "caff", 4) == 0 ? "caf" : "mov");
    const int opened = avformat_open_input(&decoder.format, nullptr, format, &options);
    av_dict_free(&options);
    if (opened < 0 || avformat_find_stream_info(decoder.format, nullptr) < 0) return false;
    stream = -1;
    for (unsigned i = 0; i < decoder.format->nb_streams; ++i)
        if (decoder.format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) { stream = int(i); break; }
    if (stream < 0) return false;
    const auto* parameters = decoder.format->streams[stream]->codecpar;
    const auto* codec = avcodec_find_decoder(parameters->codec_id);
    if (!codec) return false;
    decoder.codec = avcodec_alloc_context3(codec);
    if (!decoder.codec || avcodec_parameters_to_context(decoder.codec, parameters) < 0 ||
        avcodec_open2(decoder.codec, codec, nullptr) < 0) return false;
    const int rate = decoder.codec->sample_rate;
    const int channels = decoder.codec->ch_layout.nb_channels;
    if (rate < 4000 || rate > 192000 || channels < 1 || channels > 8) return false;
    decoder.packet = av_packet_alloc(); decoder.frame = av_frame_alloc();
    return decoder.packet && decoder.frame;
}

bool validFrame(const AVFrame& frame, int rate, int channels) {
    const auto format = av_get_packed_sample_fmt(AVSampleFormat(frame.format));
    return frame.sample_rate == rate && frame.ch_layout.nb_channels == channels && frame.nb_samples > 0 &&
           format >= AV_SAMPLE_FMT_U8 && format <= AV_SAMPLE_FMT_S64;
}

int16_t frameSample(const AVFrame& frame, int i, int channel) {
    const auto format = AVSampleFormat(frame.format);
    const bool planar = av_sample_fmt_is_planar(format);
    const int stride = av_get_bytes_per_sample(format);
    const uint8_t* at = frame.extended_data[planar ? channel : 0] +
        size_t(planar ? i : i * frame.ch_layout.nb_channels + channel) * stride;
    switch (av_get_packed_sample_fmt(format)) {
        case AV_SAMPLE_FMT_U8: return quantize((double(*at) - 128) / 128);
        case AV_SAMPLE_FMT_S16: { int16_t v; std::memcpy(&v, at, 2); return v; }
        case AV_SAMPLE_FMT_S32: { int32_t v; std::memcpy(&v, at, 4); return int16_t(v >> 16); }
        case AV_SAMPLE_FMT_S64: { int64_t v; std::memcpy(&v, at, 8); return quantize(double(v >> 48) / 32768); }
        case AV_SAMPLE_FMT_FLT: { float v; std::memcpy(&v, at, 4); return quantize(v); }
        case AV_SAMPLE_FMT_DBL: { double v; std::memcpy(&v, at, 8); return quantize(v); }
        default: return 0;
    }
}
#endif
}

// MP4/M4A, and CAF holding a codec the built-in decoders don't read (AAC in
// Jetpack Joyride's music archive). CAF with LPCM or IMA4 stays built in.
namespace {
bool hostContainer(const Input& input) {
    uint8_t header[24];
    if (input.readAt(0, header, 12) != 12) return false;
    if (std::memcmp(header + 4, "ftyp", 4) == 0) return true;
    if (std::memcmp(header, "caff", 4) != 0) return false;
    for (size_t pos = 8; pos + sizeof header <= input.size;) {
        if (input.readAt(pos, header, sizeof header) != sizeof header) return false;
        uint64_t chunk = 0;
        for (int i = 0; i < 8; ++i) chunk = (chunk << 8) | header[4 + i];
        if (std::memcmp(header, "desc", 4) == 0 && chunk >= 32)
            return std::memcmp(header + 20, "lpcm", 4) != 0 && std::memcmp(header + 20, "ima4", 4) != 0;
        if (chunk > input.size - pos - 12) break;
        pos += 12 + size_t(chunk);
    }
    return false;
}

#if defined(__APPLE__)
class AppleSource final : public AudioSource {
public:
    AppleSource(std::unique_ptr<Input> input, std::unique_ptr<AppleFiles> files,
                const AudioStreamBasicDescription& format, uint64_t length)
        : AudioSource(int(format.mSampleRate), int(format.mChannelsPerFrame), length),
          input_(std::move(input)), files_(std::move(files)), samples_(4096 * channels()) {}

    // AudioToolbox retains the callback context. Its address must remain stable
    // for the lifetime of both AudioFile handles.
    static std::shared_ptr<AudioSource> create(std::unique_ptr<Input> input) {
        auto files = std::make_unique<AppleFiles>();
        AudioStreamBasicDescription source{}, client{};
        if (!openAppleDecoder(*files, *input, source, client)) return nullptr;
        SInt64 length = 0;
        UInt32 bytes = sizeof(length);
        if (ExtAudioFileGetProperty(files->extended, kExtAudioFileProperty_FileLengthFrames, &bytes, &length) != noErr ||
            length <= 0) return nullptr;
        return std::make_shared<AppleSource>(std::move(input), std::move(files), source, uint64_t(length));
    }

    size_t read(uint64_t first, int16_t* stereo, size_t count) override {
        if (first >= frames() || !count) return 0;
        if (first != cursor_ && ExtAudioFileSeek(files_->extended, SInt64(first)) != noErr) return 0;
        cursor_ = first;
        size_t done = 0;
        while (done < count && cursor_ < frames()) {
            UInt32 take = UInt32(std::min<uint64_t>({count - done, frames() - cursor_, 4096}));
            AudioBufferList buffers{};
            buffers.mNumberBuffers = 1;
            buffers.mBuffers[0] = {UInt32(channels()), UInt32(take * channels() * sizeof(float)), samples_.data()};
            if (ExtAudioFileRead(files_->extended, &take, &buffers) != noErr || !take) break;
            for (UInt32 i = 0; i < take; ++i) {
                stereo[2 * (done + i)] = quantize(samples_[i * channels()]);
                stereo[2 * (done + i) + 1] = quantize(samples_[i * channels() + (channels() > 1)]);
            }
            done += take;
            cursor_ += take;
        }
        return done;
    }

private:
    std::unique_ptr<Input> input_;
    std::unique_ptr<AppleFiles> files_;
    std::vector<float> samples_;
    uint64_t cursor_ = 0;
};
#elif defined(HVM_HAVE_FFMPEG)
class FfmpegSource final : public AudioSource {
public:
    FfmpegSource(std::unique_ptr<Input> input, std::unique_ptr<Decoder> decoder,
                 int stream, int64_t length)
        : AudioSource(decoder->codec->sample_rate, decoder->codec->ch_layout.nb_channels, uint64_t(length)),
          input_(std::move(input)), decoder_(std::move(decoder)), stream_(stream),
          timeBase_(decoder_->format->streams[stream]->time_base),
          origin_(decoder_->format->streams[stream]->start_time == AV_NOPTS_VALUE ? 0 : decoder_->format->streams[stream]->start_time) {}

    static std::shared_ptr<AudioSource> create(std::unique_ptr<Input> input) {
        auto decoder = std::make_unique<Decoder>();
        int stream;
        if (!openDecoder(*decoder, *input, stream)) return nullptr;
        const auto* info = decoder->format->streams[stream];
        if (info->duration <= 0) return nullptr;
        const auto length = av_rescale_q(info->duration, info->time_base, AVRational{1, decoder->codec->sample_rate});
        if (length <= 0 || length > std::numeric_limits<int32_t>::max()) return nullptr;
        return std::make_shared<FfmpegSource>(std::move(input), std::move(decoder), stream, length);
    }

    size_t read(uint64_t first, int16_t* stereo, size_t count) override {
        if (first >= frames() || !count) return 0;
        if (first != cursor_ && !seek(first)) return 0;
        cursor_ = first;
        count = std::min<uint64_t>(count, frames() - first);
        size_t done = 0;
        while (done < count) {
            while (int64_t(cursor_) >= frameStart_ + decoder_->frame->nb_samples)
                if (!nextFrame()) return done;
            if (int64_t(cursor_) < frameStart_) return done;
            const int offset = int(int64_t(cursor_) - frameStart_);
            const size_t take = std::min(count - done, size_t(decoder_->frame->nb_samples - offset));
            for (size_t i = 0; i < take; ++i) {
                stereo[2 * (done + i)] = frameSample(*decoder_->frame, offset + int(i), 0);
                stereo[2 * (done + i) + 1] = frameSample(*decoder_->frame, offset + int(i), channels() > 1);
            }
            done += take;
            cursor_ += take;
        }
        return done;
    }

private:
    bool seek(uint64_t first) {
        // Decode some packets before the desired frame so AAC filter history
        // is available. The demuxer's sample timestamps locate the exact frame.
        const int64_t preroll = std::max<int64_t>(0, int64_t(first) - sampleRate() / 4);
        const int64_t target = origin_ + av_rescale_q(preroll, AVRational{1, sampleRate()}, timeBase_);
        if (av_seek_frame(decoder_->format, stream_, target, AVSEEK_FLAG_BACKWARD) < 0) return false;
        avcodec_flush_buffers(decoder_->codec);
        av_frame_unref(decoder_->frame);
        frameStart_ = 0;
        flushing_ = false;
        return true;
    }

    bool nextFrame() {
        const int64_t nextStart = frameStart_ + decoder_->frame->nb_samples;
        av_frame_unref(decoder_->frame);
        for (;;) {
            int result = avcodec_receive_frame(decoder_->codec, decoder_->frame);
            if (result == 0) {
                const auto time = decoder_->frame->best_effort_timestamp;
                frameStart_ = time == AV_NOPTS_VALUE ? nextStart :
                    av_rescale_q(time - origin_, timeBase_, AVRational{1, sampleRate()});
                return validFrame(*decoder_->frame, sampleRate(), channels());
            }
            if (result != AVERROR(EAGAIN) || flushing_) return false;
            do {
                av_packet_unref(decoder_->packet);
                result = av_read_frame(decoder_->format, decoder_->packet);
            } while (result >= 0 && decoder_->packet->stream_index != stream_);
            if (result == AVERROR_EOF) flushing_ = true;
            else if (result < 0) return false;
            result = avcodec_send_packet(decoder_->codec, flushing_ ? nullptr : decoder_->packet);
            av_packet_unref(decoder_->packet);
            if (result < 0) return false;
        }
    }

    std::unique_ptr<Input> input_;
    std::unique_ptr<Decoder> decoder_;
    int stream_;
    AVRational timeBase_;
    int64_t origin_, frameStart_ = 0;
    uint64_t cursor_ = 0;
    bool flushing_ = false;
};
#endif
}

std::shared_ptr<AudioSource> openHostStream(const uint8_t* data, size_t size, std::shared_ptr<const void> owner) {
    if (!data || !hostContainer(Input(data, size))) return nullptr;
#if defined(__APPLE__)
    return AppleSource::create(std::make_unique<Input>(data, size, std::move(owner)));
#elif defined(HVM_HAVE_FFMPEG)
    return FfmpegSource::create(std::make_unique<Input>(data, size, std::move(owner)));
#else
    return nullptr;
#endif
}

std::shared_ptr<AudioSource> openHostStream(int fd, size_t size) {
#if defined(__APPLE__) || defined(HVM_HAVE_FFMPEG)
    auto input = std::make_unique<Input>(fd, size);
    if (input->file < 0 || !hostContainer(*input)) return nullptr;
#if defined(__APPLE__)
    return AppleSource::create(std::move(input));
#else
    return FfmpegSource::create(std::move(input));
#endif
#else
    return nullptr;
#endif
}

bool decodeHostContainer(DecodedAudio& out, const uint8_t* data, size_t size) {
    if (!data || size < 12 || size > 256u * 1024 * 1024 || !hostContainer(Input(data, size))) return false;
    DecodedAudio decoded;
#if defined(__APPLE__)
    Input input{data, size};
    AppleFiles files;
    AudioStreamBasicDescription source{}, client{};
    if (!openAppleDecoder(files, input, source, client)) return false;
    decoded.sampleRate = int(source.mSampleRate);
    decoded.channels = int(source.mChannelsPerFrame);
    // Size the output once: growing a vector of a long song by doubling needs
    // the old and new block at the same time, which the phone may not have.
    SInt64 lengthFrames = 0;
    UInt32 bytes = sizeof(lengthFrames);
    if (ExtAudioFileGetProperty(files.extended, kExtAudioFileProperty_FileLengthFrames, &bytes, &lengthFrames) == noErr &&
        lengthFrames > 0 && uint64_t(lengthFrames) * 2 <= kMaxSamples) {
        try {
            decoded.stereoPcm.reserve(size_t(lengthFrames) * 2 + 2 * 4096);   // one read's worth of slack
        } catch (const std::bad_alloc&) {
            // No single block that large: grow as the reads arrive instead.
        }
    }
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
    int stream;
    if (!openDecoder(decoder, input, stream)) return false;
    decoded.sampleRate = decoder.codec->sample_rate;
    decoded.channels = decoder.codec->ch_layout.nb_channels;
    auto drain = [&]() {
        for (;;) {
            const int result = avcodec_receive_frame(decoder.codec, decoder.frame);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return true;
            if (result < 0) return false;
            auto* frame = decoder.frame;
            if (!validFrame(*frame, decoded.sampleRate, decoded.channels) ||
                size_t(frame->nb_samples) * 2 > kMaxSamples - decoded.stereoPcm.size()) return false;
            for (int i = 0; i < frame->nb_samples; ++i) {
                decoded.stereoPcm.push_back(frameSample(*frame, i, 0));
                decoded.stereoPcm.push_back(frameSample(*frame, i, decoded.channels > 1 ? 1 : 0));
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
#elif defined(__linux__) && !defined(__ANDROID__)
    // Desktop bench without FFmpeg headers: the ffmpeg program, when installed, decodes to
    // 44.1 kHz stereo 16-bit. Lets the bench play AAC like a phone does (a rhythm game that
    // times itself to its music otherwise waits on silence).
    static const bool haveCli = std::system("command -v ffmpeg >/dev/null 2>&1") == 0;
    if (!haveCli) return false;
    char path[] = "/tmp/hvm-audio-XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0) return false;
    const bool wrote = ::write(fd, data, size) == ssize_t(size);
    ::close(fd);
    if (wrote) {
        const std::string cmd = std::string("ffmpeg -v quiet -i '") + path + "' -f s16le -ac 2 -ar 44100 - 2>/dev/null";
        if (std::FILE* pipe = ::popen(cmd.c_str(), "r")) {
            int16_t buf[8192];
            size_t n;
            while ((n = std::fread(buf, sizeof(int16_t), 8192, pipe)) > 0 && decoded.stereoPcm.size() < kMaxSamples)
                decoded.stereoPcm.insert(decoded.stereoPcm.end(), buf, buf + n);
            ::pclose(pipe);
        }
    }
    ::unlink(path);
    decoded.sampleRate = 44100;
    decoded.channels = 2;
    if (decoded.stereoPcm.size() % 2) decoded.stereoPcm.pop_back();
#else
    return false;
#endif
    if (decoded.stereoPcm.empty()) return false;
    decoded.duration = double(decoded.stereoPcm.size() / 2) / decoded.sampleRate;
    out = std::move(decoded);
    return true;
}
} // namespace Hydrogen::Runtime::Audio
