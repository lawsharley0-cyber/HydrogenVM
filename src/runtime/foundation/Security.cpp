// HydrogenVM — Security.framework: the keychain item API (SecItem*) over a
// per-app store in <home>/Library/Keychains/hvm-keychain.plist, the constants
// apps build queries from, SecRandomCopyBytes, and certificates / public-key
// operations (SecCertificate*, SecPolicy*, SecTrust*, SecKeyEncrypt).
//
// Apps mostly use the keychain to keep a save-game checksum, a device id or a
// login token across launches. Certificates and public-key encryption are used
// by embedded analytics SDKs (InMobi in Clash of Clans 1.70 calls
// SecCertificateCreateWithData, wraps the cert in a CFArray, builds a basic
// X.509 trust, copies the public key and SecKeyEncrypts with kSecPaddingNone).
// Those APIs are implemented with the host Security.framework on Apple and
// OpenSSL on Linux; they return NULL / errSecUnimplemented when neither is
// available. Trust evaluation does not consult a system root store: a parsed
// leaf yields a public key and kSecTrustResultRecoverableTrustFailure.
#include "Foundation.hpp"
#include "loader/Plist.hpp"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>
#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#elif defined(HVM_HAVE_OPENSSL)
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/rsa.h>
#endif

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

constexpr int32_t errSecUnimplemented = -4, errSecBufferTooSmall = -25301;
constexpr uint32_t kSecPaddingNone = 0, kSecPaddingPKCS1 = 1, kSecPaddingOAEP = 2;
constexpr uint32_t kSecTrustResultRecoverableTrustFailure = 5;

const char* kCertClass = "__HVMSecCertificate";
const char* kPolicyClass = "__HVMSecPolicy";
const char* kTrustClass = "__HVMSecTrust";
const char* kKeyClass = "__HVMSecKey";

struct SecCertObj : ObjC::HostObject {
    std::vector<uint8_t> der;
#if defined(__APPLE__)
    ::SecCertificateRef host = nullptr;
    ~SecCertObj() override { if (host) ::CFRelease(host); }
#endif
};
struct SecPolicyObj : ObjC::HostObject {};
struct SecKeyObj : ObjC::HostObject {
#if defined(__APPLE__)
    ::SecKeyRef host = nullptr;
    ~SecKeyObj() override { if (host) ::CFRelease(host); }
#elif defined(HVM_HAVE_OPENSSL)
    EVP_PKEY* key = nullptr;
    ~SecKeyObj() override { EVP_PKEY_free(key); }
#endif
};
struct SecTrustObj : ObjC::HostObject {
    id cert = 0, key = 0;
#if defined(__APPLE__)
    ::SecTrustRef host = nullptr;
    ~SecTrustObj() override { if (host) ::CFRelease(host); }
#endif
};

ObjC::Class secClass(Ctx& x, const char* n) { return ObjC::rt(x).getClass(n); }

id makeCert(Ctx& x, std::vector<uint8_t> der) {
#if defined(__APPLE__)
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, der.data(), CFIndex(der.size()));
    if (!data) return 0;
    ::SecCertificateRef host = ::SecCertificateCreateWithData(nullptr, data);
    CFRelease(data);
    if (!host) return 0;
    const id o = ObjC::rt(x).createInstance(secClass(x, kCertClass));
    auto h = std::make_unique<SecCertObj>();
    h->der = std::move(der);
    h->host = host;
    ObjC::rt(x).setHost(o, std::move(h));
    return o;
#elif defined(HVM_HAVE_OPENSSL)
    const unsigned char* p = der.data();
    X509* cert = d2i_X509(nullptr, &p, long(der.size()));
    if (!cert) return 0;
    X509_free(cert);
    const id o = ObjC::rt(x).createInstance(secClass(x, kCertClass));
    auto h = std::make_unique<SecCertObj>();
    h->der = std::move(der);
    ObjC::rt(x).setHost(o, std::move(h));
    return o;
#else
    (void)x; (void)der;
    return 0;
#endif
}

id makeKeyFromCert(Ctx& x, const SecCertObj& cert) {
#if defined(__APPLE__)
    if (!cert.host) return 0;
    ::SecPolicyRef policy = ::SecPolicyCreateBasicX509();
    if (!policy) return 0;
    ::SecTrustRef trust = nullptr;
    if (::SecTrustCreateWithCertificates(cert.host, policy, &trust) != errSecSuccess || !trust) {
        CFRelease(policy);
        return 0;
    }
    // CopyPublicKey works before or after Evaluate (Apple's contract).
    (void)::SecTrustEvaluate(trust, nullptr);
    ::SecKeyRef hostKey = ::SecTrustCopyPublicKey(trust);
    CFRelease(trust);
    CFRelease(policy);
    if (!hostKey) return 0;
    const id o = ObjC::rt(x).createInstance(secClass(x, kKeyClass));
    auto h = std::make_unique<SecKeyObj>();
    h->host = hostKey;
    ObjC::rt(x).setHost(o, std::move(h));
    return o;
#elif defined(HVM_HAVE_OPENSSL)
    const unsigned char* p = cert.der.data();
    X509* c = d2i_X509(nullptr, &p, long(cert.der.size()));
    if (!c) return 0;
    EVP_PKEY* pk = X509_get_pubkey(c);
    X509_free(c);
    if (!pk) return 0;
    const id o = ObjC::rt(x).createInstance(secClass(x, kKeyClass));
    auto h = std::make_unique<SecKeyObj>();
    h->key = pk;
    ObjC::rt(x).setHost(o, std::move(h));
    return o;
#else
    (void)x; (void)cert;
    return 0;
#endif
}

uint32_t sec_certificateCreateWithData(Ctx& x, uint32_t, id data) {
    if (!data || !isData(x, data)) return 0;
    return makeCert(x, dataBytes(x, data));
}
uint32_t sec_certificateCopyData(Ctx& x, id cert) {
    auto* c = ObjC::rt(x).hostIf<SecCertObj>(cert);
    if (!c) return 0;
    return makeData(x, c->der.data(), uint32_t(c->der.size()), true);
}
uint32_t sec_certificateCopySubjectSummary(Ctx& x, id cert) {
    auto* c = ObjC::rt(x).hostIf<SecCertObj>(cert);
    if (!c) return 0;
#if defined(__APPLE__)
    if (!c->host) return 0;
    CFStringRef s = ::SecCertificateCopySubjectSummary(c->host);
    if (!s) return 0;
    char buf[256] = {};
    CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8);
    CFRelease(s);
    return makeString(x, std::string(buf), true);
#elif defined(HVM_HAVE_OPENSSL)
    const unsigned char* p = c->der.data();
    X509* xc = d2i_X509(nullptr, &p, long(c->der.size()));
    if (!xc) return 0;
    char cn[256] = {};
    X509_NAME_get_text_by_NID(X509_get_subject_name(xc), NID_commonName, cn, sizeof cn);
    X509_free(xc);
    return makeString(x, std::string(cn[0] ? cn : "certificate"), true);
#else
    (void)x;
    return 0;
#endif
}

uint32_t sec_policyCreateBasicX509(Ctx& x) {
    return ObjC::rt(x).createInstance(secClass(x, kPolicyClass));
}

id leafCert(Ctx& x, id certificates) {
    if (!certificates) return 0;
    if (ObjC::rt(x).hostIf<SecCertObj>(certificates)) return certificates;
    if (isArray(x, certificates)) {
        const auto items = arrayItems(x, certificates);
        if (!items.empty() && ObjC::rt(x).hostIf<SecCertObj>(items[0])) return items[0];
    }
    return 0;
}

int32_t sec_trustCreate(Ctx& x, id certificates, id /*policies*/, GuestPtr trustOut) {
    if (!trustOut) return errSecParam;
    x.mem.write<uint32_t>(trustOut, 0);
    const id leaf = leafCert(x, certificates);
    auto* c = leaf ? ObjC::rt(x).hostIf<SecCertObj>(leaf) : nullptr;
    if (!c) return errSecParam;
    const id key = makeKeyFromCert(x, *c);
    if (!key) return errSecParam;
    const id trust = ObjC::rt(x).createInstance(secClass(x, kTrustClass));
    auto h = std::make_unique<SecTrustObj>();
    h->cert = ObjC::rt(x).retain(x.cpu, leaf);
    h->key = key;   // already +1 from createInstance
#if defined(__APPLE__)
    if (c->host) {
        ::SecPolicyRef policy = ::SecPolicyCreateBasicX509();
        ::SecTrustRef ht = nullptr;
        if (policy && ::SecTrustCreateWithCertificates(c->host, policy, &ht) == errSecSuccess) h->host = ht;
        if (policy) CFRelease(policy);
    }
#endif
    ObjC::rt(x).setHost(trust, std::move(h));
    x.mem.write<uint32_t>(trustOut, trust);
    return errSecSuccess;
}

int32_t sec_trustEvaluate(Ctx& x, id trust, GuestPtr result) {
    auto* t = ObjC::rt(x).hostIf<SecTrustObj>(trust);
    if (!t || !t->key) return errSecParam;
    if (result) x.mem.write<uint32_t>(result, kSecTrustResultRecoverableTrustFailure);
    return errSecSuccess;
}

uint32_t sec_trustCopyPublicKey(Ctx& x, id trust) {
    auto* t = ObjC::rt(x).hostIf<SecTrustObj>(trust);
    if (!t || !t->key) return 0;
    return ObjC::rt(x).retain(x.cpu, t->key);
}

uint32_t sec_trustGetCertificateCount(Ctx& x, id trust) {
    auto* t = ObjC::rt(x).hostIf<SecTrustObj>(trust);
    return t && t->cert ? 1 : 0;
}
uint32_t sec_trustGetCertificateAtIndex(Ctx& x, id trust, uint32_t index) {
    auto* t = ObjC::rt(x).hostIf<SecTrustObj>(trust);
    if (!t || !t->cert || index != 0) return 0;
    return t->cert;   // Apple does not give a +1 here (borrowed)
}

uint32_t sec_keyGetBlockSize(Ctx& x, id key) {
    auto* k = ObjC::rt(x).hostIf<SecKeyObj>(key);
    if (!k) return 0;
#if defined(__APPLE__)
    return k->host ? uint32_t(::SecKeyGetBlockSize(k->host)) : 0;
#elif defined(HVM_HAVE_OPENSSL)
    return k->key ? uint32_t(EVP_PKEY_get_size(k->key)) : 0;
#else
    return 0;
#endif
}

int opensslPadding(uint32_t padding) {
#if defined(HVM_HAVE_OPENSSL) && !defined(__APPLE__)
    if (padding == kSecPaddingNone) return RSA_NO_PADDING;
    if (padding == kSecPaddingPKCS1) return RSA_PKCS1_PADDING;
    if (padding == kSecPaddingOAEP) return RSA_PKCS1_OAEP_PADDING;
#else
    (void)padding;
#endif
    return -1;
}

int32_t sec_keyEncrypt(Ctx& x, id key, uint32_t padding, GuestPtr plain, uint32_t plainLen,
                       GuestPtr cipher, GuestPtr cipherLen) {
    auto* k = ObjC::rt(x).hostIf<SecKeyObj>(key);
    if (!k || !plain || !cipher || !cipherLen) return errSecParam;
    const uint32_t outCap = x.mem.read<uint32_t>(cipherLen);
    const uint32_t block = sec_keyGetBlockSize(x, key);
    if (!block) return errSecParam;
    if (outCap < block) return errSecBufferTooSmall;
    if (padding == kSecPaddingNone && plainLen != block) return errSecParam;
    std::vector<uint8_t> in(plainLen), out(block);
    if (plainLen) x.mem.copyOut(in.data(), plain, plainLen);
#if defined(__APPLE__)
    if (!k->host) return errSecParam;
    size_t n = out.size();
    const OSStatus st = ::SecKeyEncrypt(k->host, ::SecPadding(padding), in.data(), in.size(), out.data(), &n);
    if (st != errSecSuccess) return int32_t(st);
    x.mem.copyIn(cipher, out.data(), n);
    x.mem.write<uint32_t>(cipherLen, uint32_t(n));
    return errSecSuccess;
#elif defined(HVM_HAVE_OPENSSL)
    if (!k->key) return errSecParam;
    const int pad = opensslPadding(padding);
    if (pad < 0) return errSecParam;
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(k->key, nullptr);
    if (!ctx) return errSecParam;
    size_t n = out.size();
    bool ok = EVP_PKEY_encrypt_init(ctx) == 1 && EVP_PKEY_CTX_set_rsa_padding(ctx, pad) == 1 &&
              EVP_PKEY_encrypt(ctx, out.data(), &n, in.data(), in.size()) == 1;
    EVP_PKEY_CTX_free(ctx);
    if (!ok) return errSecParam;
    x.mem.copyIn(cipher, out.data(), n);
    x.mem.write<uint32_t>(cipherLen, uint32_t(n));
    return errSecSuccess;
#else
    return errSecUnimplemented;
#endif
}

int32_t sec_keyDecrypt(Ctx& x, id key, uint32_t padding, GuestPtr cipher, uint32_t cipherLen,
                       GuestPtr plain, GuestPtr plainLen) {
    // Public-only keys cannot decrypt. A private key is not produced by this path.
    (void)x; (void)key; (void)padding; (void)cipher; (void)cipherLen; (void)plain; (void)plainLen;
    return errSecUnimplemented;
}

int32_t sec_keyRawVerify(Ctx&, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {
    return errSecUnimplemented;
}

void trust_dealloc(Ctx& x, id self, SEL s) {
    if (auto* t = ObjC::rt(x).hostIf<SecTrustObj>(self)) {
        if (t->cert) { ObjC::rt(x).release(x.cpu, t->cert); t->cert = 0; }
        if (t->key) { ObjC::rt(x).release(x.cpu, t->key); t->key = 0; }
    }
    ObjC::rt(x).sendSuper(x.cpu, self, ObjC::rt(x).getClass("NSObject"), s, {});
}

} // namespace

void registerSecurity(Process& p, ObjC::Runtime& r) {
    r.defineClass(kCertClass, "NSObject");
    r.defineClass(kPolicyClass, "NSObject");
    r.defineClass(kKeyClass, "NSObject");
    r.defineClass(kTrustClass, "NSObject").m<&trust_dealloc>("dealloc", "v@:");
    p.def<&sec_certificateCreateWithData>("_SecCertificateCreateWithData");
    p.def<&sec_certificateCopySubjectSummary>("_SecCertificateCopySubjectSummary");
    p.def<&sec_certificateCopyData>("_SecCertificateCopyData");
    p.def<&sec_keyGetBlockSize>("_SecKeyGetBlockSize");
    p.def<&sec_keyEncrypt>("_SecKeyEncrypt");
    p.def<&sec_keyDecrypt>("_SecKeyDecrypt");
    p.def<&sec_keyRawVerify>("_SecKeyRawVerify");
    p.def<&sec_trustCreate>("_SecTrustCreateWithCertificates");
    p.def<&sec_trustEvaluate>("_SecTrustEvaluate");
    p.def<&sec_trustCopyPublicKey>("_SecTrustCopyPublicKey");
    p.def<&sec_trustGetCertificateCount>("_SecTrustGetCertificateCount");
    p.def<&sec_trustGetCertificateAtIndex>("_SecTrustGetCertificateAtIndex");
    p.def<&sec_policyCreateBasicX509>("_SecPolicyCreateBasicX509");

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
