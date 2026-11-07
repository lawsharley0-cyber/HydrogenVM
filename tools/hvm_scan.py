#!/usr/bin/env python3
"""Static scan of a whole game library: what every game links against that HydrogenVM lacks.

For each .ipa in --ipas the scan imports it once (into --apps, shared with hvm_probe.py),
then asks `hvm info --linking <app> --json` for every imported function, data symbol,
Objective-C class and selector together with how the runtime binds it. Nothing is run:
a game takes a second or two, so the whole library is scanned in minutes.

The gaps are merged across games and ranked by how many games need them, so the first
rows are the fixes that help the most games. Run hvm_probe.py afterwards for crashes and
for the APIs games actually call.

  python3 tools/hvm_scan.py --ipas ~/test_ipa                 # scan every .ipa in the folder
  python3 tools/hvm_scan.py --ipas ~/test_ipa --jobs 4 --out ~/scan
  python3 tools/hvm_scan.py --baseline ~/scan/latest/scan.json  # mark gaps as new / still / fixed

Output (--out, default ~/scan/<timestamp>, and ~/scan/latest points at it):
  scan.md     ranked gaps (read this first) and a per-game table
  scan.json   the same, machine-readable (the next --baseline)
  games/<bundleId>.json   the raw `hvm info --linking` output for one game
"""
import argparse
import json
import os
import subprocess
import sys
import time
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hvm_probe import import_app  # noqa: E402  (same import cache as the probe)

HOME = Path.home()

# Bindings that are gaps, worst first. "implemented" is not a gap.
GAP_RANK = {
    'missing': 0,             # nothing behind it: a call jumps to a missing-API trap, data reads as 0
    'stub': 1,                # registered but reported as a stub
    'placeholder class': 2,   # a class the runtime made up so the app links
    'zero-filled data': 3,    # a data symbol filled with zeros
}
KIND_ORDER = {'function': 0, 'class': 1, 'data': 2, 'selector': 3}


def demangle(names):
    """Readable C++ names (one c++filt call for all of them); unchanged when c++filt is absent."""
    plain = [n[1:] if n.startswith('__Z') else n for n in names]
    try:
        out = subprocess.run(['c++filt'], input='\n'.join(plain), capture_output=True, text=True, timeout=60)
        lines = out.stdout.splitlines()
        if out.returncode == 0 and len(lines) == len(names):
            return dict(zip(names, lines))
    except (OSError, subprocess.SubprocessError):
        pass
    return {n: n for n in names}


def gaps_from(linking):
    """The references of one game that are gaps: (kind, name, binding) triples."""
    out = []
    for ref in linking.get('references', []):
        binding = ref.get('binding', '')
        if binding in GAP_RANK:
            out.append((ref.get('kind', '?'), ref.get('name', '?'), binding))
    return out


def scan_one(hvm, ipa, apps_root, games_dir):
    """Import and statically link one game. Returns a result dict; never raises."""
    t0 = time.time()
    try:
        bundle_id, app, _home = import_app(hvm, ipa, apps_root)
    except Exception as e:  # import failure is itself a finding
        return {'ipa': ipa.name, 'bundleId': None, 'error': f'import failed: {e}', 'gaps': [], 'seconds': time.time() - t0}
    out = subprocess.run([hvm, 'info', '--linking', str(app), '--json'], capture_output=True, text=True, timeout=300)
    if out.returncode:
        err = (out.stderr or out.stdout).strip().splitlines()
        return {'ipa': ipa.name, 'bundleId': bundle_id, 'error': f'linking failed: {err[-1] if err else out.returncode}',
                'gaps': [], 'seconds': time.time() - t0}
    try:
        linking = json.loads(out.stdout)
    except json.JSONDecodeError as e:
        return {'ipa': ipa.name, 'bundleId': bundle_id, 'error': f'bad linking JSON: {e}', 'gaps': [], 'seconds': time.time() - t0}
    (games_dir / f'{bundle_id}.json').write_text(out.stdout)
    refs = linking.get('references', [])
    return {'ipa': ipa.name, 'bundleId': bundle_id, 'error': None, 'references': len(refs),
            'gaps': gaps_from(linking), 'seconds': round(time.time() - t0, 2)}


def rank(results):
    """Merge gaps across games: one row per (kind, name), most games first."""
    rows = defaultdict(lambda: {'games': set(), 'binding': None})
    for r in results:
        for kind, name, binding in r['gaps']:
            row = rows[(kind, name)]
            row['games'].add(r['bundleId'] or r['ipa'])
            if row['binding'] is None or GAP_RANK[binding] < GAP_RANK[row['binding']]:
                row['binding'] = binding
    ranked = [{'kind': k, 'name': n, 'binding': v['binding'], 'games': sorted(v['games'])} for (k, n), v in rows.items()]
    ranked.sort(key=lambda x: (-len(x['games']), GAP_RANK[x['binding']], KIND_ORDER.get(x['kind'], 9), x['name']))
    return ranked


def compare(ranked, baseline):
    """Tag each gap 'new' or 'still' against a previous scan.json and list the fixed ones."""
    if not baseline:
        return ranked, []
    before = {(g['kind'], g['name']) for g in baseline.get('gaps', [])}
    now = {(g['kind'], g['name']) for g in ranked}
    for g in ranked:
        g['status'] = 'still' if (g['kind'], g['name']) in before else 'new'
    fixed = sorted(f'{k} {n}' for k, n in before - now)
    return ranked, fixed


def markdown(results, ranked, fixed, names, limit):
    games = [r for r in results if not r['error']]
    lines = ['# Library scan', '',
             f'{len(results)} .ipa files, {len(games)} linked, {sum(1 for r in results if r["error"])} failed. '
             f'{len(ranked)} distinct gaps.', '',
             'Rows are ranked by how many games need them: fix from the top.', '']
    if fixed:
        lines += [f'**Fixed since the baseline ({len(fixed)}):** ' + ', '.join(fixed[:40]) + (' …' if len(fixed) > 40 else ''), '']
    by_kind = defaultdict(int)
    for g in ranked:
        by_kind[g['kind']] += 1
    lines += ['| Kind | Gaps |', '|---|---:|'] + [f'| {k} | {by_kind[k]} |' for k in sorted(by_kind, key=lambda k: KIND_ORDER.get(k, 9))] + ['']
    lines += ['## Ranked gaps', '', '| # | Games | Kind | Binding | Symbol | Status | Which games |', '|---:|---:|---|---|---|---|---|']
    for i, g in enumerate(ranked[:limit], 1):
        who = ', '.join(g['games'][:6]) + (f' +{len(g["games"]) - 6}' if len(g['games']) > 6 else '')
        sym = names.get(g['name'], g['name']).replace('|', '\\|')
        lines.append(f'| {i} | {len(g["games"])} | {g["kind"]} | {g["binding"]} | `{sym}` | {g.get("status", "")} | {who} |')
    if len(ranked) > limit:
        lines.append(f'\n{len(ranked) - limit} more in scan.json.')
    lines += ['', '## Per game', '', '| Game | .ipa | References | Gaps | Missing functions | Note |', '|---|---|---:|---:|---:|---|']
    for r in sorted(results, key=lambda r: -len(r['gaps'])):
        missing_fn = sum(1 for k, _, b in r['gaps'] if k == 'function' and b == 'missing')
        lines.append(f'| {r["bundleId"] or "?"} | {r["ipa"]} | {r.get("references", "")} | {len(r["gaps"])} | {missing_fn} | {r["error"] or ""} |')
    return '\n'.join(lines) + '\n'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ipas', type=Path, default=HOME / 'ipas')
    ap.add_argument('--apps', type=Path, default=HOME / 'apps')
    ap.add_argument('--out', type=Path)
    ap.add_argument('--hvm', default=str(Path(__file__).resolve().parent.parent / 'build' / 'hvm'))
    ap.add_argument('--jobs', type=int, default=max(1, os.cpu_count() or 2))
    ap.add_argument('--only', help='substring of the .ipa file name')
    ap.add_argument('--baseline', type=Path, help='a previous scan.json')
    ap.add_argument('--limit', type=int, default=200, help='rows in scan.md (all rows are in scan.json)')
    args = ap.parse_args()

    ipas = sorted(p for p in args.ipas.glob('*.ipa') if not args.only or args.only.lower() in p.name.lower())
    if not ipas:
        print(f'no .ipa files in {args.ipas}', file=sys.stderr)
        return 2
    out = args.out or HOME / 'scan' / time.strftime('%Y%m%d-%H%M%S')
    games_dir = out / 'games'
    games_dir.mkdir(parents=True, exist_ok=True)
    args.apps.mkdir(parents=True, exist_ok=True)

    t0 = time.time()
    with ThreadPoolExecutor(args.jobs) as pool:
        results = list(pool.map(lambda ipa: scan_one(args.hvm, ipa, args.apps, games_dir), ipas))
    ranked = rank(results)
    baseline = json.loads(args.baseline.read_text()) if args.baseline and args.baseline.exists() else None
    ranked, fixed = compare(ranked, baseline)
    names = demangle([g['name'] for g in ranked])
    for g in ranked:
        g['pretty'] = names.get(g['name'], g['name'])

    (out / 'scan.json').write_text(json.dumps({'generated': time.strftime('%Y-%m-%dT%H:%M:%S'), 'seconds': round(time.time() - t0, 1),
                                               'games': results, 'gaps': ranked, 'fixed': fixed}, indent=1, default=list))
    (out / 'scan.md').write_text(markdown(results, ranked, fixed, names, args.limit))
    latest = out.parent / 'latest'
    try:
        if latest.is_symlink() or latest.exists():
            latest.unlink()
        latest.symlink_to(out.name)
    except OSError:
        pass
    failed = sum(1 for r in results if r['error'])
    print(f'scanned {len(results)} games in {time.time() - t0:.0f}s: {len(ranked)} distinct gaps, {failed} failed -> {out / "scan.md"}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
