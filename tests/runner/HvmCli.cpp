// hvm — command-line front end used by the test suite (and handy for
// debugging on a desktop): import .ipa files, inspect bundles, run apps.
#include "loader/AppBundle.hpp"
#include <cstdio>
#include <cstring>
#include <string>

using namespace Hydrogen;

static void printInfo(const Loader::AppInfo& a) {
    std::printf("bundle: %s\nexecutable: %s\nid: %s\nname: %s\ndisplay: %s\nversion: %s\nminOS: %s\nsubtype: %u\nencrypted: %d\nrunnable: %d\n",
                a.bundlePath.c_str(), a.executablePath.c_str(), a.bundleId.c_str(), a.name.c_str(), a.displayName.c_str(),
                a.version.c_str(), a.minimumOS.c_str(), a.cpuSubtype, a.encrypted, a.runnable);
    for (auto& i : a.iconPaths) std::printf("icon: %s\n", i.c_str());
    if (!a.problem.empty()) std::printf("problem: %s\n", a.problem.c_str());
}

int hvmRunMain(int argc, char** argv);   // HvmRun.cpp

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: hvm import <ipa> <apps-root> | info <app> | run <app> [options]\n");
        return 2;
    }
    try {
        if (!std::strcmp(argv[1], "import") && argc >= 4) { printInfo(Loader::importIpa(argv[2], argv[3])); return 0; }
        if (!std::strcmp(argv[1], "info") && argc >= 3) { printInfo(Loader::readBundle(argv[2])); return 0; }
        if (!std::strcmp(argv[1], "run")) return hvmRunMain(argc - 1, argv + 1);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    std::fprintf(stderr, "unknown command\n");
    return 2;
}
