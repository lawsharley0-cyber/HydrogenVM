// HydrogenVM — libsqlite3 for guest apps, backed by the host's SQLite.
//
// Database and statement handles the guest sees are small guest-memory blocks
// that map to host sqlite3* / sqlite3_stmt*. Strings and blobs that SQLite
// returns (column_text, column_name, errmsg, ...) are copied into guest
// buffers owned by the handle and released when SQLite would invalidate them
// (the next step/reset/finalize). Bound text/blobs are copied immediately
// (SQLITE_TRANSIENT); a guest destructor, if given, runs right away.
#include "runtime/Process.hpp"
#include <cstring>
#include <mutex>
#include <sqlite3.h>
#include <unordered_map>
#include <vector>

namespace Hydrogen::Runtime {
namespace {

struct Stmt {
    sqlite3_stmt* s = nullptr;
    uint32_t db = 0;
    std::vector<uint32_t> buffers;   // guest copies handed out since the last step
};
struct Db {
    sqlite3* db = nullptr;
    uint32_t errBuf = 0;
};
struct Sql {
    explicit Sql(Process&) {}
    std::mutex mu;
    std::unordered_map<uint32_t, Db> dbs;
    std::unordered_map<uint32_t, Stmt> stmts;
    std::unordered_map<sqlite3_stmt*, uint32_t> byHost;
};
Sql& Q(Ctx& x) { return x.proc.service<Sql>(); }

Db* dbOf(Ctx& x, uint32_t h) { std::lock_guard<std::mutex> lk(Q(x).mu); auto it = Q(x).dbs.find(h); return it == Q(x).dbs.end() ? nullptr : &it->second; }
Stmt* stOf(Ctx& x, uint32_t h) { std::lock_guard<std::mutex> lk(Q(x).mu); auto it = Q(x).stmts.find(h); return it == Q(x).stmts.end() ? nullptr : &it->second; }

uint32_t guestCopy(Ctx& x, const void* p, size_t n, bool terminate) {
    const uint32_t g = x.proc.heap().alloc(uint32_t(n + (terminate ? 1 : 0)) + 1);
    if (n) std::memcpy(x.mem.host(g), p, n);
    if (terminate) x.mem.write<uint8_t>(g + uint32_t(n), 0);
    return g;
}
void dropBuffers(Ctx& x, Stmt& s) {
    for (uint32_t b : s.buffers) x.proc.heap().free(b);
    s.buffers.clear();
}
uint32_t keep(Ctx& x, Stmt& s, uint32_t g) { s.buffers.push_back(g); return g; }

int32_t openDb(Ctx& x, const std::string& path, GuestPtr out, int flags, bool v2) {
    sqlite3* db = nullptr;
    const int rc = v2 ? sqlite3_open_v2(path.c_str(), &db, flags, nullptr) : sqlite3_open(path.c_str(), &db);
    const uint32_t h = x.proc.heap().allocZeroed(16);
    {
        std::lock_guard<std::mutex> lk(Q(x).mu);
        Q(x).dbs[h].db = db;
    }
    x.mem.write<uint32_t>(out, h);
    return rc;
}
int32_t q_open(Ctx& x, GuestPtr path, GuestPtr out) { return openDb(x, x.proc.readCString(path), out, 0, false); }
int32_t q_openV2(Ctx& x, GuestPtr path, GuestPtr out, int32_t flags, GuestPtr) { return openDb(x, x.proc.readCString(path), out, flags, true); }
int32_t q_close(Ctx& x, uint32_t h) {
    Db* d = dbOf(x, h);
    if (!d) return SQLITE_OK;
    // Finalize statements the app left behind (sqlite3_close would fail with SQLITE_BUSY otherwise).
    std::vector<uint32_t> left;
    {
        std::lock_guard<std::mutex> lk(Q(x).mu);
        for (auto& [k, s] : Q(x).stmts) if (s.db == h) left.push_back(k);
    }
    for (uint32_t k : left) {
        Stmt* s = stOf(x, k);
        if (!s) continue;
        sqlite3_finalize(s->s);
        dropBuffers(x, *s);
        std::lock_guard<std::mutex> lk(Q(x).mu);
        Q(x).byHost.erase(s->s);
        Q(x).stmts.erase(k);
    }
    const int rc = sqlite3_close(d->db);
    if (d->errBuf) x.proc.heap().free(d->errBuf);
    {
        std::lock_guard<std::mutex> lk(Q(x).mu);
        Q(x).dbs.erase(h);
    }
    x.proc.heap().free(h);
    return rc;
}
uint32_t q_errmsg(Ctx& x, uint32_t h) {
    Db* d = dbOf(x, h);
    const char* m = d ? sqlite3_errmsg(d->db) : "out of memory";
    if (d && d->errBuf) x.proc.heap().free(d->errBuf);
    const uint32_t g = guestCopy(x, m, std::strlen(m), true);
    if (d) d->errBuf = g;
    return g;
}
int32_t q_errcode(Ctx& x, uint32_t h) { Db* d = dbOf(x, h); return d ? sqlite3_errcode(d->db) : SQLITE_MISUSE; }
int32_t q_changes(Ctx& x, uint32_t h) { Db* d = dbOf(x, h); return d ? sqlite3_changes(d->db) : 0; }
int64_t q_lastRowid(Ctx& x, uint32_t h) { Db* d = dbOf(x, h); return d ? sqlite3_last_insert_rowid(d->db) : 0; }
int32_t q_busyTimeout(Ctx& x, uint32_t h, int32_t ms) { Db* d = dbOf(x, h); return d ? sqlite3_busy_timeout(d->db, ms) : SQLITE_MISUSE; }

// sqlite3_prepare_v2(db, sql, nByte, &stmt, &tail)
int32_t q_prepare(Ctx& x, uint32_t h, GuestPtr sql, int32_t n, GuestPtr out, GuestPtr tail) {
    Db* d = dbOf(x, h);
    if (out) x.mem.write<uint32_t>(out, 0);
    if (!d) return SQLITE_MISUSE;
    const std::string text = n < 0 ? x.proc.readCString(sql) : std::string(reinterpret_cast<const char*>(x.mem.host(sql)), size_t(n));
    sqlite3_stmt* s = nullptr;
    const char* rest = nullptr;
    const int rc = sqlite3_prepare_v2(d->db, text.c_str(), int(text.size()), &s, &rest);
    if (tail) x.mem.write<uint32_t>(tail, sql + uint32_t(rest ? rest - text.c_str() : 0));
    if (rc != SQLITE_OK || !s) return rc;
    const uint32_t g = x.proc.heap().allocZeroed(16);
    {
        std::lock_guard<std::mutex> lk(Q(x).mu);
        Q(x).stmts[g] = Stmt{s, h, {}};
        Q(x).byHost[s] = g;
    }
    if (out) x.mem.write<uint32_t>(out, g);
    return rc;
}
int32_t q_step(Ctx& x, uint32_t h) {
    Stmt* s = stOf(x, h);
    if (!s) return SQLITE_MISUSE;
    dropBuffers(x, *s);
    return sqlite3_step(s->s);
}
int32_t q_reset(Ctx& x, uint32_t h) { Stmt* s = stOf(x, h); if (!s) return SQLITE_OK; dropBuffers(x, *s); return sqlite3_reset(s->s); }
int32_t q_clearBindings(Ctx& x, uint32_t h) { Stmt* s = stOf(x, h); return s ? sqlite3_clear_bindings(s->s) : SQLITE_MISUSE; }
int32_t q_finalize(Ctx& x, uint32_t h) {
    Stmt* s = stOf(x, h);
    if (!s) return SQLITE_OK;
    const int rc = sqlite3_finalize(s->s);
    dropBuffers(x, *s);
    {
        std::lock_guard<std::mutex> lk(Q(x).mu);
        Q(x).byHost.erase(s->s);
        Q(x).stmts.erase(h);
    }
    x.proc.heap().free(h);
    return rc;
}
uint32_t q_nextStmt(Ctx& x, uint32_t db, uint32_t after) {
    Db* d = dbOf(x, db);
    if (!d) return 0;
    Stmt* a = after ? stOf(x, after) : nullptr;
    sqlite3_stmt* n = sqlite3_next_stmt(d->db, a ? a->s : nullptr);
    std::lock_guard<std::mutex> lk(Q(x).mu);
    auto it = Q(x).byHost.find(n);
    return it == Q(x).byHost.end() ? 0 : it->second;
}

// ---- binding
void runDestructor(Ctx& x, uint32_t destructor, uint32_t data) {
    if (destructor && destructor != 0xFFFFFFFFu) x.proc.callGuest(x.cpu, destructor, {data});
}
int32_t q_bindText(Ctx& x, uint32_t h, int32_t i, GuestPtr text, int32_t n, uint32_t destructor) {
    Stmt* s = stOf(x, h);
    if (!s) return SQLITE_MISUSE;
    int rc;
    if (!text) rc = sqlite3_bind_null(s->s, i);
    else {
        const std::string v = n < 0 ? x.proc.readCString(text) : std::string(reinterpret_cast<const char*>(x.mem.host(text)), size_t(n));
        rc = sqlite3_bind_text(s->s, i, v.data(), int(v.size()), SQLITE_TRANSIENT);
    }
    runDestructor(x, destructor, text);
    return rc;
}
int32_t q_bindBlob(Ctx& x, uint32_t h, int32_t i, GuestPtr data, int32_t n, uint32_t destructor) {
    Stmt* s = stOf(x, h);
    if (!s) return SQLITE_MISUSE;
    const int rc = data ? sqlite3_bind_blob(s->s, i, n > 0 ? x.mem.host(data) : nullptr, std::max(n, 0), SQLITE_TRANSIENT)
                        : sqlite3_bind_null(s->s, i);
    runDestructor(x, destructor, data);
    return rc;
}
int32_t q_bindInt(Ctx& x, uint32_t h, int32_t i, int32_t v) { Stmt* s = stOf(x, h); return s ? sqlite3_bind_int(s->s, i, v) : SQLITE_MISUSE; }
void q_bindInt64(Ctx& x) {   // (stmt, idx, int64 in r2:r3)
    Stmt* s = stOf(x, x.cpu.r[0]);
    const int64_t v = int64_t((uint64_t(x.cpu.r[3]) << 32) | x.cpu.r[2]);
    x.cpu.r[0] = uint32_t(s ? sqlite3_bind_int64(s->s, int(x.cpu.r[1]), v) : SQLITE_MISUSE);
}
void q_bindDouble(Ctx& x) {  // (stmt, idx, double in r2:r3)
    Stmt* s = stOf(x, x.cpu.r[0]);
    const uint64_t b = (uint64_t(x.cpu.r[3]) << 32) | x.cpu.r[2];
    double v;
    std::memcpy(&v, &b, 8);
    x.cpu.r[0] = uint32_t(s ? sqlite3_bind_double(s->s, int(x.cpu.r[1]), v) : SQLITE_MISUSE);
}
int32_t q_bindNull(Ctx& x, uint32_t h, int32_t i) { Stmt* s = stOf(x, h); return s ? sqlite3_bind_null(s->s, i) : SQLITE_MISUSE; }
int32_t q_bindCount(Ctx& x, uint32_t h) { Stmt* s = stOf(x, h); return s ? sqlite3_bind_parameter_count(s->s) : 0; }
int32_t q_bindIndex(Ctx& x, uint32_t h, GuestPtr name) { Stmt* s = stOf(x, h); return s ? sqlite3_bind_parameter_index(s->s, x.proc.readCString(name).c_str()) : 0; }
uint32_t q_bindName(Ctx& x, uint32_t h, int32_t i) {
    Stmt* s = stOf(x, h);
    const char* n = s ? sqlite3_bind_parameter_name(s->s, i) : nullptr;
    return n ? keep(x, *s, guestCopy(x, n, std::strlen(n), true)) : 0;
}

// ---- columns
int32_t q_colCount(Ctx& x, uint32_t h) { Stmt* s = stOf(x, h); return s ? sqlite3_column_count(s->s) : 0; }
int32_t q_dataCount(Ctx& x, uint32_t h) { Stmt* s = stOf(x, h); return s ? sqlite3_data_count(s->s) : 0; }
int32_t q_colType(Ctx& x, uint32_t h, int32_t i) { Stmt* s = stOf(x, h); return s ? sqlite3_column_type(s->s, i) : SQLITE_NULL; }
int32_t q_colInt(Ctx& x, uint32_t h, int32_t i) { Stmt* s = stOf(x, h); return s ? sqlite3_column_int(s->s, i) : 0; }
int64_t q_colInt64(Ctx& x, uint32_t h, int32_t i) { Stmt* s = stOf(x, h); return s ? sqlite3_column_int64(s->s, i) : 0; }
double q_colDouble(Ctx& x, uint32_t h, int32_t i) { Stmt* s = stOf(x, h); return s ? sqlite3_column_double(s->s, i) : 0; }
int32_t q_colBytes(Ctx& x, uint32_t h, int32_t i) { Stmt* s = stOf(x, h); return s ? sqlite3_column_bytes(s->s, i) : 0; }
uint32_t q_colText(Ctx& x, uint32_t h, int32_t i) {
    Stmt* s = stOf(x, h);
    if (!s) return 0;
    const unsigned char* t = sqlite3_column_text(s->s, i);
    if (!t) return 0;
    return keep(x, *s, guestCopy(x, t, size_t(sqlite3_column_bytes(s->s, i)), true));
}
uint32_t q_colBlob(Ctx& x, uint32_t h, int32_t i) {
    Stmt* s = stOf(x, h);
    if (!s) return 0;
    const void* b = sqlite3_column_blob(s->s, i);
    if (!b) return 0;
    return keep(x, *s, guestCopy(x, b, size_t(sqlite3_column_bytes(s->s, i)), false));
}
uint32_t q_colName(Ctx& x, uint32_t h, int32_t i) {
    Stmt* s = stOf(x, h);
    const char* n = s ? sqlite3_column_name(s->s, i) : nullptr;
    return n ? keep(x, *s, guestCopy(x, n, std::strlen(n), true)) : 0;
}
uint32_t q_colDecltype(Ctx& x, uint32_t h, int32_t i) {
    Stmt* s = stOf(x, h);
    const char* n = s ? sqlite3_column_decltype(s->s, i) : nullptr;
    return n ? keep(x, *s, guestCopy(x, n, std::strlen(n), true)) : 0;
}

// sqlite3_exec(db, sql, callback(arg, ncols, values, names), arg, &errmsg)
int32_t q_exec(Ctx& x, uint32_t h, GuestPtr sql, uint32_t cb, uint32_t arg, GuestPtr errOut) {
    Db* d = dbOf(x, h);
    if (errOut) x.mem.write<uint32_t>(errOut, 0);
    if (!d) return SQLITE_MISUSE;
    struct CbCtx { Ctx* x; uint32_t cb, arg; } c{&x, cb, arg};
    char* err = nullptr;
    const int rc = sqlite3_exec(d->db, x.proc.readCString(sql).c_str(), cb ? +[](void* p, int n, char** vals, char** names) -> int {
        auto* c = static_cast<CbCtx*>(p);
        Ctx& x = *c->x;
        std::vector<uint32_t> owned;
        const uint32_t va = x.proc.heap().allocZeroed(uint32_t(n) * 4 + 4), na = x.proc.heap().allocZeroed(uint32_t(n) * 4 + 4);
        for (int i = 0; i < n; ++i) {
            if (vals[i]) { const uint32_t g = guestCopy(x, vals[i], std::strlen(vals[i]), true); owned.push_back(g); x.mem.write<uint32_t>(va + uint32_t(i) * 4, g); }
            const uint32_t g = guestCopy(x, names[i], std::strlen(names[i]), true);
            owned.push_back(g);
            x.mem.write<uint32_t>(na + uint32_t(i) * 4, g);
        }
        const int r = int(int32_t(uint32_t(x.proc.callGuest(x.cpu, c->cb, {c->arg, uint32_t(n), va, na}))));
        for (uint32_t g : owned) x.proc.heap().free(g);
        x.proc.heap().free(va);
        x.proc.heap().free(na);
        return r;
    } : nullptr, &c, &err);
    if (err) {
        if (errOut) x.mem.write<uint32_t>(errOut, guestCopy(x, err, std::strlen(err), true));
        sqlite3_free(err);
    }
    return rc;
}
void q_free(Ctx& x, GuestPtr p) { if (p && x.proc.heap().blockSize(p)) x.proc.heap().free(p); }
uint32_t q_malloc(Ctx& x, int32_t n) { return n > 0 ? x.proc.heap().alloc(uint32_t(n)) : 0; }
uint32_t q_libversion(Ctx& x) { static uint32_t v = 0; if (!v) v = x.proc.allocCString(sqlite3_libversion()); return v; }
int32_t q_libversionNumber(Ctx&) { return sqlite3_libversion_number(); }
int32_t q_threadsafe(Ctx&) { return sqlite3_threadsafe(); }
// User-defined SQL functions aren't bridged: report success so setup code continues.
int32_t q_createFunction(Ctx& x, uint32_t, GuestPtr name) {
    x.proc.logf(LogLevel::Warning, "sqlite3_create_function(%s) isn't supported; queries that use it will fail", x.proc.readCString(name).c_str());
    return SQLITE_OK;
}
uint32_t q_zero1(Ctx&, uint32_t) { return 0; }
int32_t q_ok(Ctx&) { return SQLITE_OK; }

} // namespace

void registerSqlite(Process& p) {
    p.def<&q_open>("_sqlite3_open");
    p.def<&q_openV2>("_sqlite3_open_v2");
    p.def<&q_close>("_sqlite3_close");
    p.def<&q_close>("_sqlite3_close_v2");
    p.def<&q_errmsg>("_sqlite3_errmsg");
    p.def<&q_errcode>("_sqlite3_errcode");
    p.def<&q_errcode>("_sqlite3_extended_errcode");
    p.def<&q_changes>("_sqlite3_changes");
    p.def<&q_changes>("_sqlite3_total_changes");
    p.def<&q_lastRowid>("_sqlite3_last_insert_rowid");
    p.def<&q_busyTimeout>("_sqlite3_busy_timeout");
    p.def<&q_prepare>("_sqlite3_prepare_v2");
    p.def<&q_prepare>("_sqlite3_prepare");
    p.def<&q_step>("_sqlite3_step");
    p.def<&q_reset>("_sqlite3_reset");
    p.def<&q_clearBindings>("_sqlite3_clear_bindings");
    p.def<&q_finalize>("_sqlite3_finalize");
    p.def<&q_nextStmt>("_sqlite3_next_stmt");
    p.def<&q_bindText>("_sqlite3_bind_text");
    p.def<&q_bindBlob>("_sqlite3_bind_blob");
    p.def<&q_bindInt>("_sqlite3_bind_int");
    p.defineFunction("_sqlite3_bind_int64", q_bindInt64);
    p.defineFunction("_sqlite3_bind_double", q_bindDouble);
    p.def<&q_bindNull>("_sqlite3_bind_null");
    p.def<&q_bindCount>("_sqlite3_bind_parameter_count");
    p.def<&q_bindIndex>("_sqlite3_bind_parameter_index");
    p.def<&q_bindName>("_sqlite3_bind_parameter_name");
    p.def<&q_colCount>("_sqlite3_column_count");
    p.def<&q_dataCount>("_sqlite3_data_count");
    p.def<&q_colType>("_sqlite3_column_type");
    p.def<&q_colInt>("_sqlite3_column_int");
    p.def<&q_colInt64>("_sqlite3_column_int64");
    p.def<&q_colDouble>("_sqlite3_column_double");
    p.def<&q_colBytes>("_sqlite3_column_bytes");
    p.def<&q_colText>("_sqlite3_column_text");
    p.def<&q_colBlob>("_sqlite3_column_blob");
    p.def<&q_colName>("_sqlite3_column_name");
    p.def<&q_colDecltype>("_sqlite3_column_decltype");
    p.def<&q_exec>("_sqlite3_exec");
    p.def<&q_free>("_sqlite3_free");
    p.def<&q_malloc>("_sqlite3_malloc");
    p.def<&q_libversion>("_sqlite3_libversion");
    p.def<&q_libversionNumber>("_sqlite3_libversion_number");
    p.def<&q_threadsafe>("_sqlite3_threadsafe");
    p.def<&q_createFunction>("_sqlite3_create_function");
    p.def<&q_zero1>("_sqlite3_user_data");
    p.def<&q_ok>("_sqlite3_initialize");
    p.def<&q_ok>("_sqlite3_shutdown");
    p.def<&q_ok>("_sqlite3_enable_shared_cache");
}

} // namespace Hydrogen::Runtime
