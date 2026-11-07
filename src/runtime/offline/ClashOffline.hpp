#pragma once
#include <memory>
#include <cstdint>
namespace Hydrogen::Runtime { class Process; }
namespace Hydrogen::Runtime::Offline {
// Session-owned localhost service. No Python, external network, guest patching
// or fake turn/checksum acknowledgement. Only the exact pinned executable.
class ClashOffline {
public:
    explicit ClashOffline(Process&);
    ~ClashOffline();
    uint16_t port() const;
    void stop();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
