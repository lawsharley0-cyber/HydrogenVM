#include "Process.hpp"
#include "audio/Audio.hpp"
#include <iomanip>
#include <sstream>

namespace Hydrogen::Runtime {
namespace {
std::string jsonString(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << char(c);
        else if (c < 32) out << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
        else out << char(c);
    }
    return out.str() + '"';
}
}

void Process::recordThreadLiveness(const char* event) {
    std::string sample;
    {
        std::lock_guard<std::mutex> samplesLock(threadSamplesMu_);
        std::ostringstream out;
        const auto audio = mixer_->stats();
        out << "{\"event\":" << jsonString(event)
            << ",\"elapsedSeconds\":" << std::chrono::duration<double>(std::chrono::steady_clock::now() - threadSamplesStart_).count()
            << ",\"registerBoundary\":\"dispatch-or-HLE\",\"framesPresented\":" << perf_.framesPresented.load()
            << ",\"audioFramesRendered\":" << audio.framesRendered
            << ",\"audibleFrames\":" << audio.audibleFrames << ",\"threads\":[";
        std::unordered_map<uint32_t, uint64_t> nextCounts;
        bool comma = false;
        auto append = [&](const GuestThread& thread) {
            if (thread.finished.load(std::memory_order_acquire)) return;
            if (comma) out << ',';
            comma = true;
            CPU::ThreadProgressSnapshot state;
            const bool valid = thread.cpu.progress.snapshot(state);
            out << "{\"id\":" << thread.id << ",\"main\":" << (&thread == mainThread_ ? "true" : "false")
                << ",\"snapshotValid\":" << (valid ? "true" : "false");
            if (valid) {
                const auto previous = previousThreadCounts_.find(thread.id);
                const bool haveDelta = previous != previousThreadCounts_.end() && state.instructions >= previous->second;
                out << ",\"guestInstructions\":" << state.instructions << ",\"deltaInstructions\":";
                if (haveDelta) out << state.instructions - previous->second;
                else out << "null";
                out << ",\"noObservedInstructionProgress\":" << (haveDelta && state.instructions == previous->second ? "true" : "false")
                    << ",\"pc\":" << state.pc << ",\"lr\":" << state.lr << ",\"blockPc\":" << state.blockPc
                    << ",\"inGuest\":" << (state.inGuest ? "true" : "false")
                    << ",\"currentHle\":" << jsonString(state.hle == 0xFFFFFFFFu ? "" : hleFunctionName(state.hle))
                    << ",\"argument0\":" << state.argument0 << ",\"argument1\":" << state.argument1;
                nextCounts[thread.id] = state.instructions;
            }
            out << '}';
        };
        {
            std::lock_guard<std::mutex> threadsLock(threadsMu_);
            append(*mainThread_);
            for (const auto& [id, thread] : threads_) if (thread.get() != mainThread_) append(*thread);
        }
        previousThreadCounts_ = std::move(nextCounts);
        out << "]}";
        sample = out.str();
        if (threadSamples_.size() == 120) threadSamples_.pop_front();
        threadSamples_.push_back(sample);
    }
    // Persist while the process is alive, including a hang that never returns a
    // final report. Zero deltas are observations, not a deadlock diagnosis.
    log(LogLevel::Info, "[threads] " + sample);
}

std::string Process::threadLivenessJson() const {
    std::lock_guard<std::mutex> lock(threadSamplesMu_);
    std::string out = "[";
    for (const auto& sample : threadSamples_) {
        if (out.size() != 1) out += ',';
        out += sample;
    }
    return out + ']';
}
} // namespace Hydrogen::Runtime
