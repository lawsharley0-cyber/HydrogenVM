"""Exercise binary/package scanning without a cross compiler or private games."""

from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "libscan"))
import scan


# ARMv7 A32 instructions with a relocated B immediate.
FUNCTION = bytes.fromhex("10402de9000090e5000000ea1080bde8")
MASK = bytes.fromhex("ffffffffffffffff000000ffffffffff")
SIGNATURE = scan.Signature("fixture", "example", ("1.0", "1.1"), "arm",
                           bytes(b & m for b, m in zip(FUNCTION, MASK)), MASK)
CODE = bytes.fromhex("0000a0e1") + FUNCTION[:8] + bytes.fromhex("123456ea") + FUNCTION[12:]


def elf(code, text_flags=6, address=0x2000):
    ident = b"\x7fELF\x01\x01\x01" + bytes(9)
    header = struct.pack("<HHIIIIIHHHHHH", 3, 40, 1, 0, 0, 52, 0, 52, 0, 0, 40, 2, 0)
    section = struct.pack("<10I", 0, 1, text_flags, address, 132, len(code), 0, 0, 4, 0)
    return ident + header + bytes(40) + section + code


def macho(code, flags=0x80000400):
    index = 28 + 56 + 68
    header = struct.pack("<7I", 0xFEEDFACE, 12, 9, 2, 1, index - 28, 0)
    segment = struct.pack("<II16s8I", 1, index - 28, b"__TEXT", 0x1000, 0x1000, 0, index + len(code), 7, 5, 1, 0)
    section = struct.pack("<16s16s9I", b"__text", b"__TEXT", 0x1100, len(code), index, 2, 0, 0, flags, 0, 0)
    return header + segment + section + code


def fat(image):
    return struct.pack(">7I", 0xCAFEBABE, 1, 12, 9, 28, len(image), 2) + image


class LibraryScanTests(unittest.TestCase):
    def test_arm_signatures_ignore_branch_displacement_and_keep_alignment(self):
        results = scan.scan_binary(elf(CODE), [SIGNATURE])
        self.assertEqual(results, [{"library": "fixture", "function": "example", "address": 0x2004,
                                    "isa": "arm", "confidence": "high", "version_guess": ["1.0", "1.1"]}])
        self.assertEqual(scan.scan_binary(elf(b"\0" + CODE), [SIGNATURE]), [])
        self.assertEqual(scan.scan_binary(elf(CODE[:-1] + b"\x00"), [SIGNATURE]), [])
        self.assertEqual(scan.scan_binary(elf(CODE, text_flags=2), [SIGNATURE]), [])

    def test_tail_wrapper_requires_matching_destination(self):
        internal = scan.Signature("fixture", "internal", ("1.0",), "arm", SIGNATURE.value, MASK, internal=True)
        wrapper = scan.Signature("fixture", "wrapper", ("1.0",), "arm",
                                 bytes.fromhex("0120a0e3000000ea"), bytes.fromhex("ffffffff000000ff"),
                                 target="internal")
        # b at 0x2004 uses PC 0x200c and points to the function at 0x2010.
        prefix = bytes.fromhex("0120a0e3010000ea0000000000000000")
        hits = scan.scan_binary(elf(prefix + FUNCTION), [internal, wrapper])
        self.assertEqual([(hit["function"], hit["address"]) for hit in hits], [("wrapper", 0x2000)])
        bad_branch = prefix[:4] + bytes.fromhex("020000ea") + prefix[8:]
        self.assertEqual(scan.scan_binary(elf(bad_branch + FUNCTION), [internal, wrapper]), [])

    def test_thin_and_fat_macho_report_guest_virtual_address(self):
        for data in [macho(CODE), fat(macho(CODE))]:
            self.assertEqual([hit["address"] for hit in scan.scan_binary(data, [SIGNATURE])], [0x1104])
        self.assertEqual(scan.scan_binary(macho(CODE, flags=0), [SIGNATURE]), [])
        with self.assertRaises(ValueError):
            scan.scan_binary(macho(CODE)[:-1], [SIGNATURE])

    def test_package_selection_and_reports(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            ipa, apk, app = (root / name for name in ("fixture.ipa", "fixture.apk", "Fixture.app"))
            app.mkdir()
            (app / "Fixture").write_bytes(macho(CODE))
            (app / "Notes.txt").write_text("not executable")
            with zipfile.ZipFile(ipa, "w") as archive:
                archive.writestr("Payload/Fixture.app/Fixture", macho(CODE))
                archive.writestr("Outside/Fixture.app/Fixture", macho(CODE))
            with zipfile.ZipFile(apk, "w") as archive:
                archive.writestr("lib/armeabi-v7a/libtest.so", elf(CODE))
                archive.writestr("lib/arm64-v8a/libtest.so", elf(CODE))
                archive.writestr("assets/libtest.so", elf(CODE))
            for path, expected in [(ipa, "Payload/Fixture.app/Fixture"), (apk, "lib/armeabi-v7a/libtest.so"),
                                   (app, "Fixture")]:
                self.assertEqual([path for path, _ in scan.package_executables(path)], [expected])

    def test_committed_database_is_bounded_and_covers_targets(self):
        path = Path(scan.__file__).with_name("signatures.json")
        signatures = scan.load_signatures(path)
        functions = {sig.function for sig in signatures}
        self.assertTrue({"crc32", "adler32", "inflate", "inflate_fast", "mdct_backward", "vorbis_synthesis",
                         "png_read_filter_row_up", "png_read_filter_row_sub", "png_read_filter_row_avg",
                         "png_read_filter_row_paeth_1byte_pixel", "png_read_filter_row_paeth_multibyte_pixel"} <= functions)
        self.assertEqual({sig.isa for sig in signatures}, {"arm", "thumb"})
        for sig in signatures:
            self.assertEqual(len(sig.value), len(sig.mask))
            if not sig.target:
                self.assertGreaterEqual(sum(m.bit_count() for m in sig.mask), 80)
            self.assertTrue(all(value & mask == value for value, mask in zip(sig.value, sig.mask)))


if __name__ == "__main__":
    unittest.main()
