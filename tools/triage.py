#!/usr/bin/env python3
"""Rank crash bundles and compatibility reports without changing their evidence."""
import argparse
import hashlib
import json
from pathlib import Path
import re


def rank(documents):
    groups = {}
    def ingest(row, fallback='unknown'):
        nested = row.get('crashBundle') or {}
        if nested:
            row = dict(row, **nested)
        app = row.get('bundleId') or row.get('id') or fallback
        perf = row.get('perf') or {}
        crash = bool(row.get('crashed') or row.get('exceptionKind') or row.get('crashReport') or row.get('loadError'))
        signal = (row.get('frame') or {}).get('signal')

        if row.get('explore'):
            for problem in row['explore'].get('newProblems', []):
                prob_summary = problem.get('problemText', '').split('\n')[0]
                prob_sig = 'prob-' + hashlib.sha1(prob_summary.encode()).hexdigest()[:12]
                pgroup = groups.setdefault(prob_sig, {'signature': prob_sig, 'apps': set(), 'severity': 1, 'summary': prob_summary})
                pgroup['apps'].add(app)
                pgroup['severity'] = max(pgroup['severity'], 1)

        if not crash and signal not in ('all-black', 'all-white', 'no-frames'):
            return
        severity = (2 if perf.get('framesPresented', 0) > 0 else 3) if crash else 1
        summary = row.get('summary') or row.get('crashReport') or row.get('report') or row.get('loadError') or signal
        signature = row.get('signature')
        if not signature:
            # Legacy reports have no normalized image offsets; mark this fallback explicitly.
            key = re.sub(r'0x[0-9a-fA-F]+', '<addr>', str(summary).split('\n')[0])
            signature = 'legacy-' + hashlib.sha1(key.encode()).hexdigest()[:12]
        group = groups.setdefault(signature, {'signature': signature, 'apps': set(), 'severity': 0, 'summary': str(summary).split('\n')[0]})
        group['apps'].add(app)
        group['severity'] = max(group['severity'], severity)
    for name, document in documents:
        if document.get('kind') == 'hvm-compat-report':
            for row in document.get('suites', {}).get('apps', {}).get('apps', []):
                ingest(row, name)
        else:
            ingest(document, name)
    return sorted((dict(group, apps=sorted(group['apps']), score=len(group['apps']) * group['severity']) for group in groups.values()),
                  key=lambda row: (-row['score'], -row['severity'], row['signature']))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    ingest = sub.add_parser('ingest')
    ingest.add_argument('reports', nargs='+')
    ingest.add_argument('--backlog', nargs='?', const='docs/dev-system/BACKLOG.md', help='append or refresh a delimited generated block')
    args = parser.parse_args(argv)
    rows = rank([(Path(path).stem, json.loads(Path(path).read_text())) for path in args.reports])
    lines = ['| Signature | Apps blocked | Severity | Score | Evidence |', '|---|---:|---|---:|---|']
    for row in rows:
        label = {3: 'crash at boot', 2: 'crash later', 1: 'wrong output'}[row['severity']]
        evidence = (', '.join(row['apps']) + ': ' + row['summary']).replace('|', '\\|')
        lines.append(f"| {row['signature']} | {len(row['apps'])} | {label} | {row['score']} | {evidence} |")
    output = '\n'.join(lines) + '\n'
    print(output, end='')
    if args.backlog:
        path = Path(args.backlog)
        begin, end = '<!-- triage:start -->', '<!-- triage:end -->'
        text = path.read_text() if path.exists() else ''
        block = begin + '\n' + output + end
        if begin in text and end in text:
            text = text[:text.index(begin)] + block + text[text.index(end) + len(end):]
        else:
            text += '\n' + block + '\n'
        path.write_text(text)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
