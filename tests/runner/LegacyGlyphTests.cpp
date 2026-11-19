#include "runtime/uikit/CoreGraphics.hpp"
#include "TestFont.hpp"
#include <bit>
#include <cstdio>
using namespace Hydrogen;
using namespace Hydrogen::Runtime;
int runLegacyGlyphTests() {
    ProcessConfig cfg; cfg.homePath = "/tmp/hvm-legacy-glyph-tests";
    Process p(cfg); CPU::Cpu cpu; cpu.r[13] = p.heap().allocZeroed(65536)+65536-16;
    Ctx x{p,cpu,p.mem()}; auto& r = ObjC::rt(p);
    auto call = [&](const char* name,std::initializer_list<uint32_t> args) { return uint32_t(p.callGuest(cpu,*p.lookupSymbol(name),args)); };
    auto f = [](float value) { return std::bit_cast<uint32_t>(value); };
    const auto data = p.heap().allocZeroed(sizeof(kTestFont)); p.mem().copyIn(data,kTestFont,sizeof(kTestFont));
    const auto provider = call("_CGDataProviderCreateWithData",{0,data,sizeof(kTestFont),0});
    const auto font = call("_CGFontCreateWithDataProvider",{provider});
    const auto ctx = call("_CGBitmapContextCreate",{0,64,48,8,64,0,7});
    call("_CGContextSetFont",{ctx,font}); call("_CGContextSetFontSize",{ctx,f(20)});
    call("_CGContextSaveGState",{ctx});
    call("_CGContextSelectFont",{ctx,p.allocCString("Helvetica"),f(10),0});
    call("_CGContextRestoreGState",{ctx});
    r.release(cpu,font); r.release(cpu,provider);
    const auto glyph = p.heap().allocZeroed(2); p.mem().write<uint16_t>(glyph,2);
    const auto position = p.heap().allocZeroed(8);
    const auto pixels = call("_CGBitmapContextGetData",{ctx});
    auto at = [&](int a,int b) { return p.mem().read<uint8_t>(pixels+uint32_t(b*64+a)); };
    auto pos = [&] { call("_CGContextGetTextPosition",{position,ctx}); return CG::readStruct<CG::CGPoint>(x,position); };
    // CC0 font A is a triangle at (100,0),(300,700),(500,0), advance 600,
    // units/em=1000. At size 20, origin=(4,8), pixel (10,35) is inside.
    call("_CGContextSetCharacterSpacing",{ctx,f(2)});
    call("_CGContextSetTextPosition",{ctx,f(4),f(8)});
    call("_CGContextShowGlyphs",{ctx,glyph,1});
    if (at(10,35)<250 || at(4,35)!=0 || pos().x!=18 || pos().y!=8) {
        std::puts("FAIL: legacy glyphs must render the selected outline and advance text position"); return 1;
    }
    std::memset(p.mem().host(pixels),0,64*48);
    call("_CGContextShowGlyphsAtPoint",{ctx,f(4),f(8),glyph,1});
    if (at(10,35)<250 || pos().x!=18) { std::puts("FAIL: ShowGlyphsAtPoint float/pointer/count ABI"); return 1; }
    std::memset(p.mem().host(pixels),0,64*48);
    call("_CGContextSetTextDrawingMode",{ctx,3});
    call("_CGContextShowGlyphsAtPoint",{ctx,f(4),f(8),glyph,1});
    bool blank = true; for (int i=0;i<64*48;++i) blank &= p.mem().read<uint8_t>(pixels+uint32_t(i))==0;
    if (!blank || pos().x!=18) { std::puts("FAIL: invisible glyphs advance without drawing"); return 1; }
    call("_CGContextSetTextDrawingMode",{ctx,0});
    call("_CGContextSetTextMatrix",{ctx,f(2),0,0,f(1),0,0});
    call("_CGContextShowGlyphsAtPoint",{ctx,f(4),f(8),glyph,1});
    if (at(16,35)<250 || pos().x!=32) { std::puts("FAIL: text matrix transforms glyphs and advances"); return 1; }
    std::memset(p.mem().host(pixels),0,64*48);
    call("_CGContextSetTextMatrix",{ctx,f(1),0,0,f(1),0,0});
    call("_CGContextSetTextDrawingMode",{ctx,7});
    call("_CGContextShowGlyphsAtPoint",{ctx,f(4),f(8),glyph,1});
    if (at(10,35)!=0) { std::puts("FAIL: clip-only glyphs cannot paint"); return 1; }
    call("_CGContextFillRect",{ctx,0,0,f(64),f(48)});
    if (at(10,35)<250 || at(4,35)!=0) { std::puts("FAIL: glyph outlines clip subsequent drawing"); return 1; }
    bool faulted = false;
    try { call("_CGContextShowGlyphs",{ctx,0xfffffff8,32}); }
    catch (const CPU::GuestFault& e) { faulted = e.exit==CPU::Exit::MemFault; }
    if (!faulted) { std::puts("FAIL: invalid glyph array must raise a controlled guest read fault"); return 1; }
    std::puts("legacy CGContext glyphs: passed");
    return 0;
}

#ifndef HVM_CANABALT_CASES_NO_MAIN
int main() { return runLegacyGlyphTests(); }
#endif
