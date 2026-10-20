#!/usr/bin/env python3
"""HydrogenVM compatibility scanner.

Finds every issue each app runs into, in one pass per app, and ranks fixes by
how many apps they unblock — so one fix is applied, and verified, across every
app that shares the problem.

For each .ipa / .app:
  1. Unpack the IPA, read Info.plist.
  2. Static checks on the executable: architectures, FairPlay encryption.
  3. Strict run (the real behaviour): the first thing that stops or crashes the
     app, and a screenshot.
  4. Keep-going run (missing functions return 0, missing methods return nil):
     every runtime miss the app reaches in the time limit.
  5. Static gaps: imports bound to "missing" stubs that weren't reached yet.

Then across all apps:
  * blockers ranked by how many apps they stop,
  * runtime misses ranked by how many apps hit them,
  * static-only gaps ranked by how many apps import them,
  * per-app status, compared with the previous scan (regressions flagged).

usage:
  tools/compat.py scan <ipa|app|dir>... [--out compat] [--seconds 30] [--hvm build/hvm] [--jobs 2]
  tools/compat.py report [--out compat]          # rebuild REPORT.md from results.json

Outputs <out>/results.json, <out>/REPORT.md, <out>/shots/<app>.png.
"""
import argparse, concurrent.futures, datetime, json, os, plistlib, re, shutil, struct, subprocess, sys, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ------------------------------------------------------------------ Mach-O ---

CPU_ARM, CPU_ARM64 = 12, 0x0100000C
ARM_SUB = {5: "armv4t", 6: "armv6", 7: "armv5", 9: "armv7", 10: "armv7f", 11: "armv7s", 12: "armv7k", 13: "armv8", 14: "armv6m", 15: "armv7m", 16: "armv7em"}

def macho_info(path):
    """{'archs': [...], 'encrypted': bool, 'error': str|None} for the 32-bit ARM slice HydrogenVM would run."""
    try:
        data = open(path, "rb").read()
    except OSError as e:
        return {"archs": [], "encrypted": False, "error": str(e)}
    slices = []
    magic = struct.unpack(">I", data[:4])[0]
    if magic in (0xCAFEBABE, 0xBEBAFECA):
        n = struct.unpack(">I", data[4:8])[0]
        for i in range(n):
            cpu, sub, off, size, _ = struct.unpack(">5I", data[8 + 20 * i:28 + 20 * i])
            slices.append((cpu, sub & 0xFFFFFF, off, size))
    else:
        cpu, sub = struct.unpack("<II", data[4:12])
        slices.append((cpu, sub & 0xFFFFFF, 0, len(data)))
    archs = []
    for cpu, sub, _, _ in slices:
        archs.append("arm64" if cpu == CPU_ARM64 else ARM_SUB.get(sub, "arm?%d" % sub) if cpu == CPU_ARM else "cpu%d" % cpu)
    arm32 = [s for s in slices if s[0] == CPU_ARM]
    if not arm32:
        return {"archs": archs, "encrypted": False, "error": "no 32-bit ARM slice (64-bit only)"}
    # Prefer armv7 (what HydrogenVM runs), else the first 32-bit slice.
    cpu, sub, off, size = next((s for s in arm32 if s[1] in (9, 11)), arm32[0])
    d = data[off:off + size]
    ncmds = struct.unpack("<I", d[16:20])[0]
    p, encrypted = 28, False
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack("<II", d[p:p + 8])
        if cmd == 0x21:   # LC_ENCRYPTION_INFO
            encrypted = struct.unpack("<I", d[p + 16:p + 20])[0] != 0
        p += cmdsize
    gl = "GLES2 (shaders)" if b"_glCreateShader" in d else "GLES1 (fixed-function)" if b"_glMatrixMode" in d or b"_glVertexPointer" in d else "none"
    engine = "Unity" if b"UnityAppController" in d or b"mono_" in d or b"UnityGetGLViewController" in d else ""
    return {"archs": archs, "encrypted": encrypted, "error": None, "gl": gl, "engine": engine}

# ------------------------------------------------------------------- apps ---

def find_apps(inputs, cache):
    """Expand inputs into (name, .app path) pairs, unpacking IPAs into the cache."""
    found = []
    def add_app(app):
        found.append((os.path.basename(app)[:-4], os.path.abspath(app)))
    for item in inputs:
        item = os.path.abspath(item)
        if item.endswith(".app") and os.path.isdir(item):
            add_app(item)
        elif item.lower().endswith(".ipa") and os.path.isfile(item):
            key = re.sub(r"[^A-Za-z0-9._-]", "_", os.path.basename(item)[:-4])
            dest = os.path.join(cache, key)
            if not os.path.isdir(dest):
                os.makedirs(dest, exist_ok=True)
                try:
                    with zipfile.ZipFile(item) as z:
                        z.extractall(dest)
                except zipfile.BadZipFile as e:
                    found.append((key, None, "bad IPA: %s" % e))
                    continue
            for a in sorted(os.listdir(os.path.join(dest, "Payload"))) if os.path.isdir(os.path.join(dest, "Payload")) else []:
                if a.endswith(".app"):
                    add_app(os.path.join(dest, "Payload", a))
        elif os.path.isdir(item):
            sub = []
            for dirpath, dirnames, filenames in os.walk(item):
                for d in list(dirnames):
                    if d.endswith(".app"):
                        sub.append(os.path.join(dirpath, d))
                        dirnames.remove(d)
                for f in filenames:
                    if f.lower().endswith(".ipa"):
                        sub.append(os.path.join(dirpath, f))
            found.extend(x for x in find_apps(sub, cache))
    # de-duplicate by path
    seen, out = set(), []
    for e in found:
        k = e[1] or e[0]
        if k not in seen:
            seen.add(k)
            out.append(e)
    return out

def app_meta(app):
    try:
        with open(os.path.join(app, "Info.plist"), "rb") as f:
            info = plistlib.load(f)
    except Exception:
        info = {}
    exe = info.get("CFBundleExecutable") or os.path.basename(app)[:-4]
    return {
        "bundleId": info.get("CFBundleIdentifier", ""),
        "displayName": info.get("CFBundleDisplayName") or info.get("CFBundleName") or exe,
        "version": str(info.get("CFBundleShortVersionString") or info.get("CFBundleVersion") or ""),
        "minOS": str(info.get("MinimumOSVersion", "")),
        "executable": exe,
    }

# ------------------------------------------------------------------- runs ---

def run_hvm(hvm, app, home, seconds, report, keep_going, shot=None):
    cmd = [hvm, "run", app, "--home", home, "--run-for", str(seconds), "--report", report, "--quiet"]
    if keep_going:
        cmd.append("--keep-going")
    if shot:
        cmd += ["--screenshot", shot]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, errors="replace", timeout=seconds * 3 + 60)
        out = p.stdout + p.stderr
    except subprocess.TimeoutExpired:
        out = "[compat] hvm hung past the time limit"
    try:
        with open(report) as f:
            rep = json.load(f)
    except Exception:
        rep = {"loadError": "no report (hvm exited abnormally)", "errors": [], "warnings": [], "missingFunctions": [],
               "unimplementedImports": [], "crashed": False, "crashReport": "", "perf": {}}
    rep["_output"] = out[-20000:]
    return rep

RE_STOPPED = re.compile(r"Stopped: (\S+): (.*)")
RE_MISSING_METHOD = re.compile(r"Missing method ([+-]\[[^\]]+\])")
RE_MISSING_FN = re.compile(r"Missing function: (.+?) \(called from")
RE_CRASH = re.compile(r"Crash: (.*)")

def blocker_of(rep):
    """The one thing that ended (or would end) the strict run, or None if it kept running."""
    if rep.get("loadError"):
        return {"kind": "load", "name": rep["loadError"][:200], "detail": rep["loadError"]}
    if rep.get("stillRunning"):
        return None
    msgs = [e["message"] for e in rep.get("errors", [])]
    crash = rep.get("crashReport", "")
    for m in msgs:
        if m.startswith("Stopped: _UIApplicationMain: stopping") or "stopped by --run-for" in m:
            continue
        mm = RE_MISSING_METHOD.search(m)
        if mm:
            return {"kind": "method", "name": mm.group(1), "detail": m}
        mf = RE_MISSING_FN.search(m)
        if mf:
            return {"kind": "function", "name": mf.group(1), "detail": m}
        mc = RE_CRASH.search(m)
        if mc:
            return {"kind": "crash", "name": crash_signature(mc.group(1), crash), "detail": (m + "\n" + crash)[:4000]}
        ms = RE_STOPPED.search(m)
        if ms:
            reason = ms.group(2)
            # Normalise addresses and object ids so the same failure groups across apps.
            norm = re.sub(r"0x[0-9a-fA-F]+", "0x…", reason)
            return {"kind": "exception", "name": norm[:200], "detail": m}
    if rep.get("crashed"):
        return {"kind": "crash", "name": crash_signature("crash", crash), "detail": crash[:4000]}
    return None

def crash_signature(what, report):
    """'bad memory access … in <symbol>' — stable across runs and apps where possible."""
    pc = re.search(r"pc (\[HydrogenVM\] \S+|\S+ \S+)", report)
    where = pc.group(1) if pc else "?"
    if where.startswith("0x"):
        where = "app code"
    what = re.sub(r"0x[0-9a-fA-F]+", "0x…", what)
    return "%s @ %s" % (what, where)

def classify_runtime(rep):
    """Runtime misses (reached while running): [(kind, name)]. Feature-unavailable notes are kind 'note'."""
    out = []
    for name in rep.get("missingFunctions", []):
        note = re.search(r" \(([^)]*(disabled|offline|unavailable|not supported)[^)]*)\)$", name)
        n = re.sub(r" \([^)]*\)$", "", name)
        if note:
            out.append(("note", "%s (%s)" % (n, note.group(1))))
            continue
        out.append(("method" if n.startswith(("-[", "+[")) else "function", n))
    return out

def classify_load(rep):
    """Load-time gaps: placeholder classes and zero-filled data symbols the app binds to."""
    out = []
    for w in rep.get("warnings", []):
        m = w["message"]
        mc = re.match(r"Missing class (\S+)", m)
        if mc:
            out.append(("class", mc.group(1)))
        md = re.match(r"Missing data symbol (.+?) \(using zeros\)", m)
        if md:
            out.append(("data", md.group(1)))
    return out

FRAMEWORKS = [
    (r"^std::|^__cxa|^__gxx|^operator (new|delete)", "C++ runtime"),
    (r"^gl[A-Z]|^EAGL", "OpenGL ES"), (r"^al[A-Z]|^alc", "OpenAL"),
    (r"^Audio|^ExtAudio|^AU[A-Z]|^AV", "Audio / AVFoundation"),
    (r"^CF|^kCF", "CoreFoundation / CFNetwork"), (r"^CG|^kCG", "CoreGraphics"),
    (r"^CT|^kCT", "CoreText / CoreTelephony"), (r"^Sec|^kSec", "Security"), (r"^SC|^CN", "SystemConfiguration"),
    (r"^sqlite3", "SQLite"), (r"^(inflate|deflate|crc32|adler32|gz|z[A-Z]|uncompress|compress)", "zlib"),
    (r"^CC_|^CCH|^CCC", "CommonCrypto"), (r"^dispatch_", "libdispatch"), (r"^pthread_|^thread_|^task_|^mach_|^semaphore_", "threads / Mach"),
    (r"^objc_|^class_|^sel_|^method_|^object_|^ivar_|^protocol_|^property_", "ObjC runtime"),
    (r"^[+-]\[(UI|CA)", "UIKit / QuartzCore"), (r"^[+-]\[(NS|__NS)", "Foundation"),
    (r"^[+-]\[(GK|SK|MP|CL|CM|AD|AV)", "Game Center / StoreKit / Media / Location / Motion / iAd"),
    (r"^[+-]\[", "app / third-party SDK class"), (r"^(UI|CA)", "UIKit / QuartzCore"), (r"^NS", "Foundation"),
]
PREFIXES = [("UI", "UIKit"), ("CA", "QuartzCore"), ("NS", "Foundation"), ("__NS", "Foundation"), ("CF", "CoreFoundation"),
            ("kCF", "CoreFoundation"), ("CL", "CoreLocation"), ("MP", "MediaPlayer"), ("EK", "EventKit"), ("MK", "MapKit"),
            ("SK", "StoreKit"), ("GK", "GameKit"), ("AB", "AddressBook"), ("UT", "MobileCoreServices"), ("AV", "AVFoundation"),
            ("AD", "iAd"), ("CM", "CoreMotion"), ("CT", "CoreTelephony"), ("kCT", "CoreTelephony"), ("CN", "SystemConfiguration"),
            ("kCN", "SystemConfiguration"), ("SC", "SystemConfiguration"), ("Sec", "Security"), ("kSec", "Security"),
            ("MF", "MessageUI"), ("AL", "AssetsLibrary"), ("TW", "Twitter"), ("SL", "Social"), ("CI", "CoreImage"),
            ("CV", "CoreVideo"), ("kCV", "CoreVideo"), ("CG", "CoreGraphics"), ("kCG", "CoreGraphics"), ("asl_", "libsystem (asl)")]
def framework_of(name):
    m = re.match(r"^[+-]\[(\S+)", name)
    base = m.group(1) if m else name
    for rx, fw in FRAMEWORKS[:16]:   # C-level families (C++, GL, AL, audio, sqlite, zlib, crypto, threads, objc)
        if re.search(rx, base):
            return fw
    for pre, fw in sorted(PREFIXES, key=lambda p: -len(p[0])):
        if base.startswith(pre) and (len(base) == len(pre) or not base[len(pre)].islower() or pre.endswith("_")):
            return fw
    if m:
        return "app / third-party SDK class"
    return "libc / POSIX" if re.match(r"^[a-z_]", base) else "other"

def frame_check(path):
    """Flags last frames that look like a rendering failure: blank, or one colour covering most of the screen."""
    try:
        from PIL import Image
    except ImportError:
        return None
    try:
        im = Image.open(path).convert("RGB")
    except Exception as e:
        return {"error": str(e)}
    small = im.resize((80, 120))
    px = list(small.getdata())
    quant = {}
    for r, g, b in px:
        k = (r >> 4, g >> 4, b >> 4)
        quant[k] = quant.get(k, 0) + 1
    top, n = max(quant.items(), key=lambda kv: kv[1])
    share = n / len(px)
    colors = sum(1 for v in quant.values() if v > len(px) * 0.002)
    out = {"dominantShare": round(share, 3), "colors": colors, "dominant": "#%02x%02x%02x" % tuple(c * 17 for c in top)}
    if share > 0.97:
        out["suspicious"] = "last frame is a single colour (%s) — nothing drawn, or drawn off-screen" % out["dominant"]
    elif share > 0.80:
        out["suspicious"] = "last frame is %d%% one colour (%s) — background or layers missing?" % (round(share * 100), out["dominant"])
    return out

def scan_app(entry, args):
    name, app = entry[0], entry[1]
    res = {"name": name, "app": app, "scannedAt": datetime.datetime.now().isoformat(timespec="seconds")}
    if app is None:
        res.update(status="bad-ipa", blocker={"kind": "load", "name": entry[2], "detail": entry[2]}, issues=[])
        return res
    res.update(app_meta(app))
    exe = os.path.join(app, res["executable"])
    mi = macho_info(exe)
    res["archs"] = mi["archs"]
    res["encrypted"] = mi["encrypted"]
    res["gl"] = mi.get("gl", "")
    res["engine"] = mi.get("engine", "")
    if mi["error"]:
        res.update(status="unsupported", blocker={"kind": "load", "name": mi["error"], "detail": mi["error"]}, issues=[])
        return res
    if mi["encrypted"]:
        msg = "FairPlay-encrypted executable — needs a decrypted copy of the app"
        res.update(status="encrypted", blocker={"kind": "load", "name": msg, "detail": msg}, issues=[])
        return res
    work = os.path.join(args.out, "runs", name)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(os.path.join(work, "home-strict"), exist_ok=True)
    os.makedirs(os.path.join(work, "home-kg"), exist_ok=True)
    shot = os.path.join(args.out, "shots", name + ".png")
    if os.path.exists(shot):
        os.remove(shot)
    strict = run_hvm(args.hvm, app, os.path.join(work, "home-strict"), args.seconds, os.path.join(work, "strict.json"), False, shot)
    kg = run_hvm(args.hvm, app, os.path.join(work, "home-kg"), args.seconds, os.path.join(work, "keepgoing.json"), True)
    blocker = blocker_of(strict)
    perf = strict.get("perf", {})
    frames = perf.get("framesPresented", 0)
    if blocker is None:
        status = "runs" if frames else "runs-no-frames"
    else:
        status = {"load": "load-error", "crash": "crashes"}.get(blocker["kind"], "stops")
    issues, seen = [], set()
    def add(kind, n, runtime):
        key = (kind, n)
        if key in seen:
            return
        seen.add(key)
        issues.append({"kind": kind, "name": n, "framework": framework_of(n), "runtime": runtime})
    for kind, n in classify_runtime(strict) + classify_runtime(kg):
        add(kind, n, True)
    for kind, n in classify_load(strict) + classify_load(kg):
        add(kind, n, False)
    for n in kg.get("unimplementedImports", []) or strict.get("unimplementedImports", []):
        add("function", n, False)
    kgBlocker = blocker_of(kg)
    visual = frame_check(shot) if os.path.exists(shot) else None
    if visual and visual.get("suspicious"):
        add("render", visual["suspicious"], True)
    res.update(status=status, blocker=blocker, keepGoingBlocker=kgBlocker, issues=issues,
               perf={"framesPresented": frames, "fps": round(frames / max(1.0, perf.get("wallSeconds", 1)), 1),
                     "guestMIPS": round(perf.get("guestMIPS", 0), 1), "avgFrameMs": round(perf.get("avgFrameMs", 0), 1)},
               screenshot=os.path.relpath(shot, args.out) if os.path.exists(shot) else None, visual=visual)
    return res

# ----------------------------------------------------------------- report ---

STATUS_ORDER = {"runs": 0, "runs-no-frames": 1, "stops": 2, "crashes": 3, "load-error": 4, "encrypted": 5, "unsupported": 6, "bad-ipa": 7}
STATUS_LABEL = {"runs": "✅ runs", "runs-no-frames": "🟡 runs, no frames yet", "stops": "🛑 stops", "crashes": "💥 crashes",
                "load-error": "⛔ load error", "encrypted": "🔒 encrypted", "unsupported": "⛔ unsupported", "bad-ipa": "⛔ bad IPA"}

def aggregate(apps):
    blockers, runtime, static = {}, {}, {}
    for a in apps:
        b = a.get("blocker")
        if b and a["status"] in ("stops", "crashes"):
            blockers.setdefault((b["kind"], b["name"]), []).append(a["name"])
        for i in a.get("issues", []):
            (runtime if i["runtime"] else static).setdefault((i["kind"], i["name"], i["framework"]), []).append(a["name"])
    # an issue hit at runtime in any app isn't "static-only"
    rt_keys = {(k, n) for (k, n, _) in runtime}
    static = {k: v for k, v in static.items() if (k[0], k[1]) not in rt_keys}
    rank = lambda d: sorted(d.items(), key=lambda kv: (-len(kv[1]), kv[0]))
    return rank(blockers), rank(runtime), rank(static)

def write_report(out, results, previous):
    apps = sorted(results["apps"], key=lambda a: (STATUS_ORDER.get(a["status"], 9), a["name"].lower()))
    prev = {a["name"]: a for a in (previous or {}).get("apps", [])}
    blockers, runtime, static = aggregate(apps)
    L = []
    L.append("# HydrogenVM compatibility report")
    L.append("")
    L.append("Scanned %s · %d apps · %ss per run · build %s" % (results["scannedAt"], len(apps), results["seconds"], results.get("build", "?")))
    L.append("")
    counts = {}
    for a in apps:
        counts[a["status"]] = counts.get(a["status"], 0) + 1
    L.append(" · ".join("%s: %d" % (STATUS_LABEL[s], counts[s]) for s in sorted(counts, key=lambda s: STATUS_ORDER.get(s, 9))))
    L.append("")
    changes = []
    for a in apps:
        p = prev.get(a["name"])
        if p and p["status"] != a["status"]:
            better = STATUS_ORDER.get(a["status"], 9) < STATUS_ORDER.get(p["status"], 9)
            changes.append("- %s **%s**: %s → %s" % ("⬆️" if better else "⚠️ REGRESSION", a["name"], STATUS_LABEL.get(p["status"], p["status"]), STATUS_LABEL.get(a["status"], a["status"])))
        elif p and p.get("blocker") and a.get("blocker") and p["blocker"]["name"] != a["blocker"]["name"] and a["status"] in ("stops", "crashes"):
            changes.append("- ➡️ **%s** now stops later: `%s` (was `%s`)" % (a["name"], a["blocker"]["name"], p["blocker"]["name"]))
    if changes:
        L.append("## Changes since the last scan")
        L.append("")
        L.extend(changes)
        L.append("")
    L.append("## Apps")
    L.append("")
    L.append("| App | Version | Graphics | Status | Frames (fps) | First blocker | Runtime misses |")
    L.append("|---|---|---|---|---|---|---|")
    for a in apps:
        b = a.get("blocker")
        bt = ("`%s`" % b["name"].replace("|", "\\|")) if b else "—"
        n_rt = sum(1 for i in a.get("issues", []) if i["runtime"] and i["kind"] not in ("note",))
        perf = a.get("perf", {})
        fr = "%d (%s)" % (perf.get("framesPresented", 0), perf.get("fps", 0)) if perf else "—"
        gfx = (a.get("gl", "") + (" · " + a["engine"] if a.get("engine") else "")).strip(" ·")
        L.append("| %s | %s | %s | %s | %s | %s | %d |" % (a.get("displayName", a["name"]), a.get("version", ""), gfx, STATUS_LABEL.get(a["status"], a["status"]), fr, bt, n_rt))
    L.append("")
    renders = [(a, i) for a in apps for i in a.get("issues", []) if i["kind"] == "render"]
    if renders:
        L.append("## Rendering looks wrong (last-frame check)")
        L.append("")
        for a, i in renders:
            L.append("- **%s** (%s): %s — see `%s`" % (a.get("displayName", a["name"]), a.get("gl", "?"), i["name"], a.get("screenshot") or "no screenshot"))
        L.append("")
    L.append("## Fix next: blockers (each fix unblocks every app listed)")
    L.append("")
    if blockers:
        for (kind, name), who in blockers:
            L.append("- **%d app%s** · %s `%s` — %s" % (len(who), "s" if len(who) > 1 else "", kind, name, ", ".join(who)))
    else:
        L.append("None — every app that loads keeps running.")
    L.append("")
    notes = [(k, w) for k, w in runtime if k[0] == "note"]
    runtime = [(k, w) for k, w in runtime if k[0] != "note"]
    L.append("## Hit at runtime (with keep-going), by apps affected")
    L.append("")
    by_fw = {}
    for (kind, name, fw), who in runtime:
        by_fw.setdefault(fw, []).append((kind, name, who))
    for fw in sorted(by_fw, key=lambda f: -sum(len(w) for _, _, w in by_fw[f])):
        L.append("### %s" % fw)
        for kind, name, who in by_fw[fw]:
            L.append("- %d · %s `%s` — %s" % (len(who), kind, name, ", ".join(who)))
        L.append("")
    if notes:
        L.append("## Features reported unavailable (by design or not yet)")
        L.append("")
        for (kind, name, fw), who in notes:
            L.append("- %d · %s — %s" % (len(who), name, ", ".join(who)))
        L.append("")
    L.append("## Load-time and static gaps (bound at load or imported, not yet reached), by apps")
    L.append("")
    top = [s for s in static if len(s[1]) > 1] + [s for s in static if len(s[1]) == 1][:60]
    for (kind, name, fw), who in top:
        L.append("- %d · %s `%s` (%s) — %s" % (len(who), kind, name, fw, ", ".join(who)))
    hidden = len(static) - len(top)
    if hidden > 0:
        L.append("- … and %d more single-app gaps (see results.json)" % hidden)
    L.append("")
    with open(os.path.join(out, "REPORT.md"), "w") as f:
        f.write("\n".join(L) + "\n")

def build_id():
    try:
        return subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip()
    except Exception:
        return "?"

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("scan")
    s.add_argument("inputs", nargs="+")
    s.add_argument("--out", default="compat")
    s.add_argument("--seconds", type=int, default=30)
    s.add_argument("--hvm", default=os.path.join(ROOT, "build", "hvm"))
    s.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    s.add_argument("--only", help="comma-separated app names to rescan (others keep their previous results)")
    r = sub.add_parser("report")
    r.add_argument("--out", default="compat")
    args = ap.parse_args()
    args.out = os.path.abspath(args.out)
    os.makedirs(os.path.join(args.out, "shots"), exist_ok=True)
    results_path = os.path.join(args.out, "results.json")
    previous = None
    if os.path.exists(results_path):
        with open(results_path) as f:
            previous = json.load(f)
    if args.cmd == "report":
        write_report(args.out, previous, None)
        print(os.path.join(args.out, "REPORT.md"))
        return
    entries = find_apps(args.inputs, os.path.join(args.out, "ipa-cache"))
    if args.only:
        want = set(args.only.split(","))
        entries = [e for e in entries if e[0] in want]
    print("scanning %d app(s), %ds per run, %d at a time" % (len(entries), args.seconds, args.jobs), flush=True)
    done = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(scan_app, e, args): e for e in entries}
        for fu in concurrent.futures.as_completed(futs):
            a = fu.result()
            done.append(a)
            b = a.get("blocker")
            print("  %-28s %-22s %s" % (a["name"][:28], STATUS_LABEL.get(a["status"], a["status"]), b["name"][:90] if b else ""), flush=True)
    if args.only and previous:
        keep = [a for a in previous["apps"] if a["name"] not in {d["name"] for d in done}]
        done = keep + done
    results = {"scannedAt": datetime.datetime.now().isoformat(timespec="seconds"), "seconds": args.seconds, "build": build_id(), "apps": done}
    if previous:
        with open(os.path.join(args.out, "results.previous.json"), "w") as f:
            json.dump(previous, f, indent=1)
    with open(results_path, "w") as f:
        json.dump(results, f, indent=1)
    write_report(args.out, results, previous)
    print(os.path.join(args.out, "REPORT.md"))

if __name__ == "__main__":
    main()
