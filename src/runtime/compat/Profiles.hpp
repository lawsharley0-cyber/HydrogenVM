#pragma once
#include "runtime/Process.hpp"
#include <span>
#include <string_view>

namespace Hydrogen::Runtime::Compat {
enum class Kind { RestoredContent, GrooveZeroServer, InternetChoice, FrameRateChoice };
struct Profile {
    const char* id;
    Kind kind;
    const char* match;
    const char* effect;
    const char* evidence;
    const char* verify;
};
std::span<const Profile> profiles();
const Profile* serverProfile(std::string_view normalizedHost);
void noteApplied(Process&, Kind);
void initialize(Process&);
bool restoredContentReachable(Process&);
inline bool internetAccess(const Process& p) { return p.config().internetAccess; }
inline bool unlockFrameRate(const Process& p) { return p.config().unlockFrameRate; }
}
