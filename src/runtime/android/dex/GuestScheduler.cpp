#include "runtime/android/dex/GuestScheduler.hpp"
#include "runtime/Support.hpp"

#include <map>
#include <mutex>

namespace Hydrogen::Runtime::Android::Dex {
namespace {

class Scheduler;
uint32_t runQueued(Ctx& x, uint32_t id);

class Scheduler {
public:
    explicit Scheduler(Process& process) : process_(process) {}

    bool start(std::function<void()> work) {
        std::call_once(registerEntry_, [&] {
            entry_ = supportDef<&runQueued>(process_, "__hvm_dex_start_thread", SupportStatus::Real,
                                           "runs a queued Java thread through the Process worker");
        });
        uint32_t id;
        {
            std::lock_guard lock(mutex_);
            if (!next_) return false;
            id = next_++;
            queued_.emplace(id, std::move(work));
        }
        try {
            if (process_.startThread(entry_, id, 512 * 1024, true)) return true;
        } catch (...) {
            abandon(id);
            throw;
        }
        abandon(id);
        return false;
    }

    void run(uint32_t id) {
        std::function<void()> work;
        {
            std::lock_guard lock(mutex_);
            const auto found = queued_.find(id);
            if (found == queued_.end()) return;
            work = std::move(found->second);
            queued_.erase(found);
        }
        work();
    }

private:
    void abandon(uint32_t id) {
        std::lock_guard lock(mutex_);
        queued_.erase(id);
    }

    Process& process_;
    std::once_flag registerEntry_;
    uint32_t entry_ = 0, next_ = 1;
    std::mutex mutex_;
    std::map<uint32_t, std::function<void()>> queued_;
};

uint32_t runQueued(Ctx& x, uint32_t id) {
    x.proc.service<Scheduler>().run(id);
    return 0;
}

} // namespace

Machine::Scheduler guestScheduler(Process& process) {
    auto& scheduler = process.service<Scheduler>();
    return [&scheduler](std::function<void()> work) { return scheduler.start(std::move(work)); };
}

} // namespace Hydrogen::Runtime::Android::Dex
