#pragma once
#include "runtime/android/dex/Machine.hpp"

namespace Hydrogen::Runtime {
class Process;
}

namespace Hydrogen::Runtime::Android::Dex {

// Bind Java Thread.start to the same Process worker path as Android pthread_create.
Machine::Scheduler guestScheduler(Process& process);

} // namespace Hydrogen::Runtime::Android::Dex
