import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from hvm_relaunch import diff_snapshots, run_relaunch, snapshot_home


class RelaunchEvidenceTests(unittest.TestCase):
    def test_hash_diff_preserves_existing_state(self):
        with tempfile.TemporaryDirectory() as d:
            home = Path(d) / 'home'
            home.mkdir()
            state = home / 'state'
            state.write_bytes(b'first')
            before = snapshot_home(home)
            self.assertEqual(before['state']['sha256'], hashlib.sha256(b'first').hexdigest())
            state.write_bytes(b'second')
            (home / 'added').write_bytes(b'new')
            after = snapshot_home(home)
            self.assertEqual(diff_snapshots(before, after), {'added': ['added'], 'removed': [], 'changed': ['state']})
            self.assertEqual(state.read_bytes(), b'second')

    @unittest.skipUnless(os.name == 'posix', 'uses a POSIX executable fixture')
    def test_failed_second_launch_does_not_erase_state_or_hide_third_launch(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            app = root / 'app'
            app.mkdir()
            hvm = root / 'hvm'
            hvm.write_text('''#!/usr/bin/env python3
import json, pathlib, sys
a=sys.argv
h=pathlib.Path(a[a.index('--home')+1]); h.mkdir(parents=True,exist_ok=True)
p=h/'state'; n=int(p.read_text())+1 if p.exists() else 1
p.write_text(str(n))
pathlib.Path(a[a.index('--report')+1]).write_text(json.dumps({'exitStatus':42 if n==9 else 0,'crashed':n==2}))
sys.exit(42 if n==9 else 0)
''')
            hvm.chmod(0o700)
            home = root / 'home'
            home.mkdir()
            (home / 'state').write_text('7')
            result = run_relaunch(hvm, app, home, root / 'out', launches=3, seconds=1)
            self.assertEqual([r['returnCode'] for r in result['runs']], [0, 42, 0])
            self.assertFalse(result['allProcessesExitedZero'])
            self.assertEqual((home / 'state').read_text(), '10')
            self.assertEqual(json.loads((root / 'out/home-0.json').read_text())['state']['sha256'], hashlib.sha256(b'7').hexdigest())
            self.assertEqual(json.loads((root / 'out/home-1.json').read_text())['state']['sha256'], hashlib.sha256(b'8').hexdigest())
            self.assertEqual((root / 'out').stat().st_mode & 0o777, 0o700)
            self.assertEqual(result['runs'][1]['homeDiff']['changed'], ['state'])
            self.assertEqual(len(list((root / 'out').glob('home-*.json'))), 4)

    def test_missing_home_is_not_a_fresh_preserved_home(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); (root / 'app').mkdir()
            with self.assertRaisesRegex(ValueError, 'existing home'):
                run_relaunch(root / 'unused', root / 'app', root / 'missing', root / 'out')
            self.assertFalse((root / 'out').exists())

    def test_unreadable_directory_is_not_a_complete_inventory(self):
        with tempfile.TemporaryDirectory() as d:
            with patch('hvm_relaunch.os.scandir', side_effect=PermissionError('unreadable test home')):
                with self.assertRaises(PermissionError):
                    snapshot_home(Path(d))

    @unittest.skipUnless(os.name == 'posix', 'POSIX permission bits')
    def test_public_evidence_directory_is_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            for name in ['home', 'app', 'out']: (root / name).mkdir()
            (root / 'out').chmod(0o755)
            with self.assertRaisesRegex(ValueError, 'group or other'):
                run_relaunch(root / 'unused', root / 'app', root / 'home', root / 'out')

    @unittest.skipUnless(os.name == 'posix', 'uses a POSIX executable fixture')
    def test_in_process_early_failure_is_recorded_as_two_attempts_not_three(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            for name in ['home', 'app']: (root / name).mkdir()
            hvm = root / 'hvm'
            hvm.write_text("#!/usr/bin/env python3\nimport json,pathlib,sys\na=sys.argv\nassert a[a.index('--launches')+1]=='3'\nprint('[hvm] ---- launch 1 of 3 ----')\nprint('[hvm] ---- launch 2 of 3 ----')\npathlib.Path(a[a.index('--report')+1]).write_text(json.dumps({'exitStatus':42,'crashed':True}))\nsys.exit(42)\n")
            hvm.chmod(0o700)
            result = run_relaunch(hvm, root / 'app', root / 'home', root / 'out', mode='in-process')
            row = result['runs'][0]
            self.assertEqual(row['returnCode'], 42)
            self.assertEqual(row['attemptedLaunches'], 2)
            self.assertFalse(row['allRequestedLaunchesAttempted'])
            self.assertFalse(result['allRunsExitedWithoutCrash'])
            self.assertFalse(result['intermediateSnapshotsAvailable'])

    def test_evidence_directory_must_not_modify_the_home(self):
        with tempfile.TemporaryDirectory() as d:
            home = Path(d) / 'home'
            with self.assertRaises(ValueError):
                run_relaunch(Path('/unused'), Path('/unused'), home, home / 'evidence')
            self.assertFalse(home.exists())

    @unittest.skipUnless(os.name == 'posix', 'uses a POSIX executable fixture')
    def test_zero_process_status_cannot_hide_a_reported_crash(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            app = root / 'app'
            app.mkdir()
            hvm = root / 'hvm'
            hvm.write_text('''#!/usr/bin/env python3
import json, pathlib, sys
a=sys.argv
pathlib.Path(a[a.index('--report')+1]).write_text(json.dumps({'exitStatus':0,'crashed':True}))
''')
            hvm.chmod(0o700)
            (root / 'home').mkdir()
            result = run_relaunch(hvm, app, root / 'home', root / 'out', launches=1, seconds=1)
            self.assertTrue(result['allProcessesExitedZero'])
            self.assertFalse(result['allRunsExitedWithoutCrash'])

    @unittest.skipUnless(os.name == 'posix', 'symlink semantics')
    def test_snapshot_does_not_follow_links_outside_home(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            home = root / 'home'
            home.mkdir()
            (root / 'outside').write_text('private')
            (home / 'link').symlink_to(root / 'outside')
            snapshot = snapshot_home(home)
            self.assertEqual(snapshot['link']['kind'], 'symlink')
            self.assertNotIn('sha256', snapshot['link'])
            self.assertEqual(set(snapshot), {'link'})


if __name__ == '__main__':
    unittest.main()
