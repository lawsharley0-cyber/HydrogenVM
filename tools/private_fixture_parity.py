#!/usr/bin/env python3
"""Validate bounded private-fixture parity evidence; never copy private payloads."""
from __future__ import annotations
import argparse
import hashlib
import json
import math
import os
from pathlib import Path

IDS = {"angry-birds", "tiny-tower", "art-of-glow"}
FILES = ("Tiny_Tower_v1.0_os30", "AngryBirds_v1.0_os30", "Art_Of_Glow_v1.1.2_os313")
MARKERS = {"uikit": "ALL CHECKS PASSED", "tiny": "GPU: presented frame 60 directly",
           "angry": "GPU: presented frame 600 directly", "tiny-emu": "GPU: presented frame 60 directly"}


def frames_presented(row):
    """framesPresented from a report row; missing or malformed perf counts as 0."""
    perf = row.get("perf") if isinstance(row, dict) else None
    value = perf.get("framesPresented", 0) if isinstance(perf, dict) else 0
    return value


def positive_frames(value):
    """A real frame count: a positive integer (booleans, fractions and non-finite numbers are rejected)."""
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        return False
    return value > 0 and value == int(value)


def read(path):
    value = json.loads(path.read_text())
    if not isinstance(value, dict):
        raise ValueError("expected JSON object")
    return value


def linux(root):
    from compat_harness.manifest import default_manifest_path, load_manifest
    from hvm_testlab import CASE_IDS
    from compat_harness.schema_check import validate_report, validate_diff
    work = root / "work"
    report, diff = read(work / "compat/report.json"), read(work / "compat/diff.json")
    if validate_report(report) or validate_diff(diff) or not report["summary"]["ok"] or not diff["ok"]:
        raise ValueError("compatibility/schema/baseline gate failed")
    if report["manifest"]["legacyRepoAllowed"] is not False:
        raise ValueError("legacy fallback was not disabled")
    unit = report["suites"]["unit"]
    if unit["status"] != "pass" or not any(c["id"] == "fulltest" and c["status"] == "pass" for c in unit["cases"]):
        raise ValueError("fulltest was absent or did not pass")
    specs = {a["id"]: a for a in load_manifest(default_manifest_path())["apps"]}
    rows = [a for a in report["suites"]["apps"]["apps"] if not a.get("optional")]
    if {a["id"] for a in rows} != IDS or len(rows) != len(IDS):
        raise ValueError("required real-app coverage is incomplete")
    apps = []
    for row in rows:
        if row["source"] != "fixture-root" or row["sha256"] != specs[row["id"]]["sha256"] or row["status"] in ("skipped", "error", "timeout"):
            raise ValueError("required real-app source/hash/run is invalid")
        apps.append({key: row[key] for key in ("id", "source", "sha256", "status", "crashed")})
        apps[-1]["framesPresented"] = frames_presented(row)
        if not positive_frames(apps[-1]["framesPresented"]):
            raise ValueError("required real-app run presented no valid frame count")
        apps[-1]["frameSignal"] = row.get("frame", {}).get("signal")
    lab = read(work / "testlab/testlab_report.json")
    cases = lab["cases"]
    if {c["id"] for c in cases} != set(CASE_IDS) or len(cases) != len(CASE_IDS) or any(c["status"] != "passed" for c in cases):
        raise ValueError("Test Lab coverage is incomplete or failed")
    gpu = []
    for mode in ("gpu", "gpu-emu"):
        log = (work / (mode + ".log")).read_text().splitlines()
        ok = [line for line in log if line.startswith("ok ")]
        if len(ok) != 3 or any(line.startswith(("skip ", "FAIL")) for line in log):
            raise ValueError("GPU comparison omitted or failed a required app")
        for name in FILES:
            if not any(line.startswith("ok") and (name + ":") in line for line in ok):
                raise ValueError("GPU comparison did not validate the expected app")
            row = read(work / mode / (name + "-gpu.json"))
            frames = frames_presented(row)
            if not positive_frames(frames):
                raise ValueError("GPU comparison did not present frames")
            gpu.append({"mode": mode, "fixture": name, "framesPresented": frames,
                        "crashed": bool(row.get("crashed"))})
    return {"requiredApps": apps, "baselinePassed": True, "fulltestPassed": True,
            "testLabPassed": len(cases), "gpuComparisons": gpu}


def macos(root):
    scenarios = []
    for name, marker in MARKERS.items():
        folder = root / "work" / ("sim-" + name)
        lines = (folder / "console.log").read_text(errors="replace").replace("\r", "").splitlines()
        image = (folder / "screen.png").read_bytes()
        if marker not in lines or not image.startswith(b"\x89PNG\r\n\x1a\n"):
            raise ValueError("required simulator marker or screenshot missing")
        scenarios.append({"scenario": name, "markerObserved": True,
                          "screenshotBytes": len(image), "screenshotSha256": hashlib.sha256(image).hexdigest()})
    return {"simulatorScenarios": scenarios, "unsignedDeviceBuild": True, "unsignedSimulatorBuild": True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", choices=("linux", "macos"), required=True)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    result = {"schemaVersion": 1, "commit": os.environ["GITHUB_SHA"], "platform": args.platform,
              "hostPageSize": os.sysconf("SC_PAGE_SIZE"), "parityGatePassed": False,
              "privatePayloadsUploaded": False, "physicalDeviceTested": False}
    try:
        # Step outcomes prevent fabricated/stale evidence from qualifying skipped builds.
        outcomes = {k[7:]: v for k, v in os.environ.items() if k.startswith("PARITY_")}
        result["steps"] = outcomes
        if not outcomes or any(v != "success" for v in outcomes.values()):
            raise ValueError("one or more prerequisite steps failed or were not run")
        result.update((linux if args.platform == "linux" else macos)(Path(os.environ["HVM_FIXTURE_ROOT"])))
        result["parityGatePassed"] = True
    except (OSError, ValueError, KeyError, TypeError, ImportError):
        result["reason"] = "Required parity evidence failed, is incomplete, or was not run; inspect step outcomes"
    args.out.write_text(json.dumps(result, indent=2, allow_nan=False) + "\n")
    with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as out:
        out.write("Private fixture " + args.platform + " parity: " + ("PASS" if result["parityGatePassed"] else "NOT VERIFIED") + "\n\n")
        out.write("No private binaries, screenshots, raw guest logs or credentials uploaded. Physical-device qualification is NOT RUN.\n")
    return 0 if result["parityGatePassed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
