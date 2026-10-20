#include "MachPorts.hpp"

namespace Hydrogen::Runtime {
namespace {
struct Ports {
    explicit Ports(Process&) {}
    std::mutex mu;
    std::unordered_map<uint32_t, uint32_t> refs{{kTaskPort, 1}};
};
}
uint32_t acquireMachSendRight(Process& p, uint32_t name) {
    auto& s = p.service<Ports>();
    std::lock_guard lock(s.mu);
    auto& n = s.refs[name];
    if (n == 65535) return 0;
    ++n;
    return name;
}
uint32_t machSendRefs(Process& p, uint32_t name) {
    auto& s = p.service<Ports>();
    std::lock_guard lock(s.mu);
    auto it = s.refs.find(name);
    return it == s.refs.end() ? 0 : it->second;
}
bool hasMachSendRight(Process& p, uint32_t name) { return machSendRefs(p, name) != 0; }
int32_t modifyMachSendRefs(Process& p, uint32_t task, uint32_t name, uint32_t right, int32_t delta) {
    auto& s = p.service<Ports>();
    std::lock_guard lock(s.mu);
    if (task != kTaskPort || !s.refs[kTaskPort]) return 16; // KERN_INVALID_TASK
    if (right > 4) return 18; // KERN_INVALID_VALUE
    auto it = s.refs.find(name);
    if (it == s.refs.end() || !it->second) return 15; // KERN_INVALID_NAME
    if (right != 0) return 17; // KERN_INVALID_RIGHT: synthetic objects have send rights only
    const int64_t n = int64_t(it->second) + delta;
    if (n < 0) return 18;
    if (n > 65535) return 19; // KERN_UREFS_OVERFLOW
    it->second = uint32_t(n);
    return 0;
}
}
