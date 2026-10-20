// HydrogenVM — Foundation / CoreFoundation registration.
#include "Foundation.hpp"
#include <cctype>
#include <cstring>
#include <unordered_map>

namespace Hydrogen::Runtime {

void registerFoundation(Process& p) {
    using namespace Foundation;
    ObjC::Runtime& r = ObjC::rt(p);
    // Order matters only where one module's registration uses another's
    // classes or constants (strings first: everything makes constant strings).
    registerStrings(p, r);
    registerNumbers(p, r);
    registerCollections(p, r);
    registerData(p, r);
    registerRunLoop(p, r);
    registerMisc(p, r);
    registerNotifications(p, r);
    registerBundle(p, r);
    registerDefaults(p, r);
    registerDispatch(p, r);
    registerCoreFoundation(p, r);
    registerArchiving(p, r);
    registerFormatters(p, r);
    registerSecurity(p, r);
    registerNetwork(p, r);
    registerCalendar(p, r);
    registerInvocation(p, r);
    registerURLLoading(p, r);
    registerFoundationExtras(p, r);

    // String constants from frameworks we don't otherwise model: a constant NSString
    // instead of nil, so apps can still use them as dictionary keys and notification names.
    p.addMissingSymbolResolver([&p](const std::string& sym) -> uint32_t {
        if (sym.size() < 4 || sym[0] != '_' || !std::isalpha(uint8_t(sym[1]))) return 0;
        const std::string name = sym.substr(1);
        static const std::unordered_map<std::string, std::string> known = {
            {"kCFBundleExecutableKey", "CFBundleExecutable"}, {"kCFBundleIdentifierKey", "CFBundleIdentifier"},
            {"kCFBundleNameKey", "CFBundleName"}, {"kCFBundleVersionKey", "CFBundleVersion"},
            {"kCFBundleInfoDictionaryVersionKey", "CFBundleInfoDictionaryVersion"},
            {"kCFBundleDevelopmentRegionKey", "CFBundleDevelopmentRegion"}, {"kCFBundleLocalizationsKey", "CFBundleLocalizations"},
            {"NSKeyValueChangeNewKey", "new"}, {"NSKeyValueChangeOldKey", "old"}, {"NSKeyValueChangeKindKey", "kind"},
            {"NSKeyValueChangeIndexesKey", "indexes"}, {"NSKeyValueChangeNotificationIsPriorKey", "notificationIsPrior"},
            {"NSHTTPCookieDomain", "Domain"}, {"NSHTTPCookieName", "Name"}, {"NSHTTPCookiePath", "Path"},
            {"NSHTTPCookieValue", "Value"}, {"NSHTTPCookieExpires", "Expires"}, {"NSHTTPCookieVersion", "Version"},
            {"NSHTTPCookieSecure", "Secure"}, {"NSHTTPCookieOriginURL", "OriginURL"},
            {"kCFHTTPVersion1_0", "HTTP/1.0"}, {"kCFHTTPVersion1_1", "HTTP/1.1"},
            {"NSURLAuthenticationMethodServerTrust", "NSURLAuthenticationMethodServerTrust"},
            {"NSURLAuthenticationMethodClientCertificate", "NSURLAuthenticationMethodClientCertificate"},
        };
        static const std::unordered_map<std::string, double> accuracy = {
            {"kCLLocationAccuracyBestForNavigation", -2}, {"kCLLocationAccuracyBest", -1}, {"kCLLocationAccuracyNearestTenMeters", 10},
            {"kCLLocationAccuracyHundredMeters", 100}, {"kCLLocationAccuracyKilometer", 1000}, {"kCLLocationAccuracyThreeKilometers", 3000},
            {"kCLDistanceFilterNone", -1}};
        if (auto it = accuracy.find(name); it != accuracy.end()) {
            const uint32_t cell = p.allocStatic(8);
            p.mem().write<double>(cell, it->second);
            return cell;
        }
        if (name == "kCMTimeZero" || name == "kCMTimeInvalid" || name == "kCMTimePositiveInfinity") {
            const uint32_t cell = p.allocStatic(24);   // CMTime {int64 value; int32 timescale; uint32 flags; int64 epoch}
            p.mem().write<uint32_t>(cell + 8, name == "kCMTimeInvalid" ? 0 : 1);
            p.mem().write<uint32_t>(cell + 12, name == "kCMTimeZero" ? 1 : name == "kCMTimeInvalid" ? 0 : 5);
            return cell;
        }
        auto ends = [&](const char* e) { const size_t n = std::strlen(e); return name.size() > n && name.compare(name.size() - n, n, e) == 0; };
        std::string value;
        if (auto it = known.find(name); it != known.end()) value = it->second;
        else if (ends("Notification") || ends("Key") || ends("Domain") || ends("AttributeName") || ends("Exception") ||
                 (name[0] == 'k' && (name.find("Property") != std::string::npos || name.find("SSL") != std::string::npos ||
                                     name.find("ServiceType") != std::string::npos)))
            value = name;
        else return 0;
        const uint32_t cell = p.allocStatic(4);
        p.mem().write<uint32_t>(cell, constString(p, value));
        return cell;
    });
}

} // namespace Hydrogen::Runtime
