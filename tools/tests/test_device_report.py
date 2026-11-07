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

    def test_rejects_other_json(self):
        with tempfile.TemporaryDirectory() as t:
            d = self.write(Path(t) / 'x', {'kind': 'something-else'})
            with self.assertRaises(SystemExit):
                device_report.load(d)


if __name__ == '__main__':
    unittest.main()
