"""hvm_scan.py merges per-game linking gaps into one ranked list (most games first)."""
import sys
import unittest
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

    def test_demangle_falls_back_to_the_raw_name(self):
        names = hvm_scan.demangle(['_plain', '__ZNSt3__16threadD1Ev'])
        self.assertEqual(names['_plain'], '_plain')
        self.assertIn('__ZNSt3__16threadD1Ev', names)


if __name__ == '__main__':
    unittest.main()
