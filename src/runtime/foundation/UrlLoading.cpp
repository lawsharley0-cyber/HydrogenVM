// HydrogenVM — NSURLRequest / NSMutableURLRequest / NSURLConnection. Offline unless the player
// allowed the game online (then the host's HTTP client fetches: libc/HttpClient.hpp); shut-down
// servers with a stand-in are answered locally either way (ServerStandIn.cpp).
//
// Requests are real value objects (URL, method, headers, body, cache/timeout
// policy), so apps build and inspect them. Connections behave like a device
// with no network: an async NSURLConnection reports failure to its delegate on
// the run loop with NSURLErrorNotConnectedToInternet (-1009), and the
// synchronous loader returns nil with that error. This lets apps that check for
// an error carry on, instead of hanging or crashing on a nil request.
#include "Foundation.hpp"
#include "runtime/Support.hpp"
#include <cstring>
#include <memory>
#include <future>
#include <strings.h>
#include <cstdio>
#include "runtime/libc/HttpClient.hpp"

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

// ---- Online downloads (the player allowed this game online; libc/HttpClient.hpp)
std::shared_ptr<HttpClient> onlineClient(Ctx& x) {
    return x.proc.config().internetAccess ? x.proc.config().httpClient : nullptr;
}
HttpRequest toHttp(Ctx& x, Request* r) {
    HttpRequest h;
    h.url = urlString(x, r->url);
    h.method = r->method ? utf8(x, r->method) : "GET";
    h.timeoutSeconds = r->timeout > 0 ? r->timeout : 60;
    if (r->headers)
        for (auto& [k, v] : dictEntries(x, r->headers))
            if (k && v) h.headers.push_back({utf8(x, k), utf8(x, id(R(x).send(x.cpu, v, "description")))});
    if (r->body) { const auto b = dataBytes(x, r->body); h.body.assign(b.begin(), b.end()); }
    return h;
}
// "GET https://host/path" without the query (signed URLs stay out of shared logs).
std::string logUrl(const HttpRequest& h) { return h.method + " " + h.url.substr(0, h.url.find('?')); }
void logOnline(Ctx& x, const HttpRequest& h, const HttpResponse& r) {
    if (r.status) x.proc.logf(LogLevel::Info, "Internet: %s -> %d (%zu bytes)", logUrl(h).c_str(), r.status, r.body.size());
    else x.proc.logf(LogLevel::Info, "Internet: %s failed: %s", logUrl(h).c_str(), r.error.c_str());
}
id httpResponseObject(Ctx& x, id url, const HttpResponse& r) {
    std::vector<std::pair<id, id>> hs;
    for (auto& [k, v] : r.headers) hs.push_back({makeString(x, k), makeString(x, v)});
    const id headers = makeDict(x, hs);
    const id finalUrl = r.url.empty() ? url : id(R(x).send(x.cpu, R(x).getClass("NSURL"), "URLWithString:", {makeString(x, r.url)}));
    const id o = id(R(x).send(x.cpu, R(x).getClass("NSHTTPURLResponse"), R(x).selAlloc));
    return autoreleased(x, id(R(x).send(x.cpu, o, "initWithURL:statusCode:HTTPVersion:headerFields:",
                                         {finalUrl ? finalUrl : url, uint32_t(r.status), makeString(x, "HTTP/1.1"), headers})));
}
id onlineError(Ctx& x, id url, const HttpResponse& r) {
    std::vector<std::pair<id, id>> info = {{constString(x.proc, "NSLocalizedDescription"), makeString(x, r.error.empty() ? "network error" : r.error)}};
    if (url) {
        info.push_back({constString(x.proc, "NSErrorFailingURLStringKey"), makeString(x, urlString(x, url))});
        info.push_back({constString(x.proc, "NSErrorFailingURLKey"), url});
    }
    return id(R(x).send(x.cpu, R(x).getClass("NSError"), "errorWithDomain:code:userInfo:",
                        {constString(x.proc, "NSURLErrorDomain"), uint32_t(r.errorCode ? r.errorCode : -1), makeDict(x, info)}));
}
// Blocks this guest thread until the host answers (sendSynchronousRequest, contents of a URL).
HttpResponse fetchNow(Ctx& x, const std::shared_ptr<HttpClient>& client, HttpRequest h) {
    auto done = std::make_shared<std::promise<HttpResponse>>();
    auto result = done->get_future();
    client->fetch(h, [done](HttpResponse r) { done->set_value(std::move(r)); });
    BlockedScope blocked(x.proc, "a download (NSURLConnection / contents of a URL)");
    while (result.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready)
        if (x.proc.stopping()) { HttpResponse r; r.errorCode = -999; r.error = "cancelled"; return r; }
    HttpResponse r = result.get();
    logOnline(x, h, r);
    return r;
}

// ---- NSURLConnection
struct Connection : ObjC::HostObject { id request = 0, delegate = 0; bool started = false; };
Connection* conn(Ctx& x, id c) { return c ? R(x).hostIf<Connection>(c) : nullptr; }

// A stand-in server's reply (ServerStandIn.cpp), delivered the way a real connection does on
// the run loop: connection:didReceiveResponse:, connection:didReceiveData: (one piece),
// connectionDidFinishLoading:.
bool scheduleStandIn(Ctx& x, id connection) {
    Connection* c = conn(x, connection);
    Request* r = c ? req(x, c->request) : nullptr;
    if (!c || !c->delegate || !r || !r->url) return false;
    auto reply = standInFetch(x, r->url, r->method ? utf8(x, r->method) : "GET");
    if (!reply) return false;
    R(x).retain(x.cpu, connection);
    Process* p = &x.proc;
    auto body = std::make_shared<StandInReply>(std::move(*reply));
    runLoopForObject(x, 0).post([p, connection, body](CPU::Cpu& cc) {
        Ctx cx{*p, cc, p->mem()};
        Connection* c = conn(cx, connection);
        if (c && c->delegate && c->started) {
            const id del = retainObj(cx, c->delegate);
            const id url = req(cx, c->request) ? req(cx, c->request)->url : 0;
            const uint32_t pool = R(cx).poolPush();
            auto says = [&](const char* sel) { return R(cx).respondsTo(del, R(cx).sel(sel)); };
            if (says("connection:didReceiveResponse:"))
                R(cx).send(cc, del, "connection:didReceiveResponse:", {connection, standInResponse(cx, url, *body)});
            if (conn(cx, connection) && conn(cx, connection)->started && says("connection:didReceiveData:"))
                R(cx).send(cc, del, "connection:didReceiveData:", {connection, makeData(cx, body->body.data(), uint32_t(body->body.size()))});
            if (conn(cx, connection) && conn(cx, connection)->started && says("connectionDidFinishLoading:"))
                R(cx).send(cc, del, "connectionDidFinishLoading:", {connection});
            R(cx).poolPop(cc, pool);
            if (Connection* c2 = conn(cx, connection)) setField(cx, c2->delegate, 0);
            R(cx).release(cc, del);
        }
        R(cx).release(cc, connection);
    });
    return true;
}
// A real download for a game allowed online: the host fetches, then the delegate hears
// connection:didReceiveResponse:, connection:didReceiveData:, connectionDidFinishLoading: (or
// connection:didFailWithError:) on the run loop that started it.
bool scheduleOnline(Ctx& x, id connection) {
    Connection* c = conn(x, connection);
    Request* r = c ? req(x, c->request) : nullptr;
    const auto client = onlineClient(x);
    if (!c || !c->delegate || !r || !r->url || !client) return false;
    R(x).retain(x.cpu, connection);
    Process* p = &x.proc;
    RunLoop* loop = &runLoopForObject(x, 0);
    const HttpRequest h = toHttp(x, r);
    client->fetch(h, [p, loop, connection, h](HttpResponse resp) {
        auto shared = std::make_shared<HttpResponse>(std::move(resp));
        loop->post([p, connection, h, shared](CPU::Cpu& cc) {
            Ctx cx{*p, cc, p->mem()};
            logOnline(cx, h, *shared);
            Connection* c = conn(cx, connection);
            if (c && c->delegate && c->started) {
                const id del = retainObj(cx, c->delegate);
                const id url = req(cx, c->request) ? req(cx, c->request)->url : 0;
                const uint32_t pool = R(cx).poolPush();
                auto says = [&](const char* sel) { return R(cx).respondsTo(del, R(cx).sel(sel)); };
                auto live = [&] { return conn(cx, connection) && conn(cx, connection)->started; };
                if (!shared->status) {
                    if (says("connection:didFailWithError:"))
                        R(cx).send(cc, del, "connection:didFailWithError:", {connection, onlineError(cx, url, *shared)});
                } else {
                    if (says("connection:didReceiveResponse:"))
                        R(cx).send(cc, del, "connection:didReceiveResponse:", {connection, httpResponseObject(cx, url, *shared)});
                    if (live() && !shared->body.empty() && says("connection:didReceiveData:"))
                        R(cx).send(cc, del, "connection:didReceiveData:", {connection, makeData(cx, shared->body.data(), uint32_t(shared->body.size()))});
                    if (live() && says("connectionDidFinishLoading:"))
                        R(cx).send(cc, del, "connectionDidFinishLoading:", {connection});
                }
                R(cx).poolPop(cc, pool);
                if (Connection* c2 = conn(cx, connection)) setField(cx, c2->delegate, 0);
                R(cx).release(cc, del);
            }
            R(cx).release(cc, connection);
        });
    });
    return true;
}
// Fire connection:didFailWithError: on the current run loop, soon.
void scheduleFailure(Ctx& x, id connection) {
    if (scheduleStandIn(x, connection)) return;
    if (scheduleOnline(x, connection)) return;
    Connection* c = conn(x, connection);
    if (!c || !c->delegate) return;
    R(x).retain(x.cpu, connection);
    Process* p = &x.proc;
    runLoopForObject(x, 0).post([p, connection](CPU::Cpu& cc) {
        Ctx cx{*p, cc, p->mem()};
        Connection* c = conn(cx, connection);
        if (c && c->delegate && c->started) {
            const id del = retainObj(cx, c->delegate);
            const id url = req(cx, c->request) ? req(cx, c->request)->url : 0;
            if (R(cx).respondsTo(del, R(cx).sel("connection:didFailWithError:"))) {
                const id err = notConnectedError(cx, url);
                R(cx).send(cc, del, "connection:didFailWithError:", {connection, err});
            }
            if (Connection* c2 = conn(cx, connection)) setField(cx, c2->delegate, 0);
            R(cx).release(cc, del);
        }
        R(cx).release(cc, connection);
    });
}
id conn_init(Ctx& x, id self, SEL, id request, id delegate, bool start) {
    auto h = std::make_unique<Connection>();
    h->request = retainObj(x, request);
    h->delegate = retainObj(x, delegate);
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
void conn_cancel(Ctx& x, id self, SEL) { if (Connection* c = conn(x, self)) { c->started = false; setField(x, c->delegate, 0); } }
void conn_schedule(Ctx&, id, SEL, id, id) {}
void conn_setDelegateQueue(Ctx&, id, SEL, id) {}
void conn_dealloc(Ctx& x, id self, SEL) {
    if (Connection* c = conn(x, self)) {
        if (c->request) R(x).release(x.cpu, c->request);
        if (c->delegate) R(x).release(x.cpu, c->delegate);
    }
    R(x).disposeInstance(x.cpu, self);
}
// + sendSynchronousRequest:returningResponse:error: → nil, offline error.
id connc_sync(Ctx& x, id, SEL, id request, GuestPtr responseOut, GuestPtr errorOut) {
    if (responseOut) x.mem.write<uint32_t>(responseOut, 0);
    if (Request* r = req(x, request); r && r->url) {
        if (auto reply = standInFetch(x, r->url, r->method ? utf8(x, r->method) : "GET")) {
            if (responseOut) x.mem.write<uint32_t>(responseOut, standInResponse(x, r->url, *reply));
            if (errorOut) x.mem.write<uint32_t>(errorOut, 0);
            return makeData(x, reply->body.data(), uint32_t(reply->body.size()));
        }
    }
    if (Request* r = req(x, request); r && r->url) {
        if (const auto client = onlineClient(x)) {
            const HttpResponse resp = fetchNow(x, client, toHttp(x, r));
            if (!resp.status) { if (errorOut) x.mem.write<uint32_t>(errorOut, onlineError(x, r->url, resp)); return 0; }
            if (responseOut) x.mem.write<uint32_t>(responseOut, httpResponseObject(x, r->url, resp));
            if (errorOut) x.mem.write<uint32_t>(errorOut, 0);
            return makeData(x, resp.body.data(), uint32_t(resp.body.size()));
        }
    }
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
    if (auto reply = url ? standInFetch(x, url, req(x, request)->method ? utf8(x, req(x, request)->method) : "GET") : std::nullopt) {
        auto body = std::make_shared<StandInReply>(std::move(*reply));
        runLoopForObject(x, 0).post([p, block, url, body](CPU::Cpu& cc) {
            Ctx cx{*p, cc, p->mem()};
            const uint32_t pool = R(cx).poolPush();
            ObjC::callBlock(cx, block, {standInResponse(cx, url, *body), makeData(cx, body->body.data(), uint32_t(body->body.size())), 0});
            R(cx).poolPop(cc, pool);
            ObjC::blockRelease(cx, block);
            R(cx).release(cc, url);
        });
        return;
    }
    if (const auto client = url ? onlineClient(x) : nullptr) {
        RunLoop* loop = &runLoopForObject(x, 0);
        const HttpRequest h = toHttp(x, req(x, request));
        client->fetch(h, [p, loop, block, url, h](HttpResponse resp) {
            auto shared = std::make_shared<HttpResponse>(std::move(resp));
            loop->post([p, block, url, h, shared](CPU::Cpu& cc) {
                Ctx cx{*p, cc, p->mem()};
                logOnline(cx, h, *shared);
                const uint32_t pool = R(cx).poolPush();
                if (shared->status)
                    ObjC::callBlock(cx, block, {httpResponseObject(cx, url, *shared), makeData(cx, shared->body.data(), uint32_t(shared->body.size())), 0});
                else
                    ObjC::callBlock(cx, block, {0, 0, onlineError(cx, url, *shared)});
                R(cx).poolPop(cc, pool);
                ObjC::blockRelease(cx, block);
                R(cx).release(cc, url);
            });
        });
        return;
    }
    runLoopForObject(x, 0).post([p, block, url](CPU::Cpu& cc) {
        Ctx cx{*p, cc, p->mem()};
        const id err = notConnectedError(cx, url);
        ObjC::callBlock(cx, block, {0, 0, err});
        ObjC::blockRelease(cx, block);
        if (url) R(cx).release(cc, url);
    });
}
bool connc_canHandle(Ctx&, id, SEL, id) { return true; }

// ---- NSURLSession (iOS 7): sessions, data and download tasks, completion handlers and
// delegates. A task fetches the way NSURLConnection does (stand-in server, then the host's
// HTTP client when the game is allowed online, else the offline error). Results are delivered
// on the main run loop (a delegate queue is not modelled).
struct SessionConfig : ObjC::HostObject { id headers = 0; double timeout = 60; bool background = false; };
struct Session : ObjC::HostObject { id config = 0, delegate = 0, queue = 0; bool invalid = false; };
struct Task : ObjC::HostObject {
    id session = 0, request = 0, response = 0, error = 0, description = 0;
    uint32_t handler = 0;      // a copied completion block, or 0 (delegate)
    int32_t state = 1;         // NSURLSessionTaskState: 0 running, 1 suspended, 2 canceling, 3 completed
    uint32_t ident = 0;
    bool download = false, cancelled = false, started = false;
    int64_t received = 0, expected = -1;
    float priority = 0.5f;
};
struct SessionState {
    explicit SessionState(Process&) {}
    Class config = 0, session = 0, task = 0, dataTask = 0, downloadTask = 0;
    id shared = 0;
    uint32_t nextTask = 1;
    uint32_t noopBlock = 0;
};
SessionState& SS(Ctx& x) { return x.proc.service<SessionState>(); }
SessionConfig* scfg(Ctx& x, id o) { return o ? R(x).hostIf<SessionConfig>(o) : nullptr; }
Session* sess(Ctx& x, id o) { return o ? R(x).hostIf<Session>(o) : nullptr; }
Task* task(Ctx& x, id o) { return o ? R(x).hostIf<Task>(o) : nullptr; }

// A global block that does nothing: the completionHandler handed to a delegate's
// URLSession:dataTask:didReceiveResponse:completionHandler: (the answer does not matter here).
void noopInvoke(Ctx&) {}
uint32_t noopBlock(Ctx& x) {
    auto& st = SS(x);
    if (st.noopBlock) return st.noopBlock;
    const uint32_t desc = x.proc.heap().allocZeroed(8);
    x.mem.write<uint32_t>(desc + 4, 20);   // size of the block literal
    const uint32_t b = x.proc.heap().allocZeroed(20);
    const auto isa = x.proc.lookupSymbol("__NSConcreteGlobalBlock");
    x.mem.write<uint32_t>(b, isa ? x.mem.read<uint32_t>(*isa) : 0);
    x.mem.write<uint32_t>(b + 4, 1u << 28);   // BLOCK_IS_GLOBAL
    x.mem.write<uint32_t>(b + 12, x.proc.lookupSymbol("__hvm_url_session_noop_block").value_or(0));
    x.mem.write<uint32_t>(b + 16, desc);
    st.noopBlock = b;
    return b;
}

// NSURLSessionConfiguration
id newConfig(Ctx& x, bool background) {
    const id o = R(x).createInstance(SS(x).config);
    auto h = std::make_unique<SessionConfig>();
    h->background = background;
    R(x).setHost(o, std::move(h));
    return o;
}
id cfgc_default(Ctx& x, id, SEL) { return autoreleased(x, newConfig(x, false)); }
id cfgc_background(Ctx& x, id, SEL, id) { return autoreleased(x, newConfig(x, true)); }
id cfg_copy(Ctx& x, id self, SEL, uint32_t) {
    SessionConfig* c = scfg(x, self);
    const id o = newConfig(x, c && c->background);
    if (c) { SessionConfig* n = scfg(x, o); n->timeout = c->timeout; n->headers = retainObj(x, c->headers); }
    return o;
}
void cfg_setHeaders(Ctx& x, id self, SEL, id d) {
    SessionConfig* c = scfg(x, self);
    if (!c) return;
    const id copy = d ? id(R(x).send(x.cpu, d, "copy")) : 0;   // +1, owned by the configuration
    if (c->headers) R(x).release(x.cpu, c->headers);
    c->headers = copy;
}
id cfg_headers(Ctx& x, id self, SEL) { SessionConfig* c = scfg(x, self); return c ? c->headers : 0; }
void cfg_setTimeout(Ctx& x, id self, SEL, double t) { if (SessionConfig* c = scfg(x, self)) c->timeout = t; }
double cfg_timeout(Ctx& x, id self, SEL) { SessionConfig* c = scfg(x, self); return c ? c->timeout : 60; }
void cfg_ignoreId(Ctx&, id, SEL, id) {}
void cfg_ignoreInt(Ctx&, id, SEL, uint32_t) {}
void cfg_ignoreDouble(Ctx&, id, SEL, double) {}
void cfg_dealloc(Ctx& x, id self, SEL) {
    if (SessionConfig* c = scfg(x, self); c && c->headers) R(x).release(x.cpu, c->headers);
    R(x).disposeInstance(x.cpu, self);
}

// NSURLSession
id newSession(Ctx& x, id config, id delegate, id queue) {
    const id o = R(x).createInstance(SS(x).session);
    auto h = std::make_unique<Session>();
    h->config = config ? id(R(x).send(x.cpu, config, "copy")) : newConfig(x, false);
    h->delegate = retainObj(x, delegate);   // a session keeps its delegate until invalidated
    h->queue = retainObj(x, queue);
    R(x).setHost(o, std::move(h));
    return o;
}
id sesc_shared(Ctx& x, id, SEL) {
    auto& st = SS(x);
    if (!st.shared) { st.shared = newSession(x, 0, 0, 0); R(x).makeImmortal(st.shared); }
    return st.shared;
}
id sesc_withConfig(Ctx& x, id, SEL, id config) { return autoreleased(x, newSession(x, config, 0, 0)); }
id sesc_withConfigDelegate(Ctx& x, id, SEL, id config, id delegate, id queue) { return autoreleased(x, newSession(x, config, delegate, queue)); }
id ses_config(Ctx& x, id self, SEL) { Session* s = sess(x, self); return s ? s->config : 0; }
id ses_delegate(Ctx& x, id self, SEL) { Session* s = sess(x, self); return s ? s->delegate : 0; }
id ses_queue(Ctx& x, id self, SEL) { Session* s = sess(x, self); return s ? s->queue : 0; }
void ses_invalidate(Ctx& x, id self, SEL) {
    Session* s = sess(x, self);
    if (!s || s->invalid || self == SS(x).shared) return;
    s->invalid = true;
    const id del = s->delegate;
    s->delegate = 0;
    if (del) {
        if (R(x).respondsTo(del, R(x).sel("URLSession:didBecomeInvalidWithError:")))
            R(x).send(x.cpu, del, "URLSession:didBecomeInvalidWithError:", {self, 0});
        R(x).release(x.cpu, del);
    }
}
void ses_getTasks(Ctx& x, id self, SEL, uint32_t handler) {
    if (!handler) return;
    const id empty = makeArray(x, {});
    ObjC::callBlock(x, handler, {empty, empty, empty});
}
void ses_dealloc(Ctx& x, id self, SEL) {
    if (Session* s = sess(x, self)) for (id* f : {&s->config, &s->delegate, &s->queue}) if (*f) R(x).release(x.cpu, *f);
    R(x).disposeInstance(x.cpu, self);
}

id newTask(Ctx& x, id session, id request, uint32_t handler, bool download) {
    const id o = R(x).createInstance(download ? SS(x).downloadTask : SS(x).dataTask);
    auto h = std::make_unique<Task>();
    h->session = retainObj(x, session);
    h->request = retainObj(x, request);
    h->handler = handler ? ObjC::blockCopy(x, handler) : 0;
    h->download = download;
    h->ident = SS(x).nextTask++;
    R(x).setHost(o, std::move(h));
    return autoreleased(x, o);
}
id requestFor(Ctx& x, id url) { return autoreleased(x, makeRequest(x, url, false, true)); }
id ses_dataReq(Ctx& x, id self, SEL, id rq) { return newTask(x, self, rq, 0, false); }
id ses_dataReqH(Ctx& x, id self, SEL, id rq, uint32_t h) { return newTask(x, self, rq, h, false); }
id ses_dataUrl(Ctx& x, id self, SEL, id url) { return newTask(x, self, requestFor(x, url), 0, false); }
id ses_dataUrlH(Ctx& x, id self, SEL, id url, uint32_t h) { return newTask(x, self, requestFor(x, url), h, false); }
id ses_downReq(Ctx& x, id self, SEL, id rq) { return newTask(x, self, rq, 0, true); }
id ses_downReqH(Ctx& x, id self, SEL, id rq, uint32_t h) { return newTask(x, self, rq, h, true); }
id ses_downUrl(Ctx& x, id self, SEL, id url) { return newTask(x, self, requestFor(x, url), 0, true); }
id ses_downUrlH(Ctx& x, id self, SEL, id url, uint32_t h) { return newTask(x, self, requestFor(x, url), h, true); }

// The result of a task, then delivery on the main run loop.
void deliverTask(Ctx& x, id t, std::shared_ptr<HttpResponse> resp, bool offline) {
    Task* k = task(x, t);
    if (!k) return;
    Session* s = sess(x, k->session);
    const id url = req(x, k->request) ? req(x, k->request)->url : 0;
    const uint32_t pool = R(x).poolPush();
    id error = 0, response = 0, data = 0, location = 0;
    if (!offline && url && !resp->url.empty() && !k->cancelled) {
        HttpRequest h; h.method = req(x, k->request)->method ? utf8(x, req(x, k->request)->method) : "GET"; h.url = urlString(x, url);
        if (resp->status) x.proc.logf(LogLevel::Info, "Internet: %s -> %d (%zu bytes, NSURLSession)", logUrl(h).c_str(), resp->status, resp->body.size());
        else x.proc.logf(LogLevel::Info, "Internet: %s failed: %s (NSURLSession)", logUrl(h).c_str(), resp->error.c_str());
    }
    if (k->cancelled) {
        HttpResponse c; c.errorCode = -999; c.error = "cancelled";
        error = onlineError(x, url, c);
    } else if (offline) error = notConnectedError(x, url);
    else if (!resp->status) error = onlineError(x, url, *resp);
    else {
        response = httpResponseObject(x, url, *resp);
        data = makeData(x, resp->body.data(), uint32_t(resp->body.size()));
        k->received = int64_t(resp->body.size());
        k->expected = k->received;
        if (k->download) {
            // A download lands in tmp/ as a file the handler may move; removed afterwards, as on iOS.
            const std::string path = x.proc.homePath() + "/tmp/CFNetworkDownload_" + std::to_string(k->ident) + ".tmp";
            if (std::FILE* f = std::fopen(path.c_str(), "wb")) { std::fwrite(resp->body.data(), 1, resp->body.size(), f); std::fclose(f); }
            location = id(R(x).send(x.cpu, R(x).getClass("NSURL"), "fileURLWithPath:", {makeString(x, path)}));
        }
    }
    setField(x, k->response, response);
    setField(x, k->error, error);
    k->state = 3;
    if (k->handler) {
        if (k->download) ObjC::callBlock(x, k->handler, {location, response, error});
        else ObjC::callBlock(x, k->handler, {data, response, error});
    } else if (s && s->delegate && !s->invalid) {
        const id del = retainObj(x, s->delegate);
        auto says = [&](const char* sel) { return R(x).respondsTo(del, R(x).sel(sel)); };
        if (response && !k->download) {
            if (says("URLSession:dataTask:didReceiveResponse:completionHandler:"))
                R(x).send(x.cpu, del, "URLSession:dataTask:didReceiveResponse:completionHandler:", {k->session, t, response, noopBlock(x)});
            if (!resp->body.empty() && says("URLSession:dataTask:didReceiveData:"))
                R(x).send(x.cpu, del, "URLSession:dataTask:didReceiveData:", {k->session, t, data});
        }
        if (response && k->download && says("URLSession:downloadTask:didFinishDownloadingToURL:"))
            R(x).send(x.cpu, del, "URLSession:downloadTask:didFinishDownloadingToURL:", {k->session, t, location});
        if (says("URLSession:task:didCompleteWithError:"))
            R(x).send(x.cpu, del, "URLSession:task:didCompleteWithError:", {k->session, t, error});
        R(x).release(x.cpu, del);
    }
    if (location) std::remove(utf8(x, id(R(x).send(x.cpu, location, "path"))).c_str());
    R(x).poolPop(x.cpu, pool);
}
void task_resume(Ctx& x, id self, SEL) {
    Task* k = task(x, self);
    if (!k || k->started || k->state != 1) { if (k && k->state == 1) k->state = 0; return; }
    k->started = true;
    k->state = 0;
    R(x).retain(x.cpu, self);   // a running task stays alive until it is done
    Process* p = &x.proc;
    RunLoop* loop = &mainRunLoop(x.proc);
    Request* r = req(x, k->request);
    const id url = r ? r->url : 0;
    auto finish = [p, loop, self](std::shared_ptr<HttpResponse> resp, bool offline) {
        loop->post([p, self, resp, offline](CPU::Cpu& cc) {
            Ctx cx{*p, cc, p->mem()};
            deliverTask(cx, self, resp, offline);
            R(cx).release(cc, self);
        });
    };
    const std::string method = r && r->method ? utf8(x, r->method) : "GET";
    if (auto reply = url ? standInFetch(x, url, method) : std::nullopt) {
        auto resp = std::make_shared<HttpResponse>();
        resp->status = reply->status;
        resp->url = urlString(x, url);
        resp->headers = {{"Content-Type", reply->mime}, {"Content-Length", std::to_string(reply->body.size())}};
        resp->body = std::move(reply->body);
        finish(resp, false);
        return;
    }
    const auto client = url ? onlineClient(x) : nullptr;
    if (!client) { finish(std::make_shared<HttpResponse>(), true); return; }
    HttpRequest h = toHttp(x, r);
    if (SessionConfig* c = scfg(x, sess(x, k->session) ? sess(x, k->session)->config : 0)) {
        if (c->headers)
            for (auto& [hk, hv] : dictEntries(x, c->headers)) {
                const std::string key = utf8(x, hk);
                bool have = false;
                for (auto& [ek, ev] : h.headers) if (strcasecmp(ek.c_str(), key.c_str()) == 0) have = true;
                if (!have) h.headers.push_back({key, utf8(x, id(R(x).send(x.cpu, hv, "description")))});
            }
        if (r && r->timeout >= 60 && c->timeout > 0) h.timeoutSeconds = c->timeout;
    }
    client->fetch(h, [finish](HttpResponse resp) { finish(std::make_shared<HttpResponse>(std::move(resp)), false); });
}
void task_cancel(Ctx& x, id self, SEL) {
    Task* k = task(x, self);
    if (!k || k->state == 3) return;
    k->cancelled = true;
    if (!k->started) { k->state = 1; task_resume(x, self, 0); }   // a cancelled task still reports -999
    k->state = 2;
}
void task_suspend(Ctx& x, id self, SEL) { if (Task* k = task(x, self); k && !k->started) k->state = 1; }
int32_t task_state(Ctx& x, id self, SEL) { Task* k = task(x, self); return k ? k->state : 3; }
uint32_t task_ident(Ctx& x, id self, SEL) { Task* k = task(x, self); return k ? k->ident : 0; }
id task_request(Ctx& x, id self, SEL) { Task* k = task(x, self); return k ? k->request : 0; }
id task_response(Ctx& x, id self, SEL) { Task* k = task(x, self); return k ? k->response : 0; }
id task_error(Ctx& x, id self, SEL) { Task* k = task(x, self); return k ? k->error : 0; }
int64_t task_received(Ctx& x, id self, SEL) { Task* k = task(x, self); return k ? k->received : 0; }
int64_t task_expected(Ctx& x, id self, SEL) { Task* k = task(x, self); return k ? k->expected : -1; }
float task_priority(Ctx& x, id self, SEL) { Task* k = task(x, self); return k ? k->priority : 0.5f; }
void task_setPriority(Ctx& x, id self, SEL, float v) { if (Task* k = task(x, self)) k->priority = v; }
id task_description(Ctx& x, id self, SEL) { Task* k = task(x, self); return k ? k->description : 0; }
void task_setDescription(Ctx& x, id self, SEL, id d) { if (Task* k = task(x, self)) setField(x, k->description, d); }
void task_dealloc(Ctx& x, id self, SEL) {
    if (Task* k = task(x, self)) {
        for (id* f : {&k->session, &k->request, &k->response, &k->error, &k->description}) if (*f) R(x).release(x.cpu, *f);
        if (k->handler) ObjC::blockRelease(x, k->handler);
    }
    R(x).disposeInstance(x.cpu, self);
}

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

std::optional<std::string> onlineFetchContents(Ctx& x, id url) {
    const auto client = onlineClient(x);
    if (!client || !url) return std::nullopt;
    HttpRequest h;
    h.url = urlString(x, url);
    const HttpResponse r = fetchNow(x, client, h);
    if (r.status < 200 || r.status >= 300) return std::nullopt;   // NSData returns nil for an error page too
    return r.body;
}

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

    supportDef<&noopInvoke>(p, "__hvm_url_session_noop_block", SupportStatus::Real,
                            "the invoke of a block that does nothing, handed to a delegate's completionHandler: argument");
    SessionState& ss = p.service<SessionState>();
    ObjC::ClassBuilder sc = r.defineClass("NSURLSessionConfiguration", "NSObject");
    ss.config = sc.cls();
    sc.cm<&cfgc_default>("defaultSessionConfiguration").cm<&cfgc_default>("ephemeralSessionConfiguration")
      .cm<&cfgc_background>("backgroundSessionConfiguration:").cm<&cfgc_background>("backgroundSessionConfigurationWithIdentifier:")
      .m<&cfg_copy>("copyWithZone:").m<&cfg_setHeaders>("setHTTPAdditionalHeaders:", "v@:@").m<&cfg_headers>("HTTPAdditionalHeaders")
      .m<&cfg_setTimeout>("setTimeoutIntervalForRequest:", "v@:d").m<&cfg_timeout>("timeoutIntervalForRequest", "d@:")
      .m<&cfg_dealloc>("dealloc", "v@:");
    {
        // Settings that change caching, cookies, cellular use, pipelining and the like: accepted
        // and ignored, because the host's client fetches every request fresh, without cookies.
        struct Ignored { const char* sel; const char* types; };
        const Ignored ignoredInts[] = {
            {"setRequestCachePolicy:", "v@:I"}, {"setHTTPShouldSetCookies:", "v@:c"}, {"setAllowsCellularAccess:", "v@:c"},
            {"setHTTPMaximumConnectionsPerHost:", "v@:i"}, {"setTLSMinimumSupportedProtocol:", "v@:i"}, {"setDiscretionary:", "v@:c"},
            {"setSessionSendsLaunchEvents:", "v@:c"}, {"setNetworkServiceType:", "v@:I"}, {"setHTTPShouldUsePipelining:", "v@:c"},
            {"setHTTPCookieAcceptPolicy:", "v@:I"}};
        for (const Ignored& i : ignoredInts)
            supportMethod(p, sc, "NSURLSessionConfiguration", i.sel, [](Ctx& x) { callWrapped<&cfg_ignoreInt>(x); }, SupportStatus::Approx,
                          "accepted and ignored: the host's HTTP client fetches each request fresh, without cookies or caching", i.types);
        const char* ignoredObjects[] = {"setURLCache:", "setHTTPCookieStorage:", "setURLCredentialStorage:", "setProtocolClasses:",
                                        "setConnectionProxyDictionary:", "setSharedContainerIdentifier:"};
        for (const char* sel : ignoredObjects)
            supportMethod(p, sc, "NSURLSessionConfiguration", sel, [](Ctx& x) { callWrapped<&cfg_ignoreId>(x); }, SupportStatus::Approx,
                          "accepted and ignored: no caches, cookie stores, custom protocols or proxies are modelled", "v@:@");
        supportMethod(p, sc, "NSURLSessionConfiguration", "setTimeoutIntervalForResource:", [](Ctx& x) { callWrapped<&cfg_ignoreDouble>(x); },
                      SupportStatus::Approx, "accepted and ignored: only the per-request timeout is applied", "v@:d");
    }
    ObjC::ClassBuilder se = r.defineClass("NSURLSession", "NSObject");
    ss.session = se.cls();
    se.cm<&sesc_shared>("sharedSession").cm<&sesc_withConfig>("sessionWithConfiguration:")
      .cm<&sesc_withConfigDelegate>("sessionWithConfiguration:delegate:delegateQueue:")
      .m<&ses_config>("configuration").m<&ses_delegate>("delegate").m<&ses_queue>("delegateQueue")
      .m<&ses_dataReq>("dataTaskWithRequest:").m<&ses_dataReqH>("dataTaskWithRequest:completionHandler:", "@@:@@?")
      .m<&ses_dataUrl>("dataTaskWithURL:").m<&ses_dataUrlH>("dataTaskWithURL:completionHandler:", "@@:@@?")
      .m<&ses_downReq>("downloadTaskWithRequest:").m<&ses_downReqH>("downloadTaskWithRequest:completionHandler:", "@@:@@?")
      .m<&ses_downUrl>("downloadTaskWithURL:").m<&ses_downUrlH>("downloadTaskWithURL:completionHandler:", "@@:@@?")
      .m<&ses_invalidate>("invalidateAndCancel", "v@:").m<&ses_invalidate>("finishTasksAndInvalidate", "v@:")
      .m<&ses_getTasks>("getTasksWithCompletionHandler:", "v@:@?").m<&ses_dealloc>("dealloc", "v@:");
    ObjC::ClassBuilder tk = r.defineClass("NSURLSessionTask", "NSObject");
    ss.task = tk.cls();
    tk.m<&task_resume>("resume", "v@:").m<&task_cancel>("cancel", "v@:").m<&task_suspend>("suspend", "v@:")
      .m<&task_state>("state", "i@:").m<&task_ident>("taskIdentifier", "I@:")
      .m<&task_request>("originalRequest").m<&task_request>("currentRequest").m<&task_response>("response").m<&task_error>("error")
      .m<&task_received>("countOfBytesReceived", "q@:").m<&task_expected>("countOfBytesExpectedToReceive", "q@:")
      .m<&task_priority>("priority", "f@:").m<&task_setPriority>("setPriority:", "v@:f")
      .m<&task_description>("taskDescription").m<&task_setDescription>("setTaskDescription:", "v@:@")
      .m<&task_dealloc>("dealloc", "v@:");
    ss.dataTask = r.defineClass("NSURLSessionDataTask", "NSURLSessionTask").cls();
    ss.downloadTask = r.defineClass("NSURLSessionDownloadTask", "NSURLSessionTask").cls();
    {   // const int64_t NSURLSessionTransferSizeUnknown = -1
        const uint32_t unknown = p.defineDataBlock("_NSURLSessionTransferSizeUnknown", 8);
        p.mem().write<uint32_t>(unknown, 0xFFFFFFFFu);
        p.mem().write<uint32_t>(unknown + 4, 0xFFFFFFFFu);
    }

    ObjC::ClassBuilder cn = r.defineClass("NSURLConnection", "NSObject");
    s.connection = cn.cls();
    cn.cm<&connc_with>("connectionWithRequest:delegate:")
      .cm<&connc_sync>("sendSynchronousRequest:returningResponse:error:", "@@:@^@^@")
      .cm<&connc_async>("sendAsynchronousRequest:queue:completionHandler:", "v@:@@@?")
      .cm<&connc_canHandle>("canHandleRequest:", "c@:@")
      .m<&conn_initDefault>("initWithRequest:delegate:").m<&conn_init>("initWithRequest:delegate:startImmediately:", "@@:@@c")
      .m<&conn_start>("start", "v@:").m<&conn_cancel>("cancel", "v@:")
      .m<&conn_schedule>("scheduleInRunLoop:forMode:", "v@:@@").m<&conn_schedule>("unscheduleFromRunLoop:forMode:", "v@:@@")
      .m<&conn_setDelegateQueue>("setDelegateQueue:", "v@:@")
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
