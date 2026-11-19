"""Authored local-mail wire and atomic-extension fault vectors, no game data."""
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
class GuestMailTests(unittest.TestCase):
    def compile(self, directory, name, source):
        path = directory/(name+'.c');path.write_text(source)
        exe = directory/name
        result = subprocess.run(['clang', '-std=c11', '-O2', '-I'+str(ROOT/'tools/clash_guest'),
                                 str(path), '-o', str(exe)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        return exe

    def test_native_inbox_wire_age_and_identity(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            exe = self.compile(directory, 'wire', r'''
#include <stdio.h>
#include <stdlib.h>
typedef unsigned int u32;typedef unsigned char u8;
static u32 local_mail_sequence=1,clan_chat_sequence=1;
static void fail(const char *why) {fprintf(stderr,"%s\n",why);exit(2);}
#include "reader.inc"
struct SectorWriter {u8 *data;u32 size,capacity;};
static void sector_byte(struct SectorWriter *w,u8 b) {if(w->size>=w->capacity)fail("capacity");w->data[w->size++]=b;}
static void sector_bytes(struct SectorWriter *w,const u8 *p,u32 n) {while(n--)sector_byte(w,*p++);}
static void sector_vint(struct SectorWriter *w,int value) {
    u32 v=(u32)value;u8 first=v&63;v>>=6;sector_byte(w,first|(v?128:0));
    while(v){u8 b=v&127;v>>=7;sector_byte(w,b|(v?128:0));}
}
#include "clan_wire.inc"
#include "clan_chat_wire.inc"
#include "mail_wire.inc"
int main(int argc,char **argv) {
    u8 data[4096],out[4100];u32 size=(u32)fread(data,1,sizeof data,stdin);
    struct SectorWriter w={out,0,sizeof out};
    local_mail_entry(&w,data,size,100,argc>1?(u32)atoi(argv[1]):225);
    fwrite(out,1,w.size,stdout);
}
''')
            fixed = lambda high, low: struct.pack('>II', high, low)
            string = lambda value: struct.pack('>I', len(value))+value
            prefix = b'\x06'+fixed(0,1)+fixed(0,1)+string(b'Ada')+b'\0'
            suffix = b'\x02'+string(b'Hi')+b'\x01'+fixed(0,1)+string(b'Note')+fixed(1,7)+string(b'Oak')+b'\x10\x22'
            original = prefix+b'\0'+suffix
            def run(data, now='225'):
                return subprocess.run([str(exe), now], input=data, capture_output=True)
            for now, age in [('225',b'\xbd\x01'),('100',b'\0'),('99',b'\0'),('163',b'\x3f'),('164',b'\x80\x01')]:
                result = run(original,now)
                self.assertEqual((result.returncode,result.stdout),(0,prefix+age+suffix))
            vectors = [original[:n] for n in range(len(original))]
            vectors += [original+b'\0',b'\x07'+original[1:],prefix+b'\x01'+suffix,
                        prefix[:-1]+b'\x01\0'+suffix,
                        prefix+b'\0\0'+suffix[1:],prefix+b'\0'+suffix[:-2]+b'\x39\0',
                        original[:8]+b'\x02'+original[9:]]
            for data in vectors:
                with self.subTest(vector=data.hex()):
                    self.assertEqual(run(data).returncode,2)

    def test_mail_extension_corruption_and_empty_inbox(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            exe = self.compile(directory, 'storage', r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
typedef unsigned int u32;typedef unsigned char u8;
#define CALL(address,result,args) ((result (*)args)(address))
static u32 rd(void *p,u32 size,u32 count,void *f) {return (u32)fread(p,size,count,f);}
static u32 wr(const void *p,u32 size,u32 count,void *f) {return (u32)fwrite(p,size,count,f);}
static uintptr_t s_read=(uintptr_t)&rd,s_write=(uintptr_t)&wr;
static void fail(const char *why) {fprintf(stderr,"%s\n",why);exit(2);}
static u32 checksum(const u8 *p,u32 n) {u32 h=2166136261u;while(n--)h=(h^*p++)*16777619u;return h;}
#include "mail_state.inc"
int main(int argc,char **argv) {
    if(argc==1) {
        local_mail_size=20;local_mail_count=1;local_mail_sequence=5;local_mail_echo_crc=0xfedcba98u;
        for(u32 i=0;i<20;++i)local_mail[i]=(u8)i;
        write_local_mail(stdout,0x12345678u);return 0;
    }
    u32 tag;if(fread(&tag,1,4,stdin)!=4 || tag!=0x31414d47u)return 2;
    read_local_mail(stdin,0x12345678u);
    printf("%u %u %u %08x\n",local_mail_size,local_mail_count,local_mail_sequence,local_mail_echo_crc);
}
''')
            payload=bytes(range(20))
            header=[0x31414d47,20,1,5,0x12345678,fnv(payload),0xfedcba98]
            def encode(h,payload):
                head=struct.pack('<7I',*h)
                return head+struct.pack('<I',fnv(head))+payload
            expected=encode(header,payload)
            result=subprocess.run([str(exe)],capture_output=True)
            self.assertEqual((result.returncode,result.stdout),(0,expected))
            def run(data):
                return subprocess.run([str(exe),'read'],input=data,capture_output=True)
            result=run(expected)
            self.assertEqual((result.returncode,result.stdout),(0,b'20 1 5 fedcba98\n'))
            for n in range(len(expected)):
                self.assertEqual(run(expected[:n]).returncode,2)
            for at in range(len(expected)):
                broken=bytearray(expected);broken[at]^=1
                self.assertEqual(run(broken).returncode,2)
            for index,value in [(1,32769),(1,0),(2,0),(2,33),(2,6),(3,0),(3,1000000001),(4,0)]:
                broken=header.copy();broken[index]=value
                self.assertEqual(run(encode(broken,payload)).returncode,2)
            empty=header.copy();empty[1]=0;empty[2]=0;empty[5]=fnv(b'')
            result=run(encode(empty,b''))
            self.assertEqual((result.returncode,result.stdout),(0,b'0 0 5 fedcba98\n'))


if __name__ == '__main__':
    unittest.main()
