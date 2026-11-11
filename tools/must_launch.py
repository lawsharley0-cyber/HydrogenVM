#!/usr/bin/env python3
"""Must-launch check: every game that works has to keep working.

Runs each game listed in tools/must_launch.json on the desktop bench and fails
if one that used to reach its title screen (and play sound) no longer does.
Run it before every build that goes to a phone.

    tools/must_launch.py --apps ~/bench/apps                 # check
    tools/must_launch.py --apps ~/bench/apps --only subway   # some games
    tools/must_launch.py --apps ~/bench/apps --record        # rewrite the list
                                                             # from what works now

The games are private files, so this runs where the games are, not in CI.
A game whose folder is missing is reported as skipped, never as passed.

--record only ever adds games or raises what is expected of them. A game
already in the list that fails stays in the list with its old expectations:
dropping a broken game from the list is how a regression gets hidden.
"""
import argparse
import concurrent.futures
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_MANIFEST = os.path.join(HERE, "must_launch.json")
LIT_FRACTION = 0.05      # share of pixels that must not be black for "shows a picture"
LIT_LEVEL = 24           # a pixel is lit when any channel is above this
FRAME_MARGIN = 0.3       # a recorded game must keep at least this share of its frames (runs share the machine)


def lit_fraction(png_path):
    """Share of the screenshot's pixels that are not black; None if it cannot be read."""
    try:
        from PIL import Image
        with Image.open(png_path) as im:
            raw = im.convert("RGB").resize((64, 64)).tobytes()
    except Exception:
        return None
    pixels = [raw[i:i + 3] for i in range(0, len(raw), 3)]
    return sum(1 for p in pixels if max(p) > LIT_LEVEL) / len(pixels)


def measure(report, lit):
    """What one run showed, from the runner's report and the screenshot."""
    perf = report.get("perf") or {}
    mixer = report.get("mixer") or {}
    return {
        "crashed": bool(report.get("crashed")) or bool(report.get("loadError")),
        "frames": int(perf.get("framesPresented") or 0),
        "lit": lit,
        "audibleFrames": int(mixer.get("audibleFrames") or 0),
        "signature": str(report.get("signature") or "")[:160],
    }


def evaluate(expect, got):
    """Problems with a run against what the list expects. Empty means it passed."""
    problems = []
    if got is None:
        return ["the run produced no report"]
    if got["crashed"]:
        problems.append("crashed" + (": " + got["signature"] if got.get("signature") else ""))
    if got["frames"] < expect.get("frames", 1):
        problems.append("presented %d frames, needs %d" % (got["frames"], expect.get("frames", 1)))
    if expect.get("picture", True):
        if got["lit"] is None:
            problems.append("no screenshot to check the picture")
        elif got["lit"] < LIT_FRACTION:
            problems.append("black screen (%.0f%% of the picture lit)" % (got["lit"] * 100))
    if expect.get("sound") and got["audibleFrames"] <= 0:
        problems.append("silent, used to play sound")
    return problems


def expectation(entry, got):
    """The list entry for a game after a recording run.

    Never weaker than the entry it replaces: a broken run keeps the old entry,
    and a working one cannot lower the frame count or drop picture or sound.
    """
    works = got is not None and not got["crashed"] and got["frames"] > 0 and (got["lit"] or 0) >= LIT_FRACTION
    if not works:
        return entry if entry.get("frames") else None
    new = dict(entry)
    new["frames"] = max(int(entry.get("frames", 0)), max(1, int(got["frames"] * FRAME_MARGIN)))
    new["picture"] = True
    new["sound"] = bool(entry.get("sound")) or got["audibleFrames"] > 0
    return new


def listed(entries):
    """The entries that go in the list file, by name: only games with frames expected."""
    return sorted((g for g in entries if g.get("frames")), key=lambda g: g["name"].lower())


def find_app(apps_dir, folder):
    root = os.path.join(apps_dir, folder)
    if not os.path.isdir(root):
        return None
    for base, dirs, _files in os.walk(root):
        if base[len(root):].count(os.sep) > 3:
            dirs[:] = []
            continue
        for d in dirs:
            if d.endswith(".app"):
                return os.path.join(base, d)
    return None


def run_game(hvm, app, seconds, tap=None):
    work = tempfile.mkdtemp(prefix="must_launch_")
    try:
        report_path = os.path.join(work, "report.json")
        shot = os.path.join(work, "shot.png")
        cmd = [hvm, "run", app, "--home", os.path.join(work, "home"), "--keep-going", "--run-for", str(seconds),
               "--gpu", "--report", report_path, "--screenshot", shot, "--quiet"]
        if tap:
            cmd += ["--tap", tap]
        try:
            subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=seconds + 90, check=False)
        except subprocess.TimeoutExpired:
            return None
        try:
            with open(report_path) as f:
                report = json.load(f)
        except (OSError, ValueError):
            return None
        return measure(report, lit_fraction(shot))
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--apps", required=True, help="folder with one sub-folder per extracted game")
    ap.add_argument("--hvm", default=os.path.join(HERE, "..", "build", "hvm"))
    ap.add_argument("--manifest", default=DEFAULT_MANIFEST)
    ap.add_argument("--only", action="append", default=[], help="only games whose name or folder contains this")
    ap.add_argument("--seconds", type=int, default=None,
                    help="how long each game runs (30); with --record it is stored for the games recorded")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--record", action="store_true", help="rewrite the list from what works now (never weakens it)")
    args = ap.parse_args(argv)

    try:
        with open(args.manifest) as f:
            manifest = json.load(f)
    except OSError:
        manifest = {"games": []}
    entries = {g["folder"]: g for g in manifest.get("games", [])}
    if args.record:
        folders = sorted(f for f in os.listdir(args.apps) if os.path.isdir(os.path.join(args.apps, f)))
        for folder in folders:
            # "<name>-<10 hex digits>" beside "<name>" is a second copy of the same game
            copy = re.match(r"^(.*)-[0-9a-f]{10}$", folder)
            if copy and copy.group(1) in folders:
                continue
            if folder not in entries:
                entries[folder] = {"name": folder, "folder": folder}

    def wanted(g):
        return not args.only or any(o.lower() in (g["name"] + " " + g["folder"]).lower() for o in args.only)

    todo = [g for g in entries.values() if wanted(g)]
    default_seconds = 30
    if args.record and args.seconds:
        # Frames are only comparable for the same running time: keep it with the entry.
        for g in todo:
            g["seconds"] = args.seconds
    elif args.seconds:
        default_seconds = args.seconds
    results = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        futures = {}
        for g in todo:
            app = find_app(args.apps, g["folder"])
            if app is None:
                results[g["folder"]] = "skipped"
                continue
            futures[pool.submit(run_game, args.hvm, app, g.get("seconds", default_seconds), g.get("tap"))] = g
        for fut in concurrent.futures.as_completed(futures):
            results[futures[fut]["folder"]] = fut.result()

    # A game that fails beside three others gets a second run on its own, for twice as long:
    # the bench shares one machine, and a slow start is not a regression.
    retried = []
    if not args.record:
        for g in todo:
            got = results.get(g["folder"])
            if got == "skipped" or not evaluate(g, got):
                continue
            app = find_app(args.apps, g["folder"])
            if app is None:
                continue
            again = run_game(args.hvm, app, g.get("seconds", default_seconds) * 2, g.get("tap"))
            if not evaluate(g, again):
                retried.append(g["name"])
                print("retry %s passed on its own (first run: %s)" % (g["name"], "; ".join(evaluate(g, got))))
                results[g["folder"]] = again

    failed = skipped = passed = 0
    for g in sorted(todo, key=lambda g: g["name"].lower()):
        got = results.get(g["folder"])
        if got == "skipped":
            skipped += 1
            print("skip  %s (no folder %s)" % (g["name"], g["folder"]))
            continue
        if args.record:
            new = expectation(g, got)
            if new is None:
                entries.pop(g["folder"], None)
                if got is not None and not got["crashed"] and got["frames"] > 0 and got["lit"] is None:
                    print("----  %s ran, but its screenshot could not be read (is Pillow installed?); not listed" % g["name"])
                else:
                    print("----  %s does not launch; not listed" % g["name"])
                continue
            entries[g["folder"]] = new
        if not g.get("frames") and args.record:
            print("new   %s: %d frames%s" % (g["name"], entries[g["folder"]]["frames"], ", sound" if entries[g["folder"]]["sound"] else ""))
            passed += 1
            continue
        problems = evaluate(g, got)
        if problems:
            failed += 1
            print("FAIL  %s: %s" % (g["name"], "; ".join(problems)))
        else:
            passed += 1
            print("ok    %s" % g["name"])

    if args.record:
        # Only games with something expected of them: a folder that was not run this time
        # (--only) or does not launch is not an entry.
        manifest["games"] = listed(entries.values())
        # Written beside the list and moved into place, so an interrupted run leaves the old list.
        tmp = args.manifest + ".tmp"
        with open(tmp, "w") as f:
            json.dump(manifest, f, indent=2, sort_keys=True)
            f.write("\n")
        os.replace(tmp, args.manifest)
    print("%d passed, %d failed, %d skipped" % (passed, failed, skipped))
    if retried:
        # Not a failure, but not nothing either: a game that only passes alone is worth a look.
        print("warning: %d passed only on a second run alone: %s" % (len(retried), ", ".join(sorted(retried))))
    if skipped and not passed and not failed:
        print("nothing was checked: no listed game was found under --apps")
        return 2
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
