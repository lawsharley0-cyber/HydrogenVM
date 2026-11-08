#!/usr/bin/env python3
"""Read a phone "Test All Games" export and compare it with an earlier one.

The iPhone app's Test All Games screen exports one folder per run
(HydrogenVM-test-<time>/ with report.json, report.md, thumbnails/, logs/).
Save it anywhere the desktop can see it (the desktop's test_ipa folder), then:

  tools/device_report.py <export>                      per-game summary
  tools/device_report.py <export> --previous <older>   plus fixed / new / still failing
  tools/device_report.py <export> --json               machine-readable summary

A game's "problems" are its failing and warning checks, a crash, a blank screen
and GL errors, so two runs compare the same way the desktop probe's do.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


def load(path: Path) -> dict:
    p = path / 'report.json' if path.is_dir() else path
    try:
        data = json.loads(p.read_text(encoding='utf-8'))
    except (OSError, ValueError) as error:
        raise SystemExit(f'{p}: cannot read the report ({error})')
    if data.get('kind') != 'hvm-compat-report':
        raise SystemExit(f'{p}: not a HydrogenVM Test All Games report')
    return data


def rows(report: dict) -> list[dict]:
    return ((report.get('suites') or {}).get('apps') or {}).get('apps') or []


def key(row: dict) -> str:
    return f"{row.get('bundleId', '?')} [{row.get('renderer', '?')}]"


def problems(row: dict) -> list[str]:
    """Stable, comparable descriptions of what went wrong in one run of one game."""
    out: list[str] = []
    if row.get('crashed'):
        out.append('crash: ' + (str(row.get('signature') or row.get('summary') or 'crash')).splitlines()[0][:160])
    signal = (row.get('frame') or {}).get('signal')
    if signal and signal != 'ok' and not row.get('crashed'):
        out.append(f'picture: {signal}')
    for c in row.get('checks') or []:
        if c.get('status') in ('fail', 'warn'):
            out.append(f"{c.get('status')}: {c.get('name')}")
    for line in (row.get('glErrorLog') or [])[:8]:
        out.append(f'gl-error: {gl_key(line)}')
    return out


def gl_key(line: str) -> str:
    """'glBindTexture(0x0de1, 1) -> 0x0502 [main] (framebuffer ...)' -> 'glBindTexture -> 0x0502 [main]':
    the arguments and framebuffer details vary between runs, the call and error do not."""
    m = re.match(r'(\w+)(?:\([^)]*\))?\s*(->\s*0x[0-9a-fA-F]+\s*\[\w+\])', line)
    return f'{m.group(1)} {m.group(2)}' if m else line


def summarize(report: dict) -> dict[str, dict]:
    result = {}
    for row in rows(report):
        diag = row.get('diagnostics') or {}
        found = problems(row)
        result[key(row)] = {
            'name': row.get('name', ''),
            'version': row.get('version', ''),
            'ok': not found,
            'problems': found,
            'frames': (row.get('perf') or {}).get('framesPresented', 0),
            'fps': (row.get('perf') or {}).get('fps', 0),
            'glDraws': diag.get('glDraws', 0),
            'glErrors': diag.get('glErrors', 0),
            'summary': row.get('summary', ''),
            'glErrorLog': row.get('glErrorLog') or [],
        }
    return result


def compare(new: dict[str, dict], old: dict[str, dict]) -> dict[str, list]:
    diff = {'fixed': [], 'new': [], 'still': [], 'added': [], 'removed': []}
    for k, cur in sorted(new.items()):
        if k not in old:
            diff['added'].append(k)
            continue
        before, after = set(old[k]['problems']), set(cur['problems'])
        for p in sorted(before - after):
            diff['fixed'].append((k, p))
        for p in sorted(after - before):
            diff['new'].append((k, p))
        for p in sorted(after & before):
            diff['still'].append((k, p))
    diff['removed'] = sorted(set(old) - set(new))
    return diff


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('export', type=Path, help='exported folder (or its report.json)')
    ap.add_argument('--previous', type=Path, help='an earlier export to compare with')
    ap.add_argument('--json', action='store_true', help='print JSON instead of text')
    a = ap.parse_args(argv)

    report = load(a.export)
    cur = summarize(report)
    diff = compare(cur, summarize(load(a.previous))) if a.previous else None
    if a.json:
        print(json.dumps({'build': report.get('build'), 'device': report.get('device'), 'games': cur, 'diff': diff}, indent=1))
        return 0

    build = (report.get('build') or {}).get('commit', '?')
    device = report.get('device') or {}
    print(f"build {str(build)[:12]} on {device.get('model', '?')} iOS {device.get('systemVersion', '?')}: "
          f"{sum(g['ok'] for g in cur.values())}/{len(cur)} runs without problems")
    for k, g in sorted(cur.items()):
        mark = 'ok  ' if g['ok'] else 'FAIL'
        print(f"{mark} {g['name']} {g['version']} {k}  frames={g['frames']} draws={g['glDraws']} glErrors={g['glErrors']}")
        for p in g['problems']:
            print(f'       - {p}')
        for line in g['glErrorLog'][:8]:
            print(f'         gl: {line}')
    if diff:
        for label in ('fixed', 'new', 'still'):
            print(f"\n{label} ({len(diff[label])}):")
            for k, p in diff[label]:
                print(f'  {k}: {p}')
        if diff['added']:
            print('\nonly in this run: ' + ', '.join(diff['added']))
        if diff['removed']:
            print('only in the previous run: ' + ', '.join(diff['removed']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
