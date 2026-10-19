// `hvm run <bundle.app | file.ipa> [--home DIR] [--keep-going] [--trace] [-- args...]`
#include "loader/AppBundle.hpp"
#include "runtime/Process.hpp"
#include <cstdio>
#include <cstring>
#include <filesystem>

using namespace Hydrogen;
namespace fs = std::filesystem;

int hvmRunMain(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: hvm run <app|ipa> [--home DIR] [--keep-going] [--trace] [-- args]\n"); return 2; }
    std::string target = argv[1], home;
    Runtime::ProcessConfig cfg;
    bool quiet = false;
    for (int i = 2; i < argc; ++i) {
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
    Runtime::Process proc(cfg);
    proc.load();
    const int r = proc.run();
    if (proc.crashed()) return 70;
    return r;
}
