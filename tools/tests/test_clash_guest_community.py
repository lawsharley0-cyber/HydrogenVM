"""Authored local calendar, two-actor score and NPC profile wire vectors."""
from datetime import datetime, timedelta, timezone
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]

def fnv(data):
    result=2166136261
    for byte in data:result=((result^byte)*16777619)&0xffffffff
    return result

@unittest.skipUnless(shutil.which('clang'),'C compiler unavailable')
class GuestCommunityTests(unittest.TestCase):
    def compile(self,directory,source):
        path=Path(directory)/'test.c';exe=Path(directory)/'test';path.write_text(source)
        p=subprocess.run(['clang','-std=c11','-O2','-I'+str(ROOT/'tools/clash_guest'),str(path),'-o',str(exe)],capture_output=True,text=True)
        self.assertEqual(p.returncode,0,p.stderr);return exe

    def test_week_calendar_and_atomic_scores(self):
        with tempfile.TemporaryDirectory() as directory:
            exe=self.compile(directory,r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
typedef unsigned int u32;typedef unsigned char u8;
#define CALL(address,result,args) ((result (*)args)(address))
static uintptr_t s_read=(uintptr_t)&fread,s_write=(uintptr_t)&fwrite;
static u8 donor_home[]={1,2,3};static u32 donor_size=3;
static void fail(const char *why) {fprintf(stderr,"%s\n",why);exit(2);}
static u32 checksum(const u8 *p,u32 n) {u32 h=2166136261;while(n--)h=(h^*p++)*16777619;return h;}
#include "community_state.inc"
int main(int argc,char **argv) {
    if(argc>1) {u32 epoch;while(scanf("%u",&epoch)==1)printf("%u\n",community_week_start(epoch));return 0;}
    u32 tag;if(fread(&tag,1,4,stdin)!=4 || tag!=0x314d4347u)return 2;
    read_community(stdin,0x12345678);
    if(fgetc(stdin)!=EOF || !community_loaded || community_binding!=checksum(donor_home,donor_size))return 2;
    write_community(stdout,0x12345678);
}
''')
            epochs=[0,345599,345600,951782400,1792972799,1792972800,1793577599,1793577600,2147483647,4294967295]
            expected=[]
            for epoch in epochs:
                date=datetime.fromtimestamp(epoch,timezone.utc)
                monday=(date-timedelta(days=date.weekday())).replace(hour=0,minute=0,second=0,microsecond=0)
                expected.append(max(0,int(monday.timestamp())))
            p=subprocess.run([str(exe),'week'],input='\n'.join(map(str,epochs)),capture_output=True,text=True)
            self.assertEqual(p.returncode,0,p.stderr)
            self.assertEqual(list(map(int,p.stdout.split())),expected)
            payload=bytes(range(40))
            def pack(**changes):
                h=[0x314d4347,1,7,1792972800,11,1,1,11,1793075900,len(payload),0x12345678,fnv(b'\1\2\3'),fnv(payload),1]
                for key,value in changes.items():h[int(key)]=value
                raw=struct.pack('<14I',*h);return raw+struct.pack('<I',fnv(raw))+payload
            def run(data):return subprocess.run([str(exe)],input=data,capture_output=True)
            raw=pack();p=run(raw);self.assertEqual((p.returncode,p.stdout),(0,raw))
            for end in [0,4,16,40,56,59,60,99]:self.assertEqual(run(raw[:end]).returncode,2)
            for word,value in [(1,2),(2,0),(2,1000001),(3,0),(3,1792972801),(4,1000000001),
                               (5,0),(6,0),(7,0),(8,0),(9,0),(9,257),(10,0),(11,0),(12,0),(13,2)]:
                with self.subTest(word=word,value=value):self.assertEqual(run(pack(**{str(word):value})).returncode,2)
            damaged=bytearray(raw);damaged[-1]^=1
            self.assertEqual(run(damaged).returncode,2)
            self.assertEqual(run(raw+b'\0').returncode,2)

    def test_profile_entry_has_fixed_local_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            exe=self.compile(directory,r'''
#include <stdio.h>
#include <stdlib.h>
typedef unsigned int u32;typedef unsigned char u8;
static void fail(const char *why) {fprintf(stderr,"%s\n",why);exit(2);}
#include "reader.inc"
struct SectorWriter {u8 *data;u32 size,capacity;};
static void sector_bytes(struct SectorWriter *w,const u8 *p,u32 n) {if(n>w->capacity-w->size)fail("capacity");while(n--)w->data[w->size++]=*p++;}
static void sector_vint(struct SectorWriter *w,int v) {if(v!=0)fail("test age");u8 b=0;sector_bytes(w,&b,1);}
#include "community_wire.inc"
int main(void) {
    u8 raw[512],out[512];u32 size=(u32)fread(raw,1,sizeof raw,stdin);struct SectorWriter w={out,0,sizeof out};
    community_entry_wire(&w,raw,size,100,100);fwrite(out,1,w.size,stdout);
}
''')
            string=lambda s:struct.pack('>I',len(s))+s
            raw=b'\2\4\1\3\1\3\1'+string(b'Local Donor')+b'\3\1\0\0'+string(b'Offline clanmate. Tap my name to view or invite me.')
            def run(data):return subprocess.run([str(exe)],input=data,capture_output=True)
            p=run(raw);self.assertEqual((p.returncode,p.stdout),(0,raw))
            for offset,value in [(0,1),(1,0),(2,2),(3,0),(4,2),(5,0),(6,2),(22,13),(23,5),(24,1),(25,1),(30,ord('X'))]:
                damaged=bytearray(raw);damaged[offset]=value
                with self.subTest(offset=offset):self.assertEqual(run(damaged).returncode,2)
            for end in [0,7,20,len(raw)-1]:self.assertEqual(run(raw[:end]).returncode,2)

if __name__=='__main__':unittest.main()
