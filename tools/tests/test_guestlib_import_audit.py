#!/usr/bin/env python3
"""Unit tests for tools/guestlibs/import_audit.py (S4)."""

from __future__ import annotations

import json
import os
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
AUDIT_SCRIPT = REPO_ROOT / "tools" / "guestlibs" / "import_audit.py"
REPORT_DOC = REPO_ROOT / "docs" / "research" / "guestlib_imports.md"


def _load_import_audit():
    sys.path.insert(0, str(AUDIT_SCRIPT.parent))
    try:
        import import_audit  # type: ignore

        return import_audit
    finally:
        sys.path.pop(0)


def _make_synthetic_macho_dylib(
    deps: list[str | tuple[str, int]],
    undef_syms: list[tuple[str, int]],
) -> bytes:
    """Build a minimal 32-bit ARM MH_DYLIB with load commands and LC_SYMTAB."""
    load_cmds = bytearray()
    ncmds = 0
    for dep_item in deps:
        if isinstance(dep_item, tuple):
            dep, cmd = dep_item
        else:
            dep, cmd = dep_item, 0xC
        payload = dep.encode("utf-8") + b"\x00"
        cmdsize = (24 + len(payload) + 3) & ~3
        padded = payload + b"\x00" * (cmdsize - 24 - len(payload))
        load_cmds.extend(struct.pack("<IIIIII", cmd, cmdsize, 24, 2, 0x10000, 0x10000) + padded)
        ncmds += 1

    strtab = bytearray(b"\x00")
    nlists = bytearray()
    for sym, ordinal in undef_syms:
        strx = len(strtab)
        strtab.extend(sym.encode("utf-8") + b"\x00")
        n_desc = (ordinal & 0xFF) << 8
        nlists.extend(struct.pack("<IBBHI", strx, 0x01, 0, n_desc, 0))

    symoff = 28 + len(load_cmds) + 24
    stroff = symoff + len(nlists)
    load_cmds.extend(struct.pack("<IIIIII", 0x2, 24, symoff, len(undef_syms), stroff, len(strtab)))
    ncmds += 1

    hdr = struct.pack("<IIIIIII", 0xFEEDFACE, 12, 9, 6, ncmds, len(load_cmds), 0x84)
    return hdr + bytes(load_cmds) + bytes(nlists) + bytes(strtab)


class GuestLibImportAuditTests(unittest.TestCase):
    def test_audit_script_exists_and_classifies_all_four_runtime_libraries(self) -> None:
        self.assertTrue(AUDIT_SCRIPT.is_file(), f"Missing {AUDIT_SCRIPT}")
        import_audit = _load_import_audit()

        result = import_audit.run_audit(REPO_ROOT)
        self.assertEqual(result["importSource"], "manifest")
        self.assertIn("libraries", result)
        for lib_name in (
            "libgcc_s.1.dylib",
            "libstdc++.6.dylib",
            "libc++abi.dylib",
            "libc++.1.dylib",
        ):
            self.assertIn(lib_name, result["libraries"])
            lib_summary = result["libraries"][lib_name]
            self.assertGreater(lib_summary["totalImports"], 10)
            self.assertEqual(
                lib_summary["totalImports"],
                lib_summary["present"]
                + lib_summary["missing"]
                + lib_summary["stubbed-dishonestly"]
                + lib_summary["unverified"],
            )

        symbols = result["symbols"]
        self.assertGreater(len(symbols), 80)
        for sym_name, info in symbols.items():
            self.assertIn(
                info["status"],
                {"present", "missing", "stubbed-dishonestly", "unverified"},
                f"Invalid status for {sym_name}: {info}",
            )
            self.assertTrue(info["importedBy"], f"Empty importedBy for {sym_name}")

        # Exact statuses for representative present, stubbed-dishonestly, and missing symbols
        self.assertEqual(symbols["_malloc"]["status"], "present")
        self.assertEqual(symbols["_free"]["status"], "present")
        self.assertEqual(symbols["_pthread_mutex_lock"]["status"], "present")
        self.assertEqual(symbols["_pthread_getspecific"]["status"], "present")
        self.assertEqual(symbols["_setlocale"]["status"], "stubbed-dishonestly")
        self.assertEqual(symbols["_uselocale"]["status"], "missing")
        self.assertEqual(symbols["_wcrtomb"]["status"], "missing")

    def test_parser_and_scanner_with_synthetic_fixtures(self) -> None:
        import_audit = _load_import_audit()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            runtime_dir = root / "src" / "runtime" / "libc"
            runtime_dir.mkdir(parents=True)

            sample_cpp = runtime_dir / "Sample.cpp"
            sample_cpp.write_text(
                """
                // p.def<&fake_commented>("_commented_out");
                /* p.def<&fake_block>("_block_commented"); */
                void* real_malloc(Ctx&, size_t size) { return malloc(size); }
                int f_stat(Ctx& x) { return x.proc.stat(); }
                int a_approx(Ctx& x) { return x.proc.signal(); }
                void registerSample(Process& p) {
                    p.def<&real_malloc>("_malloc");
                    p.def<&s_zero>("_tcgetattr");
                    for (const char* n : {"_stat", "_stat64"}) p.def<&f_stat>(n);
                    supportDef<&m_ok3>(p, "_mprotect", SupportStatus::Stub, "No-op stub");
                    supportDef<&u_unavail>(p, "_unavail_fn", SupportStatus::Unavailable, "Unavailable");
                    supportDef<&a_approx>(p, "_pthread_cond_signal", SupportStatus::Approx, "Approx");
                    p.defineData("_vm_page_size", 4096);
                }
                """,
                encoding="utf-8",
            )

            regs = import_audit.scan_hvm_registrations(root)
            self.assertNotIn("_commented_out", regs)
            self.assertNotIn("_block_commented", regs)
            self.assertEqual(regs["_malloc"]["status"], "present")
            self.assertEqual(regs["_tcgetattr"]["status"], "stubbed-dishonestly")
            self.assertEqual(regs["_stat"]["status"], "present")
            self.assertEqual(regs["_stat64"]["status"], "present")
            self.assertEqual(regs["_mprotect"]["status"], "stubbed-dishonestly")
            self.assertEqual(regs["_unavail_fn"]["status"], "stubbed-dishonestly")
            self.assertEqual(regs["_pthread_cond_signal"]["status"], "present")
            self.assertEqual(regs["_vm_page_size"]["status"], "present")

            # Valid Mach-O with ordinal 1 = libgcc_s.1.dylib (peer) and ordinal 2 = libSystem.B.dylib
            valid_dylib = root / "valid.dylib"
            valid_dylib.write_bytes(
                _make_synthetic_macho_dylib(
                    ["/usr/lib/libgcc_s.1.dylib", "/usr/lib/libSystem.B.dylib"],
                    [("__Unwind_SjLj_Register", 1), ("_CustomPeerHelper", 1), ("_malloc", 2), ("_free", 2)],
                )
            )
            parsed = import_audit.parse_macho_undef_symbols(valid_dylib)
            self.assertEqual(parsed, ["_free", "_malloc"])

            # Valid Mach-O with weak dylib (ordinal 1), reexport (ordinal 2), lazy (ordinal 3), upward (ordinal 4), libSystem (ordinal 5)
            # LC_LOAD_WEAK_DYLIB=0x80000018, LC_REEXPORT_DYLIB=0x8000001f, LC_LAZY_LOAD_DYLIB=0x20, LC_LOAD_UPWARD_DYLIB=0x80000023
            complex_dylib = root / "complex.dylib"
            complex_dylib.write_bytes(
                _make_synthetic_macho_dylib(
                    [
                        ("/usr/lib/weak.dylib", 0x80000018),
                        ("/usr/lib/reexport.dylib", 0x8000001f),
                        ("/usr/lib/lazy.dylib", 0x20),
                        ("/usr/lib/upward.dylib", 0x80000023),
                        "/usr/lib/libSystem.B.dylib"
                    ],
                    [("_weak_sym", 1), ("_reexport_sym", 2), ("_lazy_sym", 3), ("_upward_sym", 4), ("_malloc", 5)],
                )
            )
            parsed2 = import_audit.parse_macho_undef_symbols(complex_dylib)
            self.assertEqual(parsed2, ["_malloc"])

            # Truncated Mach-O header
            trunc_dylib = root / "trunc.dylib"
            trunc_dylib.write_bytes(b"\xce\xfa\xed\xfe\x00\x00")
            with self.assertRaises(ValueError):
                import_audit.parse_macho_undef_symbols(trunc_dylib)

            # Corrupt LC_SYMTAB offset exceeding file size
            bad_symtab = bytearray(valid_dylib.read_bytes())
            sizeofcmds = struct.unpack_from("<I", bad_symtab, 20)[0]
            struct.pack_into("<I", bad_symtab, 28 + sizeofcmds - 16, 0xFFFFFF)
            corrupt_dylib = root / "corrupt.dylib"
            corrupt_dylib.write_bytes(bytes(bad_symtab))
            with self.assertRaises(ValueError):
                import_audit.parse_macho_undef_symbols(corrupt_dylib)

            # --hvm-bin missing file raises FileNotFoundError
            with self.assertRaises(FileNotFoundError):
                import_audit.run_audit(root, hvm_bin=root / "no_such_hvm")

            # --hvm-bin valid executable script adds symbol statuses
            fake_hvm = root / "fake_hvm.sh"
            fake_hvm.write_text(
                "#!/bin/sh\n"
                + "echo '"
                + json.dumps(
                    {
                        "symbols": [
                            {"name": "_calloc", "kind": "function", "status": "Real"},
                            {"name": "_btowc", "kind": "function", "status": "Stub"},
                        ]
                    }
                )
                + "'\n",
                encoding="utf-8",
            )
            fake_hvm.chmod(fake_hvm.stat().st_mode | stat.S_IEXEC)
            audit_with_bin = import_audit.run_audit(root, hvm_bin=fake_hvm)
            self.assertEqual(audit_with_bin["symbols"]["_calloc"]["status"], "unverified")
            self.assertEqual(audit_with_bin["symbols"]["_btowc"]["status"], "stubbed-dishonestly")

    def test_arbitrary_constant_success_handler_is_not_present(self) -> None:
        audit = _load_import_audit()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            runtime = root / "src/runtime"
            runtime.mkdir(parents=True)
            (runtime / "Sample.cpp").write_text(
                'int plausible_name(Ctx&) { return 0; }\n'
                'int stateful(Ctx& x) { x.proc.detachThread(1); return 0; }\n'
                'void reg(Process& p) { p.def<&plausible_name>("_pthread_mutex_lock"); '
                'p.def<&stateful>("_pthread_detach"); '
                'supportDef<&plausible_name>(p, "_pthread_once", SupportStatus::Approx, "mislabelled"); }')
            regs = audit.scan_hvm_registrations(root)
            self.assertEqual(regs["_pthread_mutex_lock"]["status"], "stubbed-dishonestly")
            self.assertEqual(regs["_pthread_once"]["status"], "stubbed-dishonestly")
            self.assertEqual(regs["_pthread_detach"]["status"], "present")

    def test_registration_without_a_resolved_body_is_unverified(self) -> None:
        audit = _load_import_audit()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            runtime = root / "src/runtime"
            runtime.mkdir(parents=True)
            (runtime / "Sample.cpp").write_text('void reg(Process& p) { p.def<&not_defined_here>("_malloc"); }')
            self.assertEqual(audit.scan_hvm_registrations(root)["_malloc"]["status"], "unverified")

    def test_resource_free_mutex_attribute_destructor_is_real(self) -> None:
        audit = _load_import_audit()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            runtime = root / "src/runtime/libc"
            runtime.mkdir(parents=True)
            (runtime / "Pthread.cpp").write_text(
                'int32_t pma_init(Ctx& x, GuestPtr a) { x.mem.write<uint32_t>(a, 0x4D555458); x.mem.write<uint32_t>(a + 4, 0); return 0; }\n'
                'int32_t pma_ok(Ctx&, GuestPtr) { return 0; }\n'
                'void reg(Process& p) { p.def<&pma_ok>("_pthread_mutexattr_destroy"); p.def<&pma_ok>("_other_api"); }')
            regs = audit.scan_hvm_registrations(root)
            self.assertEqual(regs["_pthread_mutexattr_destroy"]["status"], "present")
            self.assertEqual(regs["_other_api"]["status"], "stubbed-dishonestly")
            path = runtime / "Pthread.cpp"
            path.write_text(path.read_text().replace("return 0;", "x.proc.allocStatic(64); return 0;", 1))
            self.assertEqual(audit.scan_hvm_registrations(root)["_pthread_mutexattr_destroy"]["status"], "stubbed-dishonestly")

    def test_mutex_attribute_exemption_needs_whole_token_match(self) -> None:
        audit = _load_import_audit()
        reg = 'void reg(Process& p) { p.def<&pma_ok>("_pthread_mutexattr_destroy"); }'
        init = 'int32_t pma_init(Ctx& x, GuestPtr a) { x.mem.write<uint32_t>(a, 0x4D555458); x.mem.write<uint32_t>(a + 4, 0); return 0; }\n'
        cases = {
            # Identifier suffix: `uint32_t` contains `int32_t` as text, but is another token.
            "identifier suffix": init + 'uint32_t pma_ok(Ctx&, GuestPtr) { return 0; }\n' + reg,
            # The exempt initializer text appears only inside a string literal; the real initializer leaks.
            "inside string literal": 'int32_t pma_init(Ctx& x, GuestPtr a) { x.proc.allocStatic(64); return 0; }\n'
                'int32_t pma_ok(Ctx&, GuestPtr) { return 0; }\n'
                'const char* k = "' + " ".join(tk.value for tk in audit.lint_stubs.tokens(init)) + '";\n' + reg,
        }
        for name, source in cases.items():
            with self.subTest(name), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                (root / "src/runtime/libc").mkdir(parents=True)
                (root / "src/runtime/libc/Pthread.cpp").write_text(source)
                self.assertEqual(audit.scan_hvm_registrations(root)["_pthread_mutexattr_destroy"]["status"], "stubbed-dishonestly")

    def test_malformed_support_manifest_is_runtime_error(self) -> None:
        from unittest.mock import patch
        audit = _load_import_audit()
        for payload in ([], {}, {"symbols": {}}, {"symbols": [None]}, {"symbols": [{}]}):
            with self.subTest(payload=payload), patch.object(audit.subprocess, "check_output", return_value=json.dumps(payload)):
                with self.assertRaises(RuntimeError):
                    audit.run_audit(REPO_ROOT, hvm_bin=Path(sys.executable))

    def test_cli_generates_markdown_report(self) -> None:
        self.assertTrue(AUDIT_SCRIPT.is_file(), f"Missing {AUDIT_SCRIPT}")
        with tempfile.TemporaryDirectory() as tmp:
            out_md = Path(tmp) / "guestlib_imports.md"
            proc = subprocess.run(
                [sys.executable, str(AUDIT_SCRIPT), "--out", str(out_md)],
                cwd=str(REPO_ROOT),
                capture_output=True,
                text=True,
                timeout=120,
                check=False,
            )
            self.assertEqual(proc.returncode, 0, f"stderr: {proc.stderr}")
            self.assertTrue(out_md.is_file())
            text = out_md.read_text(encoding="utf-8")
            self.assertIn("libstdc++.6.dylib", text)
            self.assertIn("libc++.1.dylib", text)
            self.assertIn("present", text)
            self.assertIn("missing", text)
            self.assertIn("stubbed-dishonestly", text)

    def test_checked_in_report_doc_exists_and_matches_audit(self) -> None:
        self.assertTrue(REPORT_DOC.is_file(), f"Missing {REPORT_DOC}")
        import_audit = _load_import_audit()
        expected = import_audit.render_markdown_report(import_audit.run_audit(REPO_ROOT))
        self.assertEqual(REPORT_DOC.read_text(encoding="utf-8"), expected)


if __name__ == "__main__":
    unittest.main()
