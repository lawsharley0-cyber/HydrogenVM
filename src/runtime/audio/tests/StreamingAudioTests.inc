// Focused host-driven tests of the guest AVAudioPlayer and ExtAudioFile APIs.
// The companion shell script generates the audio files outside the worktree.
#include "runtime/Abi.hpp"
#include "runtime/audio/Audio.hpp"
#include "runtime/uikit/UIKit.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>
#include <sys/resource.h>

#define mp3dec_init oracle_mp3dec_init
#define mp3dec_decode_frame oracle_mp3dec_decode_frame
#define MINIMP3_IMPLEMENTATION
#include "thirdparty/minimp3/minimp3.h"

using namespace Hydrogen;
using namespace Hydrogen::Runtime;

namespace {
constexpr int kRate = 44100;
constexpr int kDuration = 240;
constexpr int kProbeFrames = 2048;
constexpr uint64_t kSeekFrame = 121 * kRate + 217;

bool expect(bool condition, const char* message) {
    std::printf("%s %s\n", condition ? "ok" : "FAIL", message);
    return condition;
}

struct PlayerFixture {
    explicit PlayerFixture(const std::string& home)
        : process(config(home)), cpu(process.mainThread().cpu), context{process, cpu, process.mem()},
          objc(ObjC::rt(process)) {
        process.mem().map(0xBF000000u - (1u << 20), 1u << 20, Memory::kProtRW);
        cpu.r[CPU::kSP] = 0xBF000000u - 256;
    }

    static ProcessConfig config(const std::string& home) {
        ProcessConfig cfg;
        cfg.homePath = home;
        cfg.bundlePath = home;
        cfg.phoneMode = true;
        return cfg;
    }

    uint32_t call(const char* name, std::initializer_list<uint32_t> args) {
        return uint32_t(process.callGuest(cpu, *process.lookupSymbol(name), args));
    }

    Process process;
    CPU::Cpu& cpu;
    Ctx context;
    ObjC::Runtime& objc;
};

#if defined(__linux__)
long peakKiB() {
    rusage usage{};
    return ::getrusage(RUSAGE_SELF, &usage) ? -1 : usage.ru_maxrss;
}
#endif

bool hostPlayer(const std::string& path, const std::string& home) {
    PlayerFixture f(home);
    auto url = ObjC::id(f.objc.send(f.cpu, f.objc.getClass("NSURL"), "fileURLWithPath:",
                                    {Foundation::makeString(f.context, path)}));
    auto player = ObjC::id(f.objc.send(f.cpu, f.objc.getClass("AVAudioPlayer"), "alloc"));
    std::array<float, kProbeFrames> left{}, right{};
#if defined(__linux__)
    // Linux resets the process high-water mark to current RSS when clear_refs
    // is written with 5. The peak below includes opening, seeking and playback.
    { std::ofstream reset("/proc/self/clear_refs"); reset << 5; }
    const auto before = peakKiB();
#endif
    player = ObjC::id(f.objc.send(f.cpu, player, "initWithContentsOfURL:error:", {url, 0}));
    const auto duration = std::bit_cast<double>(f.objc.send(f.cpu, player, "duration"));
    const bool prepared = f.objc.send(f.cpu, player, "prepareToPlay") & 255;
    bool ok = expect(prepared && std::abs(duration - kDuration) < 0.1, "AVAudioPlayer prepares the four-minute stereo fixture");
    const double target = double(kSeekFrame) / kRate;
    const auto bits = std::bit_cast<uint64_t>(target);
    f.objc.send(f.cpu, player, "setCurrentTime:", {uint32_t(bits), uint32_t(bits >> 32)});
    const bool played = f.objc.send(f.cpu, player, "play") & 255;
    f.process.mixer().render(left.data(), right.data(), left.size());
    const auto time = std::bit_cast<double>(f.objc.send(f.cpu, player, "currentTime"));
    ok &= expect(played && std::abs(time - (target + double(kProbeFrames) / kRate)) < 1.0 / kRate,
                 "AVAudioPlayer advances from the requested frame");
    // Source fixture: amplitude-0.16 chirp f(t)=311+0.25t in the left channel.
    // A local normalized dot product verifies the seek phase and waveform near
    // 121 s using the source signal rather than a sample from another HVM run.
    double dot = 0, reference = 0, observed = 0;
    for (size_t i = 0; i < left.size(); ++i) {
        const double t = double(kSeekFrame + i) / kRate;
        const double sample = 0.16 * std::sin(2 * std::acos(-1.0) * (311 * t + 0.125 * t * t));
        dot += sample * left[i];
        reference += sample * sample;
        observed += double(left[i]) * left[i];
    }
    if (!path.ends_with(".mp3"))
        ok &= expect(dot > 0.97 * std::sqrt(reference * observed) && observed > 10.0,
                     "decoded seek waveform matches the source chirp");
    else
        ok &= expect(observed > 10.0, "seek produces audible MP3 samples");

    f.objc.send(f.cpu, player, "setCurrentTime:", {0, 0});
    for (size_t pos = 0; pos <= size_t(duration * kRate); pos += left.size())
        f.process.mixer().render(left.data(), right.data(), left.size());
#if defined(__linux__)
    const auto extra = peakKiB() - before;
    std::printf("AVAudioPlayer peak extra RSS: %ld KiB\n", extra);
    ok &= expect(before >= 0 && extra >= 0 && extra < 4096, "peak extra RSS below 4 MiB");
#endif
    f.objc.release(f.cpu, player);

    const auto extOut = f.process.heap().allocZeroed(64);
    const auto pcm = f.process.heap().allocZeroed(32 * 4);
    const bool opened = f.call("_ExtAudioFileOpenURL", {url, extOut}) == 0;
    const uint32_t ext = f.process.mem().read<uint32_t>(extOut);
    const bool seeked = f.call("_ExtAudioFileSeek", {ext, uint32_t(kSeekFrame), 0}) == 0;
    const bool told = f.call("_ExtAudioFileTell", {ext, extOut + 8}) == 0;
    ok &= expect(opened && seeked && told &&
                 f.process.mem().read<uint64_t>(extOut + 8) == kSeekFrame, "ExtAudioFile opens and seeks by frame");
    f.process.mem().write<uint32_t>(extOut + 16, 32);
    f.process.mem().write<uint32_t>(extOut + 24, 1);
    f.process.mem().write<uint32_t>(extOut + 28, 2);
    f.process.mem().write<uint32_t>(extOut + 32, 32 * 4);
    f.process.mem().write<uint32_t>(extOut + 36, pcm);
    const bool read = f.call("_ExtAudioFileRead", {ext, extOut + 16, extOut + 24}) == 0;
    bool audible = false;
    for (int i = 0; i < 32; ++i) audible |= std::abs(f.process.mem().read<int16_t>(pcm + 4 * i)) > 500;
    ok &= expect(read && f.process.mem().read<uint32_t>(extOut + 16) == 32 && audible,
                 "ExtAudioFile reads audible PCM after seeking");
    f.call("_ExtAudioFileDispose", {ext});
    return ok;
}

bool compareMp3(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(stream)), {});
    auto source = Audio::openStream(path);
    if (!source || source->sampleRate() != kRate || source->channels() != 2) return false;

    // Decode the entire input with the upstream minimp3 API. FFmpeg writes
    // this fixture with no ID3 or Xing header, so locator policy is independent.
    mp3dec_t decoder{};
    oracle_mp3dec_init(&decoder);
    std::array<int16_t, MINIMP3_MAX_SAMPLES_PER_FRAME> frame{};
    std::vector<int16_t> full;
    for (size_t off = 0; off < bytes.size();) {
        mp3dec_frame_info_t info{};
        const auto produced = oracle_mp3dec_decode_frame(&decoder, bytes.data() + off, int(bytes.size() - off), frame.data(), &info);
        if (!info.frame_bytes || produced <= 0 || info.channels != 2) return false;
        full.insert(full.end(), frame.begin(), frame.begin() + produced * 2);
        off += info.frame_bytes;
    }
    bool same = source->frames() * 2 == full.size();
    std::array<int16_t, 4096 * 2> window{};
    for (uint64_t position = 0; same && position < source->frames();) {
        const size_t expected = std::min<uint64_t>(4096, source->frames() - position);
        const auto got = source->read(position, window.data(), 4096);
        same = got == expected && std::equal(window.data(), window.data() + got * 2, full.data() + position * 2);
        position += got;
    }
    bool ok = expect(same, "streamed MP3 PCM matches a full upstream minimp3 decode bit for bit");
    const auto got = source->read(kSeekFrame, window.data(), 4096);
    ok &= expect(got == 4096 && std::equal(window.data(), window.data() + got * 2, full.data() + kSeekFrame * 2),
                 "MP3 seek is sample-exact after reservoir preroll");
    return ok;
}
bool compareOggSeek(const std::string& path, const std::string& oracle) {
    auto source = Audio::openStream(path);
    if (!source) return false;
    std::ifstream reference(oracle, std::ios::binary);
    std::array<int16_t, 4096 * 2> expected{}, actual{};
    reference.seekg(kSeekFrame * 2 * sizeof(int16_t));
    if (!reference.read(reinterpret_cast<char*>(expected.data()), sizeof expected)) return false;
    if (source->read(kSeekFrame, actual.data(), actual.size() / 2) != actual.size() / 2) return false;
    int maxDifference = 0;
    for (size_t i = 0; i < actual.size(); ++i)
        maxDifference = std::max(maxDifference, std::abs(int(actual[i]) - int(expected[i])));
    std::printf("Ogg seek max PCM difference versus ffmpeg: %d counts\n", maxDifference);
    return expect(maxDifference <= 4, "Ogg seek agrees with the independently decoded frame");
}

}

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) {
        std::fprintf(stderr, "Usage: %s player <track> <home> | mp3-reference <track> | ogg-reference <track> <pcm>\n", argv[0]);
        return 2;
    }
    if (argc == 4 && std::strcmp(argv[1], "player") == 0)
        return hostPlayer(std::filesystem::absolute(argv[2]).string(), argv[3]) ? 0 : 1;
    if (argc == 4 && std::strcmp(argv[1], "ogg-reference") == 0)
        return compareOggSeek(argv[2], argv[3]) ? 0 : 1;
    if (argc == 3 && std::strcmp(argv[1], "mp3-reference") == 0)
        return compareMp3(argv[2]) ? 0 : 1;
    return 2;
}
