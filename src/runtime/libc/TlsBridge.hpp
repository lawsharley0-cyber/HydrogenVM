// HydrogenVM — TLS bridge for games the player allowed online.
//
// Old games carry their own TLS library (Minion Rush: OpenSSL from 2013, TLS 1.0
// only). Servers refuse TLS 1.0 today, so the game's secure connections fail and it
// retries forever. With a bridge, a guest TCP connection to port 443 goes to a local
// port instead: the bridge accepts the game's old TLS there (with its own throwaway
// certificate; these games do not check certificates) and opens a modern TLS
// connection to the real server with the host's TLS library, checking the server's
// certificate. Bytes in between pass unchanged. No cipher code lives here: the host
// library (OpenSSL on Linux, Security.framework on Apple) does all of it.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace Hydrogen::Runtime {

struct TlsBridge {
    virtual ~TlsBridge() = default;
    // Starts a bridge for one connection to `host` (the name the game looked up, or the
    // dotted address) at `ipNetworkOrder`:`port`. Returns the local 127.0.0.1 port the
    // guest should connect to instead, or 0 when no bridge could be set up.
    virtual uint16_t open(const std::string& host, uint32_t ipNetworkOrder, uint16_t port,
                          std::function<void(const std::string&)> log) = 0;
};

// Linux with OpenSSL 3; nullptr elsewhere (`why` says why). verifyServer=false is for
// tests against a local server with a self-signed certificate.
std::shared_ptr<TlsBridge> makeOpenSslTlsBridge(bool verifyServer, std::string* why);

}  // namespace Hydrogen::Runtime
