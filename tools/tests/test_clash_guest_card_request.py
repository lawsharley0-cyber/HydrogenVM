"""Authored pinned DonateStreamEntry and atomic request boundary vectors."""
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def fnv(data):
    result = 2166136261
    for byte in data:
        result = ((result ^ byte) * 16777619) & 0xffffffff
    return result


@unittest.skipUnless(shutil.which('clang'), 'C compiler unavailable')
class GuestCardRequestTests(unittest.TestCase):
    def compile(self, directory, name, source):
        path = directory/(name+'.c')
        path.write_text(source)
        exe = directory/name
        result = subprocess.run(['clang', '-std=c11', '-O2', '-I'+str(ROOT/'tools/clash_guest'),
                                 str(path), '-o', str(exe)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        return exe

    def test_native_request_entry_without_invented_donors(self):
        with tempfile.TemporaryDirectory() as temporary:
            exe = self.compile(Path(temporary), 'wire', r'''
#include <stdio.h>
#include <stdlib.h>
typedef unsigned int u32;typedef unsigned char u8;
static u32 card_request_sequence=1,clan_chat_sequence=1;
static void fail(const char *why) {fprintf(stderr,"%s\n",why);exit(2);}
#include "reader.inc"
struct SectorWriter {u8 *data;u32 size,capacity;};
static void sector_byte(struct SectorWriter *w,u8 b) {if(w->size>=w->capacity)fail("capacity");w->data[w->size++]=b;}
static void sector_bytes(struct SectorWriter *w,const u8 *p,u32 n) {while(n--)sector_byte(w,*p++);}
static void sector_vint(struct SectorWriter *w,int value) {
    u32 v=(u32)value;u8 first=v&63;v>>=6;sector_byte(w,first|(v?128:0));
    while(v){u8 b=v&127;v>>=7;sector_byte(w,b|(v?128:0));}
}
#include "clan_chat_wire.inc"
#include "card_request_wire.inc"
int main(int argc,char **argv) {
    u8 data[4096],out[4100];u32 size=(u32)fread(data,1,sizeof data,stdin);
    struct SectorWriter w={out,0,sizeof out};
    if(argc>2)npc_request_entry(&w,data,size,100,(u32)atoi(argv[1]),1);
    else card_request_entry(&w,data,size,100,argc>1?(u32)atoi(argv[1]):225);
    fwrite(out,1,w.size,stdout);
}
''')
            # Native 0x11051c uses fixed BE capacity/donor count, then the
            # spell-list helper's VInt count/references, then optional text.
            string = lambda value: struct.pack('>I', len(value))+value
            prefix = b'\x01\x01\x01\0\x01\0\x01'+string(b'Ada')+b'\x03\x02'
            suffix = b'\0'+struct.pack('>II', 10, 0)+b'\x01\x1a\0\x01'+string(b'Help')
            original = prefix+b'\0'+suffix
            def run(data, now='225'):
                return subprocess.run([str(exe), now], input=data, capture_output=True)
            for now, age in [('225', b'\xbd\x01'), ('100', b'\0'), ('99', b'\0')]:
                result = run(original, now)
                self.assertEqual((result.returncode,result.stdout), (0,prefix+age+suffix))
            co=prefix[:-1]+b'\x04'
            self.assertEqual((run(co+b'\0'+suffix).returncode,run(co+b'\0'+suffix).stdout),
                             (0,co+b'\xbd\x01'+suffix))
            for role in [0,5]:self.assertEqual(run(prefix[:-1]+bytes([role])+b'\0'+suffix).returncode,2)
            # Literal original DonationContainer: fixed BE donor (3,1),
            # one fixed BE card count, matching class-26/index-0 reference.
            donor=struct.pack('>III',3,1,1)+b'\x1a\0'
            donated_suffix=b'\0'+struct.pack('>II',10,1)+donor+suffix[9:]
            donated=prefix+b'\0'+donated_suffix
            result=run(donated)
            self.assertEqual((result.returncode,result.stdout),(0,prefix+b'\xbd\x01'+donated_suffix))
            for word,value in [(0,0),(0,4),(1,2),(2,0),(2,2)]:
                bad=bytearray(donor);struct.pack_into('>I',bad,word*4,value)
                vectorsuffix=b'\0'+struct.pack('>II',10,1)+bad+suffix[9:]
                self.assertEqual(run(prefix+b'\0'+vectorsuffix).returncode,2)
            mismatched=donated.replace(donor,donor[:-1]+b'\x01',1)
            self.assertEqual(run(mismatched).returncode,2)
            for length in range(len(donated)):
                self.assertEqual(run(donated[:length]).returncode,2)
            vectors = [original[:n] for n in range(len(original))]
            vectors += [original+b'\0', b'\x02'+original[1:], original[:2]+b'\x02'+original[3:],
                        prefix+b'\x01'+suffix, prefix+b'\0\x01'+suffix[1:],
                        prefix+b'\0\0'+struct.pack('>II',10,1)+suffix[9:],
                        prefix+b'\0\0'+struct.pack('>II',0,0)+suffix[9:]]
            for data in vectors:
                with self.subTest(vector=data.hex()):
                    self.assertEqual(run(data).returncode, 2)
            # Separate NPC namespace3/author(3,1), human donor(0,1).
            # Parsing multiple donated cards does not grant native eligibility;
            # production calls original canAddDonation for the actual arena.
            npc_prefix=b'\x01\x03\x01\x03\x01\x03\x01'+string(b'Local Donor')+b'\x03\x01\0'
            def npc(data):return subprocess.run([str(exe),'100','npc'],input=data,capture_output=True)
            for count in [0,1,2,10]:
                donated=b'' if not count else struct.pack('>III',0,1,count)+b'\x1a\0'*count
                payload=npc_prefix+b'\0'+struct.pack('>II',10,int(count>0))+donated+b'\x01\x1a\0\0'
                self.assertEqual((npc(payload).returncode,npc(payload).stdout),(0,payload))
                self.assertEqual(run(payload).returncode,2)
            for namespace,author in [(1,3),(3,0),(2,3)]:
                bad=bytearray(payload);bad[1]=namespace;bad[3]=author
                self.assertEqual(npc(bad).returncode,2)
            over=npc_prefix+b'\0'+struct.pack('>IIIII',10,1,0,1,11)+b'\x1a\0'*11+b'\x01\x1a\0\0'
            self.assertEqual(npc(over).returncode,2)

    def test_npc_request_atomic_binding_and_retirement(self):
        with tempfile.TemporaryDirectory() as temporary:
            exe=self.compile(Path(temporary),'npc_state',r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
typedef unsigned int u32;typedef unsigned char u8;
#define CALL(address,result,args) ((result (*)args)(address))
static uintptr_t s_read=(uintptr_t)&fread,s_write=(uintptr_t)&fwrite;
static u8 donor_home[]={1,2,3};static u32 donor_size=3;
static void fail(const char *why) {fprintf(stderr,"%s\n",why);exit(2);}
static u32 checksum(const u8 *p,u32 n) {u32 h=2166136261u;while(n--)h=(h^*p++)*16777619u;return h;}
#include "npc_request_state.inc"
int main(void) {
    u32 tag;if(fread(&tag,1,4,stdin)!=4 || tag!=0x31524e47u)return 2;
    read_npc_request(stdin,0x12345678u);
    if(!npc_request_loaded || npc_request_binding!=checksum(donor_home,donor_size) || fgetc(stdin)!=EOF)return 2;
    write_npc_request(stdout,0x12345678u);
}
''')
            def pack(payload,**changes):
                h=[0x31524e47,1,len(payload),7,100 if payload else 0,0x12345678,fnv(b'\1\2\3'),fnv(payload)]
                for word,value in changes.items():h[int(word)]=value
                raw=struct.pack('<8I',*h);return raw+struct.pack('<I',fnv(raw))+payload
            def run(raw):return subprocess.run([str(exe)],input=raw,capture_output=True)
            for payload in [b'',bytes(range(40))]:
                raw=pack(payload);result=run(raw)
                self.assertEqual((result.returncode,result.stdout),(0,raw))
                for end in [0,4,16,32,35,len(raw)-1]:self.assertEqual(run(raw[:end]).returncode,2)
                for word,value in [(1,2),(2,2049),(3,0),(3,1000000001),(5,0),(6,0),(7,0)]:
                    self.assertEqual(run(pack(payload,**{str(word):value})).returncode,2)
                wrong_stamp=1 if not payload else 0
                self.assertEqual(run(pack(payload,**{'4':wrong_stamp})).returncode,2)
                self.assertEqual(run(raw+b'\0').returncode,2)

    def test_atomic_request_extension_faults_and_disbanded_state(self):
        with tempfile.TemporaryDirectory() as temporary:
            exe = self.compile(Path(temporary), 'state', r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
typedef unsigned int u32;typedef unsigned char u8;
#define CALL(address,result,args) ((result (*)args)(address))
static u32 rd(void *p,u32 s,u32 n,void *f) {return (u32)fread(p,s,n,f);}
static u32 wr(const void *p,u32 s,u32 n,void *f) {return (u32)fwrite(p,s,n,f);}
static uintptr_t s_read=(uintptr_t)&rd,s_write=(uintptr_t)&wr;
static void fail(const char *why) {fprintf(stderr,"%s\n",why);exit(2);}
static u32 checksum(const u8 *p,u32 n) {u32 h=2166136261u;while(n--)h=(h^*p++)*16777619u;return h;}
#include "card_request_state.inc"
int main(void) {
    u32 tag;if(fread(&tag,1,4,stdin)!=4 || tag!=0x31524347u)return 2;
    read_card_request(stdin,0x12345678u);write_card_request(stdout,0x12345678u);
}
''')
            payload = bytes(range(40))
            header = [0x31524347, len(payload), 5, 100, 0x12345678, fnv(payload), 0xfedcba98]
            def encode(h, data):
                raw = struct.pack('<7I', *h)
                return raw+struct.pack('<I', fnv(raw))+data
            expected = encode(header, payload)
            def run(data):
                return subprocess.run([str(exe)], input=data, capture_output=True)
            result = run(expected)
            self.assertEqual((result.returncode,result.stdout), (0,expected))
            for n in range(len(expected)):
                self.assertEqual(run(expected[:n]).returncode, 2)
            for at in range(len(expected)):
                broken = bytearray(expected);broken[at] ^= 1
                self.assertEqual(run(broken).returncode, 2)
            for index, value in [(1,2049),(1,0),(2,0),(2,1000000001),(3,0),(4,0)]:
                broken = header.copy();broken[index] = value
                self.assertEqual(run(encode(broken,payload)).returncode, 2)
            cleared = header.copy();cleared[1]=0;cleared[3]=0;cleared[5]=fnv(b'')
            expected = encode(cleared,b'');result = run(expected)
            self.assertEqual((result.returncode,result.stdout), (0,expected))


if __name__ == '__main__':
    unittest.main()
