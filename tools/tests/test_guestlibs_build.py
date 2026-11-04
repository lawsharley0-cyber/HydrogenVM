"""Tests for tools/guestlibs/build_guestlibs.py and docs/research/guestlib_licensing.md (S3)."""

from __future__ import annotations

from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools" / "guestlibs"))

MH_MAGIC = 0xFEEDFACE
CPU_TYPE_ARM = 12
CPU_SUBTYPE_ARM_V7 = 9
MH_DYLIB = 6
LC_SYMTAB = 0x2
LC_LOAD_DYLIB = 0xC
LC_ID_DYLIB = 0xD


def _inspect_macho(data: bytes) -> dict[str, object]:
    magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags = struct.unpack_from(
        "<IIIIIII", data, 0
    )
    cpusubtype &= 0x00FFFFFF
    install_name = ""
    load_dylibs: list[str] = []
    exports: set[str] = set()
    off = 28
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", data, off)
        if cmd in (LC_ID_DYLIB, LC_LOAD_DYLIB):
            str_off = struct.unpack_from("<I", data, off + 8)[0]
            raw = data[off + str_off : off + cmdsize]
            name = raw.split(b"\x00", 1)[0].decode("utf-8", errors="replace")
            if cmd == LC_ID_DYLIB:
                install_name = name
            else:
                load_dylibs.append(name)
        elif cmd == LC_SYMTAB:
            symoff, nsyms, stroff, strsize = struct.unpack_from("<IIII", data, off + 8)
            for i in range(nsyms):
                n_strx, n_type, n_sect, n_desc, n_value = struct.unpack_from(
                    "<IBBHI", data, symoff + i * 12
                )
                if (n_type & 0xE0) == 0 and (n_type & 0x0E) == 0x0E and (n_type & 0x01):
                    end = data.find(b"\x00", stroff + n_strx, stroff + strsize)
                    if end != -1:
                        exports.add(data[stroff + n_strx : end].decode("utf-8", errors="replace"))
        off += cmdsize
    return {
        "magic": magic,
        "cputype": cputype,
        "cpusubtype": cpusubtype,
        "filetype": filetype,
        "flags": flags,
        "install_name": install_name,
        "load_dylibs": load_dylibs,
        "exports": exports,
    }


class GuestLibsBuildTests(unittest.TestCase):
    def test_pinned_sources_and_flags_manifest(self) -> None:
        import build_guestlibs

        self.assertIn("target1_gcc", build_guestlibs.PINNED_SOURCES)
        self.assertIn("target2_llvm", build_guestlibs.PINNED_SOURCES)
        for group in build_guestlibs.PINNED_SOURCES.values():
            for item in group:
                self.assertTrue(item["url"].startswith("https://"))
                self.assertEqual(len(item["sha256"]), 64)
                self.assertTrue(item["license"])
        self.assertIn("-arch armv7", " ".join(build_guestlibs.DOCUMENTED_CFLAGS))
        self.assertIn("-miphoneos-version-min=6.0", " ".join(build_guestlibs.DOCUMENTED_CFLAGS))

    def test_build_guestlibs_produces_valid_armv7_mh_dylibs_outside_repo(self) -> None:
        import build_guestlibs

        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp) / "out"
            manifest = build_guestlibs.build_all(out_dir)
            expected = {
                "libgcc_s.1.dylib": {
                    "install_name": "/usr/lib/libgcc_s.1.dylib",
                    "deps": ["/usr/lib/libSystem.B.dylib"],
                    "must_export": ["__Unwind_SjLj_Register", "__Unwind_SjLj_RaiseException", "___divsi3"],
                },
                "libstdc++.6.dylib": {
                    "install_name": "/usr/lib/libstdc++.6.dylib",
                    "deps": ["/usr/lib/libgcc_s.1.dylib", "/usr/lib/libSystem.B.dylib"],
                    "must_export": ["__Znwm", "__ZdlPv", "___cxa_throw", "__ZNSsC1EPKcRKSaIcE"],
                },
                "libc++abi.dylib": {
                    "install_name": "/usr/lib/libc++abi.dylib",
                    "deps": ["/usr/lib/libSystem.B.dylib"],
                    "must_export": ["___cxa_allocate_exception", "___cxa_throw", "___cxa_guard_acquire", "___gxx_personality_sj0"],
                },
                "libc++.1.dylib": {
                    "install_name": "/usr/lib/libc++.1.dylib",
                    "deps": ["/usr/lib/libc++abi.dylib", "/usr/lib/libSystem.B.dylib"],
                    "must_export": [
                        "__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEC1EPKc",
                        "__ZNSt3__15mutex4lockEv",
                        "__ZNSt3__16chrono12system_clock3nowEv",
                        "__ZNSt3__19to_stringEi",
                    ],
                },
            }
            for filename, spec in expected.items():
                p = out_dir / filename
                self.assertTrue(p.is_file(), f"missing built dylib {filename}")
                info = _inspect_macho(p.read_bytes())
                self.assertEqual(info["magic"], MH_MAGIC)
                self.assertEqual(info["cputype"], CPU_TYPE_ARM)
                self.assertEqual(info["cpusubtype"], CPU_SUBTYPE_ARM_V7)
                self.assertEqual(info["filetype"], MH_DYLIB)
                self.assertEqual(info["install_name"], spec["install_name"])
                for dep in spec["deps"]:
                    self.assertIn(dep, info["load_dylibs"])
                for sym in spec["must_export"]:
                    self.assertIn(sym, info["exports"], f"{filename} missing export {sym}")
                self.assertIn(filename, manifest["libraries"])

            status = subprocess.run(
                ["git", "-C", str(REPO_ROOT), "status", "--porcelain"],
                capture_output=True,
                text=True,
                check=True,
            ).stdout
            for line in status.splitlines():
                self.assertNotIn(".dylib", line)

    def test_licensing_doc_exists_and_covers_both_runtimes(self) -> None:
        doc = REPO_ROOT / "docs" / "research" / "guestlib_licensing.md"
        self.assertTrue(doc.is_file(), "missing docs/research/guestlib_licensing.md")
        text = doc.read_text(encoding="utf-8")
        self.assertIn("GPLv3", text)
        self.assertIn("GCC Runtime Library Exception", text)
        self.assertIn("Apache-2.0 WITH LLVM-exception", text)
        self.assertIn("libstdc++.6.dylib", text)
        self.assertIn("libc++.1.dylib", text)


if __name__ == "__main__":
    unittest.main()
