// Microsoft ADPCM is separate from IMA WAV. Use the audited single-file
// decoder on both Apple and Linux; no host-codec availability difference.
#include "Audio.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#define DR_WAV_NO_STDIO
#define DR_WAV_IMPLEMENTATION
#include "thirdparty/dr_wav/dr_wav.h"

namespace Hydrogen::Runtime::Audio {
bool decodeMsAdpcm(DecodedAudio& out,const uint8_t* data,size_t size) {
    if(!data || size<12 || size>256u*1024*1024)return false;
    auto le16=[](const uint8_t* p){return unsigned(p[0])|unsigned(p[1])<<8;};
    auto le32=[](const uint8_t* p){return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;};
    if(std::memcmp(data,"RIFF",4) || std::memcmp(data+8,"WAVE",4) || uint64_t(le32(data+4))+8>size)return false;
    bool coefficients=false;uint32_t factFrames=0;
    const std::array<int16_t,14> standard{256,0,512,-256,0,0,192,64,240,0,460,-208,392,-232};
    for(size_t at=12;at+8<=size;) {
        const auto bytes=le32(data+at+4);at+=8;if(bytes>size-at)return false;
        if(!std::memcmp(data+at-8,"fmt ",4)) {
            // dr_wav uses the standard predictor table. Explicitly reject a
            // different table rather than decoding it with the wrong predictors.
            if(bytes<50 || le16(data+at)!=2 || le16(data+at+16)<32 || le16(data+at+20)!=7)return false;
            for(size_t i=0;i<standard.size();++i)if(int16_t(le16(data+at+22+2*i))!=standard[i])return false;
            coefficients=true;
        }
        if(!std::memcmp(data+at-8,"fact",4)){if(bytes<4)return false;factFrames=le32(data+at);}
        at+=bytes+(bytes&1);
    }
    if(!coefficients)return false;
    drwav wav{};
    if(!drwav_init_memory(&wav,data,size,nullptr))return false;
    struct Close {drwav& wav;~Close(){drwav_uninit(&wav);}} close{wav};
    constexpr uint64_t maxFrames=32u*1024*1024;
    const uint64_t frameCount=factFrames?factFrames:wav.totalPCMFrameCount;
    if(wav.translatedFormatTag!=DR_WAVE_FORMAT_ADPCM || wav.channels<1 || wav.channels>2 ||
        wav.sampleRate<4000 || wav.sampleRate>192000 || !frameCount || frameCount>maxFrames || frameCount>wav.totalPCMFrameCount)return false;
    DecodedAudio decoded;decoded.channels=int(wav.channels);decoded.sampleRate=int(wav.sampleRate);
    decoded.stereoPcm.reserve(size_t(frameCount)*2);
    std::array<int16_t,8192> samples{};uint64_t frames=0;
    while(frames<frameCount) {
        const auto count=std::min<uint64_t>(4096,frameCount-frames);
        const auto got=drwav_read_pcm_frames_s16(&wav,count,samples.data());
        if(got!=count)return false;
        for(uint64_t i=0;i<got;++i) {
            decoded.stereoPcm.push_back(samples[i*wav.channels]);
            decoded.stereoPcm.push_back(samples[i*wav.channels+(wav.channels==2?1:0)]);
        }
        frames+=got;
    }
    decoded.duration=double(frames)/decoded.sampleRate;out=std::move(decoded);return true;
}
}
