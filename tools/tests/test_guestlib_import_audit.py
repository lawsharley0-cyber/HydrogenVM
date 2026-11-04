#!/usr/bin/env python3
"""Unit tests for tools/guestlibs/import_audit.py (S4)."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
AUDIT_SCRIPT = REPO_ROOT / "tools" / "guestlibs" / "import_audit.py"
REPORT_DOC = REPO_ROOT / "docs" / "research" / "guestlib_imports.md"


class GuestLibImportAuditTests(unittest.TestCase):
    def test_audit_script_exists_and_classifies_all_four_runtime_libraries(self) -> None:
        self.assertTrue(AUDIT_SCRIPT.is_file(), f"Missing {AUDIT_SCRIPT}")
        sys.path.insert(0, str(AUDIT_SCRIPT.parent))
        try:
            import import_audit  # type: ignore
        finally:
            sys.path.pop(0)

        result = import_audit.run_audit(REPO_ROOT)
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
                + lib_summary["stubbed-dishonestly"],
            )

        symbols = result["symbols"]
        self.assertGreater(len(symbols), 80)
        for sym_name, info in symbols.items():
            self.assertIn(
                info["status"],
                {"present", "missing", "stubbed-dishonestly"},
                f"Invalid status for {sym_name}: {info}",
            )
            self.assertTrue(info["importedBy"], f"Empty importedBy for {sym_name}")

        # Core malloc/pthread/string primitives are present in HydrogenVM
        self.assertEqual(symbols["_malloc"]["status"], "present")
        self.assertEqual(symbols["_free"]["status"], "present")
        self.assertEqual(symbols["_pthread_mutex_lock"]["status"], "present")
        self.assertEqual(symbols["_pthread_getspecific"]["status"], "present")

        # Wide-char / extended locale primitives are honestly classified as missing or stubbed-dishonestly
        self.assertIn(symbols["_uselocale"]["status"], {"missing", "stubbed-dishonestly"})
        self.assertIn(symbols["_wcrtomb"]["status"], {"missing", "stubbed-dishonestly"})

    def test_cli_generates_markdown_report(self) -> None:
        self.assertTrue(AUDIT_SCRIPT.is_file(), f"Missing {AUDIT_SCRIPT}")
        with tempfile.TemporaryDirectory() as tmp:
            out_md = Path(tmp) / "guestlib_imports.md"
            proc = subprocess.run(
                [sys.executable, str(AUDIT_SCRIPT), "--out", str(out_md)],
                cwd=str(REPO_ROOT),
                capture_output=True,
                text=True,
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
        text = REPORT_DOC.read_text(encoding="utf-8")
        for required in (
            "libgcc_s.1.dylib",
            "libstdc++.6.dylib",
            "libc++abi.dylib",
            "libc++.1.dylib",
            "present",
            "missing",
            "stubbed-dishonestly",
            "_malloc",
            "_pthread_key_create",
            "_uselocale",
        ):
            self.assertIn(required, text)


if __name__ == "__main__":
    unittest.main()
