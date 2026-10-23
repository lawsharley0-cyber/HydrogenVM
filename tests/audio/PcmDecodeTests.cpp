// Synthetic PCM fixtures only: no commercial audio/assets.
#include "runtime/audio/Audio.hpp"
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>
using Bytes = std::vector<uint8_t>;
using Hydrogen::Runtime::Audio::decode;
void number(Bytes& b, uint64_t v, int bytes, bool be = false) {
    for (int i = 0; i < bytes; ++i) b.push_back(uint8_t(v >> (8 * (be ? bytes - 1 - i : i))));
}
void tag(Bytes& b, const char* s) { b.insert(b.end(), s, s + 4); }
Bytes wave(int bits, bool floating, int channels, const Bytes& pcm) {
    Bytes b; tag(b,"RIFF"); number(b,36+pcm.size(),4); tag(b,"WAVE"); tag(b,"fmt "); number(b,16,4);
    number(b,floating?3:1,2); number(b,channels,2); number(b,44100,4);
    number(b,44100*channels*(bits/8),4); number(b,channels*(bits/8),2); number(b,bits,2);
    tag(b,"data"); number(b,pcm.size(),4); b.insert(b.end(),pcm.begin(),pcm.end()); return b;
}
Bytes caf(int bits, bool floating, int channels, bool be, const Bytes& pcm) {
    Bytes b; tag(b,"caff"); number(b,1,2,true); number(b,0,2,true); tag(b,"desc"); number(b,32,8,true);
    number(b,std::bit_cast<uint64_t>(44100.0),8,true); tag(b,"lpcm"); number(b,(floating?1:0)|(be?0:2),4,true);
    number(b,channels*(bits/8),4,true); number(b,1,4,true); number(b,channels,4,true); number(b,bits,4,true);
    tag(b,"data"); number(b,pcm.size()+4,8,true); number(b,0,4,true); b.insert(b.end(),pcm.begin(),pcm.end()); return b;
}
int main(int argc, char** argv) {
    int passed=0, failed=0;
    // Filter lets the pre-fix sanitizer demonstrate each distinct failure independently.
    auto check=[&](const std::string& name, const Bytes& b, const std::vector<int16_t>& expected, int channels) {
        if (argc>1 && name.find(argv[1])==std::string::npos) return;
        const auto out=decode(b.data(),b.size());
        const bool ok=out.stereoPcm==expected && (expected.empty() ||
            (out.sampleRate==44100 && out.channels==channels &&
             std::abs(out.duration-double(expected.size()/2)/44100)<1e-12));
        std::printf("%s %s\n",ok?"PASS":"FAIL",name.c_str());
        ok?++passed:++failed;
    };
    for (int bits : {32,64}) {
        for (int layout=0;layout<3;++layout) {
            const bool be=layout==2;
            const std::string name="float"+std::to_string(bits)+(layout==0?"-wav":be?"-caf-be":"-caf-le");
            Bytes pcm;
            for(double v : {0.5,-0.5,1.0,-1.0,2.0,-2.0,0.0}) {
                number(pcm,bits==32?uint64_t(std::bit_cast<uint32_t>(float(v))):std::bit_cast<uint64_t>(v),bits/8,be);
            }
            check(name,layout==0?wave(bits,true,1,pcm):caf(bits,true,1,be,pcm),
                  {16383,16383,-16383,-16383,32767,32767,-32767,-32767,32767,32767,-32768,-32768,0,0},1);
            Bytes nonfinite;
            for(double v : {std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity()}) {
                number(nonfinite,bits==32?uint64_t(std::bit_cast<uint32_t>(float(v))):std::bit_cast<uint64_t>(v),bits/8,be);
            }
            check("nonfinite-"+name,layout==0?wave(bits,true,1,nonfinite):caf(bits,true,1,be,nonfinite),{0,0,32767,32767,-32768,-32768},1);
        }
    }
    for(int bits : {8,16,24,32}) {
        for(bool be : {false,true}) {
            Bytes pcm; number(pcm,bits==8?192: uint64_t(1)<<(bits-2),bits/8,be);
            number(pcm,bits==8?64: (uint64_t(3)<<(bits-2)),bits/8,be);
            // CAF shares the generic conversion with high-width WAVs; mono duplication above,
            // interleaved stereo and signed integer extension here.
            check("integer"+std::to_string(bits)+(be?"-be":"-le"),caf(bits,false,2,be,pcm),{16383,-16383},2);
            if(!be && bits>=24) check("integer-wav"+std::to_string(bits),wave(bits,false,2,pcm),{16383,-16383},2);
        }
    }
    for(int bits : {0,4,12,20,40,64,65535}) {
        Bytes pcm(32,0x7f);
        check("reject-integer"+std::to_string(bits),wave(bits,false,1,pcm),{},1);
        check("reject-caf-integer"+std::to_string(bits),caf(bits,false,1,false,pcm),{},1);
    }
    for(int bits : {8,16,24,40,80}) {
        Bytes pcm(32,0x7f);
        check("reject-float"+std::to_string(bits),wave(bits,true,1,pcm),{},1);
    }
    check("truncated-float64",wave(64,true,1,Bytes(7,0)),{},1);
    std::printf("PCM decode: %d passed, %d failed\n",passed,failed);
    return failed?1:0;
}
