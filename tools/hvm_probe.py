#!/usr/bin/env python3
"""Probe every game: launch it headless, interact with it, and collect every problem.

For each .ipa in --ipas the probe imports it once (into --apps), runs it with a
deterministic script of taps, swipes and a background/foreground cycle, then
reads the run's --report JSON, log and screenshot. Problems from all games are
grouped by cause and ranked, so one fix that helps many games comes first.

  python3 tools/hvm_probe.py                       # all games in ~/ipas, software renderer
  python3 tools/hvm_probe.py --modes software,gpu  # both renderers (like Test All Games)
  python3 tools/hvm_probe.py --only BadPiggies     # one game (substring of bundle id or file name)
  python3 tools/hvm_probe.py --baseline ~/probe/latest/summary.json   # show fixed / new / still

Output (--out, default ~/probe/<timestamp>, and ~/probe/latest points at it):
  summary.md     ranked issue list + per-game table (read this first)
  summary.json   the same, machine-readable (use as the next --baseline)
  evidence/      shared hvm-evidence/v1 runs, fingerprints and private artifact index
  <bundleId>-<mode>/  run.log, report.json, shot.png, script.txt
"""
import argparse
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import time
import zlib
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

# Isolated Python (-I) removes the script directory; load only trusted siblings.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from hvm_evidence import bundle_from_probe, current_source_provenance, write_bundle  # noqa: E402

HOME = Path.home()

# Severity: higher is worse. Fix from the top.
SEV = {'load': 100, 'crash': 100, 'hang': 90, 'picture': 80, 'offscreen': 70, 'error': 60,
       'fault': 50, 'close': 45, 'missing': 40, 'unimplemented': 30, 'warning': 20, 'silent': 10}


def norm(text):
    """Make messages from different runs comparable (addresses and counts vary)."""
    text = re.sub(r'0x[0-9a-fA-F]{5,}', '0x…', text)
    text = re.sub(r'(?<![\w.])\d{3,}(?![\w.])', 'N', text)
    return text.strip()[:240]


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def png_signal(path):
    """'ok', 'all-black', 'all-white' or 'no-picture' from a PNG, without third-party modules."""
    try:
        data = Path(path).read_bytes()
    except OSError:
        return 'no-picture'
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        return 'no-picture'
    pos, idat, w = 8, b'', 0
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b'IHDR':
            w, h, depth, ctype = struct.unpack('>IIBB', body[:10])
        elif kind == b'IDAT':
            idat += body
        pos += 12 + n
    if not w or depth != 8 or ctype not in (2, 6):
        return 'ok'   # unusual format: don't guess
    bpp = 3 if ctype == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * bpp
    prev = bytearray(stride)
    dark = bright = total = 0
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):   # undo PNG filters
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        if y % max(1, h // 24):
            continue
        for x in range(0, w, max(1, w // 24)):
            r, g, bl = line[x * bpp:x * bpp + 3]
            total += 1
            dark += r < 16 and g < 16 and bl < 16
            bright += r > 239 and g > 239 and bl > 239
    if total and dark / total > 0.97: return 'all-black'
    if total and bright / total > 0.97: return 'all-white'
    return 'ok'


def script_for(bundle_id, seconds):
    """A deterministic interaction script (same game -> same actions, so runs compare)."""
    rng = int(hashlib.sha1(bundle_id.encode()).hexdigest()[:8], 16)
    spots = [(160, 240), (160, 400), (160, 330), (160, 160), (60, 440), (260, 440),
             (100, 300), (220, 300), (40, 40), (280, 40), (160, 460), (160, 80)]
    swipes = [(160, 380, 160, 120), (160, 120, 160, 380), (280, 240, 40, 240), (40, 240, 280, 240)]
    args, t, i = [], 8.0, rng % len(spots)
    bg_at = round(seconds * 0.6, 1)
    while t < seconds - 6:
        if bg_at <= t < bg_at + 4:
            t += 2.5
            continue
        if i % 5 == 4:
            x1, y1, x2, y2 = swipes[(i // 5) % len(swipes)]
            args += ['--drag', f'{x1},{y1},{x2},{y2}@{t:.1f}']
        else:
            x, y = spots[i % len(spots)]
            args += ['--tap', f'{x},{y}@{t:.1f}']
        i += 1
        t += 2.5
    args += ['--lifecycle', f'bg@{bg_at},fg@{bg_at + 3:.1f}']
    return args


def import_app(hvm, ipa, apps_root):
    """Import once; reuse the folder while the .ipa is unchanged. Returns (bundleId, app path, home)."""
    st = ipa.stat()
    stamp = f'{ipa.name}:{st.st_size}:{int(st.st_mtime)}'
    for marker in apps_root.glob('*/.probe_source'):
        if marker.read_text().strip() == stamp:
            home = marker.parent
            apps = sorted(home.glob('*.app'))
            if apps:
                return home.name, apps[0], home
    out = subprocess.run([hvm, 'import', str(ipa), str(apps_root)], capture_output=True, text=True)
    m = re.search(r'^bundle: (.+)$', out.stdout, re.M)
    if out.returncode or not m:
        raise RuntimeError(f'import failed: {(out.stderr or out.stdout).strip()[:300]}')
    app = Path(m.group(1))
    home = app.parent
    (home / '.probe_source').write_text(stamp)
    return home.name, app, home


# By design, not bugs (networking is offline on purpose). Keep this list short and explain each entry.
EXPECTED = ('SCNetworkReachability (network reported offline)',)


def issues_from(rep, log_text, shot, timed_out):
    found = []
    def add(kind, text, detail=''):
        found.append({'kind': kind, 'severity': SEV[kind], 'key': f'{kind}: {norm(text)}',
                      'text': (text + (' ' + detail if detail else '')).strip()[:400]})
    if timed_out:
        add('hang', 'did not stop when asked (killed by the probe timeout)')
    if rep is None:
        tail = '\n'.join(log_text.strip().splitlines()[-5:])
        add('crash', 'no report written; log tail: ' + tail)
        return found
    if rep.get('loadError'):
        add('load', rep['loadError'])
    report = (rep.get('crashReport') or '').strip()
    if report.startswith('The game did not finish saving'):
        # Graceful-quit watchdog. Can be load-dependent: re-run the game alone (--jobs 1) before trusting it.
        add('close', 'did not finish quitting before the watchdog (re-run alone to confirm)')
    elif rep.get('crashed') or report:
        first = report.splitlines()[:1]
        add('crash', first[0] if first else 'crash', f"[signature {rep.get('signature') or '?'}]")
    frames = (rep.get('perf') or {}).get('framesPresented', 0)
    sig = png_signal(shot)
    if not (rep.get('crashed') and not report.startswith('The game did not finish saving')) and not rep.get('loadError'):
        if frames == 0:
            add('picture', 'no frames presented')
        elif sig in ('all-black', 'all-white'):
            add('picture', f'screen stays {sig}')
    if (rep.get('display') or {}).get('offscreenPresents', 0):
        add('offscreen', 'GL frames presented to a layer that is not in a visible window')
    if (rep.get('display') or {}).get('faultsToApp', 0):
        add('fault', 'memory faults passed to the game\'s own signal handler')
    for e in rep.get('errors') or []:
        add('error', e['message'])
    for name in rep.get('missingFunctions') or []:
        if not any(x in name for x in EXPECTED):
            add('missing', name)
    for name in rep.get('unimplementedImports') or []:
        add('unimplemented', name)
    for w in rep.get('warnings') or []:
        if w['message'].startswith('Missing: ') or any(x in w['message'] for x in EXPECTED):
            continue   # already reported under 'missing'
        add('warning', w['message'])
    mx = rep.get('mixer') or {}
    if mx.get('framesRendered', 0) and not mx.get('audibleFrames', 0) and not report:
        add('silent', 'no audible sound during the whole run')
    return found


def taint_info(rep):
    """(tainted, stub hits, distinct APIs, three most-called API names) from a run report.

    A tainted run called at least one missing or approximated API, so a clean-looking
    exit is not a compatibility verdict. Reports from older builds, and runs with no
    report at all, carry none of these fields and read as (False, 0, 0, []).
    """
    rep = rep or {}
    hits = rep.get('stubHits')
    hits = hits if isinstance(hits, int) and not isinstance(hits, bool) and hits > 0 else 0
    apis = rep.get('missingApis')
    apis = [a for a in apis if isinstance(a, dict) and isinstance(a.get('name'), str)] if isinstance(apis, list) else []

    def count(a):
        return a['count'] if isinstance(a.get('count'), int) else 0
    top = [a['name'] for a in sorted(apis, key=lambda a: (-count(a), a['name']))[:3]]
    return hits > 0, hits, len(apis), top


def tainted_runs(results):
    """The runs that hit a missing or approximated API, most hits first. Status is left as it was."""
    rows = [{'bundleId': r.get('bundleId'), 'mode': r.get('mode'), 'status': r.get('status'),
             'stubHits': r.get('stubHits', 0), 'distinctMissing': r.get('distinctMissing', 0),
             'topMissing': r.get('topMissing') or [], 'dir': r.get('dir')}
            for r in results if r.get('tainted')]
    return sorted(rows, key=lambda r: (-r['stubHits'], str(r['bundleId']), str(r['mode'])))


def tainted_md(rows):
    """summary.md section for tainted_runs(); says so when there are none."""
    md = ['', '## Tainted runs', '',
          'A tainted run called at least one missing or approximated API. It can still exit 0 with a '
          'healthy picture, so its status above is not a compatibility verdict.', '']
    if not rows:
        return md + ['No tainted runs.']
    md += ['| Game | Mode | Status | Stub hits | Distinct APIs | Most called | Run folder |', '|---|---|---|---:|---:|---|---|']
    for r in rows:
        # API names come from the guest: keep a name inside its table cell (no '|'), on one line (no CR/LF)
        # and inside its code span (no backtick).
        top = ', '.join('`' + n.replace('`', "'").replace('|', '/').replace('\r', '\\r').replace('\n', '\\n') + '`'
                        for n in r['topMissing'])
        md.append(f"| {r['bundleId']} | {r['mode']} | {r['status']} | {r['stubHits']} | {r['distinctMissing']} | {top} | `{r.get('dir', '')}` |")
    return md


def probe_one(args, ipa, mode):
    ipa_hash = file_sha256(ipa)
    try:
        bid, app, home = import_app(args.hvm, ipa, args.apps)
    except Exception as e:  # noqa: BLE001 - report every failure as an issue
        return {'bundleId': ipa.stem, 'ipa': ipa.name, 'sha256': ipa_hash, 'mode': mode, 'status': 'import-failed',
                'issues': [{'kind': 'load', 'severity': 100, 'key': f'load: {norm(str(e))}', 'text': str(e)}]}
    run_dir = args.out / f'{bid}-{mode}'
    run_dir.mkdir(parents=True, exist_ok=True)
    script = script_for(bid, args.seconds)
    extra = Path(__file__).resolve().parent / 'probe_scripts' / f'{bid}.txt'
    if extra.exists():   # per-game steps that get past a menu, e.g. "--tap 160,430@22" (one or more per line)
        script = extra.read_text().split() + script
    (run_dir / 'script.txt').write_text(' '.join(script) + '\n')
    cmd = [args.hvm, 'run', str(app), '--home', str(home), '--run-for', str(args.seconds), '--keep-going',
           '--graceful', '--report', str(run_dir / 'report.json'), '--screenshot', str(run_dir / 'shot.png')]
    if mode == 'gpu':
        cmd.append('--gpu')
    if args.launches > 1:
        cmd += ['--launches', str(args.launches)]
    cmd += script
    timed_out, started = False, time.time()
    with open(run_dir / 'run.log', 'w') as log:
        try:
            p = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, timeout=args.seconds * args.launches + 90)
            code = p.returncode
        except subprocess.TimeoutExpired:
            timed_out, code = True, -9
    log_text = (run_dir / 'run.log').read_text(errors='replace')
    try:
        rep = json.loads((run_dir / 'report.json').read_text())
    except (OSError, ValueError):
        rep = None
    issues = issues_from(rep, log_text, run_dir / 'shot.png', timed_out)
    worst = max([i['severity'] for i in issues], default=0)
    status = 'crash' if worst >= 90 else 'broken' if worst >= 70 else 'issues' if worst >= 45 else 'ok'
    perf = (rep or {}).get('perf') or {}
    tainted, stub_hits, distinct_missing, top_missing = taint_info(rep)
    return {'bundleId': bid, 'ipa': ipa.name, 'sha256': ipa_hash, 'mode': mode, 'status': status, 'exit': code,
            'tainted': tainted, 'stubHits': stub_hits, 'distinctMissing': distinct_missing, 'topMissing': top_missing,
            'seconds': round(time.time() - started, 1), 'framesPresented': perf.get('framesPresented', 0),
            'mixer': (rep or {}).get('mixer'), 'dir': str(run_dir), 'issues': issues,
            'signature': (rep or {}).get('signature'), 'exceptionKind': (rep or {}).get('exceptionKind'),
            'launches': args.launches}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ipas', type=Path, default=HOME / 'ipas')
    ap.add_argument('--apps', type=Path, default=HOME / 'apps')
    ap.add_argument('--out', type=Path)
    ap.add_argument('--hvm', default=str(Path(__file__).resolve().parent.parent / 'build' / 'hvm'))
    ap.add_argument('--seconds', type=int, default=60)
    ap.add_argument('--modes', default='software', help='software, gpu or software,gpu')
    ap.add_argument('--only', help='substring of the .ipa file name or bundle id')
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) // 2),
                    help='parallel games; keep it at half the CPUs or timings get unreliable')
    ap.add_argument('--launches', type=int, default=1, help='2 = relaunch on the saved data, like Test All Games')
    ap.add_argument('--baseline', type=Path, help='a previous summary.json to compare against')
    args = ap.parse_args()

    ipas = sorted(args.ipas.glob('*.ipa'))
    if args.only:
        ipas = [p for p in ipas if args.only.lower() in p.name.lower()] or \
               [p for p in ipas if args.only.lower() in p.name.lower().replace('_', '')]
    if not ipas:
        sys.exit(f'no .ipa files in {args.ipas}' + (f' matching {args.only!r}' if args.only else ''))
    args.apps.mkdir(parents=True, exist_ok=True)
    args.out = args.out or HOME / 'probe' / time.strftime('%Y%m%d-%H%M%S')
    args.out.mkdir(parents=True, exist_ok=True)
    modes = [m.strip() for m in args.modes.split(',') if m.strip()]
    jobs = [(ipa, m) for ipa in ipas for m in modes]
    print(f'probing {len(jobs)} runs ({len(ipas)} games x {modes}), {args.seconds}s each, {args.jobs} at a time -> {args.out}')
    results = []
    with ThreadPoolExecutor(args.jobs) as pool:
        for r in pool.map(lambda j: probe_one(args, *j), jobs):
            results.append(r)
            print(f"  {r['status']:13} {r['bundleId']} ({r['mode']}): {len(r['issues'])} issues")

    groups = {}
    for r in results:
        for i in r['issues']:
            g = groups.setdefault(i['key'], {'key': i['key'], 'kind': i['kind'], 'severity': i['severity'],
                                             'example': i['text'], 'runs': [], 'exampleDir': r.get('dir')})
            g['runs'].append(f"{r['bundleId']}({r['mode']})")
    ranked = sorted(groups.values(), key=lambda g: (-g['severity'], -len(g['runs']), g['key']))
    build, host = current_source_provenance()
    summary = {'kind': 'hvm-probe', 'time': time.strftime('%Y-%m-%d %H:%M:%S'), 'seconds': args.seconds,
               'modes': modes, 'launches': args.launches, 'build': build, 'host': host,
               'results': results, 'issues': ranked, 'taintedRuns': tainted_runs(results)}

    diff = None
    if args.baseline and args.baseline.exists():
        old = {g['key'] for g in json.loads(args.baseline.read_text()).get('issues', [])}
        new = {g['key'] for g in ranked}
        diff = {'fixed': sorted(old - new), 'new': sorted(new - old), 'still': len(old & new)}
        summary['baselineDiff'] = diff
    (args.out / 'summary.json').write_text(json.dumps(summary, indent=1))
    write_bundle(bundle_from_probe(summary), args.out / 'evidence')

    md = [f"# HydrogenVM probe {summary['time']}", '',
          f"{len(results)} runs, {args.seconds}s each, modes {', '.join(modes)}. "
          f"Fix from the top: severity first, then how many games share the issue.", '']
    if diff:
        md += [f"**Compared with baseline:** {len(diff['fixed'])} fixed, {len(diff['new'])} new, {diff['still']} still present.", '']
        md += [f'- ✅ fixed: `{k}`' for k in diff['fixed'][:40]] + [f'- 🆕 new: `{k}`' for k in diff['new'][:40]] + ['']
    md += ['## Games', '', '| Game | Mode | Status | Frames | Audible | Issues | Run folder |', '|---|---|---|---:|---:|---:|---|']
    for r in sorted(results, key=lambda r: (r['status'] == 'ok', r['bundleId'], r['mode'])):
        mx = r.get('mixer') or {}
        md.append(f"| {r['bundleId']} | {r['mode']} | {r['status']} | {r.get('framesPresented', 0)} | "
                  f"{mx.get('audibleFrames', 0)} | {len(r['issues'])} | `{r.get('dir', '')}` |")
    md += tainted_md(summary['taintedRuns'])
    md += ['', '## Issues (ranked)', '', '| # | Severity | Issue | Games | Example run |', '|---:|---:|---|---|---|']
    for n, g in enumerate(ranked, 1):
        if g['severity'] < 20 and n > 60:
            break
        games = ', '.join(g['runs'][:6]) + (f" +{len(g['runs']) - 6}" if len(g['runs']) > 6 else '')
        md.append(f"| {n} | {g['severity']} | `{g['key'].replace('|', '/')}` | {games} | `{g['exampleDir']}` |")
    (args.out / 'summary.md').write_text('\n'.join(md) + '\n')
    latest = args.out.parent / 'latest'
    try:
        if latest.is_symlink() or latest.exists():
            latest.unlink()
        latest.symlink_to(args.out.resolve())
    except OSError:
        pass
    print(f"\n{len(ranked)} distinct issues. Read {args.out / 'summary.md'}")


if __name__ == '__main__':
    main()
