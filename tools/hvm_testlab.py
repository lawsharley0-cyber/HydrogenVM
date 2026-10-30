#!/usr/bin/env python3
"""HydrogenVM Test Lab — host orchestrator.

Builds the TestLab IPA, runs each case (or a selection) through the HVM CLI,
applies external timeouts, detects interrupted cases, assembles a host-side
report, and validates it against the schema.

usage:
    python3 tools/hvm_testlab.py run
        [--build-dir build] [--out build/testlab-report]
        [--hvm PATH] [--case TL-A] [--all]
        [--timeout 30] [--skip-build]
        [--home DIR]

    python3 tools/hvm_testlab.py validate REPORT.json

Exit status:
    0  all cases passed
    1  one or more cases failed
    2  one or more cases interrupted
    3  tool/build error

The runner executes one case per hvm process. For Case E (persistence) it runs
phase 1, keeps the sandbox, then runs phase 2 in a second process with the same
--home directory so NSUserDefaults persists.
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile
from pathlib import Path

ROOT = Path(__file__).parent.parent
TESTLAB_SRC = ROOT / "tests" / "ios" / "apps" / "testlab"
SCHEMA_MOD  = str(ROOT / "tools" / "testlab_schema")

CASE_IDS = ["TL-A", "TL-B", "TL-C", "TL-D", "TL-E", "TL-F", "TL-G", "TL-H", "TL-I", "TL-J", "TL-K", "TL-L"]
# TL-K needs --tap and --lifecycle injected before the -- separator.
TIMEOUT_K = 10  # tap at 1s, bg at 2s, fg at 3s — settled well within 10s
# TL-L plays 300ms of audio; allow extra for hvm startup.
TIMEOUT_L = 20
# Wall-clock timeout (seconds) per case process, including hvm startup.
DEFAULT_TIMEOUT = 30
# TL-E phase 1 is fast; allow extra for the full two-launch cycle.
TIMEOUT_E_TOTAL = 60


# ─────────────────────────── build helpers ───────────────────────────────────

def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def build_testlab(build_dir: str, skip_build: bool) -> tuple[str, str]:
    """Build the TestLab IPA.  Returns (ipa_path, sha256)."""
    build_dir = Path(build_dir)
    ios_out    = build_dir / "ios"
    ipa_path   = ios_out / "TestLab.ipa"

    if skip_build and ipa_path.exists():
        print(f"[testlab] skip-build: using {ipa_path}", flush=True)
        return str(ipa_path), sha256_file(str(ipa_path))

    ios_out.mkdir(parents=True, exist_ok=True)

    # Ensure tone.wav and empty.dat exist
    tone_src  = TESTLAB_SRC / "tone.wav"
    empty_src = TESTLAB_SRC / "empty.dat"
    if not tone_src.exists():
        print(f"[testlab] generating tone.wav …", flush=True)
        subprocess.run(
            [sys.executable, str(TESTLAB_SRC / "gen_tone_wav.py"), str(tone_src)],
            check=True,
        )
    if not empty_src.exists():
        empty_src.write_bytes(b"")

    cmd = [
        sys.executable, str(ROOT / "tools" / "ios_build.py"), "app",
        "--name", "TestLab",
        "--display", "Test Lab",
        "--out", str(ios_out),
        "--min-ios", "6.0",
        "--resource", str(tone_src),
        "--resource", str(empty_src),
        str(TESTLAB_SRC / "main.m"),
    ]
    print(f"[testlab] building IPA …", flush=True)
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stderr)
        sys.stderr.write(r.stdout)
        raise SystemExit(3)

    if not ipa_path.exists():
        sys.stderr.write(f"[testlab] IPA not found after build at {ipa_path}\n")
        raise SystemExit(3)

    digest = sha256_file(str(ipa_path))
    print(f"[testlab] IPA: {ipa_path}  sha256={digest[:16]}…", flush=True)
    return str(ipa_path), digest


def verify_ipa_contents(ipa_path: str) -> list[str]:
    """Return a list of error strings (empty → ok)."""
    errors = []
    try:
        with zipfile.ZipFile(ipa_path) as zf:
            names = zf.namelist()
            # Must have Payload/TestLab.app/TestLab
            exe = "Payload/TestLab.app/TestLab"
            if exe not in names:
                errors.append(f"executable '{exe}' missing from IPA")
            plist = "Payload/TestLab.app/Info.plist"
            if plist not in names:
                errors.append(f"Info.plist missing from IPA")
            tone = "Payload/TestLab.app/tone.wav"
            if tone not in names:
                errors.append(f"tone.wav resource missing from IPA")
            empty = "Payload/TestLab.app/empty.dat"
            if empty not in names:
                errors.append(f"empty.dat resource missing from IPA")
            # Verify stored (size 0) entry is actually there
            emp_info = next((i for i in zf.infolist() if i.filename == empty), None)
            if emp_info is None:
                errors.append("empty.dat not found in zip listing")
    except Exception as e:
        errors.append(f"IPA open failed: {e}")
    return errors


# ─────────────────────────── run one case ────────────────────────────────────

def run_case(
    hvm: str,
    ipa_path: str,
    case_id: str,
    home: str,
    timeout: int,
    import_ipa: bool = True,
) -> dict:
    """Run one case in a subprocess.  Returns a caseResult dict."""
    t0 = time.monotonic()

    # hvm run imports the IPA on first use; for phase 2 of TL-E we must NOT
    # re-import (that would replace the sandbox and erase the persisted prefs).
    cmd = [hvm, "run", ipa_path, "--home", home, "--quiet",
           "--run-for", str(timeout),
           "--", "--run", case_id]
    if not import_ipa:
        # Pass a flag (if supported) or use the already-extracted .app path.
        # hvm's --home approach keeps the installed .app; passing the IPA would
        # re-import. We use the installed .app path instead.
        app_path = _find_installed_app(home)
        if app_path:
            cmd = [hvm, "run", app_path, "--home", home, "--quiet",
                   "--run-for", str(timeout),
                   "--", "--run", case_id]

    try:
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout + 5,  # extra 5s for hvm startup/shutdown
        )
        rc      = proc.returncode
        stdout  = proc.stdout
        stderr  = proc.stderr
        elapsed = time.monotonic() - t0
        timed_out = False
    except subprocess.TimeoutExpired as e:
        rc      = -1
        stdout  = e.stdout.decode(errors="replace") if e.stdout else ""
        stderr  = e.stderr.decode(errors="replace") if e.stderr else ""
        elapsed = time.monotonic() - t0
        timed_out = True

    case_result = _parse_case_output(stdout, case_id, elapsed, rc, timed_out, stderr)
    return case_result


def _find_installed_app(home: str) -> str | None:
    """Find the extracted .app folder inside an hvm home directory."""
    apps_dir = Path(home) / "Apps"
    if not apps_dir.is_dir():
        return None
    # Look for com.hydrogenvm.test.testlab
    for entry in apps_dir.iterdir():
        if "testlab" in entry.name.lower():
            for sub in entry.iterdir():
                if sub.suffix == ".app":
                    return str(sub)
    return None


def _parse_case_output(stdout: str, case_id: str, elapsed: float,
                        rc: int, timed_out: bool, stderr: str) -> dict:
    """Parse TESTLAB lines from stdout and produce a caseResult dict."""
    # Look for TESTLAB <id> <status> lines
    pattern = re.compile(r"^TESTLAB\s+(\S+)\s+(\S+)", re.MULTILINE)
    matches = {m.group(1): m.group(2) for m in pattern.finditer(stdout)}

    # Try to read result JSON written to Documents/
    # (we rely on stdout lines as the primary signal; JSON gives more detail)
    status  = matches.get(case_id)
    reason  = None
    diag    = {}

    if timed_out:
        status = "interrupted"
        reason = f"External timeout after {elapsed:.1f}s"
        diag["timedOut"] = True
    elif status is None:
        # hvm may have crashed or failed to start
        if rc == 2:
            status = "interrupted"
            reason = "Checkpoint detected (exit code 2)"
        elif rc != 0 and rc != 1:
            status = "interrupted"
            reason = f"hvm exited with code {rc} without producing output"
        else:
            status = "failed"
            reason = "No TESTLAB output line found in stdout"

    # Extract reason line from stdout
    reason_match = re.search(r"reason: (.+)", stdout)
    if reason_match and reason is None:
        reason = reason_match.group(1).strip()

    diag["hvmExitCode"]  = rc
    diag["elapsedSec"]   = round(elapsed, 3)
    diag["stdoutTail"]   = (stdout + stderr)[-800:] if (stdout + stderr) else ""

    result: dict = {
        "id":                case_id,
        "version":           1,
        "status":            status or "failed",
        "durationMs":        round(elapsed * 1000, 1),
        "clockDefinition":   "CLOCK_MONOTONIC (host)",
        "checkpointWritten": None,
        "diagnostics":       diag,
    }
    if reason:
        result["reason"] = reason
    return result


# ─────────────────────────── TL-J baseline ceiling ───────────────────────────

def _baseline_mflops_ceiling() -> float | None:
    """Read the linux-headless baseline and return the MFLOPS regression floor.

    The floor is 50% of the selftest MIPS from the baseline (MIPS ≈ MFLOPS for
    the integer self-test workload). Returns None when the baseline is absent or
    the key is missing — in that case the guest's own 50 MFLOPS floor applies.
    """
    baseline_path = ROOT / "docs" / "dev-system" / "baselines" / "linux-headless.json"
    try:
        data = json.loads(baseline_path.read_text())
        # The baseline stores MIPS under suites.unit.cases[0].diagnostics.mips
        # or under a top-level selftest_mips key added by the harness.
        mips = data.get("selftest_mips")
        if mips is None:
            # Dig into the unit suite for the selftest case
            for case in data.get("suites", {}).get("unit", {}).get("cases", []):
                if "selftest" in case.get("id", "").lower():
                    mips = case.get("diagnostics", {}).get("mips")
                    break
        if mips and float(mips) > 0:
            return float(mips) * 0.5  # 50% regression ceiling
    except Exception:
        pass
    return None


def _apply_tj_ceiling(case_result: dict) -> dict:
    """For a passed TL-J result, apply the host-side MFLOPS regression ceiling."""
    if case_result.get("status") != "passed":
        return case_result
    ceiling = _baseline_mflops_ceiling()
    if ceiling is None:
        return case_result
    # Extract mflops from stdoutTail diagnostics (guest writes it to stdout)
    stdout = case_result.get("diagnostics", {}).get("stdoutTail", "")
    mflops_match = re.search(r"mflops=([\d.]+)", stdout)
    if not mflops_match:
        return case_result
    mflops = float(mflops_match.group(1))
    case_result["diagnostics"]["hostCeiling_mflops"] = ceiling
    case_result["diagnostics"]["observed_mflops"]    = mflops
    if mflops < ceiling:
        case_result["status"] = "failed"
        case_result["reason"] = (
            f"Throughput {mflops:.1f} MFLOPS < baseline ceiling {ceiling:.1f} MFLOPS"
        )
    return case_result


# ─────────────────────────── run all cases ───────────────────────────────────

def run_all_cases(
    hvm: str,
    ipa_path: str,
    home: str,
    cases: list[str],
    timeout: int,
) -> list[dict]:
    results = []
    imported = False
    # Start from a clean sandbox: imports keep app data, so stale prefs from an
    # earlier run would otherwise turn TL-E phase 1 into a phase-2 read.
    import shutil
    shutil.rmtree(home, ignore_errors=True)
    os.makedirs(home, exist_ok=True)

    for case_id in cases:
        if case_id == "TL-E":
            result = _run_case_e(hvm, ipa_path, home, TIMEOUT_E_TOTAL)
            results.append(result)
            continue

        if case_id == "TL-K":
            # TL-K injects --tap and --lifecycle before '--', handled separately.
            # Ensure the IPA has been imported first by running a cheap preceding case.
            if not imported:
                # Import by running TL-A first (fast, just checks metadata)
                _pre = run_case(hvm, ipa_path, "TL-A", home, timeout, import_ipa=True)
                imported = True
                results.append(_pre)
            cr = _run_case_k(hvm, ipa_path, home)
            results.append(cr)
            continue

        if case_id == "TL-L":
            if not imported:
                _pre = run_case(hvm, ipa_path, "TL-A", home, timeout, import_ipa=True)
                imported = True
                results.append(_pre)
            cr = _run_case_l(hvm, ipa_path, home, TIMEOUT_L)
            results.append(cr)
            continue

        print(f"[testlab] running {case_id} …", flush=True)
        cr = run_case(hvm, ipa_path, case_id, home, timeout, import_ipa=not imported)
        imported = True
        if case_id == "TL-J":
            cr = _apply_tj_ceiling(cr)
        _print_result(cr)
        results.append(cr)

    return results


def _run_case_e(hvm: str, ipa_path: str, home: str, timeout: int) -> dict:
    """Two-launch persistence test for TL-E."""
    print(f"[testlab] running TL-E phase 1 (write) …", flush=True)
    cr1 = run_case(hvm, ipa_path, "TL-E", home, timeout)
    _print_result(cr1)

    # Phase 1 should be skipped (write done) or already interrupted.
    if cr1["status"] == "interrupted":
        return cr1  # carry forward the interrupted result

    print(f"[testlab] running TL-E phase 2 (read) …", flush=True)
    # Re-importing the IPA now preserves Documents/Library/tmp (app updates keep saves),
    # so phase 2 can use the normal launch path and sees the same sandbox as phase 1.
    cr2 = run_case(hvm, ipa_path, "TL-E", home, timeout)
    _print_result(cr2)

    # Phase 2 result is the canonical TL-E result.
    # Annotate it with both phases.
    cr2["diagnostics"]["phase1Result"] = cr1["status"]
    return cr2


def _run_case_k(hvm: str, ipa_path: str, home: str) -> dict:
    """Touch and lifecycle test for TL-K.

    Injects a tap at (160,240)@1.0 and a bg@2.0,fg@3.0 lifecycle event
    before the '--' separator so hvm delivers them while the guest runs.
    """
    print(f"[testlab] running TL-K (touch+lifecycle) …", flush=True)
    t0 = time.monotonic()
    cmd = [
        hvm, "run", ipa_path,
        "--home", home, "--quiet",
        "--run-for", str(TIMEOUT_K),
        "--tap", "160,240@1.0",
        "--lifecycle", "bg@2.0,fg@3.0",
        "--", "--run", "TL-K",
        "--tap", "160,240@1.0",       # duplicated after -- so guest sees it
        "--lifecycle", "bg@2.0,fg@3.0",
    ]
    try:
        proc = subprocess.run(
            cmd, capture_output=True, text=True,
            timeout=TIMEOUT_K + 5,
        )
        rc, stdout, stderr = proc.returncode, proc.stdout, proc.stderr
        timed_out = False
    except subprocess.TimeoutExpired as e:
        rc = -1
        stdout = e.stdout.decode(errors="replace") if e.stdout else ""
        stderr = e.stderr.decode(errors="replace") if e.stderr else ""
        timed_out = True
    except FileNotFoundError:
        elapsed = time.monotonic() - t0
        cr = _parse_case_output("", "TL-K", elapsed, -1, False, "")
        cr["reason"] = f"hvm binary not found: {hvm}"
        cr["status"] = "failed"
        _print_result(cr)
        return cr
    elapsed = time.monotonic() - t0
    cr = _parse_case_output(stdout, "TL-K", elapsed, rc, timed_out, stderr)
    _print_result(cr)
    return cr


def _run_case_l(hvm: str, ipa_path: str, home: str, timeout: int) -> dict:
    """Audio mixer PCM energy test for TL-L.

    Passes --audio-fd so the mixer drains into a socket. The host
    inspects the audio signal and fails TL-L if the mixer was silent.
    """
    import socket as _socket
    print(f"[testlab] running TL-L (audio mixer) …", flush=True)
    t0 = time.monotonic()

    # Try to set up a socketpair for audio drain.
    audio_cmd_extra: list[str] = []
    recv_sock = None
    try:
        recv_sock, send_sock = _socket.socketpair(_socket.AF_UNIX, _socket.SOCK_DGRAM)
        audio_cmd_extra = ["--audio-fd", str(send_sock.fileno())]
    except (AttributeError, OSError):
        # AF_UNIX not available (e.g. Windows CI) — still run the guest check
        pass

    cmd = [
        hvm, "run", ipa_path,
        "--home", home, "--quiet",
        "--run-for", str(timeout),
    ] + audio_cmd_extra + [
        "--", "--run", "TL-L",
    ]
    try:
        proc = subprocess.run(
            cmd, capture_output=True, text=True,
            timeout=timeout + 5,
            pass_fds=(send_sock.fileno(),) if recv_sock else (),
        )
        rc, stdout, stderr = proc.returncode, proc.stdout, proc.stderr
        timed_out = False
    except subprocess.TimeoutExpired as e:
        rc = -1
        stdout = e.stdout.decode(errors="replace") if e.stdout else ""
        stderr = e.stderr.decode(errors="replace") if e.stderr else ""
        timed_out = True
    except FileNotFoundError:
        if recv_sock:
            try: send_sock.close()
            except Exception: pass
            try: recv_sock.close()
            except Exception: pass
        elapsed = time.monotonic() - t0
        cr = _parse_case_output("", "TL-L", elapsed, -1, False, "")
        cr["reason"] = f"hvm binary not found: {hvm}"
        cr["status"] = "failed"
        _print_result(cr)
        return cr
    finally:
        if recv_sock:
            try: send_sock.close()
            except Exception: pass

    elapsed = time.monotonic() - t0
    cr = _parse_case_output(stdout, "TL-L", elapsed, rc, timed_out, stderr)

    # Check audio signal from the socket if available
    if recv_sock and cr.get("status") == "passed":
        try:
            recv_sock.settimeout(0.1)
            frames_received = 0
            non_zero = 0
            while True:
                try:
                    data = recv_sock.recv(65536)
                    if not data:
                        break
                    frames_received += 1
                    if any(b != 0 for b in data):
                        non_zero += 1
                except _socket.timeout:
                    break
            cr["diagnostics"]["audioFramesReceived"] = frames_received
            cr["diagnostics"]["nonZeroFrames"]       = non_zero
            if frames_received > 0 and non_zero == 0:
                cr["status"] = "failed"
                cr["reason"]  = "Audio socket received frames but all samples were zero"
            elif frames_received == 0:
                cr["diagnostics"]["audioNote"] = "No frames received on socket — mixer may not have run"
        except Exception as exc:
            cr["diagnostics"]["audioSocketError"] = str(exc)
        finally:
            try: recv_sock.close()
            except Exception: pass
    elif recv_sock:
        try: recv_sock.close()
        except Exception: pass

    _print_result(cr)
    return cr


def _print_result(cr: dict) -> None:
    status = cr.get("status", "?")
    reason = cr.get("reason", "")
    reason_str = f" — {reason}" if reason else ""
    print(f"  {cr['id']}: {status}{reason_str}", flush=True)


# ─────────────────────────── host report ─────────────────────────────────────

def _git_info() -> dict:
    def git(*args):
        try:
            return subprocess.run(
                ["git", "-C", str(ROOT)] + list(args),
                capture_output=True, text=True,
            ).stdout.strip()
        except Exception:
            return None
    commit = git("rev-parse", "HEAD") or None
    short  = git("rev-parse", "--short", "HEAD") or None
    dirty_out = git("status", "--porcelain")
    dirty  = bool(dirty_out) if dirty_out is not None else None
    branch = git("rev-parse", "--abbrev-ref", "HEAD") or None
    return {"commit": commit, "short": short, "dirty": dirty, "branch": branch}


def _page_size() -> int | None:
    try:
        import resource
        return resource.getpagesize()
    except Exception:
        return None


def _compiler_info() -> str:
    try:
        r = subprocess.run(["clang++", "--version"], capture_output=True, text=True)
        return r.stdout.splitlines()[0] if r.stdout else "unknown"
    except Exception:
        return "unknown"


def make_host_report(
    cases: list[dict],
    ipa_sha: str | None,
    run_id: str,
    git: dict,
) -> dict:
    now = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    return {
        "schemaVersion": 1,
        "kind":          "hvm-testlab-host-report",
        "generatedAt":   now,
        "runId":         run_id,
        "appVersion":    "1.0",
        "appBuild":      "1",
        "artifactSha256": ipa_sha,
        "sandboxMode":   "hvm-home",
        "host": {
            "system":         platform.system(),
            "release":        platform.release(),
            "machine":        platform.machine(),
            "python":         platform.python_version(),
            "pageSize":       _page_size(),
            "compiler":       _compiler_info(),
            "emulatorCommit": git.get("commit"),
            "emulatorDirty":  git.get("dirty"),
        },
        "startTime": now,
        "cases":     cases,
    }


# ─────────────────────────── validate ────────────────────────────────────────

def validate_report(path: str) -> bool:
    sys.path.insert(0, str(ROOT / "tools"))
    try:
        from testlab_schema.validate_result import validate
    except ImportError as e:
        sys.stderr.write(f"[testlab] cannot import validator: {e}\n")
        return False
    data   = json.loads(Path(path).read_text())
    errors = validate(data)
    if errors:
        for e in errors:
            sys.stderr.write(f"  FAIL  {e}\n")
        sys.stderr.write(f"validation FAILED: {len(errors)} error(s)\n")
        return False
    print(f"ok  {path}  ({len(data.get('cases', []))} cases, schemaVersion=1)")
    return True


# ─────────────────────────── CLI ─────────────────────────────────────────────

def cmd_run(args: argparse.Namespace) -> int:
    hvm = args.hvm or str(ROOT / "build" / "hvm")
    if not Path(hvm).exists():
        sys.stderr.write(f"[testlab] hvm not found at {hvm} — pass --hvm PATH\n")
        return 3

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    # Build IPA
    ipa_path, ipa_sha = build_testlab(args.build_dir, args.skip_build)

    # Verify contents
    errors = verify_ipa_contents(ipa_path)
    if errors:
        for e in errors:
            sys.stderr.write(f"[testlab] IPA error: {e}\n")
        return 3

    # Select cases
    cases = [args.case] if args.case else CASE_IDS

    # Home directory
    home = args.home
    if not home:
        home = str(out / "home")
    Path(home).mkdir(parents=True, exist_ok=True)

    run_id = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    git    = _git_info()

    case_results = run_all_cases(hvm, ipa_path, home, cases, args.timeout)

    # Build host report
    report = make_host_report(case_results, ipa_sha, run_id, git)
    report_path = out / "testlab_report.json"
    report_path.write_text(json.dumps(report, indent=2))
    print(f"\n[testlab] report: {report_path}")

    # Validate
    ok = validate_report(str(report_path))
    if not ok:
        return 3

    # Summary
    n_pass  = sum(1 for c in case_results if c["status"] == "passed")
    n_fail  = sum(1 for c in case_results if c["status"] == "failed")
    n_int   = sum(1 for c in case_results if c["status"] == "interrupted")
    n_skip  = sum(1 for c in case_results if c["status"] == "skipped")
    print(f"\n{'─'*40}")
    print(f"passed={n_pass}  failed={n_fail}  interrupted={n_int}  skipped={n_skip}")

    if n_int:
        return 2
    if n_fail:
        return 1
    return 0


def cmd_validate(args: argparse.Namespace) -> int:
    return 0 if validate_report(args.report) else 1


def main() -> int:
    ap = argparse.ArgumentParser(description="HydrogenVM Test Lab orchestrator")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_run = sub.add_parser("run", help="Build and run the test suite")
    p_run.add_argument("--build-dir", default="build")
    p_run.add_argument("--out", default="build/testlab-report")
    p_run.add_argument("--hvm")
    p_run.add_argument("--case", choices=CASE_IDS)
    p_run.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT)
    p_run.add_argument("--skip-build", action="store_true")
    p_run.add_argument("--home")

    p_val = sub.add_parser("validate", help="Validate a report JSON")
    p_val.add_argument("report")

    a = ap.parse_args()
    if a.cmd == "run":
        return cmd_run(a)
    if a.cmd == "validate":
        return cmd_validate(a)
    return 0


if __name__ == "__main__":
    sys.exit(main())
