#!/usr/bin/env python3
"""HydrogenVM Android Test Lab runner.

Builds tests/android/testlab.c into an armeabi-v7a library with plain clang (no NDK), wraps it in
a minimal imported-APK bundle, runs it through `hvm run --gpu` and checks the per-case output.

    python3 tools/android_testlab.py                 # build/hvm, report in build/android-testlab
    python3 tools/android_testlab.py --phone         # also run under phone mode

Exit status: 0 all cases passed, 1 a case failed, 2 the run itself broke (no clang, crash, ...).
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "tests" / "android" / "testlab.c"
LIB = "libhvmtestlab.so"
CLANG_FLAGS = ["--target=armv7a-linux-androideabi19", "-march=armv7-a", "-mfloat-abi=softfp", "-marm",
               "-O1", "-fPIC", "-shared", "-nostdlib", "-fuse-ld=lld", "-ffreestanding",
               "-fno-builtin", "-fno-stack-protector", "-Wl,-soname," + LIB]
PLIST = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
\t<key>CFBundleIdentifier</key><string>com.hydrogenvm.androidtestlab</string>
\t<key>CFBundleName</key><string>Android Test Lab</string>
\t<key>CFBundleExecutable</key><string>lib/armeabi-v7a/{lib}</string>
\t<key>HydrogenVMAndroidLibrary</key><string>lib/armeabi-v7a/{lib}</string>
\t<key>HydrogenVMAndroidEngine</key><string>Test Lab</string>
\t<key>HydrogenVMAndroidEngineSupport</key><string>driver</string>
\t<key>MinimumOSVersion</key><string>Android</string>
</dict>
</plist>
"""
LINE = re.compile(r"^(ok|FAIL) ([^:\n]+?)(?:: (.*))?$")


def find_clang() -> str:
    for name in ("clang", "clang-18", "clang-17", "clang-19"):
        path = shutil.which(name)
        if path:
            return path
    raise SystemExit("[android-testlab] clang not found")


def build_bundle(work: Path) -> Path:
    app = work / "AndroidTestLab.app"
    libdir = app / "lib" / "armeabi-v7a"
    libdir.mkdir(parents=True, exist_ok=True)
    cmd = [find_clang(), *CLANG_FLAGS, "-I", str(SRC.parent), str(SRC), "-o", str(libdir / LIB)]
    subprocess.run(cmd, check=True)
    (app / "Info.plist").write_text(PLIST.format(lib=LIB))
    return app


def parse(output: str) -> list:
    cases = []
    for line in output.splitlines():
        m = LINE.match(line.strip())
        if m:
            cases.append({"name": m.group(2), "passed": m.group(1) == "ok", "detail": m.group(3) or ""})
    return cases


def run_once(hvm: str, app: Path, home: Path, phone: bool, timeout: int) -> dict:
    cmd = [hvm, "run", str(app), "--gpu", "--home", str(home)]
    if phone:
        cmd.append("--phone")
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        out, code = proc.stdout + proc.stderr, proc.returncode
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"").decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        code = None
    cases = parse(out)
    finished = "done:" in out
    return {"mode": "phone" if phone else "desktop", "exitCode": code, "finished": finished,
            "cases": cases, "log": out[-20000:]}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--hvm", default=str(ROOT / "build" / "hvm"))
    ap.add_argument("--out", default=str(ROOT / "build" / "android-testlab"))
    ap.add_argument("--phone", action="store_true", help="also run under phone mode")
    ap.add_argument("--timeout", type=int, default=120)
    args = ap.parse_args()

    if not Path(args.hvm).exists():
        print(f"[android-testlab] hvm not found at {args.hvm}", file=sys.stderr)
        return 2
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        app = build_bundle(work)
        runs = []
        for phone in ([False, True] if args.phone else [False]):
            home = work / ("home-phone" if phone else "home")
            home.mkdir()
            runs.append(run_once(args.hvm, app, home, phone, args.timeout))

    (out / "report.json").write_text(json.dumps({"runs": runs}, indent=2))
    broken = failed = 0
    for run in runs:
        print(f"── {run['mode']} ──")
        for c in run["cases"]:
            print(f"  {'ok  ' if c['passed'] else 'FAIL'} {c['name']}" + (f" — {c['detail']}" if c["detail"] else ""))
        failed += sum(not c["passed"] for c in run["cases"])
        if not run["finished"] or not run["cases"]:
            broken += 1
            print(f"  run did not finish (exit {run['exitCode']}); last output:")
            print("    " + "\n    ".join(run["log"].splitlines()[-25:]))
    total = sum(len(r["cases"]) for r in runs)
    print(f"{total - failed}/{total} passed" + (", run broke" if broken else ""))
    return 2 if broken else (1 if failed else 0)


if __name__ == "__main__":
    sys.exit(main())
