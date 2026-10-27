"""Authored opponent choice, persistence and arena unlock invariants."""
from pathlib import Path
import csv
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
from clash_bot_policy import DECKS,load_policy

def fnv(raw):
    n=2166136261
    for b in raw:n=((n^b)*16777619)&0xffffffff
    return n

class GuestOpponentPolicyTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('clang'),'C compiler unavailable')
    def test_native_name_identity_and_training_fallback(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'test.c';exe=Path(directory)/'test'
            path.write_text(r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
typedef unsigned int u32;typedef unsigned char u8;
static u32 field(void *p,u32 n) {
 if((uintptr_t)p==0x464c98)return 11;
 if((uintptr_t)p==0x464c9c)return 12;
 if((uintptr_t)p==0x464ca0)return 13;
 return *(u32*)((u8*)p+n);
}
#define FIELD(p,n) field((void*)(p),n)
static void fail(const char *why){fprintf(stderr,"%s\n",why);exit(2);}
#include "opponent_names.inc"
#include "opponent_math.inc"
static struct OpponentHistory opponent_history={1,1,0,0,0,1};static int opponent_enabled;
static const int arena1_missions[]={18000007},arena_mana_rates[]={28000,14000,14000};
static u32 npc[64],human[64],clan[64];static int assignments,localized;
static void *assign(void *a,void *b){++assignments;strcpy(a,b);return a;}
static void construct(void *a,const char *b){strcpy(a,b);}
static void destroy(void *a){}
static u32 length(void *a){return strlen(a);}
static const char *cstring(void *a){return a;}
static int global(void *a){return *(int*)a;}
static void *mode(void){return human;}
static void *top(void *a){return npc;}
static void *original_label(void *a){++localized;return "Trainer Earl";}
static int original_csv(void *row,int column){if((uintptr_t)row!=0x1234)exit(3);return column*1000;}
static uintptr_t resolve(u32 a){switch(a){
case 0x18dcf9:return (uintptr_t)construct;case 0x18df31:return (uintptr_t)assign;
case 0x18db2d:return (uintptr_t)destroy;case 0x18f4bd:return (uintptr_t)length;
case 0x19c099:return (uintptr_t)cstring;case 0x7d2e1:return (uintptr_t)global;
case 0x91f7d:return (uintptr_t)mode;case 0x93e8d:return (uintptr_t)top;
case 0x511a9:return (uintptr_t)original_label;case 0x19120d:return (uintptr_t)original_csv;
default:return (uintptr_t)printf;}}
#define CALL(a,result,args) ((result (*)args)resolve(a))
#define API_printf 0
#include "opponent.inc"
int main(void){
 npc[4]=0xffffffffu;human[5]=1;clan[4]=3;clan[5]=1;
 char value[]="Original";guest_avatar_set_name(npc,value);
 if(strcmp((char*)npc+32,value))return 3;
 int data=18000007;if(strcmp(guest_npc_label(&data),"Trainer Earl") || localized!=1)return 3;
 opponent_enabled=1;
 guest_avatar_set_name(human,value);guest_avatar_set_name(clan,value);
 if(strcmp((char*)human+32,value)||strcmp((char*)clan+32,value))return 3;
 guest_avatar_set_name(npc,value);
 if(strcmp((char*)npc+32,opponent_names[0]))return 3;
 strcpy((char*)npc+32,"Trainer Jonas");
 if(guest_npc_label(&data)!=(u8*)npc+32 || strcmp((char*)npc+32,opponent_names[0]))return 3;
 int count=assignments;guest_npc_label(&data);if(assignments!=count)return 3;
 data=18000000;if(strcmp(guest_npc_label(&data),"Trainer Earl")||localized!=2)return 3;
 // Even a stale paid-mode flag must not alter any of the seven trainers'
 // original CSV regeneration getters or native reserve fields.
 for(int i=0;i<7;++i){u32 trainer[21]={0};trainer[0]=18000000+i;trainer[1]=0x1234;trainer[20]=i+1;
  if(guest_npc_mana(trainer)!=11000 || guest_npc_mana_end(trainer)!=12000 ||
     guest_npc_mana_overtime(trainer)!=13000 || guest_npc_reserve(trainer)!=i+1)return 3;
 }
 opponent_enabled=0;guest_avatar_set_name(npc,value);
 if(strcmp((char*)npc+32,value))return 3;
 return 0;
}
''')
            p=subprocess.run(['clang','-std=c11','-O2','-I'+str(ROOT/'tools/clash_guest'),str(path),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(p.returncode,0,p.stderr)
            p=subprocess.run([str(exe)],capture_output=True,text=True)
            self.assertEqual(p.returncode,0,p.stderr)

    def test_authored_decks_require_source_unlocks(self):
        with tempfile.TemporaryDirectory() as directory:
            bundle=Path(directory);res=bundle/'res/csv_logic';res.mkdir(parents=True)
            def table(name,header,rows):
                with (res/(name+'.csv')).open('w',newline='') as f:csv.writer(f).writerows([header,['string']*len(header),*rows])
            table('arenas',['Name','Arena'],[['TrainingCamp','0'],['Arena1','1'],['Arena2','2']])
            names=sorted({n for deck in DECKS.values() for n in deck})
            groups={'spells_characters':[],'spells_buildings':[],'spells_other':[]}
            for name in names:
                group='spells_buildings' if name=='GoblinHut' else 'spells_other' if name in ['Fireball','Arrows','GoblinBarrel'] else 'spells_characters'
                groups[group].append([name,'Arena1','Rare' if name in ['Fireball','Giant'] else 'Common','5'])
            header=['Name','UnlockArena','Rarity','ManaCost']
            for group,rows in groups.items():table(group,header,rows)
            table('globals',['Name','NumberValue'],[['MAX_MANA','10'],['START_MANA','5'],['MANA_REGEN_MS','28000'],['MANA_REGEN_MS_END','14000'],['MANA_REGEN_MS_OVERTIME','14000']])
            policy=load_policy(bundle)
            self.assertEqual(len(policy['decks']),6)
            self.assertEqual(policy['manaRates'],[28000,14000,14000])
            for deck in policy['decks']:
                self.assertEqual(len({c['id'] for c in deck['cards']}),8)
                self.assertTrue(all(c['arena']<=1 for c in deck['cards']))
            table('spells_buildings',header,[['GoblinHut','Arena2','Rare','5']])
            with self.assertRaisesRegex(ValueError,'unlocks'):load_policy(bundle)
            table('spells_buildings',header,[['GoblinHut','Arena1','Rare','7']])
            with self.assertRaisesRegex(ValueError,'afford'):load_policy(bundle)

    @unittest.skipUnless(shutil.which('clang'),'C compiler unavailable')
    def test_nonrepeating_choice_and_checkpoint(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'test.c';exe=Path(directory)/'test'
            path.write_text(r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
typedef unsigned int u32;typedef unsigned char u8;
#define CALL(address,result,args) ((result (*)args)(address))
#define GUEST_ARENA_PROFILE_COUNT 18
static uintptr_t s_read=(uintptr_t)&fread,s_write=(uintptr_t)&fwrite;
static void fail(const char *why) {fprintf(stderr,"%s\n",why);exit(2);}
static u32 checksum(const u8 *p,u32 n) {u32 h=2166136261;while(n--)h=(h^*p++)*16777619;return h;}
#include "opponent_state.inc"
int main(int argc,char **argv) {
    if(argc>1) {
        u32 tag;if(fread(&tag,1,4,stdin)!=4 || tag!=0x314f4247u)return 2;
        read_opponent(stdin,0x12345678);if(fgetc(stdin)!=EOF)return 2;write_opponent(stdout,0x12345678);return 0;
    }
    if(sizeof opponent_names/sizeof *opponent_names!=64)return 3;
    for(u32 i=0;i<64;++i) {
        if(!strlen(opponent_names[i]) || strlen(opponent_names[i])>15)return 3;
        for(u32 j=0;j<i;++j)if(!strcmp(opponent_names[i],opponent_names[j]))return 3;
    }
    u32 rng=1;const u32 expected[]={270369u,67634689u,2647435461u};
    for(int i=0;i<3;++i)if(opponent_random(&rng)!=expected[i])return 3;
    int scores[18];unsigned decks[18];for(int i=0;i<18;++i){scores[i]=i<6?50:i<12?150:300;decks[i]=i%6;}
    struct OpponentHistory h={1,0,0,0,0,0},old={0};unsigned seen=0;
    for(int i=0;i<2000;++i) {
        if(!opponent_choose(&h,scores,decks,18,65) || h.variant>=12 || h.name>=64 || h.style>=3 || !h.seed || h.seed>0x7fffffff)return 3;
        if(i && (h.name==old.name || h.style==old.style || decks[h.variant]==decks[old.variant]))return 3;
        seen|=1u<<decks[h.variant];old=h;
    }
    if(seen!=63 || opponent_choose(&h,scores,decks,0,65) || opponent_choose(&h,scores,decks,18,-1))return 3;
    opponent_history=(struct OpponentHistory){123,7,11,63,2,2147483647};write_opponent(stdout,0x12345678);
}
''')
            p=subprocess.run(['clang','-std=c11','-O2','-I'+str(ROOT/'tools/clash_guest'),str(path),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(p.returncode,0,p.stderr)
            prefix=struct.pack('<9I',0x314f4247,1,123,7,11,63,2,2147483647,0x12345678)
            raw=prefix+struct.pack('<I',fnv(prefix))
            p=subprocess.run([str(exe)],capture_output=True);self.assertEqual((p.returncode,p.stdout),(0,raw),p.stderr)
            def run(data):return subprocess.run([str(exe),'read'],input=data,capture_output=True)
            p=run(raw);self.assertEqual((p.returncode,p.stdout),(0,raw))
            for n in range(len(raw)):self.assertEqual(run(raw[:n]).returncode,2)
            for word,value in [(1,2),(2,0),(3,0),(3,1000000001),(4,18),(5,64),(6,3),(7,0),(7,2147483648),(8,0)]:
                wrong=bytearray(raw);struct.pack_into('<I',wrong,word*4,value);struct.pack_into('<I',wrong,36,fnv(wrong[:36]))
                self.assertEqual(run(wrong).returncode,2)
            self.assertEqual(run(raw+b'\0').returncode,2)

if __name__=='__main__':unittest.main()
