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


def anchored_macho():
    """ARMv6 fixture with three entry points, PC-relative loads and a direct call."""
    diagnostics = [
        b"incorrect header check\0", b"unknown compression method\0",
        b"invalid distance code\0", b"invalid literal/length code\0",
        struct.pack("<8I", 0, 0x77073096, 0xEE0E612C, 0x990951BA,
                    0x076DC419, 0x706AF48F, 0xE963A535, 0x9E6495A3),
    ]
    rodata_address = 0x2000
    constants = b""
    addresses = []
    for item in diagnostics:
        addresses.append(rodata_address + len(constants))
        constants += item
    # At 0x1104 the PC is 0x110c and the first literal is at 0x1118.
    # The BL instructions at 0x110c and 0x1110 target 0x1120 and 0x1140.
    code = struct.pack(
        "<24I", 0xE92D4010, 0xE59F000C, 0xE59F100C, 0xEB000003, 0xEB00000A, 0xE8BD8010,
        addresses[0], addresses[1],
        0xE92D4010, 0xE59F000C, 0xE59F100C, 0xE8BD8010, 0, 0, addresses[2], addresses[3],
        0xE92D4010, 0xE59F0008, 0xE0200421, 0xE8BD8010, 0, addresses[4], 0, 0,
    )
    segment_size = 56 + 2 * 68
    offset = 28 + segment_size
    header = struct.pack("<7I", 0xFEEDFACE, 12, 6, 2, 1, segment_size, 0)
    segment = struct.pack("<II16s8I", 1, segment_size, b"__TEXT", 0x1000, 0x2000, 0,
                          offset + len(code) + len(constants), 7, 5, 2, 0)
    text = struct.pack("<16s16s9I", b"__text", b"__TEXT", 0x1100, len(code), offset,
                       2, 0, 0, 0x80000400, 0, 0)
    data = struct.pack("<16s16s9I", b"__const", b"__TEXT", rodata_address, len(constants), offset + len(code),
                       2, 0, 0, 0, 0, 0)
    return header + segment + text + data + code + constants, offset


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

    def test_armv6_literal_references_and_direct_calls(self):
        image, offset = anchored_macho()
        hits = scan.identify_libraries(image, [])
        self.assertEqual(
            {hit["function"]: (hit["address"], hit["isa"], hit["evidence"]) for hit in hits},
            {"inflate": (0x1100, "arm", "constant-references"),
             "inflate_fast": (0x1120, "arm", "constant-references"),
             "crc32": (0x1140, "arm", "constant-references")},
        )
        # Strings still present but neither header diagnostic is referenced.
        unreferenced = image[:offset + 4] + bytes(8) + image[offset + 12:]
        self.assertEqual(scan.identify_libraries(unreferenced, []), [])
        # Drop the call but leave the other routine and its error strings in place.
        disconnected = image[:offset + 12] + bytes(4) + image[offset + 16:]
        self.assertEqual({hit["function"] for hit in scan.identify_libraries(disconnected, [])},
                         {"inflate", "crc32"})
        without_crc_loop = image[:offset + 0x48] + bytes(4) + image[offset + 0x4C:]
        self.assertEqual({hit["function"] for hit in scan.identify_libraries(without_crc_loop, [])},
                         {"inflate", "inflate_fast"})

    def test_macho_symbols_require_a_defined_text_entry(self):
        image = macho(CODE)
        text_at = len(image) - len(CODE) + 24
        names = b"\0_inflate\0_crc32\0"
        symbols = (struct.pack("<IBBHI", 1, 0x0E, 1, 0, 0x1104) +
                   struct.pack("<IBBHI", 10, 0x01, 0, 0, 0))
        symtab = struct.pack("<6I", 2, 24, text_at + len(CODE), 2,
                             text_at + len(CODE) + len(symbols), len(names))
        data = bytearray(image[:len(image) - len(CODE)] + symtab + CODE + symbols + names)
        struct.pack_into("<2I", data, 16, 2, 56 + 68 + 24)
        struct.pack_into("<I", data, 28 + 36, len(data))
        struct.pack_into("<I", data, 28 + 56 + 40, text_at)
        hits = scan.identify_libraries(bytes(data), [])
        self.assertEqual([(h["function"], h["address"], h["evidence"]) for h in hits],
                         [("inflate", 0x1104, "defined-symbol")])

    def test_thumb_movw_movt_reference_uses_function_start_mode(self):
        from legacy import Text, function_starts, anchor_hits
        starts = function_starts(bytes.fromhex("810200"), 0x1000)  # ULEB128 delta 0x101
        # push {lr}; movw r0, #0x2000; movt r0, #0;
        # movw r1, #0x2040; movt r1, #0; pop {pc}
        text = Text(0x1100, bytes.fromhex("00b542f20000c0f2000042f24001c0f2000100bd"))
        anchors = {0x2000: {"png_header"}, 0x2040: {"png_order"}}
        hits = anchor_hits(text, starts, anchors)
        self.assertEqual([(h["function"], h["address"], h["isa"]) for h in hits],
                         [("png_read_info", 0x1100, "thumb")])
        self.assertEqual(anchor_hits(text, {0x1100: "arm"}, anchors), [])

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
