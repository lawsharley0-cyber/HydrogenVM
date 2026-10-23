import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from compat_harness.library import load_library
from compat_harness.manifest import ManifestError, resolve_app
from compat_harness import runner
from triage import rank, main


class LibraryTriageTests(unittest.TestCase):
    def test_hash_index_and_missing_fixture_coverage(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'renamed.ipa').write_bytes(b'fixture')
            library = root / 'library.json'
            library.write_text(json.dumps({'apps': [dict(id='fixture', file='expected.ipa', sha256=hashlib.sha256(b'fixture').hexdigest())]}))
            app = load_library(library, root)['apps'][0]
            self.assertEqual(app['fixtureRelPath'], 'renamed.ipa')
            self.assertEqual(resolve_app(app, repo=directory, fixture_root=directory, allow_legacy=False)['status'], 'resolved')
            (root / 'renamed.ipa').write_bytes(b'changed')
            self.assertEqual(resolve_app(app, repo=directory, fixture_root=directory, allow_legacy=False)['status'], 'error')

    def test_library_rejects_traversal_and_invalid_hash(self):
        with tempfile.TemporaryDirectory() as directory:
            library = Path(directory) / 'library.json'
            for filename, digest in [('../outside.ipa', 'a' * 64), ('valid.ipa', 'invalid')]:
                library.write_text(json.dumps({'apps': [dict(id='fixture', file=filename, sha256=digest)]}))
                with self.assertRaises(ManifestError): load_library(library)

    def test_rank_deduplicates_apps_and_orders_by_impact(self):
        boot = dict(bundleId='a', signature='shared', exceptionKind='fault')
        later = dict(bundleId='b', signature='shared', exceptionKind='fault', perf={'framesPresented': 10})
        wrong = dict(id='c', frame={'signal': 'all-black'})
        report = dict(kind='hvm-compat-report', suites={'apps': {'apps': [boot, later, wrong]}})
        rows = rank([('report', report), ('duplicate', boot), ('cli', {'crashed': True, 'crashBundle': boot})])
        self.assertEqual(rows[0]['apps'], ['a', 'b'])
        self.assertEqual(rows[0]['score'], 6)
        self.assertEqual(rows[1]['severity'], 1)

    def test_keep_going_preserves_strict_result_and_separate_home(self):
        with tempfile.TemporaryDirectory() as directory:
            hvm = Path(directory) / 'hvm'
            hvm.touch()
            strict = dict(id='a', status='crashed', crashed=True)
            calls = []
            def run(*args, **kwargs):
                calls.append((args[3], kwargs))
                return dict(id='a', status='ran', crashed=False) if kwargs else strict.copy()
            with patch.object(runner, '_run_one', side_effect=run):
                output = runner.run_apps({'apps': [dict(id='a')]}, [dict(id='a', status='resolved')], hvm=str(hvm), out_dir=directory, seconds=30, keep_going=True)
            self.assertTrue(output['apps'][0]['crashed'])
            self.assertFalse(output['apps'][0]['keepGoing']['crashed'])
            self.assertNotEqual(calls[0][0], calls[1][0])

    def test_existing_scan_relaunch_caller_and_probe_import(self):
        from compat import macho_info
        from compat_harness.scan import main as scan
        self.assertTrue(callable(macho_info))
        with tempfile.TemporaryDirectory() as directory:
            ipa = Path(directory) / 'fixture.ipa'
            ipa.write_bytes(b'fixture')
            with patch('compat_harness.scan.harness_main', return_value=0) as run:
                self.assertEqual(scan(['scan', str(ipa), '--relaunch']), 0)
            command = run.call_args.args[0]
            self.assertEqual(command[command.index('--launches') + 1], '2')
            self.assertIn('--keep-going', command)
