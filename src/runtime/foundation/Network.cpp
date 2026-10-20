// HydrogenVM — NSStream / CFStream and CFNetwork, offline.
//
// Memory and file streams work (NSInputStream from NSData or a file,
// NSOutputStream to memory). Network streams — CFReadStreamCreateForHTTPRequest,
// socket stream pairs — open, then fail with ENETDOWN the way an iPhone with no
// connection does: the client callback (or NSStreamDelegate) gets an error
// event on the run loop the stream was scheduled on. CFHTTPMessage objects are
// real enough to build requests and read them back.
#include "Foundation.hpp"
#include <fstream>
#include <iterator>

namespace Hydrogen::Runtime::Foundation {
namespace {

using ObjC::Class;
using ObjC::SEL;
ObjC::Runtime& R(Ctx& x) { return ObjC::rt(x); }

// ------------------------------------------------------------ CFHTTPMessage ---
struct Message : ObjC::HostObject {
    Process* proc = nullptr;
    bool request = true;
    id method = 0, url = 0, version = 0, headers = 0, body = 0;
    int32_t status = 0;
};
struct NetState {
    explicit NetState(Process&) {}
    Class message = 0, stream = 0, inputStream = 0, outputStream = 0, host = 0;
};
NetState& NS_(Ctx& x) { return x.proc.service<NetState>(); }

id retainObj(Ctx& x, id o) { return o ? R(x).retain(x.cpu, o) : 0; }

id newMessage(Ctx& x, bool request) {
    const id m = R(x).createInstance(NS_(x).message);
    auto h = std::make_unique<Message>();
    h->request = request;
    h->headers = makeDict(x, {}, true, true);
    h->version = constString(x.proc, "HTTP/1.1");
    R(x).setHost(m, std::move(h));
    return m;   // +1, like every CF Create function
}
Message* msg(Ctx& x, id m) { return m ? R(x).hostIf<Message>(m) : nullptr; }
uint32_t cfm_createRequest(Ctx& x, uint32_t, id method, id url, id version) {
    const id m = newMessage(x, true);
    Message* h = msg(x, m);
    h->method = retainObj(x, method);
    h->url = retainObj(x, url);
    if (version) h->version = retainObj(x, version);
    return m;
}
uint32_t cfm_createResponse(Ctx& x, uint32_t, int32_t status, id, id version) {
    const id m = newMessage(x, false);
    Message* h = msg(x, m);
    h->status = status;
    if (version) h->version = retainObj(x, version);
    return m;
}
uint32_t cfm_createEmpty(Ctx& x, uint32_t, uint32_t isRequest) { return newMessage(x, (isRequest & 0xFF) != 0); }
void cfm_setHeader(Ctx& x, id m, id field, id value) {
    Message* h = msg(x, m);
    if (!h || !field) return;
    if (value) R(x).send(x.cpu, h->headers, "setObject:forKey:", {value, field});
    else R(x).send(x.cpu, h->headers, "removeObjectForKey:", {field});
}
uint32_t cfm_copyHeader(Ctx& x, id m, id field) { Message* h = msg(x, m); return h ? retainObj(x, dictGet(x, h->headers, field)) : 0; }
uint32_t cfm_copyAllHeaders(Ctx& x, id m) {
    Message* h = msg(x, m);
    return h ? id(R(x).send(x.cpu, h->headers, "copy")) : makeDict(x, {}, true);
}
void cfm_setBody(Ctx& x, id m, id data) {
    Message* h = msg(x, m);
    if (!h) return;
    const id old = h->body;
    h->body = data ? id(R(x).send(x.cpu, data, "copy")) : 0;
    if (old) R(x).release(x.cpu, old);
}
uint32_t cfm_copyBody(Ctx& x, id m) { Message* h = msg(x, m); return h ? retainObj(x, h->body) : 0; }
uint32_t cfm_copyURL(Ctx& x, id m) { Message* h = msg(x, m); return h ? retainObj(x, h->url) : 0; }
uint32_t cfm_copyMethod(Ctx& x, id m) { Message* h = msg(x, m); return h ? retainObj(x, h->method) : 0; }
uint32_t cfm_copyVersion(Ctx& x, id m) { Message* h = msg(x, m); return h ? retainObj(x, h->version) : 0; }
uint32_t cfm_copyStatusLine(Ctx& x, id m) {
    Message* h = msg(x, m);
    return h && !h->request ? makeString(x, "HTTP/1.1 " + std::to_string(h->status), true) : 0;
}
int32_t cfm_status(Ctx& x, id m) { Message* h = msg(x, m); return h ? h->status : 0; }
uint32_t cfm_false2(Ctx&, id, uint32_t, uint32_t) { return 0; }
uint32_t cfm_false1(Ctx&, id) { return 0; }
uint32_t cfm_false2b(Ctx&, id, id) { return 0; }
uint32_t cfm_serialized(Ctx& x, id) { return makeData(x, nullptr, 0, true); }
void msg_dealloc(Ctx& x, id self, SEL) {
    if (Message* h = msg(x, self))
        for (id o : {h->method, h->url, h->version, h->headers, h->body}) if (o) R(x).release(x.cpu, o);
    R(x).disposeInstance(x.cpu, self);
}

// ------------------------------------------------------------------ streams ---
enum Status : int32_t { kNotOpen = 0, kOpening = 1, kOpen = 2, kReading = 3, kWriting = 4, kAtEnd = 5, kClosed = 6, kError = 7 };
enum Event : uint32_t { evNone = 0, evOpen = 1, evBytes = 2, evCanAccept = 4, evEnd = 16, evError = 8 };
constexpr int32_t kENETDOWN = 50;

struct Stream : ObjC::HostObject {
    enum Kind { Memory, OutMemory, Network } kind = Memory;
    bool input = true;
    int32_t status = kNotOpen;
    std::vector<uint8_t> data;   // input bytes, or what was written
    size_t pos = 0;
    // CF client
    uint32_t callback = 0, info = 0, flags = 0;
    // NSStream delegate
    id delegate = 0;
    RunLoop* loop = nullptr;
    bool errorPosted = false;
};
Stream* st(Ctx& x, id s) { return s ? R(x).hostIf<Stream>(s) : nullptr; }

id newStream(Ctx& x, Stream::Kind kind, bool input, std::vector<uint8_t> bytes = {}) {
    const id s = R(x).createInstance(input ? NS_(x).inputStream : NS_(x).outputStream);
    auto h = std::make_unique<Stream>();
    h->kind = kind;
    h->input = input;
    h->data = std::move(bytes);
    R(x).setHost(s, std::move(h));
    return s;
}

// Delivers an event the way CFStream does: on the scheduled run loop, to the CF
// client callback and/or the NSStream delegate.
void deliver(Ctx& x, id s, uint32_t event) {
    Stream* h = st(x, s);
    if (!h || !h->loop) return;
    R(x).retain(x.cpu, s);
    Process* p = &x.proc;
    h->loop->post([p, s, event](CPU::Cpu& c) {
        Ctx cx{*p, c, p->mem()};
        Stream* hh = st(cx, s);
        if (hh && hh->callback && (hh->flags & event)) p->callGuest(c, hh->callback, {s, event, hh->info});
        if (hh && hh->delegate && R(cx).respondsTo(hh->delegate, R(cx).sel("stream:handleEvent:")))
            R(cx).send(c, hh->delegate, "stream:handleEvent:", {s, event});
        R(cx).release(c, s);
    });
}

bool streamOpen(Ctx& x, id s) {
    Stream* h = st(x, s);
    if (!h || h->status != kNotOpen) return false;
    if (h->kind == Stream::Network) {
        h->status = kOpening;
        if (h->loop) { h->errorPosted = true; h->status = kError; deliver(x, s, evError); }
        else h->status = kError;
        return true;
    }
    h->status = kOpen;
    if (h->loop) {
        deliver(x, s, evOpen);
        deliver(x, s, h->input ? (h->pos < h->data.size() ? evBytes : evEnd) : evCanAccept);
    }
    return true;
}
void streamSchedule(Ctx& x, id s, id loop) {
    Stream* h = st(x, s);
    if (!h) return;
    h->loop = &runLoopForObject(x, loop);
    if (h->kind == Stream::Network && h->status == kError && !h->errorPosted) { h->errorPosted = true; deliver(x, s, evError); }
}
int32_t streamRead(Ctx& x, id s, GuestPtr buf, int32_t n) {
    Stream* h = st(x, s);
    if (!h || !h->input) return -1;
    if (h->status == kError) return -1;
    if (h->status != kOpen && h->status != kAtEnd) return -1;
    const size_t k = std::min<size_t>(size_t(std::max(n, 0)), h->data.size() - h->pos);
    if (k) x.mem.copyIn(buf, h->data.data() + h->pos, k);
    h->pos += k;
    if (h->pos >= h->data.size()) {
        if (h->status != kAtEnd) { h->status = kAtEnd; deliver(x, s, evEnd); }
    } else if (k) deliver(x, s, evBytes);
    return int32_t(k);
}
int32_t streamWrite(Ctx& x, id s, GuestPtr buf, int32_t n) {
    Stream* h = st(x, s);
    if (!h || h->input || h->status != kOpen || h->kind == Stream::Network) return -1;
    const uint8_t* p = x.mem.host(buf);
    h->data.insert(h->data.end(), p, p + std::max(n, 0));
    deliver(x, s, evCanAccept);
    return n;
}
void streamClose(Ctx& x, id s) { if (Stream* h = st(x, s)) { h->status = kClosed; h->loop = nullptr; } }
id streamError(Ctx& x, id s) {
    Stream* h = st(x, s);
    if (!h || h->status != kError) return 0;
    const Class err = R(x).getClass("NSError");
    return id(R(x).send(x.cpu, err, "errorWithDomain:code:userInfo:", {constString(x.proc, "NSPOSIXErrorDomain"), uint32_t(kENETDOWN), 0}));
}

// CF entry points
uint32_t cfs_httpRead(Ctx& x, uint32_t, id) { return newStream(x, Stream::Network, true); }
uint32_t cfs_httpReadStreamed(Ctx& x, uint32_t, id, id) { return newStream(x, Stream::Network, true); }
void cfs_pair(Ctx& x, GuestPtr readOut, GuestPtr writeOut) {
    if (readOut) x.mem.write<uint32_t>(readOut, newStream(x, Stream::Network, true));
    if (writeOut) x.mem.write<uint32_t>(writeOut, newStream(x, Stream::Network, false));
}
void cfs_pairToHost(Ctx& x, uint32_t, id, uint32_t, GuestPtr r, GuestPtr w) { cfs_pair(x, r, w); }
void cfs_pairWithSocket(Ctx& x, uint32_t, int32_t, GuestPtr r, GuestPtr w) { cfs_pair(x, r, w); }
uint32_t cfs_createWithBytes(Ctx& x, uint32_t, GuestPtr bytes, int32_t n, uint32_t) {
    const uint8_t* p = n > 0 ? x.mem.host(bytes) : nullptr;
    return newStream(x, Stream::Memory, true, std::vector<uint8_t>(p, p + std::max(n, 0)));
}
uint32_t cfs_createWithFile(Ctx& x, uint32_t, id url) {
    const id path = url ? id(R(x).send(x.cpu, url, "path")) : 0;
    std::ifstream f(path ? utf8(x, path) : std::string(), std::ios::binary);
    if (!f) return 0;
    return newStream(x, Stream::Memory, true, std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {}));
}
uint32_t cfs_writeToMemory(Ctx& x, uint32_t) { return newStream(x, Stream::OutMemory, false); }
uint32_t cfs_true3(Ctx&, id, id, id) { return 1; }
uint32_t cfs_copyProperty(Ctx& x, id s, id key) {
    Stream* h = st(x, s);
    if (h && h->kind == Stream::OutMemory && key && utf8(x, key) == "kCFStreamPropertyDataWritten")
        return makeData(x, h->data.data(), uint32_t(h->data.size()), true);
    return 0;
}
// CFReadStreamSetClient(stream, flags, callback, CFStreamClientContext* {version, info, retain, release, copyDesc})
uint32_t cfs_setClient(Ctx& x, id s, uint32_t flags, uint32_t cb, GuestPtr ctx) {
    Stream* h = st(x, s);
    if (!h) return 0;
    h->flags = cb ? flags : 0;
    h->callback = cb;
    h->info = ctx ? x.mem.read<uint32_t>(ctx + 4) : 0;
    return 1;
}
void cfs_schedule(Ctx& x, id s, id loop, id) { streamSchedule(x, s, loop); }
void cfs_unschedule(Ctx& x, id s, id, id) { if (Stream* h = st(x, s)) h->loop = nullptr; }
uint32_t cfs_open(Ctx& x, id s) { return streamOpen(x, s) ? 1 : 0; }
void cfs_close(Ctx& x, id s) { streamClose(x, s); }
int32_t cfs_status(Ctx& x, id s) { Stream* h = st(x, s); return h ? h->status : kError; }
// CFStreamError CFReadStreamGetError(stream): 8-byte struct returned through r0; stream in r1.
void cfs_getError(Ctx& x) {
    const uint32_t ret = x.cpu.r[0];
    Stream* h = st(x, x.cpu.r[1]);
    const bool failed = h && h->status == kError;
    x.mem.write<uint32_t>(ret, failed ? 1 : 0);          // kCFStreamErrorDomainPOSIX
    x.mem.write<uint32_t>(ret + 4, failed ? kENETDOWN : 0);
}
uint32_t cfs_copyError(Ctx& x, id s) { const id e = streamError(x, s); return e ? R(x).retain(x.cpu, e) : 0; }
uint32_t cfs_hasBytes(Ctx& x, id s) { Stream* h = st(x, s); return h && h->input && h->status == kOpen && h->pos < h->data.size(); }
uint32_t cfs_canAccept(Ctx& x, id s) { Stream* h = st(x, s); return h && !h->input && h->status == kOpen && h->kind != Stream::Network; }
int32_t cfs_read(Ctx& x, id s, GuestPtr buf, int32_t n) { return streamRead(x, s, buf, n); }
int32_t cfs_write(Ctx& x, id s, GuestPtr buf, int32_t n) { return streamWrite(x, s, buf, n); }

// NSStream methods
id ns_inputWithData(Ctx& x, id, SEL, id data) { return autoreleased(x, newStream(x, Stream::Memory, true, dataBytes(x, data))); }
id ns_inputWithFile(Ctx& x, id, SEL, id path) {
    std::ifstream f(path ? utf8(x, path) : std::string(), std::ios::binary);
    if (!f) return 0;
    return autoreleased(x, newStream(x, Stream::Memory, true, std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {})));
}
id ns_inputWithURL(Ctx& x, id cls, SEL s, id url) { return ns_inputWithFile(x, cls, s, url ? id(R(x).send(x.cpu, url, "path")) : 0); }
id ns_initWithData(Ctx& x, id self, SEL, id data) {
    R(x).release(x.cpu, self);
    return newStream(x, Stream::Memory, true, dataBytes(x, data));
}
id ns_initWithFile(Ctx& x, id self, SEL s, id path) {
    R(x).release(x.cpu, self);
    const id r = ns_inputWithFile(x, 0, s, path);
    return r ? R(x).retain(x.cpu, r) : 0;
}
id ns_outputToMemory(Ctx& x, id, SEL) { return autoreleased(x, newStream(x, Stream::OutMemory, false)); }
id ns_initToMemory(Ctx& x, id self, SEL) { R(x).release(x.cpu, self); return newStream(x, Stream::OutMemory, false); }
void ns_open(Ctx& x, id self, SEL) { streamOpen(x, self); }
void ns_close(Ctx& x, id self, SEL) { streamClose(x, self); }
int32_t ns_read(Ctx& x, id self, SEL, GuestPtr buf, int32_t n) { return streamRead(x, self, buf, n); }
int32_t ns_write(Ctx& x, id self, SEL, GuestPtr buf, int32_t n) { return streamWrite(x, self, buf, n); }
bool ns_hasBytes(Ctx& x, id self, SEL) { return cfs_hasBytes(x, self); }
bool ns_hasSpace(Ctx& x, id self, SEL) { return cfs_canAccept(x, self); }
int32_t ns_status(Ctx& x, id self, SEL) { return cfs_status(x, self); }
id ns_error(Ctx& x, id self, SEL) { return streamError(x, self); }
id ns_delegate(Ctx& x, id self, SEL) { Stream* h = st(x, self); return h ? h->delegate : 0; }
void ns_setDelegate(Ctx& x, id self, SEL, id d) { if (Stream* h = st(x, self)) h->delegate = d; }
void ns_schedule(Ctx& x, id self, SEL, id loop, id) { streamSchedule(x, self, loop); }
void ns_unschedule(Ctx& x, id self, SEL, id, id) { if (Stream* h = st(x, self)) h->loop = nullptr; }
id ns_property(Ctx& x, id self, SEL, id key) {
    Stream* h = st(x, self);
    if (h && h->kind == Stream::OutMemory && key) {
        const std::string k = utf8(x, key);
        if (k == "NSStreamDataWrittenToMemoryStreamKey" || k == "kCFStreamPropertyDataWritten")
            return makeData(x, h->data.data(), uint32_t(h->data.size()));
    }
    return 0;
}
bool ns_setProperty(Ctx&, id, SEL, id, id) { return true; }
// +getStreamsToHost:port:inputStream:outputStream: (NSHost) / getStreamsToHostWithName:port:...
void ns_streamsToHost(Ctx& x, id, SEL, id, int32_t, GuestPtr in, GuestPtr out) {
    if (in) x.mem.write<uint32_t>(in, autoreleased(x, newStream(x, Stream::Network, true)));
    if (out) x.mem.write<uint32_t>(out, autoreleased(x, newStream(x, Stream::Network, false)));
}

// ------------------------------------------------------- CFSocket / CFHost ---
uint32_t z0(Ctx&) { return 0; }
uint32_t z1(Ctx&, uint32_t) { return 0; }
uint32_t z2(Ctx&, uint32_t, uint32_t) { return 0; }
uint32_t z3(Ctx&, uint32_t, uint32_t, uint32_t) { return 0; }
int32_t m1_2(Ctx&, uint32_t, uint32_t) { return -1; }
int32_t m1_1(Ctx&, uint32_t) { return -1; }
int32_t m1_3(Ctx&, uint32_t, uint32_t, uint32_t) { return -1; }
uint32_t one3(Ctx&, uint32_t, uint32_t, uint32_t) { return 1; }
void v1(Ctx&, uint32_t) {}
void v2(Ctx&, uint32_t, uint32_t) {}
void v3(Ctx&, uint32_t, uint32_t, uint32_t) {}
uint32_t host_create(Ctx& x, uint32_t, id) { return R(x).createInstance(NS_(x).host); }
// CFHostStartInfoResolution(host, info, CFStreamError* error): fails with a NetDB "host not found".
uint32_t host_start(Ctx& x, id, uint32_t, GuestPtr err) {
    if (err) { x.mem.write<uint32_t>(err, 12); x.mem.write<uint32_t>(err + 4, 8); }   // kCFStreamErrorDomainNetDB, EAI_NONAME
    return 0;
}
uint32_t proxies_none(Ctx& x, id, id) {
    const id entry = makeDict(x, {{constString(x.proc, "kCFProxyTypeKey"), constString(x.proc, "kCFProxyTypeNone")}});
    return makeArray(x, {entry}, true);
}
uint32_t proxies_pac(Ctx& x, id, id, GuestPtr err) { if (err) x.mem.write<uint32_t>(err, 0); return proxies_none(x, 0, 0); }
uint32_t proxy_settings(Ctx& x) { return makeDict(x, {}, true); }

} // namespace

void registerNetwork(Process& p, ObjC::Runtime& r) {
    NetState& n = p.service<NetState>();
    ObjC::ClassBuilder m = r.defineClass("__NSCFHTTPMessage", "NSObject");
    n.message = m.cls();
    m.m<&msg_dealloc>("dealloc", "v@:");
    n.host = r.defineClass("__NSCFHost", "NSObject").cls();

    ObjC::ClassBuilder s = r.defineClass("NSStream", "NSObject");
    s.m<&ns_open>("open", "v@:").m<&ns_close>("close", "v@:").m<&ns_status>("streamStatus", "i@:").m<&ns_error>("streamError")
     .m<&ns_delegate>("delegate").m<&ns_setDelegate>("setDelegate:", "v@:@")
     .m<&ns_schedule>("scheduleInRunLoop:forMode:", "v@:@@").m<&ns_unschedule>("removeFromRunLoop:forMode:", "v@:@@")
     .m<&ns_property>("propertyForKey:").m<&ns_setProperty>("setProperty:forKey:", "c@:@@")
     .cm<&ns_streamsToHost>("getStreamsToHost:port:inputStream:outputStream:", "v@:@i^@^@")
     .cm<&ns_streamsToHost>("getStreamsToHostWithName:port:inputStream:outputStream:", "v@:@i^@^@");
    n.stream = s.cls();
    ObjC::ClassBuilder in = r.defineClass("NSInputStream", "NSStream");
    in.cm<&ns_inputWithData>("inputStreamWithData:").cm<&ns_inputWithFile>("inputStreamWithFileAtPath:")
      .cm<&ns_inputWithURL>("inputStreamWithURL:")
      .m<&ns_initWithData>("initWithData:").m<&ns_initWithFile>("initWithFileAtPath:")
      .m<&ns_read>("read:maxLength:", "i@:^CI").m<&ns_hasBytes>("hasBytesAvailable", "c@:");
    n.inputStream = in.cls();
    ObjC::ClassBuilder out = r.defineClass("NSOutputStream", "NSStream");
    out.cm<&ns_outputToMemory>("outputStreamToMemory").m<&ns_initToMemory>("initToMemory")
       .m<&ns_write>("write:maxLength:", "i@:r^CI").m<&ns_hasSpace>("hasSpaceAvailable", "c@:");
    n.outputStream = out.cls();

    // CFHTTPMessage
    p.def<&cfm_createRequest>("_CFHTTPMessageCreateRequest");
    p.def<&cfm_createResponse>("_CFHTTPMessageCreateResponse");
    p.def<&cfm_createEmpty>("_CFHTTPMessageCreateEmpty");
    p.def<&cfm_setHeader>("_CFHTTPMessageSetHeaderFieldValue");
    p.def<&cfm_copyHeader>("_CFHTTPMessageCopyHeaderFieldValue");
    p.def<&cfm_copyAllHeaders>("_CFHTTPMessageCopyAllHeaderFields");
    p.def<&cfm_setBody>("_CFHTTPMessageSetBody");
    p.def<&cfm_copyBody>("_CFHTTPMessageCopyBody");
    p.def<&cfm_copyURL>("_CFHTTPMessageCopyRequestURL");
    p.def<&cfm_copyMethod>("_CFHTTPMessageCopyRequestMethod");
    p.def<&cfm_copyVersion>("_CFHTTPMessageCopyVersion");
    p.def<&cfm_copyStatusLine>("_CFHTTPMessageCopyResponseStatusLine");
    p.def<&cfm_status>("_CFHTTPMessageGetResponseStatusCode");
    p.def<&cfm_false1>("_CFHTTPMessageIsHeaderComplete");
    p.def<&cfm_false2>("_CFHTTPMessageAppendBytes");
    p.def<&cfm_serialized>("_CFHTTPMessageCopySerializedMessage");
    p.def<&cfm_false2b>("_CFHTTPMessageApplyCredentialDictionary");
    p.def<&z3>("_CFHTTPMessageApplyCredentials");
    p.def<&z2>("_CFHTTPMessageAddAuthentication");
    p.def<&z2>("_CFHTTPAuthenticationCreateFromResponse");
    p.def<&z2>("_CFHTTPAuthenticationIsValid");
    p.def<&z1>("_CFHTTPAuthenticationCopyMethod");
    p.def<&z1>("_CFHTTPAuthenticationCopyRealm");
    p.def<&z1>("_CFHTTPAuthenticationRequiresAccountDomain");
    p.def<&z1>("_CFHTTPAuthenticationRequiresUserNameAndPassword");
    p.def<&proxies_none>("_CFNetworkCopyProxiesForURL");
    p.def<&proxies_pac>("_CFNetworkCopyProxiesForAutoConfigurationScript");
    p.def<&proxy_settings>("_CFNetworkCopySystemProxySettings");

    // CFReadStream / CFWriteStream
    p.def<&cfs_httpRead>("_CFReadStreamCreateForHTTPRequest");
    p.def<&cfs_httpReadStreamed>("_CFReadStreamCreateForStreamedHTTPRequest");
    p.def<&cfs_pairToHost>("_CFStreamCreatePairWithSocketToHost");
    p.def<&cfs_pairToHost>("_CFStreamCreatePairWithSocketToCFHost");
    p.def<&cfs_pairWithSocket>("_CFStreamCreatePairWithSocket");
    p.def<&cfs_createWithBytes>("_CFReadStreamCreateWithBytesNoCopy");
    p.def<&cfs_createWithFile>("_CFReadStreamCreateWithFile");
    p.def<&cfs_writeToMemory>("_CFWriteStreamCreateWithAllocatedBuffers");
    for (const char* k : {"Read", "Write"}) {
        const std::string pre = std::string("_CF") + k + "Stream";
        p.def<&cfs_true3>(pre + "SetProperty");
        p.def<&cfs_copyProperty>(pre + "CopyProperty");
        p.def<&cfs_setClient>(pre + "SetClient");
        p.def<&cfs_schedule>(pre + "ScheduleWithRunLoop");
        p.def<&cfs_unschedule>(pre + "UnscheduleFromRunLoop");
        p.def<&cfs_open>(pre + "Open");
        p.def<&cfs_close>(pre + "Close");
        p.def<&cfs_status>(pre + "GetStatus");
        p.defineFunction(pre + "GetError", [](Ctx& x) { cfs_getError(x); });
        p.def<&cfs_copyError>(pre + "CopyError");
    }
    p.def<&cfs_hasBytes>("_CFReadStreamHasBytesAvailable");
    p.def<&cfs_read>("_CFReadStreamRead");
    p.def<&cfs_canAccept>("_CFWriteStreamCanAcceptBytes");
    p.def<&cfs_write>("_CFWriteStreamWrite");

    // CFSocket: creation fails (no network).
    p.def<&z0>("_CFSocketCreate");
    p.def<&z0>("_CFSocketCreateWithNative");
    p.def<&z0>("_CFSocketCreateConnectedToSocketSignature");
    p.def<&z1>("_CFSocketIsValid");
    p.def<&v1>("_CFSocketInvalidate");
    p.def<&m1_1>("_CFSocketGetNative");
    p.def<&z1>("_CFSocketCopyAddress");
    p.def<&z1>("_CFSocketCopyPeerAddress");
    p.def<&m1_2>("_CFSocketSetAddress");
    p.def<&m1_3>("_CFSocketConnectToAddress");
    p.def<&z3>("_CFSocketCreateRunLoopSource");
    p.def<&v2>("_CFSocketSetSocketFlags");
    p.def<&z1>("_CFSocketGetSocketFlags");
    p.def<&v2>("_CFSocketDisableCallBacks");
    p.def<&v2>("_CFSocketEnableCallBacks");
    // CFHost: lookups fail.
    p.def<&host_create>("_CFHostCreateWithName");
    p.def<&host_start>("_CFHostStartInfoResolution");
    p.def<&z3>("_CFHostGetAddressing");
    p.def<&z3>("_CFHostGetNames");
    p.def<&one3>("_CFHostSetClient");
    p.def<&v3>("_CFHostScheduleWithRunLoop");
    p.def<&v3>("_CFHostUnscheduleFromRunLoop");
    p.def<&v2>("_CFHostCancelInfoResolution");

    // Error-domain numbers (SInt32 constants) and CFNetwork's string constants.
    const std::pair<const char*, uint32_t> domains[] = {
        {"_kCFStreamErrorDomainSSL", 3}, {"_kCFStreamErrorDomainHTTP", 4}, {"_kCFStreamErrorDomainSOCKS", 5},
        {"_kCFStreamErrorDomainFTP", 6}, {"_kCFStreamErrorDomainNetServices", 10}, {"_kCFStreamErrorDomainMach", 11},
        {"_kCFStreamErrorDomainNetDB", 12}, {"_kCFStreamErrorDomainSystemConfiguration", 17}, {"_kCFStreamErrorDomainWinSock", 10}};
    for (auto& [name, v] : domains) p.mem().write<uint32_t>(p.defineDataBlock(name, 4), v);
    for (const char* name : {"_kCFErrorDomainCFNetwork", "_kCFStreamPropertyHTTPResponseHeader", "_kCFStreamPropertyHTTPProxy",
                             "_kCFStreamPropertyHTTPProxyHost", "_kCFStreamPropertyHTTPProxyPort", "_kCFStreamPropertyHTTPSProxyHost",
                             "_kCFStreamPropertyHTTPSProxyPort", "_kCFStreamPropertyHTTPAttemptPersistentConnection",
                             "_kCFStreamPropertyHTTPRequestBytesWrittenCount", "_kCFStreamPropertyHTTPShouldAutoredirect",
                             "_kCFStreamPropertyHTTPFinalURL", "_kCFStreamPropertySocketNativeHandle", "_kCFStreamPropertyDataWritten",
                             "_kCFHTTPAuthenticationUsername", "_kCFHTTPAuthenticationPassword", "_kCFHTTPAuthenticationAccountDomain",
                             "_kCFHTTPAuthenticationSchemeBasic", "_kCFHTTPAuthenticationSchemeNTLM", "_kCFHTTPAuthenticationSchemeDigest",
                             "_kCFProxyTypeKey", "_kCFProxyTypeNone", "_kCFProxyTypeHTTP", "_kCFProxyTypeHTTPS", "_kCFProxyTypeSOCKS",
                             "_kCFProxyTypeAutoConfigurationURL", "_kCFProxyHostNameKey", "_kCFProxyPortNumberKey",
                             "_kCFProxyAutoConfigurationURLKey", "_kCFNetworkProxiesHTTPProxy", "_kCFNetworkProxiesHTTPPort",
                             "_kCFNetworkProxiesHTTPEnable", "_NSStreamDataWrittenToMemoryStreamKey", "_NSStreamFileCurrentOffsetKey",
                             "_NSStreamSocketSecurityLevelKey", "_NSStreamSocketSecurityLevelNegotiatedSSL", "_NSStreamSOCKSProxyConfigurationKey",
                             "_NSStreamNetworkServiceType", "_NSStreamNetworkServiceTypeVoIP"}) {
        const uint32_t cell = p.defineDataBlock(name, 4);
        p.mem().write<uint32_t>(cell, constString(p, name + 1));
    }
}

} // namespace Hydrogen::Runtime::Foundation
