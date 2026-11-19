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
#include "runtime/compat/Profiles.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
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

// A reply that makes the request fail the way an unreachable server does (no connection).
constexpr const char* kUnreachable = "\x01unreachable";

std::string xmlReply(const std::string& body) { return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<response>" + body + "</response>\n"; }

// A zip holding one file, stored (no compression): what a download that is a zip of one song
// looks like, made from a file the player supplied.
uint32_t crc32Of(const std::string& b) {
    static uint32_t table[256];
    static bool made = false;
    if (!made) {
        for (uint32_t i = 0; i < 256; ++i) { uint32_t c = i; for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; table[i] = c; }
        made = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char ch : b) c = table[(c ^ ch) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
std::string storedZip(const std::string& name, const std::string& data) {
    std::string z;
    auto u16 = [&](uint32_t v) { z += char(v & 0xFF); z += char((v >> 8) & 0xFF); };
    auto u32 = [&](uint32_t v) { u16(v & 0xFFFF); u16(v >> 16); };
    const uint32_t crc = crc32Of(data), size = uint32_t(data.size());
    const uint16_t dosTime = 0, dosDate = (32 << 9) | (1 << 5) | 1;   // 2012-01-01
    u32(0x04034b50); u16(10); u16(0); u16(0); u16(dosTime); u16(dosDate); u32(crc); u32(size); u32(size); u16(uint32_t(name.size())); u16(0);
    z += name; z += data;
    const uint32_t central = uint32_t(z.size());
    u32(0x02014b50); u16(20); u16(10); u16(0); u16(0); u16(dosTime); u16(dosDate); u32(crc); u32(size); u32(size);
    u16(uint32_t(name.size())); u16(0); u16(0); u16(0); u16(0); u32(0); u32(0);
    z += name;
    const uint32_t centralSize = uint32_t(z.size()) - central;
    u32(0x06054b50); u16(0); u16(0); u16(1); u16(1); u32(centralSize); u32(central); u16(0);
    return z;
}
std::optional<std::string> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(in), {});
}
// The player's files for a stand-in live in "Stand-in Files/<host>/" at the top of HydrogenVM's
// Documents (Files app: On My iPhone > HydrogenVM > Stand-in Files > gczero.com), two levels
// above a game's home (Documents/Apps/<bundle id>/). Not in the game's own Documents: Groove
// Coaster Zero clears that at launch. A file named as the game asks ("plot3_bgm.m4a.zip") is
// sent as it is; a bare "plot3_bgm.m4a" is sent inside a stored zip under that name. Neither:
// unreachable, and the log says where to put it.
std::optional<std::string> songFile(Ctx& x, const std::string& zipName) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path base = fs::weakly_canonical(fs::path(x.proc.homePath()) / ".." / "..", ec);
    if (ec) base = fs::path(x.proc.homePath()) / ".." / "..";
    const std::string dir = (base / "Stand-in Files" / "gczero.com").string() + "/";
    if (auto z = readFile(dir + zipName)) return z;
    const std::string inner = zipName.size() > 4 && zipName.compare(zipName.size() - 4, 4, ".zip") == 0 ? zipName.substr(0, zipName.size() - 4) : zipName;
    if (auto f = readFile(dir + inner)) {
        x.proc.logf(LogLevel::Info, "Stand-in server: sent %s as %s (stored zip of %zu bytes)", inner.c_str(), zipName.c_str(), f->size());
        return storedZip(inner, *f);
    }
    x.proc.logf(LogLevel::Info, "Stand-in server: no song file %s (or %s) in %s", zipName.c_str(), inner.c_str(), dir.c_str());
    return std::string(kUnreachable);
}

// ---- Groove Coaster Zero (Taito, 2012): gczero.com. Request parameters are encrypted by the
// game, so replies depend on the path only. code 0 is success.
std::optional<std::string> grooveZero(Ctx& x, const UrlParts& u) {
    const std::string& p = u.path;
    // Song music: the game builds <m4a_path><name>.m4a.zip ("plot3_bgm.m4a.zip"). The music was
    // only ever on Taito's server; the player supplies it (see songFile).
    if (p.rfind("/bgm/", 0) == 0) return songFile(x, p.substr(5));
    // start.php: the account (coins, current event, tutorial done), then the data update: for
    // each of stage_pak / model_pak / tuneFile_pak a <date> and a <url>. When a date differs from
    // the one in Library/Caches/version.cfg the game downloads <url> to Library/Caches/<name>
    // (it looks there before its bundle) and then writes version.cfg. The song menu stays dim
    // and ignores taps until version.cfg exists, so the first launch has to go through one update.
    // The original update files are gone; the stand-in sends the game's own bundled paks
    // (unchanged copies), dated with the build ("ver:20120920-A"), so the update is a no-op.
    const char* account = "<code>0</code><message>OK</message><en>0</en><ja>0</ja><my_coin>0</my_coin><event_id>0</event_id>"
                          "<taito_id>0</taito_id><m4a_path>http://gczero.com/bgm/</m4a_path><is_tutorial>0</is_tutorial>";
    if (p == "/start.php") {
        std::string body = account;
        for (const char* pak : {"stage", "model", "tuneFile"})
            body += std::string("<") + pak + "_pak><date>20120920</date><url>http://gczero.com/pak/" + pak + ".pak</url></" + pak + "_pak>";
        return xmlReply(body);
    }
    if (p == "/pak/stage.pak" || p == "/pak/model.pak" || p == "/pak/tuneFile.pak") {
        if (auto f = readFile(x.proc.bundlePath() + "/" + p.substr(5))) return f;
        return std::string(kUnreachable);
    }
    // event.php: no event is running. The data update above is only checked after event.php
    // succeeds (when it is unreachable the game skips the update, and the menu stays dim), so it
    // is answered with a plain success; the game reads no event fields from it.
    if (p == "/event.php") return xmlReply("<code>0</code><message>OK</message>");
    // sync.php (after TOUCH TO START): the player's saved state. No coins, friends or bought
    // songs; the game's own songs are in its bundle.
    if (p == "/sync.php")
        return xmlReply("<code>0</code><my_coin>0</my_coin><friend_num>0</friend_num><m4a_path>http://gczero.com/bgm/</m4a_path><cm_stage></cm_stage>"
                        "<buy_id></buy_id><play_id></play_id><friend_info></friend_info>");
    // The shop (coin packs, songs and avatars for sale, each a list of <item>s) is "unreachable":
    // nothing can be bought. An empty success also works now that the data update above runs
    // (before it, any shop reply ended on a "Download" screen), but unreachable is what the game
    // was seen to handle on the phone.
    if (p == "/shop_list.php") return std::string(kUnreachable);
    return std::nullopt;
}

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
    if (const auto* profile = Compat::serverProfile(u.host)) {
        Compat::noteApplied(x.proc, profile->kind);
        const std::string game = "Groove Coaster Zero";
        StandInReply r;
        if (auto body = grooveZero(x, u)) {
            if (*body == kUnreachable) {
                once(u.host + u.path, "Stand-in server: " + method + " " + u.host + u.path + " is answered as unreachable (" + game +
                                          " carries on without it)");
                return std::nullopt;
            }
            r.body = std::move(*body);
            const bool php = u.path.size() >= 4 && u.path.compare(u.path.size() - 4, 4, ".php") == 0;
            if (!php) r.mime = "application/octet-stream";   // pak, zip
            once(u.host + u.path, "Stand-in server: " + method + " " + u.host + u.path + " answered by HydrogenVM (" + game +
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
