"""Draw completion must preserve ownership and commit only the native copy."""
from pathlib import Path
import shutil,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[2]
class DrawCheckpointTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('clang'),'C compiler unavailable')
    def test_native_draw_path_and_rejected_sources(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'draw.c';exe=Path(directory)/'draw'
            path.write_text(r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef unsigned int u32;typedef unsigned char u8;
static int fault,order;static u8 saved_home[2];static u32 saved_size=2;
static void fail(const char *s){fprintf(stderr,"%s\n",s);exit(2);}
static u32 field(void *p,int n){
 uintptr_t id=(uintptr_t)p;
 if(id==1 && n==0x74)return fault==1?0:0xffffffffu;
 if(id==1 && n==0)return 9;
 if(id==2 && n==0x10)return fault==3?3:0;
 if(id==2 && n==0x14)return 1;
 if(id==6 && n==0x10)return fault==6?0:0xffffffffu;
 if(id==6 && n==0x14)return fault==6?1:0;
 if(id==3 && n==0x30)return fault==4?0:4;
 if(id==3 && n==0x34)return fault==5?0:5;
 fail("unexpected native field");return 0;
}
#define FIELD(p,n) field(p,n)
#define BYTE(p,n) (fault==2?0:1)
static void *no_winner(void *p){if((uintptr_t)p!=1)fail("wrong battle");return 0;}
static void *live(void *mode,int index){if((uintptr_t)mode!=9||index<0||index>1)fail("wrong mode/index");return index?(void*)2:(void*)6;}
static void refresh(void *p){if((uintptr_t)p!=5||order++!=3)fail("wrong refresh order");}
static uintptr_t api(u32 address){switch(address){
 case 0x14089:case 0x140b5:return (uintptr_t)no_winner;
 case 0xd7bd9:return (uintptr_t)live;
 case 0x9b709:return (uintptr_t)refresh;
 case 0x1234:return (uintptr_t)puts;
 default:fail("unexpected API: must not award chest or change wallet/counters");return 0;
}}
#define API_puts 0x1234
#define CALL(address,result,args) ((result (*)args)api(address))
static void serialize_native(void *home,void *avatar,int commit){if(home||(uintptr_t)avatar!=2||commit||order++!=0)fail("wrong serialize");}
static void *decode_packet(int kind,int version,const u8 *body,u32 size){if(kind!=24101||version||body!=saved_home||size!=2||order++!=1)fail("wrong copy");return (void*)3;}
static void credit_arena_crowns(void *p){if((uintptr_t)p!=5||order++!=2)fail("crowns must use copy");}
static void capture_native(void *h,void *a){if((uintptr_t)h!=4||(uintptr_t)a!=5||order++!=4)fail("wrong commit");puts("committed");}
static void dispose(void *p){if((uintptr_t)p!=3||order++!=5)fail("wrong dispose");}
#include "arena_draw.inc"
int main(int argc,char **argv){if(argc>1)fault=atoi(argv[1]);persist_arena_draw((void*)1);return order==6?0:3;}
''')
            p=subprocess.run(['clang','-std=c11','-O2','-I'+str(ROOT/'tools/clash_guest'),str(path),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(p.returncode,0,p.stderr)
            p=subprocess.run([str(exe)],capture_output=True,text=True)
            self.assertEqual(p.returncode,0,p.stderr);self.assertEqual(p.stdout.count('committed'),1)
            for fault in range(1,7):
                p=subprocess.run([str(exe),str(fault)],capture_output=True,text=True)
                self.assertEqual(p.returncode,2,p.stderr);self.assertNotIn('committed',p.stdout)
if __name__=='__main__':unittest.main()
