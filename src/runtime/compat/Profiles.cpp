#include "Profiles.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>

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
constexpr FallbackInfo fallbackTable[] = {
    {Fallback::Gles1Emulation, "gles1-emulation", "GLES1 emulation path",
     "Force the GLES1-over-ES2 fixed-function path (HVM_GL1_EMULATE=1)"},
    {Fallback::FrameRateCap, "frame-rate-cap", "frame-rate cap",
     "Disable experimental unlocked frame rate and cap at the game's requested rate"},
    {Fallback::InternetOff, "internet-off", "Allow internet off",
     "Disable outbound sockets/DNS for this launch"},
    {Fallback::ReducedTextureResolution, "reduced-texture-resolution", "reduced texture resolution",
     "Cap reported GL_MAX_TEXTURE_SIZE to 1024 to reduce VRAM/texture pressure"},
    {Fallback::ExtraMemoryHeadroom, "extra-memory-headroom", "extra memory headroom",
     "Increase the guest memory commit budget by 512 MB"},
};
struct Applied {
    explicit Applied(Process&) {}
    std::mutex mutex;
    std::set<Kind> kinds;
};

std::string lowerAscii(std::string_view in) {
    std::string out(in);
    for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool hasAny(const std::string& hay, std::initializer_list<const char*> needles) {
    for (const char* n : needles) {
        if (hay.find(n) != std::string::npos) return true;
    }
    return false;
}
}

std::span<const Profile> profiles() { return table; }
std::span<const FallbackInfo> fallbacks() { return fallbackTable; }

const FallbackInfo* findFallback(std::string_view id) {
    for (const auto& fb : fallbackTable) {
        if (id == fb.id) return &fb;
    }
    return nullptr;
}

std::optional<Fallback> chooseFallback(std::string_view crashText, const ProcessConfig& current,
                                       std::span<const Fallback> tried) {
    auto wasTried = [&](Fallback fb) {
        return std::find(tried.begin(), tried.end(), fb) != tried.end();
    };
    auto eligible = [&](Fallback fb) {
        if (wasTried(fb)) return false;
        switch (fb) {
            case Fallback::Gles1Emulation:
                return !current.gles1Emulate;
            case Fallback::FrameRateCap:
                return current.unlockFrameRate;
            case Fallback::InternetOff:
                return current.internetAccess;
            case Fallback::ReducedTextureResolution:
                return current.maxTextureSize == 0 || current.maxTextureSize > 1024;
            case Fallback::ExtraMemoryHeadroom:
                return true;
        }
        return false;
    };

    const std::string lower = lowerAscii(crashText);

    if (eligible(Fallback::InternetOff) &&
        hasAny(lower, {"nsurl", "cfnetwork", "tls", "ssl", "socket", "http", "dns", "internet", "connect"})) {
        return Fallback::InternetOff;
    }
    if (eligible(Fallback::FrameRateCap) &&
        hasAny(lower, {"displaylink", "cadisplaylink", "fps", "frame rate", "framerate", "unlock", "timer"})) {
        return Fallback::FrameRateCap;
    }
    if (eligible(Fallback::ReducedTextureResolution) &&
        hasAny(lower, {"texture", "teximage", "gl_out_of_memory", "0x0505", "framebuffer", "renderbuffer", "vram", "max_texture"})) {
        return Fallback::ReducedTextureResolution;
    }
    if (eligible(Fallback::ExtraMemoryHeadroom) &&
        hasAny(lower, {"out of memory", "memory budget", "memory", "oom", "jetsam", "enomem", "bad_alloc", "mmap"})) {
        return Fallback::ExtraMemoryHeadroom;
    }
    if (eligible(Fallback::Gles1Emulation) &&
        hasAny(lower, {"gles", "gl1", "opengl", "egl", "shader", "fixed-function", "0x0500", "0x0502"})) {
        return Fallback::Gles1Emulation;
    }

    constexpr Fallback order[] = {
        Fallback::Gles1Emulation,
        Fallback::ReducedTextureResolution,
        Fallback::ExtraMemoryHeadroom,
        Fallback::FrameRateCap,
        Fallback::InternetOff,
    };
    for (Fallback fb : order) {
        if (eligible(fb)) return fb;
    }
    return std::nullopt;
}

void applyFallback(ProcessConfig& cfg, Fallback fb) {
    for (const auto& info : fallbackTable) {
        if (info.fallback == fb) {
            cfg.safeModeFallback = info.id;
            break;
        }
    }
    switch (fb) {
        case Fallback::Gles1Emulation:
            cfg.gles1Emulate = true;
            break;
        case Fallback::FrameRateCap:
            cfg.unlockFrameRate = false;
            break;
        case Fallback::InternetOff:
            cfg.internetAccess = false;
            cfg.tlsBridge.reset();
            cfg.httpClient.reset();
            break;
        case Fallback::ReducedTextureResolution:
            cfg.maxTextureSize = 1024;
            break;
        case Fallback::ExtraMemoryHeadroom:
            if (cfg.memoryBudgetBytes > 0) cfg.memoryBudgetBytes += (512ULL << 20);
            else cfg.memoryBudgetBytes = (2048ULL << 20);
            break;
    }
}

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
    if (!p.config().safeModeFallback.empty()) {
        p.logf(LogLevel::Info, "launched with fallback %s", p.config().safeModeFallback.c_str());
    }
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
