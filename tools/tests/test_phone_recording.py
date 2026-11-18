import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from phone_recording import extract


def comment(number, label, text, fence='```', author=17):
    return {'id': number, 'user': {'id': author}, 'body': f'**{label}**\n\n{fence}\n{text}\n{fence}'}


class PhoneRecordings(unittest.TestCase):
    def test_preserves_split_inside_line_and_final_newline(self):
        parts = [comment(7, 'session.hvmrec (1 of 2)', 'HVMREC 2 seed=ab\n123 4/5 clk'),
                 comment(8, 'session.hvmrec (2 of 2)', ' 0\n', '~~~~')]
        self.assertEqual(extract(parts), 'HVMREC 2 seed=ab\n123 4/5 clk 0\n')

    def test_incomplete_cannot_consume_another_upload(self):
        first = comment(1, 'session.hvmrec (1 of 2)', 'HVMREC 2 seed=1\n')
        second = comment(2, 'session.hvmrec (2 of 2)', 'end\n')
        for items in ([first], [first, comment(8, 'Seen again', ''), second],
                      [first, dict(second, user={'id': 18})]):
            with self.subTest(items=items), self.assertRaises(ValueError):
                extract(items)

    def test_repeat_report_requires_selection(self):
        reports = [comment(i, 'session.hvmrec', f'HVMREC 1 seed={i}') for i in (1, 2)]
        with self.assertRaisesRegex(ValueError, '--first-comment'):
            extract(reports)
        self.assertEqual(extract(reports, 2), 'HVMREC 1 seed=2')

    def test_malformed(self):
        for body in ('anything', '**session.hvmrec**\n\n```\ntruncated',
                     '**session.hvmrec**\n\n```\ninvalid header\n```'):
            with self.assertRaises(ValueError):
                extract([{'body': body, 'id': 1, 'user': {'id': 2}}])
