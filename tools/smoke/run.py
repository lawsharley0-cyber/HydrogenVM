"""Run JSON smoke scenarios: python3 -m tools.smoke.run --help.

Every screenshot checkpoint replays from a fresh home. No reference images,
game files, or saves are downloaded, generated, or added to the repository.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import plistlib
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import zipfile


def within(root, name):
    """Reject absolute paths, parent traversal and symlinks escaping a root."""
    root = Path(root).resolve()
    path = (root / name).resolve()
    if Path(name).is_absolute() or not path.is_relative_to(root) or path == root:
        raise ValueError(f"path must stay below its root: {name}")
    return path


def number(value, label, minimum=0):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value < minimum:
        raise ValueError(f"invalid {label}: {value!r}")
    return value


def load_scenario(path):
    spec = json.loads(Path(path).read_text(encoding="utf-8"))
    if spec.get("schema") != 1:
        raise ValueError("unsupported scenario schema")
    if not re.fullmatch(r"[a-z0-9][a-z0-9-]*", spec["id"]):
        raise ValueError("invalid scenario id")
    for key in ("bundle_id", "ipa", "version", "source"):
        if not isinstance(spec.get(key), str) or not spec[key]:
            raise ValueError(f"missing {key}")
    # Lexical containment is checked here; symlink containment at use time.
    for key in ("ipa", "home_seed"):
        if spec.get(key):
            within(Path(path).parent, spec[key])
    duration = number(spec["run_seconds"], "run_seconds", .01)
    number(spec["rss_limit_mb"], "rss_limit_mb", .01)
    number(spec.get("timeout_slack_seconds", 30), "timeout slack", 1)
    for tap in spec.get("taps", []):
        if len(tap) != 3:
            raise ValueError("tap must be [x, y, seconds]")
        for value in tap:
            number(value, "tap")
        if tap[2] > duration:
            raise ValueError("tap after scenario end")
    previous = -1
    for point in spec.get("screenshots", []):
        at = number(point["at"], "screenshot time", .01)
        if at > duration or at <= previous:
            raise ValueError("screenshot times must increase within the run")
        previous = at
        within(Path(path).parent, point["reference"])
        number(point.get("max_mae", .04), "max_mae")
        if point.get("max_mae", .04) > 1:
            raise ValueError("max_mae must be <= 1")
    if any(not isinstance(line, str) or not line for line in spec.get("required_log", [])):
        raise ValueError("required_log must contain nonempty strings")
    return spec


def identity(path):
    if path.is_dir():
        return plistlib.loads((path / "Info.plist").read_bytes())
    with zipfile.ZipFile(path) as archive:
        names = [n for n in archive.namelist() if re.fullmatch(r"Payload/[^/]+\.app/Info\.plist", n)]
        if len(names) != 1:
            raise ValueError("expected one app Info.plist in IPA")
        return plistlib.loads(archive.read(names[0]))


def screenshot_difference(actual, reference):
    from PIL import Image, ImageChops, ImageStat
    with Image.open(actual) as a, Image.open(reference) as b:
        if a.size != b.size:
            raise ValueError(f"screenshot dimensions differ: {a.size} vs {b.size}")
        # A small downsample tolerates minor antialiasing differences. RGB MAE
        # is normalized to [0,1]; dimensions/orientation must still match.
        a = a.convert("RGB").resize((64, 64), Image.Resampling.BOX)
        b = b.convert("RGB").resize((64, 64), Image.Resampling.BOX)
        return sum(ImageStat.Stat(ImageChops.difference(a, b)).mean) / (3 * 255)


def stop_process(proc):
    if os.name == "posix":
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    else:
        proc.kill()
    proc.wait()


def execute(command, log_path, timeout, rss_limit):
    """Linux bench RSS sampling; fail closed if memory cannot be measured."""
    peak = 0
    sampled = False
    timed_out = False
    exceeded = False
    start = time.monotonic()
    with log_path.open("wb") as log:
        proc = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                                start_new_session=(os.name == "posix"))
        try:
            while proc.poll() is None:
                try:
                    status = Path(f"/proc/{proc.pid}/status").read_text()
                    match = re.search(r"^VmHWM:\s+(\d+) kB", status, re.MULTILINE)
                    if match:
                        sampled = True
                        peak = max(peak, int(match[1]) * 1024)
                except (FileNotFoundError, ProcessLookupError):
                    pass
                if peak > rss_limit:
                    exceeded = True
                    stop_process(proc)
                    break
                if time.monotonic() - start > timeout:
                    timed_out = True
                    stop_process(proc)
                    break
                time.sleep(.05)
        except BaseException:
            stop_process(proc)
            raise
    return {"exit_code": proc.returncode, "timeout": timed_out,
            "rss_exceeded": exceeded, "sampled_rss_bytes": peak if sampled else None,
            "elapsed_seconds": time.monotonic() - start}


def evaluate(spec, log, report, execution):
    failures = []
    if execution["exit_code"] != 0:
        failures.append(f"process exit {execution['exit_code']}")
    if execution["timeout"]:
        failures.append("wall-clock timeout")
    if execution["rss_exceeded"]:
        failures.append("RSS limit exceeded")
    if re.search(r"\bCrash:", log) or report.get("crashed") or report.get("crashReport"):
        failures.append("crash reported")
    if re.search(r"\bStall:", log):
        failures.append("stall reported")
    if report.get("loadError") or report.get("exitStatus", 0) != 0:
        failures.append("runner reported an error")
    # An early clean exit did not exercise the requested duration.
    if report.get("stillRunning") is not True:
        failures.append("scenario ended before requested duration")
    for line in spec.get("required_log", []):
        if line not in log:
            failures.append(f"required log absent: {line}")
    samples = report.get("runtimeMemorySamples", [])
    if isinstance(samples, dict):
        samples = samples.get("samples", [])
    measured = [execution["sampled_rss_bytes"]]
    for sample in samples:
        if isinstance(sample, dict):
            measured.extend(sample.get(k) for k in ("residentBytes", "lifetimePeakResidentBytes"))
    measured = [v for v in measured if isinstance(v, (int, float)) and v > 0]
    if not measured:
        failures.append("RSS measurement unavailable")
    elif max(measured) > spec["rss_limit_mb"] * 1024 * 1024:
        failures.append("RSS limit exceeded")
    return failures, max(measured, default=None)


def run_scenario(spec, hvm, games, refs, output, require_refs=False):
    result = {"id": spec["id"], "bundle_id": spec["bundle_id"], "status": "SKIPPED",
              "runs": [], "notes": spec.get("todos", [])}
    if games is None:
        result["reason"] = "HVM_SMOKE_GAMES is unset"
        return result
    fixture = within(games, spec["ipa"])
    if not fixture.exists():
        result["reason"] = f"missing fixture: {spec['ipa']}"
        return result
    seed = within(games, spec["home_seed"]) if spec.get("home_seed") else None
    if seed and not seed.is_dir():
        result["reason"] = f"missing private home seed: {spec['home_seed']}"
        return result
    info = identity(fixture)
    if info.get("CFBundleIdentifier") != spec["bundle_id"] or str(info.get("CFBundleShortVersionString", info.get("CFBundleVersion"))) != spec["version"]:
        raise ValueError(f"{spec['id']}: fixture identity/version differs from scenario")
    result["scenario_sha256"] = hashlib.sha256(json.dumps(spec, sort_keys=True).encode()).hexdigest()
    output.mkdir(parents=True, exist_ok=False)
    points = {p["at"]: p for p in spec.get("screenshots", [])}
    points.setdefault(spec["run_seconds"], None)
    for index, (duration, point) in enumerate(sorted(points.items())):
        run_dir = output / str(index)
        run_dir.mkdir()
        with tempfile.TemporaryDirectory(prefix="hvm-smoke-") as scratch:
            home = Path(scratch) / "home"
            if seed:
                # Follow no links: private seeds must be plain directory trees.
                if any(p.is_symlink() for p in seed.rglob("*")):
                    raise ValueError("home seed contains symlinks")
                shutil.copytree(seed, home)
            else:
                home.mkdir()
            command = [str(hvm), "run", str(fixture), "--home", str(home),
                       "--run-for", str(duration), "--keep-going", "--graceful",
                       "--report", str(run_dir / "report.json")]
            if spec.get("gpu", True):
                command.append("--gpu")
            if point:
                command.extend(["--screenshot", str(run_dir / "screen.png")])
            for x, y, at in spec.get("taps", []):
                if at < duration:
                    command.extend(["--tap", f"{x},{y}@{at}"])
            # Never inherit opt-in internet or additional CLI flags from a manifest.
            execution = execute(command, run_dir / "run.log",
                                duration + spec.get("timeout_slack_seconds", 30),
                                spec["rss_limit_mb"] * 1024 * 1024)
        report_path = run_dir / "report.json"
        report = json.loads(report_path.read_text()) if report_path.exists() else {}
        failures, peak = evaluate(spec, (run_dir / "run.log").read_text(errors="replace"), report, execution)
        if not report_path.exists():
            failures.append("runner report missing")
        row = {"duration": duration, **execution, "peak_rss_bytes": peak,
               "failures": failures, "tainted": report.get("tainted"),
               "stub_hits": report.get("stubHits"), "screenshot": "NOT_REQUESTED"}
        if point:
            reference = within(refs, point["reference"]) if refs else None
            if not (run_dir / "screen.png").is_file():
                failures.append("screenshot missing")
                row["screenshot"] = "FAILED"
            elif reference is None or not reference.is_file():
                row["screenshot"] = "SKIPPED"
                if require_refs:
                    failures.append("screenshot reference missing")
            else:
                mae = screenshot_difference(run_dir / "screen.png", reference)
                row["screenshot_mae"] = mae
                row["screenshot"] = "PASS" if mae <= point.get("max_mae", .04) else "FAILED"
                if row["screenshot"] == "FAILED":
                    failures.append("screenshot differs")
        result["runs"].append(row)
    result["status"] = "FAILED" if any(r["failures"] for r in result["runs"]) else "PASS"
    result["visual_status"] = "SKIPPED" if any(r["screenshot"] == "SKIPPED" for r in result["runs"]) else "CHECKED"
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hvm", type=Path, default=Path("build/hvm"))
    parser.add_argument("--games", type=Path, default=os.environ.get("HVM_SMOKE_GAMES"))
    parser.add_argument("--refs", type=Path, default=os.environ.get("HVM_SMOKE_REFS"))
    parser.add_argument("--scenarios", type=Path, default=Path(__file__).parent / "scenarios")
    parser.add_argument("--only", action="append", default=[])
    parser.add_argument("--out", type=Path, default=Path("build/smoke"))
    parser.add_argument("--require-refs", action="store_true")
    args = parser.parse_args(argv)
    specs = [load_scenario(p) for p in sorted(args.scenarios.glob("*.json"))]
    if args.only:
        unknown = set(args.only) - {s["id"] for s in specs}
        if unknown:
            parser.error(f"unknown scenario ids: {sorted(unknown)}")
        specs = [s for s in specs if s["id"] in args.only]
    if not specs or len({s["id"] for s in specs}) != len(specs):
        parser.error("need nonempty, unique scenarios")
    # Keep each invocation's output private and separate, never overwrite evidence.
    args.out.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="run-", dir=args.out)).resolve()
    results = []
    for spec in specs:
        try:
            result = run_scenario(spec, args.hvm.resolve(), args.games, args.refs,
                                  output / spec["id"], args.require_refs)
        except (OSError, ValueError, KeyError, zipfile.BadZipFile, plistlib.InvalidFileException) as error:
            result = {"id": spec["id"], "status": "FAILED", "reason": str(error)}
        results.append(result)
        print(f"{result['status']}: {spec['id']}" + (f" ({result['reason']})" if result.get("reason") else ""))
    summary = {status: sum(r["status"] == status for r in results) for status in ("PASS", "FAILED", "SKIPPED")}
    (output / "summary.json").write_text(json.dumps({"schema": 1, "results": results, "summary": summary}, indent=2) + "\n")
    print(f"Summary: {summary}; details: {output / 'summary.json'}")
    return int(summary["FAILED"] > 0)


if __name__ == "__main__":
    sys.exit(main())
