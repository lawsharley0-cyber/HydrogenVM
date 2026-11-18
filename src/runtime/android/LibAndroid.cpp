// HydrogenVM — libandroid.so: the NDK's native APIs for games. See Android.hpp.
//
// So far: assets (AAssetManager, AAsset, AAssetDir), read from the APK's extracted assets/,
// and AConfiguration with fixed values. Input queues, loopers, windows and sensors come with
// the NativeActivity driver.
#include "runtime/android/Android.hpp"
#include "runtime/android/BionicInternal.hpp"
#include "runtime/android/Jni.hpp"
#include "runtime/Support.hpp"
#include <cstdio>
#include <unistd.h>
#include <filesystem>
#include <mutex>
#include <unordered_map>

namespace fs = std::filesystem;

namespace Hydrogen::Runtime::Android {
namespace {

struct Asset {
    std::FILE* f = nullptr;
    int64_t size = 0;
    uint32_t buffer = 0;   // AAsset_getBuffer: the whole asset in guest memory, made on demand
};
struct AssetDir {
    std::vector<std::string> names;
    size_t next = 0;
    uint32_t nameBuf = 0;
};
struct Assets {
    explicit Assets(Process&) {}
    std::mutex mu;
    uint32_t manager = 0;   // the one AAssetManager (a guest cell, so the pointer is real memory)
    std::unordered_map<uint32_t, Asset> open;
    std::unordered_map<uint32_t, AssetDir> dirs;
};
Assets& A(Ctx& x) { return x.proc.service<Assets>(); }
std::string root(Process& p) { return p.config().bundlePath + "/assets"; }

// Asset names are relative to assets/; no escaping it.
std::string assetPath(Process& p, std::string name) {
    while (!name.empty() && name.front() == '/') name.erase(name.begin());
    const fs::path rel = fs::path(name).lexically_normal();
    if (rel.empty() || *rel.begin() == "..") return {};
    return root(p) + "/" + rel.string();
}

uint32_t m_fromJava(Ctx& x, GuestPtr, GuestPtr) {
    return assetManagerHandle(x.proc);
}
uint32_t m_open(Ctx& x, GuestPtr, GuestPtr nameP, int32_t) {
    const std::string name = x.proc.readCString(nameP, 1024);
    const std::string path = assetPath(x.proc, name);
    std::FILE* f = path.empty() ? nullptr : std::fopen(path.c_str(), "rb");
    x.proc.recordFileOpen("assets/" + name, "AAssetManager_open", f ? 0 : errno, -1, 0, f != nullptr);
    if (!f) return 0;
    Asset as;
    as.f = f;
    std::fseek(f, 0, SEEK_END);
    as.size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    const uint32_t h = x.proc.heap().allocZeroed(16);
    auto& a = A(x);
    std::lock_guard<std::mutex> lk(a.mu);
    a.open[h] = as;
    return h;
}
Asset* find(Assets& a, uint32_t h) {
    auto it = a.open.find(h);
    return it == a.open.end() ? nullptr : &it->second;
}
int32_t s_read(Ctx& x, GuestPtr h, GuestPtr buf, uint32_t n) {
    auto& a = A(x);
    std::lock_guard<std::mutex> lk(a.mu);
    Asset* as = find(a, h);
    if (!as) return -1;
    return n ? int32_t(std::fread(x.mem.host(buf), 1, n, as->f)) : 0;
}
int64_t seekTo(Asset& as, int64_t off, int32_t whence) {
    const int64_t base = whence == 1 ? std::ftell(as.f) : whence == 2 ? as.size : 0;
    const int64_t to = base + off;
    if (to < 0 || to > as.size) return -1;
    std::fseek(as.f, long(to), SEEK_SET);
    return to;
}
int32_t s_seek(Ctx& x, GuestPtr h, int32_t off, int32_t whence) {
    auto& a = A(x);
    std::lock_guard<std::mutex> lk(a.mu);
    Asset* as = find(a, h);
    return as ? int32_t(seekTo(*as, off, whence)) : -1;
}
int64_t s_seek64(Ctx& x, GuestPtr h, int64_t off, int32_t whence) {
    auto& a = A(x);
    std::lock_guard<std::mutex> lk(a.mu);
    Asset* as = find(a, h);
    return as ? seekTo(*as, off, whence) : -1;
}
int32_t s_length(Ctx& x, GuestPtr h) {
    auto& a = A(x);
    std::lock_guard<std::mutex> lk(a.mu);
    Asset* as = find(a, h);
    return as ? int32_t(as->size) : 0;
}
int64_t s_length64(Ctx& x, GuestPtr h) {
    auto& a = A(x);
    std::lock_guard<std::mutex> lk(a.mu);
    Asset* as = find(a, h);
    return as ? as->size : 0;
}
int32_t s_remaining(Ctx& x, GuestPtr h) {
    auto& a = A(x);
    std::lock_guard<std::mutex> lk(a.mu);
    Asset* as = find(a, h);
    return as ? int32_t(as->size - std::ftell(as->f)) : 0;
}
int64_t s_remaining64(Ctx& x, GuestPtr h) {
    auto& a = A(x);
    std::lock_guard<std::mutex> lk(a.mu);
    Asset* as = find(a, h);
    return as ? as->size - std::ftell(as->f) : 0;
}
uint32_t s_buffer(Ctx& x, GuestPtr h) {
    auto& a = A(x);
    std::lock_guard<std::mutex> lk(a.mu);
    Asset* as = find(a, h);
    if (!as) return 0;
    if (!as->buffer && as->size >= 0 && as->size < (1ll << 30)) {
        as->buffer = x.proc.heap().alloc(uint32_t(std::max<int64_t>(as->size, 1)));
        if (!as->buffer) return 0;
        const long at = std::ftell(as->f);
        std::fseek(as->f, 0, SEEK_SET);
        const size_t got = std::fread(x.mem.host(as->buffer), 1, size_t(as->size), as->f);
        (void)got;
        std::fseek(as->f, at, SEEK_SET);
    }
    return as->buffer;
}
// AAsset_openFileDescriptor(asset, outStart, outLength): a descriptor of the asset's file.
int32_t s_openFd(Ctx& x, GuestPtr h, GuestPtr start, GuestPtr length) {
    auto& a = A(x);
    std::lock_guard<std::mutex> lk(a.mu);
    Asset* as = find(a, h);
    if (!as) return -1;
    const int fd = ::dup(fileno(as->f));
    if (fd < 0) return -1;
    if (start) x.mem.write<int32_t>(start, 0);
    if (length) x.mem.write<int32_t>(length, int32_t(as->size));
    return fd;
}
int32_t s_isAllocated(Ctx&, GuestPtr) { return 0; }
void s_close(Ctx& x, GuestPtr h) {
    uint32_t buffer = 0;
    {
        auto& a = A(x);
        std::lock_guard<std::mutex> lk(a.mu);
        auto it = a.open.find(h);
        if (it == a.open.end()) return;
        if (it->second.f) std::fclose(it->second.f);
        buffer = it->second.buffer;
        a.open.erase(it);
    }
    if (buffer) x.proc.heap().free(buffer);
    x.proc.heap().free(h);
}

uint32_t d_open(Ctx& x, GuestPtr, GuestPtr dirP) {
    const std::string dir = x.proc.readCString(dirP, 1024);
    const std::string path = dir.empty() ? root(x.proc) : assetPath(x.proc, dir);
    AssetDir d;
    std::error_code ec;
    if (!path.empty())
        for (auto& e : fs::directory_iterator(path, ec))
            if (e.is_regular_file(ec)) d.names.push_back(e.path().filename().string());   // files only, as on Android
    const uint32_t h = x.proc.heap().allocZeroed(16);
    auto& a = A(x);
    std::lock_guard<std::mutex> lk(a.mu);
    a.dirs[h] = std::move(d);
    return h;
}
uint32_t d_next(Ctx& x, GuestPtr h) {
    auto& a = A(x);
    std::lock_guard<std::mutex> lk(a.mu);
    auto it = a.dirs.find(h);
    if (it == a.dirs.end() || it->second.next >= it->second.names.size()) return 0;
    auto& d = it->second;
    if (d.nameBuf) x.proc.heap().free(d.nameBuf);
    d.nameBuf = x.proc.allocCString(d.names[d.next++]);
    return d.nameBuf;
}
void d_rewind(Ctx& x, GuestPtr h) {
    auto& a = A(x);
    std::lock_guard<std::mutex> lk(a.mu);
    if (auto it = a.dirs.find(h); it != a.dirs.end()) it->second.next = 0;
}
void d_close(Ctx& x, GuestPtr h) {
    uint32_t buf = 0;
    {
        auto& a = A(x);
        std::lock_guard<std::mutex> lk(a.mu);
        auto it = a.dirs.find(h);
        if (it == a.dirs.end()) return;
        buf = it->second.nameBuf;
        a.dirs.erase(it);
    }
    if (buf) x.proc.heap().free(buf);
    x.proc.heap().free(h);
}

// AConfiguration: a phone in landscape, English, medium-large screen, touch, no keyboard.
uint32_t c_new(Ctx& x) { return x.proc.heap().allocZeroed(64); }
void c_delete(Ctx& x, GuestPtr c) { if (c) x.proc.heap().free(c); }
void c_fromAssetManager(Ctx&, GuestPtr, GuestPtr) {}
int32_t c_orientation(Ctx&, GuestPtr) { return 2; }   // ACONFIGURATION_ORIENTATION_LAND
int32_t c_density(Ctx&, GuestPtr) { return 320; }     // xhdpi
int32_t c_screenSize(Ctx&, GuestPtr) { return 3; }    // large
int32_t c_touchscreen(Ctx&, GuestPtr) { return 3; }   // finger
int32_t c_keyboard(Ctx&, GuestPtr) { return 1; }      // none
int32_t c_navigation(Ctx&, GuestPtr) { return 1; }    // nonav
int32_t c_sdk(Ctx&, GuestPtr) { return 19; }
int32_t c_zero(Ctx&, GuestPtr) { return 0; }
void c_language(Ctx& x, GuestPtr, GuestPtr out) { x.mem.write<uint8_t>(out, 'e'); x.mem.write<uint8_t>(out + 1, 'n'); }
void c_country(Ctx& x, GuestPtr, GuestPtr out) { x.mem.write<uint8_t>(out, 'U'); x.mem.write<uint8_t>(out + 1, 'S'); }

void na_finish(Ctx& x, GuestPtr) { x.proc.requestStop(); }
void na_noop2(Ctx&, GuestPtr, uint32_t) {}
void na_noop3(Ctx&, GuestPtr, uint32_t, uint32_t) {}

} // namespace

uint32_t assetManagerHandle(Process& p) {
    auto& a = p.service<Assets>();
    std::lock_guard<std::mutex> lk(a.mu);
    if (!a.manager) a.manager = p.allocStatic(16);
    return a.manager;
}

void registerLibAndroid(Process& p) {
#define R(F, NAME, WHAT) supportDef<&F>(p, std::string("android:") + NAME, SupportStatus::Real, WHAT)
#define A(F, NAME, WHAT) supportDef<&F>(p, std::string("android:") + NAME, SupportStatus::Approx, WHAT)
    R(m_fromJava, "AAssetManager_fromJava", "the APK's assets");
    R(m_open, "AAssetManager_open", "a file of the APK's assets");
    R(s_read, "AAsset_read", "host read");
    R(s_seek, "AAsset_seek", "host seek");
    R(s_seek64, "AAsset_seek64", "host seek");
    R(s_length, "AAsset_getLength", "file size");
    R(s_length64, "AAsset_getLength64", "file size");
    R(s_remaining, "AAsset_getRemainingLength", "size less position");
    R(s_remaining64, "AAsset_getRemainingLength64", "size less position");
    R(s_buffer, "AAsset_getBuffer", "the whole asset read into guest memory");
    R(s_openFd, "AAsset_openFileDescriptor", "a descriptor of the extracted file");
    R(s_isAllocated, "AAsset_isAllocated", "assets are files");
    R(s_close, "AAsset_close", "host close");
    R(d_open, "AAssetManager_openDir", "the files of an assets folder");
    R(d_next, "AAssetDir_getNextFileName", "next file name");
    R(d_rewind, "AAssetDir_rewind", "back to the first name");
    R(d_close, "AAssetDir_close", "frees the listing");
    R(c_new, "AConfiguration_new", "fixed device configuration");
    R(c_delete, "AConfiguration_delete", "frees it");
    A(c_fromAssetManager, "AConfiguration_fromAssetManager", "the fixed configuration");
    A(c_orientation, "AConfiguration_getOrientation", "landscape");
    A(c_density, "AConfiguration_getDensity", "320 dpi");
    A(c_screenSize, "AConfiguration_getScreenSize", "large");
    A(c_touchscreen, "AConfiguration_getTouchscreen", "finger");
    A(c_keyboard, "AConfiguration_getKeyboard", "none");
    A(c_navigation, "AConfiguration_getNavigation", "none");
    A(c_sdk, "AConfiguration_getSdkVersion", "19");
    A(c_zero, "AConfiguration_getKeysHidden", "0");
    A(c_zero, "AConfiguration_getNavHidden", "0");
    A(c_zero, "AConfiguration_getUiModeType", "0");
    A(c_zero, "AConfiguration_getUiModeNight", "0");
    A(c_zero, "AConfiguration_getMcc", "0");
    A(c_zero, "AConfiguration_getMnc", "0");
    A(c_language, "AConfiguration_getLanguage", "en");
    A(c_country, "AConfiguration_getCountry", "US");
    R(na_finish, "ANativeActivity_finish", "stops the activity");
    A(na_noop2, "ANativeActivity_setWindowFormat", "window format is fixed");
    A(na_noop3, "ANativeActivity_setWindowFlags", "window flags are fixed");
    A(na_noop2, "ANativeActivity_showSoftInput", "soft input not shown");
    A(na_noop2, "ANativeActivity_hideSoftInput", "soft input not shown");
#undef R
#undef A
}

} // namespace Hydrogen::Runtime::Android
