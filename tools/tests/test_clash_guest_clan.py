"""Authored clan wire/storage fault vectors; no game assets or player data."""
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def fnv(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


@unittest.skipUnless(shutil.which('clang'), 'C compiler unavailable')
class GuestClanTests(unittest.TestCase):
    def compile(self, directory, name, source):
        path = directory / (name + '.c')
        path.write_text(source)
        exe = directory / name
        result = subprocess.run(['clang', '-std=c11', '-O2', '-I' + str(ROOT/'tools/clash_guest'),
                                 str(path), '-o', str(exe)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        return exe

    def test_original_clan_reference_classes_and_boundaries(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            exe = self.compile(directory, 'wire', r'''
#include <stdio.h>
#include <stdlib.h>
typedef unsigned int u32; typedef unsigned char u8;
static void fail(const char *why) {fprintf(stderr,"%s\n",why);exit(2);}
#include "reader.inc"
#include "clan_wire.inc"
int main(int argc,char **argv) {
    u8 data[1024];u32 size=(u32)fread(data,1,sizeof data,stdin);
    struct ClanFields f=clan_fields(data,size);
    printf("%u %u %d %d %d %d\n",f.name_size,f.desc_size,f.badge,f.type,f.score,f.region);
}
''')
            # Literal pinned ByteStream schema: name, description, badge class
            # 16, type 1, trophy requirement 0, region class 57 (not class 1).
            valid = b'\0\0\0\3Oak\0\0\0\0\x10\x22\x01\x00\x39\x00'
            def run(data):
                return subprocess.run([str(exe)], input=data, capture_output=True)
            result = run(valid)
            self.assertEqual((result.returncode, result.stdout), (0, b'3 0 34 1 0 0\n'))
            self.assertEqual(run(valid[:7]+b'\xff'*4+valid[11:]).returncode, 0)
            for data in [valid[:n] for n in range(len(valid))] + [
                valid+b'\0', valid[:4]+b'O\nK'+valid[7:],
                valid[:11]+b'\x0f'+valid[12:], valid[:-2]+b'\x01\0',
                b'\xff'*4+valid[4:], valid[:13]+b'\0'+valid[14:],
                valid[:7]+b'\0\0\x10\0'+valid[11:]]:
                with self.subTest(vector=data.hex()):
                    self.assertEqual(run(data).returncode, 2)

    def test_atomic_clan_extension_integrity_and_truncation(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            exe = self.compile(directory, 'storage', r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
typedef unsigned int u32; typedef unsigned char u8;
#define CALL(address,result,args) ((result (*)args)(address))
static u32 rd(void *p,u32 size,u32 count,void *f) {return (u32)fread(p,size,count,f);}
static u32 wr(const void *p,u32 size,u32 count,void *f) {return (u32)fwrite(p,size,count,f);}
static uintptr_t s_read=(uintptr_t)&rd,s_write=(uintptr_t)&wr;
static void fail(const char *why) {fprintf(stderr,"%s\n",why);exit(2);}
static u32 checksum(const u8 *p,u32 n) {u32 h=2166136261u;while(n--)h=(h^*p++)*16777619u;return h;}
#include "clan_state.inc"
int main(int argc,char **argv) {
    if(argc==1) {
        clan_serial=7;clan_status=1;clan_gold_before=5000;clan_gold_after=4000;
        clan_request_size=10;clan_command_size=16;
        for(u32 i=0;i<10;++i)clan_request[i]=(u8)i;
        for(u32 i=0;i<16;++i)clan_command[i]=(u8)(i+20);
        write_clan(stdout,0x12345678u);return 0;
    }
    u32 tag;if(fread(&tag,1,4,stdin)!=4 || tag!=0x314c4347u)return 2;
    read_clan(stdin,0x12345678u);
    printf("%u %u %d %d %u %u\n",clan_serial,clan_status,clan_gold_before,clan_gold_after,clan_request_size,clan_command_size);
}
''')
            request, command = bytes(range(10)), bytes(range(20, 36))
            header = [0x314c4347, 7, 1, 10, 16, 5000, 4000, 0x12345678, fnv(request)^fnv(command)]
            head = struct.pack('<9I', *header)
            expected = head+struct.pack('<I', fnv(head))+request+command
            result = subprocess.run([str(exe)], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, expected)
            def run(data):
                return subprocess.run([str(exe), 'read'], input=data, capture_output=True)
            result = run(expected)
            self.assertEqual((result.returncode, result.stdout), (0, b'7 1 5000 4000 10 16\n'))
            for n in range(len(expected)):
                self.assertEqual(run(expected[:n]).returncode, 2)
            for at in range(len(expected)):
                broken = bytearray(expected); broken[at] ^= 1
                self.assertEqual(run(broken).returncode, 2)
            # Recomputed checksums cannot make impossible lengths/state/wallet
            # valid. A disbanded clan retains its monotonic ID, no payload.
            for index, value in [(1,0),(2,5),(3,1025),(4,257),(5,2000000001),(6,5001)]:
                broken = header.copy();broken[index]=value
                head = struct.pack('<9I', *broken)
                self.assertEqual(run(head+struct.pack('<I',fnv(head))+request+command).returncode, 2)

    def test_native_chat_identity_age_and_payload_bounds(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            exe = self.compile(directory, 'chat', r'''
#include <stdio.h>
#include <stdlib.h>
typedef unsigned int u32;typedef unsigned char u8;
static u32 clan_chat_sequence=1;
static void fail(const char *why) {fprintf(stderr,"%s\n",why);exit(2);}
#include "reader.inc"
struct SectorWriter {u8 *data;u32 size,capacity;};
static void sector_byte(struct SectorWriter *w,u8 b) {if(w->size>=w->capacity)fail("capacity");w->data[w->size++]=b;}
static void sector_bytes(struct SectorWriter *w,const u8 *p,u32 n) {while(n--)sector_byte(w,*p++);}
static void sector_vint(struct SectorWriter *w,int value) {
    /* This test only writes nonnegative ages: an independent width boundary
       encoder, not the guest's signed general-purpose sector serializer. */
    u32 v=(u32)value;u8 first=v&63;v>>=6;sector_byte(w,first|(v?128:0));
    while(v){u8 b=v&127;v>>=7;sector_byte(w,b|(v?128:0));}
}
#include "clan_chat_wire.inc"
int main(int argc,char **argv) {
    u8 data[1024],out[1030];u32 size=(u32)fread(data,1,sizeof data,stdin);
    struct SectorWriter w={out,0,sizeof out};
    clan_chat_entry(&w,data,size,100,argc>1?(u32)atoi(argv[1]):225);
    fwrite(out,1,w.size,stdout);
}
''')
            prefix = b'\x02\x00\x01\x00\x01\x00\x01'+b'\0\0\0\3Ada'+b'\x03\x02'
            suffix = b'\0'+b'\0\0\0\2Hi'  # not removed, actual text
            original = prefix+b'\0'+suffix
            def run(data, now='225'):
                return subprocess.run([str(exe), now], input=data, capture_output=True)
            for now, age in [('225', b'\xbd\x01'), ('100', b'\0'), ('99', b'\0'), ('163', b'\x3f'), ('164', b'\x80\x01')]:
                result = run(original, now)
                self.assertEqual((result.returncode, result.stdout), (0, prefix+age+suffix))
            bad_vectors = [original[:n] for n in range(len(original))]
            bad_vectors += [original+b'\0', b'\x01'+original[1:], original[:2]+b'\x02'+original[3:],
                            original[:4]+b'\x02'+original[5:], prefix+b'\x01'+suffix,
                            prefix+b'\0\x01'+suffix[1:], prefix+b'\0\0\0\0\x02\x01Hi']
            for data in bad_vectors:
                with self.subTest(vector=data.hex()):
                    self.assertEqual(run(data).returncode, 2)


if __name__ == '__main__':
    unittest.main()
