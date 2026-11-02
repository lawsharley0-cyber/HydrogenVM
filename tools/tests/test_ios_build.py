"""Tests for iOS build prerequisite detection."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import ios_build


class TestPrerequisites(unittest.TestCase):
    def test_non_executable_override_fails_before_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            tool = Path(tmp) / "tool"
            tool.write_text("#!/bin/sh\nexit 0\n")
            tool.chmod(0o644)
            for variable, label in (("HVM_CLANG", "clang:"),
                                    ("HVM_LD64", "ld64"),
                                    ("HVM_NM", "llvm-nm:")):
                with self.subTest(variable=variable):
                    env = os.environ.copy()
                    env.update(HVM_CLANG=sys.executable, HVM_LD64=sys.executable,
                               HVM_NM=sys.executable)
                    env[variable] = str(tool)
                    result = subprocess.run(
                        [sys.executable, str(ROOT / "tools/ios_build.py"), "app",
                         "--name", "Test", "--out", str(Path(tmp) / "out"), "test.c"],
                        capture_output=True, text=True, env=env,
                    )
                    self.assertEqual(result.returncode, 3, result.stderr)
                    self.assertIn("missing build prerequisites", result.stderr)
                    self.assertIn(label, result.stderr)
                    self.assertNotIn("PermissionError", result.stderr)

    def test_executable_paths_and_command_names(self):
        with tempfile.TemporaryDirectory() as tmp:
            tool = Path(tmp) / "tool"
            tool.write_text("#!/bin/sh\nexit 0\n")
            tool.chmod(0o755)
            with patch.dict(os.environ, {"PATH": tmp}), \
                 patch.multiple(ios_build, CLANG=str(tool), LD="tool", NM=str(tool)):
                ios_build.check_prerequisites()
            with patch.dict(os.environ, {"PATH": tmp}):
                self.assertTrue(ios_build.tool_available(os.path.relpath(tool)))
                self.assertFalse(ios_build.tool_available(tmp))
                self.assertFalse(ios_build.tool_available("missing-tool"))

    def test_command_name_is_resolved_on_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            tool = Path(tmp) / "tool"
            tool.write_text("#!/bin/sh\nexit 0\n")
            tool.chmod(0o755)
            # A same-named file in the current directory is not a PATH lookup.
            empty_path = Path(tmp) / "empty-path"
            empty_path.mkdir()
            result = subprocess.run(
                [sys.executable, "-c",
                 "import sys; sys.path.insert(0, sys.argv[1]); "
                 "import ios_build; sys.exit(0 if not ios_build.tool_available('tool') else 1)",
                 str(ROOT / "tools")], cwd=tmp, env={**os.environ, "PATH": str(empty_path)},
            )
            self.assertEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
