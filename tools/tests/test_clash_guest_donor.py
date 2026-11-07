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
        donor_clan=7;donor_size=24;donor_offset=8;donor_epoch=100;donor_present=1;
        for(u32 i=0;i<donor_size;++i)donor_home[i]=(u8)(i*11);
        write_donor(stdout,0x12345678);return 0;
    }
    u32 tag=0;if(fread(&tag,1,4,stdin)!=4 || tag!=0x314e4447u)return 2;
    read_donor(stdin,0x12345678);
    if(fgetc(stdin)!=EOF)return 2;
    printf("%u %u %u %u\n",donor_clan,donor_size,donor_offset,donor_present);
}
''')
            compiled=subprocess.run(['clang','-std=c11','-O2','-I'+str(ROOT/'tools/clash_guest'),
                                     str(source),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(compiled.returncode,0,compiled.stderr)
            # Independent literal LE header: tag, version, clan, extent, offset,
            # epoch, human-body binding, NPC FNV, presence, header FNV.
            payload=bytes((i*11)&255 for i in range(24))
            prefix=struct.pack('<9I',0x314e4447,2,7,24,8,100,0x12345678,fnv(payload),1)
            valid=prefix+struct.pack('<I',fnv(prefix))+payload
            written=subprocess.run([str(exe)],capture_output=True)
            self.assertEqual((written.returncode,written.stdout),(0,valid))
            def run(data):return subprocess.run([str(exe),'read'],input=data,capture_output=True)
            self.assertEqual((run(valid).returncode,run(valid).stdout),(0,b'7 24 8 1\n'))
            legacy=struct.pack('<8I',0x314e4447,1,7,24,8,100,0x12345678,fnv(payload))
            legacy+=struct.pack('<I',fnv(legacy))+payload
            self.assertEqual((run(legacy).returncode,run(legacy).stdout),(0,b'7 24 8 1\n'))
            retired=bytearray(valid);struct.pack_into('<I',retired,32,0)
            struct.pack_into('<I',retired,36,fnv(retired[:36]))
            self.assertEqual((run(retired).returncode,run(retired).stdout),(0,b'7 24 8 0\n'))
            for length in range(len(legacy)):
                self.assertEqual(run(legacy[:length]).returncode,2)
            for offset in range(len(legacy)):
                damaged=bytearray(legacy);damaged[offset]^=1
                self.assertEqual(run(damaged).returncode,2)
            for length in range(len(valid)):
                with self.subTest(truncated=length):self.assertEqual(run(valid[:length]).returncode,2)
            for offset in range(len(valid)):
                damaged=bytearray(valid);damaged[offset]^=1
                with self.subTest(bitflip=offset):self.assertEqual(run(damaged).returncode,2)
            # Checksummed malformed metadata must fail independently of FNV.
            for word,value in [(1,0),(1,3),(2,0),(2,1000001),(3,15),(3,65537),
                               (4,24),(4,17),(4,0xffffffff),(5,0),(6,0x12345679),(8,2)]:
                damaged=bytearray(valid);struct.pack_into('<I',damaged,word*4,value)
                struct.pack_into('<I',damaged,36,fnv(damaged[:36]))
                with self.subTest(word=word,value=value):self.assertEqual(run(damaged).returncode,2)
            self.assertEqual(run(valid+b'\0').returncode,2)

    def test_one_local_leader_topology(self):
        with tempfile.TemporaryDirectory() as directory:
            directory=Path(directory);source=directory/'roles.c';exe=directory/'roles'
            source.write_text(r'''
#include <stdio.h>
#include "clan_roles.inc"
int main(void) {
    int a,b,c;while(scanf("%d%d%d",&a,&b,&c)==3)printf("%d\n",clan_roles_valid(a,b,c));
}
''')
            result=subprocess.run(['clang','-std=c11','-I'+str(ROOT/'tools/clash_guest'),str(source),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            # Exactly one actual local leader: the human, or the NPC after a
            # completed handoff with the human co-leader. Absent NPCs do not lead.
            vectors=[(2,1,1),(2,1,3),(2,1,4),(2,0,1),(4,1,2),
                     (2,1,2),(4,0,2),(4,1,1),(1,1,2),(0,0,0),(2,2,1)]
            result=subprocess.run([str(exe)],input='\n'.join(' '.join(map(str,v)) for v in vectors),capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual(result.stdout,'1\n'*5+'0\n'*6)


if __name__=='__main__':unittest.main()
