#!/usr/bin/env python3
"""Unity/Mono startup checklist verification tool and probe check (T5c).

Verifies the 5 critical startup requirements for Unity and Mono on HydrogenVM:
  1. stat_layout: Darwin 32-bit struct stat legacy (96 bytes) and $INODE64 (108 bytes) layouts
  2. signals: SIGSEGV / SIGBUS / SIGILL / SIGFPE routing and signal delivery to guest
  3. dlopen_null: dlopen(NULL) returning RTLD_DEFAULT and dlsym resolving guest/HLE exports
  4. getpw: getpwnam("mobile") / getpwuid(501) user and sandbox directory mapping (/var/mobile)
  5. audio_format: Stereo 16-bit 44.1kHz PCM negotiation and AudioQueue format negotiation
"""

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys


DARWIN_STAT32_LEGACY_SIZE = 96
DARWIN_STAT32_INODE64_SIZE = 108

DARWIN_STAT32_OFFSETS = {
    "st_dev": 0,
    "st_mode": 4,
    "st_nlink": 6,
    "st_ino": 8,
    "st_uid": 16,
    "st_gid": 20,
    "st_rdev": 24,
    "st_mtime": 36,
    "st_size": 48,      # 64-bit size at offset 48
}

MONO_FAULT_SIGNALS = {
    "SIGILL": 4,
    "SIGABRT": 6,
    "SIGFPE": 8,
    "SIGBUS": 10,
    "SIGSEGV": 11,
}


PASS, FAIL, UNVERIFIED = "pass", "fail", "unverified"

# Exact fulltest assertion text each runtime check relies on. A check whose assertion is not in the output is
# UNVERIFIED, never PASS: a checklist that passes without evidence is worse than none.
STAT_MARKER = "Darwin 32-bit struct stat legacy and INODE64 layouts"   # no such assertion in fulltest yet
DLOPEN_MARKER = "dlopen(NULL) returns RTLD_DEFAULT"
GETPWNAM_MARKER = "getpwnam_r finds only mobile"
GETPWUID_MARKER = "getpwuid(501) returns mobile"                          # no such assertion in fulltest yet
AUDIO_MARKER = "AudioQueue accepts 44.1 kHz stereo 16-bit PCM"           # no such assertion in fulltest yet
RTLD_DEFAULT_HANDLE = 0xFFFFFFFE   # what _dlopen(NULL) returns (tests/runner/TestRunner.cpp)


def run_fulltest(hvm_tests_bin: Path | None) -> str | None:
    """stdout of `hvm_tests fulltest`, or None when the binary is not available. Run once, shared by the checks."""
    if not hvm_tests_bin or not Path(hvm_tests_bin).exists():
        return None
    res = subprocess.run([str(hvm_tests_bin), "fulltest"], capture_output=True, text=True, encoding="utf-8", errors="replace")
    return res.stdout


def assertion_status(output: str | None, marker: str) -> str:
    """PASS when every fulltest line naming `marker` is an `ok` line, FAIL when one is not, UNVERIFIED when there is
    no output or no such assertion."""
    if output is None:
        return UNVERIFIED
    lines = [l for l in output.splitlines() if marker in l]
    if not lines:
        return UNVERIFIED
    return PASS if all(l.lstrip().startswith("ok") for l in lines) else FAIL


def combine(*statuses: str) -> str:
    if FAIL in statuses:
        return FAIL
    return UNVERIFIED if UNVERIFIED in statuses else PASS


def _result(status: str, **extra) -> dict:
    return {"status": status, "pass": status == PASS, **extra}


def check_stat_layout(fulltest_output: str | None = None) -> dict:
    """struct stat layout (96 bytes legacy, 108 bytes INODE64): needs a dedicated fulltest assertion."""
    return _result(assertion_status(fulltest_output, STAT_MARKER),
                   legacy_size=DARWIN_STAT32_LEGACY_SIZE, inode64_size=DARWIN_STAT32_INODE64_SIZE,
                   st_size_offset=DARWIN_STAT32_OFFSETS["st_size"], st_mtime_offset=DARWIN_STAT32_OFFSETS["st_mtime"],
                   st_mode_offset=DARWIN_STAT32_OFFSETS["st_mode"],
                   description="Darwin 32-bit struct stat legacy (96) and INODE64 (108) layouts")


def check_signals(report_json: dict | None = None) -> dict:
    """Fault signal delivery to the guest handler. Only a run in which faults reached the guest handler and the
    process still did not crash is evidence; an ordinary exit proves nothing."""
    status = UNVERIFIED
    if report_json:
        faults = (report_json.get("display") or {}).get("faultsToApp", 0)
        crashed = bool(report_json.get("crashed"))
        if faults > 0:
            status = FAIL if crashed else PASS
    return _result(status, signals=MONO_FAULT_SIGNALS,
                   description="SIGSEGV, SIGBUS, SIGILL, SIGFPE delivered to guest handler")


def check_dlopen_null(fulltest_output: str | None = None) -> dict:
    """dlopen(NULL) returns RTLD_DEFAULT."""
    return _result(assertion_status(fulltest_output, DLOPEN_MARKER), rtld_default=RTLD_DEFAULT_HANDLE,
                   description="dlopen(NULL) returns the RTLD_DEFAULT handle")


def check_getpw(fulltest_output: str | None = None) -> dict:
    """getpwnam_r('mobile') and getpwuid(501) both resolve to the mobile user."""
    by_name = assertion_status(fulltest_output, GETPWNAM_MARKER)
    by_uid = assertion_status(fulltest_output, GETPWUID_MARKER)
    return _result(combine(by_name, by_uid), getpwnam=by_name, getpwuid=by_uid, user="mobile", uid=501, gid=501,
                   dir="/var/mobile", description="getpwnam/getpwuid map to the mobile user")


def check_audio_format(fulltest_output: str | None = None) -> dict:
    """AudioQueue accepts 44.1 kHz stereo 16-bit PCM: needs a targeted fulltest assertion (a frame count is not it)."""
    return _result(assertion_status(fulltest_output, AUDIO_MARKER), rate=44100, channels=2, bits=16, format="lpcm",
                   description="AudioQueue negotiates 44.1 kHz stereo 16-bit PCM")


def check_probe_run(rep: dict | None, log_text: str = "") -> list[dict]:
    """Inspect probe report and log for Unity/Mono startup checklist failures."""
    issues = []
    # 1. stat check
    if "stat struct size mismatch" in log_text:
        issues.append({"kind": "error", "check": "stat_layout", "text": "Unity/Mono stat layout failure"})
    elif "Could not load Manifest asset" in log_text:
        # Ambiguous: a missing asset, a bad path or a stat layout problem all print this. Do not blame stat.
        issues.append({"kind": "error", "check": "manifest_asset", "text": "Unity could not load its Manifest asset (cause not established)"})

    # 2. signal check
    if "SIGSEGV" in log_text and "mono_sigsegv_signal_handler" in log_text and (rep and rep.get("crashed")):
        issues.append({"kind": "crash", "check": "signals", "text": "Mono fault signal routing failure"})

    # 3. dlopen(NULL) check
    if "dlsym(RTLD_DEFAULT)" in log_text and "symbol not found" in log_text:
        issues.append({"kind": "missing", "check": "dlopen_null", "text": "dlopen(NULL) export resolution failure"})

    # 4. getpw check
    if "getpwnam failed" in log_text or "getpwuid failed" in log_text:
        issues.append({"kind": "missing", "check": "getpw", "text": "getpw mobile user resolution failure"})

    # 5. audio_format check
    if "AudioQueue: unsupported format" in log_text:
        issues.append({"kind": "error", "check": "audio_format", "text": "AudioQueue format negotiation failure"})

    return issues


def run_checklist(hvm_tests_bin: Path | None = None, report_json: dict | None = None) -> dict:
    """Run the checklist. `overall_pass` needs every check to PASS; a check without evidence is UNVERIFIED."""
    out = run_fulltest(hvm_tests_bin)
    results = {
        "stat_layout": check_stat_layout(out),
        "signals": check_signals(report_json),
        "dlopen_null": check_dlopen_null(out),
        "getpw": check_getpw(out),
        "audio_format": check_audio_format(out),
    }
    statuses = [r["status"] for r in results.values()]
    return {
        "overall_pass": all(x == PASS for x in statuses),
        "overall_status": combine(*statuses),
        "fulltest_available": out is not None,
        "checks": results,
    }


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--tests-bin", type=Path, default=Path(__file__).resolve().parent.parent / "build" / "hvm_tests",
                   help="Path to hvm_tests binary")
    p.add_argument("--json", action="store_true", help="Output JSON")
    args = p.parse_args(argv)

    res = run_checklist(args.tests_bin if args.tests_bin.exists() else None)
    if args.json:
        print(json.dumps(res, indent=2))
    else:
        print("=== Unity/Mono Startup Checklist (T5c) ===")
        if not res["fulltest_available"]:
            print(f"hvm_tests not found at {args.tests_bin}: runtime checks are UNVERIFIED")
        print(f"Status: {res['overall_status'].upper()}")
        for k, v in res["checks"].items():
            print(f"  [{v['status']:10}] {k}: {v.get('description', '')}")

    # 0 only when every check passed; 1 when one failed; 2 when none failed but some could not be verified.
    return {PASS: 0, FAIL: 1, UNVERIFIED: 2}[res["overall_status"]]


if __name__ == "__main__":
    sys.exit(main())
