import hashlib
from pathlib import Path
import struct
import sys
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from dex_asm import AssemblyError, assemble, mutf8, sleb, uleb
from android_gaps import parse_dex


class DexAssemblerTest(unittest.TestCase):
    def test_fixtures_are_reproducible_and_valid(self):
        fixtures = ROOT / "tests/android/java"
        for name, classes, methods in (("classes", 2, 4), ("classes2", 1, 1), ("arithmetic", 1, 12), ("objects", 1, 7),
                                       ("linkage-base", 2, 5), ("linkage-child", 4, 33), ("jni", 4, 38),
                                       ("native-driver", 1, 1)):
            with self.subTest(fixture=name):
                data = assemble((fixtures / f"{name}.smali").read_text())
                self.assertEqual(data, (fixtures / f"{name}.dex").read_bytes())
                self.assertEqual(data[:8], b"dex\n035\0")
                self.assertEqual(data[12:32], hashlib.sha1(data[32:]).digest())
                self.assertEqual(struct.unpack_from("<I", data, 8)[0], zlib.adler32(data[12:]))
                dex = parse_dex(data)
                self.assertEqual((dex.classes, dex.methods), (classes, methods))

    def test_reference_encodings(self):
        self.assertEqual(mutf8("A\0é😀"), bytes.fromhex("05 41 c0 80 c3 a9 ed a0 bd ed b8 80 00"))
        self.assertEqual(uleb(624485), bytes.fromhex("e5 8e 26"))
        self.assertEqual(sleb(-624485), bytes.fromhex("9b f1 59"))

    def test_registers_and_branch_errors(self):
        start = ".class Ltest/C; 1\n.method f()I 9\n.registers 1\n"
        for text, diagnostic in (("const/4 v1, 1", "register"),
                                 ("goto :missing", "unknown label"),
                                 ("bad-instruction", "unknown opcode")):
            with self.subTest(text=text):
                with self.assertRaisesRegex(AssemblyError, diagnostic):
                    assemble(start + text + "\n.end method\n")

    def test_native_method_has_no_code(self):
        image = assemble(".class Ltest/C; 1\n.method n(J)I 0x109\n.end method\n")
        self.assertEqual(parse_dex(image).natives, [("test/C", "n", "(J)I")])
