"""Decode guest-generated SC with the independent Python format reader."""
import ctypes
from pathlib import Path
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
from clash_custom_art import bitmap_shape, encode_metadata, parse_metadata, static_clip, tag, texture
HARNESS = r'''
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <openssl/md5.h>
typedef unsigned int u32; typedef unsigned char u8;
#define CALL(a,r,args) ((r (*)args)(uintptr_t)(a))
#define API_malloc malloc
#define f_free free
#define f_open fopen
#define f_close fclose
#define f_read fread
#define f_write fwrite
#define f_error ferror
static uintptr_t symbol(const char *s){return !strcmp(s,"CC_MD5")?(uintptr_t)MD5:0;}
static int forge_finish(void *f){return !fclose(f);}
static u32 forge_length(const char *s){return strlen(s);}
static void path_append(char *out,const char *a,const char *b,u32 n){snprintf(out,n,"%s%s",a,b);}
static void forge_identifier(char *out,const char *prefix,u32 slot){sprintf(out,"%s%04u",prefix,slot);}
static void art_asset_name(char *out,u32 kind,u32 slot){sprintf(out,"unused%u%u",kind,slot);}
static u32 checksum(const u8 *p,u32 n){u32 h=2166136261u;while(n--)h=(h^*p++)*16777619u;return h;}
#include "photo_assets.inc"
static struct PhotoBytes m,t;
int icon(const void *a,u32 an,const void *b,u32 bn,u32 slot,const u8 *rgba){
 ph_free(&m);ph_free(&t);m=ph_buffer(an);t=ph_buffer(bn);ph_bytes(&m,a,an);ph_bytes(&t,b,bn);return ph_icon(&m,&t,slot,rgba);
}
int troop(u32 slot,const u8 *rgba){ph_free(&m);ph_free(&t);return ph_troop(&m,&t,slot,rgba);}
const void *data(int which){return which?t.data:m.data;}
u32 size(int which){return which?t.size:m.size;}
'''

@unittest.skipUnless(os.name != 'nt' and shutil.which('clang'), 'requires POSIX clang and libcrypto')
class PhotoAssetsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        root = Path(cls.temp.name)
        (root/'assets.c').write_text(HARNESS, encoding='utf-8')
        # Guest function pointers use 32-bit size arguments. Avoid rewriting
        # those indirect calls into 64-bit host libc builtins in the harness.
        compiled=subprocess.run(['clang','-shared','-fPIC','-O2','-fno-builtin','-Wno-deprecated-declarations',
                        '-I'+str(ROOT/'tools/clash_guest'),str(root/'assets.c'),'-lcrypto',
                        '-o',str(root/'assets.so')], capture_output=True,text=True)
        if compiled.returncode: raise RuntimeError(compiled.stderr)
        cls.lib = ctypes.CDLL(str(root/'assets.so'))
        cls.lib.icon.argtypes = [ctypes.c_void_p,ctypes.c_uint,ctypes.c_void_p,ctypes.c_uint,ctypes.c_uint,ctypes.c_void_p]
        cls.lib.troop.argtypes = [ctypes.c_uint,ctypes.c_void_p]
        cls.lib.data.argtypes = [ctypes.c_int]; cls.lib.data.restype = ctypes.c_void_p
        cls.lib.size.argtypes = [ctypes.c_int]; cls.lib.size.restype = ctypes.c_uint

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def result(self):
        return tuple(ctypes.string_at(self.lib.data(i), self.lib.size(i)) for i in (0,1))

    def baseline(self):
        return (encode_metadata([1,1,1,0,0,0],bytes(5),[(1,b'Original')],
                [(1,struct.pack('<BHH',0,2,1)),(18,bitmap_shape(0,0,2,1)),(10,static_clip(1,0))]),
                tag(1,texture(2,1,b'\xff\0\0\xff'*2))+tag(0))

    def test_static_troop_shape_pixels_and_export(self):
        rgba=b'\x10\x20\x30\xff'*(128*160)
        self.assertEqual(self.lib.troop(3,rgba),1)
        m,t=self.result();counts,_,exports,tags=parse_metadata(m)
        self.assertEqual(counts,[1,1,1,0,0,0]);self.assertIn((1,b'ModLabArt_0003'),exports)
        self.assertIn((18,bitmap_shape(0,0,64,80,feet=True)),tags)
        self.assertEqual(t,tag(1,texture(128,160,rgba))+tag(0))

    def test_icon_append_replace_preserves_other_slots_and_original(self):
        m,t=self.baseline();original_texture=t[:-5]
        red=b'\xff\0\0\xff'*(128*162);green=b'\0\xff\0\xff'*(128*162)
        for slot,rgba in [(0,red),(1,red),(0,green)]:
            self.assertEqual(self.lib.icon(m,len(m),t,len(t),slot,rgba),1)
            m,t=self.result()
        counts,_,exports,tags=parse_metadata(m)
        self.assertEqual(counts,[3,3,3,0,0,0])
        self.assertEqual(sum(name.startswith(b'ModLabHash_') for _,name in exports),2)
        self.assertTrue(t.startswith(original_texture))
        self.assertEqual(t,original_texture+tag(1,texture(128,162,green))+tag(1,texture(128,162,red))+tag(0))
        self.assertIn((1,b'Original'),exports)

    def test_truncated_or_invalid_terminal_tags_reject_without_mutation(self):
        m,t=self.baseline();rgba=bytes(128*162*4)
        for bad_m,bad_t in [(m[:-1],t),(m+b'x',t),(m,t[:-5]),(m,t+b'x'),(m[:-5]+tag(0,b'x'),t)]:
            self.assertEqual(self.lib.icon(bad_m,len(bad_m),bad_t,len(bad_t),0,rgba),0)
            self.assertEqual(self.result(),(bad_m,bad_t))

if __name__ == '__main__': unittest.main()
