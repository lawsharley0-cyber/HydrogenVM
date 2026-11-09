import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import device_report  # noqa: E402


def report(apps):
    return {'kind': 'hvm-compat-report', 'build': {'commit': 'abcdef1234567'},
            'device': {'model': 'iPhone', 'systemVersion': '27.0'},
            'suites': {'apps': {'apps': apps}}}


def app(bid, renderer='gpu', crashed=False, signal='ok', checks=(), gl=None, draws=10):
    row = {'bundleId': bid, 'name': bid, 'version': '1.0', 'renderer': renderer, 'crashed': crashed,
           'summary': 'Bad memory access' if crashed else 'ok', 'signature': 'sig' if crashed else None,
           'frame': {'signal': signal}, 'perf': {'framesPresented': 100, 'fps': 60},
           'checks': [{'name': n, 'status': s, 'detail': ''} for n, s in checks],
           'diagnostics': {'glDraws': draws, 'glErrors': len(gl or [])}}
    if gl:
        row['glErrorLog'] = gl
    return row


class DeviceReportTests(unittest.TestCase):
    def write(self, d, data):
        Path(d).mkdir(parents=True, exist_ok=True)
        (Path(d) / 'report.json').write_text(json.dumps(data))
        return Path(d)

    def test_problems_cover_crash_picture_checks_and_gl_errors(self):
        r = app('a', crashed=True)
        self.assertEqual(device_report.problems(r), ['crash: sig'])
        r = app('b', signal='all-black', checks=[('Launches', 'pass'), ('Shows a picture', 'fail'), ('Music', 'warn')],
                gl=['glDrawArrays -> 0x0506 [worker]'])
        self.assertEqual(device_report.problems(r), ['picture: all-black', 'fail: Shows a picture', 'warn: Music',
                                                     'gl-error: glDrawArrays -> 0x0506 [worker]'])

    def test_compare_reports_fixed_new_and_still(self):
        with tempfile.TemporaryDirectory() as t:
            old = self.write(Path(t) / 'old', report([app('a', signal='all-black'), app('b'), app('gone')]))
            new = self.write(Path(t) / 'new', report([app('a'), app('b', crashed=True), app('added')]))
            diff = device_report.compare(device_report.summarize(device_report.load(new)),
                                         device_report.summarize(device_report.load(old)))
            self.assertEqual(diff['fixed'], [('a [gpu]', 'picture: all-black')])
            self.assertEqual(diff['new'], [('b [gpu]', 'crash: sig')])
            self.assertEqual(diff['added'], ['added [gpu]'])
            self.assertEqual(diff['removed'], ['gone [gpu]'])
            out = io.StringIO()
            with redirect_stdout(out):
                self.assertEqual(device_report.main([str(new), '--previous', str(old)]), 0)
            self.assertIn('fixed (1):', out.getvalue())

    def test_gl_errors_compare_without_arguments(self):
        self.assertEqual(device_report.gl_key('glBindTexture(0x0de1, 1) -> 0x0502 [main]'), 'glBindTexture -> 0x0502 [main]')
        self.assertEqual(device_report.gl_key('glClear -> 0x0506 [main] (framebuffer 1 status 0x8cd6: color0 renderbuffer 1 0x0 0x8056)'),
                         'glClear -> 0x0506 [main]')

    def test_crash_clues_point_at_the_host_function_and_its_source(self):
        report = ("Crash: bad memory access at 0x00000004 (NULL pointer)\n  pc 0x00123456 crossy+1193046 foo + 0x10\n"
                  "  backtrace:\n    0x00123456 crossy+1193046\n  last host calls (oldest first):\n"
                  "    _malloc -> 0x10001000 from 0x00100000 crossy+1\n"
                  "    _CFBundleCopyResourceURL -> 0x00000000 from 0x00100004 crossy+5\n"
                  "  likely cause: _CFBundleCopyResourceURL returned 0 just before: if the app expected an object or buffer, that function is the likely cause\n")
        row = app('c', crashed=True)
        row.update(signature='crash-fault', crashReport=report)
        self.assertEqual(device_report.crash_key(row), 'Crash: bad memory access at 0x (NULL pointer) at crossy+1193046')
        c = device_report.clues(row)
        self.assertEqual(len(c['calls']), 2)
        self.assertTrue(c['likely'][0].startswith('_CFBundleCopyResourceURL returned 0'))
        self.assertEqual(device_report.implicated(row), ['_CFBundleCopyResourceURL'])
        where = device_report.locate('_CFBundleCopyResourceURL')
        self.assertIsNotNone(where)
        self.assertTrue(where.startswith('src/'))

    def test_shared_causes_rank_problems_several_games_have(self):
        cur = {'a [gpu]': {'problems': ['gl-error: glClear -> 0x0506 [main]', 'fail: Music'], 'implicated': ['_x']},
               'b [gpu]': {'problems': ['gl-error: glClear -> 0x0506 [main]'], 'implicated': ['_x']},
               'b [software]': {'problems': ['gl-error: glClear -> 0x0506 [main]'], 'implicated': []},
               'c [gpu]': {'problems': ['fail: Music', 'warn: Sound'], 'implicated': []},
               'd [gpu]': {'problems': ['warn: Sound'], 'implicated': []}}
        common = device_report.shared(cur)
        self.assertEqual(common[0], ('crash near host function _x', ['a', 'b']))
        self.assertNotIn('fail: Music', [p for p, _ in common])   # a symptom, not a cause
        self.assertIn(('gl-error: glClear -> 0x0506 [main]', ['a', 'b']), common)
        self.assertNotIn('warn: Sound', [p for p, _ in common])

    def test_rejects_other_json(self):
        with tempfile.TemporaryDirectory() as t:
            d = self.write(Path(t) / 'x', {'kind': 'something-else'})
            with self.assertRaises(SystemExit):
                device_report.load(d)


if __name__ == '__main__':
    unittest.main()
