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

For each crash it also prints the crash report's clues (the crashing thread's
last host calls, "likely cause" lines) and where in src/ the implicated host
function is registered. "Shared causes" lists problems several games have in
common: one fix there helps all of them, so those come first.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


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
        out.append('crash: ' + crash_key(row))
    signal = (row.get('frame') or {}).get('signal')
    if signal and signal != 'ok' and not row.get('crashed'):
        out.append(f'picture: {signal}')
    for c in row.get('checks') or []:
        if c.get('status') in ('fail', 'warn'):
            out.append(f"{c.get('status')}: {c.get('name')}")
    for line in (row.get('glErrorLog') or [])[:8]:
        out.append(f'gl-error: {gl_key(line)}')
    return out


def crash_key(row: dict) -> str:
    """A crash's headline without run-specific numbers: 'Crash: bad memory access at 0x
    (NULL pointer) at crossy+N <symbol>'. Generic signatures ('crash-fault') say nothing."""
    sig = str(row.get('signature') or '')
    head = (str(row.get('crashReport') or row.get('summary') or sig or 'crash').splitlines() or ['crash'])[0]
    if sig and not sig.startswith('crash-') and sig != 'crash':
        head = sig.splitlines()[0]
    head = re.sub(r' at 0x[0-9a-fA-F]+ \S+\+\d+.*$', '', head)   # the site is added below, from the pc line
    head = re.sub(r'0x[0-9a-fA-F]+', '0x', head)
    # Where: the image offset of the pc (stable between runs of the same binary).
    m = re.search(r'^\s*pc 0x[0-9a-fA-F]+ (\S+\+\d+)', str(row.get('crashReport') or ''), re.M)
    if m:
        head += ' at ' + m.group(1)
    return head[:160]


def clues(row: dict) -> dict:
    """From a crash report: 'likely cause' lines and the last host calls (newest last)."""
    report = str(row.get('crashReport') or '')
    likely = [l.strip()[len('likely cause: '):] for l in report.splitlines() if l.strip().startswith('likely cause: ')]
    calls, inside = [], False
    for line in report.splitlines():
        if line.strip().startswith('last host calls'):
            inside = True
            continue
        if inside:
            if not line.startswith('    '):
                inside = False
                continue
            calls.append(line.strip())
    return {'likely': likely, 'calls': calls}


_SOURCES: list[tuple[Path, str]] | None = None


def locate(name: str) -> str | None:
    """Where a host function is registered in src/ ('src/runtime/libc/System.cpp:812'), so a
    report points at the code to change."""
    global _SOURCES
    if _SOURCES is None:
        _SOURCES = []
        for p in sorted((ROOT / 'src').rglob('*.cpp')):
            try:
                _SOURCES.append((p, p.read_text(encoding='utf-8', errors='replace')))
            except OSError:
                pass
    needle = '"' + name + '"'
    for p, text in _SOURCES:
        i = text.find(needle)
        if i >= 0:
            return f'{p.relative_to(ROOT)}:{text.count(chr(10), 0, i) + 1}'
    return None


def implicated(row: dict) -> list[str]:
    """Host functions a crash points at: named in 'likely cause' lines, else the last call."""
    c = clues(row)
    names = []
    # Names may be C functions (_malloc) or host methods (-[NSBundle pathForResource:ofType:]):
    # match up to the fixed text that follows the name.
    for l in c['likely']:
        m = re.match(r'(?:the crash happened inside )?(.+?)(?: returned 0 just before| \(or code it called back into\))', l)
        if m:
            names.append(m.group(1).strip())
    if not names and c['calls']:
        m = re.match(r'(.+?)(?: -> 0x[0-9a-fA-F]+| \(did not return\)) from ', c['calls'][-1])
        if m:
            names.append(m.group(1))
    return list(dict.fromkeys(names))


def shared(cur: dict[str, dict]) -> list[tuple[str, list[str]]]:
    """Problems more than one game has, most games first: one fix helps all of them."""
    games: dict[str, set[str]] = defaultdict(set)
    for k, g in cur.items():
        bundle = k.rsplit(' [', 1)[0]
        for p in g['problems']:
            games[p].add(bundle)
        for fn in g.get('implicated', []):
            games[f'crash near host function {fn}'].add(bundle)
    # Failing checks ("Launches", "Shows a picture") are symptoms; group the causes.
    out = [(p, sorted(b)) for p, b in games.items() if len(b) > 1 and not p.startswith(('warn: ', 'fail: '))]
    return sorted(out, key=lambda t: (-len(t[1]), t[0]))


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
            'clues': clues(row) if row.get('crashed') else {'likely': [], 'calls': []},
            'implicated': implicated(row) if row.get('crashed') else [],
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
        print(json.dumps({'build': report.get('build'), 'device': report.get('device'), 'games': cur, 'diff': diff,
                          'shared': [{'problem': p, 'games': b} for p, b in shared(cur)]}, indent=1))
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
        for line in g['clues']['likely']:
            print(f'         likely cause: {line}')
        for line in g['clues']['calls'][-4:]:
            print(f'         last call: {line}')
        for fn in g['implicated']:
            where = locate(fn)
            if where:
                print(f'         {fn} is implemented at {where}')
    common = shared(cur)
    if common:
        print('\nshared causes (fix once, helps every game listed):')
        for p, bundles in common[:15]:
            print(f'  {len(bundles)} games: {p}')
            print('      ' + ', '.join(bundles))
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
