#!/usr/bin/env python3
"""Recover a phone session from the one-tap reporter's GitHub comments."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess

REPO = 'lawsharley0-cyber/HydrogenVM'
LABEL = re.compile(r'\*\*session\.hvmrec(?: \(([1-9][0-9]*) of ([1-9][0-9]*)\))?\*\*')


def attachment(comment):
    label, _, body = comment['body'].partition('\n\n')
    match = LABEL.fullmatch(label)
    if not match:
        return None
    fence, newline, content = body.partition('\n')
    if fence not in ('```', '~~~~') or not newline or not content.endswith('\n' + fence):
        raise ValueError(f"Malformed recording in comment {comment['id']}")
    return (int(match[1] or 1), int(match[2] or 1), content[:-(1 + len(fence))])


def extract(comments, first_id=None):
    starts = [i for i, c in enumerate(comments)
              if (first_id is None or c['id'] == first_id)
              and (part := attachment(c)) and part[0] == 1]
    if not starts:
        raise ValueError('No matching first recording comment')
    if len(starts) > 1:
        ids = ', '.join(str(comments[i]['id']) for i in starts)
        raise ValueError(f'Multiple recordings. Select one with --first-comment from {ids}')
    start = starts[0]
    first = comments[start]
    count = attachment(first)[1]
    pieces = []
    for c in comments[start:]:
        if c['body'].startswith('**Seen again**') or c['body'].startswith('**report.json'):
            break
        part = attachment(c)
        if part is None:
            continue
        number, total, piece = part
        if number != len(pieces) + 1 or total != count or c['user']['id'] != first['user']['id']:
            raise ValueError('Recording parts are missing, out of order, or from different reporters')
        pieces.append(piece)
        if number == count:
            break
    if len(pieces) != count:
        raise ValueError(f'Incomplete recording ({len(pieces)} of {count} parts)')
    recording = ''.join(pieces)
    if not recording.startswith(('HVMREC 1 ', 'HVMREC 2 ')):
        raise ValueError('Unrecognized HVMREC header')
    return recording


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('issue', type=int)
    parser.add_argument('output', type=Path, help='new local file')
    parser.add_argument('--first-comment', type=int, help='first attachment comment ID for a repeat report')
    args = parser.parse_args()
    try:
        # --jq emits one object per line on every page, including older gh versions.
        raw = subprocess.check_output(['gh', 'api', '--paginate',
            f'repos/{REPO}/issues/{args.issue}/comments?per_page=100', '--jq', '.[] | @json'])
        comments = [json.loads(line) for line in raw.splitlines()]
        data = extract(comments, args.first_comment).encode('utf-8')
        fd = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'wb') as out:
            out.write(data)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'{error}\n')
    print(f'Saved {len(data)} bytes to {args.output}')


if __name__ == '__main__':
    main()
