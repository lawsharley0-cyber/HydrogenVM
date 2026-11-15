// HydrogenVM — stand-in servers for games whose own servers were shut down.
//
// Some games will not start without their server: Groove Coaster Zero downloads its start
// data from gczero.com, which Taito closed years ago, and sits on a black screen without it.
// A stand-in answers those addresses from inside HydrogenVM, through every way a game
// downloads (NSData / NSString contents of a URL, NSXMLParser, NSURLConnection sync and
// async), whether or not the game's "Allow internet" switch is on: the real server is gone
// either way, so nothing leaves the phone.
//
// The replies were worked out by watching what the game looks for in them (the element
// names it compares against), not from the game's code. A request a stand-in has no reply
// for gets an empty "OK" and a Defaulted line, so the missing-API ledger lists it.
//
// For working out new replies, HVM_STANDIN_DIR=folder serves folder/<host>/<path> files
// first (any host), so a reply can be tried without rebuilding.
#include "Foundation.hpp"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>

namespace Hydrogen::Runtime::Foundation {
namespace {

struct UrlParts { std::string host, path, query; };
UrlParts split(const std::string& url) {
    UrlParts u;
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return u;
    std::string rest = url.substr(scheme + 3);
    const size_t slash = rest.find('/');
    u.host = rest.substr(0, slash);
    if (const size_t at = u.host.rfind('@'); at != std::string::npos) u.host = u.host.substr(at + 1);
    if (const size_t colon = u.host.find(':'); colon != std::string::npos) u.host = u.host.substr(0, colon);
    std::transform(u.host.begin(), u.host.end(), u.host.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (u.host.rfind("www.", 0) == 0) u.host = u.host.substr(4);
    std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
    if (const size_t q = path.find('?'); q != std::string::npos) { u.query = path.substr(q + 1); path = path.substr(0, q); }
    if (const size_t h = path.find('#'); h != std::string::npos) path = path.substr(0, h);
    u.path = path;
    return u;
}

std::string xmlReply(const std::string& body) { return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<response>" + body + "</response>\n"; }

// ---- Groove Coaster Zero (Taito, 2012): gczero.com. Request parameters are encrypted by the
// game, so replies depend on the path only. code 0 is success.
std::optional<std::string> grooveZero(const UrlParts& u) {
    const std::string& p = u.path;
    // start.php and event.php: the game looks these up in both.
    const char* account = "<code>0</code><message>OK</message><en>0</en><ja>0</ja><my_coin>0</my_coin><event_id>0</event_id>"
                          "<taito_id>0</taito_id><m4a_path>0</m4a_path><is_tutorial>0</is_tutorial>";
    if (p == "/start.php" || p == "/event.php") return xmlReply(account);
    // The shop: coin packs, songs (stage) and avatars for sale, each a list of <item>s. Nothing
    // is for sale; the game's own songs are in its bundle.
    if (p == "/shop_list.php") return xmlReply("<code>0</code><coin></coin><stage></stage><avatar></avatar>");
    return std::nullopt;
}

struct StandIn { const char* host; const char* game; std::optional<std::string> (*reply)(const UrlParts&); };
const StandIn kStandIns[] = {
    {"gczero.com", "Groove Coaster Zero", &grooveZero},
    {"staging.gczero.com", "Groove Coaster Zero", &grooveZero},
};

struct StandInLog { explicit StandInLog(Process&) {} std::mutex mu; std::set<std::string> told; };

}  // namespace

std::optional<StandInReply> standInFetch(Ctx& x, const std::string& url, const std::string& method) {
    const UrlParts u = split(url);
    if (u.host.empty()) return std::nullopt;
    auto& log = x.proc.service<StandInLog>();
    auto once = [&](const std::string& key, const std::string& line) {
        std::lock_guard<std::mutex> lk(log.mu);
        if (log.told.insert(key).second) x.proc.log(LogLevel::Info, line);
    };
    if (const char* dir = std::getenv("HVM_STANDIN_DIR")) {
        std::ifstream in(std::string(dir) + "/" + u.host + u.path, std::ios::binary);
        if (in) {
            StandInReply r;
            r.body.assign(std::istreambuf_iterator<char>(in), {});
            r.mime = u.path.size() > 4 && u.path.compare(u.path.size() - 4, 4, ".png") == 0 ? "image/png" : "text/xml";
            once(u.host + u.path, "Stand-in server: " + method + " " + u.host + u.path + " answered from HVM_STANDIN_DIR");
            return r;
        }
    }
    for (const StandIn& s : kStandIns) {
        if (u.host != s.host) continue;
        StandInReply r;
        if (auto body = s.reply(u)) {
            r.body = std::move(*body);
            once(u.host + u.path, "Stand-in server: " + method + " " + u.host + u.path + " answered by HydrogenVM (" + s.game +
                                      "'s server was shut down)");
        } else {
            r.body = xmlReply("<code>0</code>");
            x.proc.reportDefaulted("stand-in server " + u.host + " has no reply for " + u.path + " (answered an empty OK)");
        }
        return r;
    }
    return std::nullopt;
}

std::optional<StandInReply> standInFetch(Ctx& x, id url, const std::string& method) {
    if (!url) return std::nullopt;
    return standInFetch(x, urlString(x, url), method);
}

id standInResponse(Ctx& x, id url, const StandInReply& r) {
    ObjC::Runtime& rt = ObjC::rt(x);
    const id headers = makeDict(x, {{makeString(x, "Content-Type"), makeString(x, r.mime)},
                                    {makeString(x, "Content-Length"), makeString(x, std::to_string(r.body.size()))}});
    const id o = id(rt.send(x.cpu, rt.getClass("NSHTTPURLResponse"), rt.selAlloc));
    const id resp = id(rt.send(x.cpu, o, "initWithURL:statusCode:HTTPVersion:headerFields:",
                               {url, uint32_t(r.status), makeString(x, "HTTP/1.1"), headers}));
    return autoreleased(x, resp);
}

}  // namespace Hydrogen::Runtime::Foundation
