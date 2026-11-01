#!/usr/bin/env python3
"""HydrogenVM test + performance report.

Runs every test suite, records every problem it sees (failed checks, crashes,
missing functions, runtime errors and warnings, compiler warnings), runs the
speed benchmarks, and writes:

  <out>/report.json   machine-readable, everything
  <out>/report.md     human-readable summary (also appended to the GitHub
                      Actions job summary when $GITHUB_STEP_SUMMARY is set)

usage: tools/hvm_report.py [--build-dir build] [--out report]
                           [--skip-build] [--prebuilt-apps] [--quick]
                           [--only cpu,ios,bench] [--history perf/history.csv]

Exit status is 1 if anything failed (warnings alone don't fail the run).
"""
import argparse, datetime, json, os, platform, re, shutil, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
COREMARK_REPO = "https://github.com/eembc/coremark"
COREMARK_COMMIT = "1f483d5b8316753a742cbf5590caf5bd0a4e4777"

problems = []          # {suite, test, severity, message}
suites = {}            # name -> {passed, failed, skipped, seconds, tests: [...]}
speed = {}


def problem(suite, test, severity, message):
    problems.append({"suite": suite, "test": test, "severity": severity, "message": message.strip()})


def suite(name):
    return suites.setdefault(name, {"passed": 0, "failed": 0, "skipped": 0, "seconds": 0.0, "tests": []})


def record(sname, test, ok, seconds=0.0, **extra):
    s = suite(sname)
    s["passed" if ok is True else "skipped" if ok is None else "failed"] += 1
    s["seconds"] += seconds
    s["tests"].append({"name": test, "result": "pass" if ok is True else "skip" if ok is None else "fail",
                       "seconds": round(seconds, 3), **extra})


def run(cmd, timeout=600, cwd=None, env=None):
    t0 = time.time()
    try:
        p = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True, timeout=timeout, errors="replace")
        return p.returncode, p.stdout, p.stderr, time.time() - t0
    except subprocess.TimeoutExpired as e:
        out = e.stdout.decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        err = e.stderr.decode(errors="replace") if isinstance(e.stderr, bytes) else (e.stderr or "")
        return 124, out, err + f"\n(timed out after {timeout}s)", time.time() - t0
    except FileNotFoundError as e:
        return 127, "", str(e), time.time() - t0


def host_info():
    cpu = platform.processor() or platform.machine()
    try:
        if sys.platform == "darwin":
            cpu = subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"], capture_output=True, text=True).stdout.strip() or cpu
        else:
            for line in open("/proc/cpuinfo"):
                if line.lower().startswith("model name"):
                    cpu = line.split(":", 1)[1].strip()
                    break
    except Exception:
        pass
    commit = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip()
    return {"os": f"{platform.system()} {platform.release()}", "arch": platform.machine(), "cpu": cpu, "commit": commit,
            "date": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds")}


# ------------------------------------------------------------------ build ---
def do_build(b):
    rc, out, err, t = run(["cmake", "-S", ROOT, "-B", b, "-G", "Ninja", "-DCMAKE_CXX_COMPILER=clang++", "-DCMAKE_BUILD_TYPE=Release"])
    if rc:
        problem("build", "cmake", "fail", err[-3000:])
        record("build", "cmake", False, t)
        return False
    rc, out, err, t2 = run(["ninja", "-C", b], timeout=1800)
    text = out + err
    warnings = sorted(set(m.group(0) for m in re.finditer(r"[^\s:]+:\d+:\d+: warning: [^\n]+", text)))
    for w in warnings:
        problem("build", "compiler", "warning", w.replace(ROOT + "/", ""))
    if rc:
        errors = re.findall(r"[^\n]*error[^\n]*", text)
        problem("build", "ninja", "fail", "\n".join(errors[:40]) or text[-3000:])
        record("build", "engine", False, t + t2)
        return False
    record("build", "engine", True, t + t2, warnings=len(warnings))
    return True


# -------------------------------------------------------------------- cpu ---
def do_cpu(b):
    rc, out, err, t = run([sys.executable, "-m", "unittest", "discover", "-s",
                           os.path.join(ROOT, "tools", "tests"), "-p", "test_corpus_tools.py", "-v"], cwd=ROOT)
    record("tools", "corpus bisect and A/B regression tests", rc == 0, t)
    if rc:
        problem("tools", "corpus bisect and A/B regression tests", "fail", (out + err)[-3000:])
    tb = os.path.join(b, "tests")
    rc, out, err, t = run([os.path.join(ROOT, "tools", "build_tests.sh"), tb], timeout=1800)
    if rc:
        problem("cpu", "build_tests.sh", "fail", (out + err)[-3000:])
        record("cpu", "build guest tests", False, t)
        return
    tests = os.path.join(b, "hvm_tests")
    jobs = [("selftest", [tests, "selftest"])]
    for f in sorted(os.listdir(os.path.join(ROOT, "tests", "asm"))):
        if f.endswith(".S"):
            n = f[:-2]
            jobs.append((f"asm/{n}", [tests, "asm", os.path.join(tb, n + ".elf"), os.path.join(ROOT, "tests", "asm", f)]))
    for e in ["ctests_arm", "ctests_arm_os", "ctests_thumb", "ctests_thumb_os", "ctests_thumb_idiv"]:
        jobs.append((f"c/{e}", [tests, "c", os.path.join(tb, e + ".elf"), os.path.join(tb, "expected_ctests.txt")]))
    for e in ["ftests_vfp_arm_hard", "ftests_vfp_thumb_softfp", "ftests_neon_thumb", "ftests_neon_arm", "ftests_neon_thumb_os"]:
        jobs.append((f"c/{e}", [tests, "c", os.path.join(tb, e + ".elf"), os.path.join(tb, "expected_ftests.txt")]))
    for name, cmd in jobs:
        rc, out, err, t = run(cmd)
        text = out + err
        fails = [l for l in text.splitlines() if l.startswith("FAIL")]
        m = re.search(r"(\d+) passed, (\d+) failed", text)
        counts = {"checks": int(m.group(1)) + int(m.group(2)), "failedChecks": int(m.group(2))} if m else {}
        ok = rc == 0 and not fails
        for l in fails:
            problem("cpu", name, "fail", l)
        if rc and not fails:
            problem("cpu", name, "fail", f"exit {rc}: " + text[-1500:])
        record("cpu", name, ok, t, **counts)


# -------------------------------------------------------------------- ios ---
APPS = [
    # name, build args, expected exit, expected line, run args
    ("CTest", ["ctest/main.c"], 3, "atexit handler ran", []),
    ("CTestLegacy", ["--min-ios", "3.0", "--no-pie", "ctest/main.c"], 3, "atexit handler ran", []),
    ("BinPlist", ["--binary-plist", "ctest/main.c"], 3, "atexit handler ran", []),
    ("ObjCTest", ["objctest/main.m"], 0, "ALL CHECKS PASSED", []),
    ("FoundationTest", ["@foundationtest", "--resource", "config.plist", "--resource", "en.lproj", "main.m"], 0, "ALL CHECKS PASSED", []),
    ("UIKitTest", ["@uikit"], 0, "ALL CHECKS PASSED", ["--tap", "70,350@0.3", "--tap", "230,350@0.5"]),
]


def build_ios_app(name, spec, out):
    apps = os.path.join(ROOT, "tests", "ios", "apps")
    base = ["python3", os.path.join(ROOT, "tools", "ios_build.py"), "app", "--name", name, "--out", out]
    cwd = None
    if spec[0] == "@foundationtest":
        cwd, spec = os.path.join(apps, "foundationtest"), spec[1:]
    elif spec[0] == "@uikit":
        res = os.path.join(out, "_work", "uikit-res")
        rc, o, e, _ = run(["python3", os.path.join(apps, "uikittest", "make_resources.py"), res])
        if rc:
            return rc, o + e
        spec = []
        for r in ["test.png", "test@2x.png", "crushed.png", "Panel.nib", "PanelKeyed.nib"]:
            spec += ["--resource", os.path.join(res, r)]
        spec.append("uikittest/main.m")
    spec = [os.path.join(apps, s) if s.endswith((".c", ".m")) and cwd is None else s for s in spec]
    rc, o, e, _ = run(base + spec, cwd=cwd, timeout=900)
    return rc, o + e


def run_app(b, app, run_args, timeout=180, extra=None):
    """Run an .app with hvm --report; returns (rc, stdout, stderr, seconds, report-dict)."""
    home = tempfile.mkdtemp(prefix="hvm-home-")
    rep = os.path.join(home, "report.json")
    rc, out, err, t = run([os.path.join(b, "hvm"), "run", app, "--home", home, "--quiet", "--report", rep] + run_args + (extra or []),
                          timeout=timeout)
    data = {}
    try:
        data = json.load(open(rep))
    except Exception:
        pass
    shutil.rmtree(home, ignore_errors=True)
    return rc, out, err, t, data


def app_problems(sname, name, data, err):
    for f in data.get("missingFunctions", []):
        problem(sname, name, "missing", f"missing function: {f}")
    for e in data.get("errors", []):
        problem(sname, name, "error", e["message"] + (f"  (x{e['count']})" if e["count"] > 1 else ""))
    for w in data.get("warnings", []):
        if w["message"].startswith("Missing: "):
            continue   # already listed via missingFunctions
        problem(sname, name, "warning", w["message"] + (f"  (x{w['count']})" if w["count"] > 1 else ""))
    if data.get("crashed"):
        problem(sname, name, "crash", data.get("crashReport", "")[:4000])
    if data.get("loadError"):
        problem(sname, name, "fail", "load failed: " + data["loadError"])
    if not data and err.strip():
        problem(sname, name, "error", err[-2000:])


def do_ios(b, prebuilt):
    out = os.path.join(b, "ios")
    os.makedirs(out, exist_ok=True)
    ld = os.environ.get("HVM_LD64", "/opt/cctools/bin/arm-apple-darwin11-ld")
    can_build = os.path.exists(ld)
    for name, spec, want_rc, want_line, run_args in APPS:
        app = os.path.join(out, name + ".app")
        if not prebuilt:
            if not can_build:
                record("ios", name, None)
                problem("ios", name, "skip", f"ld64 not found ({ld}); set HVM_LD64 or run tools/build_cctools.sh")
                continue
            rc, text = build_ios_app(name, spec, out)
            if rc:
                problem("ios", name, "fail", "build failed:\n" + text[-3000:])
                record("ios", name, False)
                continue
        if not os.path.isdir(app):
            record("ios", name, None)
            problem("ios", name, "skip", f"{app} not found")
            continue
        rc, sout, serr, t, data = run_app(b, app, run_args)
        lines = sout.splitlines()
        ok = rc == want_rc and want_line in lines
        if not ok:
            failed = [l for l in lines if l.startswith(("FAIL", "not ok", "CHECK FAILED")) or "FAILED" in l]
            problem("ios", name, "fail", f"exit {rc} (want {want_rc}); expected line {'found' if want_line in lines else 'missing'}\n"
                    + "\n".join(failed[:40] or lines[-25:]) + ("\n--- stderr ---\n" + serr[-1500:] if serr.strip() else ""))
        app_problems("ios", name, data, serr)
        record("ios", name, ok, t, perf=data.get("perf", {}))


# ------------------------------------------------------------------ bench ---
def ensure_coremark():
    d = os.path.join(ROOT, "third_party", "coremark")
    if not os.path.exists(os.path.join(d, "core_main.c")):
        os.makedirs(os.path.dirname(d), exist_ok=True)
        subprocess.run(["git", "clone", "-q", COREMARK_REPO, d], check=True)
        subprocess.run(["git", "-C", d, "checkout", "-q", COREMARK_COMMIT], check=True)
    return d


def do_bench(b, quick, prebuilt):
    targets = json.load(open(os.path.join(ROOT, "tools", "perf_targets.json")))["targets"]
    # 1. raw interpreter speed on the CPU test programs
    mips = {}
    for e in ["ctests_thumb", "ctests_arm", "ftests_vfp_thumb_softfp", "ftests_neon_thumb"]:
        elf = os.path.join(b, "tests", e + ".elf")
        if not os.path.exists(elf):
            continue
        rc, out, err, t = run([os.path.join(b, "hvm_bench"), elf, "5" if quick else "30"])
        m = re.search(r"= ([\d.]+) MIPS", out)
        if rc or not m:
            problem("bench", e, "fail", (out + err)[-1000:])
            record("bench", f"mips/{e}", False, t)
            continue
        mips[e] = float(m.group(1))
        record("bench", f"mips/{e}", True, t, mips=mips[e])
    speed["interpreterMIPS"] = mips

    # 2. CoreMark as a real 32-bit iOS app through the whole runtime
    out = os.path.join(b, "ios")
    app = os.path.join(out, "CoreMark.app")
    if not prebuilt:
        ld = os.environ.get("HVM_LD64", "/opt/cctools/bin/arm-apple-darwin11-ld")
        if not os.path.exists(ld):
            record("bench", "coremark", None)
            problem("bench", "coremark", "skip", "ld64 not found; can't build CoreMark")
            return
        cm = ensure_coremark()
        srcs = [os.path.join(cm, f) for f in ["core_list_join.c", "core_main.c", "core_matrix.c", "core_state.c", "core_util.c"]]
        srcs.append(os.path.join(cm, "posix", "core_portme.c"))
        flags = ["-O2", f"-I{cm}", f"-I{cm}/posix", "-DPERFORMANCE_RUN=1", '-DFLAGS_STR="-O2 Thumb-2 (iOS armv7)"']
        rc, o, e, _ = run(["python3", os.path.join(ROOT, "tools", "ios_build.py"), "app", "--name", "CoreMark", "--out", out]
                          + [f"--cflags={f}" for f in flags] + srcs, timeout=900)
        if rc:
            problem("bench", "coremark", "fail", "build failed:\n" + (o + e)[-3000:])
            record("bench", "coremark", False)
            return
    if not os.path.isdir(app):
        record("bench", "coremark", None)
        problem("bench", "coremark", "skip", "CoreMark.app not built")
        return
    iters = "3000" if quick else "0"   # 0 = CoreMark's own calibration (>= 10 s, a valid run)
    rc, sout, serr, t, data = run_app(b, app, [], timeout=900, extra=["--", "0x0", "0x0", "0x66", iters])
    m = re.search(r"Iterations/Sec\s*:\s*([\d.]+)", sout)
    # Short runs (< 10 s) make CoreMark report "Errors detected" for timing alone, so
    # check the known checksums of the standard 2K performance run instead.
    known = {"crclist": "0xe714", "crcmatrix": "0x1fd7", "crcstate": "0x8e3a"}
    crcs = dict(re.findall(r"\[0\](crc\w+)\s*:\s*(0x[0-9a-f]+)", sout))
    valid = "Correct operation validated" in sout or all(crcs.get(k) == v for k, v in known.items())
    app_problems("bench", "coremark", data, serr)
    if rc or not m or not valid:
        problem("bench", "coremark", "fail", f"exit {rc}, validated={valid}\n" + sout[-2000:] + serr[-1000:])
        record("bench", "coremark", False, t)
        return
    score = float(m.group(1))
    rows = []
    for tg in targets:
        ref = tg["mhz"] * tg["coremarkPerMHz"]
        rows.append({"device": tg["device"], "cpu": tg["cpu"], "reference": round(ref), "percent": round(100 * score / ref, 1),
                     "effectiveMHz": round(score / tg["coremarkPerMHz"]), "goal": bool(tg.get("goal"))})
    goal = next((r for r in rows if r["goal"]), rows[-1])
    speed["coremark"] = {"score": round(score, 1), "guestMIPS": round(data.get("perf", {}).get("guestMIPS", 0), 1),
                         "quick": quick, "vsDevices": rows, "goalDevice": goal["device"], "goalPercent": goal["percent"]}
    record("bench", "coremark", True, t, score=round(score, 1))
    if goal["percent"] < 100:
        problem("bench", "coremark", "perf", f"{goal['percent']}% of {goal['device']} single-core speed (goal: 100%)")


# ----------------------------------------------------------------- output ---
SEV_ORDER = ["fail", "crash", "error", "missing", "perf", "warning", "skip"]
SEV_LABEL = {"fail": "Failures", "crash": "Crashes", "error": "Runtime errors", "missing": "Missing functions",
             "perf": "Below speed goal", "warning": "Warnings", "skip": "Skipped"}


def markdown(info):
    L = [f"# HydrogenVM report — {info['commit']}", "",
         f"{info['date']} · {info['cpu']} ({info['arch']}, {info['os']})", ""]
    total_f = sum(s["failed"] for s in suites.values())
    L.append(("**All tests passed.**" if total_f == 0 else f"**{total_f} test(s) failed.**") + "")
    L += ["", "| Suite | Passed | Failed | Skipped | Time |", "|---|---:|---:|---:|---:|"]
    for n, s in suites.items():
        L.append(f"| {n} | {s['passed']} | {s['failed']} | {s['skipped']} | {s['seconds']:.1f}s |")
    if speed:
        L += ["", "## Speed", ""]
        cm = speed.get("coremark")
        if cm:
            L.append(f"CoreMark **{cm['score']}** iterations/s ({cm['guestMIPS']} guest MIPS)"
                     + (" — quick run, not a full-length CoreMark" if cm["quick"] else "") + "")
            L += ["", "| Original device | CPU | Device CoreMark (est.) | Emulator | Effective clock |", "|---|---|---:|---:|---:|"]
            for r in cm["vsDevices"]:
                tag = " ← goal" if r["goal"] else ""
                L.append(f"| {r['device']}{tag} | {r['cpu']} | {r['reference']} | **{r['percent']}%** | {r['effectiveMHz']} MHz |")
        if speed.get("interpreterMIPS"):
            L += ["", "Raw interpreter speed (CPU test programs): " +
                  ", ".join(f"{k} {v:.0f} MIPS" for k, v in speed["interpreterMIPS"].items())]
        apps = [t for t in suites.get("ios", {}).get("tests", []) if t.get("perf")]
        if apps:
            L += ["", "| App | Wall | Frames | Avg frame | Max frame | Slow frames |", "|---|---:|---:|---:|---:|---:|"]
            for t in apps:
                p = t["perf"]
                L.append(f"| {t['name']} | {p.get('wallSeconds', 0):.2f}s | {p.get('frames', 0)} | {p.get('avgFrameMs', 0):.2f} ms | "
                         f"{p.get('maxFrameMs', 0):.2f} ms | {p.get('slowFrames', 0)} |")
    L += ["", f"## Problems ({len(problems)})", ""]
    if not problems:
        L.append("None.")
    for sev in SEV_ORDER:
        items = [p for p in problems if p["severity"] == sev]
        if not items:
            continue
        L += [f"### {SEV_LABEL[sev]} ({len(items)})", ""]
        for p in items:
            msg = p["message"]
            if "\n" in msg:
                L += [f"- **{p['suite']} / {p['test']}**", "", "  ```", *["  " + l for l in msg.splitlines()[:60]], "  ```"]
            else:
                L.append(f"- **{p['suite']} / {p['test']}**: {msg}")
        L.append("")
    return "\n".join(L) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", default=os.path.join(ROOT, "build"))
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "report"))
    ap.add_argument("--skip-build", action="store_true")
    ap.add_argument("--prebuilt-apps", action="store_true", help="run the apps already in <build>/ios instead of building them")
    ap.add_argument("--quick", action="store_true", help="shorter benchmarks")
    ap.add_argument("--only", default="cpu,ios,bench")
    ap.add_argument("--history", help="append a CSV line of speed results to this file")
    a = ap.parse_args()
    b = os.path.abspath(a.build_dir)
    only = set(a.only.split(","))
    info = host_info()
    t0 = time.time()

    built = True if a.skip_build else do_build(b)
    if built:
        if "cpu" in only: do_cpu(b)
        if "ios" in only: do_ios(b, a.prebuilt_apps)
        if "bench" in only: do_bench(b, a.quick, a.prebuilt_apps)

    os.makedirs(a.out, exist_ok=True)
    failed = any(s["failed"] for s in suites.values())
    report = {"host": info, "seconds": round(time.time() - t0, 1), "ok": not failed, "suites": suites, "speed": speed,
              "problems": problems}
    json.dump(report, open(os.path.join(a.out, "report.json"), "w"), indent=2)
    md = markdown(info)
    open(os.path.join(a.out, "report.md"), "w").write(md)
    if os.environ.get("GITHUB_ACTIONS"):
        # Annotations are readable through the API even when job logs aren't.
        for p in problems:
            if p["severity"] in ("fail", "crash", "error", "missing", "perf"):
                kind = "error" if p["severity"] in ("fail", "crash") else "warning"
                msg = p["message"].replace("%", "%25").replace("\r", "").replace("\n", "%0A")[:4000]
                print(f"::{kind} title={p['suite']}/{p['test']} ({p['severity']})::{msg}")
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as f:
            f.write(md)
    if a.history and speed.get("coremark"):
        new = not os.path.exists(a.history)
        os.makedirs(os.path.dirname(os.path.abspath(a.history)), exist_ok=True)
        with open(a.history, "a") as f:
            if new:
                f.write("date,commit,host,coremark,goal_percent,thumb_mips,arm_mips,quick\n")
            cm, mp = speed["coremark"], speed.get("interpreterMIPS", {})
            f.write(f"{info['date']},{info['commit']},\"{info['cpu']}\",{cm['score']},{cm['goalPercent']},"
                    f"{mp.get('ctests_thumb', '')},{mp.get('ctests_arm', '')},{int(cm['quick'])}\n")
    # Console summary
    for n, s in suites.items():
        print(f"{n:6} {s['passed']:3} passed {s['failed']:3} failed {s['skipped']:3} skipped  ({s['seconds']:.1f}s)")
    if speed.get("coremark"):
        cm = speed["coremark"]
        print(f"CoreMark {cm['score']}  =  {cm['goalPercent']}% of {cm['goalDevice']}")
    counts = {}
    for p in problems:
        counts[p["severity"]] = counts.get(p["severity"], 0) + 1
    print("problems: " + (", ".join(f"{v} {k}" for k, v in counts.items()) or "none"))
    print(f"report: {os.path.join(a.out, 'report.md')}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
