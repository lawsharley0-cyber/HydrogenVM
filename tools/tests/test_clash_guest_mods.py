"""Editor arithmetic, typed-entry rejection and real card-to-unit catalog IDs."""
from pathlib import Path
import random,shutil,struct,subprocess,tempfile,unittest
from unittest.mock import patch
import sys
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'tools'))
from clash_mod_catalog import catalog
class ModLabTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('clang'),'C compiler unavailable')
    def test_optional_profile_validation_and_explicit_recovery_preserve_bad_file(self):
        code=(ROOT/'tools/clash_guest/mods.inc').read_text()
        prefix=code[code.index('struct ModRecord'):code.index('static void *mod_data')]
        load=code[code.index('    const char *home=CALL(s_getenv'):code.index('static void mod_save')]
        def fnv(data):
            h=2166136261
            for b in data:h=((h^b)*16777619)&0xffffffff
            return h
        def profile(records,enabled=1,selected=0):
            body=struct.pack('<'+'I'*len(records),*records)
            h=struct.pack('<6I',0x31444f4d,1,len(records)//8,enabled,selected,fnv(body))
            return h+struct.pack('<I',fnv(h))+body
        with tempfile.TemporaryDirectory() as d:
            directory=Path(d);source=directory/'profile.c';exe=directory/'profile'
            source.write_text(r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
typedef unsigned int u32;typedef unsigned char u8;
#define GUEST_MOD_UNIT_COUNT 2
#define FIELD(p,n) (*(u32*)((char*)(p)+(n)))
#define API_puts puts
#define CALL(address,result,args) ((result (*)args)(address))
#define s_open fopen
#define s_read fread
#define s_close fclose
#define s_error error_ptr
#define s_getenv test_home
#define s_rename rename
static char *home;
static int *error_ptr(void){return &errno;}
static const char *test_home(const char *name){return home;}
static uintptr_t symbol(const char *name){if(strcmp(name,"ferror"))exit(4);return (uintptr_t)ferror;}
static void path_append(char *out,const char *a,const char *b,u32 cap){if(snprintf(out,cap,"%s%s",a,b)>=(int)cap)exit(4);}
static u32 checksum(const u8 *p,u32 n){u32 h=2166136261u;while(n--)h=(h^*p++)*16777619u;return h;}
static int save_initialized=1,logged_in=1,committed_wins=7,battle_started,arena_battle;
static void fail(const char *s){fprintf(stderr,"%s\n",s);exit(2);}
#include "mods_math.inc"
''' + prefix + r'''
static void load(void){
 for(u32 i=0;i<2;++i)for(u32 f=0;f<8;++f)mod_records[i].value[f]=100;
''' + load + r'''
int main(int argc,char **argv){
 home=argv[1];load();
 if(argc>2){if(!mod_profile_rejected)return 5;
  if(mod_archive_rejected())return 6;
  mod_reset_requested=1;printf("archive=%d\n",mod_archive_rejected());
 }
 printf("%u %u %u %d\n",mod_enabled,mod_records[0].value[0],mod_records[1].value[0],mod_profile_rejected);
 return 0;
}
''')
            p=subprocess.run(['clang','-std=c11','-O2','-I'+str(ROOT/'tools/clash_guest'),str(source),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(p.returncode,0,p.stderr)
            (directory/'Documents').mkdir();path=directory/'Documents/ClashModLab-v1.bin'
            def run(*args):
                p=subprocess.run([str(exe),str(directory),*args],capture_output=True,text=True)
                self.assertEqual(p.returncode,0,p.stderr);return p.stdout
            self.assertTrue(run().endswith('0 100 100 0\n'))
            valid=profile([125]+[100]*7)
            path.write_bytes(valid);self.assertTrue(run().endswith('1 125 100 0\n'))
            cases=[valid[:n] for n in [0,4,24,27,28,40,len(valid)-1]]
            cases += [valid+b'x',profile([100]*8,2),profile([100]*8,1,1),profile([0]+[100]*7),profile([100]*3+[401]+[100]*4)]
            damaged=bytearray(valid);damaged[-1]^=1;cases.append(bytes(damaged))
            for bad in cases:
                path.write_bytes(bad);self.assertTrue(run().endswith('0 100 100 1\n'));self.assertEqual(path.read_bytes(),bad)
            bad=b'broken optional settings';path.write_bytes(bad)
            self.assertIn('archive=1',run('recover'));self.assertFalse(path.exists())
            rejected=Path(str(path)+'.rejected');self.assertEqual(rejected.read_bytes(),bad)
            path.write_bytes(b'another invalid profile')
            self.assertIn('archive=0',run('recover'));self.assertEqual(path.read_bytes(),b'another invalid profile');self.assertEqual(rejected.read_bytes(),bad)

    @unittest.skipUnless(shutil.which('clang'),'C compiler unavailable')
    def test_native_data_modifiers_restore_and_do_not_compound(self):
        code=(ROOT/'tools/clash_guest/mods.inc').read_text()
        prefix=code[code.index('struct ModRecord'):code.index('static void *mod_data')]
        suffix=code[code.index('extern int guest_original_mod_health'):]
        with tempfile.TemporaryDirectory() as d:
            source=Path(d)/'native.c';exe=Path(d)/'native'
            source.write_text(r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned int u32;
#define GUEST_MOD_UNIT_COUNT 2
#define FIELD(p,n) (*(u32*)((char*)(p)+(n)))
#define API_puts puts
#define CALL(address,result,args) ((result (*)args)(address))
static int save_initialized=1,logged_in=1,committed_wins=7,battle_started,arena_battle;
static void fail(const char *s){fprintf(stderr,"%s\n",s);exit(2);}
#include "mods_math.inc"
''' + prefix + suffix + r'''
int guest_original_mod_health(void *p,int level){return 100+level;}
int guest_original_mod_damage(void *p,int level){return 50+level;}
int guest_original_mod_shot_damage(void *p,int level){return 40+level;}
#define CHECK(c) do{if(!(c)){fprintf(stderr,"line %d\n",__LINE__);return 3;}}while(0)
int main(void){
 u32 unit[2][160]={0},shot[2][160]={0};
 for(u32 i=0;i<2;++i){mod_units[i]=unit[i];mod_shots[i]=shot[i];
  for(u32 j=0;j<6;++j){mod_original[i][j]=j==1||j==2?1000:100;}
  for(u32 f=0;f<MOD_FIELDS;++f)mod_records[i].value[f]=100;
 }
 mod_ready=mod_enabled=1;
 u32 values[8]={125,200,50,150,200,200,150,125};
 for(u32 f=0;f<8;++f)mod_records[0].value[f]=values[f];
 for(int repeat=0;repeat<3;++repeat){mod_apply_units();
  CHECK(FIELD(unit[0],0x150)==125 && FIELD(unit[0],0x154)==500 && FIELD(unit[0],0x1bc)==2000);
  CHECK(FIELD(unit[0],0x1cc)==150 && FIELD(shot[0],0x74)==200 && FIELD(unit[0],0x15c)==125);
  CHECK(FIELD(unit[1],0x150)==100 && FIELD(shot[1],0x74)==100);
  CHECK(guest_mod_health(unit[0],5)==210 && guest_mod_damage(unit[0],2)==78);
  CHECK(guest_mod_shot_damage(shot[0],2)==63 && guest_mod_health(unit[1],5)==105);
 }
 // Friendly battles and original training use the original native statistics.
 battle_started=1;arena_battle=0;mod_apply_units();
 CHECK(FIELD(unit[0],0x150)==100 && FIELD(unit[0],0x154)==1000 && guest_mod_health(unit[0],5)==105);
 arena_battle=1;mod_apply_units();CHECK(FIELD(unit[0],0x150)==125);
 committed_wins=6;mod_apply_units();CHECK(FIELD(unit[0],0x150)==100 && guest_mod_damage(unit[0],2)==52);
 committed_wins=7;mod_enabled=0;mod_apply_units();CHECK(FIELD(unit[0],0x1cc)==100);
 return 0;
}
''')
            p=subprocess.run(['clang','-std=c11','-O2','-I'+str(ROOT/'tools/clash_guest'),str(source),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(p.returncode,0,p.stderr)
            p=subprocess.run([str(exe)],capture_output=True,text=True)
            self.assertEqual(p.returncode,0,p.stderr)

    def test_catalog_uses_original_spell_ids_for_building_and_shared_units(self):
        data={'characters':[dict(Name='Knight',Projectile=''),dict(Name='Archer',Projectile='Arrow')],
              'buildings':[dict(Name='Cannon',Projectile='CannonBall')],
              'spells_characters':[dict(Name='Knight',SummonCharacter='Knight'),dict(Name='Archer',SummonCharacter='Archer'),dict(Name='Pair',SummonCharacter='Knight')],
              'spells_buildings':[dict(Name='CannonCard',SummonCharacter='Cannon')]}
        with patch('clash_mod_catalog.read_table',side_effect=lambda _,table:data[table]):
            result=catalog('.')
            self.assertEqual([x['cardId'] for x in result],[26000000,26000001,27000000])
            self.assertEqual(result[-1]['projectile'],'CannonBall')
            data['spells_buildings'][0]['SummonCharacter']='Missing'
            with self.assertRaises(ValueError):catalog('.')

    @unittest.skipUnless(shutil.which('clang'),'C compiler unavailable')
    def test_percent_bounds_numeric_input_and_wide_integer_division(self):
        with tempfile.TemporaryDirectory() as d:
            source=Path(d)/'mod.c';exe=Path(d)/'mod'
            source.write_text(r'''
#include <stdio.h>
#include <stdlib.h>
typedef unsigned int u32;
static void fail(const char *why){fprintf(stderr,"%s\n",why);exit(2);}
#include "mods_math.inc"
int main(int argc,char **argv){
 if(argc>1){u32 n=0;if(!mod_parse_number(argv[1],&n))return 2;printf("%u\n",n);return 0;}
 for(u32 f=0;f<MOD_FIELDS;++f){if(!mod_percent_valid(f,100)||mod_percent_valid(f,0)||mod_percent_valid(f,801))return 3;}
 if(mod_percent_valid(MOD_SIZE,24)||mod_percent_valid(MOD_SIZE,401)||mod_percent_valid(8,100))return 3;
 if(mod_scaled(650,200,0)!=1300||mod_scaled(1100,200,1)!=550||mod_scaled(87,200,0)!=174||mod_scaled(0,100,0)!=0||mod_scaled(1,800,1)!=1)return 3;
 u32 n,d;while(scanf("%u %u",&n,&d)==2)printf("%u\n",mod_div(n,d));return 0;
}
''')
            p=subprocess.run(['clang','-std=c11','-O2','-I'+str(ROOT/'tools/clash_guest'),str(source),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(p.returncode,0,p.stderr)
            rng=random.Random(42);cases=[(0,1),(0xffffffff,10),(0xffffffff,0xffffffff),(0xffffffff,0x80000000)]+[(rng.randrange(1<<32),rng.randrange(1,1<<32)) for _ in range(300)]
            p=subprocess.run([str(exe)],input=''.join(f'{n} {d}\n' for n,d in cases),capture_output=True,text=True)
            self.assertEqual(p.returncode,0,p.stderr);self.assertEqual(list(map(int,p.stdout.split())),[n//d for n,d in cases])
            for value in ['0','75','1000000','999999999']:
                p=subprocess.run([str(exe),value],capture_output=True,text=True);self.assertEqual((p.returncode,p.stdout),(0,value+'\n'))
            for value in ['', '-1','+2','1e5','0.5',' 4','1000000000','NaN','1;reset']:
                self.assertEqual(subprocess.run([str(exe),value],capture_output=True).returncode,2)
if __name__=='__main__':unittest.main()
