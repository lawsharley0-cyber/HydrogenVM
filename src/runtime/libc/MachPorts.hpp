#pragma once
#include "runtime/Process.hpp"

namespace Hydrogen::Runtime {
// The local namespace contains send rights for synthetic kernel objects only.
// No receive rights or IPC endpoints are implied by these names.
constexpr uint32_t kTaskPort = 0x103, kHostPort = 0x203;
uint32_t acquireMachSendRight(Process& p, uint32_t name);
bool hasMachSendRight(Process& p, uint32_t name);
int32_t modifyMachSendRefs(Process& p, uint32_t task, uint32_t name, uint32_t right, int32_t delta);
uint32_t machSendRefs(Process& p, uint32_t name);
}
