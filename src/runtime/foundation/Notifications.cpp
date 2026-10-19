// HydrogenVM — NSNotification, NSNotificationCenter (selector and block
// observers), NSNotificationQueue (basic), CFNotificationCenter.
#include "Foundation.hpp"

namespace Hydrogen::Runtime::Foundation {

namespace {

struct Observer {
    uint64_t serial = 0;
    id observer = 0;       // not retained (pre-iOS 9 semantics); for blocks: the token object
    SEL sel = 0;
    std::u16string name;   // empty = any
    bool anyName = true;
    id object = 0;         // 0 = any
    id block = 0;          // copied block (block observers)
    id queue = 0;          // NSOperationQueue for block observers
    uint32_t cfCallback = 0, cfCenter = 0;   // CFNotificationCenter observers
    bool removed = false;
};

struct Centers {
    explicit Centers(Process&) {}
    Class notification = 0, center = 0, token = 0, nqueue = 0;
    id defaultCenter = 0, darwinCenter = 0, defaultQueue = 0;
    std::recursive_mutex mu;
    std::vector<std::shared_ptr<Observer>> observers;
    uint64_t nextSerial = 1;
};
Centers& C(Process& p) { return p.service<Centers>(); }
Centers& C(Ctx& x) { return C(x.proc); }

struct NoteData : ObjC::HostObject { id name = 0, object = 0, userInfo = 0; };
NoteData* nd(Ctx& x, id n) { return n ? rt(x).hostIf<NoteData>(n) : nullptr; }

id newNote(Ctx& x, id name, id object, id userInfo, bool owned = false) {
    const id n = rt(x).createInstance(C(x).notification);
    auto h = std::make_unique<NoteData>();
    h->name = id(rt(x).send(x.cpu, name, "copy"));
    h->object = rt(x).retain(x.cpu, object);
    h->userInfo = rt(x).retain(x.cpu, userInfo);
    rt(x).setHost(n, std::move(h));
    return owned ? n : autoreleased(x, n);
}
id nc_notification(Ctx& x, id, SEL, id name, id object) { return newNote(x, name, object, 0); }
id nc_notificationInfo(Ctx& x, id, SEL, id name, id object, id info) { return newNote(x, name, object, info); }
id n_initWith(Ctx& x, id self, SEL, id name, id object, id info) {
    rt(x).disposeInstance(x.cpu, self);
    return newNote(x, name, object, info, true);
}
id n_name(Ctx& x, id self, SEL) { NoteData* d = nd(x, self); return d ? d->name : id(rt(x).send(x.cpu, self, "name")); }
id n_object(Ctx& x, id self, SEL) { NoteData* d = nd(x, self); return d ? d->object : 0; }
id n_userInfo(Ctx& x, id self, SEL) { NoteData* d = nd(x, self); return d ? d->userInfo : 0; }
id n_copy(Ctx& x, id self, SEL, uint32_t) { return rt(x).retain(x.cpu, self); }
id n_description(Ctx& x, id self, SEL) {
    NoteData* d = nd(x, self);
    char buf[64];
    std::snprintf(buf, sizeof buf, "NSConcreteNotification 0x%x {name = ", self);
    std::string s = buf + (d ? describe(x, d->name) : std::string("?"));
    if (d && d->object) s += "; object = " + describe(x, d->object);
    if (d && d->userInfo) s += "; userInfo = " + describe(x, d->userInfo);
    return makeString(x, s + "}");
}
void n_dealloc(Ctx& x, id self, SEL) {
    if (NoteData* d = nd(x, self)) {
        const id a = d->name, b = d->object, c = d->userInfo;
        d->name = d->object = d->userInfo = 0;
        rt(x).release(x.cpu, a); rt(x).release(x.cpu, b); rt(x).release(x.cpu, c);
    }
    rt(x).disposeInstance(x.cpu, self);
}

// ---------------------------------------------------------- the center ---

id center_default(Ctx& x, id, SEL) {
    auto& c = C(x);
    if (!c.defaultCenter) { c.defaultCenter = rt(x).createInstance(c.center); rt(x).makeImmortal(c.defaultCenter); }
    return c.defaultCenter;
}

std::shared_ptr<Observer> addObserver(Ctx& x, Observer o) {
    auto& c = C(x);
    auto p = std::make_shared<Observer>(std::move(o));
    std::lock_guard<std::recursive_mutex> lk(c.mu);
    p->serial = c.nextSerial++;
    c.observers.push_back(p);
    return p;
}
void center_add(Ctx& x, id, SEL, id observer, SEL sel, id name, id object) {
    if (!observer || !sel) return;
    Observer o;
    o.observer = observer;
    o.sel = sel;
    o.anyName = !name;
    if (name) o.name = stringValue(x, name);
    o.object = object;
    addObserver(x, std::move(o));
}
id center_addBlock(Ctx& x, id, SEL, id name, id object, id queue, id block) {
    const id token = rt(x).createInstance(C(x).token);
    Observer o;
    o.observer = token;
    o.anyName = !name;
    if (name) o.name = stringValue(x, name);
    o.object = object;
    o.block = ObjC::blockCopy(x, block);
    o.queue = rt(x).retain(x.cpu, queue);
    addObserver(x, std::move(o));
    return autoreleased(x, token);   // the center keeps the observer alive until removed
}
void removeMatching(Ctx& x, id observer, id name, id object, bool matchName, bool matchObject) {
    auto& c = C(x);
    std::vector<std::shared_ptr<Observer>> gone;
    {
        std::lock_guard<std::recursive_mutex> lk(c.mu);
        const std::u16string n = name ? stringValue(x, name) : std::u16string();
        for (auto it = c.observers.begin(); it != c.observers.end();) {
            Observer& o = **it;
            const bool match = o.observer == observer && (!matchName || !name || (!o.anyName && o.name == n)) &&
                               (!matchObject || !object || o.object == object);
            if (match) { o.removed = true; gone.push_back(*it); it = c.observers.erase(it); }
            else ++it;
        }
    }
    for (auto& o : gone) {
        if (o->block) ObjC::blockRelease(x, o->block);
        rt(x).release(x.cpu, o->queue);
    }
}
void center_remove(Ctx& x, id, SEL, id observer) { removeMatching(x, observer, 0, 0, false, false); }
void center_removeNamed(Ctx& x, id, SEL, id observer, id name, id object) { removeMatching(x, observer, name, object, true, true); }

void deliver(Ctx& x, const Observer& o, id note) {
    if (o.cfCallback) {
        NoteData* d = nd(x, note);
        x.proc.callGuest(x.cpu, o.cfCallback, {o.cfCenter, o.observer, d ? d->name : 0, d ? d->object : 0, d ? d->userInfo : 0});
        return;
    }
    if (o.block) {
        if (o.queue) {
            // Run on the observer's operation queue (asynchronously, like Foundation),
            // keeping the note and block alive until then.
            const id blk = ObjC::blockCopy(x, o.block);
            rt(x).retain(x.cpu, note);
            enqueueOnOperationQueue(x, o.queue, [blk, note](Ctx& cx) {
                ObjC::callBlock(cx, blk, {note});
                ObjC::blockRelease(cx, blk);
                rt(cx).release(cx.cpu, note);
            });
            return;
        }
        ObjC::callBlock(x, o.block, {note});
        return;
    }
    rt(x).send(x.cpu, o.observer, o.sel, {note});
}

void post(Ctx& x, id note) {
    NoteData* d = nd(x, note);
    const id name = d ? d->name : id(rt(x).send(x.cpu, note, "name"));
    const id object = d ? d->object : id(rt(x).send(x.cpu, note, "object"));
    const std::u16string n = name ? stringValue(x, name) : std::u16string();
    std::vector<std::shared_ptr<Observer>> targets;
    {
        auto& c = C(x);
        std::lock_guard<std::recursive_mutex> lk(c.mu);
        for (auto& o : c.observers) {
            if (o->cfCallback) continue;
            if ((o->anyName || o->name == n) && (!o->object || o->object == object)) targets.push_back(o);
        }
    }
    rt(x).retain(x.cpu, note);
    for (auto& o : targets) if (!o->removed) deliver(x, *o, note);
    rt(x).release(x.cpu, note);
}
void center_post(Ctx& x, id, SEL, id note) { if (note) post(x, note); }
void center_postName(Ctx& x, id, SEL, id name, id object) { post(x, newNote(x, name, object, 0)); }
void center_postNameInfo(Ctx& x, id, SEL, id name, id object, id info) { post(x, newNote(x, name, object, info)); }
void token_dealloc(Ctx& x, id self, SEL) { rt(x).disposeInstance(x.cpu, self); }

// --------------------------------------------------- NSNotificationQueue ---

id nq_default(Ctx& x, id, SEL) {
    auto& c = C(x);
    if (!c.defaultQueue) { c.defaultQueue = rt(x).createInstance(c.nqueue); rt(x).makeImmortal(c.defaultQueue); }
    return c.defaultQueue;
}
id nq_init(Ctx&, id self, SEL, id) { return self; }
// postingStyle: 1 NSPostWhenIdle, 2 NSPostASAP, 3 NSPostNow. Coalescing is not implemented.
void nq_enqueue(Ctx& x, id, SEL, id note, uint32_t style) {
    if (style == 3) { post(x, note); return; }
    rt(x).retain(x.cpu, note);
    Process* p = &x.proc;
    mainRunLoop(x.proc).post([p, note](CPU::Cpu& c) {
        Ctx cx{*p, c, p->mem()};
        post(cx, note);
        rt(cx).release(c, note);
    });
}
void nq_enqueueFull(Ctx& x, id self, SEL s, id note, uint32_t style, uint32_t, id) { nq_enqueue(x, self, s, note, style); }
void nq_dequeue(Ctx&, id, SEL, id, uint32_t) {}

// ------------------------------------------------- CFNotificationCenter ---

uint32_t cfn_local(Ctx& x) { return center_default(x, 0, 0); }
uint32_t cfn_darwin(Ctx& x) {
    auto& c = C(x);
    if (!c.darwinCenter) { c.darwinCenter = rt(x).createInstance(c.center); rt(x).makeImmortal(c.darwinCenter); }
    return c.darwinCenter;
}
void cfn_add(Ctx& x, uint32_t center, uint32_t observer, uint32_t callback, id name, id object, uint32_t) {
    Observer o;
    o.observer = observer;
    o.cfCallback = callback;
    o.cfCenter = center;
    o.anyName = !name;
    if (name) o.name = stringValue(x, name);
    o.object = object;
    addObserver(x, std::move(o));
}
void cfn_remove(Ctx& x, uint32_t, uint32_t observer, id name, id object) { removeMatching(x, observer, name, object, true, true); }
void cfn_removeAll(Ctx& x, uint32_t, uint32_t observer) { removeMatching(x, observer, 0, 0, false, false); }
void cfn_post(Ctx& x, uint32_t center, id name, id object, id userInfo, bool) {
    const std::u16string n = name ? stringValue(x, name) : std::u16string();
    std::vector<std::shared_ptr<Observer>> targets;
    {
        auto& c = C(x);
        std::lock_guard<std::recursive_mutex> lk(c.mu);
        for (auto& o : c.observers)
            if (o->cfCallback && o->cfCenter == center && (o->anyName || o->name == n) && (!o->object || o->object == object)) targets.push_back(o);
    }
    for (auto& o : targets) if (!o->removed) x.proc.callGuest(x.cpu, o->cfCallback, {center, o->observer, name, object, userInfo});
    // CFNotificationCenter posts on the local center are also seen by NSNotificationCenter observers.
    if (center == C(x).defaultCenter) post(x, newNote(x, name, object, userInfo));
}
void cfn_postOpts(Ctx& x, uint32_t center, id name, id object, id userInfo, uint32_t) { cfn_post(x, center, name, object, userInfo, true); }

} // namespace

void postNotification(Ctx& x, id name, id object, id userInfo) { post(x, newNote(x, name, object, userInfo)); }
void postNotification(Ctx& x, const std::string& name, id object, id userInfo) { postNotification(x, constString(x.proc, name), object, userInfo); }

void registerNotifications(Process& p, ObjC::Runtime& r) {
    auto& c = C(p);
    ObjC::ClassBuilder note = r.defineClass("NSNotification", "NSObject");
    c.notification = note.cls();
    note.cm<&nc_notification>("notificationWithName:object:").cm<&nc_notificationInfo>("notificationWithName:object:userInfo:");
    note.m<&n_initWith>("initWithName:object:userInfo:").m<&n_name>("name").m<&n_object>("object").m<&n_userInfo>("userInfo")
        .m<&n_copy>("copyWithZone:").m<&n_description>("description").m<&n_dealloc>("dealloc", "v@:");

    ObjC::ClassBuilder center = r.defineClass("NSNotificationCenter", "NSObject");
    c.center = center.cls();
    center.cm<&center_default>("defaultCenter");
    center.m<&center_add>("addObserver:selector:name:object:", "v@:@:@@")
          .m<&center_addBlock>("addObserverForName:object:queue:usingBlock:")
          .m<&center_remove>("removeObserver:", "v@:@").m<&center_removeNamed>("removeObserver:name:object:", "v@:@@@")
          .m<&center_post>("postNotification:", "v@:@").m<&center_postName>("postNotificationName:object:", "v@:@@")
          .m<&center_postNameInfo>("postNotificationName:object:userInfo:", "v@:@@@");
    ObjC::ClassBuilder token = r.defineClass("__NSObserver", "NSObject");
    c.token = token.cls();
    token.m<&token_dealloc>("dealloc", "v@:");

    ObjC::ClassBuilder nq = r.defineClass("NSNotificationQueue", "NSObject");
    c.nqueue = nq.cls();
    nq.cm<&nq_default>("defaultQueue");
    nq.m<&nq_init>("initWithNotificationCenter:").m<&nq_enqueue>("enqueueNotification:postingStyle:", "v@:@I")
      .m<&nq_enqueueFull>("enqueueNotification:postingStyle:coalesceMask:forModes:", "v@:@II@")
      .m<&nq_dequeue>("dequeueNotificationsMatching:coalesceMask:", "v@:@I");

    p.def<&cfn_local>("_CFNotificationCenterGetLocalCenter");
    p.def<&cfn_darwin>("_CFNotificationCenterGetDarwinNotifyCenter");
    p.def<&cfn_add>("_CFNotificationCenterAddObserver");
    p.def<&cfn_remove>("_CFNotificationCenterRemoveObserver");
    p.def<&cfn_removeAll>("_CFNotificationCenterRemoveEveryObserver");
    p.def<&cfn_post>("_CFNotificationCenterPostNotification");
    p.def<&cfn_postOpts>("_CFNotificationCenterPostNotificationWithOptions");
}

} // namespace Hydrogen::Runtime::Foundation
