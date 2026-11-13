#include "runtime/uikit/UIKit.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
using namespace Hydrogen;
using namespace Hydrogen::Runtime;
int main(int argc, char**) {
    ProcessConfig cfg; cfg.homePath = "/tmp/hvm-raster-rotation-test";
    Process p(cfg); CPU::Cpu cpu; cpu.r[13] = p.heap().allocZeroed(65536) + 65536 - 16;
    Ctx x{p, cpu, p.mem()}; auto& r = ObjC::rt(p);
    const auto layer = uint32_t(r.send(cpu, uint32_t(r.send(cpu, r.getClass("CAEAGLLayer"), "alloc")), "init"));
    r.send(cpu, layer, "setOpaque:", {1});
    r.send(cpu, layer, "setBounds:", {0,0,UIKit::fbits(2),UIKit::fbits(3)});
    r.send(cpu, layer, "setContentsScale:", {UIKit::fbits(1)});
    const auto context = uint32_t(r.send(cpu, uint32_t(r.send(cpu, r.getClass("EAGLContext"), "alloc")), "initWithAPI:", {2}));
    r.send(cpu, r.getClass("EAGLContext"), "setCurrentContext:", {context});
    auto call = [&](const char* name, std::initializer_list<uint32_t> args) { return p.callGuest(cpu, *p.lookupSymbol(name), args); };
    const auto ids = p.heap().allocZeroed(8);
    call("_glGenRenderbuffers", {1, ids});
    const auto rb = p.mem().read<uint32_t>(ids);
    call("_glBindRenderbuffer", {0x8d41, rb});
    r.send(cpu, context, "renderbufferStorage:fromDrawable:", {0x8d41, layer});
    call("_glGenFramebuffers", {1, ids + 4});
    call("_glBindFramebuffer", {0x8d40, p.mem().read<uint32_t>(ids + 4)});
    call("_glFramebufferRenderbuffer", {0x8d40,0x8ce0,0x8d41,rb});
    call("_glClearColor", {UIKit::fbits(1),0,0,0}); call("_glClear", {0x4000});
    r.send(cpu, context, "presentRenderbuffer:", {0x8d41});
    const uint8_t rgba[] = {10,20,30,0, 40,50,60,0, 70,80,90,0, 100,110,120,0, 130,140,150,0, 160,170,180,0};
    const auto frame = UIKit::layerData(x, layer).externalContents();
    bool ok = frame && frame->opaque;
    if (frame) for (size_t i = 3; i < frame->rgba.size(); i += 4) ok &= frame->rgba[i] == 255;
    Graphics::Image image; image.width = 2; image.height = 3; image.rgba.assign(rgba, rgba + 24); image.opaque = true;
    for (size_t i = 3; i < image.rgba.size(); i += 4) image.rgba[i] = 255;
    std::vector<uint8_t> pixels(24, 0);
    Graphics::Surface dst{pixels.data(), 3, 2, 12, Graphics::PixelFormat::RGBA, true};
    const Graphics::Affine rotate{0,1,-1,0,3,0};
    Graphics::drawImage(dst, image, rotate, {0,0,3,2}, 1, true);
    const uint8_t expected[] = {130,140,150,255,70,80,90,255,10,20,30,255,160,170,180,255,100,110,120,255,40,50,60,255};
    ok &= std::memcmp(pixels.data(), expected, 24) == 0;
    if (!ok) std::puts("FAIL: opaque GL presentation metadata and exact quarter-turn pixel mapping");
    if (argc > 1) {
        image.width = 1136; image.height = 640; image.rgba.assign(1136 * 640 * 4, 127);
        for (size_t i = 3; i < image.rgba.size(); i += 4) image.rgba[i] = 255;
        pixels.resize(image.rgba.size()); dst = {pixels.data(), 640, 1136, 640 * 4, Graphics::PixelFormat::RGBA, true};
        const Graphics::Affine turn{0,1,-1,0,640,0};
        std::vector<double> times;
        for (int i = 0; i < 12; ++i) {
            const auto start = std::chrono::steady_clock::now();
            Graphics::drawImage(dst, image, turn, {0,0,640,1136}, 1, true);
            times.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now() - start).count());
        }
        std::sort(times.begin(), times.end());
        std::printf("opaque 1136x640 quarter-turn median %.3f ms (local benchmark)\n", times[6]);
        if (times[6] >= 33) return 1;
    }
    if (ok) std::puts("raster quarter-turn: passed");
    return ok ? 0 : 1;
}
