"""Run the existing suites and the manifest apps. No emulator changes.

The C++ case list matches ``tools/run_tests.sh``. Python cases are the
packaging and Clash fixture tests that do not need a downloaded client.
"""
from __future__ import annotations

import datetime
import json
import os
import platform
import re
import socket
import subprocess
import sys
import threading
import time

from . import cluster as cluster_mod
from . import frame as frame_mod
from .manifest import repo_root

INT_GUESTS = (
    "ctests_arm",
    "ctests_arm_os",
    "ctests_thumb",
    "ctests_thumb_os",
    "ctests_thumb_idiv",
)
FP_GUESTS = (
    "ftests_vfp_arm_hard",
    "ftests_vfp_thumb_softfp",
    "ftests_neon_thumb",
    "ftests_neon_arm",
    "ftests_neon_thumb_os",
)

LIMITS = {
    "runsOnLinuxHeadless": [
        "cmake/ninja build of hvm, hvm_tests, hvm_decodecheck",
        "hvm_tests selftest and fulltest",
        "asm and differential C guests (tools/build_tests.sh, ld.lld, armv7 clang)",
        "python tools/tests/test_verify_ipa.py and the Clash protocol/home fixture tests",
        "hvm run of decrypted IPAs with a software framebuffer, PNG screenshot, and JSON report",
        "mixer levels when a datagram socket is passed as --audio-fd (no sound device)",
    ],
    "notRunHere": [
        "Xcode, SwiftUI app/, simulator (tools/sim_test.sh), device signing, entitlements, Metal",
        "iOS stub-SDK guests and CoreMark, which need ld64 from tools/build_cctools.sh",
        "Clash Royale native two-launch probe; the IPA is not in git and this harness does not download it",
        "16 KB host-page memory model (this report records the host page size and does not claim an iPhone pass)",
        "pygame desktop viewer (tools/desktop.py), which needs a display",
        "GitHub Actions execution: runs since 239a0ab have been dying in seconds before any step (billing is the project's stated cause; the vendor string was not visible to the audit)",
    ],
}


def run_unit(root: str, build_dir: str) -> dict:
    hvm_tests = os.path.join(build_dir, "hvm_tests")
    tests = os.path.join(build_dir, "tests")
    cases = []
    if not os.path.isfile(hvm_tests):
        return _suite([_case("hvm_tests", "fail", 127, 0, f"missing binary {hvm_tests}")])
    cases.append(_exec("selftest", [hvm_tests, "selftest"], root))
    cases.append(_exec("fulltest", [hvm_tests, "fulltest"], root))
    asm_dir = os.path.join(root, "tests", "asm")
    if os.path.isdir(asm_dir):
        for name in sorted(os.listdir(asm_dir)):
            if not name.endswith(".S"):
                continue
            stem = name[:-2]
            cases.append(_exec(
                f"asm:{stem}",
                [hvm_tests, "asm", os.path.join(tests, stem + ".elf"), os.path.join(asm_dir, name)],
                root,
            ))
    expected_c = os.path.join(tests, "expected_ctests.txt")
    expected_f = os.path.join(tests, "expected_ftests.txt")
    for name in INT_GUESTS:
        cases.append(_exec(
            f"c:{name}",
            [hvm_tests, "c", os.path.join(tests, name + ".elf"), expected_c],
            root,
        ))
    for name in FP_GUESTS:
        cases.append(_exec(
            f"c:{name}",
            [hvm_tests, "c", os.path.join(tests, name + ".elf"), expected_f],
            root,
        ))
    return _suite(cases)


def run_python(root: str) -> dict:
    py = sys.executable or "python3"
    specs = [
        ("verify_ipa", [py, "-m", "unittest", "discover", "-s", os.path.join(root, "tools", "tests"), "-p", "test_verify_ipa.py"]),
        ("clash_protocol", [py, os.path.join(root, "tools", "test_clash_protocol_probe.py")]),
        ("clash_home_state", [py, os.path.join(root, "tools", "test_clash_home_state.py")]),
    ]
    return _suite([_exec(name, cmd, root) for name, cmd in specs])


def run_fuzz(root: str, build_dir: str, count: int) -> dict:
    binary = os.path.join(build_dir, "hvm_decodecheck")
    if count <= 0:
        return {"status": "not-run", "reason": "pass --fuzz N to sample the decoder", "cases": []}
    if not os.path.isfile(binary):
        return _suite([_case("decode_fuzz", "fail", 127, 0, "hvm_decodecheck is missing")])
    case = _exec(
        "decode_fuzz",
        [sys.executable or "python3", os.path.join(root, "tools", "decode_fuzz.py"), binary, str(count)],
        root,
        timeout=max(120, count // 50),
    )
    return _suite([case])


def run_apps(manifest: dict, resolved: list[dict], *, hvm: str, out_dir: str, seconds: float | None) -> dict:
    apps = []
    by_id = {a["id"]: a for a in manifest["apps"]}
    if not os.path.isfile(hvm):
        for item in resolved:
            apps.append(_unrun(item, by_id[item["id"]], "error" if item["status"] == "resolved" else item["status"],
                                item.get("reason") or f"hvm binary missing: {hvm}"))
        return {"status": "fail", "hvm": hvm, "apps": apps}
    os.makedirs(out_dir, exist_ok=True)
    for item in resolved:
        spec = by_id[item["id"]]
        if item["status"] != "resolved":
            apps.append(_unrun(item, spec, item["status"], item.get("reason") or ""))
            continue
        run_seconds = float(seconds if seconds is not None else spec.get("seconds") or 3)
        apps.append(_run_one(hvm, spec, item, out_dir, run_seconds))
    # Load errors and crashes are measurements. Timeouts and fixture/hash
    # errors mean the harness did not measure the app.
    failed = any(a["status"] in ("error", "timeout") for a in apps)
    return {"status": "fail" if failed else "pass", "hvm": os.path.relpath(hvm, repo_root()) if hvm.startswith(repo_root()) else hvm, "apps": apps}


def build_report(*, root: str, unit: dict, python: dict, fuzz: dict, apps: dict, manifest_path: str,
                 fixture_root: str | None, allow_legacy: bool) -> dict:
    app_rows = apps.get("apps") or []
    for row in app_rows:
        row["clusters"] = cluster_mod.clusters_for_app(row)
        for item in row["clusters"]:
            item["apps"] = [row["id"]]
    clusters = cluster_mod.aggregate(app_rows)
    unit_failed = sum(1 for c in unit.get("cases") or [] if c["status"] != "pass")
    py_failed = sum(1 for c in python.get("cases") or [] if c["status"] != "pass")
    resolve_errors = sum(1 for a in app_rows if a["status"] == "error")
    crashed = sum(1 for a in app_rows if a.get("crashed"))
    launch_failed = sum(1 for a in app_rows if a["status"] in ("ran", "crashed", "load-error", "timeout") and not a.get("launchSuccess"))
    skipped = sum(1 for a in app_rows if a["status"] == "skipped")
    ok = unit_failed == 0 and py_failed == 0 and resolve_errors == 0 and apps.get("status") != "fail"
    # A crash during a smoke is data, not a harness failure. Hash errors and
    # timeouts are harness failures because the run did not measure the app.
    if any(a["status"] == "timeout" for a in app_rows):
        ok = False
    return {
        "schemaVersion": 1,
        "kind": "hvm-compat-report",
        "generatedAt": datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0).isoformat(),
        "git": _git(root),
        "host": _host(),
        "limits": LIMITS,
        "manifest": {
            "path": os.path.relpath(manifest_path, root) if os.path.isabs(manifest_path) else manifest_path,
            "schemaVersion": 1,
            "fixtureRoot": fixture_root,
            "legacyRepoAllowed": allow_legacy,
        },
        "suites": {"unit": unit, "python": python, "fuzz": fuzz, "apps": apps},
        "clusters": clusters,
        "summary": {
            "ok": ok,
            "unitFailed": unit_failed,
            "pythonFailed": py_failed,
            "fuzzStatus": fuzz.get("status"),
            "appsResolved": sum(1 for a in app_rows if a.get("source") and a["status"] not in ("skipped", "error")),
            "appsSkipped": skipped,
            "appsCrashed": crashed,
            "appsLaunchFailed": launch_failed,
            "resolveErrors": resolve_errors,
            "clusterCount": len(clusters),
        },
    }


def _run_one(hvm: str, spec: dict, item: dict, out_dir: str, seconds: float) -> dict:
    work = os.path.join(out_dir, "work", spec["id"])
    os.makedirs(work, exist_ok=True)
    report_path = os.path.join(work, "hvm-report.json")
    shot_path = os.path.join(work, "frame.png")
    home = os.path.join(work, "home")
    os.makedirs(home, exist_ok=True)
    for stale in (report_path, shot_path):
        if os.path.exists(stale):
            os.remove(stale)
    audio = _AudioBridge()
    cmd = [
        hvm, "run", item["path"],
        "--home", home,
        "--run-for", _seconds_arg(seconds),
        "--report", report_path,
        "--screenshot", shot_path,
        "--quiet",
    ]
    if audio.fd is not None:
        cmd.extend(["--audio-fd", str(audio.fd)])
    display_cmd = [
        "hvm", "run", item["displayPath"],
        "--home", "<work>/home",
        "--run-for", _seconds_arg(seconds),
        "--report", "<work>/hvm-report.json",
        "--screenshot", "<work>/frame.png",
        "--quiet",
    ]
    if audio.fd is not None:
        display_cmd.extend(["--audio-fd", "<datagram>"])
    started = time.monotonic()
    timed_out = False
    log = ""
    exit_code = None
    try:
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            errors="replace",
            timeout=max(45.0, seconds * 5 + 30),
            pass_fds=audio.pass_fds,
        )
        exit_code = proc.returncode
        log = (proc.stdout or "") + (proc.stderr or "")
    except subprocess.TimeoutExpired as e:
        timed_out = True
        exit_code = None
        out = e.stdout or b""
        err = e.stderr or b""
        if isinstance(out, bytes):
            out = out.decode("utf-8", "replace")
        if isinstance(err, bytes):
            err = err.decode("utf-8", "replace")
        log = out + err + "\n[harness] hvm exceeded the wall-clock timeout"
    except OSError as e:
        log = f"[harness] failed to start hvm: {e}"
        exit_code = 127
    finally:
        audio.close()
    duration = round(time.monotonic() - started, 3)
    hvm_report = _read_json(report_path)
    row = _base_row(item, spec)
    row["durationSec"] = duration
    row["command"] = display_cmd
    row["exitStatus"] = hvm_report.get("exitStatus", exit_code)
    row["stillRunning"] = bool(hvm_report.get("stillRunning"))
    row["crashed"] = bool(hvm_report.get("crashed"))
    row["loadError"] = hvm_report.get("loadError") or ""
    row["crashReport"] = hvm_report.get("crashReport") or ""
    row["unimplementedImports"] = list(hvm_report.get("unimplementedImports") or [])
    row["missingFunctions"] = list(hvm_report.get("missingFunctions") or [])
    row["warnings"] = _pairs(hvm_report.get("warnings"))
    row["errors"] = _pairs(hvm_report.get("errors"))
    row["perf"] = _round_numbers(hvm_report.get("perf") or {})
    row["audio"] = _audio_signal(hvm_report.get("audio") or {}, measured=audio.fd is not None, setup_error=audio.error)
    row["logExcerpt"] = _redact(log[-4000:], repo_root())
    row["frame"] = _frame_signal(shot_path, row["perf"])
    if timed_out and not hvm_report:
        row["status"] = "timeout"
        row["launchSuccess"] = False
        row["reason"] = "hvm exceeded the wall-clock timeout"
    elif not hvm_report:
        row["status"] = "error"
        row["launchSuccess"] = False
        row["reason"] = "hvm did not write a report"
    elif row["loadError"]:
        row["status"] = "load-error"
        row["launchSuccess"] = False
    elif row["crashed"]:
        row["status"] = "crashed"
        row["launchSuccess"] = False
    else:
        row["status"] = "ran"
        row["launchSuccess"] = bool(row["stillRunning"] or row["exitStatus"] == 0)
        if not row["launchSuccess"]:
            row["reason"] = f"exit status {row['exitStatus']}"
    return row


def _frame_signal(shot_path: str, perf: dict) -> dict:
    presented = perf.get("framesPresented") or 0
    if not frame_mod.png_exists(shot_path):
        signal = "no-frames" if not presented else "no-screenshot"
        return {"signal": signal, "framesPresented": presented}
    summary = frame_mod.summarize_png(shot_path)
    summary["framesPresented"] = presented
    if not presented and summary.get("signal") not in ("unreadable",):
        summary["signal"] = "no-frames"
    return summary


def _audio_signal(raw: dict, *, measured: bool, setup_error: str) -> dict:
    if not measured:
        return {"measured": False, "signal": "not-measured", "setupError": setup_error, "enabled": False,
                "frames": 0, "nonSilentFrames": 0, "peak": 0, "droppedBlocks": 0, "transportError": 0}
    frames = int(raw.get("frames") or 0)
    non_silent = int(raw.get("nonSilentFrames") or 0)
    error = int(raw.get("transportError") or 0)
    if error:
        signal = "transport-error"
    elif non_silent > 0:
        signal = "active"
    elif frames > 0:
        signal = "silent"
    else:
        signal = "no-mixer-output"
    return {
        "measured": True,
        "signal": signal,
        "setupError": setup_error,
        "enabled": bool(raw.get("enabled")),
        "frames": frames,
        "nonSilentFrames": non_silent,
        "peak": raw.get("peak") or 0,
        "droppedBlocks": int(raw.get("droppedBlocks") or 0),
        "transportError": error,
    }


class _AudioBridge:
    def __init__(self) -> None:
        self.fd = None
        self.pass_fds: tuple[int, ...] = ()
        self.error = ""
        self._recv = None
        self._send = None
        self._stop = threading.Event()
        self._thread = None
        try:
            recv, send = socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM)
        except OSError as e:
            self.error = str(e)
            return
        recv.setblocking(False)
        self._recv = recv
        self._send = send
        self.fd = send.fileno()
        self.pass_fds = (self.fd,)
        self._thread = threading.Thread(target=self._drain, daemon=True)
        self._thread.start()

    def _drain(self) -> None:
        assert self._recv is not None
        while not self._stop.is_set():
            try:
                self._recv.recv(1 << 16)
            except BlockingIOError:
                self._stop.wait(0.02)
            except OSError:
                return

    def close(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=1)
        for sock in (self._send, self._recv):
            if sock is not None:
                try:
                    sock.close()
                except OSError:
                    pass


def _base_row(item: dict, spec: dict) -> dict:
    return {
        "id": item["id"],
        "name": spec.get("name") or item["id"],
        "version": spec.get("version") or "",
        "optional": bool(spec.get("optional")),
        "sha256": item["sha256"],
        "source": item.get("source"),
        "displayPath": item.get("displayPath"),
        "status": item["status"],
        "reason": item.get("reason") or "",
        "launchSuccess": False,
        "crashed": False,
        "exitStatus": None,
        "stillRunning": False,
        "loadError": "",
        "unimplementedImports": [],
        "missingFunctions": [],
        "warnings": [],
        "errors": [],
        "crashReport": "",
        "perf": {},
        "frame": {"signal": "not-run"},
        "audio": {"measured": False, "signal": "not-measured"},
        "logExcerpt": "",
        "clusters": [],
        "durationSec": 0,
        "command": [],
    }


def _unrun(item: dict, spec: dict, status: str, reason: str) -> dict:
    row = _base_row(item, spec)
    row["status"] = status
    row["reason"] = reason
    row["clusters"] = []
    return row


def _exec(name: str, cmd: list[str], cwd: str, timeout: float = 600) -> dict:
    started = time.monotonic()
    try:
        proc = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, errors="replace", timeout=timeout)
    except subprocess.TimeoutExpired:
        return _case(name, "fail", None, round(time.monotonic() - started, 3), "timeout")
    except OSError as e:
        return _case(name, "fail", 127, round(time.monotonic() - started, 3), str(e))
    text = ((proc.stdout or "") + (proc.stderr or "")).strip()
    summary = text[-500:] if text else f"exit {proc.returncode}"
    status = "pass" if proc.returncode == 0 else "fail"
    return _case(name, status, proc.returncode, round(time.monotonic() - started, 3), summary)


def _case(name: str, status: str, exit_code, duration: float, summary: str) -> dict:
    return {"id": name, "status": status, "exitCode": exit_code, "durationSec": duration, "summary": summary}


def _suite(cases: list[dict]) -> dict:
    failed = sum(1 for c in cases if c["status"] != "pass")
    return {
        "status": "fail" if failed else "pass",
        "passed": len(cases) - failed,
        "failed": failed,
        "cases": cases,
    }


def _host() -> dict:
    page = None
    try:
        page = os.sysconf("SC_PAGE_SIZE")
    except (AttributeError, ValueError, OSError):
        page = None
    compiler = ""
    try:
        proc = subprocess.run(["clang++", "--version"], capture_output=True, text=True, timeout=10)
        compiler = (proc.stdout or proc.stderr or "").splitlines()[0] if proc.returncode == 0 else ""
    except (OSError, subprocess.TimeoutExpired):
        compiler = ""
    return {
        "system": platform.system(),
        "release": platform.release(),
        "machine": platform.machine(),
        "python": platform.python_version(),
        "pageSize": page,
        "compiler": compiler,
        "notes": [
            "Host page size is the kernel page. Guest permissions are 4 KB and are not enforced on the fast path.",
            "A pass recorded here is not a 16 KB iOS memory-model pass.",
        ],
    }


def _git(root: str) -> dict:
    def capture(args: list[str]) -> str:
        try:
            proc = subprocess.run(args, cwd=root, capture_output=True, text=True, timeout=10)
        except (OSError, subprocess.TimeoutExpired):
            return ""
        return proc.stdout.strip() if proc.returncode == 0 else ""
    commit = capture(["git", "rev-parse", "HEAD"])
    return {
        "commit": commit,
        "short": commit[:12],
        "branch": capture(["git", "rev-parse", "--abbrev-ref", "HEAD"]),
        "dirty": bool(capture(["git", "status", "--porcelain"])),
    }


def _read_json(path: str) -> dict:
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError):
        return {}
    return data if isinstance(data, dict) else {}


def _pairs(value) -> list:
    if not isinstance(value, list):
        return []
    out = []
    for item in value[:50]:
        if isinstance(item, dict) and "message" in item:
            out.append({"message": str(item["message"])[:500], "count": item.get("count", 1)})
        elif isinstance(item, str):
            out.append({"message": item[:500], "count": 1})
    return out


def _round_numbers(value: dict) -> dict:
    out = {}
    for key, item in value.items():
        if isinstance(item, float):
            out[key] = round(item, 3)
        else:
            out[key] = item
    return out


_UUID = re.compile(
    r"(?<![0-9A-Fa-f])[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}(?![0-9A-Fa-f])"
)
_KEY = re.compile(r"(?i)((?:key|token|secret|password)\s*(?:=|:|is\s*[:=])\s*)(\S+)")


def _redact(text: str, root: str) -> str:
    """Drop host paths, guest analytics keys, and UUIDs before a report is stored."""
    if root and root in text:
        text = text.replace(root + os.sep, "").replace(root, ".")
    text = _KEY.sub(lambda m: m.group(1) + "<redacted>", text)
    text = _UUID.sub("<uuid>", text)
    cleaned = []
    for ch in text:
        if ch in "\n\t\r" or 32 <= ord(ch) < 127:
            cleaned.append(ch)
        else:
            cleaned.append("?")
    return "".join(cleaned)


def _seconds_arg(seconds: float) -> str:
    if float(seconds).is_integer():
        return str(int(seconds))
    return str(seconds)
