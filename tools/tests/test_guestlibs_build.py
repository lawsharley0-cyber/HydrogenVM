"""Tests for tools/guestlibs/build_guestlibs.py and docs/research/guestlib_licensing.md (S3)."""

from __future__ import annotations

import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools" / "guestlibs"))

MH_MAGIC = 0xFEEDFACE
CPU_TYPE_ARM = 12
CPU_SUBTYPE_ARM_V7 = 9
MH_DYLIB = 6
LC_SEGMENT = 0x1
LC_SYMTAB = 0x2
LC_LOAD_DYLIB = 0xC
LC_ID_DYLIB = 0xD

FAKE_ZERO_STUB = struct.pack("<II", 0xE3A00000, 0xE12FFF1E)  # mov r0, #0; bx lr


def _inspect_macho(data: bytes) -> dict[str, object]:
    magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags = struct.unpack_from(
        "<IIIIIII", data, 0
    )
    cpusubtype &= 0x00FFFFFF
    install_name = ""
    load_dylibs: list[str] = []
    exports: set[str] = set()
    symbol_info: dict[str, tuple[int, int]] = {}
    sections: list[tuple[str, str, int, int, int]] = []
    off = 28
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", data, off)
        if cmd == LC_SEGMENT:
            nsects = struct.unpack_from("<I", data, off + 48)[0]
            sect_off = off + 56
            for _s in range(nsects):
                sname = (
                    data[sect_off : sect_off + 16]
                    .split(b"\x00", 1)[0]
                    .decode("ascii", errors="replace")
                )
                segname = (
                    data[sect_off + 16 : sect_off + 32]
                    .split(b"\x00", 1)[0]
                    .decode("ascii", errors="replace")
                )
                s_addr, s_size, s_fileoff = struct.unpack_from("<III", data, sect_off + 32)
                sections.append((segname, sname, s_addr, s_size, s_fileoff))
                sect_off += 68
        elif cmd in (LC_ID_DYLIB, LC_LOAD_DYLIB):
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
                        sym_name = data[stroff + n_strx : end].decode("utf-8", errors="replace")
                        exports.add(sym_name)
                        symbol_info[sym_name] = (n_sect, n_value)
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
        "symbol_info": symbol_info,
        "sections": sections,
    }


def _symbol_bytes(data: bytes, info: dict[str, object], sym: str, length: int = 8) -> bytes:
    symbol_info: dict[str, tuple[int, int]] = info["symbol_info"]  # type: ignore[assignment]
    sections: list[tuple[str, str, int, int, int]] = info["sections"]  # type: ignore[assignment]
    n_sect, n_value = symbol_info[sym]
    _segname, _sname, s_addr, _s_size, s_fileoff = sections[n_sect - 1]
    addr = n_value & ~1
    file_pos = s_fileoff + (addr - s_addr)
    return data[file_pos : file_pos + length]


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
        self.assertEqual(
            build_guestlibs.LIBRARY_SPECS["libgcc_s.1.dylib"]["license"],
            "GPL-2.0-or-later WITH GCC-exception-2.0",
        )
        self.assertEqual(
            build_guestlibs.LIBRARY_SPECS["libstdc++.6.dylib"]["license"],
            "GPL-2.0-or-later WITH GCC-exception-2.0",
        )
        self.assertIn("-arch armv7", " ".join(build_guestlibs.DOCUMENTED_CFLAGS))
        self.assertIn("-miphoneos-version-min=6.0", " ".join(build_guestlibs.DOCUMENTED_CFLAGS))

        with mock.patch.dict(os.environ, {"HVM_CLANG": "/nonexistent/clang-binary"}):
            with self.assertRaises(SystemExit):
                build_guestlibs._find_tool("HVM_CLANG", ["clang"])

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
                    "active_code_syms": ["___divsi3", "___udivsi3", "__Unwind_SjLj_Register"],
                    "data_syms": [],
                },
                "libstdc++.6.dylib": {
                    "install_name": "/usr/lib/libstdc++.6.dylib",
                    "deps": ["/usr/lib/libgcc_s.1.dylib", "/usr/lib/libSystem.B.dylib"],
                    "must_export": ["__Znwm", "__ZdlPv", "___cxa_throw", "__ZNSsC1EPKcRKSaIcE"],
                    "active_code_syms": ["__Znwm", "__ZdlPv", "___cxa_allocate_exception", "___cxa_guard_acquire"],
                    "data_syms": ["__ZSt4cout"],
                },
                "libc++abi.dylib": {
                    "install_name": "/usr/lib/libc++abi.dylib",
                    "deps": ["/usr/lib/libSystem.B.dylib"],
                    "must_export": ["___cxa_allocate_exception", "___cxa_throw", "___cxa_guard_acquire", "___gxx_personality_sj0"],
                    "active_code_syms": ["___cxa_allocate_exception", "___cxa_guard_acquire", "___cxa_guard_release"],
                    "data_syms": ["__ZTISt9exception"],
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
                    "active_code_syms": ["__Znwm", "__ZNSt3__15mutex4lockEv", "__ZNSt3__15mutex6unlockEv"],
                    "data_syms": ["__ZNSt3__14coutE"],
                },
            }
            for filename, spec in expected.items():
                p = out_dir / filename
                self.assertTrue(p.is_file(), f"missing built dylib {filename}")
                raw = p.read_bytes()
                info = _inspect_macho(raw)
                self.assertEqual(info["magic"], MH_MAGIC)
                self.assertEqual(info["cputype"], CPU_TYPE_ARM)
                self.assertEqual(info["cpusubtype"], CPU_SUBTYPE_ARM_V7)
                self.assertEqual(info["filetype"], MH_DYLIB)
                self.assertEqual(info["install_name"], spec["install_name"])
                for dep in spec["deps"]:
                    self.assertIn(dep, info["load_dylibs"])
                for sym in spec["must_export"]:
                    self.assertIn(sym, info["exports"], f"{filename} missing export {sym}")
                # Verify no exported symbol is a fake-success `mov r0, #0; bx lr` stub
                for sym in info["exports"]:  # type: ignore[union-attr]
                    sym_bytes = _symbol_bytes(raw, info, sym, 8)
                    self.assertNotEqual(
                        sym_bytes,
                        FAKE_ZERO_STUB,
                        f"{filename}:{sym} must not be a fake-success `mov r0, #0; bx lr` stub",
                    )
                # Verify representative runtime functions are real multi-instruction bodies (not loud-trap stubs)
                for sym in spec["active_code_syms"]:
                    first_word = struct.unpack_from("<I", _symbol_bytes(raw, info, sym, 4), 0)[0]
                    self.assertNotEqual(
                        first_word,
                        0xE7F000F0,
                        f"{filename}:{sym} must be an active runtime implementation, not udf #0",
                    )
                # Verify data symbols reside in __DATA with non-zero sentinel header
                for dsym in spec["data_syms"]:
                    n_sect, _n_val = info["symbol_info"][dsym]  # type: ignore[index]
                    segname = info["sections"][n_sect - 1][0]  # type: ignore[index]
                    self.assertEqual(segname, "__DATA", f"{filename}:{dsym} must be in __DATA")
                    magic_word = struct.unpack_from("<I", _symbol_bytes(raw, info, dsym, 4), 0)[0]
                    self.assertEqual(magic_word, 0x48564D44)
                self.assertIn(filename, manifest["libraries"])

            # Also verify deterministic fallback builder clears MH_NOUNDEFS and avoids fake-zero stubs
            det_bytes = build_guestlibs._build_macho_dylib_deterministic(
                build_guestlibs.LIBRARY_SPECS["libc++abi.dylib"]
            )
            det_info = _inspect_macho(det_bytes)
            self.assertEqual(int(det_info["flags"]) & 0x1, 0, "fallback Mach-O must not set MH_NOUNDEFS")
            for sym in det_info["exports"]:  # type: ignore[union-attr]
                self.assertNotEqual(_symbol_bytes(det_bytes, det_info, sym, 8), FAKE_ZERO_STUB)

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
        self.assertIn("GPL-2.0-or-later WITH GCC-exception-2.0", text)
        self.assertIn("Apache-2.0 WITH LLVM-exception", text)
        self.assertIn("libstdc++.6.dylib", text)
        self.assertIn("libc++.1.dylib", text)


if __name__ == "__main__":
    unittest.main()
