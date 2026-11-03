import os
import re
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PROJECT = ROOT / "app/project.yml"


class RevisionHeaderTests(unittest.TestCase):
    def test_current_revision_does_not_rewrite_header(self):
        # Run the script from the XcodeGen spec, so the test covers the build phase.
        match = re.search(
            r"(?m)^      - name: Record emulator revision\n"
            r"(?:^        .*\n|^          .*\n)*?"
            r"^        script: \|\n((?:^          .*\n)+)",
            PROJECT.read_text(),
        )
        self.assertIsNotNone(match, "revision build script not found")
        script = textwrap.dedent(match.group(1))
        revision = subprocess.check_output(
            ["/usr/bin/git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True
        ).strip()
        expected = f'#define HVM_COMMIT "{revision}"\n'

        with tempfile.TemporaryDirectory() as derived:
            header = Path(derived) / "HVMRevision.h"
            header.write_text(expected)
            os.utime(header, (946684800, 946684800))
            original_mtime = header.stat().st_mtime_ns
            env = dict(os.environ, SRCROOT=str(ROOT / "app"), DERIVED_FILE_DIR=derived)
            subprocess.run(["/bin/sh", "-c", script], env=env, check=True)
            self.assertEqual(header.read_text(), expected)
            self.assertEqual(header.stat().st_mtime_ns, original_mtime)


if __name__ == "__main__":
    unittest.main()
