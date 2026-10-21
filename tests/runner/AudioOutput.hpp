// Optional desktop transport. One datagram = 2048 frames of native-endian
// stereo int16 at 44100 Hz. The inherited socket is owned by the caller.
// Backpressure drops whole blocks; it must never stall the guest or grow a queue.
#pragma once
#include "runtime/audio/Audio.hpp"
#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>
#include <sys/socket.h>
#include <cerrno>
#include <stdexcept>

class DesktopAudioOutput {
public:
    uint64_t frames = 0, nonSilentFrames = 0, droppedBlocks = 0;
    float peak = 0;
    int error = 0;
    static bool valid(int fd) {
        int type = 0; socklen_t size = sizeof type;
        return fd >= 3 && getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size) == 0 && type == SOCK_DGRAM;
    }
    void start(int fd, std::shared_ptr<Hydrogen::Runtime::Audio::Mixer> mixer) {
        if (fd < 0) return;
        worker_ = std::jthread([this, fd, mixer](std::stop_token stop) {
            using namespace std::chrono;
            std::array<float, 2048> left{}, right{};
            std::array<int16_t, 4096> pcm{};
            auto next = steady_clock::now();
            while (!stop.stop_requested()) {
                mixer->render(left.data(), right.data(), left.size());
                for (size_t i = 0; i < left.size(); ++i) {
                    const float l = std::isfinite(left[i]) ? left[i] : 0;
                    const float r = std::isfinite(right[i]) ? right[i] : 0;
                    peak = std::max(peak, std::max(std::abs(l), std::abs(r)));
                    if (l != 0 || r != 0) ++nonSilentFrames;
                    pcm[2*i] = int16_t(std::clamp(l, -1.0f, 1.0f) * 32767);
                    pcm[2*i+1] = int16_t(std::clamp(r, -1.0f, 1.0f) * 32767);
                }
                frames += left.size();
                int flags = MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
                flags |= MSG_NOSIGNAL;
#endif
                const auto sent = send(fd, pcm.data(), sizeof pcm, flags);
                if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) ++droppedBlocks;
                else if (sent != sizeof pcm) { error = sent < 0 ? errno : EIO; break; }
                next += duration_cast<steady_clock::duration>(duration<double>(2048.0 / 44100));
                // Skip missed deadlines rather than replaying stale sound in bursts.
                if (next < steady_clock::now()) next = steady_clock::now();
                std::this_thread::sleep_until(next);
            }
        });
    }
    void stop() { if (worker_.joinable()) { worker_.request_stop(); worker_.join(); } }
    ~DesktopAudioOutput() { stop(); }
private:
    std::jthread worker_;
};
