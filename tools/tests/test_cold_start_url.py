import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'test_cold_start_url.sh'

class ColdStartAnnotationTests(unittest.TestCase):
    def test_log_percent_sequences_are_not_actions_command_data(self):
        script = SCRIPT.read_text()
        start = script.index('fail() {')
        function = script[start:script.index('\n}', start) + 2]
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run(['bash', '-c',
                'app_log() { printf "%s\\n" "%0A %25"; }\n' + function + '\nfail missing-marker'],
                env={**os.environ, 'DATA': directory}, capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn('%0A %25', result.stdout)
        for line in result.stdout.splitlines():
            if line.startswith('::error'):
                self.assertNotIn('%0A', line)
                self.assertNotIn('%25', line)
