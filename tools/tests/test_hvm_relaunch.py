import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

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
pathlib.Path(a[a.index('--report')+1]).write_text(json.dumps({'exitStatus':42 if n==2 else 0,'crashed':n==2}))
sys.exit(42 if n==2 else 0)
''')
            hvm.chmod(0o700)
            result = run_relaunch(hvm, app, root / 'home', root / 'out', launches=3, seconds=1)
            self.assertEqual([r['returnCode'] for r in result['runs']], [0, 42, 0])
            self.assertFalse(result['allProcessesExitedZero'])
            self.assertEqual((root / 'home/state').read_text(), '3')
            self.assertEqual(result['runs'][1]['homeDiff']['changed'], ['state'])
            self.assertEqual(len(list((root / 'out').glob('home-*.json'))), 4)

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
