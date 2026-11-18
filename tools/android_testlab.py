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
import plistlib
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "tests" / "android" / "testlab.c"
NA_SRC = ROOT / "tests" / "android" / "native_activity_test.c"
LIB = "libhvmtestlab.so"
NA_LIB = "libhvmnativeactivity.so"
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
\t<key>HydrogenVMAndroidEngine</key><string>{engine}</string>
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


def build_bundle(work: Path, src: Path = SRC, lib: str = LIB, name: str = "AndroidTestLab.app", engine: str = "Test Lab") -> Path:
    app = work / name
    libdir = app / "lib" / "armeabi-v7a"
    libdir.mkdir(parents=True, exist_ok=True)
    flags = [f if not f.startswith("-Wl,-soname,") else f"-Wl,-soname,{lib}" for f in CLANG_FLAGS]
    cmd = [find_clang(), *flags, "-I", str(src.parent), str(src), "-o", str(libdir / lib)]
    subprocess.run(cmd, check=True)
    (app / "Info.plist").write_text(PLIST.format(lib=lib, engine=engine))
    return app


def parse(output: str) -> list:
    cases = []
    for line in output.splitlines():
        m = LINE.match(line.strip())
        if m:
            cases.append({"name": m.group(2), "passed": m.group(1) == "ok", "detail": m.group(3) or ""})
    return cases


def run_once(hvm: str, app: Path, home: Path, phone: bool, timeout: int, extra_args: list[str] | None = None, mode_suffix: str = "") -> dict:
    cmd = [hvm, "run", str(app), "--gpu", "--home", str(home)]
    if phone:
        cmd.append("--phone")
    if extra_args:
        cmd.extend(extra_args)
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        out, code = proc.stdout + proc.stderr, proc.returncode
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"").decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        code = None
    cases = parse(out)
    finished = "done:" in out
    mode = ("phone" if phone else "desktop") + mode_suffix
    return {"mode": mode, "exitCode": code, "finished": finished,
            "cases": cases, "log": out[-20000:]}


def launcher_manifest() -> bytes:
    """Binary XML for a launcher alias preceded by misleading intent filters."""
    strings: list[str] = []

    def string(value: str) -> int:
        if value not in strings:
            strings.append(value)
        return strings.index(value)

    def node(kind: int, payload: bytes) -> bytes:
        return struct.pack("<HHIII", kind, 16, 16 + len(payload), 1, 0xffffffff) + payload

    def element(tag: str, attrs=None, children=()) -> bytes:
        name = string(tag)
        attrs = attrs or {}
        start = struct.pack("<IIHHHHHH", 0xffffffff, name, 20, 20, len(attrs), 0, 0, 0)
        for key, value in attrs.items():
            attr_name = string(key)
            attr_value = string(value) if isinstance(value, str) else 0xffffffff
            kind, data = (3, attr_value) if isinstance(value, str) else (0x12, int(value))
            start += struct.pack("<IIIHBBI", 0xffffffff, attr_name, attr_value, 8, 0, kind, data)
        return node(0x0102, start) + b"".join(children) + node(0x0103, struct.pack("<II", 0xffffffff, name))

    action = element("action", {"name": "android.intent.action.MAIN"})
    category = element("category", {"name": "android.intent.category.LAUNCHER"})
    launcher = element("intent-filter", children=(action, category))
    app = element("application", children=(
        element("activity", {"name": ".SplitFilters"}, (
            element("intent-filter", children=(action,)), element("intent-filter", children=(category,)))),
        element("activity", {"name": ".Disabled", "enabled": False}, (launcher,)),
        element("activity", {"name": ".Play"}),
        element("activity-alias", {"name": ".FromHome", "targetActivity": ".Play"}, (launcher,)),
    ))
    body = element("manifest", {"package": "com.example.launcher"}, (app,))
    values = bytearray()
    offsets = []
    for value in strings:
        offsets.append(len(values))
        values += struct.pack("<H", len(value)) + value.encode("utf-16-le") + b"\0\0"
    values += b"\0" * (-len(values) % 4)
    start = 28 + 4 * len(offsets)
    pool = struct.pack("<HHIIIIII", 1, 28, start + len(values), len(offsets), 0, 0, start, 0)
    pool += struct.pack(f"<{len(offsets)}I", *offsets) + values
    return struct.pack("<HHI", 3, 8, 8 + len(pool) + len(body)) + pool + body


def run_import_check(hvm: str, work: Path, timeout: int) -> dict:
    fixtures = ROOT / "tests/android/java"
    images = [(fixtures / name).read_bytes() for name in ("classes.dex", "classes2.dex", "jni.dex")]
    base, feature = work / "base.apk", work / "feature.apk"
    with zipfile.ZipFile(base, "w", zipfile.ZIP_DEFLATED) as apk:
        apk.writestr("AndroidManifest.xml", launcher_manifest())
        apk.writestr("classes2.dex", images[1])
        apk.writestr("classes.dex", images[0])
        for invalid in ("nested/classes.dex", "classes01.dex", "classes1.dex", "classesExtra.dex"):
            apk.writestr(invalid, b"ignored")
    with zipfile.ZipFile(feature, "w", zipfile.ZIP_DEFLATED) as apk:
        apk.writestr("classes.dex", images[2])
    split = work / "split.apks"
    with zipfile.ZipFile(split, "w") as archive:
        archive.write(feature, feature.name)
        archive.write(base, base.name)

    cases = []
    for source, expected in ((base, images[:2]), (split, images)):
        destination = work / ("installed-" + source.stem)
        run = subprocess.run([hvm, "import", str(source), str(destination)], capture_output=True,
                             text=True, timeout=timeout)
        bundles = list(destination.glob("*/*.app"))
        actual = {str(p.relative_to(bundles[0])): p.read_bytes() for p in bundles[0].rglob("*.dex")} if len(bundles) == 1 else {}
        wanted = dict(zip(("classes.dex", "classes2.dex", "classes3.dex"), expected))
        passed = run.returncode == 0 and actual == wanted
        if len(bundles) == 1:
            info = plistlib.loads((bundles[0] / "Info.plist").read_bytes())
            activity = info.get("HydrogenVMAndroidLauncherActivity")
        else:
            activity = None
        cases.append({"name": f"import {source.suffix} retains the enabled MAIN/LAUNCHER alias target",
                      "passed": activity == "com.example.launcher.Play", "detail": str(activity or "")})
        cases.append({"name": f"import {source.suffix} preserves canonical DEX images in order",
                      "passed": passed, "detail": "" if passed else run.stderr + repr(list(actual))})
    return {"mode": "APK import", "group": "java", "exitCode": 0, "finished": True,
            "cases": cases, "log": ""}


def run_java_check(work: Path, timeout: int, source: str, label: str) -> dict:
    """Run a host-side DEX check with the same C++ sources as the runtime."""
    fixtures = ROOT / "tests/android/java"
    executable = work / source
    compiler = shlex.split(os.environ.get("CXX", "c++"))
    try:
        build = subprocess.run(
            [*compiler, "-std=c++20", "-O2", "-pthread", "-I", str(ROOT / "src"),
             str(fixtures / (source + ".cpp")), str(ROOT / "src/runtime/android/dex/DexFile.cpp"),
             str(ROOT / "src/runtime/android/dex/Interpreter.cpp"), str(ROOT / "src/runtime/android/dex/Heap.cpp"),
             str(ROOT / "src/runtime/android/dex/Machine.cpp"),
             "-o", str(executable)], capture_output=True, text=True, timeout=timeout,
        )
        run = subprocess.run([str(executable), str(fixtures)], capture_output=True, text=True,
                             timeout=timeout) if build.returncode == 0 else build
        output, code = run.stdout + run.stderr, run.returncode
    except (OSError, subprocess.TimeoutExpired) as err:
        output, code = str(err), None
    return {"mode": f"java ({label}, host)", "group": "java", "exitCode": code, "finished": "done:" in output,
            "cases": parse(output), "log": output[-20000:]}


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
        na_app = build_bundle(work, NA_SRC, NA_LIB, "NativeActivityTest.app", "NativeActivity")
        java = ROOT / "tests/android/java"
        shutil.copyfile(java / "native-driver.dex", na_app / "classes.dex")
        bridge_app = build_bundle(work, java / "JniRoundTrip.c", LIB, "JniRoundTrip.app")
        shutil.copyfile(java / "jni.dex", bridge_app / "classes.dex")
        activity_app = build_bundle(work, java / "DexActivity.c", "libgdx.so", "DexActivity.app", "libGDX")
        activity_plist = activity_app / "Info.plist"
        props = plistlib.loads(activity_plist.read_bytes())
        props["HydrogenVMAndroidLauncherActivity"] = "test.Launcher"
        activity_plist.write_bytes(plistlib.dumps(props))
        shutil.copyfile(java / "activity.dex", activity_app / "classes.dex")
        runs = [
            run_import_check(args.hvm, work, args.timeout),
            run_java_check(work, args.timeout, "DexLayout", "DEX layout"),
            run_java_check(work, args.timeout, "DexExecution", "Dalvik execution"),
            run_java_check(work, args.timeout, "DexLinkage", "Dalvik class linkage and threads"),
        ]
        for phone in ([False, True] if args.phone else [False]):
            home = work / ("home-phone" if phone else "home")
            home.mkdir()
            runs.append(run_once(args.hvm, app, home, phone, args.timeout))
            bridge_home = work / ("jni-home-phone" if phone else "jni-home")
            bridge_home.mkdir()
            runs.append(run_once(args.hvm, bridge_app, bridge_home, phone, args.timeout,
                                 mode_suffix=" (DEX JNI round-trip)"))
            runs[-1]["group"] = "java"
            activity_home = work / ("activity-home-phone" if phone else "activity-home")
            activity_home.mkdir()
            runs.append(run_once(args.hvm, activity_app, activity_home, phone, args.timeout,
                                 extra_args=["--run-for", "1", "--graceful"], mode_suffix=" (DEX activity)"))
            runs[-1]["group"] = "java"
            na_home = work / ("na-home-phone" if phone else "na-home")
            na_home.mkdir()
            runs.append(run_once(args.hvm, na_app, na_home, phone, args.timeout,
                                 extra_args=["--tap", "100,100@0.5", "--for", "5s"],
                                 mode_suffix=" (NativeActivity)"))

    (out / "report.json").write_text(json.dumps({"runs": runs}, indent=2))
    broken = failed = 0
    for run in runs:
        print(f"── {run['mode']} ──")
        for c in run["cases"]:
            print(f"  {'ok  ' if c['passed'] else 'FAIL'} {c['name']}" + (f" — {c['detail']}" if c["detail"] else ""))
        failed += sum(not c["passed"] for c in run["cases"])
        bad_exit = run["exitCode"] not in (0, 1)
        if not run["finished"] or not run["cases"] or (run.get("group") == "java" and bad_exit):
            broken += 1
            print(f"  run did not finish (exit {run['exitCode']}); last output:")
            print("    " + "\n    ".join(run["log"].splitlines()[-25:]))
        elif bad_exit:
            print(f"  process exited {run['exitCode']} after reporting completion")
    total = sum(len(r["cases"]) for r in runs)
    print(f"{total - failed}/{total} passed" + (", run broke" if broken else ""))
    return 2 if broken else (1 if failed else 0)


if __name__ == "__main__":
    sys.exit(main())
