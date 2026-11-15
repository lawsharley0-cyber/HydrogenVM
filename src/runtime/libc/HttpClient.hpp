// HydrogenVM — the host's HTTP client, for a game's Foundation downloads (NSURLConnection,
// NSURLSession, contents of a URL) when the player allowed it online.
//
// A game's own sockets already reach the internet (Net.cpp, with the TLS bridge for old TLS).
// Downloads made with Apple's URL loading never touch the game's sockets: they were answered
// "The Internet connection appears to be offline" even with the switch on, so ad SDKs, update
// checks and asset downloaders that use NSURLConnection never got anything. This interface
// gives them the host's own HTTP stack: NSURLSession on the iPhone (app/.../HttpClientApple.mm),
// OpenSSL on the desktop bench (HttpClientOpenSSL.cpp). The host follows redirects and
// decompresses gzip, as NSURLConnection does.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Hydrogen::Runtime {

struct HttpRequest {
    std::string method = "GET";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    double timeoutSeconds = 60;
};

struct HttpResponse {
    int status = 0;                 // 0: no response (see error)
    std::string url;                // after redirects
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    int errorCode = 0;              // an NSURLErrorDomain code when status is 0 (-1001 timed out, -1003 no host, ...)
    std::string error;              // for the play log and NSLocalizedDescription
};

struct HttpClient {
    virtual ~HttpClient() = default;
    // Runs the request on a host thread and calls `done` there (once).
    virtual void fetch(HttpRequest request, std::function<void(HttpResponse)> done) = 0;
};

// The desktop bench's client (OpenSSL); nullptr where it is not built.
std::shared_ptr<HttpClient> makeOpenSslHttpClient();

}  // namespace Hydrogen::Runtime
