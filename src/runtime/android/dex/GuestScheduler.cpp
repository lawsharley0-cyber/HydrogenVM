#include "runtime/android/dex/GuestScheduler.hpp"
#include "runtime/Process.hpp"

#include <map>
#include <mutex>

namespace Hydrogen::Runtime::Android::Dex {
namespace {

class Scheduler {
public:
    explicit Scheduler(Process& process) : process_(process) {
        entry_ = process_.defineFunction("__hvm_dex_start_thread", [this](Ctx& x) {
            std::function<void()> run;
            {
                std::lock_guard lock(mutex_);
                const auto found = queued_.find(x.cpu.r[0]);
                if (found == queued_.end()) return;
                run = std::move(found->second);
                queued_.erase(found);
            }
            run();
            x.cpu.r[0] = 0;
        });
    }

    bool start(std::function<void()> work) {
        uint32_t id;
        {
            std::lock_guard lock(mutex_);
            if (!next_) return false;
            id = next_++;
            queued_.emplace(id, std::move(work));
        }
        if (process_.startThread(entry_, id, 512 * 1024, true)) return true;
        std::lock_guard lock(mutex_);
        queued_.erase(id);
        return false;
    }

private:
    Process& process_;
    uint32_t entry_, next_ = 1;
    std::mutex mutex_;
    std::map<uint32_t, std::function<void()>> queued_;
};

} // namespace

Machine::Scheduler guestScheduler(Process& process) {
    auto& scheduler = process.service<Scheduler>();
    return [&scheduler](std::function<void()> work) { return scheduler.start(std::move(work)); };
}

} // namespace Hydrogen::Runtime::Android::Dex
