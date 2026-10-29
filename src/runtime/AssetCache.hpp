#pragma once
#include <mutex>
#include <string>
#include <unordered_map>
#include <sys/stat.h>
#include "Process.hpp"
#include <unistd.h>
#include <fcntl.h>

namespace Hydrogen::Runtime {

struct AssetCache {
    explicit AssetCache(Process&) {}
    ~AssetCache() {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& [path, entry] : entries) {
            if (entry.fd >= 0) ::close(entry.fd);
        }
        entries.clear();
    }
    std::mutex mu;
    struct Entry { int fd; struct stat st; };
    std::unordered_map<std::string, Entry> entries;
    bool isAsset(const std::string& path) {
        if (path.find("CookedIPhone") == std::string::npos) return false;
        if (path.size() < 4) return false;
        std::string ext = path.substr(path.size() - 4);
        return ext == ".tfc" || ext == ".xxx";
    }
};

inline AssetCache& getAssetCache(Process& p) {
    return p.service<AssetCache>();
}

} // namespace Hydrogen::Runtime
