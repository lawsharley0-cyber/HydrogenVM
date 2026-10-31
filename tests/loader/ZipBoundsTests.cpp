// Synthetic ZIP metadata: no app binaries or commercial assets.
#include "loader/Zip.hpp"
#include <zlib.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <string>
#include <vector>
using Bytes = std::vector<uint8_t>;
namespace fs = std::filesystem;
using Hydrogen::Loader::ZipReader;
using Hydrogen::Loader::ZipError;
void num(Bytes& b, uint64_t v, int n) { for (int i=0;i<n;++i) b.push_back(uint8_t(v>>(8*i))); }
void set(Bytes& b, size_t off, uint64_t v, int n) { for(int i=0;i<n;++i)b[off+i]=uint8_t(v>>(8*i)); }
void append(Bytes& b, const Bytes& a) { b.insert(b.end(),a.begin(),a.end()); }
const Bytes payload = {'s','y','n','t','h','e','t','i','c'};
Bytes compressed() {
    z_stream zs{}; if(deflateInit2(&zs,6,Z_DEFLATED,-MAX_WBITS,8,Z_DEFAULT_STRATEGY)!=Z_OK)throw std::runtime_error("deflateInit2");
    Bytes out(100);zs.next_in=const_cast<uint8_t*>(payload.data());zs.avail_in=payload.size();zs.next_out=out.data();zs.avail_out=out.size();
    const int result=deflate(&zs,Z_FINISH);const size_t n=zs.total_out;deflateEnd(&zs);
    if(result!=Z_STREAM_END)throw std::runtime_error("deflate");out.resize(n);return out;
}
Bytes zip64(unsigned mask, size_t compressedSize) {
    Bytes body;
    if(mask&1)num(body,payload.size(),8);
    if(mask&2)num(body,compressedSize,8);
    if(mask&4)num(body,0,8);
    Bytes field;num(field,1,2);num(field,body.size(),2);append(field,body);return field;
}
Bytes archive(unsigned mask, const Bytes& extra, bool deflated=false, bool secondEntry=false) {
    const std::string name="Payload/Synthetic.app/resource";
    const Bytes comp=deflated?compressed():payload;
    const uint32_t crc=crc32(0,payload.data(),payload.size());
    Bytes b(30);set(b,0,0x04034b50,4);set(b,4,20,2);set(b,8,deflated?8:0,2);set(b,14,crc,4);
    set(b,18,comp.size(),4);set(b,22,payload.size(),4);set(b,26,name.size(),2);
    b.insert(b.end(),name.begin(),name.end());append(b,comp);
    const size_t cdOffset=b.size();Bytes cd(46);set(cd,0,0x02014b50,4);set(cd,6,45,2);set(cd,10,deflated?8:0,2);
    set(cd,16,crc,4);set(cd,20,(mask&2)?0xffffffff:comp.size(),4);set(cd,24,(mask&1)?0xffffffff:payload.size(),4);
    set(cd,28,name.size(),2);set(cd,30,extra.size(),2);set(cd,42,(mask&4)?0xffffffff:0,4);
    cd.insert(cd.end(),name.begin(),name.end());append(cd,extra);
    if(secondEntry) { // The first entry must never borrow bytes from its neighbor.
        Bytes next(46);set(next,0,0x02014b50,4);append(cd,next);
    }
    append(b,cd);Bytes end(22);set(end,0,0x06054b50,4);set(end,8,secondEntry?2:1,2);set(end,10,secondEntry?2:1,2);
    set(end,12,cd.size(),4);set(end,16,cdOffset,4);append(b,end);return b;
}
int main(int argc,char** argv) {
    const fs::path root=argc>1?argv[1]:"build/zip-test-fixtures";fs::create_directories(root);
    const std::string filter=argc>2?argv[2]:"";int passed=0,failed=0;
    auto check=[&](const std::string& name,const Bytes& bytes,bool valid) {
        if(!filter.empty() && name.find(filter)==std::string::npos)return;
        const fs::path path=root/(name+".zip");
        {std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());}
        bool ok=false;
        try {
            ZipReader z(path.string());
            if(valid) { const auto& e=z.entries().at(0);ok=e.size==payload.size() && e.localHeaderOffset==0 && z.read(e)==payload; }
        } catch(const ZipError&) {ok=!valid;}
        catch(const std::exception& e){std::printf("unexpected %s: %s\n",name.c_str(),e.what());}
        fs::remove(path);std::printf("%s %s\n",ok?"PASS":"FAIL",name.c_str());ok?++passed:++failed;
    };
    check("stored",archive(0,{}),true);check("deflated",archive(0,{},true),true);
    for(unsigned mask=1;mask<8;++mask) {
        check("zip64-stored-mask"+std::to_string(mask),archive(mask,zip64(mask,payload.size())),true);
        check("zip64-deflated-mask"+std::to_string(mask),archive(mask,zip64(mask,compressed().size()),true),true);
        check("missing-zip64-mask"+std::to_string(mask),archive(mask,{}),false);
    }
    Bytes unknown;num(unknown,0xcafe,2);num(unknown,3,2);unknown.insert(unknown.end(),{9,8,7});
    Bytes mixed=unknown;append(mixed,zip64(7,payload.size()));check("unknown-before-zip64",archive(7,mixed),true);
    mixed=zip64(7,payload.size());append(mixed,unknown);check("unknown-after-zip64",archive(7,mixed),true);
    check("zero-length-unneeded-zip64",archive(0,Bytes{1,0,0,0}),true);
    for(size_t n=0;n<24;++n) {
        Bytes field;num(field,1,2);num(field,n,2);field.resize(n+4,0);
        check("short-zip64-"+std::to_string(n),archive(7,field),false);
    }
    for(size_t n=1;n<4;++n)check("truncated-field-header-"+std::to_string(n),archive(0,Bytes(n,0)),false);
    Bytes oversized{1,0,8,0};
    check("field-overrun-at-directory-end",archive(1,oversized),false);
    check("field-overrun-into-next-entry",archive(1,oversized,false,true),false);
    check("unknown-field-overrun",archive(0,Bytes{0xfe,0xca,0xff,0xff}),false);
    check("field-borrows-next-field",archive(7,Bytes{1,0,0,0,0xfe,0xca,24,0,0,0,0,0}),false);
#ifdef __linux__
    if (filter.empty()) {
        auto descriptors=[]() {
            size_t count=0;for(const auto& e:fs::directory_iterator("/proc/self/fd")){(void)e;++count;}return count;
        };
        const auto path=root/"repeat-malformed.zip";
        const Bytes bytes(22,0);   // missing EOCD: constructor must throw on every build
        {std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());}
        const size_t before=descriptors();
        for(int i=0;i<64;++i) {try {ZipReader z(path.string());}catch(const ZipError&) {}}
        const bool ok=descriptors()==before;fs::remove(path);
        std::printf("%s malformed-constructor-closes-file\n",ok?"PASS":"FAIL");ok?++passed:++failed;
    }
#endif
    std::printf("ZIP metadata: %d passed, %d failed\n",passed,failed);return failed?1:0;
}
