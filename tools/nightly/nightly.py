#!/usr/bin/env python3
"""HydrogenVM nightly corpus run.

Runs every .ipa and .apk in a fixtures folder through `hvm run --phone`, classifies each game,
compares it with the previous night and writes:

  <state>/nights/<stamp>/status.json    every game's result tonight (feeds the compatibility list)
  <state>/nights/<stamp>/summary.md     what changed since the previous night
  <state>/nights/<stamp>/shots/*.png    screenshots (stay on this machine)
  <state>/latest.json                   a copy of tonight's status

With --publish it opens one GitHub issue, only when a game got worse, naming the game and the
commit range to bisect. Game files and screenshots never leave the machine; the issue holds only
names, statuses and API names.

    python3 tools/nightly/nightly.py --fixtures /mnt/c/Users/<you>/Desktop/test_ipa

Optional sidecar files next to a game, same name with another extension:
  <game>.taps     one "seconds x y" per line (portrait points), passed to hvm as --tap
  <game>.skip     present: leave the game out tonight
"""
import argparse
import datetime
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))

import compare  # noqa: E402

DEFAULT_SECONDS = 45


def git_commit(repo: Path) -> str:
    try:
        return subprocess.run(["git", "-C", str(repo), "rev-parse", "HEAD"], capture_output=True,
                              text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def find_games(fixtures: Path) -> list:
    games = []
    for path in sorted(fixtures.iterdir(), key=lambda p: p.name.lower()):
        if path.suffix.lower() in (".ipa", ".apk") and path.is_file() and not path.with_suffix(".skip").exists():
            games.append(path)
    return games


def find_all_games(fixture_dirs: list) -> list:
    """Collect (path, unique_game_name, shot_stem) across one or more fixture folders."""
    raw = []
    name_counts = {}
    stem_counts = {}
    for folder in fixture_dirs:
        for path in find_games(folder):
            raw.append((folder, path))
            name_counts[path.name] = name_counts.get(path.name, 0) + 1
            stem_counts[path.stem] = stem_counts.get(path.stem, 0) + 1
    out = []
    seen_names = set()
    seen_stems = set()
    for folder, path in raw:
        game_name = path.name if name_counts[path.name] == 1 else f"{folder.name}/{path.name}"
        if game_name in seen_names:
            idx = 2
            while f"{folder.name}#{idx}/{path.name}" in seen_names:
                idx += 1
            game_name = f"{folder.name}#{idx}/{path.name}"
        seen_names.add(game_name)

        shot_stem = path.stem if stem_counts[path.stem] == 1 else f"{folder.name}_{path.stem}"
        if shot_stem in seen_stems:
            idx = 2
            while f"{shot_stem}_{idx}" in seen_stems:
                idx += 1
            shot_stem = f"{shot_stem}_{idx}"
        seen_stems.add(shot_stem)
        out.append((path, game_name, shot_stem))
    return out


def tap_args(game: Path) -> list:
    taps = game.with_suffix(".taps")
    if not taps.exists():
        return []
    args = []
    for line in taps.read_text().splitlines():
        parts = line.split("#", 1)[0].split()
        if len(parts) == 3:
            seconds, x, y = parts
            args += ["--tap", f"{x},{y}@{seconds}"]
    return args


def run_game(hvm: str, game: Path, shots: Path, seconds: int,
             game_name: str = "", shot_stem: str = "") -> dict:
    with tempfile.TemporaryDirectory(prefix="hvm-nightly-") as tmp:
        home = Path(tmp) / "home"
        home.mkdir()
        report = Path(tmp) / "report.json"
        shot = shots / ((shot_stem or game.stem) + ".png")
        cmd = [hvm, "run", str(game), "--phone", "--home", str(home), "--run-for", str(seconds),
               "--screenshot", str(shot), "--report", str(report), "--quiet"]
        if game.suffix.lower() == ".apk":
            cmd.append("--gpu")
        cmd += tap_args(game)
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=seconds * 4 + 120)
            exit_code, tail = proc.returncode, (proc.stdout + proc.stderr)[-4000:]
        except subprocess.TimeoutExpired:
            exit_code, tail = None, "hvm did not stop in time"
        data = {}
        if report.exists():
            try:
                data = json.loads(report.read_text())
            except ValueError:
                data = {}
    result = compare.classify(data, exit_code, tail)
    result["game"] = game_name or game.name
    result["platform"] = "Android" if game.suffix.lower() == ".apk" else "iOS"
    result["screenshot"] = str(shot) if shot.exists() else ""
    return result


def publish(summary: dict, markdown: str, repo: str) -> str:
    title = f"Nightly: {len(summary['regressions'])} game(s) got worse ({summary['date']})"
    try:
        out = subprocess.run(["gh", "issue", "create", "--repo", repo, "--title", title, "--body", markdown,
                              "--label", "nightly-regression"], capture_output=True, text=True)
        if out.returncode != 0:     # the label may not exist yet
            out = subprocess.run(["gh", "issue", "create", "--repo", repo, "--title", title, "--body", markdown],
                                 capture_output=True, text=True)
        return out.stdout.strip() or out.stderr.strip()
    except OSError as e:
        return f"gh not available: {e}"


def main() -> int:
    ap = argparse.ArgumentParser(description="HydrogenVM nightly corpus run")
    ap.add_argument("--fixtures", action="append", required=True,
                    help="folder holding .ipa and/or .apk files (repeatable)")
    ap.add_argument("--hvm", default=str(ROOT / "build" / "hvm"))
    ap.add_argument("--state", default=os.path.expanduser("~/.hvm-nightly"))
    ap.add_argument("--seconds", type=int, default=DEFAULT_SECONDS)
    ap.add_argument("--only", help="comma-separated file names")
    ap.add_argument("--publish", action="store_true", help="open a GitHub issue when a game got worse")
    ap.add_argument("--repo", default="lawsharley0-cyber/HydrogenVM")
    args = ap.parse_args()

    fixture_dirs = [Path(f) for f in args.fixtures]
    for fixtures in fixture_dirs:
        if not fixtures.is_dir():
            print(f"[nightly] fixtures folder not found: {fixtures}", file=sys.stderr)
            return 2
    if not Path(args.hvm).exists():
        print(f"[nightly] hvm not found: {args.hvm}", file=sys.stderr)
        return 2
    games = find_all_games(fixture_dirs)
    if args.only:
        wanted = {n.strip() for n in args.only.split(",")}
        games = [(p, name, stem) for (p, name, stem) in games if p.name in wanted or name in wanted]
    if not games:
        print("[nightly] no .ipa or .apk files to run", file=sys.stderr)
        return 2

    state = Path(args.state)
    now = datetime.datetime.now()
    night = state / "nights" / now.strftime("%Y-%m-%d_%H%M")
    shots = night / "shots"
    shots.mkdir(parents=True, exist_ok=True)
    previous = compare.load(state / "latest.json")

    results = []
    for game, game_name, shot_stem in games:
        print(f"[nightly] {game_name} ...", flush=True)
        r = run_game(args.hvm, game, shots, args.seconds, game_name=game_name, shot_stem=shot_stem)
        print(f"[nightly]   {r['status']}" + (f" — {r['detail']}" if r["detail"] else ""), flush=True)
        results.append(r)

    status = {"schemaVersion": 1, "date": now.isoformat(timespec="seconds"), "commit": git_commit(ROOT),
              "seconds": args.seconds, "games": results}
    summary = compare.compare(previous, status, compare.screenshot_difference)
    status["regressions"] = summary["regressions"]
    markdown = compare.summary_markdown(summary)

    (night / "status.json").write_text(json.dumps(status, indent=2))
    (night / "summary.md").write_text(markdown)
    shutil.copyfile(night / "status.json", state / "latest.json")
    print(markdown)

    if summary["regressions"] and args.publish:
        print("[nightly] issue: " + publish(summary, markdown, args.repo))
    return 1 if summary["regressions"] else 0


if __name__ == "__main__":
    sys.exit(main())
