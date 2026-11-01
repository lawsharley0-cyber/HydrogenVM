#!/usr/bin/env python3
"""Build/strip a project-owned guest and verify real crash-bundle method names."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--hvm", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    args = ap.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    subprocess.run([sys.executable, str(ROOT / "tools/ios_build.py"), "app", "--name", "ObjCSymbolication",
                    "--out", str(out), "--cflags=-fno-omit-frame-pointer", "--cflags=-fno-optimize-sibling-calls",
                    str(ROOT / "tests/ios/apps/objcsymbolication/main.m")], check=True)
    app = out / "ObjCSymbolication.app"
    executable = app / "ObjCSymbolication"
    subprocess.run(["llvm-strip", "--strip-all", str(executable)], check=True)
    nm = subprocess.run(["llvm-nm", "--defined-only", str(executable)], capture_output=True, text=True, check=True)
    if "HVMTrace" in nm.stdout or "HVMBoot" in nm.stdout:
        raise RuntimeError("fixture still contains named method symbols")
    (out / "nm.log").write_text(nm.stdout + nm.stderr, encoding="utf-8")
    report = out / "hvm-report.json"
    result = subprocess.run([str(args.hvm.resolve()), "run", str(app), "--home", str(out / "home"),
                             "--report", str(report)], capture_output=True, text=True, timeout=30)
    (out / "run.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    data = json.loads(report.read_text(encoding="utf-8"))
    trace = "\n".join((data.get("crashBundle") or {}).get("backtrace") or [])
    expected = ["-[HVMTraceCaller enter]", "+[HVMTraceCallee trap]", "+[NSObject(HVMBoot) load]"]
    found = {name: name in trace for name in expected}
    # A guest trap is reported as EX_SOFTWARE (70), not a generic tool error.
    ok = result.returncode == 70 and data.get("exitStatus") == 70 and data.get("crashed") is True and all(found.values())
    summary = {"schemaVersion": 1, "expected": expected, "found": found,
               "returnCode": result.returncode, "guestSHA256": hashlib.sha256(executable.read_bytes()).hexdigest(),
               "host": (data.get("crashBundle") or {}).get("host"), "ok": ok}
    (out / "verification.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2))
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
