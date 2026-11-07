#!/usr/bin/env python3
"""Static scan of a whole game library: what every game links against that HydrogenVM lacks.

For each .ipa in --ipas the scan imports it once (into --apps/<ipa name>/, reused while the .ipa is unchanged),
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
import hashlib
import json
import os
import subprocess
import sys
import time
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hvm_probe import import_app, import_root, safe_name  # noqa: E402,F401  (same import logic, folders and cache marker as the probe)

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
        out = subprocess.run(['c++filt'], input='\n'.join(plain), capture_output=True, text=True,
                             encoding='utf-8', errors='replace', timeout=60)
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


def scan_one(hvm, ipa, apps_root, games_dir, importer=import_app):
    """Import and statically link one game. Returns a result dict; never raises."""
    t0 = time.time()
    result = {'ipa': ipa.name, 'bundleId': None, 'error': None, 'gaps': [], 'seconds': 0}

    def fail(message):
        result['error'] = message
        result['seconds'] = round(time.time() - t0, 2)
        return result

    try:
        # Each .ipa gets its own import folder: two versions of one game share a bundle id,
        # and importing both into one folder (in parallel) makes them overwrite each other.
        root = import_root(apps_root, ipa)
        root.mkdir(parents=True, exist_ok=True)
        bundle_id, app, _home = importer(hvm, ipa, root)
    except Exception as e:  # an import failure is itself a finding
        return fail(f'import failed: {e}')
    result['bundleId'] = bundle_id
    try:
        out = subprocess.run([hvm, 'info', '--linking', str(app), '--json'], capture_output=True, text=True,
                             encoding='utf-8', errors='replace', timeout=300)
    except subprocess.TimeoutExpired:
        return fail('linking timed out after 300 s')
    except OSError as e:
        return fail(f'could not run hvm: {e}')
    if out.returncode:
        err = (out.stderr or out.stdout).strip().splitlines()
        return fail(f'linking failed: {err[-1] if err else out.returncode}')
    try:
        linking = json.loads(out.stdout)
    except json.JSONDecodeError as e:
        return fail(f'bad linking JSON: {e}')
    refs = linking.get('references') if isinstance(linking, dict) else None
    if not isinstance(refs, list) or not all(isinstance(ref, dict) for ref in refs):
        return fail('bad linking JSON: expected an object with a "references" list of objects')
    try:
        (games_dir / f'{import_root(Path(), ipa).name}.json').write_text(out.stdout, encoding='utf-8')
    except OSError as e:
        return fail(f'could not save the linking output: {e}')
    result.update(references=len(linking.get('references', [])), gaps=gaps_from(linking))
    result['seconds'] = round(time.time() - t0, 2)
    return result


def label_games(results):
    """A name per result for the tables: the bundle id, plus the .ipa name when two files
    share a bundle id (two versions of one game), so neither hides the other."""
    counts = defaultdict(int)
    for r in results:
        counts[r['bundleId']] += 1
    for r in results:
        bid = r['bundleId']
        r['label'] = r['ipa'] if not bid else (f'{bid} ({r["ipa"]})' if counts[bid] > 1 else bid)
    return results


def rank(results):
    """Merge gaps across games: one row per (kind, name), most games first."""
    rows = defaultdict(lambda: {'games': set(), 'binding': None})
    for r in results:
        for kind, name, binding in r['gaps']:
            row = rows[(kind, name)]
            row['games'].add(r.get('label') or r['bundleId'] or r['ipa'])
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
    # Functions, classes and data are what a game links against; selectors are messages
    # nobody answers (often SDK delegate callbacks), so they get their own, second table.
    def table(title, rows):
        out = [title, '', '| # | Games | Kind | Binding | Symbol | Status | Which games |', '|---:|---:|---|---|---|---|---|']
        for i, g in enumerate(rows[:limit], 1):
            who = ', '.join(g['games'][:6]) + (f' +{len(g["games"]) - 6}' if len(g['games']) > 6 else '')
            sym = names.get(g['name'], g['name']).replace('|', '\\|')
            out.append(f'| {i} | {len(g["games"])} | {g["kind"]} | {g["binding"]} | `{sym}` | {g.get("status", "")} | {who} |')
        if len(rows) > limit:
            out.append(f'\n{len(rows) - limit} more in scan.json.')
        return out + ['']
    lines += table('## Ranked gaps: functions, classes, data', [g for g in ranked if g['kind'] != 'selector'])
    lines += table('## Selectors nothing implements', [g for g in ranked if g['kind'] == 'selector'])
    lines += ['', '## Per game', '', '| Game | .ipa | References | Gaps | Missing functions | Note |', '|---|---|---:|---:|---:|---|']
    for r in sorted(results, key=lambda r: -len(r['gaps'])):
        missing_fn = sum(1 for k, _, b in r['gaps'] if k == 'function' and b == 'missing')
        lines.append(f'| {r.get("label") or r["bundleId"] or "?"} | {r["ipa"]} | {r.get("references", "")} | {len(r["gaps"])} | {missing_fn} | {r["error"] or ""} |')
    return '\n'.join(lines) + '\n'


def load_baseline(path):
    """The previous scan.json, read before any work so a bad path fails at once."""
    if path is None:
        return None
    try:
        data = json.loads(Path(path).read_text(encoding='utf-8'))
    except (OSError, ValueError) as e:   # ValueError covers bad JSON and bytes that are not UTF-8
        raise SystemExit(f'cannot read baseline {path}: {e}')
    if not isinstance(data, dict) or not isinstance(data.get('gaps'), list):
        raise SystemExit(f'baseline {path} is not a scan.json (no "gaps" list)')
    if not all(isinstance(g, dict) and isinstance(g.get('kind'), str) and isinstance(g.get('name'), str) for g in data['gaps']):
        raise SystemExit(f'baseline {path} has gap entries without a "kind" and "name"')
    return data


def point_latest(out):
    """Point <parent>/latest at this run. Only a symlink (or nothing) is ever replaced."""
    latest = out.parent / 'latest'
    if latest.exists() and not latest.is_symlink():
        print(f'note: {latest} exists and is not a link; not updated', file=sys.stderr)
        return False
    try:
        if latest.is_symlink():
            latest.unlink()
        latest.symlink_to(out.name)
        return True
    except OSError as e:
        print(f'note: could not update {latest}: {e}', file=sys.stderr)
        return False


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ipas', type=Path, default=HOME / 'ipas')
    ap.add_argument('--apps', type=Path, default=HOME / 'apps')
    ap.add_argument('--out', type=Path, help='output folder (default ~/scan/<timestamp>, which also updates ~/scan/latest)')
    ap.add_argument('--hvm', default=str(Path(__file__).resolve().parent.parent / 'build' / 'hvm'))
    ap.add_argument('--jobs', type=int, default=max(1, os.cpu_count() or 2))
    ap.add_argument('--only', help='substring of the .ipa file name')
    ap.add_argument('--baseline', type=Path, help='a previous scan.json')
    ap.add_argument('--limit', type=int, default=200, help='rows in scan.md (all rows are in scan.json)')
    args = ap.parse_args(argv)

    baseline = load_baseline(args.baseline)
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
    label_games(results)
    ranked = rank(results)
    ranked, fixed = compare(ranked, baseline)
    names = demangle([g['name'] for g in ranked])
    for g in ranked:
        g['pretty'] = names.get(g['name'], g['name'])

    (out / 'scan.json').write_text(json.dumps({'generated': time.strftime('%Y-%m-%dT%H:%M:%S'), 'seconds': round(time.time() - t0, 1),
                                               'games': results, 'gaps': ranked, 'fixed': fixed}, indent=1, default=list),
                                   encoding='utf-8')
    (out / 'scan.md').write_text(markdown(results, ranked, fixed, names, args.limit), encoding='utf-8')
    if args.out is None:
        point_latest(out)
    failed = sum(1 for r in results if r['error'])
    print(f'scanned {len(results)} games in {time.time() - t0:.0f}s: {len(ranked)} distinct gaps, {failed} failed -> {out / "scan.md"}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
