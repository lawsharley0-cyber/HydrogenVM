"""Test Lab CI output triage: keep successful cases concise, surface failures."""

import contextlib
import io
import importlib.util
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location("hvm_testlab_runner", ROOT / "tools" / "hvm_testlab.py")
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)


class TestFailureLogOutput(unittest.TestCase):
    def capture(self, status, tail):
        case = {
            "id": "TL-N",
            "status": status,
            "reason": "Checkpoint detected (exit code 2)" if status == "interrupted" else "",
            "diagnostics": {"stdoutTail": tail},
        }
        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer):
            RUNNER._print_result(case)
        return buffer.getvalue()

    def test_interrupted_case_shows_bounded_emulator_tail(self):
        out = self.capture("interrupted", "driver error: context bind failed\nprocess exit 2")
        self.assertIn("TL-N: interrupted", out)
        self.assertIn("emulator stdout/stderr tail:", out)
        self.assertIn("driver error: context bind failed", out)
        self.assertIn("process exit 2", out)

    def test_passed_case_does_not_dump_logs(self):
        out = self.capture("passed", "private debug chatter")
        self.assertIn("TL-N: passed", out)
        self.assertNotIn("private debug chatter", out)
        self.assertNotIn("emulator stdout/stderr tail:", out)

    def test_failures_without_tail_stay_concise(self):
        out = self.capture("failed", "")
        self.assertIn("TL-N: failed", out)
        self.assertNotIn("emulator stdout/stderr tail:", out)

    def test_long_tail_prints_at_most_sixteen_lines(self):
        out = self.capture("interrupted", "\n".join(f"trace {i}" for i in range(40)))
        self.assertNotIn("trace 23\n", out)
        self.assertIn("trace 24", out)
        self.assertIn("trace 39", out)


if __name__ == "__main__":
    unittest.main()
