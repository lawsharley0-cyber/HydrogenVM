"""hvm_scan.py merges per-game linking gaps into one ranked list (most games first)."""
import json
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import hvm_scan  # noqa: E402


def game(bundle, refs):
    linking = {'references': [{'kind': k, 'name': n, 'binding': b} for k, n, b in refs]}
    return {'ipa': bundle + '.ipa', 'bundleId': bundle, 'error': None, 'references': len(refs),
            'gaps': hvm_scan.gaps_from(linking)}


class ScanTest(unittest.TestCase):
    def setUp(self):
        self.results = [
            game('a', [('function', '_sem_open', 'missing'), ('function', '_strlen', 'implemented'),
                       ('class', 'NSPointerArray', 'placeholder class')]),
            game('b', [('function', '_sem_open', 'missing'), ('data', '_kFoo', 'zero-filled data')]),
            game('c', [('function', '_sem_open', 'stub'), ('selector', 'doThing:', 'missing'),
                       ('class', 'NSPointerArray', 'placeholder class')]),
            {'ipa': 'broken.ipa', 'bundleId': None, 'error': 'import failed: bad zip', 'gaps': []},
        ]

    def test_implemented_references_are_not_gaps(self):
        self.assertNotIn(('function', '_strlen', 'implemented'), self.results[0]['gaps'])

    def test_rank_orders_by_game_count_then_severity(self):
        ranked = hvm_scan.rank(self.results)
        self.assertEqual([(g['name'], len(g['games'])) for g in ranked[:2]], [('_sem_open', 3), ('NSPointerArray', 2)])
        # One game links _sem_open as a stub, two as missing: the row keeps the worse binding.
        self.assertEqual(ranked[0]['binding'], 'missing')
        self.assertEqual(ranked[0]['games'], ['a', 'b', 'c'])
        # Ties (one game each) are ordered by severity: missing selector before zero-filled data.
        self.assertEqual([g['name'] for g in ranked[2:]], ['doThing:', '_kFoo'])

    def test_baseline_marks_new_still_and_fixed(self):
        ranked = hvm_scan.rank(self.results)
        baseline = {'gaps': [{'kind': 'function', 'name': '_sem_open'}, {'kind': 'function', 'name': '_old_gone'}]}
        ranked, fixed = hvm_scan.compare(ranked, baseline)
        status = {g['name']: g['status'] for g in ranked}
        self.assertEqual(status['_sem_open'], 'still')
        self.assertEqual(status['NSPointerArray'], 'new')
        self.assertEqual(fixed, ['function _old_gone'])

    def test_markdown_lists_failures_and_ranks(self):
        ranked = hvm_scan.rank(self.results)
        md = hvm_scan.markdown(self.results, ranked, [], {}, limit=10)
        self.assertIn('4 .ipa files, 3 linked, 1 failed', md)
        self.assertIn('| 1 | 3 | function | missing | `_sem_open` |', md)
        # Selectors get their own table after the linked symbols.
        self.assertLess(md.index('## Ranked gaps: functions, classes, data'), md.index('## Selectors nothing implements'))
        self.assertIn('| 1 | 1 | selector | missing | `doThing:` |', md)
        self.assertIn('import failed: bad zip', md)

    def test_each_ipa_gets_its_own_import_folder_name(self):
        a = hvm_scan.safe_name('Terraria-(com.505games.terraria)-1.2.11587-(iOS_7.0)')
        b = hvm_scan.safe_name('Terraria-v1_0-eromeu52-iOS5_1-_Clutch-1_4_6_')
        self.assertNotEqual(a, b)
        self.assertTrue(all(c.isalnum() or c in '.-_' for c in a + b))
        self.assertEqual(hvm_scan.safe_name('../..'), 'ipa')

    def test_demangle_falls_back_to_the_raw_name(self):
        names = hvm_scan.demangle(['_plain', '__ZNSt3__16threadD1Ev'])
        self.assertEqual(names['_plain'], '_plain')
        self.assertIn('__ZNSt3__16threadD1Ev', names)


class DemangleTest(unittest.TestCase):
    def run_with(self, **kw):
        with mock.patch.object(hvm_scan.subprocess, 'run', **kw):
            return hvm_scan.demangle(['__ZN1a1bEv', '_plain'])

    def test_success_maps_each_name(self):
        done = subprocess.CompletedProcess([], 0, stdout='a::b()\n_plain\n', stderr='')
        self.assertEqual(self.run_with(return_value=done), {'__ZN1a1bEv': 'a::b()', '_plain': '_plain'})

    def test_missing_cxxfilt_keeps_raw_names(self):
        self.assertEqual(self.run_with(side_effect=OSError('no c++filt')),
                         {'__ZN1a1bEv': '__ZN1a1bEv', '_plain': '_plain'})

    def test_line_count_mismatch_keeps_raw_names(self):
        done = subprocess.CompletedProcess([], 0, stdout='only one line\n', stderr='')
        self.assertEqual(self.run_with(return_value=done)['__ZN1a1bEv'], '__ZN1a1bEv')


class ScanOneTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.games = self.root / 'games'
        self.games.mkdir()
        self.roots = []

    def tearDown(self):
        self.tmp.cleanup()

    def importer(self, _hvm, ipa, root):
        self.roots.append(root)
        return 'com.same.game', root / 'Game.app', root

    def linked(self, stdout='{"references": []}', code=0):
        return mock.patch.object(hvm_scan.subprocess, 'run',
                                 return_value=subprocess.CompletedProcess([], code, stdout=stdout, stderr='boom'))

    def test_two_ipas_of_one_game_get_distinct_roots_and_files(self):
        with self.linked():
            # Names that collapse to the same safe name still get different folders.
            for name in ('Game (1).ipa', 'Game [1].ipa'):
                r = hvm_scan.scan_one('hvm', Path(name), self.root, self.games, importer=self.importer)
                self.assertIsNone(r['error'])
        self.assertEqual(len(set(self.roots)), 2)
        self.assertEqual(len(list(self.games.glob('*.json'))), 2)

    def test_same_bundle_id_stays_two_rows(self):
        results = hvm_scan.label_games([
            {'ipa': 'v1.ipa', 'bundleId': 'com.x', 'error': None, 'gaps': [('function', '_f', 'missing')]},
            {'ipa': 'v2.ipa', 'bundleId': 'com.x', 'error': None, 'gaps': [('function', '_f', 'missing')]}])
        self.assertEqual(len(hvm_scan.rank(results)[0]['games']), 2)

    def test_failures_become_results(self):
        cases = [(dict(side_effect=subprocess.TimeoutExpired('hvm', 300)), 'timed out'),
                 (dict(side_effect=OSError('missing hvm')), 'could not run hvm'),
                 (dict(return_value=subprocess.CompletedProcess([], 1, stdout='', stderr='boom')), 'linking failed: boom'),
                 (dict(return_value=subprocess.CompletedProcess([], 0, stdout='{', stderr='')), 'bad linking JSON'),
                 (dict(return_value=subprocess.CompletedProcess([], 0, stdout='[1]', stderr='')), 'bad linking JSON'),
                 (dict(return_value=subprocess.CompletedProcess([], 0, stdout='{"references": 3}', stderr='')), 'bad linking JSON'),
                 (dict(return_value=subprocess.CompletedProcess([], 0, stdout='{"references": [1]}', stderr='')), 'bad linking JSON')]
        for kw, msg in cases:
            with mock.patch.object(hvm_scan.subprocess, 'run', **kw):
                r = hvm_scan.scan_one('hvm', Path('g.ipa'), self.root, self.games, importer=self.importer)
            self.assertIn(msg, r['error'])

    def test_import_failure_is_a_result(self):
        def bad(*_a):
            raise RuntimeError('bad zip')
        r = hvm_scan.scan_one('hvm', Path('g.ipa'), self.root, self.games, importer=bad)
        self.assertIn('import failed: bad zip', r['error'])


class ProbeFoldersTest(unittest.TestCase):
    def test_probe_gives_each_ipa_its_own_import_and_run_folder(self):
        import hvm_probe
        with tempfile.TemporaryDirectory() as d:
            seen = []

            def fake_import(_hvm, _ipa, root):
                seen.append(root)
                raise RuntimeError('stop after choosing folders')
            args = mock.Mock(apps=Path(d), out=Path(d) / 'out', hvm='hvm')
            with mock.patch.object(hvm_probe, 'import_app', side_effect=fake_import), \
                    mock.patch.object(hvm_probe, 'file_sha256', return_value='0'):
                a = hvm_probe.probe_one(args, Path('Terraria 1.0.ipa'), 'software')
                b = hvm_probe.probe_one(args, Path('Terraria 1.2.ipa'), 'software')
            self.assertNotEqual(seen[0], seen[1])
            self.assertNotEqual(a['ipaKey'], b['ipaKey'])


class MainTest(unittest.TestCase):
    def test_bad_baseline_fails_before_scanning(self):
        with tempfile.TemporaryDirectory() as d:
            bad = Path(d) / 'scan.json'
            bad.write_text('{"not": "a scan"}', encoding='utf-8')
            with mock.patch.object(hvm_scan, 'scan_one') as scan:
                for path in (bad, Path(d) / 'missing.json'):
                    with self.assertRaises(SystemExit):
                        hvm_scan.main(['--ipas', d, '--baseline', str(path)])
                scan.assert_not_called()

    def test_every_bad_baseline_is_a_clean_exit(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'scan.json'
            for content in (b'\xff', b'{', b'{"gaps": [1]}', b'{"gaps": [{"kind": "function"}]}'):
                path.write_bytes(content)
                with self.assertRaises(SystemExit, msg=content):
                    hvm_scan.load_baseline(path)
            path.write_bytes(b'{"gaps": [{"kind": "function", "name": "_f"}]}')
            self.assertEqual(hvm_scan.load_baseline(path)['gaps'][0]['name'], '_f')

    def test_latest_never_replaces_a_real_file(self):
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / 'run1'
            out.mkdir()
            (Path(d) / 'latest').write_text('keep me', encoding='utf-8')
            self.assertFalse(hvm_scan.point_latest(out))
            self.assertEqual((Path(d) / 'latest').read_text(encoding='utf-8'), 'keep me')
            (Path(d) / 'latest').unlink()
            self.assertTrue(hvm_scan.point_latest(out))
            self.assertTrue((Path(d) / 'latest').is_symlink())

    def test_markdown_written_as_utf8(self):
        with tempfile.TemporaryDirectory() as d:
            ipas = Path(d) / 'ipas'
            ipas.mkdir()
            (ipas / 'g.ipa').write_bytes(b'')
            fake = {'ipa': 'g.ipa', 'bundleId': 'com.g', 'error': None, 'references': 1,
                    'gaps': [('function', '_f\u2026', 'missing')]}
            # The reports must say UTF-8 explicitly: on a host whose default encoding is not
            # UTF-8 (Windows, a C locale) the default would fail on the '…' in scan.md.
            real_write = Path.write_text
            encodings = []

            def write_text(path, text, encoding=None, errors=None, newline=None):
                encodings.append(encoding)
                return real_write(path, text, encoding=encoding, errors=errors, newline=newline)
            with mock.patch.object(hvm_scan, 'scan_one', return_value=fake), \
                    mock.patch.object(Path, 'write_text', autospec=True, side_effect=write_text):
                self.assertEqual(hvm_scan.main(['--ipas', str(ipas), '--apps', d + '/apps', '--out', d + '/out']), 0)
            self.assertTrue(encodings and all(e == 'utf-8' for e in encodings), encodings)
            self.assertIn('\u2026', (Path(d) / 'out' / 'scan.md').read_bytes().decode('utf-8'))
            json.loads((Path(d) / 'out' / 'scan.json').read_text(encoding='utf-8'))


if __name__ == '__main__':
    unittest.main()
