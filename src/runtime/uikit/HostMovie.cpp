// Video playback handed to the host. See UIKit.hpp (MovieRequest).
#include "UIKit.hpp"
#include <algorithm>
#include <atomic>
#include <filesystem>

namespace Hydrogen::Runtime::UIKit {
namespace {
// Unique across process instances: a late answer for a closed game never matches a new one.
std::atomic<uint64_t> nextMovie{1};
void publish(Process& p, const MovieRequest& q) {
    std::function<void(const MovieRequest&)> observer;
    { std::lock_guard<std::mutex> lock(ui(p).inputMu); observer = ui(p).movieObserver; }
    if (observer) observer(q);
}
}
void setMovieObserver(Process& p, std::function<void(const MovieRequest&)> observer) {
    std::lock_guard<std::mutex> lock(ui(p).inputMu);
    ui(p).movieObserver = std::move(observer);
}
bool hostMovieFile(Ctx& x, id url, std::string& hostPath) {
    if (!url) return false;
    const std::string guest = Foundation::urlFilePath(x, url);
    if (guest.empty()) return false;
    std::string ext = std::filesystem::path(guest).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (ext != ".mp4" && ext != ".m4v" && ext != ".mov" && ext != ".3gp") return false;
    const std::string host = x.proc.hostPath(guest);
    std::error_code ec;
    if (host.empty() || !std::filesystem::is_regular_file(host, ec)) return false;
    hostPath = host;
    return true;
}
uint64_t startHostMovie(Ctx& x, const std::string& hostPath, std::function<void(Ctx&)> onEnd) {
    auto& u = ui(x);
    uint64_t previous = 0, id = 0;
    {
        std::lock_guard<std::mutex> lock(u.inputMu);
        if (!u.movieObserver || hostPath.empty()) return 0;
        previous = u.activeMovieId;
        id = nextMovie.fetch_add(1);
        u.activeMovieId = id;
        u.movieOnEnd = std::move(onEnd);
    }
    if (previous) publish(x.proc, {false, previous, {}});
    x.proc.log(LogLevel::Info, "Movie: playing " + std::filesystem::path(hostPath).filename().string() + " on the host");
    publish(x.proc, {true, id, hostPath});
    return id;
}
void stopHostMovie(Ctx& x, uint64_t requestId) {
    auto& u = ui(x);
    {
        std::lock_guard<std::mutex> lock(u.inputMu);
        if (!requestId || u.activeMovieId != requestId) return;
        u.activeMovieId = 0;
        u.movieOnEnd = nullptr;
    }
    publish(x.proc, {false, requestId, {}});
}
void movieFinished(Process& p, uint64_t requestId) {
    Process* process = &p;
    Foundation::mainRunLoop(p).post([process, requestId](CPU::Cpu& cpu) {
        Ctx x{*process, cpu, process->mem()};
        auto& u = ui(x);
        std::function<void(Ctx&)> onEnd;
        {
            std::lock_guard<std::mutex> lock(u.inputMu);
            if (!requestId || u.activeMovieId != requestId) return;   // stopped, replaced, or answered twice
            u.activeMovieId = 0;
            onEnd = std::move(u.movieOnEnd);
            u.movieOnEnd = nullptr;
        }
        publish(x.proc, {false, requestId, {}});
        if (onEnd) onEnd(x);
    });
}
}
