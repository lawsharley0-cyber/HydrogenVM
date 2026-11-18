#pragma once
#include "runtime/Process.hpp"
#include <optional>
#include <span>
#include <string_view>

namespace Hydrogen::Runtime::Compat {
enum class Kind { RestoredContent, GrooveZeroServer, InternetChoice, FrameRateChoice };
enum class Fallback {
    Gles1Emulation,
    FrameRateCap,
    InternetOff,
    ReducedTextureResolution,
    ExtraMemoryHeadroom,
};
struct FallbackInfo {
    Fallback fallback;
    const char* id;
    const char* label;
    const char* detail;
};
struct Profile {
    const char* id;
    Kind kind;
    const char* match;
    const char* effect;
    const char* evidence;
    const char* verify;
};
std::span<const Profile> profiles();
std::span<const FallbackInfo> fallbacks();
const FallbackInfo* findFallback(std::string_view id);
std::optional<Fallback> chooseFallback(std::string_view crashText, const ProcessConfig& current,
                                       std::span<const Fallback> tried = {});
void applyFallback(ProcessConfig& cfg, Fallback fb);
const Profile* serverProfile(std::string_view normalizedHost);
void noteApplied(Process&, Kind);
void initialize(Process&);
bool restoredContentReachable(Process&);
inline bool internetAccess(const Process& p) { return p.config().internetAccess; }
inline bool unlockFrameRate(const Process& p) { return p.config().unlockFrameRate; }
}
