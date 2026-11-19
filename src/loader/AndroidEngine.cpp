#include "AndroidEngine.hpp"

#include <algorithm>
#include <cctype>

namespace Hydrogen::Loader {

AndroidEngine detectAndroidEngine(const std::vector<std::string>& libs, const std::string& dex) {
    auto has = [&](const char* n) { return std::find(libs.begin(), libs.end(), n) != libs.end(); };
    auto any = [&](auto pred) { return std::any_of(libs.begin(), libs.end(), pred); };
    auto in = [&](const char* s) { return dex.find(s) != std::string::npos; };
    if (has("libil2cpp.so")) return {"Unity (IL2CPP)", "none"};
    if (has("libunity.so") && has("libmono.so")) return {"Unity (Mono)", "driver"};
    if (has("libunity.so")) return {"Unity", "driver"};
    if (has("libUE4.so")) return {"Unreal Engine 4", "none"};
    if (has("libUE3.so") || any([](const std::string& n) { return n.rfind("libUnrealEngine3", 0) == 0; })) return {"Unreal Engine 3", "none"};
    if (in("Lcom/ea/blast/")) return {"EA Blast", "driver"};
    if (has("libuaf.so")) return {"Ubisoft UAF", "driver"};
    if (in("Lorg/cocos2dx/") || any([](const std::string& n) {
            std::string l = n; for (char& c : l) c = char(std::tolower(static_cast<unsigned char>(c)));
            return l.find("cocos") != std::string::npos; }))
        return {"Cocos2d-x", "none"};
    if (has("libyoyo.so")) return {"GameMaker", "none"};
    if (has("libgdx.so") || in("Lcom/badlogic/gdx/")) return {"libGDX", "none"};
    if (has("libCore.so") && in("Lcom/adobe/air/")) return {"Adobe AIR", "none"};
    if (has("libcorona.so")) return {"Corona", "none"};
    if (any([](const std::string& n) { return n.rfind("libs3e", 0) == 0; })) return {"Marmalade", "none"};
    if (in("Landroid/app/NativeActivity;")) return {"NativeActivity", "planned"};
    if (libs.empty()) return {"Java only", "none"};
    return {"Java + native", "none"};
}

} // namespace Hydrogen::Loader
