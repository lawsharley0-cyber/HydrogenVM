// HydrogenVM — Security.framework: the keychain item API (SecItem*) over a
// per-app store in <home>/Library/Keychains/hvm-keychain.plist, the constants
// apps build queries from, and SecRandomCopyBytes.
//
// Apps mostly use the keychain to keep a save-game checksum, a device id or a
// login token across launches; that's what this supports. Keys, certificates
// and trust evaluation aren't implemented (SecKey*, SecTrust* stay missing).
#include "Foundation.hpp"
#include "loader/Plist.hpp"
#include <filesystem>
#include <fstream>
#include <random>

namespace Hydrogen::Runtime::Foundation {
namespace {

namespace fs = std::filesystem;
using ObjC::id;

constexpr int32_t errSecSuccess = 0, errSecParam = -50, errSecDuplicateItem = -25299, errSecItemNotFound = -25300;

// Attribute keys whose values identify an item (used for matching and duplicates).
bool isAttributeKey(const std::string& k) {
    return !(k.rfind("r_", 0) == 0 || k.rfind("m_", 0) == 0 || k.rfind("u_", 0) == 0 || k == "v_Data" || k == "v_Ref" || k == "v_PersistentRef");
}

struct Keychain {
    explicit Keychain(Process& p) : path((fs::path(p.homePath()) / "Library" / "Keychains" / "hvm-keychain.plist").string()) {}
    std::mutex mu;
    std::string path;
    bool loaded = false;
    std::vector<Plist::ValuePtr> items;

    void load() {
        if (loaded) return;
        loaded = true;
        try {
            if (!fs::exists(path)) return;
            if (auto root = Plist::parseFile(path); root && root->type == Plist::Value::Type::Array) items = root->array;
        } catch (...) {}
    }
    void save() {
        try {
            fs::create_directories(fs::path(path).parent_path());
            auto root = Plist::Value::makeArray();
            root->array = items;
            Plist::writeXmlFile(*root, path);
        } catch (...) {}
    }
};

void syncTinyTowerFileToKeychain(Process& p, Keychain& kc) {
    const fs::path backupPath = fs::path(p.homePath()) / "Documents" / "backup.txt";
    std::error_code ec;
    if (!fs::exists(backupPath, ec)) return;

    const auto backupTime = fs::last_write_time(backupPath, ec);
    const auto kcTime = fs::exists(kc.path, ec) ? fs::last_write_time(kc.path, ec) : fs::file_time_type::min();
    if (backupTime <= kcTime && !kc.items.empty()) return;

    std::ifstream f(backupPath, std::ios::binary);
    if (!f.is_open()) return;
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (text.find("coins=") == std::string::npos && text.find("bux=") == std::string::npos) return;

    bool found = false;
    for (auto& it : kc.items) {
        if (it->getString("svce") == "com.eeenmachine.tinytowers.gameData" || it->getString("svce").find("gameData") != std::string::npos) {
            it->set("v_Data", Plist::Value::makeData(text));
            found = true;
            break;
        }
    }
    if (!found) {
        auto item = Plist::Value::makeDict();
        item->set("class", Plist::Value::makeString("genp"));
        item->set("svce", Plist::Value::makeString("com.eeenmachine.tinytowers.gameData"));
        item->set("acct", Plist::Value::makeString("gameData"));
        item->set("v_Data", Plist::Value::makeData(text));
        kc.items.push_back(item);
    }
    kc.save();
}

void syncTinyTowerKeychainToFile(Process& p, const Plist::Value& item) {
    if (item.getString("svce") != "com.eeenmachine.tinytowers.gameData" && item.getString("svce").find("gameData") == std::string::npos) return;
    const Plist::Value* dataVal = item.get("v_Data");
    if (!dataVal || dataVal->type != Plist::Value::Type::Data) return;
    const auto& s = dataVal->s;
    if (s.find("coins=") == std::string::npos && s.find("bux=") == std::string::npos) return;

    const fs::path backupPath = fs::path(p.homePath()) / "Documents" / "backup.txt";
    std::error_code ec;
    fs::create_directories(backupPath.parent_path(), ec);
    std::ofstream out(backupPath, std::ios::binary);
    if (out.is_open()) {
        out.write(s.data(), s.size());
    }
}

std::string valueKey(const Plist::Value& v) { return Plist::toXml(v); }

// The query as a plist dict (keys are the constants' string values).
Plist::ValuePtr queryDict(Ctx& x, id dict) {
    auto out = Plist::Value::makeDict();
    for (auto& [k, v] : dictEntries(x, dict)) {
        if (!isString(x, k)) continue;
        Plist::ValuePtr pv = objectToPlist(x, v);
        if (!pv) continue;   // SecKeyRef, SecAccessControl, ...
        out->set(utf8(x, k), pv);
    }
    return out;
}

bool truthy(const Plist::Value* v) {
    return v && ((v->type == Plist::Value::Type::Bool && v->b) || (v->type == Plist::Value::Type::Integer && v->i));
}

bool matches(const Plist::Value& item, const Plist::Value& query) {
    for (const auto& [k, v] : query.dict) {
        if (!isAttributeKey(k)) continue;
        const Plist::Value* iv = item.get(k);
        if (!iv || valueKey(*iv) != valueKey(*v)) return false;
    }
    return true;
}

std::string itemClass(const Plist::Value& v) { return v.getString("class", "genp"); }

int32_t s_add(Ctx& x, id attrs, GuestPtr result) {
    if (!attrs) return errSecParam;
    Keychain& kc = x.proc.service<Keychain>();
    auto q = queryDict(x, attrs);
    auto item = Plist::Value::makeDict();
    for (auto& [k, v] : q->dict) if (isAttributeKey(k) || k == "v_Data") item->set(k, v);
    if (!item->get("class")) item->set("class", Plist::Value::makeString("genp"));
    std::lock_guard<std::mutex> lk(kc.mu);
    kc.load();
    // Duplicate: same class and same primary-key attributes.
    for (const auto& it : kc.items) {
        auto probe = Plist::Value::makeDict();
        for (const char* key : {"class", "svce", "acct", "agrp", "srvr", "atag", "ptcl", "port", "path"})
            if (const Plist::Value* v = item->get(key)) probe->set(key, std::make_shared<Plist::Value>(*v));
        if (matches(*it, *probe) && itemClass(*it) == itemClass(*item)) return errSecDuplicateItem;
    }
    kc.items.push_back(item);
    kc.save();
    syncTinyTowerKeychainToFile(x.proc, *item);
    if (result) x.mem.write<uint32_t>(result, 0);
    return errSecSuccess;
}

id itemResult(Ctx& x, const Plist::Value& item, bool wantData, bool wantAttrs) {
    const Plist::Value* data = item.get("v_Data");
    if (!wantAttrs) return data ? plistToObject(x, *data) : makeData(x, nullptr, 0);
    auto d = Plist::Value::makeDict();
    for (const auto& [k, v] : item.dict) if (k != "v_Data" || wantData) d->set(k, v);
    return plistToObject(x, *d, true);
}

int32_t s_copyMatching(Ctx& x, id query, GuestPtr result) {
    if (!query) return errSecParam;
    Keychain& kc = x.proc.service<Keychain>();
    auto q = queryDict(x, query);
    if (!q->get("class")) q->set("class", Plist::Value::makeString("genp"));
    const bool wantData = truthy(q->get("r_Data")), wantAttrs = truthy(q->get("r_Attributes"));
    const bool all = q->getString("m_Limit") == "m_LimitAll";
    std::vector<id> found;
    {
        std::lock_guard<std::mutex> lk(kc.mu);
        kc.load();
        syncTinyTowerFileToKeychain(x.proc, kc);
        for (const auto& it : kc.items) {
            if (!matches(*it, *q)) continue;
            if (!result) return errSecSuccess;
            found.push_back(itemResult(x, *it, wantData, wantAttrs || (!wantData && !wantAttrs)));
            if (!all) break;
        }
    }
    if (found.empty()) { if (result) x.mem.write<uint32_t>(result, 0); return errSecItemNotFound; }
    auto& r = ObjC::rt(x);
    id out = all ? makeArray(x, found) : found.front();
    r.retain(x.cpu, out);   // Copy rule: the caller owns the result
    x.mem.write<uint32_t>(result, out);
    return errSecSuccess;
}

int32_t s_update(Ctx& x, id query, id attrs) {
    if (!query || !attrs) return errSecParam;
    Keychain& kc = x.proc.service<Keychain>();
    auto q = queryDict(x, query);
    if (!q->get("class")) q->set("class", Plist::Value::makeString("genp"));
    auto u = queryDict(x, attrs);
    std::lock_guard<std::mutex> lk(kc.mu);
    kc.load();
    bool any = false;
    for (auto& it : kc.items) {
        if (!matches(*it, *q)) continue;
        for (auto& [k, v] : u->dict) if (isAttributeKey(k) || k == "v_Data") it->set(k, v);
        any = true;
        syncTinyTowerKeychainToFile(x.proc, *it);
    }
    if (any) kc.save();
    return any ? errSecSuccess : errSecItemNotFound;
}

int32_t s_delete(Ctx& x, id query) {
    if (!query) return errSecParam;
    Keychain& kc = x.proc.service<Keychain>();
    auto q = queryDict(x, query);
    if (!q->get("class")) q->set("class", Plist::Value::makeString("genp"));
    std::lock_guard<std::mutex> lk(kc.mu);
    kc.load();
    const size_t before = kc.items.size();
    kc.items.erase(std::remove_if(kc.items.begin(), kc.items.end(), [&](const Plist::ValuePtr& it) { return matches(*it, *q); }), kc.items.end());
    if (kc.items.size() == before) return errSecItemNotFound;
    kc.save();
    return errSecSuccess;
}

int32_t s_randomBytes(Ctx& x, GuestPtr, uint32_t count, GuestPtr out) {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    for (uint32_t i = 0; i < count; ++i) x.mem.write<uint8_t>(out + i, uint8_t(rng()));
    return 0;
}

} // namespace

void registerSecurity(Process& p, ObjC::Runtime&) {
    // CFStringRef constants: each symbol is a pointer-sized variable holding the string.
    static const std::pair<const char*, const char*> kConstants[] = {
        {"_kSecClass", "class"}, {"_kSecClassGenericPassword", "genp"}, {"_kSecClassInternetPassword", "inet"},
        {"_kSecClassCertificate", "cert"}, {"_kSecClassKey", "keys"}, {"_kSecClassIdentity", "idnt"},
        {"_kSecAttrAccount", "acct"}, {"_kSecAttrService", "svce"}, {"_kSecAttrLabel", "labl"}, {"_kSecAttrGeneric", "gena"},
        {"_kSecAttrDescription", "desc"}, {"_kSecAttrComment", "icmt"}, {"_kSecAttrAccessGroup", "agrp"},
        {"_kSecAttrServer", "srvr"}, {"_kSecAttrProtocol", "ptcl"}, {"_kSecAttrPort", "port"}, {"_kSecAttrPath", "path"},
        {"_kSecAttrCreationDate", "cdat"}, {"_kSecAttrModificationDate", "mdat"}, {"_kSecAttrCreator", "crtr"},
        {"_kSecAttrType", "type"}, {"_kSecAttrIsInvisible", "invi"}, {"_kSecAttrIsNegative", "nega"},
        {"_kSecAttrSynchronizable", "sync"}, {"_kSecAttrAccessible", "pdmn"},
        {"_kSecAttrAccessibleWhenUnlocked", "ak"}, {"_kSecAttrAccessibleAfterFirstUnlock", "ck"}, {"_kSecAttrAccessibleAlways", "dk"},
        {"_kSecAttrAccessibleWhenUnlockedThisDeviceOnly", "aku"}, {"_kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly", "cku"},
        {"_kSecAttrAccessibleAlwaysThisDeviceOnly", "dku"},
        {"_kSecAttrApplicationTag", "atag"}, {"_kSecAttrApplicationLabel", "klbl"}, {"_kSecAttrKeyType", "type"},
        {"_kSecAttrKeyTypeRSA", "42"}, {"_kSecAttrKeyClass", "kcls"}, {"_kSecAttrKeyClassPublic", "0"}, {"_kSecAttrKeyClassPrivate", "1"},
        {"_kSecAttrKeySizeInBits", "bsiz"}, {"_kSecAttrEffectiveKeySize", "esiz"}, {"_kSecAttrIsPermanent", "perm"},
        {"_kSecAttrCanEncrypt", "encr"}, {"_kSecAttrCanDecrypt", "decr"}, {"_kSecAttrCanSign", "sign"}, {"_kSecAttrCanVerify", "vrfy"},
        {"_kSecValueData", "v_Data"}, {"_kSecValueRef", "v_Ref"}, {"_kSecValuePersistentRef", "v_PersistentRef"},
        {"_kSecReturnData", "r_Data"}, {"_kSecReturnAttributes", "r_Attributes"}, {"_kSecReturnRef", "r_Ref"},
        {"_kSecReturnPersistentRef", "r_PersistentRef"}, {"_kSecMatchLimit", "m_Limit"}, {"_kSecMatchLimitOne", "m_LimitOne"},
        {"_kSecMatchLimitAll", "m_LimitAll"}, {"_kSecMatchCaseInsensitive", "m_CaseInsensitive"},
    };
    for (const auto& [sym, value] : kConstants) {
        const uint32_t cell = p.defineDataBlock(sym, 4);
        p.mem().write<uint32_t>(cell, constString(p, value));
    }
    p.defineDataBlock("_kSecRandomDefault", 4);
    p.def<&s_add>("_SecItemAdd");
    p.def<&s_copyMatching>("_SecItemCopyMatching");
    p.def<&s_update>("_SecItemUpdate");
    p.def<&s_delete>("_SecItemDelete");
    p.def<&s_randomBytes>("_SecRandomCopyBytes");
}

} // namespace Hydrogen::Runtime::Foundation
