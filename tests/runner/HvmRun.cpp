// `hvm run <bundle.app | file.ipa> [--home DIR] [--keep-going] [--trace] [--quiet]
//          [--run-for SECONDS] [--screenshot FILE.png] [--tap X,Y@T] [--drag X1,Y1,X2,Y2@T] [-- args...]`
#include "loader/AppBundle.hpp"
#include "runtime/Process.hpp"
#include "runtime/graphics/Graphics.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <algorithm>
#include <atomic>

using namespace Hydrogen;
namespace fs = std::filesystem;

int hvmRunMain(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: hvm run <app|ipa> [--home DIR] [--keep-going] [--trace] [-- args]\n"); return 2; }
    std::string target = argv[1], home;
    Runtime::ProcessConfig cfg;
    bool quiet = false;
    double runFor = 0;
    std::string screenshot;
    struct Gesture { float x1, y1, x2, y2; double at; bool drag; };
    std::vector<Gesture> gestures;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--run-for") && i + 1 < argc) { runFor = std::atof(argv[++i]); continue; }
        if (!std::strcmp(argv[i], "--screenshot") && i + 1 < argc) { screenshot = argv[++i]; continue; }
        if (!std::strcmp(argv[i], "--tap") && i + 1 < argc) {
            Gesture g{};
            if (std::sscanf(argv[++i], "%f,%f@%lf", &g.x1, &g.y1, &g.at) == 3) { g.x2 = g.x1; g.y2 = g.y1; gestures.push_back(g); }
            continue;
        }
        if (!std::strcmp(argv[i], "--drag") && i + 1 < argc) {
            Gesture g{};
            g.drag = true;
            if (std::sscanf(argv[++i], "%f,%f,%f,%f@%lf", &g.x1, &g.y1, &g.x2, &g.y2, &g.at) == 5) gestures.push_back(g);
            continue;
        }
        if (!std::strcmp(argv[i], "--home") && i + 1 < argc) home = argv[++i];
        else if (!std::strcmp(argv[i], "--keep-going")) cfg.stopOnMissingFunction = false;
        else if (!std::strcmp(argv[i], "--trace")) cfg.traceCalls = true;
        else if (!std::strcmp(argv[i], "--quiet")) quiet = true;
        else if (!std::strcmp(argv[i], "--")) { for (++i; i < argc; ++i) cfg.extraArgs.push_back(argv[i]); }
    }
    if (target.size() > 4 && target.compare(target.size() - 4, 4, ".ipa") == 0) {
        const std::string root = home.empty() ? (fs::temp_directory_path() / "hvm-apps").string() : home;
        auto info = Loader::importIpa(target, root);
        if (!info.runnable) { std::fprintf(stderr, "can't run: %s\n", info.problem.c_str()); return 3; }
        target = info.bundlePath;
        home.clear();
    }
    cfg.bundlePath = fs::absolute(target).lexically_normal().string();
    if (!cfg.bundlePath.empty() && cfg.bundlePath.back() == '/') cfg.bundlePath.pop_back();
    cfg.homePath = home;
    cfg.log = [quiet](Runtime::LogLevel lvl, const std::string& msg) {
        switch (lvl) {
            case Runtime::LogLevel::GuestStdout: std::fwrite(msg.data(), 1, msg.size(), stdout); std::fflush(stdout); break;
            case Runtime::LogLevel::GuestStderr: std::fwrite(msg.data(), 1, msg.size(), stderr); break;
            case Runtime::LogLevel::Error: std::fprintf(stderr, "[hvm:error] %s\n", msg.c_str()); break;
            case Runtime::LogLevel::Warning: if (!quiet) std::fprintf(stderr, "[hvm:warn] %s\n", msg.c_str()); break;
            default: if (!quiet) std::fprintf(stderr, "[hvm] %s\n", msg.c_str()); break;
        }
    };
    // Keep the last presented frame for --screenshot.
    std::mutex frameMu;
    Graphics::Image lastFrame;
    if (!screenshot.empty()) {
        cfg.present = [&](const uint8_t* rgba, int w, int h) {
            std::lock_guard<std::mutex> lk(frameMu);
            lastFrame.width = w; lastFrame.height = h;
            lastFrame.rgba.assign(rgba, rgba + size_t(w) * size_t(h) * 4);
        };
    }
    Runtime::Process proc(cfg);
    proc.load();
    // Scripted input and the run-time limit run on a helper thread.
    std::atomic<bool> done{false};
    std::thread driver([&] {
        using namespace std::chrono;
        const auto start = steady_clock::now();
        auto waitUntil = [&](double t) {
            while (!done && duration<double>(steady_clock::now() - start).count() < t) std::this_thread::sleep_for(milliseconds(5));
        };
        std::vector<Gesture> gs = gestures;
        std::sort(gs.begin(), gs.end(), [](const Gesture& a, const Gesture& b) { return a.at < b.at; });
        for (const Gesture& g : gs) {
            waitUntil(g.at);
            if (done) return;
            proc.injectTouch(Runtime::TouchPhase::Began, g.x1, g.y1, 1);
            if (g.drag) {
                for (int k = 1; k <= 10; ++k) {
                    std::this_thread::sleep_for(milliseconds(16));
                    proc.injectTouch(Runtime::TouchPhase::Moved, g.x1 + (g.x2 - g.x1) * float(k) / 10, g.y1 + (g.y2 - g.y1) * float(k) / 10, 1);
                }
            } else std::this_thread::sleep_for(milliseconds(60));
            proc.injectTouch(Runtime::TouchPhase::Ended, g.x2, g.y2, 1);
        }
        if (runFor > 0) { waitUntil(runFor); if (!done) proc.requestStop(); }
    });
    const int r = proc.run();
    done = true;
    driver.join();
    if (!screenshot.empty()) {
        std::lock_guard<std::mutex> lk(frameMu);
        if (lastFrame.width > 0) {
            const auto png = Graphics::encodePng(lastFrame);
            std::ofstream(screenshot, std::ios::binary).write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
            if (!quiet) std::fprintf(stderr, "[hvm] screenshot: %s (%dx%d)\n", screenshot.c_str(), lastFrame.width, lastFrame.height);
        } else std::fprintf(stderr, "[hvm] no frame was presented; no screenshot\n");
    }
    if (proc.crashed()) return 70;
    return r;
}
