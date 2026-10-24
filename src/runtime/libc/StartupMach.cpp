// In-process Mach facilities for startup code. No host ports or exception delivery.
#include "runtime/Process.hpp"
#include "MachPorts.hpp"
#include <deque>
#include <chrono>

namespace Hydrogen::Runtime {
namespace {
bool range(Ctx& x, uint32_t address, uint64_t size, uint8_t access) {
    if (!size) return true;
    const uint64_t end = uint64_t(address) + size;
    if (end > (1ull << 32)) return false;
    for (uint64_t page = address & ~4095u; page < end; page += 4096)
        if ((x.mem.protAt(uint32_t(page)) & access) != access) return false;
    return true;
}
bool task(Ctx& x, uint32_t name) { return name == kTaskPort && hasMachSendRight(x.proc, name); }
struct IPC {
    explicit IPC(Process&) {}
    std::mutex mu;
    std::condition_variable cv;
    uint32_t next = 0x20000;
    std::unordered_map<uint32_t, std::deque<std::vector<uint8_t>>> receive;
};
int32_t allocate(Ctx& x, uint32_t owner, uint32_t right, GuestPtr out) {
    if (!task(x, owner)) return 16;
    if (!range(x, out, 4, Memory::kProtWrite)) return 1;
    x.mem.write<uint32_t>(out, 0);
    if (right != 1) return 46; // Only receive-right allocation is implemented.
    auto& ipc = x.proc.service<IPC>(); std::lock_guard lock(ipc.mu);
    if (ipc.receive.size() >= 4096) return 6;
    const auto name = ipc.next++;
    ipc.receive.emplace(name, std::deque<std::vector<uint8_t>>{});
    x.mem.write<uint32_t>(out, name); return 0;
}
int32_t insert(Ctx& x, uint32_t owner, uint32_t name, uint32_t source, uint32_t disposition) {
    if (!task(x, owner)) return 16;
    auto& ipc = x.proc.service<IPC>(); std::lock_guard lock(ipc.mu);
    if (!ipc.receive.count(name)) return 15;
    if (source != name || disposition != 20) return 46; // MACH_MSG_TYPE_MAKE_SEND
    return acquireMachSendRight(x.proc, name) ? 0 : 19;
}
int32_t destroy(Ctx& x, uint32_t owner, uint32_t name) {
    if (!task(x, owner)) return 16;
    auto& ipc = x.proc.service<IPC>(); std::lock_guard lock(ipc.mu);
    if (!ipc.receive.erase(name)) return 15;
    const auto refs = machSendRefs(x.proc, name);
    if (refs) modifyMachSendRefs(x.proc, owner, name, 0, -int32_t(refs));
    ipc.cv.notify_all(); return 0;
}
int32_t message(Ctx& x, GuestPtr buffer, uint32_t options, uint32_t sendSize, uint32_t receiveSize,
                uint32_t receiveName, uint32_t timeout, uint32_t) {
    auto& ipc = x.proc.service<IPC>();
    std::unique_lock lock(ipc.mu);
    if (options & 1) { // MACH_SEND_MSG. Complex messages/descriptors are unsupported.
        if (sendSize < 24 || sendSize > 65536 || !range(x, buffer, sendSize, Memory::kProtRead)) return 0x10000002;
        const uint32_t bits = x.mem.read<uint32_t>(buffer), length = x.mem.read<uint32_t>(buffer + 4);
        const uint32_t destination = x.mem.read<uint32_t>(buffer + 8);
        if ((bits & 0x80000000) || length < 24 || length > sendSize) return 0x10000010;
        if ((bits & 0xff) != 19 || x.mem.read<uint32_t>(buffer + 12)) return 0x1000000a; // COPY_SEND, no reply-right transfer
        auto it = ipc.receive.find(destination);
        if (it == ipc.receive.end() || !hasMachSendRight(x.proc, destination)) return 0x10000003;
        if (it->second.size() >= 64) return 0x10000004;
        it->second.emplace_back(x.mem.host(buffer), x.mem.host(buffer) + length);
        ipc.cv.notify_all();
    }
    if (!(options & 2)) return 0;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
    for (;;) {
        auto it = ipc.receive.find(receiveName);
        if (it == ipc.receive.end()) return 0x10004002;
        if (!it->second.empty()) {
            const auto& msg = it->second.front();
            if (msg.size() > receiveSize) return 0x10004004;
            if (!range(x, buffer, msg.size(), Memory::kProtWrite)) return 0x10004001;
            x.mem.copyIn(buffer, msg.data(), msg.size());
            // Receive headers expose the destination as the local port, and the reply as remote.
            const auto reply = x.mem.read<uint32_t>(buffer + 12);
            const auto bits = x.mem.read<uint32_t>(buffer);
            x.mem.write<uint32_t>(buffer + 8, reply);
            x.mem.write<uint32_t>(buffer + 12, receiveName);
            x.mem.write<uint32_t>(buffer, (bits >> 8) & 0xff);
            it->second.pop_front(); return 0;
        }
        if (x.proc.stopping()) return 0x10004005;
        if ((options & 0x100) && std::chrono::steady_clock::now() >= end) return 0x10004003;
        ipc.cv.wait_for(lock, std::chrono::milliseconds(10));
    }
}
int32_t exceptionPorts(Ctx& x, uint32_t target, uint32_t, uint32_t port, int32_t, int32_t,
                       GuestPtr, GuestPtr count, GuestPtr, GuestPtr, GuestPtr) {
    if (!task(x, target) && !(target >= 0x1000 && hasMachSendRight(x.proc, target) && x.proc.findThreadById(target - 0x1000))) return 4;
    if (port && !hasMachSendRight(x.proc, port)) return 15;
    if (!range(x, count, 4, Memory::kProtWrite)) return 1;
    x.mem.write<uint32_t>(count, 0);
    // Deliberate Mono compatibility: accept setup with an empty prior-handler list.
    // Guest faults still use Engine diagnostics; they are never sent as Mach exceptions.
    x.proc.reportMissing("Mach exception delivery (unavailable; setup accepted)");
    return 0;
}
int32_t map(Ctx& x, uint32_t owner, GuestPtr out, uint32_t size, uint32_t mask, uint32_t flags,
            uint32_t object, uint64_t, bool, uint32_t current, uint32_t maximum, uint32_t) {
    if (!task(x, owner)) return 16;
    if (!range(x, out, 4, Memory::kProtWrite)) return 1;
    if (!size || size > 0xfffff000 || (current | maximum) & ~7u || (current & ~maximum)) return 4;
    if (object || flags != 1 || !current || maximum != 7) return 46; // Anonymous, anywhere; no maximum-protection tracking.
    const uint64_t alignment = uint64_t(mask) + 1;
    if ((alignment & (alignment - 1)) || alignment > 0x40000000) return 4;
    const uint32_t rounded = (size + 4095) & ~4095u;
    const auto address = x.proc.heap().allocAligned(rounded, std::max(uint32_t(alignment), 4096u));
    if (!address) return 3;
    x.mem.protect(address, rounded, uint8_t(current));
    x.mem.write<uint32_t>(out, address); return 0;
}
int32_t protect(Ctx& x, uint32_t owner, uint32_t address, uint32_t size, bool maximum, uint32_t protection) {
    if (!task(x, owner)) return 16;
    if (protection & ~7u) return 4;
    // GuestArena has no maximum-protection or inaccessible-page metadata yet.
    if (maximum || !protection) return 46;
    if (!range(x, address, size, Memory::kProtRead)) return 1;
    x.mem.protect(address, size, uint8_t(protection)); return 0;
}
}
void registerStartupMach(Process& p) {
    p.def<&allocate>("_mach_port_allocate");
    p.def<&insert>("_mach_port_insert_right");
    p.def<&destroy>("_mach_port_destroy");
    p.def<&message>("_mach_msg");
    p.def<&exceptionPorts>("_task_swap_exception_ports");
    p.def<&exceptionPorts>("_thread_swap_exception_ports");
    p.def<&exceptionPorts>("_task_set_exception_ports");
    p.def<&exceptionPorts>("_thread_set_exception_ports");
    p.def<&map>("_vm_map");
    p.def<&protect>("_vm_protect");
}
}
