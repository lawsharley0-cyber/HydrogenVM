// HydrogenVM — NSUserDefaults and CFPreferences. Values are kept as
// property-list trees on the host and persisted as XML to
// <home>/Library/Preferences/<bundle id>.plist, like the real thing.
#include "Foundation.hpp"
#include <filesystem>

namespace fs = std::filesystem;

namespace Hydrogen::Runtime::Foundation {

namespace {

using PV = Plist::ValuePtr;

struct Domain {
    std::string path;
    std::vector<std::pair<std::string, PV>> values;
    bool dirty = false;
};

struct Defaults {
    explicit Defaults(Process& p) : proc(p) {}
    Process& proc;
    std::mutex mu;
    std::unordered_map<std::string, Domain> domains;     // by name (app id, or other persistent domains)
    std::vector<std::pair<std::string, PV>> registered;  // registration domain (not persisted)
    std::string appDomain;
    Class cls = 0;
    id standard = 0;

    std::string prefsDir() const { return proc.homePath() + "/Library/Preferences"; }
    Domain& domain(const std::string& name) {
        auto it = domains.find(name);
        if (it != domains.end()) return it->second;
        Domain d;
        d.path = prefsDir() + "/" + name + ".plist";
        std::error_code ec;
        if (fs::exists(d.path, ec)) {
            try {
                if (auto v = Plist::parseFile(d.path); v && v->type == Plist::Value::Type::Dict) d.values = v->dict;
            } catch (const std::exception& e) {
                proc.logf(LogLevel::Warning, "NSUserDefaults: can't read %s: %s", d.path.c_str(), e.what());
            }
        }
        return domains.emplace(name, std::move(d)).first->second;
    }
    Domain& app() {
        if (appDomain.empty()) {
            appDomain = proc.infoPlist() ? proc.infoPlist()->getString("CFBundleIdentifier") : "";
            if (appDomain.empty()) appDomain = "com.hydrogenvm.unknown";
        }
        return domain(appDomain);
    }
    static PV find(const std::vector<std::pair<std::string, PV>>& v, const std::string& k) {
        for (auto& [kk, vv] : v) if (kk == k) return vv;
        return nullptr;
    }
    PV get(const std::string& k) {
        std::lock_guard<std::mutex> lk(mu);
        if (PV v = find(app().values, k)) return v;
        if (k == "AppleLanguages") { auto a = Plist::Value::makeArray(); a->array.push_back(Plist::Value::makeString("en")); return a; }
        if (k == "AppleLocale") return Plist::Value::makeString("en_US");
        return find(registered, k);
    }
    void set(const std::string& k, PV v) {
        std::lock_guard<std::mutex> lk(mu);
        Domain& d = app();
        for (auto it = d.values.begin(); it != d.values.end(); ++it) {
            if (it->first == k) { if (v) it->second = std::move(v); else d.values.erase(it); d.dirty = true; return; }
        }
        if (v) { d.values.emplace_back(k, std::move(v)); d.dirty = true; }
    }
    bool save(Domain& d) {
        if (!d.dirty) return true;
        std::error_code ec;
        fs::create_directories(prefsDir(), ec);
        Plist::Value root;
        root.type = Plist::Value::Type::Dict;
        root.dict = d.values;
        try {
            const std::string tmp = d.path + ".tmp";
            Plist::writeXmlFile(root, tmp);
            fs::rename(tmp, d.path, ec);
            if (ec) return false;
        } catch (const std::exception& e) {
            proc.logf(LogLevel::Warning, "NSUserDefaults: can't write %s: %s", d.path.c_str(), e.what());
            return false;
        }
        d.dirty = false;
        return true;
    }
    bool synchronize() {
        std::lock_guard<std::mutex> lk(mu);
        bool ok = true;
        for (auto& [n, d] : domains) ok &= save(d);
        return ok;
    }
};
Defaults& D(Process& p) { return p.service<Defaults>(); }
Defaults& D(Ctx& x) { return D(x.proc); }

std::string K(Ctx& x, id key) { return key ? utf8(x, key) : std::string(); }

id ud_standard(Ctx& x, id, SEL) {
    auto& d = D(x);
    if (!d.standard) { d.standard = rt(x).createInstance(d.cls); rt(x).makeImmortal(d.standard); }
    return d.standard;
}
id ud_init(Ctx&, id self, SEL) { return self; }
id ud_initWithSuite(Ctx&, id self, SEL, id) { return self; }
void ud_reset(Ctx& x, id, SEL) { D(x).synchronize(); }

id ud_object(Ctx& x, id, SEL, id key) {
    PV v = D(x).get(K(x, key));
    return v ? plistToObject(x, *v) : 0;
}
void ud_setObject(Ctx& x, id, SEL, id value, id key) {
    if (!value) { D(x).set(K(x, key), nullptr); return; }
    PV v = objectToPlist(x, value);
    if (!v) {
        if (rt(x).respondsTo(value, rt(x).sel("data"))) {
            v = objectToPlist(x, id(rt(x).send(x.cpu, value, rt(x).sel("data"))));
        } else if (rt(x).respondsTo(value, rt(x).sel("archive"))) {
            v = objectToPlist(x, id(rt(x).send(x.cpu, value, rt(x).sel("archive"))));
        }
    }
    if (!v) {
        if (isString(x, value)) {
            v = Plist::Value::makeString(utf8(x, value));
        } else if (rt(x).respondsTo(value, rt(x).sel("string"))) {
            v = Plist::Value::makeString(utf8(x, id(rt(x).send(x.cpu, value, rt(x).sel("string")))));
        } else if (rt(x).respondsTo(value, rt(x).sel("description"))) {
            v = Plist::Value::makeString(describe(x, value));
        }
    }
    if (!v) {
        x.proc.logf(LogLevel::Warning, "NSUserDefaults setObject:forKey: ignoring non-plist object %s for key %s", describe(x, value).c_str(), K(x, key).c_str());
        return;
    }
    D(x).set(K(x, key), v);
    postNotification(x, "NSUserDefaultsDidChangeNotification", ud_standard(x, 0, 0));
}
void ud_remove(Ctx& x, id self, SEL s, id key) { ud_setObject(x, self, s, 0, key); }

using T = Plist::Value::Type;
int64_t intOf(const PV& v) {
    if (!v) return 0;
    switch (v->type) {
        case T::Integer: return v->i;
        case T::Real: return int64_t(v->r);
        case T::Bool: return v->b;
        case T::String: return std::atoll(v->s.c_str());
        default: return 0;
    }
}
double doubleOf(const PV& v) {
    if (!v) return 0;
    switch (v->type) {
        case T::Integer: return double(v->i);
        case T::Real: return v->r;
        case T::Bool: return v->b;
        case T::String: return std::atof(v->s.c_str());
        default: return 0;
    }
}
bool boolOf(const PV& v) {
    if (!v) return false;
    if (v->type == T::String) {
        const std::string s = v->s;
        if (s == "YES" || s == "yes" || s == "Y" || s == "y" || s == "true" || s == "TRUE") return true;
        return std::atoi(s.c_str()) != 0;
    }
    return intOf(v) != 0 || (v->type == T::Real && v->r != 0);
}
id ud_string(Ctx& x, id, SEL, id key) {
    PV v = D(x).get(K(x, key));
    if (!v) return 0;
    if (v->type == T::String) return makeString(x, v->s);
    if (v->type == T::Integer) return makeString(x, std::to_string(v->i));
    if (v->type == T::Real) { char b[64]; std::snprintf(b, sizeof b, "%.17g", v->r); return makeString(x, std::string(b)); }
    if (v->type == T::Bool) return makeString(x, v->b ? "1" : "0");
    return 0;
}
id typed(Ctx& x, id key, T t) { PV v = D(x).get(K(x, key)); return v && v->type == t ? plistToObject(x, *v) : 0; }
id ud_array(Ctx& x, id, SEL, id key) { return typed(x, key, T::Array); }
id ud_dict(Ctx& x, id, SEL, id key) { return typed(x, key, T::Dict); }
id ud_data(Ctx& x, id, SEL, id key) { return typed(x, key, T::Data); }
id ud_stringArray(Ctx& x, id, SEL, id key) {
    PV v = D(x).get(K(x, key));
    if (!v || v->type != T::Array) return 0;
    for (auto& e : v->array) if (e->type != T::String) return 0;
    return plistToObject(x, *v);
}
int32_t ud_integer(Ctx& x, id, SEL, id key) { return int32_t(intOf(D(x).get(K(x, key)))); }
bool ud_bool(Ctx& x, id, SEL, id key) { return boolOf(D(x).get(K(x, key))); }
float ud_float(Ctx& x, id, SEL, id key) { return float(doubleOf(D(x).get(K(x, key)))); }
double ud_double(Ctx& x, id, SEL, id key) { return doubleOf(D(x).get(K(x, key))); }
id ud_url(Ctx& x, id, SEL, id key) {
    PV v = D(x).get(K(x, key));
    if (!v) return 0;
    if (v->type == T::String) return makeFileURL(x, v->s);
    if (v->type == T::Data) {
        // archived NSURL — not supported yet
        x.proc.reportMissing("NSUserDefaults URLForKey: with archived URL");
    }
    return 0;
}
void ud_setInteger(Ctx& x, id, SEL, int32_t v, id key) { D(x).set(K(x, key), Plist::Value::makeInt(v)); }
void ud_setBool(Ctx& x, id, SEL, bool v, id key) { D(x).set(K(x, key), Plist::Value::makeBool(v)); }
void ud_setFloat(Ctx& x, id, SEL, float v, id key) { D(x).set(K(x, key), Plist::Value::makeReal(v)); }
void ud_setDouble(Ctx& x, id, SEL, double v, id key) { D(x).set(K(x, key), Plist::Value::makeReal(v)); }
void ud_setURL(Ctx& x, id, SEL, id url, id key) { D(x).set(K(x, key), url ? Plist::Value::makeString(urlFilePath(x, url)) : nullptr); }

void ud_register(Ctx& x, id, SEL, id dict) {
    auto& d = D(x);
    std::vector<std::pair<std::string, PV>> add;
    for (auto& [k, v] : dictEntries(x, dict)) if (PV pv = objectToPlist(x, v)) add.emplace_back(utf8(x, k), pv);
    std::lock_guard<std::mutex> lk(d.mu);
    for (auto& [k, v] : add) {
        bool replaced = false;
        for (auto& [kk, vv] : d.registered) if (kk == k) { vv = v; replaced = true; break; }
        if (!replaced) d.registered.emplace_back(k, v);
    }
}
bool ud_sync(Ctx& x, id, SEL) { return D(x).synchronize(); }
id ud_dictRep(Ctx& x, id, SEL) {
    auto& d = D(x);
    Plist::Value root;
    root.type = T::Dict;
    {
        std::lock_guard<std::mutex> lk(d.mu);
        root.dict = d.registered;
        for (auto& [k, v] : d.app().values) root.set(k, v);
    }
    return plistToObject(x, root);
}
id ud_persistentDomain(Ctx& x, id, SEL, id name) {
    auto& d = D(x);
    Plist::Value root;
    root.type = T::Dict;
    {
        std::lock_guard<std::mutex> lk(d.mu);
        root.dict = d.domain(K(x, name)).values;
    }
    return plistToObject(x, root);
}
void ud_setPersistentDomain(Ctx& x, id, SEL, id dict, id name) {
    auto& d = D(x);
    std::vector<std::pair<std::string, PV>> vals;
    for (auto& [k, v] : dictEntries(x, dict)) if (PV pv = objectToPlist(x, v)) vals.emplace_back(utf8(x, k), pv);
    std::lock_guard<std::mutex> lk(d.mu);
    Domain& dom = d.domain(K(x, name));
    dom.values = std::move(vals);
    dom.dirty = true;
}
void ud_removePersistentDomain(Ctx& x, id, SEL, id name) {
    auto& d = D(x);
    std::lock_guard<std::mutex> lk(d.mu);
    Domain& dom = d.domain(K(x, name));
    dom.values.clear();
    dom.dirty = true;
}
bool ud_objectIsForced(Ctx&, id, SEL, id) { return false; }
void ud_addSuite(Ctx&, id, SEL, id) {}

// ------------------------------------------------------------ CFPreferences ---

bool isApp(Ctx& x, id appId) {
    if (!appId) return true;
    const std::string a = utf8(x, appId);
    if (a == "kCFPreferencesCurrentApplication") return true;
    auto& d = D(x);
    std::lock_guard<std::mutex> lk(d.mu);
    d.app();
    return a == d.appDomain;
}
uint32_t cfp_copyAppValue(Ctx& x, id key, id appId) {
    if (isApp(x, appId)) { PV v = D(x).get(K(x, key)); return v ? rt(x).retain(x.cpu, plistToObject(x, *v)) : 0; }
    auto& d = D(x);
    std::lock_guard<std::mutex> lk(d.mu);
    PV v = Defaults::find(d.domain(K(x, appId)).values, K(x, key));
    return v ? rt(x).retain(x.cpu, plistToObject(x, *v)) : 0;
}
void cfp_setAppValue(Ctx& x, id key, id value, id appId) {
    PV v = value ? objectToPlist(x, value) : nullptr;
    if (isApp(x, appId)) { D(x).set(K(x, key), v); return; }
    auto& d = D(x);
    std::lock_guard<std::mutex> lk(d.mu);
    Domain& dom = d.domain(K(x, appId));
    for (auto it = dom.values.begin(); it != dom.values.end(); ++it) if (it->first == K(x, key)) { dom.values.erase(it); break; }
    if (v) dom.values.emplace_back(K(x, key), v);
    dom.dirty = true;
}
bool cfp_getAppBool(Ctx& x, id key, id appId, GuestPtr valid) {
    PV v = D(x).get(K(x, key));
    (void)appId;
    if (valid) x.mem.write<uint8_t>(valid, v ? 1 : 0);
    return boolOf(v);
}
int32_t cfp_getAppInt(Ctx& x, id key, id appId, GuestPtr valid) {
    PV v = D(x).get(K(x, key));
    (void)appId;
    if (valid) x.mem.write<uint8_t>(valid, v ? 1 : 0);
    return int32_t(intOf(v));
}
bool cfp_sync(Ctx& x, id) { return D(x).synchronize(); }
uint32_t cfp_copyValue(Ctx& x, id key, id appId, id, id) { return cfp_copyAppValue(x, key, appId); }
void cfp_setValue(Ctx& x, id key, id value, id appId, id, id) { cfp_setAppValue(x, key, value, appId); }
bool cfp_syncFull(Ctx& x, id, id, id) { return D(x).synchronize(); }

} // namespace

void registerDefaults(Process& p, ObjC::Runtime& r) {
    auto& d = D(p);
    ObjC::ClassBuilder ud = r.defineClass("NSUserDefaults", "NSObject");
    d.cls = ud.cls();
    ud.cm<&ud_standard>("standardUserDefaults").cm<&ud_reset>("resetStandardUserDefaults", "v@:");
    ud.m<&ud_init>("init").m<&ud_initWithSuite>("initWithSuiteName:").m<&ud_initWithSuite>("initWithUser:")
      .m<&ud_object>("objectForKey:").m<&ud_setObject>("setObject:forKey:", "v@:@@").m<&ud_remove>("removeObjectForKey:", "v@:@")
      .m<&ud_string>("stringForKey:").m<&ud_array>("arrayForKey:").m<&ud_dict>("dictionaryForKey:").m<&ud_data>("dataForKey:")
      .m<&ud_stringArray>("stringArrayForKey:").m<&ud_integer>("integerForKey:", "i@:@").m<&ud_bool>("boolForKey:", "c@:@")
      .m<&ud_float>("floatForKey:", "f@:@").m<&ud_double>("doubleForKey:", "d@:@").m<&ud_url>("URLForKey:")
      .m<&ud_setInteger>("setInteger:forKey:", "v@:i@").m<&ud_setBool>("setBool:forKey:", "v@:c@")
      .m<&ud_setFloat>("setFloat:forKey:", "v@:f@").m<&ud_setDouble>("setDouble:forKey:", "v@:d@")
      .m<&ud_setURL>("setURL:forKey:", "v@:@@").m<&ud_register>("registerDefaults:", "v@:@")
      .m<&ud_sync>("synchronize", "c@:").m<&ud_dictRep>("dictionaryRepresentation")
      .m<&ud_persistentDomain>("persistentDomainForName:").m<&ud_setPersistentDomain>("setPersistentDomain:forName:", "v@:@@")
      .m<&ud_removePersistentDomain>("removePersistentDomainForName:", "v@:@")
      .m<&ud_objectIsForced>("objectIsForcedForKey:", "c@:@").m<&ud_addSuite>("addSuiteNamed:", "v@:@")
      .m<&ud_addSuite>("removeSuiteNamed:", "v@:@")
      // KVC on NSUserDefaults goes straight to the defaults (as Foundation's override does).
      .m<&ud_object>("valueForKey:").m<&ud_setObject>("setValue:forKey:", "v@:@@");
    for (const char* n : {"NSUserDefaultsDidChangeNotification", "NSGlobalDomain", "NSArgumentDomain", "NSRegistrationDomain"}) {
        const uint32_t cell = p.defineDataBlock(std::string("_") + n, 4);
        p.mem().write<uint32_t>(cell, constString(p, n));
    }
    for (const char* n : {"kCFPreferencesCurrentApplication", "kCFPreferencesAnyApplication", "kCFPreferencesCurrentHost",
                          "kCFPreferencesAnyHost", "kCFPreferencesCurrentUser", "kCFPreferencesAnyUser"}) {
        const uint32_t cell = p.defineDataBlock(std::string("_") + n, 4);
        p.mem().write<uint32_t>(cell, constString(p, n));
    }
    p.def<&cfp_copyAppValue>("_CFPreferencesCopyAppValue");
    p.def<&cfp_setAppValue>("_CFPreferencesSetAppValue");
    p.def<&cfp_getAppBool>("_CFPreferencesGetAppBooleanValue");
    p.def<&cfp_getAppInt>("_CFPreferencesGetAppIntegerValue");
    p.def<&cfp_sync>("_CFPreferencesAppSynchronize");
    p.def<&cfp_copyValue>("_CFPreferencesCopyValue");
    p.def<&cfp_setValue>("_CFPreferencesSetValue");
    p.def<&cfp_syncFull>("_CFPreferencesSynchronize");
    // Flush periodically and on exit, like cfprefsd would.
    mainRunLoop(p).addTimer(5.0, 5.0, true, [&p](CPU::Cpu&) { D(p).synchronize(); });
    p.onExit([&p] { D(p).synchronize(); });
}

void flushUserDefaults(Process& p) { D(p).synchronize(); }

} // namespace Hydrogen::Runtime::Foundation
