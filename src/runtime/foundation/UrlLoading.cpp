// HydrogenVM — NSURLRequest / NSMutableURLRequest / NSURLConnection, offline.
//
// Requests are real value objects (URL, method, headers, body, cache/timeout
// policy), so apps build and inspect them. Connections behave like a device
// with no network: an async NSURLConnection reports failure to its delegate on
// the run loop with NSURLErrorNotConnectedToInternet (-1009), and the
// synchronous loader returns nil with that error. This lets apps that check for
// an error carry on, instead of hanging or crashing on a nil request.
#include "Foundation.hpp"
#include <cstring>

namespace Hydrogen::Runtime::Foundation {
namespace {

using ObjC::Class;
using ObjC::SEL;
ObjC::Runtime& R(Ctx& x) { return ObjC::rt(x); }

constexpr int32_t kNotConnected = -1009;

struct Request : ObjC::HostObject {
    id url = 0, method = 0, body = 0, bodyStream = 0, headers = 0, mainDoc = 0;
    double timeout = 60.0;
    uint32_t cachePolicy = 0;
    uint32_t networkServiceType = 0;
    bool handleCookies = true, pipelining = false, allowsCellular = true;
};
struct UrlLoadState {
    explicit UrlLoadState(Process&) {}
    Class request = 0, mutableRequest = 0, connection = 0, response = 0, httpResponse = 0, cache = 0;
};
UrlLoadState& LS(Ctx& x) { return x.proc.service<UrlLoadState>(); }
Request* req(Ctx& x, id r) { return r ? R(x).hostIf<Request>(r) : nullptr; }

id retainObj(Ctx& x, id o) { return o ? R(x).retain(x.cpu, o) : 0; }
void setField(Ctx& x, id& slot, id v) { const id old = slot; slot = v ? R(x).retain(x.cpu, v) : 0; if (old) R(x).release(x.cpu, old); }

id makeRequest(Ctx& x, id url, bool mutableReq, bool owned) {
    const id o = R(x).createInstance(mutableReq ? LS(x).mutableRequest : LS(x).request);
    auto h = std::make_unique<Request>();
    h->url = retainObj(x, url);
    h->method = constString(x.proc, "GET");
    h->headers = makeDict(x, {}, true, true);
    R(x).setHost(o, std::move(h));
    return owned ? o : autoreleased(x, o);
}

// ---- NSURLRequest
id reqc_with(Ctx& x, id cls, SEL, id url) { return makeRequest(x, url, cls == LS(x).mutableRequest, false); }
id reqc_withPolicy(Ctx& x, id cls, SEL, id url, uint32_t policy, double timeout) {
    const id o = makeRequest(x, url, cls == LS(x).mutableRequest, false);
    Request* r = req(x, o);
    r->cachePolicy = policy;
    r->timeout = timeout;
    return o;
}
id req_initWithURL(Ctx& x, id self, SEL, id url) {
    auto h = std::make_unique<Request>();
    h->url = retainObj(x, url);
    h->method = constString(x.proc, "GET");
    h->headers = makeDict(x, {}, true, true);
    R(x).setHost(self, std::move(h));
    return self;
}
id req_initWithPolicy(Ctx& x, id self, SEL s, id url, uint32_t policy, double timeout) {
    req_initWithURL(x, self, s, url);
    Request* r = req(x, self);
    r->cachePolicy = policy;
    r->timeout = timeout;
    return self;
}
id req_URL(Ctx& x, id self, SEL) { Request* r = req(x, self); return r ? r->url : 0; }
id req_method(Ctx& x, id self, SEL) { Request* r = req(x, self); return r ? r->method : 0; }
id req_body(Ctx& x, id self, SEL) { Request* r = req(x, self); return r ? r->body : 0; }
id req_bodyStream(Ctx& x, id self, SEL) { Request* r = req(x, self); return r ? r->bodyStream : 0; }
id req_mainDoc(Ctx& x, id self, SEL) { Request* r = req(x, self); return r ? r->mainDoc : 0; }
uint32_t req_cachePolicy(Ctx& x, id self, SEL) { Request* r = req(x, self); return r ? r->cachePolicy : 0; }
double req_timeout(Ctx& x, id self, SEL) { Request* r = req(x, self); return r ? r->timeout : 60.0; }
uint32_t req_serviceType(Ctx& x, id self, SEL) { Request* r = req(x, self); return r ? r->networkServiceType : 0; }
bool req_handleCookies(Ctx& x, id self, SEL) { Request* r = req(x, self); return !r || r->handleCookies; }
bool req_pipelining(Ctx& x, id self, SEL) { Request* r = req(x, self); return r && r->pipelining; }
bool req_allowsCellular(Ctx& x, id self, SEL) { Request* r = req(x, self); return !r || r->allowsCellular; }
id req_headers(Ctx& x, id self, SEL) { Request* r = req(x, self); return r ? r->headers : 0; }
id req_headerValue(Ctx& x, id self, SEL, id field) {
    Request* r = req(x, self);
    return r && field ? dictGet(x, r->headers, field) : 0;
}
id req_copy(Ctx& x, id self, SEL, uint32_t) {
    const id o = makeRequest(x, 0, false, true);
    if (Request* a = req(x, self)) {
        Request* b = req(x, o);
        b->url = retainObj(x, a->url);
        b->method = retainObj(x, a->method);
        b->body = retainObj(x, a->body);
        b->bodyStream = retainObj(x, a->bodyStream);
        b->mainDoc = retainObj(x, a->mainDoc);
        setField(x, b->headers, id(R(x).send(x.cpu, a->headers, "mutableCopy")));
        R(x).release(x.cpu, b->headers);   // mutableCopy is +1; setField retained again
        b->headers = R(x).retain(x.cpu, b->headers);
        b->timeout = a->timeout;
        b->cachePolicy = a->cachePolicy;
        b->networkServiceType = a->networkServiceType;
        b->handleCookies = a->handleCookies;
        b->pipelining = a->pipelining;
        b->allowsCellular = a->allowsCellular;
    }
    return o;
}
id req_mutableCopy(Ctx& x, id self, SEL s, uint32_t z) {
    const id o = req_copy(x, self, s, z);
    x.mem.write<uint32_t>(o, LS(x).mutableRequest);   // same ivars, mutable class
    return o;
}
void req_dealloc(Ctx& x, id self, SEL) {
    if (Request* r = req(x, self))
        for (id o : {r->url, r->method, r->body, r->bodyStream, r->mainDoc, r->headers}) if (o) R(x).release(x.cpu, o);
    R(x).disposeInstance(x.cpu, self);
}

// ---- NSMutableURLRequest setters
void mreq_setURL(Ctx& x, id self, SEL, id v) { if (Request* r = req(x, self)) setField(x, r->url, v); }
void mreq_setMethod(Ctx& x, id self, SEL, id v) { if (Request* r = req(x, self)) setField(x, r->method, v); }
void mreq_setBody(Ctx& x, id self, SEL, id v) { if (Request* r = req(x, self)) setField(x, r->body, v); }
void mreq_setBodyStream(Ctx& x, id self, SEL, id v) { if (Request* r = req(x, self)) setField(x, r->bodyStream, v); }
void mreq_setMainDoc(Ctx& x, id self, SEL, id v) { if (Request* r = req(x, self)) setField(x, r->mainDoc, v); }
void mreq_setTimeout(Ctx& x, id self, SEL, double v) { if (Request* r = req(x, self)) r->timeout = v; }
void mreq_setCachePolicy(Ctx& x, id self, SEL, uint32_t v) { if (Request* r = req(x, self)) r->cachePolicy = v; }
void mreq_setServiceType(Ctx& x, id self, SEL, uint32_t v) { if (Request* r = req(x, self)) r->networkServiceType = v; }
void mreq_setHandleCookies(Ctx& x, id self, SEL, bool v) { if (Request* r = req(x, self)) r->handleCookies = v; }
void mreq_setPipelining(Ctx& x, id self, SEL, bool v) { if (Request* r = req(x, self)) r->pipelining = v; }
void mreq_setAllowsCellular(Ctx& x, id self, SEL, bool v) { if (Request* r = req(x, self)) r->allowsCellular = v; }
void mreq_setValue(Ctx& x, id self, SEL, id value, id field) {
    Request* r = req(x, self);
    if (!r || !field) return;
    if (value) R(x).send(x.cpu, r->headers, "setObject:forKey:", {value, field});
    else R(x).send(x.cpu, r->headers, "removeObjectForKey:", {field});
}
void mreq_addValue(Ctx& x, id self, SEL, id value, id field) {
    Request* r = req(x, self);
    if (!r || !field || !value) return;
    const id existing = dictGet(x, r->headers, field);
    if (existing) {
        const id combined = makeString(x, utf8(x, existing) + "," + utf8(x, value));
        R(x).send(x.cpu, r->headers, "setObject:forKey:", {combined, field});
    } else R(x).send(x.cpu, r->headers, "setObject:forKey:", {value, field});
}
void mreq_setHeaders(Ctx& x, id self, SEL, id dict) {
    Request* r = req(x, self);
    if (!r) return;
    const id m = dict ? id(R(x).send(x.cpu, dict, "mutableCopy")) : makeDict(x, {}, true, true);
    setField(x, r->headers, m);
    R(x).release(x.cpu, m);
}

// ---- NSError helper
id notConnectedError(Ctx& x, id url) {
    std::vector<std::pair<id, id>> info = {{constString(x.proc, "NSLocalizedDescription"), makeString(x, "The Internet connection appears to be offline.")}};
    if (url) {
        info.push_back({constString(x.proc, "NSErrorFailingURLStringKey"), makeString(x, urlString(x, url))});
        info.push_back({constString(x.proc, "NSErrorFailingURLKey"), url});
    }
    const id ui = makeDict(x, info);
    return id(R(x).send(x.cpu, R(x).getClass("NSError"), "errorWithDomain:code:userInfo:",
                        {constString(x.proc, "NSURLErrorDomain"), uint32_t(kNotConnected), ui}));
}

// ---- NSURLConnection
struct Connection : ObjC::HostObject { id request = 0, delegate = 0; bool started = false; };
Connection* conn(Ctx& x, id c) { return c ? R(x).hostIf<Connection>(c) : nullptr; }

// Fire connection:didFailWithError: on the current run loop, soon.
void scheduleFailure(Ctx& x, id connection) {
    Connection* c = conn(x, connection);
    if (!c || !c->delegate) return;
    R(x).retain(x.cpu, connection);
    Process* p = &x.proc;
    runLoopForObject(x, 0).post([p, connection](CPU::Cpu& cc) {
        Ctx cx{*p, cc, p->mem()};
        Connection* c = conn(cx, connection);
        if (c && c->delegate && c->started && R(cx).respondsTo(c->delegate, R(cx).sel("connection:didFailWithError:"))) {
            const id err = notConnectedError(cx, req(cx, c->request) ? req(cx, c->request)->url : 0);
            R(cx).send(cc, c->delegate, "connection:didFailWithError:", {connection, err});
        }
        R(cx).release(cc, connection);
    });
}
id conn_init(Ctx& x, id self, SEL, id request, id delegate, bool start) {
    auto h = std::make_unique<Connection>();
    h->request = retainObj(x, request);
    h->delegate = delegate;
    h->started = start;
    R(x).setHost(self, std::move(h));
    if (start) scheduleFailure(x, self);
    return self;
}
id conn_initDefault(Ctx& x, id self, SEL s, id request, id delegate) { return conn_init(x, self, s, request, delegate, true); }
id connc_with(Ctx& x, id cls, SEL s, id request, id delegate) {
    const id o = id(R(x).send(x.cpu, cls, R(x).selAlloc));
    return autoreleased(x, conn_init(x, o, s, request, delegate, true));
}
void conn_start(Ctx& x, id self, SEL) { if (Connection* c = conn(x, self)) { if (!c->started) { c->started = true; scheduleFailure(x, self); } } }
void conn_cancel(Ctx& x, id self, SEL) { if (Connection* c = conn(x, self)) c->started = false; }
void conn_schedule(Ctx&, id, SEL, id, id) {}
void conn_dealloc(Ctx& x, id self, SEL) {
    if (Connection* c = conn(x, self)) if (c->request) R(x).release(x.cpu, c->request);
    R(x).disposeInstance(x.cpu, self);
}
// + sendSynchronousRequest:returningResponse:error: → nil, offline error.
id connc_sync(Ctx& x, id, SEL, id request, GuestPtr responseOut, GuestPtr errorOut) {
    if (responseOut) x.mem.write<uint32_t>(responseOut, 0);
    if (errorOut) x.mem.write<uint32_t>(errorOut, notConnectedError(x, req(x, request) ? req(x, request)->url : 0));
    return 0;
}
// + sendAsynchronousRequest:queue:completionHandler: → completion(nil, nil, error) on the run loop.
void connc_async(Ctx& x, id, SEL, id request, id, uint32_t handler) {
    if (!handler) return;
    const uint32_t block = ObjC::blockCopy(x, handler);
    const id url = req(x, request) ? req(x, request)->url : 0;
    if (url) R(x).retain(x.cpu, url);
    Process* p = &x.proc;
    runLoopForObject(x, 0).post([p, block, url](CPU::Cpu& cc) {
        Ctx cx{*p, cc, p->mem()};
        const id err = notConnectedError(cx, url);
        ObjC::callBlock(cx, block, {0, 0, err});
        ObjC::blockRelease(cx, block);
        if (url) R(cx).release(cc, url);
    });
}
bool connc_canHandle(Ctx&, id, SEL, id) { return true; }

// ---- NSURLCache (a no-op cache)
id cachec_shared(Ctx& x, id cls, SEL) {
    static const char sharedKey = 0; uint32_t& shared = x.proc.processSlot(&sharedKey);
    if (!shared) { shared = R(x).createInstance(LS(x).cache); R(x).makeImmortal(shared); }
    return shared;
}
id cache_init(Ctx&, id self, SEL) { return self; }
id cache_initWith(Ctx&, id self, SEL, uint32_t, uint32_t, id) { return self; }
id cache_cachedResponse(Ctx&, id, SEL, id) { return 0; }
void cache_store(Ctx&, id, SEL, id, id) {}
void cache_removeAll(Ctx&, id, SEL) {}
void cache_remove(Ctx&, id, SEL, id) {}
uint32_t cache_zero(Ctx&, id, SEL) { return 0; }
void cache_setZero(Ctx&, id, SEL, uint32_t) {}

} // namespace

void registerURLLoading(Process& p, ObjC::Runtime& r) {
    UrlLoadState& s = p.service<UrlLoadState>();
    ObjC::ClassBuilder rq = r.defineClass("NSURLRequest", "NSObject");
    s.request = rq.cls();
    rq.cm<&reqc_with>("requestWithURL:").cm<&reqc_withPolicy>("requestWithURL:cachePolicy:timeoutInterval:", "@@:@Id")
      .m<&req_initWithURL>("initWithURL:").m<&req_initWithPolicy>("initWithURL:cachePolicy:timeoutInterval:", "@@:@Id")
      .m<&req_URL>("URL").m<&req_method>("HTTPMethod").m<&req_body>("HTTPBody").m<&req_bodyStream>("HTTPBodyStream")
      .m<&req_mainDoc>("mainDocumentURL").m<&req_cachePolicy>("cachePolicy", "I@:").m<&req_timeout>("timeoutInterval", "d@:")
      .m<&req_serviceType>("networkServiceType", "I@:").m<&req_handleCookies>("HTTPShouldHandleCookies", "c@:")
      .m<&req_pipelining>("HTTPShouldUsePipelining", "c@:").m<&req_allowsCellular>("allowsCellularAccess", "c@:")
      .m<&req_headers>("allHTTPHeaderFields").m<&req_headerValue>("valueForHTTPHeaderField:")
      .m<&req_copy>("copyWithZone:").m<&req_mutableCopy>("mutableCopyWithZone:").m<&req_dealloc>("dealloc", "v@:");

    ObjC::ClassBuilder mrq = r.defineClass("NSMutableURLRequest", "NSURLRequest");
    s.mutableRequest = mrq.cls();
    mrq.m<&mreq_setURL>("setURL:", "v@:@").m<&mreq_setMethod>("setHTTPMethod:", "v@:@").m<&mreq_setBody>("setHTTPBody:", "v@:@")
       .m<&mreq_setBodyStream>("setHTTPBodyStream:", "v@:@").m<&mreq_setMainDoc>("setMainDocumentURL:", "v@:@")
       .m<&mreq_setTimeout>("setTimeoutInterval:", "v@:d").m<&mreq_setCachePolicy>("setCachePolicy:", "v@:I")
       .m<&mreq_setServiceType>("setNetworkServiceType:", "v@:I").m<&mreq_setHandleCookies>("setHTTPShouldHandleCookies:", "v@:c")
       .m<&mreq_setPipelining>("setHTTPShouldUsePipelining:", "v@:c").m<&mreq_setAllowsCellular>("setAllowsCellularAccess:", "v@:c")
       .m<&mreq_setValue>("setValue:forHTTPHeaderField:", "v@:@@").m<&mreq_addValue>("addValue:forHTTPHeaderField:", "v@:@@")
       .m<&mreq_setHeaders>("setAllHTTPHeaderFields:", "v@:@");

    ObjC::ClassBuilder cn = r.defineClass("NSURLConnection", "NSObject");
    s.connection = cn.cls();
    cn.cm<&connc_with>("connectionWithRequest:delegate:")
      .cm<&connc_sync>("sendSynchronousRequest:returningResponse:error:", "@@:@^@^@")
      .cm<&connc_async>("sendAsynchronousRequest:queue:completionHandler:", "v@:@@@?")
      .cm<&connc_canHandle>("canHandleRequest:", "c@:@")
      .m<&conn_initDefault>("initWithRequest:delegate:").m<&conn_init>("initWithRequest:delegate:startImmediately:", "@@:@@c")
      .m<&conn_start>("start", "v@:").m<&conn_cancel>("cancel", "v@:")
      .m<&conn_schedule>("scheduleInRunLoop:forMode:", "v@:@@").m<&conn_schedule>("unscheduleFromRunLoop:forMode:", "v@:@@")
      .m<&conn_dealloc>("dealloc", "v@:");

    ObjC::ClassBuilder ca = r.defineClass("NSURLCache", "NSObject");
    s.cache = ca.cls();
    ca.cm<&cachec_shared>("sharedURLCache").cm<&cache_removeAll>("setSharedURLCache:", "v@:@")
      .m<&cache_init>("init").m<&cache_initWith>("initWithMemoryCapacity:diskCapacity:diskPath:", "@@:II@")
      .m<&cache_cachedResponse>("cachedResponseForRequest:").m<&cache_store>("storeCachedResponse:forRequest:", "v@:@@")
      .m<&cache_removeAll>("removeAllCachedResponses", "v@:").m<&cache_remove>("removeCachedResponseForRequest:", "v@:@")
      .m<&cache_zero>("memoryCapacity", "I@:").m<&cache_setZero>("setMemoryCapacity:", "v@:I")
      .m<&cache_zero>("diskCapacity", "I@:").m<&cache_setZero>("setDiskCapacity:", "v@:I")
      .m<&cache_zero>("currentMemoryUsage", "I@:").m<&cache_zero>("currentDiskUsage", "I@:");

    for (const char* n : {"_NSURLErrorDomain", "_NSURLErrorFailingURLErrorKey", "_NSURLErrorFailingURLStringErrorKey",
                          "_NSURLAuthenticationMethodDefault", "_NSURLAuthenticationMethodHTTPBasic"}) {
        const uint32_t cell = p.defineDataBlock(n, 4);
        p.mem().write<uint32_t>(cell, constString(p, n + 1));
    }
}

} // namespace Hydrogen::Runtime::Foundation
