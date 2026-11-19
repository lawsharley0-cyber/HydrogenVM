"""Independent UTC calendar boundaries and strict training exclusion."""
from pathlib import Path
import shutil,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[2]

class GuestShopTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('clang'),'C compiler unavailable')
    def test_native_banner_ownership_and_refresh_order(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'view.c';exe=Path(directory)/'view'
            path.write_text(r'''
#include <stdint.h>
#include <stdlib.h>
typedef uintptr_t u32;
#define FIELD(p,n) (*(u32*)((unsigned char*)(p)+(n)))
static unsigned char page[512];static u32 banner[2],table[2];static int screen,exists,destroyed,refreshed;
static void *mode(void){return page;}
static void *home(void *p){return screen?page:0;}
static void *shop(void *p){return exists?page:0;}
static void destroy(void *p){if(p!=banner || FIELD(page,0xd4))exit(3);++destroyed;}
static void refresh(void *p){if(p!=page)exit(3);++refreshed;}
static uintptr_t resolve(uintptr_t a){switch(a){
case 0x91f7d:return (uintptr_t)mode;case 0x93b6d:return (uintptr_t)home;
case 0xe6cf5:return (uintptr_t)shop;case 0x8693d:return (uintptr_t)refresh;
default:return a;}}
#define CALL(a,result,args) ((result (*)args)resolve(a))
#include "shop_view.inc"
int main(void){
 table[1]=(uintptr_t)destroy;banner[0]=(uintptr_t)table;FIELD(page,0xd4)=(uintptr_t)banner;
 shop_refresh_view(2);if(refreshed||destroyed)return 3;
 screen=1;shop_refresh_view(2);if(refreshed||destroyed)return 3;
 exists=1;shop_refresh_view(2);if(refreshed!=1||destroyed!=1||FIELD(page,0xd4))return 3;
 shop_refresh_view(2);if(refreshed!=2||destroyed!=1)return 3;
 FIELD(page,0xd4)=(uintptr_t)banner;shop_refresh_view(1);
 if(refreshed!=3||destroyed!=1||FIELD(page,0xd4)!=(uintptr_t)banner)return 3;
}
''')
            p=subprocess.run(['clang','-std=c11','-O2','-I'+str(ROOT/'tools/clash_guest'),str(path),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(p.returncode,0,p.stderr)
            p=subprocess.run([str(exe)],capture_output=True,text=True)
            self.assertEqual(p.returncode,0,p.stderr)

    @unittest.skipUnless(shutil.which('clang'),'C compiler unavailable')
    def test_original_card_purchase_wire(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'wire.c';exe=Path(directory)/'wire'
            path.write_text(r'''
#include <stdio.h>
#include <stdlib.h>
typedef unsigned int u32;typedef unsigned char u8;
static void fail(const char *why){fprintf(stderr,"%s\n",why);exit(2);}
#include "reader.inc"
#include "shop_wire.inc"
int main(void){u8 bytes[32];u32 size=fread(bytes,1,sizeof bytes,stdin);
 struct GuestReader r={bytes,size,0,0,0};int card=shop_card_echo(&r);
 if(r.at!=size)return 2;printf("%d\n",card);}
''')
            p=subprocess.run(['clang','-std=c11','-O2','-I'+str(ROOT/'tools/clash_guest'),str(path),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(p.returncode,0,p.stderr)
            good=bytes.fromhex('000000011a01')
            def run(raw):return subprocess.run([str(exe)],input=raw,capture_output=True)
            p=run(good);self.assertEqual((p.returncode,p.stdout),(0,b'26000001\n'))
            for n in range(len(good)):self.assertEqual(run(good[:n]).returncode,2)
            for raw in [bytes.fromhex('011a01'),bytes.fromhex('000000001a01'),bytes.fromhex('000000011901'),good+b'\0']:
                self.assertEqual(run(raw).returncode,2)

    @unittest.skipUnless(shutil.which('clang'),'C compiler unavailable')
    def test_calendar_and_generation(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'test.c';exe=Path(directory)/'test'
            path.write_text(r'''
#include <stdio.h>
#include <stdlib.h>
typedef unsigned int u32;
#include "shop_math.inc"
int main(int argc,char **argv){
 if(argc!=3)return 2;u32 now=strtoul(argv[1],0,10),seed=strtoul(argv[2],0,10);
 printf("%u %u %u %d\n",shop_day(now),shop_weekday(shop_day(now)),shop_seconds_left(now),shop_should_refresh(seed,now));
 for(int wins=0;wins<7;++wins)for(int arena=54000000;arena<=54000020;++arena)if(shop_eligible(wins,arena))return 3;
 if(shop_eligible(7,54000000)||shop_eligible(7,0)||!shop_eligible(7,54000001))return 3;
 if(!shop_fields_valid(0xffffffffu,-1,0)||shop_fields_valid(0xffffffffu,-1,3)||shop_fields_valid(12,1,3))return 3;
 if(!shop_fields_valid(1000000003u,1,6)||shop_fields_valid(1000000003u,1,3))return 3;
 if(!shop_fields_valid(1000000004u,2,3)||shop_fields_valid(1000000004u,3,3))return 3;
}
''')
            p=subprocess.run(['clang','-std=c11','-O2','-I'+str(ROOT/'tools/clash_guest'),str(path),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(p.returncode,0,p.stderr)
            for now in [1,86399,86400,345599,345600,1792886400,1792972799,1792972800,4294967295]:
                day=now//86400
                for seed in [0,4294967295,123,1000000000+day,1000000000+max(0,day-1),1000000000+min(49710,day+1)]:
                    expected=1 if seed in [0,4294967295] else -1 if not 1000000000<=seed<=1000049710 else int(seed<1000000000+day)
                    p=subprocess.run([str(exe),str(now),str(seed)],capture_output=True,text=True)
                    self.assertEqual(p.returncode,0,p.stderr)
                    self.assertEqual(list(map(int,p.stdout.split())),[day,(day+4)%7+1,86400-now%86400,expected])

if __name__=='__main__':unittest.main()
