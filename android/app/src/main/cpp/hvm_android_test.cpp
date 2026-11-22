// A command-line check of the Android bridge, run on a device or emulator through adb (not part of
// the app): imports a game, plays it for a while through the same session code the app uses, with
// scripted touches, and writes the last frame and the log. Usage:
//   hvm_android_test <game.ipa|.apk> <work dir> <seconds> [u,v@seconds ...]
#include "hvm_android.cpp"

#include <chrono>
#include <cstdio>
#include <sstream>

namespace Hydrogen::Runtime::Audio { bool decodeHostContainer(DecodedAudio&, const uint8_t*, size_t); }

int main(int argc, char** argv) {
    // hvm_android_test --decode <file>: decode an M4A/AAC file with the Android decoder and report it.
    if (argc == 3 && std::string(argv[1]) == "--decode") {
        std::ifstream in(argv[2], std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
        RT::Audio::DecodedAudio out;
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = RT::Audio::decodeHostContainer(out, bytes.data(), bytes.size());
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        double peak = 0;
        for (int16_t v : out.stereoPcm) peak = std::max(peak, std::abs(double(v)));
        std::printf("decode: ok=%d rate=%d channels=%d frames=%zu peak=%.0f (%.1f s)\n", int(ok), out.sampleRate, out.channels,
                    out.stereoPcm.size() / 2, peak, secs);
        return ok ? 0 : 1;
    }
    if (argc < 4) { std::fprintf(stderr, "usage: %s <game> <workdir> <seconds> [u,v@t ...]\n", argv[0]); return 2; }
    const std::string game = argv[1], work = argv[2];
    const double seconds = std::atof(argv[3]);
    fs::create_directories(work + "/apps");
    auto r = hvmandroid::importGame(game, work + "/apps");
    std::printf("import: bundle=%s title=%s id=%s problem=%s\n", r[0].c_str(), r[1].c_str(), r[2].c_str(), r[3].c_str());
    if (r[0].empty()) return 3;
    const std::string icon = fs::path(r[0]).parent_path().string() + "/hvm-icon.png";
    std::printf("icon: %s\n", hvmandroid::writeIcon(r[0], icon) ? icon.c_str() : "none");
    struct Tap { float u, v; double t; bool sent = false; };
    std::vector<Tap> taps;
    for (int i = 4; i < argc; ++i) {
        Tap t{};
        if (std::sscanf(argv[i], "%f,%f@%lf", &t.u, &t.v, &t.t) == 3) taps.push_back(t);
    }
    Session* s = hvmandroid::startSession(r[0], fs::path(r[0]).parent_path().string(), false, 1080, false, 0, work + "/game.log");
    const auto start = std::chrono::steady_clock::now();
    double last = -1;
    while (s->running) {
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (t >= seconds) break;
        for (auto& tap : taps)
            if (!tap.sent && t >= tap.t) {
                tap.sent = true;
                hvmandroid::touchSession(s, 0, tap.u, tap.v, 0);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                hvmandroid::touchSession(s, 3, tap.u, tap.v, 0);
                std::printf("tap %.2f,%.2f at %.1fs\n", tap.u, tap.v, t);
            }
        if (t - last >= 10) {
            last = t;
            std::lock_guard<std::mutex> lk(s->mu);
            std::printf("t=%.0fs frames=%llu frame=%dx%d orientation=%d\n", t, (unsigned long long)s->frames, s->frameW, s->frameH, s->orientation);
            std::fflush(stdout);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    {
        std::lock_guard<std::mutex> lk(s->mu);
        std::printf("end: running=%d frames=%llu exit='%s'\n", int(s->running.load()), (unsigned long long)s->frames, s->exitMessage.c_str());
        if (!s->frame.empty() && s->frameW > 0) {
            Hydrogen::Graphics::Image img;
            img.width = s->frameW; img.height = s->frameH;
            img.rgba = s->frame;
            const auto png = Hydrogen::Graphics::encodePng(img);
            std::ofstream(work + "/last-frame.png", std::ios::binary).write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
            std::printf("frame: %s/last-frame.png\n", work.c_str());
        }
    }
    std::fflush(stdout);
    hvmandroid::stopSession(s);
    std::printf("stopped\n");
    return 0;
}
