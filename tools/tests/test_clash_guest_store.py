"""Native chest/pack echo schemas and durable reward-source identities."""
from pathlib import Path
import shutil,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[2]

class GuestStoreTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('clang'),'C compiler unavailable')
    def test_original_chest_pack_wire_and_reward_sources(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'store.c';exe=Path(directory)/'store'
            path.write_text(r'''
#include <stdio.h>
#include <stdlib.h>
typedef unsigned int u32;typedef unsigned char u8;
static void fail(const char *why){fprintf(stderr,"%s\n",why);exit(2);}
#include "reader.inc"
#include "shop_wire.inc"
#include "reward_sources.inc"
int main(int argc,char **argv){
 if(argc==1){
  for(u32 kind=0;kind<=5;++kind)for(u32 id=0;id<=1;++id)
   if(reward_source_valid(kind,id)!=(kind>=1&&kind<=4&&(kind==1||!id)))return 3;
  if(!reward_source_valid(1,1000000)||reward_source_valid(1,1000001)||reward_source_valid(4,1)||reward_source_valid(0xffffffffu,0))return 3;
  if(reward_home_offset(2)!=0x78||reward_home_offset(3)!=0x90||reward_home_offset(4)!=0x7c||reward_home_offset(1)||reward_home_offset(5))return 3;
  return 0;
 }
 u8 data[64];u32 n=fread(data,1,sizeof data,stdin);struct GuestReader r={data,n,0,0,0};
 int value=argv[1][0]=='c'?shop_chest_echo(&r):shop_pack_echo(&r);
 if(r.at!=n)return 2;printf("%d\n",value);
}
''')
            p=subprocess.run(['clang','-std=c11','-O2','-I'+str(ROOT/'tools/clash_guest'),str(path),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(p.returncode,0,p.stderr)
            self.assertEqual(subprocess.run([str(exe)],capture_output=True).returncode,0)
            for mode,raw,value in [('chest',bytes.fromhex('000000011309'),19000009),('pack',bytes.fromhex('3700'),55000000)]:
                def run(data):return subprocess.run([str(exe),mode],input=data,capture_output=True)
                p=run(raw);self.assertEqual((p.returncode,p.stdout),(0,(str(value)+'\n').encode()))
                for n in range(len(raw)):self.assertEqual(run(raw[:n]).returncode,2)
                self.assertEqual(run(raw+b'\0').returncode,2)
            for mode,raw in [('chest','011309'),('chest','000000001309'),('chest','000000011409'),('pack','000000013700'),('pack','3600')]:
                self.assertEqual(subprocess.run([str(exe),mode],input=bytes.fromhex(raw),capture_output=True).returncode,2)

if __name__=='__main__':unittest.main()
