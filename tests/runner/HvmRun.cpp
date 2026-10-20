// `hvm run <bundle.app | file.ipa> [--home DIR] [--keep-going] [--trace] [--quiet]
//          [--run-for SECONDS] [--screenshot FILE.png] [--tap X,Y@T] [--drag X1,Y1,X2,Y2@T]
//          [--report FILE.json] [-- args...]`
// --report writes a machine-readable summary: exit status, crash report, every
// warning/error the runtime logged, missing functions, and performance counters.
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

namespace {
std::string jsonEscape(const std::string& in) {
    std::string o;
    o.reserve(in.size() + 8);
    for (unsigned char c : in) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += char(c);
        }
    }
    return o;
}
std::string jsonList(const std::vector<std::string>& v) {
    std::string o = "[";
    for (size_t i = 0; i < v.size(); ++i) o += (i ? ", \"" : "\"") + jsonEscape(v[i]) + "\"";
    return o + "]";
}
} // namespace

int hvmRunMain(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: hvm run <app|ipa> [--home DIR] [--keep-going] [--trace] [-- args]\n"); return 2; }
    std::string target = argv[1], home;
    Runtime::ProcessConfig cfg;
    bool quiet = false;
    double runFor = 0;
    bool timedStop = false;   // --run-for elapsed while the app was still running: not a failure
    std::string screenshot, reportPath;
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
        if (!std::strcmp(argv[i], "--report") && i + 1 < argc) { reportPath = argv[++i]; continue; }
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
    // Everything the runtime reports, for --report (deduplicated, in order, with counts).
    std::mutex logMu;
    std::vector<std::pair<std::string, int>> warnings, errors;
    auto note = [&logMu](std::vector<std::pair<std::string, int>>& v, const std::string& m) {
        std::lock_guard<std::mutex> lk(logMu);
        for (auto& e : v) if (e.first == m) { ++e.second; return; }
        if (v.size() < 2000) v.emplace_back(m, 1);
    };
    cfg.log = [quiet, &note, &warnings, &errors](Runtime::LogLevel lvl, const std::string& msg) {
        if (lvl == Runtime::LogLevel::Warning) note(warnings, msg);
        if (lvl == Runtime::LogLevel::Error) note(errors, msg);
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
    const auto wallStart = std::chrono::steady_clock::now();
    Runtime::Process proc(cfg);
    std::string loadError;
    auto writeReport = [&](int status) {
        if (reportPath.empty()) return;
        auto& pf = proc.perf();
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count();
        const uint64_t insns = proc.guestInstructions();
        const uint64_t frames = pf.frames.load();
        auto pairs = [](const std::vector<std::pair<std::string, int>>& v) {
            std::string o = "[";
            for (size_t i = 0; i < v.size(); ++i)
                o += std::string(i ? ", " : "") + "{\"message\": \"" + jsonEscape(v[i].first) + "\", \"count\": " + std::to_string(v[i].second) + "}";
            return o + "]";
        };
        std::lock_guard<std::mutex> lk(logMu);
        std::ofstream f(reportPath);
        f << "{\n";
        f << "  \"bundle\": \"" << jsonEscape(cfg.bundlePath) << "\",\n";
        f << "  \"exitStatus\": " << status << ",\n";
        f << "  \"stillRunning\": " << (timedStop ? "true" : "false") << ",\n";   // reached --run-for without failing
        f << "  \"crashed\": " << (proc.crashed() ? "true" : "false") << ",\n";
        f << "  \"crashReport\": \"" << jsonEscape(proc.crashReport()) << "\",\n";
        f << "  \"loadError\": \"" << jsonEscape(loadError) << "\",\n";
        f << "  \"missingFunctions\": " << jsonList(proc.missingSymbols()) << ",\n";
        f << "  \"unimplementedImports\": " << jsonList(proc.unimplementedImports()) << ",\n";
        f << "  \"warnings\": " << pairs(warnings) << ",\n";
        f << "  \"errors\": " << pairs(errors) << ",\n";
        f << "  \"perf\": {\n";
        f << "    \"wallSeconds\": " << wall << ",\n";
        f << "    \"guestInstructions\": " << insns << ",\n";
        f << "    \"guestMIPS\": " << (wall > 0 ? double(insns) / 1e6 / wall : 0) << ",\n";
        f << "    \"blocksDecoded\": " << proc.engine().stats().blocks_decoded << ",\n";
        f << "    \"frames\": " << frames << ",\n";
        f << "    \"framesPresented\": " << pf.framesPresented.load() << ",\n";
        f << "    \"avgFrameMs\": " << (frames ? double(pf.frameNs.load()) / 1e6 / double(frames) : 0) << ",\n";
        f << "    \"avgCompositeMs\": " << (pf.framesPresented ? double(pf.compositeNs.load()) / 1e6 / double(pf.framesPresented.load()) : 0) << ",\n";
        f << "    \"maxFrameMs\": " << double(pf.maxFrameNs.load()) / 1e6 << ",\n";
        f << "    \"slowFrames\": " << pf.slowFrames.load() << "\n";
        f << "  }\n}\n";
    };
    try {
        proc.load();
    } catch (const std::exception& e) {
        loadError = e.what();
        std::fprintf(stderr, "[hvm:error] load failed: %s\n", e.what());
        writeReport(71);
        return 71;
    }
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
        if (runFor > 0) { waitUntil(runFor); if (!done) { timedStop = true; proc.requestStop(); } }
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
    const int status = timedStop ? 0 : proc.crashed() ? 70 : r;
    if (timedStop && !quiet) std::fprintf(stderr, "[hvm] still running after %.0f s (stopped by --run-for)\n", runFor);
    writeReport(status);
    return status;
}
