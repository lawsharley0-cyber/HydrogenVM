// hvm — command-line front end used by the test suite (and handy for
// debugging on a desktop): import .ipa files, inspect bundles, run apps.
#include <iostream>
#include <iomanip>
#include <sstream>
#include <chrono>
#include <map>
#include <set>
#include <vector>
#include <string>
#include <memory>
#include <functional>
#include <algorithm>
#include <cxxabi.h>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <shared_mutex>
#include "loader/AppBundle.hpp"
#define private public
#include "runtime/Process.hpp"
#include "runtime/objc/ObjC.hpp"
#undef private
#include "runtime/Support.hpp"

using namespace Hydrogen;

static void printInfo(const Loader::AppInfo& a) {
    std::printf("bundle: %s\nexecutable: %s\nid: %s\nname: %s\ndisplay: %s\nversion: %s\nminOS: %s\nsubtype: %u\nencrypted: %d\nrunnable: %d\n",
                a.bundlePath.c_str(), a.executablePath.c_str(), a.bundleId.c_str(), a.name.c_str(), a.displayName.c_str(),
                a.version.c_str(), a.minimumOS.c_str(), a.cpuSubtype, a.encrypted, a.runnable);
    for (auto& i : a.iconPaths) std::printf("icon: %s\n", i.c_str());
    if (!a.problem.empty()) std::printf("problem: %s\n", a.problem.c_str());
}

static std::string quoted(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << char(c);
        else if (c < 32) out << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
        else out << char(c);
    }
    return out.str() + '"';
}

static std::string pretty(std::string name) {
    if (!name.empty() && name[0] == '_') name.erase(0, 1);
    int status = 0;
    char* demangled = abi::__cxa_demangle(name.c_str(), nullptr, nullptr, &status);
    if (demangled && !status) name = demangled;
    std::free(demangled);
    return name;
}

static void dumpSupport() {
    Runtime::ProcessConfig cfg;
    cfg.bundlePath = "/tmp/dummy.app";
    cfg.homePath = "/tmp/dummy_home";
    cfg.log = [](Runtime::LogLevel, const std::string&) {};
    Runtime::Process process(cfg);
    auto& objc = Runtime::ObjC::rt(process);
    auto& supportReg = Runtime::supportRegistry(process);

    std::cout << "{\"$schema\":\"https://hydrogenvm.local/schemas/support-manifest-v1.json\",\"version\":1";

    // Symbols
    std::cout << ",\"symbols\":[";
    bool first = true;
    for (const auto& [name, addr] : process.symbols_) {
        const bool isFn = process.engine().isHleAddress(addr);
        const auto rec = supportReg.find(name);
        std::string status = rec ? Runtime::supportStatusName(rec->status) : "Real";
        std::string provenance = rec ? rec->provenance : "HydrogenVM";
        std::cout << (first ? "" : ",") << "{\"name\":" << quoted(name)
                  << ",\"kind\":" << quoted(isFn ? "function" : "data")
                  << ",\"status\":" << quoted(status)
                  << ",\"provenance\":" << quoted(provenance) << "}";
        first = false;
    }
    std::cout << "]";

    // Classes (only non-metaclasses)
    std::cout << ",\"classes\":[";
    first = true;
    std::set<std::string> allSelectors;
    for (const auto& [cls, infoPtr] : objc.classes_) {
        const auto* info = infoPtr.get();
        if (!info || info->isMeta) continue;
        const std::string status = info->stub ? "Stub" : "Real";
        const std::string provenance = "HydrogenVM";
        std::cout << (first ? "" : ",") << "{\"name\":" << quoted(info->name)
                  << ",\"status\":" << quoted(status)
                  << ",\"provenance\":" << quoted(provenance)
                  << ",\"methods\":[";
        bool mFirst = true;
        for (const auto& [sel, imp] : info->methods) {
            std::string selName = objc.selName(sel);
            if (!info->stub) allSelectors.insert(selName);
            std::cout << (mFirst ? "" : ",") << quoted(selName);
            mFirst = false;
        }
        // Also collect class methods on meta
        if (info->meta) {
            const auto* metaInfo = objc.info(info->meta);
            if (metaInfo) {
                for (const auto& [sel, imp] : metaInfo->methods) {
                    std::string selName = objc.selName(sel);
                    if (!info->stub) allSelectors.insert(selName);
                    std::cout << (mFirst ? "" : ",") << quoted(selName);
                    mFirst = false;
                }
            }
        }
        std::cout << "]}";
        first = false;
    }
    std::cout << "]";

    // Selectors
    std::cout << ",\"selectors\":[";
    first = true;
    for (const auto& selName : allSelectors) {
        std::cout << (first ? "" : ",") << "{\"name\":" << quoted(selName)
                  << ",\"status\":\"Real\",\"provenance\":\"HydrogenVM\"}";
        first = false;
    }
    std::cout << "]";

    // Flat entries array
    std::cout << ",\"entries\":[";
    first = true;
    for (const auto& [name, addr] : process.symbols_) {
        const auto rec = supportReg.find(name);
        std::string status = rec ? Runtime::supportStatusName(rec->status) : "Real";
        std::string provenance = rec ? rec->provenance : "HydrogenVM";
        std::cout << (first ? "" : ",") << "{\"kind\":\"symbol\",\"name\":" << quoted(name)
                  << ",\"status\":" << quoted(status)
                  << ",\"provenance\":" << quoted(provenance) << "}";
        first = false;
    }
    for (const auto& [cls, infoPtr] : objc.classes_) {
        const auto* info = infoPtr.get();
        if (!info || info->isMeta) continue;
        std::cout << (first ? "" : ",") << "{\"kind\":\"class\",\"name\":" << quoted(info->name)
                  << ",\"status\":" << quoted(info->stub ? "Stub" : "Real")
                  << ",\"provenance\":\"HydrogenVM\"}";
        first = false;
    }
    for (const auto& selName : allSelectors) {
        std::cout << (first ? "" : ",") << "{\"kind\":\"selector\",\"name\":" << quoted(selName)
                  << ",\"status\":\"Real\",\"provenance\":\"HydrogenVM\"}";
        first = false;
    }
    std::cout << "]";

    std::cout << "}\n";
}

static void linkingInfo(const char* path) {
    Runtime::ProcessConfig cfg;
    cfg.bundlePath = path;
    std::set<std::string> zeroData;
    cfg.log = [&](Runtime::LogLevel, const std::string& message) {
        const std::string prefix = "Missing data symbol ";
        const auto end = message.find(" (using zeros)");
        if (message.starts_with(prefix) && end != std::string::npos)
            zeroData.insert(message.substr(prefix.size(), end - prefix.size()));
    };
    Runtime::Process process(cfg);
    process.load(); // Binds and registers metadata; never runs +load or guest initializers.
    auto& objc = Runtime::ObjC::rt(process);
    const auto missing = process.unimplementedImports();
    std::set<std::pair<std::string, std::string>> seen;
    bool first = true;
    std::cout << "{\"selectorResolution\":\"candidate implementations only; the receiver is determined at runtime\",\"references\":[";
    auto emit = [&](const std::string& kind, const std::string& name, const std::string& binding) {
        if (!seen.emplace(kind, name).second) return;
        std::cout << (first ? "" : ",") << "{\"kind\":" << quoted(kind) << ",\"name\":" << quoted(name)
                  << ",\"binding\":" << quoted(binding) << "}";
        first = false;
    };
    auto imports = process.image().binds;
    imports.insert(imports.end(), process.image().weakBinds.begin(), process.image().weakBinds.end());
    for (const auto& bind : imports) {
        const auto target = process.mem().read<uint32_t>(bind.address);
        const auto symbol = process.lookupSymbol(bind.symbol).value_or(0);
        if (bind.symbol.starts_with("_OBJC_CLASS_$_") || bind.symbol.starts_with("_OBJC_METACLASS_$_")) {
            const auto at = bind.symbol.find("$_");
            const auto name = bind.symbol.substr(at + 2);
            const auto cls = objc.getClass(name);
            const auto info = cls ? objc.info(cls) : nullptr;
            emit("class", name, !target || !info ? "missing" : info->stub ? "placeholder class" : "implemented");
        } else {
            const auto name = pretty(bind.symbol);
            const bool stub = std::find(missing.begin(), missing.end(), name) != missing.end();
            bool function = bind.lazy || (symbol && process.engine().isHleAddress(symbol));
            for (const auto& segment : process.image().segments)
                function |= (segment.initprot & 4) && (target & ~1u) >= segment.vmaddr && (target & ~1u) - segment.vmaddr < segment.vmsize;
            emit(function ? "function" : "data", bind.symbol, !target ? "missing" : stub ? "stub" : zeroData.count(name) ? "zero-filled data" : "implemented");
        }
    }
    for (const auto& section : process.image().sections) {
        if (section.sectname != "__objc_selrefs" && section.sectname != "__objc_classrefs") continue;
        for (uint32_t offset = 0; offset + 4 <= section.size; offset += 4) {
            const auto value = process.mem().read<uint32_t>(section.addr + offset);
            if (section.sectname == "__objc_classrefs") {
                const auto info = value ? objc.info(value) : nullptr;
                if (info) emit("class", info->name, info->stub ? "placeholder class" : "implemented");
                continue;
            }
            bool provided = false;
            for (auto cls : objc.classList()) {
                const auto info = objc.info(cls);
                provided |= objc.lookup(cls, value) != 0 || (info && objc.lookup(info->meta, value) != 0);
            }
            emit("selector", objc.selName(value), provided ? "implemented" : "missing");
        }
    }
    std::cout << "]}\n";
}

int hvmRunMain(int argc, char** argv);   // HvmRun.cpp

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: hvm import <ipa> <apps-root> | info <app> | run <app> [options] | --dump-support\n");
        return 2;
    }
    try {
        if (!std::strcmp(argv[1], "--dump-support") || !std::strcmp(argv[1], "dump-support")) { dumpSupport(); return 0; }
        if (!std::strcmp(argv[1], "import") && argc >= 4) { printInfo(Loader::importIpa(argv[2], argv[3])); return 0; }
        if (!std::strcmp(argv[1], "info") && argc == 5 && !std::strcmp(argv[2], "--linking") && !std::strcmp(argv[4], "--json")) { linkingInfo(argv[3]); return 0; }
        if (!std::strcmp(argv[1], "info") && argc >= 3) { printInfo(Loader::readBundle(argv[2])); return 0; }
        if (!std::strcmp(argv[1], "run")) return hvmRunMain(argc - 1, argv + 1);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    std::fprintf(stderr, "unknown command\n");
    return 2;
}
