"""Authored spell data, independent profile framing and native cache contracts."""
from pathlib import Path
import shutil,struct,subprocess,sys,tempfile,unittest
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'tools'))
from clash_spell_catalog import catalog,header
from clash_custom_cards import fnv

PREAMBLE=r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
typedef unsigned int u32;typedef unsigned char u8;
#define GUEST_SPELL_COUNT 3
#define FIELD(p,n) (*(u32*)((char*)(p)+(n)))
#define API_puts puts
#define s_getenv getenv
#define s_open fopen
#define s_read fread
#define s_write fwrite
#define s_flush fflush
#define s_fileno fileno
#define s_sync sync_ok
#define s_close fclose
#define s_rename test_rename
#define s_error error_ptr
static int sync_ok(int fd){return 0;}
static int *error_ptr(void){return &errno;}
static int test_rename(const char *a,const char *b){if(getenv("FAIL_RENAME")){errno=EIO;return -1;}return rename(a,b);}
static void fail(const char *s){fprintf(stderr,"%s\n",s);exit(9);}
static u32 checksum(const u8 *p,u32 n){u32 h=2166136261u;while(n--)h=(h^*p++)*16777619u;return h;}
static void path_append(char *o,const char *a,const char *b,u32 n){snprintf(o,n,"%s%s",a,b);}
static void mod_message(const char *s){puts(s);}
static int allowed=1,battle_started=0,arena_battle=0;
static int mod_allowed(void){return allowed;}
struct SpellDef {u32 id,caps;const char *name,*shots[3];};
static const struct SpellDef spell_defs[]={{28000000,27,"Authored shot",{"shot","",""}},{28000002,22,"Authored aura",{"","",""}},{28000006,0,"Authored mirror",{"","",""}}};
static u32 cards[3][80],area[80],shot[80];
static void *native_card(int id){for(int i=0;i<3;++i)if((u32)id==spell_defs[i].id)return cards[i];return 0;}
static void *native_area(void *p){return p==cards[1]?area:0;}
static void *mod_data(const char *name,u32 address){return *name?shot:0;}
static uintptr_t resolve(uintptr_t a){if(a==0xb821d)return (uintptr_t)native_card;if(a==0x753a5)return (uintptr_t)native_area;return a;}
#define CALL(address,result,args) ((result (*)args)resolve((uintptr_t)(address)))
static uintptr_t symbol(const char *name){return (uintptr_t)ferror;}
#include "mods_math.inc"
static void mod_apply_units(void);
#include "spells.inc"
static void mod_apply_units(void){spell_restore();spell_apply();}
static void setup(void){for(int i=0;i<3;++i)FIELD(cards[i],0x74)=4;FIELD(shot,0x80)=2500;FIELD(shot,0x74)=600;FIELD(area,0x74)=5000;FIELD(area,0x6c)=9000;FIELD(area,0x70)=600;spell_initialize();}
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d\n",__LINE__);return 3;}}while(0)
'''

class SpellTests(unittest.TestCase):
    def test_catalog_keeps_ids_and_resolves_distinct_damage_and_area_paths(self):
        tables={'spells_other':[dict(Name='Shot',Projectile='Deco',CustomFirstProjectile='Hit'),dict(Name='Disabled',NotInUse='true'),dict(Name='Aura',AreaEffectObject='Aura'),dict(Name='Freeze-like',AreaEffectObject='Freeze'),dict(Name='Mirror',Mirror='true')],
                'projectiles':[dict(Name='Deco',Speed='600'),dict(Name='Hit',Damage='110',Radius='4000',Speed='600')],
                'area_effect_objects':[dict(Name='Aura',LifeDuration='9000',BuffTime='500',HitSpeed='300',Radius='5000'),dict(Name='Freeze',LifeDuration='6000',BuffTime='6000',Radius='4000')]}
        with patch('clash_spell_catalog.read_table',side_effect=lambda _,t:tables[t]):
            rows=catalog('.');self.assertEqual([r['id'] for r in rows],[28000000,28000002,28000003,28000004])
            self.assertEqual(rows[0]['shots'],['Deco','Hit']);self.assertEqual(rows[0]['caps'],27)
            self.assertEqual(rows[1]['caps'],22);self.assertEqual(rows[2]['caps'],18);self.assertEqual(rows[3]['caps'],0)
            self.assertIn('GUEST_SPELL_COUNT 4',header('.'))
            tables['spells_other'][0]['CustomFirstProjectile']='missing'
            with self.assertRaises(ValueError):catalog('.')

    def compile(self,root,body):
        src=root/'spell.c';exe=root/'spell';src.write_text(PREAMBLE+body,encoding='utf-8')
        p=subprocess.run(['clang','-std=gnu11','-O2','-fno-builtin','-I'+str(ROOT/'tools/clash_guest'),str(src),'-o',str(exe)],capture_output=True,text=True)
        self.assertEqual(p.returncode,0,p.stderr);return exe

    @unittest.skipUnless(shutil.which('clang'),'C compiler unavailable')
    def test_native_fields_no_compounding_damage_priority_disable_and_training_restore(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);(root/'Documents').mkdir()
            exe=self.compile(root,r'''
int main(void){setup();spell_enabled=1;spell_records[0].value[SPELL_DAMAGE]=200;spell_records[0].value[SPELL_RADIUS]=150;spell_records[0].value[SPELL_SPEED]=200;spell_records[0].value[SPELL_COST]=2;
spell_records[1].value[SPELL_DURATION]=200;spell_records[1].value[SPELL_RADIUS]=50;
for(int i=0;i<3;++i){mod_apply_units();CHECK(FIELD(shot,0x80)==3750 && FIELD(shot,0x74)==1200 && FIELD(cards[0],0x74)==2);CHECK(FIELD(area,0x74)==2500 && FIELD(area,0x6c)==18000 && FIELD(area,0x70)==1200);}
int handled=0;CHECK(spell_damage(shot,325,&handled)==650 && handled);handled=0;CHECK(spell_damage(area,325,&handled)==325 && !handled);
allowed=0;mod_apply_units();CHECK(FIELD(shot,0x80)==2500 && FIELD(shot,0x74)==600 && FIELD(cards[0],0x74)==4);handled=0;CHECK(spell_damage(shot,325,&handled)==325 && !handled);
allowed=1;battle_started=1;arena_battle=0;mod_apply_units();CHECK(FIELD(area,0x6c)==9000);arena_battle=1;mod_apply_units();CHECK(FIELD(area,0x6c)==18000);
spell_enabled=0;mod_apply_units();CHECK(FIELD(area,0x6c)==9000 && FIELD(shot,0x80)==2500);
for(u32 f=0;f<SPELL_FIELDS;++f){CHECK(!spell_value_valid(f,0xffffffff,31));CHECK(spell_value_valid(f,spell_default(f),0));}
CHECK(!spell_value_valid(SPELL_DURATION,200,18));CHECK(!spell_value_valid(SPELL_COST,11,31));CHECK(!spell_value_valid(SPELL_RADIUS,24,31));return 0;}
''')
            import os
            p=subprocess.run([str(exe)],env={**os.environ,'HOME':str(root)},capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr)

    @unittest.skipUnless(shutil.which('clang'),'C compiler unavailable')
    def test_real_loader_preserves_invalid_files_and_failed_commit_retains_previous(self):
        import os
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);(root/'Documents').mkdir();path=root/'Documents/ClashSpellLab-v1.bin'
            exe=self.compile(root,r'''
int main(int argc,char **argv){setup();if(argc>1){spell_records[0].value[SPELL_DAMAGE]=200;spell_enabled=1;spell_reset_requested=!strcmp(argv[1],"reset");int ok=spell_save();printf("save=%d\n",ok);}printf("state=%u,%u,%u\n",spell_rejected,spell_enabled,spell_records[0].value[SPELL_DAMAGE]);return 0;}
''')
            def run(*args,fail=False):
                env={**os.environ,'HOME':str(root)}
                if fail:env['FAIL_RENAME']='1'
                p=subprocess.run([str(exe),*args],env=env,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr);return p.stdout
            self.assertIn('state=0,0,100',run());self.assertIn('save=1',run('save'));valid=path.read_bytes()
            self.assertEqual(struct.unpack_from('<7I',valid)[:5],(0x31504c53,1,3,1,0));self.assertEqual(struct.unpack_from('<I',valid,20)[0],fnv(valid[28:]))
            self.assertIn('state=0,1,200',run());self.assertIn('save=0',run('save',fail=True));self.assertEqual(path.read_bytes(),valid)
            invalids=[valid[:n] for n in [0,4,27,len(valid)-1]]+[valid+b'x']
            for offset,value in [(28,28000001),(32,900),(28+24+4,200)]:
                b=bytearray(valid);struct.pack_into('<I',b,offset,value);struct.pack_into('<I',b,20,fnv(b[28:]));struct.pack_into('<I',b,24,fnv(b[:24]));invalids.append(bytes(b))
            for b in invalids:
                path.write_bytes(b);self.assertIn('state=1,0,100',run());self.assertEqual(path.read_bytes(),b)
            self.assertIn('save=0',run('save'));self.assertEqual(path.read_bytes(),b)
            self.assertIn('save=1',run('reset'));self.assertEqual(path.with_name(path.name+'.rejected').read_bytes(),b)
            self.assertIn('state=0,1,200',run())
            path.write_bytes(b);self.assertIn('save=0',run('reset'));self.assertEqual(path.read_bytes(),b)

if __name__=='__main__':unittest.main()
