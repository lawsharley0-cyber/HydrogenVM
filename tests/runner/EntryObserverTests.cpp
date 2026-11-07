#include "core/cpu/Engine.hpp"
#include <cstdio>
#include <vector>
using namespace Hydrogen;
int main() {
    int failed=0;
    auto check=[&](bool good,const char* name) { std::printf("%s %s\n",good?"ok":"FAIL",name); failed+=!good; };
    Memory::GuestArena memory;
    memory.map(0x1000,0x1000,Memory::kProtRX);
    memory.map(0x7000,0x1000,Memory::kProtRW);
    const uint32_t caller[]={0xe92d4010,0xe3a00000,0xe3a04005,0xeb00003b,0xe2544001,0x1afffffc,0xe8bd8010};
    const uint32_t callee[]={0xe2800007,0xe12fff1e};
    memory.copyIn(0x1000,caller,sizeof caller); memory.copyIn(0x1100,callee,sizeof callee);
    CPU::Engine engine(memory);
    std::vector<uint32_t> observed;
    std::vector<uint32_t> returned;
    engine.observeEntry(0x1100,[&](uint32_t pc,const CPU::Cpu& cpu) {if(pc==0x1100) observed.push_back(cpu.r[0]);});
    engine.observeEntry(0x1010,[&](uint32_t,const CPU::Cpu& cpu){returned.push_back(cpu.r[0]);});
    CPU::Cpu cpu; cpu.r[CPU::kSP]=0x8000;cpu.r[0]=123;cpu.nzcv=3;
    check(engine.callGuest(cpu,0x1000,{})==35 && cpu.r[0]==123 && cpu.nzcv==3 &&
        observed==std::vector<uint32_t>({0,7,14,21,28}),"cached direct calls preserve guest registers/flags");
    check(returned==std::vector<uint32_t>({7,14,21,28,35}),"return continuations observe each original call result");
    bool late=false;
    try {engine.observeEntry(0x1000,[](uint32_t,const CPU::Cpu&){});} catch(const std::logic_error&) {late=true;}
    check(late,"late observer configuration rejected");
    CPU::Engine failing(memory);
    failing.observeEntry(0x1100,[](uint32_t,const CPU::Cpu&){throw std::runtime_error("probe failure");});
    CPU::Cpu fc;fc.r[CPU::kSP]=0x8000;
    bool surfaced=false;
    try {failing.callGuest(fc,0x1000,{});} catch(const CPU::GuestFault& fault) {surfaced=fault.exit==CPU::Exit::HostError && fault.pc==0x1100;}
    check(surfaced,"observer failure becomes a precise HostError");
    const uint16_t thumb[]={0x3007,0x4770}; memory.copyIn(0x1200,thumb,sizeof thumb);
    CPU::Engine te(memory); uint32_t seen=0;
    te.observeEntry(0x1201,[&](uint32_t pc,const CPU::Cpu& tc){if(pc==0x1200 && tc.r[0]==5)++seen;});
    CPU::Cpu tc;tc.r[CPU::kSP]=0x8000;
    check(te.callGuest(tc,0x1201,{5})==12 && seen==1,"Thumb entry normalizes the mode bit");
    return failed?1:0;
}
