// T2a (#257): one record per missing or approximated host API that a guest called.
//
// Before this, a missing API was logged once per name and then forgotten: no count,
// no caller, no way to tell a run that hit nothing from one that limped past dozens
// of stubs. `MissingApiLog` keeps, per name, how often it was called, when (seconds
// since launch) and from where (the guest return address, plus a short backtrace of
// the first hit). A run is "tainted" from the first hit on, so a clean-looking exit
// from such a run is not read as a compatibility pass.
//
// Header-only, no dependency on Process: Process::reportMissing feeds it and the
// run report serialises it with reportFields().
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Hydrogen::Runtime {

struct MissingApiRecord {
    std::string name;
    uint64_t count = 0;
    double firstSeconds = 0;               // seconds since launch at the first hit
    double lastSeconds = 0;                // ... at the latest hit
    uint32_t callerPc = 0;                 // guest return address (LR) at the first hit, 0 when unknown
    std::string caller;                    // that address as image+offset and symbol
    std::vector<std::string> backtrace;    // first hit, innermost frame first
    uint64_t order = 0;                    // first-hit sequence number
};

class MissingApiLog {
public:
    static constexpr size_t kMaxFrames = 12;     // backtrace lines kept per API
    static constexpr size_t kMaxNames = 2048;    // distinct APIs kept; later names are only counted

    // What the first hit of an API records about where it was called from.
    struct Site {
        uint32_t callerPc = 0;
        std::string caller;
        std::vector<std::string> backtrace;
    };

    // Count one call of `name` made `secondsSinceLaunch` after launch. `site` fills in the
    // Site and runs at most once per name, on its first hit, while this log is locked: it
    // must not call back into the log. Any thread.
    template <class SiteFn>
    void hit(const std::string& name, double secondsSinceLaunch, SiteFn&& site) {
        std::lock_guard<std::mutex> lk(mu_);
        ++totalHits_;
        auto it = byName_.find(name);
        if (it == byName_.end()) {
            if (byName_.size() >= kMaxNames) { ++overflowHits_; return; }
            MissingApiRecord r;
            r.name = name;
            r.firstSeconds = secondsSinceLaunch;
            r.order = nextOrder_++;
            Site s;
            site(s);
            r.callerPc = s.callerPc;
            r.caller = std::move(s.caller);
            r.backtrace = std::move(s.backtrace);
            if (r.backtrace.size() > kMaxFrames) r.backtrace.resize(kMaxFrames);
            it = byName_.emplace(name, std::move(r)).first;
        }
        ++it->second.count;
        it->second.lastSeconds = secondsSinceLaunch;
    }

    // True from the first hit on.
    bool tainted() const { std::lock_guard<std::mutex> lk(mu_); return totalHits_ > 0; }
    // Every call, including those of names past kMaxNames.
    uint64_t totalHits() const { std::lock_guard<std::mutex> lk(mu_); return totalHits_; }
    // Calls of names that did not fit under kMaxNames (counted in totalHits, no record).
    uint64_t overflowHits() const { std::lock_guard<std::mutex> lk(mu_); return overflowHits_; }
    size_t distinct() const { std::lock_guard<std::mutex> lk(mu_); return byName_.size(); }

    // Records in the order the names were first hit.
    std::vector<MissingApiRecord> records() const {
        std::vector<MissingApiRecord> out;
        {
            std::lock_guard<std::mutex> lk(mu_);
            out.reserve(byName_.size());
            for (const auto& kv : byName_) out.push_back(kv.second);
        }
        std::sort(out.begin(), out.end(), [](const MissingApiRecord& a, const MissingApiRecord& b) { return a.order < b.order; });
        return out;
    }

    // The fields `hvm run --report` adds to its JSON object, each on its own line and
    // prefixed with `indent`, with no trailing comma or newline.
    std::string reportFields(const std::string& indent = "  ") const {
        const std::vector<MissingApiRecord> recs = records();
        uint64_t total, overflow;
        { std::lock_guard<std::mutex> lk(mu_); total = totalHits_; overflow = overflowHits_; }
        std::string o;
        o += indent + "\"tainted\": " + (total > 0 ? "true" : "false") + ",\n";
        o += indent + "\"stubHits\": " + std::to_string(total) + ",\n";
        o += indent + "\"missingApisOverflow\": " + std::to_string(overflow) + ",\n";
        o += indent + "\"missingApis\": [";
        for (size_t i = 0; i < recs.size(); ++i) {
            const MissingApiRecord& r = recs[i];
            char num[96];
            o += i ? ",\n" : "\n";
            o += indent + "  {\"name\": \"" + jsonEscape(r.name) + "\", \"count\": " + std::to_string(r.count);
            std::snprintf(num, sizeof num, ", \"firstSeconds\": %.6f, \"lastSeconds\": %.6f", r.firstSeconds, r.lastSeconds);
            o += num;
            o += ", \"callerPc\": " + std::to_string(r.callerPc) + ", \"caller\": \"" + jsonEscape(r.caller) + "\", \"backtrace\": [";
            for (size_t k = 0; k < r.backtrace.size(); ++k) o += std::string(k ? ", \"" : "\"") + jsonEscape(r.backtrace[k]) + "\"";
            o += "]}";
        }
        o += recs.empty() ? "]" : "\n" + indent + "]";
        return o;
    }

    // Process::backtrace text ("    frame\n" per line) as one trimmed entry per frame.
    static std::vector<std::string> splitLines(const std::string& text) {
        std::vector<std::string> out;
        size_t pos = 0;
        while (pos < text.size()) {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos) end = text.size();
            size_t b = pos, e = end;
            while (b < e && (text[b] == ' ' || text[b] == '\t')) ++b;
            while (e > b && (text[e - 1] == ' ' || text[e - 1] == '\t' || text[e - 1] == '\r')) --e;
            if (e > b) out.push_back(text.substr(b, e - b));
            pos = end + 1;
        }
        return out;
    }

    // JSON string contents. Guest-supplied names can hold any bytes, and a report that is not
    // valid UTF-8 is discarded whole by its readers, so a byte that is not part of a valid
    // UTF-8 sequence becomes '?'.
    static std::string jsonEscape(const std::string& in) {
        std::string o;
        o.reserve(in.size() + 8);
        const size_t n = in.size();
        for (size_t i = 0; i < n;) {
            const unsigned char c = static_cast<unsigned char>(in[i]);
            if (c < 0x80) {
                switch (c) {
                    case '"': o += "\\\""; break;
                    case '\\': o += "\\\\"; break;
                    case '\n': o += "\\n"; break;
                    case '\r': o += "\\r"; break;
                    case '\t': o += "\\t"; break;
                    default:
                        if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                        else o += static_cast<char>(c);
                }
                ++i;
                continue;
            }
            const size_t len = utf8Length(in, i);
            if (len) { o.append(in, i, len); i += len; }
            else { o += '?'; ++i; }
        }
        return o;
    }

private:
    // Length of the valid multi-byte UTF-8 sequence starting at in[i], or 0 if there is none.
    static size_t utf8Length(const std::string& in, size_t i) {
        const size_t n = in.size();
        const unsigned char c = static_cast<unsigned char>(in[i]);
        size_t len = 0;
        if (c >= 0xC2 && c <= 0xDF) len = 2;
        else if (c >= 0xE0 && c <= 0xEF) len = 3;
        else if (c >= 0xF0 && c <= 0xF4) len = 4;
        else return 0;
        if (i + len > n) return 0;
        for (size_t k = 1; k < len; ++k)
            if ((static_cast<unsigned char>(in[i + k]) & 0xC0) != 0x80) return 0;
        const unsigned char c1 = static_cast<unsigned char>(in[i + 1]);
        if (c == 0xE0 && c1 < 0xA0) return 0;    // overlong
        if (c == 0xED && c1 > 0x9F) return 0;    // surrogate
        if (c == 0xF0 && c1 < 0x90) return 0;    // overlong
        if (c == 0xF4 && c1 > 0x8F) return 0;    // beyond U+10FFFF
        return len;
    }

    mutable std::mutex mu_;
    std::unordered_map<std::string, MissingApiRecord> byName_;
    uint64_t totalHits_ = 0;
    uint64_t overflowHits_ = 0;
    uint64_t nextOrder_ = 0;
};

} // namespace Hydrogen::Runtime
