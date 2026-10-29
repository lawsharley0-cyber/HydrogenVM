// HydrogenVM — nib loading. Reads compiled iOS nibs in both formats ibtool
// produces — the binary "NIBArchive" format (iOS 4+) and the older
// NSKeyedArchiver plist — into one object graph, decodes it through a
// UINibDecoder (NSCoder) so UIKit and app classes run their own
// -initWithCoder:, resolves File's Owner / external-object proxies, applies
// outlet and action connections and sends -awakeFromNib.
#include "UIKit.hpp"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace Hydrogen::Runtime::UIKit {

using Foundation::autoreleased;
using Foundation::makeString;
using Foundation::utf8;

namespace {

struct NValue {
    enum Type { Int, Double, Bool, Data, Nil, Ref, String, Refs } type = Nil;
    int64_t i = 0;
    double d = 0;
    std::string s;                  // Data / String bytes
    uint32_t ref = 0;
    std::vector<uint32_t> refs;     // keyed-archive arrays of object references
};
struct NObject {
    std::string cls;
    std::vector<std::pair<std::string, NValue>> values;
    bool inlineString = false, inlineNumber = false, inlineData = false;   // keyed-archive plain $objects
    NValue inlineValue;
    const NValue* get(const std::string& k) const {
        for (auto& [kk, v] : values) if (kk == k) return &v;
        return nullptr;
    }
};
struct NibGraph {
    std::vector<NObject> objects;
    uint32_t root = 0;
};

// ------------------------------------------------------------ NIBArchive ---

struct Reader {
    const std::vector<uint8_t>& b;
    size_t p = 0;
    bool bad = false;
    uint8_t u8() { if (p >= b.size()) { bad = true; return 0; } return b[p++]; }
    uint32_t u32() { uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= uint32_t(u8()) << (i * 8); return v; }
    uint64_t u64() { uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= uint64_t(u8()) << (i * 8); return v; }
    uint32_t varint() {
        uint32_t v = 0;
        for (int shift = 0; shift < 35; shift += 7) {
            const uint8_t c = u8();
            v |= uint32_t(c & 0x7F) << shift;
            if (c & 0x80) break;   // the high bit marks the last byte in NIBArchive
            if (bad) break;
        }
        return v;
    }
    std::string bytes(size_t n) {
        if (p + n > b.size()) { bad = true; return {}; }
        std::string s(reinterpret_cast<const char*>(b.data() + p), n);
        p += n;
        return s;
    }
};

bool parseNibArchive(const std::vector<uint8_t>& data, NibGraph& g, std::string& err) {
    if (data.size() < 50 || std::memcmp(data.data(), "NIBArchive", 10) != 0) { err = "not a NIBArchive"; return false; }
    Reader r{data, 10};
    r.u32(); r.u32();   // format / coder version
    const uint32_t objCount = r.u32(), objOff = r.u32(), keyCount = r.u32(), keyOff = r.u32();
    const uint32_t valCount = r.u32(), valOff = r.u32(), clsCount = r.u32(), clsOff = r.u32();
    if (r.bad || objCount > 1000000 || valCount > 10000000) { err = "corrupt NIBArchive header"; return false; }
    std::vector<std::string> keys(keyCount), classes(clsCount);
    r.p = keyOff;
    for (auto& k : keys) { const uint32_t n = r.varint(); k = r.bytes(n); }
    r.p = clsOff;
    for (auto& c : classes) {
        const uint32_t n = r.varint(), extra = r.varint();
        for (uint32_t i = 0; i < extra; ++i) r.u32();
        c = r.bytes(n);
        while (!c.empty() && c.back() == '\0') c.pop_back();
    }
    struct RawVal { uint32_t key; NValue v; };
    std::vector<RawVal> values(valCount);
    r.p = valOff;
    for (auto& rv : values) {
        rv.key = r.varint();
        const uint8_t type = r.u8();
        NValue& v = rv.v;
        switch (type) {
            case 0: v.type = NValue::Int; v.i = int8_t(r.u8()); break;
            case 1: { v.type = NValue::Int; const uint16_t a = uint16_t(r.u8() | r.u8() << 8); v.i = int16_t(a); break; }
            case 2: v.type = NValue::Int; v.i = int32_t(r.u32()); break;
            case 3: v.type = NValue::Int; v.i = int64_t(r.u64()); break;
            case 4: v.type = NValue::Bool; v.i = 1; break;
            case 5: v.type = NValue::Bool; v.i = 0; break;
            case 6: { v.type = NValue::Double; const uint32_t bits = r.u32(); float f; std::memcpy(&f, &bits, 4); v.d = f; break; }
            case 7: { v.type = NValue::Double; const uint64_t bits = r.u64(); std::memcpy(&v.d, &bits, 8); break; }
            case 8: { v.type = NValue::Data; const uint32_t n = r.varint(); v.s = r.bytes(n); break; }
            case 9: v.type = NValue::Nil; break;
            case 10: v.type = NValue::Ref; v.ref = r.u32(); break;
            default: err = "unknown NIBArchive value type " + std::to_string(type); return false;
        }
        if (r.bad) { err = "truncated NIBArchive values"; return false; }
    }
    g.objects.resize(objCount);
    r.p = objOff;
    for (auto& o : g.objects) {
        const uint32_t ci = r.varint(), vi = r.varint(), vn = r.varint();
        if (ci >= classes.size() || size_t(vi) + vn > values.size()) { err = "corrupt NIBArchive object table"; return false; }
        o.cls = classes[ci];
        for (uint32_t k = 0; k < vn; ++k) {
            const RawVal& rv = values[vi + k];
            o.values.emplace_back(rv.key < keys.size() ? keys[rv.key] : std::string(), rv.v);
        }
    }
    g.root = 0;
    return !r.bad;
}

// ------------------------------------------------- keyed-archive nibs ---

NValue fromPlist(const Plist::Value& v) {
    NValue n;
    using T = Plist::Value::Type;
    switch (v.type) {
        case T::Uid: n.type = NValue::Ref; n.ref = uint32_t(v.i); break;
        case T::Integer: n.type = NValue::Int; n.i = v.i; break;
        case T::Real: n.type = NValue::Double; n.d = v.r; break;
        case T::Bool: n.type = NValue::Bool; n.i = v.b; break;
        case T::Data: n.type = NValue::Data; n.s = v.s; break;
        case T::String: n.type = NValue::String; n.s = v.s; break;
        case T::Array:
            n.type = NValue::Refs;
            for (auto& e : v.array) if (e->type == T::Uid) n.refs.push_back(uint32_t(e->i));
            break;
        default: n.type = NValue::Nil;
    }
    return n;
}
bool parseKeyedNib(const std::vector<uint8_t>& data, NibGraph& g, std::string& err) {
    Plist::ValuePtr root;
    try { root = Plist::parse(data); } catch (const std::exception& e) { err = e.what(); return false; }
    if (!root || root->type != Plist::Value::Type::Dict) { err = "not a keyed archive"; return false; }
    const Plist::Value* objs = root->get("$objects");
    const Plist::Value* top = root->get("$top");
    if (!objs || !top) { err = "not a keyed archive"; return false; }
    // Object 0 is "$null"; we add a synthetic root object at the end holding $top.
    g.objects.resize(objs->array.size() + 1);
    for (size_t i = 0; i < objs->array.size(); ++i) {
        const Plist::Value& v = *objs->array[i];
        NObject& o = g.objects[i];
        if (v.type == Plist::Value::Type::Dict && v.get("$class")) {
            const Plist::Value* cref = v.get("$class");
            if (cref->type == Plist::Value::Type::Uid && size_t(cref->i) < objs->array.size())
                o.cls = objs->array[size_t(cref->i)]->getString("$classname");
            for (auto& [k, val] : v.dict) if (k != "$class") o.values.emplace_back(k, fromPlist(*val));
        } else if (v.type == Plist::Value::Type::String) {
            o.cls = v.s == "$null" ? "" : "NSString";
            o.inlineString = v.s != "$null";
            o.inlineValue = fromPlist(v);
        } else if (v.type == Plist::Value::Type::Data) {
            o.cls = "NSData"; o.inlineData = true; o.inlineValue = fromPlist(v);
        } else if (v.type == Plist::Value::Type::Integer || v.type == Plist::Value::Type::Real || v.type == Plist::Value::Type::Bool) {
            o.cls = "NSNumber"; o.inlineNumber = true; o.inlineValue = fromPlist(v);
        }
    }
    NObject& r = g.objects.back();
    r.cls = "__HVMNibRoot";
    const Plist::Value* od = top->get("IB.objectdata");
    if (od && od->type == Plist::Value::Type::Uid && size_t(od->i) < objs->array.size()) {
        // older nibs: the interesting keys live in the IB.objectdata object
        for (auto& kv : g.objects[size_t(od->i)].values) r.values.push_back(kv);
    } else {
        for (auto& [k, val] : top->dict) r.values.emplace_back(k, fromPlist(*val));
    }
    g.root = uint32_t(g.objects.size() - 1);
    return true;
}

// ---------------------------------------------------------- the decoder ---

struct DecoderData : ObjC::HostObject {
    std::shared_ptr<NibGraph> graph;
    bool keyed = false;                          // NSKeyedArchiver nib (ref 0 = nil)
    std::unordered_map<uint32_t, id> decoded;    // +1 references held until the load finishes
    std::vector<uint32_t> stack;                 // objects being decoded
    id owner = 0, externals = 0;
    std::vector<id> order;                       // decode order (for awakeFromNib)
    std::vector<uint32_t> proxies;
};
DecoderData& DD(Ctx& x, id d) { return rt(x).host<DecoderData>(d); }

id decodeRef(Ctx& x, id coder, uint32_t ref);

id valueToObject(Ctx& x, id coder, const NValue& v) {
    switch (v.type) {
        case NValue::Ref: return decodeRef(x, coder, v.ref);
        case NValue::Refs: {
            std::vector<id> items;
            for (uint32_t r : v.refs) { const id o = decodeRef(x, coder, r); items.push_back(o ? o : Foundation::nullObject(x.proc)); }
            return Foundation::makeArray(x, items);
        }
        case NValue::Int: return Foundation::makeNumberInt(x, v.i);
        case NValue::Double: return Foundation::makeNumberDouble(x, v.d);
        case NValue::Bool: return Foundation::makeBool(x, v.i != 0);
        case NValue::Data: return Foundation::makeData(x, v.s.data(), uint32_t(v.s.size()));
        case NValue::String: return makeString(x, v.s);
        default: return 0;
    }
}

const NObject* currentObject(Ctx& x, id coder) {
    DecoderData& d = DD(x, coder);
    if (d.stack.empty() || !d.graph) return nullptr;
    return &d.graph->objects[d.stack.back()];
}
const NValue* field(Ctx& x, id coder, id key) {
    const NObject* o = currentObject(x, coder);
    return o && key ? o->get(utf8(x, key)) : nullptr;
}

id decodeRef(Ctx& x, id coder, uint32_t ref) {
    DecoderData& d = DD(x, coder);
    NibGraph& g = *d.graph;
    if (ref >= g.objects.size() || (d.keyed && ref == 0)) return 0;
    if (auto it = d.decoded.find(ref); it != d.decoded.end()) return it->second;
    const NObject& o = g.objects[ref];
    auto& r = rt(x);
    id result = 0;
    auto elements = [&]() {
        std::vector<id> items;
        for (auto& [k, v] : o.values) {
            if (k == "UINibEncoderEmptyKey" || k == "NS.objects") {
                if (v.type == NValue::Refs) for (uint32_t rr : v.refs) { const id e = decodeRef(x, coder, rr); items.push_back(e ? e : Foundation::nullObject(x.proc)); }
                else { const id e = valueToObject(x, coder, v); items.push_back(e ? e : Foundation::nullObject(x.proc)); }
            }
        }
        return items;
    };
    const std::string& cls = o.cls;
    if (o.inlineString || o.inlineData || o.inlineNumber) {
        result = r.retain(x.cpu, valueToObject(x, coder, o.inlineValue));
    } else if (cls == "NSString" || cls == "NSMutableString" || cls == "NSLocalizableString") {
        const NValue* b = o.get("NS.bytes");
        if (!b) b = o.get("NS.string");
        result = makeString(x, b ? b->s : std::string(), true, cls == "NSMutableString");
    } else if (cls == "NSArray" || cls == "NSMutableArray") {
        result = Foundation::makeArray(x, elements(), true, cls == "NSMutableArray");
    } else if (cls == "NSSet" || cls == "NSMutableSet") {
        result = Foundation::makeSet(x, elements(), true, cls == "NSMutableSet");
    } else if (cls == "NSDictionary" || cls == "NSMutableDictionary") {
        std::vector<std::pair<id, id>> e;
        if (const NValue* ks = o.get("NS.keys")) {
            const NValue* vs = o.get("NS.objects");
            for (size_t i = 0; vs && i < ks->refs.size() && i < vs->refs.size(); ++i) e.emplace_back(decodeRef(x, coder, ks->refs[i]), decodeRef(x, coder, vs->refs[i]));
        } else {
            const auto items = elements();   // alternating key, value
            for (size_t i = 0; i + 1 < items.size(); i += 2) e.emplace_back(items[i], items[i + 1]);
        }
        result = Foundation::makeDict(x, e, true, cls == "NSMutableDictionary");
    } else if (cls == "NSNumber") {
        for (auto& [k, v] : o.values) if (v.type == NValue::Int || v.type == NValue::Double || v.type == NValue::Bool) { result = r.retain(x.cpu, valueToObject(x, coder, v)); break; }
        if (!result) result = Foundation::makeNumberInt(x, 0, true);
    } else if (cls == "NSData" || cls == "NSMutableData") {
        const NValue* b = o.get("NS.bytes");
        if (!b) b = o.get("NS.data");
        result = Foundation::makeData(x, b ? b->s.data() : nullptr, b ? uint32_t(b->s.size()) : 0, true, cls == "NSMutableData");
    } else if (cls == "NSNull") {
        result = Foundation::nullObject(x.proc);
    } else if (cls == "UIProxyObject") {
        const NValue* idv = o.get("UIProxiedObjectIdentifier");
        std::string ident = idv ? (idv->type == NValue::Ref ? textOf(x, decodeRef(x, coder, idv->ref)) : idv->s) : "";
        d.proxies.push_back(ref);
        if (ident == "IBFilesOwner") result = r.retain(x.cpu, d.owner);
        else if (ident == "IBFirstResponder") result = 0;
        else if (d.externals) result = r.retain(x.cpu, Foundation::dictGet(x, d.externals, makeString(x, ident)));
        if (!result && ident != "IBFirstResponder") x.proc.logf(LogLevel::Warning, "nib: unresolved proxy object %s", ident.c_str());
        d.decoded[ref] = result;
        return result;
    } else {
        std::string name = cls;
        if (cls == "UIClassSwapper") {
            const NValue* cn = o.get("UIClassName");
            name = cn ? (cn->type == NValue::Ref ? textOf(x, decodeRef(x, coder, cn->ref)) : cn->s) : "";
            if (!r.getClass(name)) {
                const NValue* orig = o.get("UIOriginalClassName");
                const std::string on = orig ? (orig->type == NValue::Ref ? textOf(x, decodeRef(x, coder, orig->ref)) : orig->s) : "";
                x.proc.logf(LogLevel::Warning, "nib: unknown class %s, using %s", name.c_str(), on.c_str());
                name = on;
            }
        } else if (cls == "UIImageNibPlaceholder") {
            const NValue* rn = o.get("UIResourceName");
            const std::string res = rn ? (rn->type == NValue::Ref ? textOf(x, decodeRef(x, coder, rn->ref)) : rn->s) : "";
            const id img = res.empty() ? 0 : id(r.send(x.cpu, ui(x).image, "imageNamed:", {makeString(x, res)}));
            if (!img && !res.empty()) x.proc.logf(LogLevel::Warning, "nib: image \"%s\" not found", res.c_str());
            result = img ? r.retain(x.cpu, img) : 0;
            d.decoded[ref] = result;
            return result;
        }
        Class c = r.getClass(name);
        if (!c) c = r.stubClass(name);
        id obj = id(r.send(x.cpu, c, r.selAlloc));
        d.decoded[ref] = obj;   // cycles see the allocated object
        d.stack.push_back(ref);
        id inited = 0;
        try {
            if (r.respondsTo(obj, r.sel("initWithCoder:"))) inited = id(r.send(x.cpu, obj, "initWithCoder:", {coder}));
            else inited = id(r.send(x.cpu, obj, r.selInit));
        } catch (...) { d.stack.pop_back(); throw; }
        d.stack.pop_back();
        result = inited;
        if (result != obj) d.decoded[ref] = result;
        if (result) d.order.push_back(result);
        return result;
    }
    d.decoded[ref] = result;
    return result;
}

// NSCoder interface
id nd_decodeObject(Ctx& x, id self, SEL, id key) { const NValue* v = field(x, self, key); return v ? valueToObject(x, self, *v) : 0; }
bool nd_contains(Ctx& x, id self, SEL, id key) { return field(x, self, key) != nullptr; }
int64_t asInt(const NValue* v) { if (!v) return 0; return v->type == NValue::Double ? int64_t(v->d) : v->i; }
double asDouble(const NValue* v) { if (!v) return 0; return v->type == NValue::Double ? v->d : double(v->i); }
int32_t nd_decodeInt(Ctx& x, id self, SEL, id key) { return int32_t(asInt(field(x, self, key))); }
int64_t nd_decodeInt64(Ctx& x, id self, SEL, id key) { return asInt(field(x, self, key)); }
bool nd_decodeBool(Ctx& x, id self, SEL, id key) { const NValue* v = field(x, self, key); return v && asInt(v) != 0; }
float nd_decodeFloat(Ctx& x, id self, SEL, id key) { return float(asDouble(field(x, self, key))); }
double nd_decodeDouble(Ctx& x, id self, SEL, id key) { return asDouble(field(x, self, key)); }
uint32_t nd_decodeBytes(Ctx& x, id self, SEL, id key, GuestPtr len) {
    const NValue* v = field(x, self, key);
    if (!v || v->type != NValue::Data) { if (len) x.mem.write<uint32_t>(len, 0); return 0; }
    const id d = Foundation::makeData(x, v->s.data(), uint32_t(v->s.size()));
    if (len) x.mem.write<uint32_t>(len, uint32_t(v->s.size()));
    return uint32_t(rt(x).send(x.cpu, d, "bytes"));
}
bool nd_true(Ctx&, id, SEL) { return true; }
void nd_dealloc(Ctx& x, id self, SEL) {
    if (DecoderData* d = rt(x).hostIf<DecoderData>(self)) {
        auto decoded = std::move(d->decoded);
        d->decoded.clear();
        for (auto& [k, o] : decoded) if (o) rt(x).release(x.cpu, o);
    }
    rt(x).disposeInstance(x.cpu, self);
}

// Connections
struct ConnData : ObjC::HostObject { id source = 0, destination = 0, label = 0; uint32_t mask = 0; bool collection = false; };
id conn_init(Ctx& x, id self, SEL, id coder) {
    ConnData& c = rt(x).host<ConnData>(self);
    auto& r = rt(x);
    auto obj = [&](const char* k) { return id(r.send(x.cpu, coder, "decodeObjectForKey:", {makeString(x, k)})); };
    c.source = obj("UISource");
    c.destination = obj("UIDestination");
    c.label = obj("UILabel");
    c.mask = uint32_t(r.send(x.cpu, coder, "decodeIntForKey:", {makeString(x, "UIEventMask")}));
    return self;
}
id coll_init(Ctx& x, id self, SEL s, id coder) {
    conn_init(x, self, s, coder);
    rt(x).host<ConnData>(self).collection = true;
    return self;
}

void applyOutlet(Ctx& x, id src, const std::string& key, id dst) {
    auto& r = rt(x);
    if (!src) return;
    std::string setter = "set" + key;
    setter[3] = char(std::toupper(uint8_t(setter[3])));
    setter += ":";
    if (r.respondsTo(src, r.sel(setter))) { r.send(x.cpu, src, setter.c_str(), {dst}); return; }
    try {
        r.send(x.cpu, src, "setValue:forKey:", {dst, makeString(x, key)});
    } catch (const ObjC::ObjCError& e) {
        x.proc.logf(LogLevel::Warning, "nib: can't connect outlet %s of %s: %s", key.c_str(), r.objectClassName(src).c_str(), e.what());
    }
}

std::string nibPath(Ctx& x, const std::string& name, id bundle) {
    std::string base = name;
    if (base.size() > 4 && base.substr(base.size() - 4) == ".nib") base.resize(base.size() - 4);
    std::string bundlePath = x.proc.bundlePath();
    if (bundle && rt(x).respondsTo(bundle, rt(x).sel("bundlePath"))) bundlePath = x.proc.hostPath(utf8(x, id(rt(x).send(x.cpu, bundle, "bundlePath"))));
    std::string p = bundlePath == x.proc.bundlePath() ? Foundation::bundleResource(x, base, "nib") : "";
    if (p.empty()) {
        std::error_code ec;
        for (const std::string& cand : {bundlePath + "/" + base + "~iphone.nib", bundlePath + "/" + base + ".nib"})
            if (fs::exists(cand, ec)) { p = cand; break; }
    }
    if (p.empty()) return {};
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
        // nib bundles: runtime.nib (iOS 6+), or objects-<version>.nib variants
        for (const char* f : {"runtime.nib", "objects-8.0+.nib", "objects-13.0+.nib", "objects-11.0+.nib", "objects.nib", "keyedobjects.nib"})
            if (fs::exists(fs::path(p) / f, ec)) return (fs::path(p) / f).string();
        for (auto& e : fs::directory_iterator(p, ec)) if (e.path().extension() == ".nib") return e.path().string();
        return {};
    }
    return p;
}

struct NibState { explicit NibState(Process&) {} Class decoder = 0, outlet = 0, event = 0, collection = 0, uinib = 0; };
NibState& NS(Ctx& x) { return x.proc.service<NibState>(); }

id loadGraph(Ctx& x, std::shared_ptr<NibGraph> g, bool keyed, id owner, id externals, const std::string& what) {
    auto& r = rt(x);
    const id coder = r.createInstance(NS(x).decoder);
    DecoderData& d = DD(x, coder);
    d.graph = std::move(g);
    d.keyed = keyed;
    d.owner = owner;
    d.externals = externals;
    // decode from the root object's keys
    d.stack.push_back(d.graph->root);
    auto rootObj = [&](const char* key) -> id {
        const NValue* v = d.graph->objects[d.graph->root].get(key);
        return v ? valueToObject(x, coder, *v) : 0;
    };
    id top = 0, connections = 0, visible = 0;
    try {
        top = rootObj("UINibTopLevelObjectsKey");
        rootObj("UINibObjectsKey");
        connections = rootObj("UINibConnectionsKey");
        visible = rootObj("UINibVisibleWindowsKey");
        if (!top && !connections) {
            // very old keyed nibs: IBDocument.RootObjects
            top = rootObj("IBDocument.RootObjects");
        }
    } catch (...) { r.release(x.cpu, coder); throw; }
    d.stack.pop_back();
    // connections
    for (id c : connections ? Foundation::arrayItems(x, connections) : std::vector<id>()) {
        ConnData* cd = r.hostIf<ConnData>(c);
        if (!cd) continue;
        const std::string label = textOf(x, cd->label);
        if (r.isKindOf(c, NS(x).event)) {
            if (cd->source) r.send(x.cpu, cd->source, "addTarget:action:forControlEvents:", {cd->destination, r.sel(label), cd->mask});
        } else {
            applyOutlet(x, cd->source, label, cd->destination);
        }
    }
    // windows marked visible at launch
    for (id w : visible ? Foundation::arrayItems(x, visible) : std::vector<id>()) r.send(x.cpu, w, "setHidden:", {0});
    // awakeFromNib, in decode order, skipping proxies and connections
    std::vector<id> awake = DD(x, coder).order;
    for (id o : awake) {
        if (r.isKindOf(o, NS(x).outlet) || r.isKindOf(o, NS(x).event) || r.isKindOf(o, NS(x).collection)) continue;
        if (r.respondsTo(o, r.sel("awakeFromNib"))) r.send(x.cpu, o, "awakeFromNib");
    }
    // top-level objects (excluding proxies) are returned autoreleased
    std::vector<id> result;
    for (id o : top ? Foundation::arrayItems(x, top) : std::vector<id>()) {
        if (o == owner || o == Foundation::nullObject(x.proc)) continue;
        result.push_back(o);
    }
    for (id o : result) { r.retain(x.cpu, o); autoreleased(x, o); }
    const id arr = Foundation::makeArray(x, result);
    r.release(x.cpu, coder);
    x.proc.logf(LogLevel::Debug, "nib %s: %zu top-level objects", what.c_str(), result.size());
    return arr;
}

id loadNibFile(Ctx& x, const std::string& path, id owner, id externals) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return 0;
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), {});
    auto g = std::make_shared<NibGraph>();
    std::string err;
    bool keyed = false;
    if (data.size() >= 10 && std::memcmp(data.data(), "NIBArchive", 10) == 0) {
        if (!parseNibArchive(data, *g, err)) { x.proc.logf(LogLevel::Error, "nib %s: %s", path.c_str(), err.c_str()); return 0; }
    } else {
        keyed = true;
        if (!parseKeyedNib(data, *g, err)) { x.proc.logf(LogLevel::Error, "nib %s: %s", path.c_str(), err.c_str()); return 0; }
    }
    return loadGraph(x, g, keyed, owner, externals, fs::path(path).filename().string());
}

// UINib / NSBundle API
struct UINibData : ObjC::HostObject { std::string path; std::vector<uint8_t> data; };
id un_withName(Ctx& x, id self, SEL, id name, id bundle) {
    const std::string p = nibPath(x, name ? utf8(x, name) : "", bundle);
    if (p.empty()) return 0;
    const id n = rt(x).createInstance(self);
    rt(x).host<UINibData>(n).path = p;
    return autoreleased(x, n);
}
id un_withData(Ctx& x, id self, SEL, id data, id) {
    const id n = rt(x).createInstance(self);
    rt(x).host<UINibData>(n).data = Foundation::dataBytes(x, data);
    return autoreleased(x, n);
}
id un_instantiate(Ctx& x, id self, SEL, id owner, id options) {
    UINibData& d = rt(x).host<UINibData>(self);
    const id externals = options ? Foundation::dictGet(x, options, "UINibExternalObjects") : 0;
    if (!d.path.empty()) return loadNibFile(x, d.path, owner, externals);
    auto g = std::make_shared<NibGraph>();
    std::string err;
    const bool archive = d.data.size() >= 10 && std::memcmp(d.data.data(), "NIBArchive", 10) == 0;
    if (!(archive ? parseNibArchive(d.data, *g, err) : parseKeyedNib(d.data, *g, err))) { x.proc.logf(LogLevel::Error, "UINib: %s", err.c_str()); return 0; }
    return loadGraph(x, g, !archive, owner, externals, "(data)");
}
id b_loadNib(Ctx& x, id self, SEL, id name, id owner, id options) {
    const id externals = options ? Foundation::dictGet(x, options, "UINibExternalObjects") : 0;
    std::string n = name ? utf8(x, name) : "";
    id r = loadNib(x, n, self, owner, externals);
    if (!r && n.find("Landscape") != std::string::npos) {
        std::string fallback = n;
        fallback.erase(fallback.find("Landscape"), 9);
        r = loadNib(x, fallback, self, owner, externals);
    }
    if (!r && n.find("Portrait") != std::string::npos) {
        std::string fallback = n;
        fallback.erase(fallback.find("Portrait"), 8);
        r = loadNib(x, fallback, self, owner, externals);
    }
    if (!r) {
        x.proc.logf(LogLevel::Warning, "Could not load NIB in bundle: '%s'", n.c_str());
        return 0;
    }
    return r;
}

// UIView / UIWindow -initWithCoder:
id v_initWithCoder(Ctx& x, id self, SEL, id coder) {
    auto& r = rt(x);
    const id layer = viewLayer(x, self);
    LayerData& l = layerData(x, layer);
    ViewData& d = viewData(x, self);
    auto has = [&](const char* k) { return sendBool(x, coder, "containsValueForKey:", {makeString(x, k)}); };
    auto decodeRect = [&](const char* k) {
        const uint32_t tmp = x.proc.heap().allocZeroed(16);
        const SEL s = r.sel("decodeCGRectForKey:");
        x.proc.callGuest(x.cpu, r.lookupForSend(r.classOf(coder), s), {tmp, coder, s, makeString(x, k)});
        const CGRect out = CG::readStruct<CGRect>(x, tmp);
        x.proc.heap().free(tmp);
        return CG::toG(out);
    };
    auto decodePoint = [&](const char* k) {
        const uint32_t tmp = x.proc.heap().allocZeroed(16);
        const SEL s = r.sel("decodeCGPointForKey:");
        x.proc.callGuest(x.cpu, r.lookupForSend(r.classOf(coder), s), {tmp, coder, s, makeString(x, k)});
        const CGPoint out = CG::readStruct<CGPoint>(x, tmp);
        x.proc.heap().free(tmp);
        return CG::toG(out);
    };
    auto obj = [&](const char* k) { return id(r.send(x.cpu, coder, "decodeObjectForKey:", {makeString(x, k)})); };
    auto i32 = [&](const char* k) { return int32_t(r.send(x.cpu, coder, "decodeIntForKey:", {makeString(x, k)})); };
    auto boolean = [&](const char* k) { return sendBool(x, coder, "decodeBoolForKey:", {makeString(x, k)}); };
    auto f32 = [&](const char* k) { return bitsf(uint32_t(r.send(x.cpu, coder, "decodeFloatForKey:", {makeString(x, k)}))); };
    if (has("UIBounds")) l.bounds = decodeRect("UIBounds");
    if (has("UICenter")) l.position = decodePoint("UICenter");
    else if (has("UIFrame")) layerSetFrame(x, layer, decodeRect("UIFrame"));
    if (has("UIBackgroundColor")) r.send(x.cpu, self, "setBackgroundColor:", {obj("UIBackgroundColor")});
    if (has("UIAlpha")) l.opacity = f32("UIAlpha");
    if (has("UIHidden")) l.hidden = boolean("UIHidden");
    if (has("UIOpaque")) l.opaque = boolean("UIOpaque");
    if (has("UIClipsToBounds")) l.masksToBounds = boolean("UIClipsToBounds");
    if (has("UIAutoresizingMask")) d.autoresizingMask = uint32_t(i32("UIAutoresizingMask"));
    if (has("UIAutoresizeSubviews")) d.autoresizesSubviews = boolean("UIAutoresizeSubviews");
    if (has("UIContentMode")) r.send(x.cpu, self, "setContentMode:", {uint32_t(i32("UIContentMode"))});
    if (has("UITag")) d.tag = i32("UITag");
    if (has("UIUserInteractionDisabled")) d.userInteraction = !boolean("UIUserInteractionDisabled");
    if (has("UIMultipleTouchEnabled")) d.multipleTouch = boolean("UIMultipleTouchEnabled");
    if (has("UIClearsContextBeforeDrawing")) d.clearsContextBeforeDrawing = boolean("UIClearsContextBeforeDrawing");
    if (has("UIContentStretch")) l.contentsCenter = decodeRect("UIContentStretch");
    l.needsDisplay = true;
    if (const id subs = obj("UISubviews")) for (id s : Foundation::arrayItems(x, subs)) if (s != Foundation::nullObject(x.proc)) r.send(x.cpu, self, "addSubview:", {s});
    return self;
}
id w_initWithCoder(Ctx& x, id self, SEL s, id coder) {
    viewData(x, self).isWindow = true;
    v_initWithCoder(x, self, s, coder);
    auto& u = ui(x);
    // windows in nibs are sized for whatever device the nib was designed for; use the real screen
    layerSetFrame(x, viewLayer(x, self), {0, 0, u.screenW, u.screenH});
    if (std::find(u.windows.begin(), u.windows.end(), self) == u.windows.end()) u.windows.push_back(self);
    if (const id root = id(rt(x).send(x.cpu, coder, "decodeObjectForKey:", {makeString(x, "UIRootViewController")}))) rt(x).send(x.cpu, self, "setRootViewController:", {root});
    layerData(x, viewLayer(x, self)).hidden = true;
    return self;
}
id nibroot_init(Ctx&, id self, SEL, id) { return self; }

} // namespace

bool nibExists(Ctx& x, const std::string& name) { return !nibPath(x, name, 0).empty(); }

id loadNib(Ctx& x, const std::string& name, id bundle, id owner, id externals) {
    const std::string p = nibPath(x, name, bundle);
    if (p.empty()) { x.proc.logf(LogLevel::Warning, "nib \"%s\" not found in the app bundle", name.c_str()); return 0; }
    return loadNibFile(x, p, owner, externals);
}

void registerNib(Process& p, ObjC::Runtime& r) {
    auto& ns = p.service<NibState>();
    auto& u = ui(p);
    ObjC::ClassBuilder dec = r.defineClass("UINibDecoder", "NSCoder");
    ns.decoder = dec.cls();
    dec.m<&nd_decodeObject>("decodeObjectForKey:").m<&nd_decodeObject>("decodeObjectOfClass:forKey:")
       .m<&nd_contains>("containsValueForKey:", "c@:@").m<&nd_decodeInt>("decodeIntForKey:", "i@:@")
       .m<&nd_decodeInt>("decodeIntegerForKey:", "i@:@").m<&nd_decodeInt>("decodeInt32ForKey:", "i@:@")
       .m<&nd_decodeInt64>("decodeInt64ForKey:", "q@:@").m<&nd_decodeBool>("decodeBoolForKey:", "c@:@")
       .m<&nd_decodeFloat>("decodeFloatForKey:", "f@:@").m<&nd_decodeDouble>("decodeDoubleForKey:", "d@:@")
       .m<&nd_decodeBytes>("decodeBytesForKey:returnedLength:", "r*@:@^I").m<&nd_true>("allowsKeyedCoding", "c@:")
       .m<&nd_dealloc>("dealloc", "v@:");
    // decodeObjectOfClass:forKey: takes (Class, key): fix the argument order
    dec.method("decodeObjectOfClass:forKey:", [](Ctx& x) { x.cpu.r[0] = nd_decodeObject(x, x.cpu.r[0], x.cpu.r[1], x.cpu.r[3]); }, "@@:#@");

    ObjC::ClassBuilder oc = r.defineClass("UIRuntimeOutletConnection", "NSObject");
    ObjC::ClassBuilder ec = r.defineClass("UIRuntimeEventConnection", "NSObject");
    ObjC::ClassBuilder cc = r.defineClass("UIRuntimeOutletCollectionConnection", "NSObject");
    ns.outlet = oc.cls(); ns.event = ec.cls(); ns.collection = cc.cls();
    oc.m<&conn_init>("initWithCoder:");
    ec.m<&conn_init>("initWithCoder:");
    cc.m<&coll_init>("initWithCoder:");
    r.defineClass("UIRuntimeConnection", "NSObject").m<&conn_init>("initWithCoder:");
    r.defineClass("__HVMNibRoot", "NSObject").m<&nibroot_init>("initWithCoder:");

    ObjC::ClassBuilder nib = r.defineClass("UINib", "NSObject");
    ns.uinib = nib.cls();
    nib.cm<&un_withName>("nibWithNibName:bundle:").cm<&un_withData>("nibWithData:bundle:");
    nib.m<&un_instantiate>("instantiateWithOwner:options:");
    r.builder(r.getClass("NSBundle")).m<&b_loadNib>("loadNibNamed:owner:options:");
    const uint32_t ext = p.defineDataBlock("_UINibExternalObjects", 4);
    p.mem().write<uint32_t>(ext, Foundation::constString(p, "UINibExternalObjects"));

    r.builder(u.view).m<&v_initWithCoder>("initWithCoder:");
    r.builder(u.window).m<&w_initWithCoder>("initWithCoder:");
}

} // namespace Hydrogen::Runtime::UIKit
