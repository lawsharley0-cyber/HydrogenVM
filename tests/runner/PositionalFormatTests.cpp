#include "runtime/libc/Format.hpp"
#include "runtime/foundation/Foundation.hpp"
#include <bit>
#include <cstdio>
using namespace Hydrogen;
using namespace Hydrogen::Runtime;
int main() {
    ProcessConfig cfg; cfg.homePath = "/tmp/hvm-positional-format-tests";
    Process p(cfg); CPU::Cpu cpu; cpu.r[13]=p.heap().allocZeroed(65536)+65536-256;
    Ctx x{p,cpu,p.mem()};
    const auto values = p.heap().allocZeroed(128);
    auto format = [&](const std::string& text,const std::vector<uint32_t>& words,bool registers) {
        std::memcpy(p.mem().host(values),words.data(),words.size()*4);
        cpu.r[3]=words.empty()?0:words[0];
        if (words.size()>1) p.mem().copyIn(cpu.r[13],words.data()+1,(words.size()-1)*4);
        ArgReader a = registers ? ArgReader(cpu,p.mem(),3) : ArgReader(p.mem(),values);
        return formatGuest(p,text,a,[&](uint32_t value) { cpu.r[3]=0; return value==17?"RUN":value==29?"MACH":"?"; });
    };
    for (bool registers : {false,true}) {
        if (format("Now Playing: \"%1$@\"",{17},registers)!="Now Playing: \"RUN\"" ||
            format("%2$@/%1$@/%2$@",{17,29},registers)!="MACH/RUN/MACH") {
            std::puts("FAIL: positional Cocoa objects must reorder/repeat without argument clobbering"); return 1;
        }
        const auto d=std::bit_cast<uint64_t>(2.5);
        if (format("%3$lld/%2$.2f/%1$d",{7,uint32_t(d),uint32_t(d>>32),2,1},registers)!="4294967298/2.50/7") {
            std::puts("FAIL: positional arguments preserve 32/64-bit ARMv7 argument widths"); return 1;
        }
        const auto f=std::bit_cast<uint64_t>(3.5);
        if (format("%3$*1$.*2$f",{10,2,uint32_t(f),uint32_t(f>>32)},registers)!="      3.50" ||
            format("%2$d/%1$#x",{42,7},registers)!="7/0x2a" ||
            format("Cost $%1$d; %% %1$d",{7},registers)!="Cost $7; % 7") {
            std::puts("FAIL: positional star width/precision, flags and literal percent/dollar"); return 1;
        }
    }
    for (const auto text : {"%0$d","%4097$d","%2$d","%1$d %1$lld","%1$d %d"}) {
        bool rejected=false;
        try { format(text,{7},false); } catch (const std::invalid_argument&) { rejected=true; }
        if (!rejected) { std::puts("FAIL: invalid positional layouts must fail before guessing argument types"); return 1; }
    }
    auto& r=ObjC::rt(p);
    const auto text=Foundation::makeString(x,"Now Playing: \"%1$@\"");
    const auto song=Foundation::makeString(x,"RUN");
    const auto result=uint32_t(r.send(cpu,r.getClass("NSString"),"stringWithFormat:",{text,song}));
    if (Foundation::utf8(x,result)!="Now Playing: \"RUN\"") { std::puts("FAIL: NSString variadic call uses indexed formatting"); return 1; }
    std::puts("positional guest formatting: passed");
}
