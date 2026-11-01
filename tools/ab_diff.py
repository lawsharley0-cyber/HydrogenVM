#!/usr/bin/env python3
"""Run one guest with identical input on two builds and compare observations.

Exit 0 identical observations, 1 divergence, 2 runner/report failure. This is
comparison, not deterministic replay. Timing/throughput fields are excluded;
raw audio/frame counters remain visible and can diverge with host scheduling.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys


def first_difference(old, new, path="$"):
    if type(old) is not type(new):
        return {"path": path, "old": old, "new": new}
    if isinstance(old, dict):
        for key in sorted(old.keys() | new.keys()):
            if key not in old or key not in new:
                return {"path": path + "." + key, "old": old.get(key), "new": new.get(key), "missing": "old" if key not in old else "new"}
            result = first_difference(old[key], new[key], path + "." + key)
            if result:
                return result
    elif isinstance(old, list):
        for i in range(min(len(old), len(new))):
            result = first_difference(old[i], new[i], f"{path}[{i}]")
            if result:
                return result
        if len(old) != len(new):
            return {"path": path + ".length", "old": len(old), "new": len(new)}
    elif old != new:
        return {"path": path, "old": old, "new": new}
    return None


def normalize(value, replacements):
    if isinstance(value, str):
        for source, target in replacements:
            value = value.replace(source, target)
        # Sandbox UUID is derived from the isolated home, not guest behavior.
        return re.sub(r"(/var/mobile/Applications/)[0-9A-Fa-f-]{36}(?=/)", r"\1<home-id>", value)
    if isinstance(value, list):
        return [normalize(v, replacements) for v in value]
    if isinstance(value, dict):
        return {k: normalize(v, replacements) for k, v in value.items()}
    return value


def executable(build):
    path = Path(build).resolve()
    return path / "hvm" if path.is_dir() else path


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _finite_float(text):
    value = float(text)
    if value in (float("inf"), float("-inf")):   # for example 1e999
        raise ValueError(f"report contains non-finite number {text}")
    return value


def _reject_constant(name):
    raise ValueError(f"report contains non-finite number {name}")


def observation(build, target, folder, args):
    folder.mkdir(parents=True, exist_ok=False)
    home = folder / "home"
    home.mkdir()
    hvm = executable(build)
    report_path = folder / "report.json"
    command = [str(hvm), "run", str(target), "--home", str(home), "--quiet",
               "--run-for", str(args.seconds), "--report", str(report_path),
               "--screenshot", str(folder / "shot.png"), "--frame-file", str(folder / "frame.rgba")]
    for flag, values in (("--tap", args.tap), ("--drag", args.drag), ("--lifecycle", args.lifecycle)):
        for value in values:
            command += [flag, value]
    command += ["--", *args.guest_arg]
    (folder / "command.json").write_text(json.dumps(command, indent=2) + "\n")
    try:
        # Separate stdout/stderr avoids nondeterministic stream interleaving.
        result = subprocess.run(command, capture_output=True, timeout=args.timeout, text=True, errors="replace", check=False)
    except subprocess.TimeoutExpired as exc:
        # Keep whatever the hung guest printed: it is the regression evidence. Output may be bytes even with text=True.
        for name, data in (("stdout.log", exc.stdout), ("stderr.log", exc.stderr)):
            text = data.decode("utf-8", "replace") if isinstance(data, bytes) else (data or "")
            (folder / name).write_text(text, encoding="utf-8")
        raise RuntimeError(f"guest runner timed out after {args.timeout}s; partial output kept in {folder}") from exc
    except OSError as exc:
        raise RuntimeError(f"guest runner unavailable: {exc}") from exc
    (folder / "stdout.log").write_text(result.stdout, encoding="utf-8")
    (folder / "stderr.log").write_text(result.stderr, encoding="utf-8")
    try:
        report = json.loads(report_path.read_text(encoding="utf-8"), parse_constant=_reject_constant, parse_float=_finite_float)
    except (OSError, ValueError) as exc:
        raise RuntimeError(f"no valid hvm report (exit {result.returncode}); see {folder / 'stderr.log'}") from exc
    required = {"exitStatus": int, "crashed": bool, "loadError": str, "perf": dict, "audio": dict}
    if not isinstance(report, dict) or any(type(report.get(k)) is not t for k, t in required.items()):
        raise ValueError("missing/invalid hvm report fields")
    # `hvm run` exits 70 for a guest crash and 71 for a load failure and writes a report saying so. Those are
    # measured outcomes to compare; an exit status the report does not back up is a tool problem.
    code = result.returncode
    reported = (code in (0, 1) or (code == 70 and report["crashed"]) or (code == 71 and bool(report["loadError"]))) \
        and (code not in (70, 71) or report["exitStatus"] == code)
    if not reported:
        raise RuntimeError(f"guest did not load/run (exit {code}): {report['loadError']}")
    replacements = [(str(folder), "<run>"), (str(target), "<guest>"), (str(hvm), "<hvm>")]
    stable = normalize(report, replacements)
    perf = stable.pop("perf")
    stable["perf"] = {k: v for k, v in perf.items() if k in {"frames", "framesPresented"}}
    # A crash bundle repeats timing and the emulator build, which differ between two builds by construction.
    bundle = stable.get("crashBundle")
    if isinstance(bundle, dict):
        stable["crashBundle"] = bundle = dict(bundle)
        if isinstance(bundle.get("perf"), dict):
            bundle["perf"] = {k: v for k, v in bundle["perf"].items() if k != "wallSeconds"}
        if isinstance(bundle.get("host"), dict):
            bundle["host"] = {k: v for k, v in bundle["host"].items() if k != "emulatorCommit"}
    logs = {}
    for name, content in (("stdout", result.stdout), ("stderr", result.stderr)):
        lines = normalize(content, replacements).splitlines()
        logs[name] = [line for line in lines if not any(re.search(pattern, line) for pattern in args.ignore_log_regex)]
    hashes = {name: sha256(folder / name) if (folder / name).exists() else None
              for name in ("shot.png", "frame.rgba")}
    return {"binarySha256": sha256(hvm), "returncode": result.returncode, "report": stable, "logs": logs, "frameHashes": hashes,
            "audioCounters": {k: report.get(k, {}) for k in ("audio", "mixer")},
            "informationalPerf": perf}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("old_build")
    ap.add_argument("new_build")
    ap.add_argument("--guest", required=True, type=Path)
    ap.add_argument("--out", type=Path, default=Path("build/ab-diff"))
    ap.add_argument("--seconds", type=float, default=10)
    ap.add_argument("--timeout", type=float, default=30)
    for flag in ("tap", "drag", "lifecycle", "guest-arg", "ignore-log-regex"):
        ap.add_argument("--" + flag, action="append", default=[])
    args = ap.parse_args(argv)
    try:
        if args.seconds <= 0 or args.timeout <= args.seconds:
            raise ValueError("seconds must be positive and timeout must exceed seconds")
        if not args.guest.exists():
            raise ValueError("guest path does not exist")
        for pattern in args.ignore_log_regex:
            try:
                re.compile(pattern)
            except re.error as exc:
                raise ValueError(f"invalid --ignore-log-regex {pattern!r}: {exc}") from exc
        out = args.out.resolve()
        # Refuse reuse: old sandboxes and reports can manufacture equality.
        out.mkdir(parents=True, exist_ok=False)
        observations = [observation(build, args.guest.resolve(), out / name, args)
                        for build, name in ((args.old_build, "old"), (args.new_build, "new"))]
        old, new = observations
        differences = {key: first_difference(old[key], new[key])
                       for key in ("returncode", "report", "logs", "frameHashes", "audioCounters")}
        different = any(differences.values())
        result = {"schemaVersion": 1, "kind": "hvm-ab-diff", "observations": observations,
                  "host": {"system": platform.system(), "release": platform.release(),
                           "machine": platform.machine(), "pageSize": os.sysconf("SC_PAGE_SIZE") if hasattr(os, "sysconf") else None},
                  "guest": {"path": str(args.guest.resolve()), "sha256": sha256(args.guest) if args.guest.is_file() else None},
                  "firstDivergences": differences,
                  "normalization": {"excludedReportPerf": "all except frames/framesPresented",
                                    "paths": "isolated host paths and derived guest home UUID",
                                    "ignoredLogPatterns": args.ignore_log_regex},
                  "summary": {"ok": not different, "different": different}}
        (out / "diff.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print(f"{'DIFFERENT' if different else 'IDENTICAL'}: {out / 'diff.json'}")
        for key, difference in differences.items():
            if difference:
                print(f"  {key}: {difference['path']}")
        return int(different)
    except (OSError, ValueError, RuntimeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
