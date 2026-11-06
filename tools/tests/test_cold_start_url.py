import os
from pathlib import Path
import plistlib
import subprocess
import tempfile
import threading
import time
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'test_cold_start_url.sh'

class ColdStartAnnotationTests(unittest.TestCase):
    def test_cold_url_waits_for_setup_process_to_exit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            guest = root / 'Guest.app'
            guest.mkdir()
            (guest / 'Info.plist').write_bytes(plistlib.dumps({'CFBundleIdentifier': 'test.guest'}))
            data = root / 'data'
            (data / 'Documents').mkdir(parents=True)
            worker = subprocess.Popen(['sleep', '30'])
            stop = root / 'stop'
            def reap_worker():
                while not stop.exists() and worker.poll() is None:
                    time.sleep(0.01)
                if worker.poll() is None:
                    # simctl may return before the process finishes termination.
                    time.sleep(1.5)
                    worker.terminate()
                    worker.wait()
            reaper = threading.Thread(target=reap_worker)
            reaper.start()
            xcrun = root / 'xcrun'
            xcrun.write_text('''#!/usr/bin/env python3
import json, os, pathlib, sys
args = sys.argv[1:]
data = pathlib.Path(os.environ['PROBE_DATA'])
docs = data / 'Documents'
pid = int(os.environ['PROBE_PID'])
command = args[1]
if command == 'list':
    print(json.dumps({'devices': {'iOS-26-0': [{'name': 'iPhone', 'udid': 'test'}]}}))
elif command == 'get_app_container':
    print(data)
elif command == 'launch':
    (docs / 'cold-start-launched').write_text('com.lfull.HydrogenVM')
    print('com.lfull.HydrogenVM: ' + str(pid))
elif command == 'terminate':
    pathlib.Path(os.environ['PROBE_STOP']).touch()
elif command == 'openurl':
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        pass
    else:
        sys.exit('URL opened before setup process exited')
    url = args[-1]
    (docs / 'cold-start-queued').write_text(url)
    if '?' in url:
        (docs / 'cold-start-error').write_text('That link is not a HydrogenVM game link. A game icon uses hydrogenvm://run/<game-id>.')
    else:
        (docs / 'cold-start-launched').write_text('com.lfull.HydrogenVM')
        for stage in ['routed', 'guest-passed']:
            (docs / ('cold-start-' + stage)).write_text('test.guest')
''')
            xcrun.chmod(0o755)
            try:
                result = subprocess.run(['bash', str(SCRIPT), str(root / 'Host.app'), str(guest)],
                    env={**os.environ, 'PATH': str(root) + os.pathsep + os.environ['PATH'],
                         'PROBE_DATA': str(data), 'PROBE_PID': str(worker.pid), 'PROBE_STOP': str(stop)},
                    capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn('PASS: app launched', result.stdout)
                self.assertIn('PASS: malformed URL resolved', result.stdout)
            finally:
                stop.touch()
                reaper.join(timeout=5)
                if worker.poll() is None:
                    worker.kill()
                    worker.wait()

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
