// HydrogenVM — NSData / NSMutableData. Bytes live in a guest heap buffer so
// -bytes / -mutableBytes can hand guest code a real pointer.
#include "Foundation.hpp"
#include <fstream>

namespace Hydrogen::Runtime::Foundation {
namespace {

struct DataData : ObjC::HostObject {
    Process* proc = nullptr;
    uint32_t ptr = 0, len = 0, cap = 0;
    bool owns = true, isMutable = false;
    std::function<void(Ctx&)> onRelease;
    ~DataData() override { if (owns && ptr && proc) proc->heap().free(ptr); }
};

struct Datas {
    explicit Datas(Process&) {}
    Class nsData = 0, nsMutableData = 0, concrete = 0, concreteMutable = 0;
};
Datas& D(Ctx& x) { return x.proc.service<Datas>(); }
DataData* dd(Ctx& x, id d) { return rt(x).hostIf<DataData>(d); }

id newData(Ctx& x, bool isMutable) {
    const id d = rt(x).createInstance(isMutable ? D(x).concreteMutable : D(x).concrete);
    auto h = std::make_unique<DataData>();
    h->proc = &x.proc;
    h->isMutable = isMutable;
    rt(x).setHost(d, std::move(h));
    return d;
}
void reserve(Ctx& x, DataData& d, uint32_t n) {
    if (n <= d.cap && d.owns) return;
    const uint32_t cap = std::max<uint32_t>(n, d.cap * 2 + 16);
    const uint32_t buf = x.proc.heap().allocZeroed(cap);
    if (d.len) std::memcpy(x.mem.host(buf), x.mem.host(d.ptr), d.len);
    if (d.owns && d.ptr) x.proc.heap().free(d.ptr);
    d.ptr = buf; d.cap = cap; d.owns = true;
}
void assign(Ctx& x, id self, const uint8_t* p, uint32_t n) {
    DataData* d = dd(x, self);
    if (!d) return;
    reserve(x, *d, n);
    if (n) std::memcpy(x.mem.host(d->ptr), p, n);
    d->len = n;
}
std::vector<uint8_t> bytesOf(Ctx& x, id data) {
    if (!data) return {};
    if (DataData* d = dd(x, data)) return std::vector<uint8_t>(x.mem.host(d->ptr), x.mem.host(d->ptr) + d->len);
    const uint32_t n = uint32_t(rt(x).send(x.cpu, data, "length"));
    const uint32_t p = uint32_t(rt(x).send(x.cpu, data, "bytes"));
    return n && p ? std::vector<uint8_t>(x.mem.host(p), x.mem.host(p) + n) : std::vector<uint8_t>();
}

id d_allocWithZone(Ctx& x, id self, SEL, uint32_t) {
    if (self == D(x).nsData || self == D(x).concrete) return newData(x, false);
    if (self == D(x).nsMutableData || self == D(x).concreteMutable) return newData(x, true);
    return rt(x).createInstance(self);
}
id d_alloc(Ctx& x, id self, SEL s) { return d_allocWithZone(x, self, s, 0); }
id di_init(Ctx&, id self, SEL) { return self; }
id di_initWithCapacity(Ctx& x, id self, SEL, uint32_t n) { if (DataData* d = dd(x, self)) reserve(x, *d, n); return self; }
id di_initWithLength(Ctx& x, id self, SEL, uint32_t n) { std::vector<uint8_t> z(n, 0); assign(x, self, z.data(), n); return self; }
id di_initWithBytes(Ctx& x, id self, SEL, GuestPtr p, uint32_t n) { assign(x, self, n ? x.mem.host(p) : nullptr, n); return self; }
id di_initWithBytesNoCopy(Ctx& x, id self, SEL, GuestPtr p, uint32_t n, bool freeWhenDone) {
    DataData* d = dd(x, self);
    if (!d) return self;
    if (d->isMutable) { assign(x, self, x.mem.host(p), n); if (freeWhenDone) x.proc.heap().free(p); return self; }
    d->ptr = p; d->len = n; d->cap = n;
    d->owns = freeWhenDone && x.proc.heap().blockSize(p) != 0;
    return self;
}
id di_initWithBytesNoCopy2(Ctx& x, id self, SEL s, GuestPtr p, uint32_t n) { return di_initWithBytesNoCopy(x, self, s, p, n, true); }
id di_initWithData(Ctx& x, id self, SEL, id other) { const auto b = bytesOf(x, other); assign(x, self, b.data(), uint32_t(b.size())); return self; }
id di_initWithContentsOfFile(Ctx& x, id self, SEL, id path) {
    std::ifstream in(utf8(x, path), std::ios::binary);
    if (!in) { rt(x).release(x.cpu, self); return 0; }
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), {});
    assign(x, self, b.data(), uint32_t(b.size()));
    return self;
}
id di_initWithContentsOfFileOpts(Ctx& x, id self, SEL s, id path, uint32_t, GuestPtr err) {
    const id r = di_initWithContentsOfFile(x, self, s, path);
    if (!r && err) x.mem.write<uint32_t>(err, 0);
    return r;
}
id di_initWithContentsOfURL(Ctx& x, id self, SEL s, id url) {
    const id path = id(rt(x).send(x.cpu, url, "path"));
    return di_initWithContentsOfFile(x, self, s, path);
}
template <typename F, typename... A> id construct(Ctx& x, id cls, F init, A... a) {
    id o = d_allocWithZone(x, cls, 0, 0);
    o = init(x, o, 0, a...);
    return o ? autoreleased(x, o) : 0;
}
id dc_data(Ctx& x, id self, SEL) { return construct(x, self, di_init); }
id dc_dataWithBytes(Ctx& x, id self, SEL, GuestPtr p, uint32_t n) { return construct(x, self, di_initWithBytes, p, n); }
id dc_dataWithBytesNoCopy(Ctx& x, id self, SEL, GuestPtr p, uint32_t n) { return construct(x, self, di_initWithBytesNoCopy2, p, n); }
id dc_dataWithBytesNoCopyFree(Ctx& x, id self, SEL, GuestPtr p, uint32_t n, bool f) { return construct(x, self, di_initWithBytesNoCopy, p, n, f); }
id dc_dataWithData(Ctx& x, id self, SEL, id o) { return construct(x, self, di_initWithData, o); }
id dc_dataWithContentsOfFile(Ctx& x, id self, SEL, id path) { return construct(x, self, di_initWithContentsOfFile, path); }
id dc_dataWithContentsOfFileOpts(Ctx& x, id self, SEL, id path, uint32_t o, GuestPtr e) { return construct(x, self, di_initWithContentsOfFileOpts, path, o, e); }
id dc_dataWithContentsOfURL(Ctx& x, id self, SEL, id url) { return construct(x, self, di_initWithContentsOfURL, url); }
id dc_dataWithCapacity(Ctx& x, id self, SEL, uint32_t n) { return construct(x, self, di_initWithCapacity, n); }
id dc_dataWithLength(Ctx& x, id self, SEL, uint32_t n) { return construct(x, self, di_initWithLength, n); }

uint32_t d_length(Ctx& x, id self, SEL) { DataData* d = dd(x, self); return d ? d->len : 0; }
uint32_t d_bytes(Ctx& x, id self, SEL) { DataData* d = dd(x, self); return d ? d->ptr : 0; }
void d_getBytesLength(Ctx& x, id self, SEL, GuestPtr out, uint32_t n) {
    DataData* d = dd(x, self);
    if (d && n) std::memcpy(x.mem.host(out), x.mem.host(d->ptr), std::min(n, d->len));
}
void d_getBytes(Ctx& x, id self, SEL s, GuestPtr out) { DataData* d = dd(x, self); if (d) d_getBytesLength(x, self, s, out, d->len); }
void d_getBytesRange(Ctx& x, id self, SEL, GuestPtr out, uint32_t loc, uint32_t len) {
    DataData* d = dd(x, self);
    if (!d || uint64_t(loc) + len > d->len) throw ObjC::ObjCError("-[NSData getBytes:range:]: range out of bounds");
    if (len) std::memcpy(x.mem.host(out), x.mem.host(d->ptr + loc), len);
}
id d_subdata(Ctx& x, id self, SEL, uint32_t loc, uint32_t len) {
    DataData* d = dd(x, self);
    if (!d || uint64_t(loc) + len > d->len) throw ObjC::ObjCError("-[NSData subdataWithRange:]: range out of bounds");
    return makeData(x, x.mem.host(d->ptr + loc), len);
}
bool d_isEqualToData(Ctx& x, id self, SEL, id o) { return o == self || (o && bytesOf(x, self) == bytesOf(x, o)); }
uint32_t d_hash(Ctx& x, id self, SEL) { DataData* d = dd(x, self); return d ? d->len : 0; }
bool d_writeToFile(Ctx& x, id self, SEL, id path, bool) {
    const auto b = bytesOf(x, self);
    const std::string p = utf8(x, path), tmp = p + ".hvmtmp";
    { std::ofstream out(tmp, std::ios::binary | std::ios::trunc); if (!out) return false; out.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size())); if (!out) return false; }
    return std::rename(tmp.c_str(), p.c_str()) == 0;
}
bool d_writeToFileOpts(Ctx& x, id self, SEL s, id path, uint32_t, GuestPtr) { return d_writeToFile(x, self, s, path, true); }
bool d_writeToURL(Ctx& x, id self, SEL s, id url, bool a) { return d_writeToFile(x, self, s, id(rt(x).send(x.cpu, url, "path")), a); }
id d_description(Ctx& x, id self, SEL) {
    const auto b = bytesOf(x, self);
    std::string s = "<";
    char buf[4];
    for (size_t i = 0; i < b.size(); ++i) { if (i && i % 4 == 0) s += ' '; std::snprintf(buf, sizeof buf, "%02x", b[i]); s += buf; }
    return makeString(x, s + ">");
}
id d_copyWithZone(Ctx& x, id self, SEL, uint32_t) {
    DataData* d = dd(x, self);
    if (d && !d->isMutable) return rt(x).retain(x.cpu, self);
    const auto b = bytesOf(x, self);
    return makeData(x, b.data(), uint32_t(b.size()), true, false);
}
id d_mutableCopyWithZone(Ctx& x, id self, SEL, uint32_t) { const auto b = bytesOf(x, self); return makeData(x, b.data(), uint32_t(b.size()), true, true); }
id d_copy(Ctx& x, id self, SEL s) { return d_copyWithZone(x, self, s, 0); }
id d_mutableCopy(Ctx& x, id self, SEL s) { return d_mutableCopyWithZone(x, self, s, 0); }

DataData& dmut(Ctx& x, id self) {
    DataData* d = dd(x, self);
    if (!d) throw ObjC::ObjCError("mutating method sent to a non-HydrogenVM NSData");
    return *d;
}
uint32_t m_mutableBytes(Ctx& x, id self, SEL) { auto& d = dmut(x, self); if (!d.owns) reserve(x, d, d.len); return d.ptr; }
void m_setLength(Ctx& x, id self, SEL, uint32_t n) {
    auto& d = dmut(x, self);
    reserve(x, d, n);
    if (n > d.len) std::memset(x.mem.host(d.ptr + d.len), 0, n - d.len);
    d.len = n;
}
void m_increaseLength(Ctx& x, id self, SEL s, uint32_t n) { m_setLength(x, self, s, dmut(x, self).len + n); }
void m_appendBytes(Ctx& x, id self, SEL, GuestPtr p, uint32_t n) {
    auto& d = dmut(x, self);
    if (!n) return;
    std::vector<uint8_t> tmp(x.mem.host(p), x.mem.host(p) + n);   // p may point into our own buffer
    reserve(x, d, d.len + n);
    std::memcpy(x.mem.host(d.ptr + d.len), tmp.data(), n);
    d.len += n;
}
void m_appendData(Ctx& x, id self, SEL, id o) {
    const auto b = bytesOf(x, o);
    auto& d = dmut(x, self);
    reserve(x, d, d.len + uint32_t(b.size()));
    if (!b.empty()) std::memcpy(x.mem.host(d.ptr + d.len), b.data(), b.size());
    d.len += uint32_t(b.size());
}
void m_replaceBytes(Ctx& x, id self, SEL, uint32_t loc, uint32_t len, GuestPtr p) {
    auto& d = dmut(x, self);
    if (uint64_t(loc) + len > d.len) { reserve(x, d, loc + len); d.len = loc + len; }
    std::memmove(x.mem.host(d.ptr + loc), x.mem.host(p), len);
}
void m_replaceBytesLen(Ctx& x, id self, SEL, uint32_t loc, uint32_t len, GuestPtr p, uint32_t newLen) {
    auto& d = dmut(x, self);
    std::vector<uint8_t> all(x.mem.host(d.ptr), x.mem.host(d.ptr) + d.len);
    std::vector<uint8_t> repl(p ? x.mem.host(p) : nullptr, p ? x.mem.host(p) + newLen : nullptr);
    if (!p) repl.assign(newLen, 0);
    if (loc > all.size()) all.resize(loc, 0);
    all.erase(all.begin() + loc, all.begin() + std::min<size_t>(all.size(), size_t(loc) + len));
    all.insert(all.begin() + loc, repl.begin(), repl.end());
    assign(x, self, all.data(), uint32_t(all.size()));
}
void m_resetBytes(Ctx& x, id self, SEL, uint32_t loc, uint32_t len) {
    auto& d = dmut(x, self);
    if (uint64_t(loc) + len > d.len) { reserve(x, d, loc + len); d.len = loc + len; }
    std::memset(x.mem.host(d.ptr + loc), 0, len);
}
void m_setData(Ctx& x, id self, SEL, id o) { const auto b = bytesOf(x, o); assign(x, self, b.data(), uint32_t(b.size())); }
void data_dealloc(Ctx& x, id self, SEL) {
    if (auto* d = dd(x, self)) {
        auto release = std::move(d->onRelease);
        if (release) release(x);
    }
    rt(x).disposeInstance(x.cpu, self);
}

} // namespace

id makeData(Ctx& x, const void* bytes, uint32_t n, bool owned, bool isMutable) {
    const id d = newData(x, isMutable);
    assign(x, d, static_cast<const uint8_t*>(bytes), n);
    return owned ? d : autoreleased(x, d);
}
id makeDataNoCopy(Ctx& x, uint32_t bytes, uint32_t n, std::function<void(Ctx&)> onRelease) {
    const id data = newData(x, false);
    auto* d = dd(x, data);
    d->ptr = bytes; d->len = d->cap = n; d->owns = false;
    d->onRelease = std::move(onRelease);
    return data;
}
std::vector<uint8_t> dataBytes(Ctx& x, id data) { return bytesOf(x, data); }
bool isData(Ctx& x, id obj) { return obj && rt(x).isKindOf(obj, D(x).nsData); }

void registerData(Process& p, ObjC::Runtime& r) {
    auto& st = p.service<Datas>();
    ObjC::ClassBuilder nd = r.defineClass("NSData", "NSObject");
    ObjC::ClassBuilder nmd = r.defineClass("NSMutableData", "NSData");
    ObjC::ClassBuilder cd = r.defineClass("NSConcreteData", "NSData");
    ObjC::ClassBuilder cmd = r.defineClass("NSConcreteMutableData", "NSMutableData");
    st.nsData = nd.cls(); st.nsMutableData = nmd.cls(); st.concrete = cd.cls(); st.concreteMutable = cmd.cls();
    nd.cm<&d_alloc>("alloc").cm<&d_allocWithZone>("allocWithZone:").cm<&dc_data>("data")
      .cm<&dc_dataWithBytes>("dataWithBytes:length:").cm<&dc_dataWithBytesNoCopy>("dataWithBytesNoCopy:length:")
      .cm<&dc_dataWithBytesNoCopyFree>("dataWithBytesNoCopy:length:freeWhenDone:").cm<&dc_dataWithData>("dataWithData:")
      .cm<&dc_dataWithContentsOfFile>("dataWithContentsOfFile:").cm<&dc_dataWithContentsOfFileOpts>("dataWithContentsOfFile:options:error:")
      .cm<&dc_dataWithContentsOfURL>("dataWithContentsOfURL:").cm<&dc_dataWithContentsOfFile>("dataWithContentsOfMappedFile:");
    nd.m<&di_init>("init").m<&di_initWithBytes>("initWithBytes:length:").m<&di_initWithBytesNoCopy2>("initWithBytesNoCopy:length:")
      .m<&di_initWithBytesNoCopy>("initWithBytesNoCopy:length:freeWhenDone:").m<&di_initWithData>("initWithData:")
      .m<&di_initWithContentsOfFile>("initWithContentsOfFile:").m<&di_initWithContentsOfFileOpts>("initWithContentsOfFile:options:error:")
      .m<&di_initWithContentsOfURL>("initWithContentsOfURL:").m<&di_initWithContentsOfFile>("initWithContentsOfMappedFile:")
      .m<&d_length>("length", "I@:").m<&d_bytes>("bytes", "r^v@:").m<&d_getBytes>("getBytes:").m<&d_getBytesLength>("getBytes:length:")
      .m<&d_getBytesRange>("getBytes:range:").m<&d_subdata>("subdataWithRange:")
      .m<&d_isEqualToData>("isEqualToData:", "c@:@").m<&d_isEqualToData>("isEqual:", "c@:@").m<&d_hash>("hash", "I@:")
      .m<&d_writeToFile>("writeToFile:atomically:").m<&d_writeToFileOpts>("writeToFile:options:error:").m<&d_writeToURL>("writeToURL:atomically:")
      .m<&d_description>("description")
      .m<&d_copyWithZone>("copyWithZone:").m<&d_mutableCopyWithZone>("mutableCopyWithZone:").m<&d_copy>("copy").m<&d_mutableCopy>("mutableCopy");
    nmd.cm<&dc_dataWithCapacity>("dataWithCapacity:").cm<&dc_dataWithLength>("dataWithLength:")
       .m<&di_initWithCapacity>("initWithCapacity:").m<&di_initWithLength>("initWithLength:")
       .m<&m_mutableBytes>("mutableBytes", "^v@:").m<&m_setLength>("setLength:", "v@:I").m<&m_increaseLength>("increaseLengthBy:", "v@:I")
       .m<&m_appendBytes>("appendBytes:length:", "v@:r^vI").m<&m_appendData>("appendData:", "v@:@")
       .m<&m_replaceBytes>("replaceBytesInRange:withBytes:").m<&m_replaceBytesLen>("replaceBytesInRange:withBytes:length:")
       .m<&m_resetBytes>("resetBytesInRange:").m<&m_setData>("setData:");
    cd.m<&data_dealloc>("dealloc", "v@:");
    cmd.m<&data_dealloc>("dealloc", "v@:");
}

} // namespace Hydrogen::Runtime::Foundation
