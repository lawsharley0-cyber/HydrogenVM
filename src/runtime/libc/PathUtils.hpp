#pragma once
#include <filesystem>
#include <string>
#include <cctype>

namespace Hydrogen::Runtime {

inline std::string resolveCasePath(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (path.empty() || fs::exists(path, ec)) return path;

    fs::path p(path);
    fs::path resolved = p.is_absolute() ? "/" : "";

    for (const auto& component : p) {
        std::string compStr = component.string();
        if (compStr == "/" || compStr == ".") continue;
        if (compStr == "..") {
            resolved = resolved.parent_path();
            continue;
        }

        fs::path candidate = resolved.empty() ? component : (resolved / component);
        if (fs::exists(candidate, ec)) {
            resolved = candidate;
            continue;
        }

        fs::path parent = resolved.empty() ? "." : resolved;
        if (!fs::is_directory(parent, ec)) {
            return path;
        }

        std::string compLower = compStr;
        for (char& c : compLower) c = char(std::tolower(uint8_t(c)));

        bool found = false;
        for (const auto& entry : fs::directory_iterator(parent, ec)) {
            std::string entryName = entry.path().filename().string();
            std::string entryLower = entryName;
            for (char& c : entryLower) c = char(std::tolower(uint8_t(c)));
            if (compLower == entryLower) {
                resolved = resolved.empty() ? entry.path().filename() : (resolved / entry.path().filename());
                found = true;
                break;
            }
        }
        if (!found) {
            return path;
        }
    }
    return resolved.string();
}

} // namespace Hydrogen::Runtime
