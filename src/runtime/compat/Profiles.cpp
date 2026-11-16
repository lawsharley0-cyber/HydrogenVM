#include "Profiles.hpp"
#include <filesystem>
#include <mutex>
#include <set>

namespace Hydrogen::Runtime::Compat {
namespace {
// Matches deliberately preserve the pre-refactor predicates. In particular,
// restored content and stand-in hosts are detected conditions, not title gates.
constexpr Profile table[] = {
    {"restored-gameloft-content", Kind::RestoredContent, "Library/Caches/dlcs/mnhtn_index_ios",
     "Report Reachable offline for restored DLC; do not enable sockets or DNS",
     "PR #480; docs/research/minion-rush-1.0.0.md",
     "Create/remove the index in a fresh private home; flags change 0/2, internet remains off"},
    {"groove-zero-server", Kind::GrooveZeroServer, "gczero.com",
     "Answer the retired server locally from the existing stand-in protocol",
     "docs/research/groove-coaster-zero.md",
     "Request start.php and bundled paks; unknown requests remain explicitly defaulted"},
    {"groove-zero-server", Kind::GrooveZeroServer, "staging.gczero.com",
     "Alias of the same retired server; no outbound connection",
     "docs/research/groove-coaster-zero.md; pre-refactor ServerStandIn table",
     "Both exact normalized hosts select the same responder; unrelated hosts do not"},
    {"allow-internet", Kind::InternetChoice, "ProcessConfig::internetAccess (explicit per-game choice)",
     "Permit outbound IPv4/TLS only for the chosen run; default remains false",
     "AGENTS.md owner authorization 2026-11-12; RunModel internet.<bundle id>",
     "Default false; explicit true preserved; offline content never changes this choice"},
    {"unlock-frame-rate", Kind::FrameRateChoice, "ProcessConfig::unlockFrameRate (explicit per-game choice)",
     "Use the existing experimental display-link/timer override; default remains false",
     "PR #482; RunModel unlockFPS.<bundle id>",
     "Default false; explicit true reaches both existing timer/display-link consumers"}
};
struct Applied {
    explicit Applied(Process&) {}
    std::mutex mutex;
    std::set<Kind> kinds;
};
}

std::span<const Profile> profiles() { return table; }
const Profile* serverProfile(std::string_view host) {
    for (const auto& profile : table)
        if (profile.kind == Kind::GrooveZeroServer && host == profile.match) return &profile;
    return nullptr;
}
void noteApplied(Process& p, Kind kind) {
    auto& applied=p.service<Applied>();
    {
        std::lock_guard<std::mutex> lock(applied.mutex);
        if (!applied.kinds.insert(kind).second) return;
    }
    for (const auto& profile : table) if (profile.kind == kind) {
        p.logf(LogLevel::Info,"Compatibility profile: %s — %s (%s)",profile.id,profile.effect,profile.evidence);
        break;
    }
}
void initialize(Process& p) {
    if (internetAccess(p)) noteApplied(p,Kind::InternetChoice);
    if (unlockFrameRate(p)) noteApplied(p,Kind::FrameRateChoice);
}
bool restoredContentReachable(Process& p) {
    for (const auto& profile : table) if (profile.kind == Kind::RestoredContent) {
        std::error_code error;
        const bool present=std::filesystem::exists(std::filesystem::path(p.homePath())/profile.match,error);
        if (present) noteApplied(p,profile.kind);
        return present;
    }
    return false;
}
}
