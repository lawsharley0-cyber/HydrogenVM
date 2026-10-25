"""Independent ByteStream boundary vectors for the guest sector writer."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class GuestSectorTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('clang'), 'C compiler unavailable')
    def test_signed_vints_and_capacity(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            source = directory/'test.c'
            source.write_text(r'''
#include <stdio.h>
#include <stdlib.h>
typedef unsigned int u32;
typedef unsigned char u8;
static u8 saved_home[1024];
static u32 saved_size,avatar_offset;
struct GuestTowerPatch {unsigned part,offset,width;int values[12];};
struct GuestSector {const u8 *parts[4];unsigned sizes[4];const struct GuestTowerPatch *patches;unsigned patchCount,lowBits;};
static const struct GuestSector npc_sectors[7]={0};
#define GUEST_MAX_TOWER_LEVEL 12
#define CALL(address,result,args) printf
static void fail(const char *why) {fprintf(stderr,"%s\n",why);exit(2);}
#include "reader.inc"
#include "sector.inc"
int main(int argc,char **argv) {
    u8 data[5];struct SectorWriter w={data,0,argc>1?0:sizeof data};
    const int values[]={-1,-63,-64,-65,0,63,64,8191,8192,2147483647,(-2147483647-1)};
    for(unsigned i=0;i<sizeof values/sizeof *values;++i) {
        w.size=0;sector_vint(&w,values[i]);
        for(u32 j=0;j<w.size;++j)printf("%02x",data[j]);puts("");
        struct GuestReader r={data,w.size,0,0,0};
        if(turn_vint(&r)!=values[i] || r.at!=w.size)return 1;
    }
    return 0;
}
''')
            exe=directory/'test'
            result=subprocess.run(['clang','-std=c11','-O2','-I'+str(ROOT/'tools/clash_guest'),
                                   str(source),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual(result.stdout.splitlines(),[
                '7f','41','c07f','ff7e','00','3f','8001','bf7f',
                '808001','bfffffff0f','c080808000'])
            result=subprocess.run([str(exe),'capacity'],capture_output=True,text=True)
            self.assertEqual(result.returncode,2)
            self.assertIn('exceeds bound',result.stderr)


if __name__=='__main__':unittest.main()
