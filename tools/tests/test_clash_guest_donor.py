"""Authored local-donor checkpoint corruption vectors, without game data."""
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]


def fnv(data):
    value=2166136261
    for byte in data:value=((value^byte)*16777619)&0xffffffff
    return value


@unittest.skipUnless(shutil.which('clang'),'C compiler unavailable')
class GuestDonorTests(unittest.TestCase):
    def test_atomic_donor_extension_faults(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory=Path(temporary);source=directory/'donor.c';exe=directory/'donor'
            source.write_text(r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
typedef unsigned int u32; typedef unsigned char u8;
#define CALL(address,result,args) ((result (*)args)(address))
static uintptr_t s_read=(uintptr_t)&fread,s_write=(uintptr_t)&fwrite;
static void fail(const char *why) {fprintf(stderr,"%s\n",why);exit(2);}
static u32 checksum(const u8 *p,u32 n) {u32 h=2166136261;while(n--)h=(h^*p++)*16777619;return h;}
#include "donor_state.inc"
int main(int argc,char **argv) {
    if(argc==1) {
        donor_clan=7;donor_size=24;donor_offset=8;donor_epoch=100;
        for(u32 i=0;i<donor_size;++i)donor_home[i]=(u8)(i*11);
        write_donor(stdout,0x12345678);return 0;
    }
    u32 tag=0;if(fread(&tag,1,4,stdin)!=4 || tag!=0x314e4447u)return 2;
    read_donor(stdin,0x12345678);
    if(fgetc(stdin)!=EOF)return 2;
    printf("%u %u %u\n",donor_clan,donor_size,donor_offset);
}
''')
            compiled=subprocess.run(['clang','-std=c11','-O2','-I'+str(ROOT/'tools/clash_guest'),
                                     str(source),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(compiled.returncode,0,compiled.stderr)
            # Independent literal LE header: tag, version, clan, extent, offset,
            # epoch, human-body binding, NPC FNV, header FNV. Authored payload.
            payload=bytes((i*11)&255 for i in range(24))
            prefix=struct.pack('<8I',0x314e4447,1,7,24,8,100,0x12345678,fnv(payload))
            valid=prefix+struct.pack('<I',fnv(prefix))+payload
            written=subprocess.run([str(exe)],capture_output=True)
            self.assertEqual((written.returncode,written.stdout),(0,valid))
            def run(data):return subprocess.run([str(exe),'read'],input=data,capture_output=True)
            self.assertEqual((run(valid).returncode,run(valid).stdout),(0,b'7 24 8\n'))
            for length in range(len(valid)):
                with self.subTest(truncated=length):self.assertEqual(run(valid[:length]).returncode,2)
            for offset in range(len(valid)):
                damaged=bytearray(valid);damaged[offset]^=1
                with self.subTest(bitflip=offset):self.assertEqual(run(damaged).returncode,2)
            # Checksummed malformed metadata must fail independently of FNV.
            for word,value in [(1,0),(1,2),(2,0),(2,1000001),(3,15),(3,65537),
                               (4,24),(4,17),(4,0xffffffff),(5,0),(6,0x12345679)]:
                damaged=bytearray(valid);struct.pack_into('<I',damaged,word*4,value)
                struct.pack_into('<I',damaged,32,fnv(damaged[:32]))
                with self.subTest(word=word,value=value):self.assertEqual(run(damaged).returncode,2)
            self.assertEqual(run(valid+b'\0').returncode,2)


if __name__=='__main__':unittest.main()
