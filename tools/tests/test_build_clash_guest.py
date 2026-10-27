"""Authored Mach-O vectors; no Clash executable, assets or saved player data."""
import struct
import sys
from pathlib import Path
import tempfile
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from build_clash_guest import macho, build, thumb_branch


def image(binding=False):
    result=bytearray(4096)
    section=struct.pack('<16s16s9I',b'__symbol_stub4',b'__TEXT',
                        0x800200,16,0x200,2,0,0,8,0,16)
    segment=struct.pack('<II16s8I',1,124,b'__TEXT',0x800000,4096,0,4096,7,5,1,0)+section
    symbols=struct.pack('<6I',2,24,0x240,2,0x280,24)
    dynamic=[11,80]+[0]*18
    dynamic[14]=0x230;dynamic[15]=1
    commands=segment+symbols+struct.pack('<20I',*dynamic)
    if binding:
        values=[0x80000022,48]+[0]*10
        values[5]=8
        commands+=struct.pack('<12I',*values)
    struct.pack_into('<7I',result,0,0xfeedface,12,9,2,3+binding,len(commands),0)
    result[28:28+len(commands)]=commands
    struct.pack_into('<I',result,0x230,0)
    struct.pack_into('<IBBHI',result,0x240,1,1,0,0,0)
    struct.pack_into('<IBBHI',result,0x24c,7,15,1,8,0x800300)
    strings=b'\0_puts\0_guest_connect\0'
    result[0x280:0x280+len(strings)]=strings
    return result


class GuestBuilderTests(unittest.TestCase):
    def test_four_byte_thumb_getter_branch(self):
        # Independent LLVM assembler B.W vectors: +4092 and -4100 offsets.
        self.assertEqual(thumb_branch(0x1000,0x2001),bytes.fromhex('00f0febf'))
        self.assertEqual(thumb_branch(0x2000,0x1001),bytes.fromhex('fef7febf'))
        self.assertEqual(thumb_branch(0x1000,0x2001,link=True),bytes.fromhex('00f0feff'))
        self.assertEqual(thumb_branch(0x2000,0x1001,link=True),bytes.fromhex('fef7feff'))
        for source,target in [(0x1001,0x2001),(0x1000,0x2000),(0,0x1000005)]:
            with self.assertRaises(ValueError):thumb_branch(source,target)

    def test_indirect_stub_and_thumb_symbol(self):
        parsed=macho(image())
        self.assertEqual(parsed['stubs'],{'_puts':0x800200})
        self.assertEqual(parsed['symbols']['_guest_connect'],0x800301)
        self.assertFalse(parsed.get('hasBindings',False))

    def test_new_dynamic_dependencies_detected(self):
        self.assertTrue(macho(image(binding=True))['hasBindings'])

    def test_wrong_cpu_and_truncated_commands_rejected(self):
        wrong=image();struct.pack_into('<I',wrong,4,7)
        for value in (wrong,image()[:100],b''):
            with self.assertRaises(ValueError):macho(value)

    def test_wrong_source_rejected_without_creating_output(self):
        with tempfile.TemporaryDirectory() as root:
            root=Path(root);source=root/'source.ipa';source.write_bytes(b'authored wrong input')
            with self.assertRaisesRegex(ValueError,'Pinned source'):
                build(source,root/'out.ipa',root/'build')
            self.assertFalse((root/'out.ipa').exists())
            self.assertFalse((root/'build').exists())

    def test_existing_output_is_preserved(self):
        with tempfile.TemporaryDirectory() as root:
            root=Path(root);output=root/'out.ipa';output.write_bytes(b'keep existing')
            with self.assertRaisesRegex(ValueError,'existing data'):
                build(root/'missing',output,root/'build')
            self.assertEqual(output.read_bytes(),b'keep existing')


if __name__=='__main__':unittest.main()
