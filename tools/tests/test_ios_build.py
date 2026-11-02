"""Tests for iOS build prerequisite detection."""
import importlib.util
import io
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


class TestRun(unittest.TestCase):
    def test_missing_command_exits_3_with_diagnostic(self):
        stderr = io.StringIO()
        with patch("sys.stderr", stderr), self.assertRaises(SystemExit) as raised:
            ios_build.run(["/nonexistent/hvm-no-such-tool", "--version"])
        self.assertEqual(raised.exception.code, 3)
        self.assertIn("hvm-no-such-tool", stderr.getvalue())
        self.assertIn("not found", stderr.getvalue())


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

    def test_resolve_tool_precedence(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            toolchain_bin = root / "configured toolchain" / "bin"
            path_bin = root / "path"
            override = root / "override-tool"
            toolchain_bin.mkdir(parents=True)
            path_bin.mkdir()
            toolchain_tool = toolchain_bin / "tool"
            path_tool = path_bin / "tool"
            for tool in (toolchain_tool, path_tool, override):
                tool.write_text("#!/bin/sh\nexit 0\n")
                tool.chmod(0o755)

            # The toolchain is read when the module is imported.
            with patch.dict(os.environ, {"HVM_TOOLCHAIN": str(toolchain_bin.parent),
                                         "PATH": str(path_bin)}, clear=True):
                spec = importlib.util.spec_from_file_location("ios_build_test", ROOT / "tools/ios_build.py")
                module = importlib.util.module_from_spec(spec)
                spec.loader.exec_module(module)
                self.assertEqual(module.resolve_tool("HVM_CLANG", ["tool"]), str(toolchain_tool))
                with patch.dict(os.environ, {"HVM_CLANG": str(override)}):
                    self.assertEqual(module.resolve_tool("HVM_CLANG", ["tool"]), str(override))
                toolchain_tool.unlink()
                self.assertEqual(module.resolve_tool("HVM_CLANG", ["tool"]), str(path_tool))

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
