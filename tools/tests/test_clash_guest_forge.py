"""FGR1 persistence, strict UTF-8, CSV quoting and mandatory save-prefix guards."""
import csv
import ctypes
import io
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tools'))
from clash_custom_cards import encode_registry, decode_registry, fnv, guest_header, generate_overlays, FORGE_TABLES
import test_clash_guest_custom_cards as fixtures


class ForgeTests(unittest.TestCase):
    def registry(self, name='Forge Knight'):
        return dict(version=1, cards=[dict(slot=0, baseCardId=26000000, name=name)])

    def test_registry_roundtrip_and_corrupt_extents_padding_names(self):
        registry = self.registry('勇敢な, "Knight" 👩🏽\u200d🚀')
        data = encode_registry(registry)
        self.assertEqual(len(data), 128)
        self.assertEqual(decode_registry(data), registry)
        for broken in [data[:19], data[:-1], data+b'\0', bytes(4)+data[4:], data[:25]+b'!'+data[26:]]:
            with self.subTest(size=len(broken)), self.assertRaises(ValueError): decode_registry(broken)
        for offset, value in [(24, 97), (28, 0xff), (127, 1)]:
            bad = bytearray(data);bad[offset] = value
            struct.pack_into('<I', bad, 12, fnv(bad[20:]));struct.pack_into('<I', bad, 16, fnv(bad[:16]))
            with self.subTest(offset=offset), self.assertRaises(ValueError): decode_registry(bad)
        with self.assertRaises(ValueError): encode_registry(dict(version=1, cards=[]))
        with self.assertRaises(ValueError): encode_registry(self.registry('Reassigned'), self.registry())

    @classmethod
    def setUpClass(cls):
        if not shutil.which('clang'):
            raise unittest.SkipTest('C compiler unavailable')
        cls.temp = tempfile.TemporaryDirectory();root = Path(cls.temp.name)
        (root/'format.c').write_text('''
#include <stdint.h>
typedef uint32_t u32;typedef unsigned char u8;
static u32 checksum(const u8 *p,u32 n){u32 h=2166136261u;while(n--)h=(h^*p++)*16777619u;return h;}
#include "forge_format.inc"
int valid_name(const char *p,u32 n){return forge_name_valid(p,n);}
int valid_record(const void *p){return forge_record_valid(p);}
u32 key_slot(const char *p){return forge_key_slot(p);}
int valid_link(const void *p,u32 save,const void *records,u32 count){return forge_link_valid(p,save,records,count);}
int expand(char *out,u32 cap,const char *text,const void *record,u32 slot,u32 *size){return forge_expand(out,cap,text,record,slot,size);}
''')
        result = subprocess.run(['clang','-shared','-fPIC','-std=c11','-O2','-I'+str(ROOT/'tools/clash_guest'),str(root/'format.c'),'-o',str(root/'format.so')],capture_output=True,text=True)
        if result.returncode: raise RuntimeError(result.stderr)
        cls.lib = ctypes.CDLL(str(root/'format.so'))
        cls.lib.valid_name.argtypes = [ctypes.c_char_p,ctypes.c_uint32]
        cls.lib.valid_record.argtypes = [ctypes.c_void_p]
        cls.lib.key_slot.argtypes = [ctypes.c_char_p]
        cls.lib.valid_link.argtypes = [ctypes.c_void_p,ctypes.c_uint32,ctypes.c_void_p,ctypes.c_uint32]
        cls.lib.expand.argtypes = [ctypes.c_void_p,ctypes.c_uint32,ctypes.c_char_p,ctypes.c_void_p,ctypes.c_uint32,ctypes.POINTER(ctypes.c_uint32)]

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_guest_utf8_and_padding_reject_malformed_records(self):
        for value in ['Knight','Ace, "Arrow"','勇敢な 👩🏽\u200d🚀','a'*32]:
            data = value.encode();self.assertEqual(self.lib.valid_name(data,len(data)),1)
        for data in [b'',b'   ',b'a'*33,b'a\0b',b'\xc0\x80',b'\xed\xa0\x80',b'\xf4\x90\x80\x80',b'\xe2\x80',b'a\x7fb', '\u2028'.encode(), '\x85'.encode(), '\u3000'.encode()]:
            self.assertEqual(self.lib.valid_name(data,len(data)),0,repr(data))
        body = encode_registry(self.registry())[20:]
        self.assertEqual(self.lib.valid_record(body),1)
        self.assertEqual(self.lib.valid_record(body[:-1]+b'x'),0)

    def test_required_prefix_accepts_append_but_rejects_missing_or_reassigned(self):
        old = self.registry();new = dict(version=1,cards=old['cards']+[dict(slot=1,baseCardId=27000000,name='Cannon')])
        body = encode_registry(old)[20:];longer = encode_registry(new)[20:]
        def link(count=1, digest=None, save=123):
            data=struct.pack('<4I',0x31434746,count,fnv(body) if digest is None else digest,save)
            return data+struct.pack('<I',fnv(data))
        self.assertEqual(self.lib.valid_link(link(),123,longer,2),1)
        for header, save, records, count in [(link(),124,longer,2),(link(),123,b'',0),(link(0),123,longer,2),(link(17),123,longer,2),(link(digest=0),123,longer,2),(link(),123,bytes(len(longer)),2),(link()[:-1]+b'\0',123,longer,2)]:
            self.assertEqual(self.lib.valid_link(header,save,records,count),0)

    def test_guest_csv_expansion_quotes_names_and_keeps_slot_references(self):
        registry=self.registry('Ace, "@UNIT@"')
        record=encode_registry(registry)[20:];out=ctypes.create_string_buffer(4096);size=ctypes.c_uint32()
        text=b'@CARD@,@UNIT@,@SHOT@,@TID@,@NAME@\r\n'
        self.assertEqual(self.lib.expand(out,4096,text,record,15,ctypes.byref(size)),1)
        row=next(csv.reader(io.StringIO(out.raw[:size.value].decode())))
        self.assertEqual(row,['ModLabCard_0015','ModLabUnit_0015','ModLabShot_0015','TID_MODLAB_0015','Ace, "@UNIT@"'])
        for template,cap,slot in [(text,8,0),(b'@UNKNOWN@',4096,0),(b'@UNIT',4096,0),(text,4096,16)]:
            self.assertEqual(self.lib.expand(out,cap,template,record,slot,ctypes.byref(size)),0)
        for slot in range(16):
            self.assertEqual(self.lib.key_slot(f'TID_MODLAB_{slot:04d}'.encode()),slot)
        for key in [b'',b'TID_MODLAB_',b'TID_MODLAB_000',b'TID_MODLAB_0016',b'TID_MODLAB_0000x',b'TID_MODLAB_00-1',b'TID_KNIGHT']:
            self.assertEqual(self.lib.key_slot(key),16)

    def test_private_templates_derive_from_independent_csv_reference(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);fixtures.CustomDefinitionTests().fixtures(root)
            entries=[dict(cardId=26000000),dict(cardId=27000000)]
            header=guest_header(root,entries)
            self.assertIn('@NAME@',header);self.assertIn('@UNIT@',header)
            self.assertNotIn('FORGE_NAME_TOKEN',header)
            self.assertEqual(len(FORGE_TABLES),6)
            # Concrete generator oracle: cloned Cannon points at a distinct shot,
            # with original data rows unmodified and a new text ID.
            reg=dict(version=1,cards=[dict(slot=0,baseCardId=27000000,name='Cannon')])
            overlays,meta=generate_overlays(root,reg)
            self.assertEqual(meta['cards'][0]['cardId'],27000001)
            self.assertIn(b'ModLabShot_0000',overlays['res/csv_logic/projectiles.csv'])


if __name__=='__main__':unittest.main()
