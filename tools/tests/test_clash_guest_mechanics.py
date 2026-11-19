"""Real native offsets, persisted references and bounded factory overrides."""
from pathlib import Path
import shutil,subprocess,tempfile,unittest,sys,struct
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'tools'))
from clash_mechanics_catalog import catalog,header
PREAMBLE=r"""
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
typedef unsigned int u32;typedef unsigned char u8;
#define FORGE_MAX_CARDS 16
#define GUEST_MECH_CARD_COUNT 2
#define GUEST_MECH_TROOP_COUNT 2
#define GUEST_MECH_SHOT_COUNT 2
static u32 forge_loaded_count,forge_ids[16];
struct ForgeRecord {const char *name;};static struct ForgeRecord forge_records[16];
struct MechCardDef {u32 id;const char *name;};
static struct MechCardDef mech_cards[]={{26000000,"Witch-like test"},{28000000,"Barrel-like test"}};
static const char *mech_troop_names[]={"Simple A","Simple B"},*mech_shot_names[]={"Shot A","Shot B"};
#define FIELD(p,n) (*(u32*)((char*)(p)+(n)))
#define CALL(address,result,args) ((result (*)args)(address))
#define API_puts puts
static int allowed=1,battle_started,arena_battle;
static int mod_allowed(void){return allowed;}
"""
class MechanicsTests(unittest.TestCase):
    def test_catalog_keeps_distinct_card_counts_and_barrel_and_filters_recursive_targets(self):
        data={'characters':[dict(Name='Simple'),dict(Name='Factory',SpawnCharacter='Simple'),dict(Name='DeathFactory',DeathSpawnCharacter='Simple')],
              'projectiles':[dict(Name='Arrow'),dict(Name='Barrel',SpawnCharacter='Simple')],
              'spells_characters':[dict(Name='One',SummonCharacter='Simple'),dict(Name='Many',SummonCharacter='Simple')],
              'spells_buildings':[], 'spells_other':[dict(Name='Other'),dict(Name='BarrelCard',Projectile='Barrel')]}
        with patch('clash_mechanics_catalog.read_table',side_effect=lambda _,t:data[t]):
            cards,troops,shots=catalog('.')
            self.assertEqual([c['id'] for c in cards],[26000000,26000001,28000001])
            self.assertEqual(troops,['Simple']);self.assertEqual(shots,['Arrow'])
            self.assertIn('GUEST_MECH_CARD_COUNT 3',header('.'))

    def compile_run(self,code,args=(),input=None):
        with tempfile.TemporaryDirectory() as d:
            src=Path(d)/'mechanics.c';exe=Path(d)/'mechanics';src.write_text(code)
            p=subprocess.run(['clang','-std=c11','-O2','-I'+str(ROOT/'tools/clash_guest'),str(src),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(p.returncode,0,p.stderr)
            p=subprocess.run([str(exe),*args],capture_output=True,text=True,input=input)
            self.assertEqual(p.returncode,0,p.stderr);return p.stdout

    @unittest.skipUnless(shutil.which('clang'),'C compiler unavailable')
    def test_native_deployment_factory_projectile_restore_and_training_policy(self):
        source=(ROOT/'tools/clash_guest/mechanics.inc').read_text()
        prefix=source[:source.index('static void mech_initialize')]
        apply=source[source.index('static void mech_restore'):source.index('static int mech_save')]
        code=PREAMBLE+prefix+'static void mech_initialize(void){}\n'+apply+r"""
#define CHECK(c) do{if(!(c)){fprintf(stderr,"line %d\n",__LINE__);return 3;}}while(0)
int main(void){
 u32 spell[2][160]={0},unit[160]={0},impact[160]={0};
 mech_native[0].spell=spell[0];mech_native[0].unit=unit;
 mech_native[1].spell=spell[1];mech_native[1].impact=impact;
 for(u32 i=0;i<2;++i)for(u32 p=0;p<12;++p){mech_native[i].original[p]=100+p;mech_write(i,p,100+p);}
 mech_troops[0]=(void*)0x10000;mech_troops[1]=(void*)0x11000;
 u32 choice[2][160]={0};mech_projectiles[0]=choice[0];mech_projectiles[1]=choice[1];
 mech_shot_speeds[0]=300;mech_shot_speeds[1]=700;
 for(u32 i=0;i<2;++i)mech_records[i].id=mech_id(i);
 u32 values[9]={4,5,2,2,0,0,2500,500,2};memcpy(mech_records[0].value,values,sizeof values);
 mech_records[1].value[MECH_IMPACT_COUNT]=8;mech_records[1].value[MECH_IMPACT_TYPE]=1;
 CHECK(mech_record_valid(0,&mech_records[0]) && mech_record_valid(1,&mech_records[1]));
 mech_records[0].value[MECH_IMPACT_TYPE]=1;CHECK(!mech_record_valid(0,&mech_records[0]));mech_records[0].value[MECH_IMPACT_TYPE]=0;
 mech_ready=mech_enabled=1;
 for(int repeat=0;repeat<3;++repeat){mech_apply();
  CHECK(FIELD(spell[0],0x7c)==4 && FIELD(unit,0x1ac)==5 && FIELD(unit,0xb4)==0x11000 && FIELD(unit,0xbc)==0);
  CHECK(FIELD(unit,0x64)==(u32)(uintptr_t)choice[1] && FIELD(unit,0x68)==0 && FIELD(unit,0x1b0)==2500 && FIELD(unit,0x1c0)==500 && FIELD(spell[0],0x74)==2);
  CHECK(FIELD(impact,0x58)==8 && FIELD(impact,0x50)==0x10000 && FIELD(impact,0x88)==0);
 }
 battle_started=1;arena_battle=0;mech_apply();CHECK(FIELD(unit,0xb4)==102 && FIELD(impact,0x58)==106);
 arena_battle=1;mech_apply();CHECK(FIELD(unit,0x1ac)==5 && FIELD(impact,0x58)==8);
 allowed=0;mech_apply();CHECK(FIELD(spell[0],0x7c)==100);
 allowed=1;mech_enabled=0;mech_apply();
 for(u32 i=0;i<2;++i)for(u32 p=0;p<12;++p){void *o=mech_object(i,p);if(o)CHECK(FIELD(o,mech_offsets[p])==100+p);}
 return 0;
}
"""
        self.compile_run(code)

    @unittest.skipUnless(shutil.which('clang'),'C compiler unavailable')
    def test_bounds_for_zero_defaults_counts_references_cost_and_delays(self):
        self.compile_run(PREAMBLE+r"""
#include "mechanics_rules.inc"
#define CHECK(c) do{if(!(c))return __LINE__;}while(0)
int main(void){
 for(u32 f=0;f<MECH_FIELDS;++f){CHECK(mech_value_valid(f,0,2,2));CHECK(!mech_value_valid(f,0xffffffffu,2,2));}
 CHECK(!mech_value_valid(9,0,2,2));CHECK(mech_value_valid(MECH_COUNT,16,2,2));CHECK(!mech_value_valid(MECH_COUNT,17,2,2));
 CHECK(mech_value_valid(MECH_SPAWN_COUNT,8,2,2));CHECK(!mech_value_valid(MECH_SPAWN_COUNT,9,2,2));
 CHECK(mech_value_valid(MECH_SHOT,2,2,2));CHECK(!mech_value_valid(MECH_SHOT,3,2,2));
 CHECK(mech_value_valid(MECH_IMPACT_TYPE,2,2,2));CHECK(!mech_value_valid(MECH_IMPACT_TYPE,3,2,2));
 CHECK(mech_value_valid(MECH_PAUSE,500,2,2));CHECK(mech_value_valid(MECH_PAUSE,60000,2,2));CHECK(!mech_value_valid(MECH_PAUSE,499,2,2));CHECK(!mech_value_valid(MECH_PAUSE,60001,2,2));
 CHECK(mech_value_valid(MECH_DELAY,100,2,2));CHECK(!mech_value_valid(MECH_DELAY,99,2,2));CHECK(!mech_value_valid(MECH_DELAY,10001,2,2));
 CHECK(mech_value_valid(MECH_ELIXIR,10,2,2));CHECK(!mech_value_valid(MECH_ELIXIR,11,2,2));return 0;
}
""")


    @unittest.skipUnless(shutil.which('clang'),'C compiler unavailable')
    def test_optional_profile_checksum_extent_identity_and_reference_rejection(self):
        source=(ROOT/'tools/clash_guest/mechanics.inc').read_text()
        prefix=source[:source.index('static void mech_initialize')]
        load=source[source.index('    const char *home=CALL(s_getenv'):source.index('static void mech_restore')]
        extra=r"""
#define s_getenv test_home
#define s_open fopen
#define s_read fread
#define s_close fclose
#define s_error error_ptr
static const char *directory;
static const char *test_home(const char *key){return directory;}
static int *error_ptr(void){return &errno;}
static uintptr_t symbol(const char *name){return (uintptr_t)ferror;}
static void path_append(char *out,const char *a,const char *b,u32 cap){snprintf(out,cap,"%s%s",a,b);}
static u32 checksum(const u8 *p,u32 n){u32 h=2166136261u;while(n--)h=(h^*p++)*16777619u;return h;}
"""
        code=PREAMBLE+extra+prefix+r"""
static void load(void){
 for(u32 i=0;i<2;++i){mech_records[i].id=mech_id(i);mech_native[i].spell=(void*)1;mech_native[i].unit=(void*)1;mech_native[i].original[2]=1;mech_native[i].original[4]=1;}
"""+load+r"""
int main(int argc,char **argv){directory=argv[1];load();printf("%u %u %u %d\n",mech_enabled,mech_records[0].value[0],mech_records[1].value[0],mech_rejected);return 0;}
"""
        def fnv(b):
            h=2166136261
            for v in b:h=((h^v)*16777619)&0xffffffff
            return h
        def profile(rows,enabled=1,selected=0):
            body=b''.join(struct.pack('<10I',*r) for r in rows)
            h=struct.pack('<6I',0x3148434d,1,len(rows),enabled,selected,fnv(body))
            return h+struct.pack('<I',fnv(h))+body
        with tempfile.TemporaryDirectory() as d:
            directory=Path(d);(directory/'Documents').mkdir();p=directory/'Documents/ClashMechanics-v1.bin'
            def run(expected):self.assertTrue(self.compile_run(code,[str(directory)]).endswith(expected))
            run('0 0 0 0\n')
            good=profile([[26000000,4]+[0]*8]);p.write_bytes(good);run('1 4 0 0\n');self.assertEqual(p.read_bytes(),good)
            bad=[good[:n] for n in (0,4,27,28,len(good)-1)]+[good+b'x',profile([[26000001,4]+[0]*8]),profile([[26000000,17]+[0]*8]),profile([[26000000,0,0,3]+[0]*6]),profile([[26000000]+[0]*9],2),profile([[26000000]+[0]*9],1,1)]
            b=bytearray(good);b[-1]^=1;bad.append(bytes(b))
            for value in bad:
                p.write_bytes(value);run('0 0 0 1\n');self.assertEqual(p.read_bytes(),value)

if __name__=='__main__':unittest.main()
