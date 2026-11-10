"""Authored SC fixtures and the actual guest art-pack validator; no game assets."""
from pathlib import Path
import hashlib,struct,subprocess,shutil,tempfile,unittest,sys
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'tools'))
from clash_custom_art import *
from clash_custom_cards import encode_registry

class ArtTests(unittest.TestCase):
    def test_sc_compression_extents_digest_and_primitive_geometry(self):
        raw=encode_metadata([1,1,1,0,0,0],bytes(5),[(1,b'Authored')],[(26,b''),(1,struct.pack('<BHH',0,2,2)),(18,bitmap_shape(0,0,64,80,feet=True)),(10,static_clip(1,0))])
        self.assertEqual(unpack_sc(pack_sc(raw)),raw)
        counts,padding,exports,tags=parse_metadata(raw)
        self.assertEqual(counts,[1,1,1,0,0,0]);self.assertEqual(exports,[(1,b'Authored')])
        shape=next(p for k,p in tags if k==18)
        self.assertEqual(struct.unpack_from('<HHH',shape),(0,1,4))
        self.assertEqual(struct.unpack_from('<8i',shape,13),(-640,-1600,640,-1600,640,0,-640,0))
        for bad in [b'',pack_sc(raw)[:-1],pack_sc(raw)+b'x']:
            with self.assertRaises((ValueError,lzma.LZMAError)):unpack_sc(bad)
        b=bytearray(pack_sc(raw));b[10]^=1
        with self.assertRaisesRegex(ValueError,'digest'):unpack_sc(bytes(b))
        with self.assertRaises(ValueError):parse_metadata(raw+b'x')

    def fixture(self,root):
        from PIL import Image
        bundle=root/'bundle';(bundle/'res/sc').mkdir(parents=True)
        meta=encode_metadata([0,0,0,0,0,0],bytes(5),[],[(26,b'')])
        (bundle/'res/sc/ui_spells.sc').write_bytes(pack_sc(meta));(bundle/'res/sc/ui_spells_tex.sc').write_bytes(pack_sc(tag(0)))
        image=root/'image.png';Image.new('RGBA',(2,2),(255,0,0,128)).save(image)
        registry=encode_registry(dict(version=1,cards=[dict(slot=0,baseCardId=26000000,name='Authored A'),dict(slot=1,baseCardId=26000001,name='Authored B')]))
        return bundle,image,registry

    def test_image_pack_manifest_binding_exports_and_alpha(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);bundle,image,reg=self.fixture(root)
            manifest,assets=make_pack(bundle,reg,{'0':{'icon':image,'troop':image}})
            h=struct.unpack_from('<8I',manifest);self.assertEqual(h[:6],(MAGIC,1,2,fnv(reg[20:]),source_token(bundle),4))
            self.assertEqual(h[6],fnv(manifest[32:]));self.assertEqual(h[7],fnv(manifest[:28]));self.assertEqual(struct.unpack_from('<2I',manifest,32),(3,0))
            counts,_,exports,_=parse_metadata(unpack_sc(assets[(0,0)]));self.assertEqual(counts[:3],[1,1,1]);self.assertIn((1,b'ModLabIcon_0000'),exports)
            self.assertEqual(pixels(image,2,2),bytes([128,0,0,128])*4)
            for images in [{},{'2':{'icon':image}},{'0':{'arbitrary':image}}]:
                with self.assertRaises(ValueError):make_pack(bundle,reg,images)
            self.assertNotIn((2,1),assets)
            # Same dimensions, changed pixels must change the metadata cache key.
            from PIL import Image
            changed=root/'changed.png';Image.new('RGBA',(2,2),(0,0,255,128)).save(changed)
            _,new=make_pack(bundle,reg,{'0':{'icon':changed,'troop':changed}})
            self.assertNotEqual(assets[(0,0)][10:26],new[(0,0)][10:26])
            self.assertNotEqual(assets[(2,0)][10:26],new[(2,0)][10:26])

    @unittest.skipUnless(shutil.which('clang'),'C compiler unavailable')
    def test_guest_validator_rejects_incomplete_modified_wrong_source_and_wrong_registry_packs(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);bundle,image,reg=self.fixture(root);home=root/'home';folder=home/'Documents/ClashForgeArt-v1';folder.mkdir(parents=True)
            manifest,assets=make_pack(bundle,reg,{'0':{'icon':image,'troop':image}})
            rp=root/'registry.bin';rp.write_bytes(reg)
            for (kind,slot),data in assets.items():(folder/asset_name(kind,slot)).write_bytes(data)
            mp=folder/'manifest.bin';mp.write_bytes(manifest)
            code=r"""
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
typedef unsigned int u32;typedef unsigned char u8;
#define FORGE_MAX_CARDS 16
#define API_puts puts
#define API_printf printf
#define CALL(address,result,args) ((result (*)args)(address))
#define f_open fopen
#define f_read fread
#define f_close fclose
#define f_error ferror
struct ForgeRecord {u32 base,bytes;char name[100];};
static struct ForgeRecord forge_records[16];static u32 forge_loaded_count=2;
static char *home;
static const char *test_getenv(const char *key){return home;}
static uintptr_t symbol(const char *key){return (uintptr_t)test_getenv;}
static u32 checksum(const u8 *p,u32 n){u32 h=2166136261u;while(n--)h=(h^*p++)*16777619u;return h;}
static void path_append(char *out,const char *a,const char *b,u32 cap){if(snprintf(out,cap,"%s%s",a,b)>=(int)cap)exit(4);}
static int forge_equal(const char *a,const char *b){return !strcmp(a,b);}
static void forge_identifier(char *out,const char *p,u32 slot){snprintf(out,24,"%s%04u",p,slot);}
"""+f'\n#define GUEST_ART_SOURCE_TOKEN {source_token(bundle)}u\n'+r"""
#include "forge_art.inc"
int main(int argc,char **argv){
 home=argv[1];FILE *f=fopen(argv[2],"rb");fseek(f,20,SEEK_SET);fread(forge_records,108,2,f);fclose(f);
 art_initialize();
 if(art_valid){int handled=0;FILE *asset=art_redirect("/bundle/res/sc/ui_spells.sc","rb",&handled);if(!asset||!handled)return 4;fclose(asset);
  handled=0;if(art_redirect("/home/Library/Caches/swfcache/sc/ui_spells.sc","rb",&handled)||handled)return 5;
  if(art_redirect("/bundle/notres/sc/ui_spells.sc","rb",&handled)||handled)return 6;
 }
 printf("%u %u %u\n",art_valid,art_flags[0],art_flags[1]);return 0;
}
"""
            src=root/'art.c';exe=root/'art';src.write_text(code)
            p=subprocess.run(['clang','-std=c11','-O2','-I'+str(ROOT/'tools/clash_guest'),str(src),'-o',str(exe)],capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr)
            def run(expected):
                p=subprocess.run([str(exe),str(home),str(rp)],capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr);self.assertTrue(p.stdout.endswith(expected),p.stdout)
            run('1 3 0\n')
            original_mp=manifest
            for n in (0,4,31,32,len(manifest)-1):mp.write_bytes(original_mp[:n]);run('0 0 0\n');self.assertEqual(mp.read_bytes(),original_mp[:n])
            mp.write_bytes(original_mp)
            target=folder/'ModLabArt_0000.sc';data=target.read_bytes();target.write_bytes(data[:-1]);run('0 0 0\n');self.assertEqual(target.read_bytes(),data[:-1]);target.write_bytes(data)
            modified=bytearray(reg);modified[30]^=1;rp.write_bytes(modified);run('0 0 0\n');rp.write_bytes(reg)
            def rehash(header,body):
                header[6]=fnv(body);header[7]=fnv(struct.pack('<7I',*header[:7]));return struct.pack('<8I',*header)+body
            h=list(struct.unpack_from('<8I',manifest));h[4]^=1;mp.write_bytes(rehash(h,manifest[32:]));run('0 0 0\n')
            h=list(struct.unpack_from('<8I',manifest));body=bytearray(manifest[32:]);struct.pack_into('<I',body,0,8);mp.write_bytes(rehash(h,body));run('0 0 0\n')
            body=bytearray(manifest[32:]);body[8+16:8+32]=body[8:8+16];mp.write_bytes(rehash(h,body));run('0 0 0\n')
            # An append-only registry extension still accepts a pack for slot zero.
            one=encode_registry(dict(version=1,cards=[dict(slot=0,baseCardId=26000000,name='Authored A')]))
            m,_=make_pack(bundle,one,{'0':{'icon':image,'troop':image}});mp.write_bytes(m);run('1 3 0\n')

if __name__=='__main__':unittest.main()
