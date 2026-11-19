#include "runtime/foundation/Foundation.hpp"
#include <cstdio>
#include <utility>
using namespace Hydrogen;
using namespace Hydrogen::Runtime;
namespace {
bool astral(Ctx&, uint32_t, uint32_t, uint32_t c) { return c == 0x1f600; }
bool noBmp(Ctx&, uint32_t, uint32_t, uint32_t) { return false; }
}
int runNSStringSearchTests() {
    ProcessConfig cfg; cfg.homePath = "/tmp/hvm-string-search-tests";
    Process p(cfg); CPU::Cpu cpu;
    cpu.r[13] = p.heap().allocZeroed(65536) + 65536 - 16;
    Ctx x{p,cpu,p.mem()}; auto& r = ObjC::rt(p);
    const auto text = Foundation::makeString(x, u"ab\ncd\nz");
    const auto newline = uint32_t(r.send(cpu,r.getClass("NSCharacterSet"),"newlineCharacterSet"));
    const auto out = p.heap().allocZeroed(8);
    const char* name = "rangeOfCharacterFromSet:options:range:";
    const auto selector = r.sel(name);
    if (!r.lookup(r.classOf(text),selector)) {
        std::puts("FAIL: bounded NSString character-set search is missing (Canabalt text wrapping)"); return 1;
    }
    auto search = [&](uint32_t s, uint32_t set, uint32_t opts, uint32_t loc, uint32_t len, uint32_t output = 0) {
        p.callGuest(cpu, r.lookupForSend(r.classOf(s),selector), {output ? output : out,s,selector,set,opts,loc,len});
        return std::pair{p.mem().read<uint32_t>(out),p.mem().read<uint32_t>(out+4)};
    };
    // Literal UTF-16 positions, derived directly from "ab\ncd\nz".
    struct Case { uint32_t opts, loc, len, first, count; };
    const Case cases[] = {{0,0,7,2,1},{4,0,7,5,1},{0,3,3,5,1},
        {0,0,2,0x7fffffff,0},{8,0,7,0x7fffffff,0},{8,2,4,2,1},
        {12,0,6,5,1},{12,0,7,0x7fffffff,0},{0,7,0,0x7fffffff,0}};
    for (const auto c : cases) if (search(text,newline,c.opts,c.loc,c.len) != std::pair{c.first,c.count}) {
        std::printf("FAIL: bounded/anchored/backwards search opts=%u range={%u,%u}\n",c.opts,c.loc,c.len); return 1;
    }
    // Convenience selectors must share anchored behavior with the range API.
    const auto simple = r.sel("rangeOfCharacterFromSet:options:");
    p.callGuest(cpu,r.lookupForSend(r.classOf(text),simple),{out,text,simple,newline,8});
    if (p.mem().read<uint32_t>(out) != 0x7fffffff) { std::puts("FAIL: convenience search ignores anchoring"); return 1; }
    auto cls = r.defineClass("SearchAstralSet","NSCharacterSet");
    cls.m<&astral>("longCharacterIsMember:","c@:I").m<&noBmp>("characterIsMember:","c@:S");
    const auto set = r.createInstance(cls.cls());
    const auto emoji = Foundation::makeString(x,u"A\U0001f600B\U0001f600C");
    if (search(emoji,set,0,0,7) != std::pair{1u,2u} || search(emoji,set,4,0,7) != std::pair{4u,2u} ||
        search(emoji,set,0,2,4) != std::pair{4u,2u} || search(emoji,set,0,1,1) != std::pair{0x7fffffffu,0u}) {
        std::puts("FAIL: supplementary scalar membership and UTF-16 search boundaries"); return 1;
    }
    const auto umlaut = uint32_t(r.send(cpu,r.getClass("NSCharacterSet"),"characterSetWithCharactersInString:", {Foundation::makeString(x,u"\u00fc")}));
    const auto decomposed = Foundation::makeString(x,u"u\u0308");
    if (search(decomposed,umlaut,0,0,2).second != 0) { std::puts("FAIL: character-set search must not normalize the string"); return 1; }
    for (const auto c : {Case{0,8,0,0,0},Case{0,0xffffffff,2,0,0}}) {
        bool rejected = false;
        try { search(text,newline,0,c.loc,c.len); } catch (const std::exception& e) { rejected = std::string(e.what()).find("range") != std::string::npos; }
        if (!rejected) { std::puts("FAIL: invalid search range accepted"); return 1; }
    }
    bool nilRejected = false;
    try { search(text,0,0,0,7); } catch (const std::exception& e) { nilRejected = std::string(e.what()).find("nil") != std::string::npos; }
    if (!nilRejected) { std::puts("FAIL: nil character set accepted"); return 1; }
    const uint32_t ro = 0xbf340000; p.mem().map(ro,4096,Memory::kProtRead);
    std::memset(p.mem().host(ro),0x55,8);
    bool faulted = false;
    try { search(text,newline,0,0,7,ro); } catch (const CPU::GuestFault& e) { faulted = e.exit == CPU::Exit::MemFault; }
    if (!faulted || p.mem().read<uint32_t>(ro) != 0x55555555) { std::puts("FAIL: protected NSRange output was written"); return 1; }
    std::puts("NSString character-set searches: passed");
    return 0;
}

#ifndef HVM_CANABALT_CASES_NO_MAIN
int main() { return runNSStringSearchTests(); }
#endif
