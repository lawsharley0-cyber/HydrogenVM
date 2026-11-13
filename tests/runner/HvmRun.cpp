// `hvm run <bundle.app | file.ipa> [--home DIR] [--keep-going] [--trace] [--quiet]
//          [--run-for SECONDS] [--screenshot FILE.png] [--tap X,Y@T] [--drag X1,Y1,X2,Y2@T]
//          [--report FILE.json] [-- args...]`
// --report writes a machine-readable summary: exit status, crash report, every
// warning/error the runtime logged, missing functions, and performance counters.
#include "runtime/gl/HostGL.hpp"
#include "loader/AppBundle.hpp"
#include "runtime/Process.hpp"
#include "runtime/graphics/Graphics.hpp"
#include "AudioOutput.hpp"
#include "EntryTrace.hpp"
#include "DesktopInput.hpp"
#include "runtime/uikit/UIKit.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include "runtime/libc/TlsBridge.hpp"
#include <fstream>
#include <mutex>
#include <thread>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <bit>

using namespace Hydrogen;
namespace fs = std::filesystem;

namespace {
std::string jsonEscape(const std::string& in) {
    std::string o;
    o.reserve(in.size() + 8);
    for (unsigned char c : in) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += char(c);
        }
    }
    return o;
}
std::string jsonList(const std::vector<std::string>& v) {
    std::string o = "[";
    for (size_t i = 0; i < v.size(); ++i) o += (i ? ", \"" : "\"") + jsonEscape(v[i]) + "\"";
    return o + "]";
}
} // namespace

int hvmRunMain(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: hvm run <app|ipa> [--home DIR] [--keep-going] [--trace] [-- args]\n"); return 2; }
    std::string target = argv[1], home;
    Runtime::ProcessConfig cfg;
    bool quiet = false;
    double runFor = 0;
    bool timedStop = false;   // --run-for elapsed while the app was still running: not a failure
    bool graceful = false;    // end --run-for with requestGracefulStop (save, then stop) instead of a hard stop
    int launches = 1;         // run the app this many times in one host process (like reopening it in the iOS app)
    struct LifeEvent { bool background; double at; };
    std::vector<LifeEvent> lifeEvents;
    std::string screenshot, reportPath, frameFile, inputFile;
    int audioFd = -1;
    std::string photoLibraryImage, documentImport;
    struct Gesture { float x1, y1, x2, y2; double at; bool drag; };
    std::vector<Gesture> gestures;
    double micVoiceAt = -1, micVoiceFor = -1, micNoise = 0, micFrom = 0;
    std::string sendSel;
    double sendAt = -1;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--run-for") && i + 1 < argc) { runFor = std::atof(argv[++i]); continue; }
        if (!std::strcmp(argv[i], "--screenshot") && i + 1 < argc) { screenshot = argv[++i]; continue; }
        if (!std::strcmp(argv[i], "--tap") && i + 1 < argc) {
            Gesture g{};
            if (std::sscanf(argv[++i], "%f,%f@%lf", &g.x1, &g.y1, &g.at) == 3) { g.x2 = g.x1; g.y2 = g.y1; gestures.push_back(g); }
            continue;
        }
        if (!std::strcmp(argv[i], "--drag") && i + 1 < argc) {
            Gesture g{};
            g.drag = true;
            if (std::sscanf(argv[++i], "%f,%f,%f,%f@%lf", &g.x1, &g.y1, &g.x2, &g.y2, &g.at) == 5) gestures.push_back(g);
            continue;
        }
        // --mic-voice START,SECONDS: the microphone hears silence, except a voice-like sound
        // (a 220 Hz buzz with a syllable rhythm) for SECONDS starting START seconds in. For apps
        // that listen and answer (the Talking games) on a machine with no microphone.
        // --send SELECTOR@SECONDS: sends a message without arguments to the app delegate's
        // viewController (or to the delegate), to open a screen no tap reaches.
        if (!std::strcmp(argv[i], "--send") && i + 1 < argc) {
            std::string v = argv[++i];
            const size_t at = v.rfind('@');
            if (at != std::string::npos) { sendSel = v.substr(0, at); sendAt = std::atof(v.c_str() + at + 1); }
            continue;
        }
        if (!std::strcmp(argv[i], "--mic-voice") && i + 1 < argc) {
            // START,SECONDS[,NOISE[,FROM]]: a voice for SECONDS from START; NOISE is the room's
            // level (a phone microphone never reads exact silence); nothing arrives before FROM
            // (a phone's microphone starts some time after the app asks for it).
            if (std::sscanf(argv[++i], "%lf,%lf,%lf,%lf", &micVoiceAt, &micVoiceFor, &micNoise, &micFrom) < 2) micVoiceAt = micVoiceFor = -1;
            continue;
        }
        if (!std::strcmp(argv[i], "--report") && i + 1 < argc) { reportPath = argv[++i]; continue; }
        if (!std::strcmp(argv[i], "--frame-file") && i + 1 < argc) { frameFile = argv[++i]; continue; }
        if (!std::strcmp(argv[i], "--document-import") && i+1<argc) { documentImport=argv[++i]; continue; }
        if (!std::strcmp(argv[i], "--photo-library-image") && i+1<argc) { photoLibraryImage=argv[++i]; continue; }
        if (!std::strcmp(argv[i], "--input-file") && i + 1 < argc) { inputFile = argv[++i]; continue; }
        if (!std::strcmp(argv[i], "--audio-fd") && i + 1 < argc) {
            char* end = nullptr;
            const long fd = std::strtol(argv[++i], &end, 10);
            if (!end || *end || fd < 3 || fd > INT32_MAX || !DesktopAudioOutput::valid(int(fd))) {
                std::fprintf(stderr, "--audio-fd requires an inherited datagram socket\n"); return 2;
            }
            audioFd = int(fd); continue;
        }
        if (!std::strcmp(argv[i], "--home") && i + 1 < argc) home = argv[++i];
        else if (!std::strcmp(argv[i], "--keep-going")) cfg.stopOnMissingFunction = false;
        else if (!std::strcmp(argv[i], "--graceful")) graceful = true;
        else if (!std::strcmp(argv[i], "--launches") && i + 1 < argc) { launches = std::max(1, std::atoi(argv[++i])); graceful = true; }
        else if (!std::strcmp(argv[i], "--lifecycle") && i + 1 < argc) {
            // "bg@5,fg@8": send the app to the background at 5 s, back at 8 s
            std::string spec = argv[++i];
            for (size_t a = 0; a < spec.size();) {
                size_t b = spec.find(',', a);
                if (b == std::string::npos) b = spec.size();
                const std::string item = spec.substr(a, b - a);
                const size_t at = item.find('@');
                if (at != std::string::npos) lifeEvents.push_back({item.substr(0, at) == "bg", std::atof(item.c_str() + at + 1)});
                a = b + 1;
            }
        }
        else if (!std::strcmp(argv[i], "--trace")) cfg.traceCalls = true;
        else if (!std::strcmp(argv[i], "--gl-trace")) cfg.glTrace = true;
        else if (!std::strcmp(argv[i], "--thread-liveness")) cfg.threadLiveness = true;
        else if (!std::strcmp(argv[i], "--gpu")) {
            std::string why;
            cfg.hostGL = Hydrogen::Runtime::HostGL::createEglPlatform(&why);
            if (!cfg.hostGL) { std::fprintf(stderr, "--gpu: no host OpenGL ES (%s)\n", why.c_str()); return 2; }
        }
        else if (!std::strcmp(argv[i], "--loopback-network")) cfg.loopbackNetwork = true;
        else if (!std::strcmp(argv[i], "--offline-clash")) cfg.offlineClash = true;
        else if (!std::strcmp(argv[i], "--internet")) cfg.internetAccess = true;
        else if (!std::strcmp(argv[i], "--loopback-alias") && i + 1 < argc) cfg.loopbackAliases.push_back(argv[++i]);
        else if (!std::strcmp(argv[i], "--guest-libs")) cfg.guestLibs = true;
        else if (!std::strcmp(argv[i], "--guest-lib-dir") && i + 1 < argc) { cfg.guestLibDir = argv[++i]; cfg.guestLibs = true; }
        else if (!std::strcmp(argv[i], "--quiet")) quiet = true;
        else if (!std::strcmp(argv[i], "--")) { for (++i; i < argc; ++i) cfg.extraArgs.push_back(argv[i]); }
    }
    if (cfg.internetAccess) {
        // Old game TLS in, modern TLS out (TlsBridge.hpp). HVM_TLS_BRIDGE_NOVERIFY=1 accepts a
        // local test server's self-signed certificate.
        std::string why;
        const char* noVerify = std::getenv("HVM_TLS_BRIDGE_NOVERIFY");
        cfg.tlsBridge = Hydrogen::Runtime::makeOpenSslTlsBridge(!(noVerify && *noVerify == '1'), &why);
        if (!cfg.tlsBridge) std::fprintf(stderr, "TLS bridge unavailable: %s\n", why.c_str());
    }
    if (!cfg.loopbackAliases.empty() && !cfg.loopbackNetwork) {
        std::fprintf(stderr, "--loopback-alias requires --loopback-network\n"); return 2;
    }
    if (!inputFile.empty() && (launches != 1 || !fs::is_regular_file(inputFile))) {
        std::fprintf(stderr, "--input-file requires an existing regular file and a single launch\n");
        return 2;
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
    // Everything the runtime reports, for --report (deduplicated, in order, with counts).
    std::mutex logMu;
    std::vector<std::pair<std::string, int>> warnings, errors;
    auto note = [&logMu](std::vector<std::pair<std::string, int>>& v, const std::string& m) {
        std::lock_guard<std::mutex> lk(logMu);
        for (auto& e : v) if (e.first == m) { ++e.second; return; }
        if (v.size() < 2000) v.emplace_back(m, 1);
    };
    cfg.log = [quiet, &note, &warnings, &errors](Runtime::LogLevel lvl, const std::string& msg) {
        if (lvl == Runtime::LogLevel::Warning) note(warnings, msg);
        if (lvl == Runtime::LogLevel::Error) note(errors, msg);
        switch (lvl) {
            case Runtime::LogLevel::GuestStdout: std::fwrite(msg.data(), 1, msg.size(), stdout); std::fflush(stdout); break;
            case Runtime::LogLevel::GuestStderr: std::fwrite(msg.data(), 1, msg.size(), stderr); break;
            case Runtime::LogLevel::Error: std::fprintf(stderr, "[hvm:error] %s\n", msg.c_str()); break;
            case Runtime::LogLevel::Warning: if (!quiet) std::fprintf(stderr, "[hvm:warn] %s\n", msg.c_str()); break;
            default: if (!quiet) std::fprintf(stderr, "[hvm] %s\n", msg.c_str()); break;
        }
    };
    // Keep the last presented frame for --screenshot.
    std::mutex frameMu;
    Graphics::Image lastFrame;
    auto lastPublished = std::chrono::steady_clock::time_point{};
    if (!screenshot.empty() || !frameFile.empty()) {
        cfg.present = [&](const uint8_t* rgba, int w, int h) {
            std::lock_guard<std::mutex> lk(frameMu);
            lastFrame.width = w; lastFrame.height = h;
            lastFrame.rgba.assign(rgba, rgba + size_t(w) * size_t(h) * 4);
            const auto now = std::chrono::steady_clock::now();
            if (!frameFile.empty() && now - lastPublished >= std::chrono::milliseconds(100)) {
                // Atomic, capped-at-10-Hz preview. HVF1 + LE width/height/scale + RGBA.
                // The preview is optional and does not change guest frame timing.
                lastPublished = now;
                const std::string temporary = frameFile + ".tmp";
                std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
                const uint32_t header[] = {0x31465648, uint32_t(w), uint32_t(h), std::bit_cast<uint32_t>(cfg.screenScale)};
                out.write(reinterpret_cast<const char*>(header), sizeof header);
                out.write(reinterpret_cast<const char*>(rgba), size_t(w)*size_t(h)*4);
                out.close();
                if (!out) throw std::runtime_error("Cannot write live frame: " + temporary);
                fs::rename(temporary, frameFile);
            }
        };
    }
    // Missing-API hits of the launches finished so far: each launch has its own Process, but the report file is
    // rewritten by every launch, so a hit in launch 1 must still taint the final report.
    Runtime::MissingApiLog earlierLaunches;
    // One launch = one Process (as the iOS app does each time a game is opened).
    auto runOnce = [&](int launch) -> int {
        if (launches > 1 && !quiet) std::fprintf(stderr, "[hvm] ---- launch %d of %d ----\n", launch, launches);
        const auto wallStart = std::chrono::steady_clock::now();
        EntryTrace entryTrace;
        Runtime::Process proc(cfg);
        entryTrace.attach(proc);
        if (!documentImport.empty()) {
            Runtime::UIKit::setDocumentPickerObserver(proc, [&proc, documentImport](const Runtime::UIKit::DocumentPickerRequest& q) {
                if (q.active) Runtime::UIKit::submitDocumentPickerResult(proc, q.requestId, documentImport == "cancel" ? "" : documentImport);
            });
        }
        if(!photoLibraryImage.empty()) {
            Runtime::UIKit::setPhotoPickerObserver(proc,[&proc,photoLibraryImage](const Runtime::UIKit::PhotoPickerRequest& q){
                if(!q.active)return;
                std::vector<uint8_t> bytes;
                if(photoLibraryImage!="cancel") {
                    std::ifstream file(photoLibraryImage,std::ios::binary|std::ios::ate);
                    const auto size=file.tellg();
                    if(size>0 && size<=16*1024*1024){bytes.resize(size);file.seekg(0);if(!file.read(reinterpret_cast<char*>(bytes.data()),size))bytes.clear();}
                }
                proc.log(Runtime::LogLevel::Info,"desktop photo-library fixture selected");
                Runtime::UIKit::submitPhotoPickerResult(proc,q.requestId,std::move(bytes));
            });
        }

        // Desktop text commands answer the guest's latest keyboard request (#303/#381):
        // custom UIKeyInput responders get incremental events, text fields their full text.
        // Shared with the observer, which a guest worker may still call while the process ends.
        struct KeyboardTrack { std::mutex mu; Runtime::UIKit::TextInputRequest request; uint64_t seq = 0; };
        auto keyboardTrack = std::make_shared<KeyboardTrack>();
        std::mutex& keyboardMu = keyboardTrack->mu;
        Runtime::UIKit::TextInputRequest& keyboard = keyboardTrack->request;
        uint64_t& keyboardSeq = keyboardTrack->seq;
        Runtime::UIKit::setTextInputObserver(proc, [keyboardTrack](const Runtime::UIKit::TextInputRequest& q) {
            std::lock_guard<std::mutex> lk(keyboardTrack->mu);
            if (q.active != keyboardTrack->request.active || q.focusId != keyboardTrack->request.focusId)
                std::fprintf(stderr, "[hvm] Keyboard: %s%s\n", q.active ? "shown for a " : "hidden", q.active ? (q.keyInput ? "key-input view" : "text field") : "");
            keyboardTrack->request = q;
        });
        auto sendText = [&](Runtime::UIKit::TextInputEvent::Kind kind, std::string text) {
            Runtime::UIKit::TextInputEvent ev{kind, std::move(text), 0, 0};
            {
                std::lock_guard<std::mutex> lk(keyboardMu);
                if (!keyboard.active) { std::fprintf(stderr, "[hvm:warn] Desktop text input without a focused field\n"); return; }
                ev.focusId = keyboard.focusId;
                if (!keyboard.keyInput && kind != Runtime::UIKit::TextInputEvent::Return) {
                    // A UITextField / UITextView takes its whole new text.
                    std::string full = keyboard.text;
                    if (kind == Runtime::UIKit::TextInputEvent::DeleteBackward) {
                        while (!full.empty() && (uint8_t(full.back()) & 0xC0) == 0x80) full.pop_back();
                        if (!full.empty()) full.pop_back();
                    } else full += ev.text;
                    keyboard.text = full;
                    ev = {Runtime::UIKit::TextInputEvent::SetText, std::move(full), keyboard.focusId, ++keyboardSeq};
                }
            }
            Runtime::UIKit::submitTextInput(proc, std::move(ev));
        };
        DesktopAudioOutput audio;
        std::string loadError;
        auto writeReport = [&](int status) {
            if (reportPath.empty()) return;
            auto& pf = proc.perf();
            const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count();
            const uint64_t insns = proc.guestInstructions();
            auto pairs = [](const std::vector<std::pair<std::string, int>>& v) {
                std::string o = "[";
                for (size_t i = 0; i < v.size(); ++i)
                    o += std::string(i ? ", " : "") + "{\"message\": \"" + jsonEscape(v[i].first) + "\", \"count\": " + std::to_string(v[i].second) + "}";
                return o + "]";
            };
            std::lock_guard<std::mutex> lk(logMu);
            std::ofstream f(reportPath);
            f << "{\n";
            f << "  \"bundle\": \"" << jsonEscape(cfg.bundlePath) << "\",\n";
            f << "  \"exitStatus\": " << status << ",\n";
            f << "  \"stillRunning\": " << (timedStop ? "true" : "false") << ",\n";   // reached --run-for without failing
            f << "  \"crashed\": " << (proc.crashed() ? "true" : "false") << ",\n";
            f << "  \"crashReport\": \"" << jsonEscape(proc.crashReport()) << "\",\n";
            f << "  \"signature\": \"" << proc.crashSignature() << "\",\n";
            f << "  \"crashBundle\": " << proc.crashBundle() << ",\n";
            f << "  \"runtimeMemorySamples\": " << proc.memorySamplesJson() << ",\n";
            f << "  \"threadLiveness\": " << proc.threadLivenessJson() << ",\n";
            f << "  \"loadError\": \"" << jsonEscape(loadError) << "\",\n";
            f << "  \"missingFunctions\": " << jsonList(proc.missingSymbols()) << ",\n";
            Runtime::MissingApiLog allLaunches;   // tainted, stubHits, missingApis (T2a), over every launch so far
            allLaunches.absorb(earlierLaunches);
            allLaunches.absorb(proc.missingApis());
            f << allLaunches.reportFields("  ") << ",\n";
            f << "  \"unimplementedImports\": " << jsonList(proc.unimplementedImports()) << ",\n";
            f << "  \"warnings\": " << pairs(warnings) << ",\n";
            f << "  \"errors\": " << pairs(errors) << ",\n";
            f << "  \"audio\": {\"enabled\": " << (audioFd >= 0 ? "true" : "false")
              << ", \"frames\": " << audio.frames << ", \"nonSilentFrames\": " << audio.nonSilentFrames
              << ", \"peak\": " << audio.peak << ", \"droppedBlocks\": " << audio.droppedBlocks
              << ", \"transportError\": " << audio.error << "},\n";
            {
                const auto ms = proc.mixer().stats();
                f << "  \"mixer\": {\"framesRendered\": " << ms.framesRendered << ", \"audibleFrames\": " << ms.audibleFrames
                  << ", \"musicFrames\": " << ms.musicFrames << ", \"effectFrames\": " << ms.effectFrames
                  << ", \"musicVoices\": " << ms.musicVoices << ", \"effectVoices\": " << ms.effectVoices
                  << ", \"peak\": " << ms.peak << "},\n";
                f << "  \"display\": {\"offscreenPresents\": " << pf.offscreenPresents.load()
                  << ", \"faultsToApp\": " << pf.faultsToApp.load() << ", \"missingCount\": " << proc.missingCount() << "},\n";
                if (cfg.glTrace) {
                    f << "  \"gl\": {\"errors\": " << pf.glErrors.load() << ", \"draws\": " << pf.glDraws.load()
                      << ", \"presents\": " << pf.glPresents.load() << ", \"emptyPresents\": " << pf.glEmptyPresents.load() << ", \"lastFrameDraws\": " << pf.glLastFrameDraws.load()
                      << ", \"errorLog\": [";
                    bool first = true;
                    for (const auto& line : proc.glErrorLog()) {
                        f << (first ? "" : ", ") << '"';
                        for (char c : line) { if (c == '"' || c == '\\') f << '\\'; f << c; }
                        f << '"';
                        first = false;
                    }
                    f << "]},\n";
                }
            }
            f << "  \"perf\": {\n";
            f << Runtime::perfJson(pf, wall, insns, proc.engine().stats().blocks_decoded);
            f << "  }\n}\n";
        };
        try {
            proc.load();
        } catch (const std::exception& e) {
            loadError = e.what();
            std::fprintf(stderr, "[hvm:error] load failed: %s\n", e.what());
            writeReport(71);
            return 71;
        }
        // Scripted input and the run-time limit run on a helper thread.
        audio.start(audioFd, proc.mixerPtr());
        std::atomic<bool> done{false};
        std::thread driver([&] {
            using namespace std::chrono;
            const auto start = steady_clock::now();
            std::ifstream input;
            if (!inputFile.empty()) input.open(inputFile);
            std::string pendingInput;
            bool inputOverflow=false;
            auto pumpInput = [&] {
                if (!input.is_open()) return;
                input.clear();
                char ch;
                while (input.get(ch)) {
                    if (ch != '\n') {
                        if (pendingInput.size() < 32773) pendingInput += ch;
                        else inputOverflow=true;
                        continue;
                    }
                    const std::string command = std::move(pendingInput); pendingInput.clear();
                    if(inputOverflow) {
                        inputOverflow=false;std::fprintf(stderr,"[hvm:warn] Desktop input exceeds bounded line size\n");continue;
                    }
                    if (DesktopInput::applyTouch(proc, command, quiet)) {   // "touch <phase> <x> <y> [finger]"
                    } else if(command.starts_with("text ")) {
                        const auto digit=[](char c) {
                            if(c>='0' && c<='9')return c-'0';
                            if(c>='a' && c<='f')return c-'a'+10;
                            if(c>='A' && c<='F')return c-'A'+10;
                            return -1;
                        };
                        const auto hex=command.substr(5);std::string text;
                        bool valid=hex.size()%2==0 && hex.size()<=32768;
                        for(size_t i=0;valid && i<hex.size();i+=2) {
                            const int hi=digit(hex[i]),lo=digit(hex[i+1]);
                            if(hi<0 || lo<0)valid=false;
                            else text.push_back(char(hi*16+lo));
                        }
                        if(valid) {
                            if(!quiet)std::fprintf(stderr,"[hvm] desktop text input (%zu bytes)\n",text.size());
                            sendText(Runtime::UIKit::TextInputEvent::InsertText, std::move(text)); // never log entered text
                        } else std::fprintf(stderr,"[hvm:warn] Invalid desktop text encoding\n");
                    } else if(command=="backspace")sendText(Runtime::UIKit::TextInputEvent::DeleteBackward, {});
                    else if(command=="return")sendText(Runtime::UIKit::TextInputEvent::Return, {});
                    else if (command == "background") proc.enterBackground();
                    else if (command == "foreground") proc.enterForeground();
                    else if (command == "stop") proc.requestGracefulStop();
                    else std::fprintf(stderr, "[hvm:warn] Invalid desktop input command\n");
                }
            };
            auto waitUntil = [&](double t) {
                while (!done && duration<double>(steady_clock::now() - start).count() < t) {
                    pumpInput(); std::this_thread::sleep_for(milliseconds(5));
                }
            };
            struct TimelineEvent {
                double at;
                enum Type { Tap, Drag, Life } type;
                Gesture g;
                LifeEvent l;
            };
            std::vector<TimelineEvent> timeline;
            for (const auto& g : gestures) timeline.push_back({g.at, g.drag ? TimelineEvent::Drag : TimelineEvent::Tap, g, {}});
            for (const auto& e : lifeEvents) timeline.push_back({e.at, TimelineEvent::Life, {}, e});
            std::sort(timeline.begin(), timeline.end(), [](const TimelineEvent& a, const TimelineEvent& b) { return a.at < b.at; });
            for (const auto& ev : timeline) {
                waitUntil(ev.at);
                if (done) return;
                if (ev.type == TimelineEvent::Tap) {
                    proc.injectTouch(Runtime::TouchPhase::Began, ev.g.x1, ev.g.y1, 1);
                    std::this_thread::sleep_for(milliseconds(60));
                    proc.injectTouch(Runtime::TouchPhase::Ended, ev.g.x2, ev.g.y2, 1);
                } else if (ev.type == TimelineEvent::Drag) {
                    proc.injectTouch(Runtime::TouchPhase::Began, ev.g.x1, ev.g.y1, 1);
                    for (int k = 1; k <= 10; ++k) {
                        std::this_thread::sleep_for(milliseconds(16));
                        proc.injectTouch(Runtime::TouchPhase::Moved, ev.g.x1 + (ev.g.x2 - ev.g.x1) * float(k) / 10, ev.g.y1 + (ev.g.y2 - ev.g.y1) * float(k) / 10, 1);
                    }
                    proc.injectTouch(Runtime::TouchPhase::Ended, ev.g.x2, ev.g.y2, 1);
                } else if (ev.type == TimelineEvent::Life) {
                    if (!quiet) std::fprintf(stderr, "[hvm] lifecycle: %s at %.1f s\n", ev.l.background ? "background" : "foreground", ev.l.at);
                    if (ev.l.background) proc.enterBackground(); else proc.enterForeground();
                }
            }
            if (runFor > 0) {
                waitUntil(runFor);
                if (!done) {
                    timedStop = true;
                    if (graceful) proc.requestGracefulStop(); else proc.requestStop();
                }
            }
            while (!inputFile.empty() && !done) { pumpInput(); std::this_thread::sleep_for(milliseconds(5)); }
        });
        std::thread mic;
        if (micVoiceAt >= 0 && micVoiceFor > 0) mic = std::thread([&] {
            using namespace std::chrono;
            const auto start = steady_clock::now();
            const double rate = 44100.0;
            uint64_t sent = 0;
            std::vector<float> chunk(441);   // 10 ms
            while (!done) {
                const double t = duration<double>(steady_clock::now() - start).count();
                while (double(sent) / rate < t) {
                    for (float& v : chunk) {
                        const double at = double(sent++) / rate;
                        const bool speaking = at >= micVoiceAt && at < micVoiceAt + micVoiceFor;
                        const double phase = std::fmod(at * 220.0, 1.0);
                        const double syllable = 0.5 + 0.5 * std::sin(at * 2.0 * 3.141592653589793 * 3.0);
                        const double noise = micNoise * (double(std::rand()) / RAND_MAX * 2.0 - 1.0);
                        v = float((speaking ? (phase * 2.0 - 1.0) * 0.6 * syllable : 0.0) + noise);
                    }
                    if (double(sent) / rate >= micFrom) proc.pushMicrophone(chunk.data(), chunk.size(), rate);
                }
                std::this_thread::sleep_for(milliseconds(5));
            }
        });
        std::thread sender;
        if (!sendSel.empty() && sendAt >= 0) sender = std::thread([&] {
            using namespace std::chrono;
            const auto start = steady_clock::now();
            while (!done && duration<double>(steady_clock::now() - start).count() < sendAt) std::this_thread::sleep_for(milliseconds(20));
            if (done) return;
            Runtime::Process* pp = &proc;
            const std::string name = sendSel;
            Runtime::Foundation::mainRunLoop(proc).post([pp, name](CPU::Cpu& c) {
                auto& r = Runtime::ObjC::rt(*pp);
                auto& u = Runtime::UIKit::ui(*pp);
                Runtime::ObjC::id target = u.appDelegate;
                if (target && r.respondsTo(target, r.sel("viewController")))
                    if (auto vc = Runtime::ObjC::id(r.send(c, target, "viewController")); vc && r.respondsTo(vc, r.sel(name))) target = vc;
                if (!target || !r.respondsTo(target, r.sel(name))) { std::fprintf(stderr, "[hvm:warn] --send: nothing responds to %s\n", name.c_str()); return; }
                std::fprintf(stderr, "[hvm] --send: %s to %s\n", name.c_str(), r.objectClassName(target).c_str());
                r.send(c, target, name.c_str());
            });
        });
        const int r = proc.run();
        done = true;
        driver.join();
        if (sender.joinable()) sender.join();
        if (mic.joinable()) mic.join();
        audio.stop();
        if (audio.error) std::fprintf(stderr, "[hvm:warn] Desktop audio transport failed: %d\n", audio.error);
        if (!screenshot.empty()) {
            std::lock_guard<std::mutex> lk(frameMu);
            if (lastFrame.width > 0) {
                const auto png = Graphics::encodePng(lastFrame);
                std::ofstream(screenshot, std::ios::binary).write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
                if (!quiet) std::fprintf(stderr, "[hvm] screenshot: %s (%dx%d)\n", screenshot.c_str(), lastFrame.width, lastFrame.height);
            } else std::fprintf(stderr, "[hvm] no frame was presented; no screenshot\n");
        }
        // A timed stop can itself fail (for example the save watchdog). Preserve
        // that failure instead of claiming a successful --run-for observation.
        const int status = proc.crashed() ? 70 : timedStop ? 0 : r;
        if (timedStop && !quiet) std::fprintf(stderr, "[hvm] still running after %.0f s (stopped by --run-for)\n", runFor);
        // A run that called missing or approximated APIs is not a clean pass, whatever its exit status says.
        if (proc.tainted())
            std::fprintf(stderr, "[hvm] %s with %llu stub hits (%zu distinct missing APIs); see \"missingApis\" in the report\n",
                         status == 0 ? "exited 0" : "finished", static_cast<unsigned long long>(proc.missingApis().totalHits()),
                         proc.missingApis().distinct());
        writeReport(status);
        earlierLaunches.absorb(proc.missingApis());
        return status;
    };
    int status = 0;
    for (int launch = 1; launch <= launches; ++launch) {
        timedStop = false;
        status = runOnce(launch);
        if (status != 0) break;
    }
    return status;
}
