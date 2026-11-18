#include "runtime/compat/Profiles.hpp"
#include "runtime/foundation/Foundation.hpp"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <unistd.h>
using namespace Hydrogen;
using namespace Hydrogen::Runtime;
int runCompatProfileTests() {
    int failed=0,passed=0;
    auto check=[&](bool ok,const char* name) {
        std::printf("%s profile: %s\n",ok?"ok":"FAIL",name); ok?++passed:++failed;
    };
    const auto root=std::filesystem::temp_directory_path()/("hvm-profile-tests-"+std::to_string(getpid()));
    std::filesystem::remove_all(root);
    int contentLogs=0,serverLogs=0,choiceLogs=0;
    auto log=[&](LogLevel,const std::string& line) {
        if(line.find("Compatibility profile: restored-gameloft-content")!=std::string::npos) ++contentLogs;
        if(line.find("Compatibility profile: groove-zero-server")!=std::string::npos) ++serverLogs;
        if(line.find("Compatibility profile: allow-internet")!=std::string::npos ||
           line.find("Compatibility profile: unlock-frame-rate")!=std::string::npos) ++choiceLogs;
    };
    ProcessConfig cfg; cfg.homePath=(root/"first").string(); cfg.log=log;
    {
        Process p(cfg);
        auto& cpu=p.mainThread().cpu;
        cpu.r[CPU::kSP]=p.heap().allocZeroed(65536)+65536-256;
        Ctx x{p,cpu,p.mem()};
        check(!Compat::internetAccess(p) && !Compat::unlockFrameRate(p) && choiceLogs==0,"per-game choices default off");
        check(!Compat::restoredContentReachable(p),"no content marker means no offline reachability exception");
        const auto marker=std::filesystem::path(p.homePath())/"Library/Caches/dlcs/mnhtn_index_ios";
        std::filesystem::create_directories(marker.parent_path()); std::ofstream(marker)<<"owner fixture";
        check(Compat::restoredContentReachable(p) && Compat::restoredContentReachable(p) && contentLogs==1,
              "restored-content condition applies and logs once per process");
        check(!Compat::internetAccess(p),"offline reachability exception never enables internet");
        std::filesystem::remove(marker);
        check(!Compat::restoredContentReachable(p),"removing marker preserves live predicate behavior");
        check(Compat::serverProfile("gczero.com") && Compat::serverProfile("staging.gczero.com"),"both original exact stand-in hosts match");
        check(!Compat::serverProfile("gczero.com.attacker.invalid") && !Compat::serverProfile("unrelated.invalid"),"unrelated hosts do not match");
        auto start=Foundation::standInFetch(x,std::string("http://gczero.com/start.php"),"GET");
        auto alias=Foundation::standInFetch(x,std::string("http://staging.gczero.com/start.php"),"GET");
        check(start && alias && start->body==alias->body && start->body.find("<stage_pak>")!=std::string::npos,
              "stand-in aliases preserve the existing local reply");
        check(serverLogs==1,"both stand-in aliases log the profile only once");
        check(!Foundation::standInFetch(x,std::string("http://gczero.com/shop_list.php"),"GET"),"shop remains unreachable");
        const auto unknown=Foundation::standInFetch(x,std::string("http://gczero.com/not-implemented.php"),"GET");
        check(unknown && unknown->body.find("<code>0</code>")!=std::string::npos,
              "unknown endpoint retains existing explicitly-defaulted reply");
        check(!Foundation::standInFetch(x,std::string("http://unrelated.invalid/start.php"),"GET"),"unrelated URL gets no stand-in");
    }
    // A second Process receives its own activation log, even in the same host.
    {
        Process p(cfg); Compat::noteApplied(p,Compat::Kind::RestoredContent);
        Compat::noteApplied(p,Compat::Kind::RestoredContent);
        check(contentLogs==2,"activation is per process, not process-global static once");
    }
    cfg.homePath=(root/"choices").string(); cfg.internetAccess=true; cfg.unlockFrameRate=true;
    {
        Process p(cfg); Compat::initialize(p);
        check(Compat::internetAccess(p) && Compat::unlockFrameRate(p) && choiceLogs==2,
              "explicit choices are preserved and each logs once");
    }
    bool documented=true;
    for(const auto& entry:Compat::profiles())
        documented &= *entry.id && *entry.match && *entry.effect && *entry.evidence && *entry.verify;
    check(documented,"every profile records detection, effect, evidence and verification");

    // C3 safe-mode fallback selection from crash signatures.
    {
        ProcessConfig base;
        base.internetAccess = true;
        base.unlockFrameRate = true;
        check(Compat::chooseFallback("NSURLConnection TLS handshake failed", base) == Compat::Fallback::InternetOff,
              "network crash signature chooses internet-off when internet is enabled");
        check(Compat::chooseFallback("CADisplayLink timer stall at 60 fps", base) == Compat::Fallback::FrameRateCap,
              "display-link/fps crash signature chooses frame-rate-cap when unlockFrameRate is enabled");
        check(Compat::chooseFallback("glTexImage2D GL_OUT_OF_MEMORY 0x0505", base) == Compat::Fallback::ReducedTextureResolution,
              "texture OOM signature chooses reduced-texture-resolution");
        check(Compat::chooseFallback("guest memory budget exceeded (oom)", base) == Compat::Fallback::ExtraMemoryHeadroom,
              "memory budget signature chooses extra-memory-headroom");
        check(Compat::chooseFallback("OpenGL ES fixed-function shader error 0x0502", base) == Compat::Fallback::Gles1Emulation,
              "GLES shader signature chooses gles1-emulation");

        // When internet/unlockFrameRate are already off, those fallbacks are skipped.
        ProcessConfig off;
        check(Compat::chooseFallback("NSURLConnection TLS handshake failed", off) != Compat::Fallback::InternetOff,
              "internet-off is not chosen when internetAccess is already false");
        check(Compat::chooseFallback("CADisplayLink timer stall", off) != Compat::Fallback::FrameRateCap,
              "frame-rate-cap is not chosen when unlockFrameRate is already false");

        // Tried fallbacks are skipped.
        const Compat::Fallback tried[] = {Compat::Fallback::ReducedTextureResolution};
        check(Compat::chooseFallback("glTexImage2D texture error", base, tried) != Compat::Fallback::ReducedTextureResolution,
              "already-tried fallback is skipped");

        // applyFallback sets the expected ProcessConfig fields and logs on initialize.
        ProcessConfig appliedCfg;
        appliedCfg.homePath = (root / "fallback").string();
        appliedCfg.internetAccess = true;
        appliedCfg.unlockFrameRate = true;
        std::string fallbackLog;
        appliedCfg.log = [&](LogLevel, const std::string& line) {
            if (line.find("launched with fallback ") != std::string::npos) fallbackLog = line;
        };
        Compat::applyFallback(appliedCfg, Compat::Fallback::FrameRateCap);
        check(!appliedCfg.unlockFrameRate && appliedCfg.safeModeFallback == "frame-rate-cap",
              "applyFallback(FrameRateCap) disables unlockFrameRate and records id");
        {
            Process p(appliedCfg);
            Compat::initialize(p);
            check(fallbackLog.find("launched with fallback frame-rate-cap") != std::string::npos,
                  "initialize logs launched with fallback <id>");
        }
        Compat::applyFallback(appliedCfg, Compat::Fallback::ReducedTextureResolution);
        check(appliedCfg.maxTextureSize == 1024, "applyFallback(ReducedTextureResolution) caps maxTextureSize");
        Compat::applyFallback(appliedCfg, Compat::Fallback::Gles1Emulation);
        check(appliedCfg.gles1Emulate, "applyFallback(Gles1Emulation) enables gles1Emulate");
        Compat::applyFallback(appliedCfg, Compat::Fallback::InternetOff);
        check(!appliedCfg.internetAccess, "applyFallback(InternetOff) disables internetAccess");
        Compat::applyFallback(appliedCfg, Compat::Fallback::ExtraMemoryHeadroom);
        check(appliedCfg.memoryBudgetBytes >= (2048ULL << 20), "applyFallback(ExtraMemoryHeadroom) increases memory budget");
    }

    std::filesystem::remove_all(root);
    std::printf("compatibility profiles: %d passed, %d failed\n",passed,failed);
    return failed;
}
#ifdef HVM_COMPAT_PROFILE_MAIN
int main(){ return runCompatProfileTests(); }
#endif
