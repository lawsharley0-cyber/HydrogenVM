#!/usr/bin/env python3
"""Missing-API ledger: which unimplemented calls block the most games.

Reads what HydrogenVM already writes and ranks each missing function, class,
data symbol and selector by how many distinct apps hit it:

  * `hvm run --report` JSON (missingApis, unimplementedImports, crash signature)
  * the phone's "Test All Games" report.json (each app's log text is inside)
  * play logs and exported logs (*.log, *.txt), and exported .zip bundles

A *called* API is one an app actually reached at run time ("Missing: ...",
"unrecognized selector"). That is what stops a game. An *imported* API is one
an app links against but may never call ("N imported functions aren't
implemented yet"); it ranks below any called one.

With a support manifest (`hvm --dump-support`, or `--hvm build/hvm` to make
one) each row also says whether the current build implements it now, so old
phone logs still tell you what is left: rows marked "now Real" are fixed.

  python3 tools/missing_ledger.py scan ~/exports bench-reports/ --hvm build/hvm
  python3 tools/missing_ledger.py run --hvm build/hvm --apps ~/bench/apps --out ledger-reports
  python3 tools/missing_ledger.py diff old-ledger.json new-ledger.json

Standard library only. Output is deterministic for the same inputs.
"""
import argparse
import concurrent.futures
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import zipfile

LEDGER_VERSION = 1

# ------------------------------------------------------------ classification

AREAS = [
    ("libc++/libstdc++", [r"^std::", r"__cxa", r"^typeinfo for", r"^vtable for", r"^VTT for", r"operator (new|delete)"]),
    ("OpenGL ES", [r"^gl[A-Z]", r"^EAGL", r"^GLK", r"OpenGLES"]),
    ("Audio", [r"^Audio", r"^AU[A-Z]", r"^al[A-Z]", r"^alc", r"^AV(Audio|Player)", r"^MP(Music|Volume|RemoteCommand|NowPlaying)", r"^ExtAudio"]),
    ("Network", [r"^SC(Network|Dynamic)", r"^CFSocket", r"^CFNetwork", r"^CFHTTP", r"^getaddrinfo", r"socket", r"^NSURL", r"^SecTrust", r"^SSL"]),
    ("Store/Game services", [r"^SK[A-Z]", r"StoreKit", r"^GK[A-Z]", r"GameKit", r"^AD[A-Z]", r"iAd"]),
    ("Core Foundation", [r"^CF[A-Z]", r"^kCF"]),
    ("Foundation/UIKit/ObjC", [r"^NS[A-Z]", r"^UI[A-Z]", r"^CA[A-Z]", r"^objc_", r"^object_", r"^class_", r"^sel_", r"^[-+]\[", r"^_objc"]),
    ("Media/Graphics", [r"^CG[A-Z]", r"^CT[A-Z]", r"^CV[A-Z]", r"^CM[A-Z]", r"^AV[A-Z]", r"^MTL", r"^CI[A-Z]", r"^kCV"]),
    ("libxml/sqlite/zlib", [r"^xml", r"^sqlite3", r"^inflate", r"^deflate", r"^gz"]),
    ("libc/POSIX", [r"^[a-z_][a-z0-9_]*$"]),
]


def classify(name: str) -> str:
    for area, patterns in AREAS:
        if any(re.search(p, name) for p in patterns):
            return area
    return "other"


def norm(name: str) -> str:
    """One spelling for a name from any source: no spaces (c++filt writes '> >',
    the runtime '>>'), no leading underscore on C symbols."""
    n = re.sub(r"\s+", "", name.strip())
    if n.startswith("_") and not n.startswith("__") and re.match(r"^_[A-Za-z]", n):
        n = n[1:]
    return n


def split_top_level(text: str) -> list[str]:
    """Split "a, b(x, y), c" on the commas outside brackets."""
    out, depth, cur = [], 0, []
    for ch in text:
        if ch in "(<[":
            depth += 1
        elif ch in ")>]":
            depth = max(0, depth - 1)
        if ch == "," and depth == 0:
            out.append("".join(cur).strip())
            cur = []
            continue
        cur.append(ch)
    if "".join(cur).strip():
        out.append("".join(cur).strip())
    return out


# ------------------------------------------------------------------ parsing

RE_GUEST = re.compile(r"^guest: .*/Apps/([^/]+)/([^/]+)\.app/?\s*$", re.M)
RE_LOADED = re.compile(r"^Loaded ([^:]+): ", re.M)
RE_MISSING = re.compile(r"^(?:\[[^\]]*\]\s*)?Missing: (.+?)\s*$", re.M)
RE_IMPORTS = re.compile(r"^(?:\[[^\]]*\]\s*)?(\d+) imported functions aren't implemented yet \([^)]*\): (.*)$", re.M)
RE_CLASS = re.compile(r"^(?:\[[^\]]*\]\s*)?Missing class (\S+) \(", re.M)
RE_DATA = re.compile(r"^(?:\[[^\]]*\]\s*)?Missing data symbol (.+?) \((?:using zeros|virtual calls)", re.M)
RE_SELECTOR = re.compile(r"([-+]\[\S+ [^\]]+\]): unrecognized selector sent to")
RE_CRASH = re.compile(r"^(?:Crash|Guest fault|Stopped|ObjC exception|Uncaught exception)[^\n]*", re.M)


def new_app(app_id: str, source: str, exe: str = "") -> dict:
    return {"id": app_id, "exe": exe, "sources": [source], "called": {}, "imported": set(), "crashed": False, "crash": ""}


def bundle_like(stem: str) -> str:
    """'com.gameloft.despicableme2_gpu' -> 'com.gameloft.despicableme2'; '' for other names."""
    stem = re.sub(r"_(gpu|cpu|software|soft)$", "", stem).strip()
    return stem if re.fullmatch(r"[A-Za-z0-9-]+(\.[A-Za-z0-9_-]+)+", stem) else ""


RE_NOTE = re.compile(r"^(.*?\S)\s+\(([^()]*)\)?$")   # a trailing runtime note; the log may cut its ")"


def split_note(name: str) -> tuple[str, str]:
    """'-[X y] (returned nil)' -> ('-[X y]', 'returned nil'). Runtime notes are not part of the API."""
    m = RE_NOTE.match(name.strip())
    return (m.group(1), m.group(2)) if m else (name.strip(), "")


def classify_missing(raw: str) -> tuple[str, str]:
    """(kind, name) for one "Missing: ..." text. A stub that returned a default ('(returned nil)')
    is the API itself; any other note means HydrogenVM runs it as a described approximation
    ('StoreKit (in-app purchases are disabled)'), which is a limitation, named in full."""
    name, note = split_note(raw)
    if note and not note.startswith("retu"):
        return "limitation", raw.strip().rstrip(")") + (")" if "(" in raw else "")
    return kind_of_missing(name), name


def kind_of_missing(name: str) -> str:
    """A selector, a function, or a described limitation ('binary property list output')."""
    if re.match(r"^[-+]\[", name):
        return "selector"
    if " " in name and not re.match(r"^[\w:~<>,&*\s]+\(", name) and "::" not in name:
        return "limitation"
    return "function"


def parse_log_text(text: str, source: str, app_hint: str = "") -> dict:
    """One app's findings from a play log or a report's log text."""
    exe = m.group(1).strip() if (m := RE_LOADED.search(text)) else ""
    app_id = app_hint
    if not app_id:
        if m := RE_GUEST.search(text):
            app_id = m.group(1)
        else:
            app_id = bundle_like(Path(source.split("!")[-1]).stem) or exe or Path(source).stem
    app = new_app(app_id, source, exe)
    for m in RE_MISSING.finditer(text):
        kind, name = classify_missing(m.group(1))
        add_called(app, name, kind)
    for m in RE_SELECTOR.finditer(text):
        add_called(app, m.group(1), "selector")
    for m in RE_IMPORTS.finditer(text):
        declared, items = int(m.group(1)), split_top_level(m.group(2))
        if len(items) < declared and items:
            items = items[:-1]   # the log line was cut off: its last name is partial
        app["imported"].update(("function", i) for i in items if i)
    for m in RE_CLASS.finditer(text):
        app["imported"].add(("class", m.group(1)))
    for m in RE_DATA.finditer(text):
        app["imported"].add(("data", m.group(1)))
    if m := RE_CRASH.search(text):
        app["crashed"], app["crash"] = True, m.group(0)[:200]
    return app


def add_called(app: dict, name: str, kind: str) -> None:
    key = (kind, name.strip())
    app["called"][key] = app["called"].get(key, 0) + 1


def parse_hvm_report(data: dict, source: str) -> dict:
    bundle = data.get("bundle") or ""
    app_id = Path(bundle.rstrip("/")).stem if bundle else Path(source).stem
    app = new_app(app_id, source, app_id)
    for api in data.get("missingApis") or []:
        name = api.get("name") if isinstance(api, dict) else str(api)
        if name:
            key = classify_missing(name)[::-1][::-1]
            app["called"][key] = app["called"].get(key, 0) + int(api.get("count", 1) if isinstance(api, dict) else 1)
    for name in data.get("missingFunctions") or []:
        app["called"].setdefault(classify_missing(name), 1)
    app["imported"].update(("function", n) for n in data.get("unimplementedImports") or [])
    if data.get("crashed"):
        app["crashed"] = True
        app["crash"] = (data.get("signature") or data.get("crashReport") or "crashed").split("\n")[0][:200]
    return app


def parse_batch_report(data: dict, source: str) -> list[dict]:
    apps = []
    for entry in ((data.get("suites") or {}).get("apps") or {}).get("apps") or []:
        app_id = entry.get("bundleId") or entry.get("name") or "unknown"
        app = parse_log_text(entry.get("logs") or "", f"{source}#{app_id}", app_id)
        if entry.get("crashed"):
            app["crashed"] = True
            app["crash"] = app["crash"] or (entry.get("signature") or "crashed")[:200]
        apps.append(app)
    return apps


def parse_bytes(raw: bytes, source: str) -> list[dict]:
    if source.endswith(".zip"):
        found = []
        with zipfile.ZipFile(io.BytesIO(raw)) as z:
            for info in sorted(z.infolist(), key=lambda i: i.filename):
                if not info.is_dir() and info.filename.endswith((".json", ".log", ".txt")):
                    found += parse_bytes(z.read(info), f"{source}!{info.filename}")
        return found
    text = raw.decode("utf-8", errors="replace")
    if source.endswith(".json"):
        try:
            data = json.loads(text)
        except json.JSONDecodeError:
            return []
        if isinstance(data, dict) and "suites" in data:
            return parse_batch_report(data, source)
        if isinstance(data, dict) and ("missingApis" in data or "unimplementedImports" in data):
            return [parse_hvm_report(data, source)]
        return []
    if "imported functions aren't implemented yet" in text or "Missing: " in text or "Missing class " in text:
        return [parse_log_text(text, source)]
    return []


def collect(paths: list[str]) -> list[dict]:
    files = []
    for p in paths:
        path = Path(p)
        if path.is_dir():
            files += [f for f in sorted(path.rglob("*")) if f.is_file() and f.suffix in (".json", ".log", ".txt", ".zip")]
        elif path.is_file():
            files.append(path)
    apps = []
    for f in files:
        apps += parse_bytes(f.read_bytes(), str(f))
    return apps


def merge_apps(apps: list[dict]) -> dict:
    """Runs of the same app (several logs, report plus log) become one app."""
    # The same game appears under its bundle id (phone reports, play logs) and its executable
    # name (logs without the guest line, bench reports): fold executables into bundle ids.
    exe_to_bundle = {}
    for a in apps:
        if a["exe"] and a["id"] != a["exe"] and "." in a["id"]:
            exe_to_bundle.setdefault(a["exe"], a["id"])
    for a in apps:
        a["id"] = exe_to_bundle.get(a["id"], a["id"])
    merged = {}
    for a in apps:
        m = merged.setdefault(a["id"], new_app(a["id"], a["sources"][0], a["exe"]))
        if a["sources"][0] not in m["sources"]:
            m["sources"] += a["sources"]
        for k, v in a["called"].items():
            m["called"][k] = m["called"].get(k, 0) + v
        m["imported"] |= a["imported"]
        if a["crashed"] and not m["crashed"]:
            m["crashed"], m["crash"] = True, a["crash"]
    for m in merged.values():
        m["sources"] = sorted(set(m["sources"]))
    return merged


# ---------------------------------------------------------- support manifest

def demangle(names: list[str]) -> list[str]:
    tool = shutil.which("llvm-cxxfilt") or shutil.which("c++filt")
    if not tool or not names:
        return names
    out = subprocess.run([tool, "-_"], input="\n".join(names), capture_output=True, text=True, check=False).stdout.splitlines()
    return out if len(out) == len(names) else names


def load_support(manifest_path: str | None, hvm: str | None) -> dict:
    if hvm:
        raw = subprocess.run([hvm, "--dump-support"], capture_output=True, text=True, check=True).stdout
        data = json.loads(raw)
    elif manifest_path:
        data = json.loads(Path(manifest_path).read_text())
    else:
        return {}
    status = {}
    syms = data.get("symbols") or []
    pretty = demangle([s["name"] for s in syms])
    for s, p in zip(syms, pretty):
        status[("function", norm(p))] = s.get("status", "Real")
        status[("data", norm(p))] = s.get("status", "Real")
    for c in data.get("classes") or []:
        status[("class", c["name"])] = c.get("status", "Real")
        for meth in c.get("methods") or []:
            for sign in "-+":
                status[("selector", f"{sign}[{c['name']}{meth}]")] = c.get("status", "Real")
    return status


def now_status(support: dict, kind: str, name: str) -> str:
    if not support:
        return ""
    if kind == "selector":
        m = re.match(r"([-+])\[(\S+) ([^\]]+)\]", name)
        return support.get(("selector", f"{m.group(1)}[{m.group(2)}{m.group(3)}]"), "") if m else ""
    if kind == "class":
        return support.get(("class", name), "")
    key = norm(name)
    return support.get((kind, key), "") or support.get(("function", key), "")


# ------------------------------------------------------------------ ledger

def build_ledger(apps: dict, support: dict) -> dict:
    rows = {}
    for app in apps.values():
        for (kind, name), hits in app["called"].items():
            r = rows.setdefault((kind, name), {"kind": kind, "name": name, "calledBy": {}, "importedBy": set()})
            r["calledBy"][app["id"]] = r["calledBy"].get(app["id"], 0) + hits
        for kind, name in app["imported"]:
            r = rows.setdefault((kind, name), {"kind": kind, "name": name, "calledBy": {}, "importedBy": set()})
            r["importedBy"].add(app["id"])
    out = []
    for r in rows.values():
        called = sorted(r["calledBy"])
        imported = sorted(r["importedBy"] | set(called))
        out.append({
            "name": r["name"], "kind": r["kind"], "area": classify(r["name"]),
            "calledBy": called, "importedBy": imported,
            "score": 10 * len(called) + len(imported),
            "nowStatus": now_status(support, r["kind"], r["name"]),
        })
    out.sort(key=lambda r: (-r["score"], -len(r["calledBy"]), r["kind"], r["name"]))
    app_rows = []
    for a in sorted(apps.values(), key=lambda a: a["id"]):
        open_called = sorted(n for (k, n) in a["called"] if k != "limitation" and now_status(support, k, n) not in ("Real", "Approx"))
        app_rows.append({"id": a["id"], "crashed": a["crashed"], "crash": a["crash"],
                         "called": sorted(n for (_, n) in a["called"]), "openCalled": open_called,
                         "imported": len(a["imported"]), "sources": a["sources"]})
    return {"version": LEDGER_VERSION, "apps": app_rows, "apis": out, "haveSupport": bool(support)}


def is_open(row: dict) -> bool:
    return row["nowStatus"] not in ("Real", "Approx")


def markdown(ledger: dict, limit: int) -> str:
    apis, apps = ledger["apis"], ledger["apps"]
    lines = [f"# Missing-API ledger: {len(apps)} apps, {len(apis)} missing APIs", ""]
    if ledger["haveSupport"]:
        fixed = [r for r in apis if r["calledBy"] and not is_open(r)]
        lines += [f"{len(fixed)} called APIs in these logs are implemented in the current build (marked *now*).", ""]
    lines += ["## Called at run time and still missing (these stop games)", "",
              "| # | API | Kind | Area | Apps that called it | Imported by | Now |", "|---|---|---|---|---|---|---|"]
    called = [r for r in apis if r["calledBy"] and r["kind"] != "limitation"]
    fixed_rows = [r for r in called if not is_open(r)]
    for i, r in enumerate([r for r in called if is_open(r)][:limit], 1):
        who = ", ".join(r["calledBy"][:6]) + (f" +{len(r['calledBy']) - 6}" if len(r["calledBy"]) > 6 else "")
        lines.append(f"| {i} | `{r['name'][:90]}` | {r['kind']} | {r['area']} | {len(r['calledBy'])}: {who} | {len(r['importedBy'])} | {r['nowStatus'] or '-'} |")
    if fixed_rows:
        lines += ["", f"Implemented since these logs ({len(fixed_rows)}): " + ", ".join(f"`{r['name'][:60]}`" for r in fixed_rows[:limit])]
    limits = [r for r in apis if r["kind"] == "limitation"]
    if limits:
        lines += ["", "## Known limitations the apps ran into (approximations, not crashes)", "", "| Limitation | Apps |", "|---|---|"]
        lines += [f"| {r['name'][:90]} | {len(r['calledBy'])}: {', '.join(r['calledBy'][:6])} |" for r in limits[:limit]]
    lines += ["", "## Imported but not seen called (most apps first)", "",
              "| # | API | Kind | Area | Apps | Now |", "|---|---|---|---|---|---|"]
    imported = [r for r in apis if not r["calledBy"] and is_open(r)]
    for i, r in enumerate(imported[:limit], 1):
        lines.append(f"| {i} | `{r['name'][:90]}` | {r['kind']} | {r['area']} | {len(r['importedBy'])} | {r['nowStatus'] or '-'} |")
    areas = {}
    for r in called:
        if is_open(r):
            areas[r["area"]] = areas.get(r["area"], 0) + len(r["calledBy"])
    if areas:
        lines += ["", "## Open called APIs by area (app hits)", ""]
        lines += [f"- {a}: {n}" for a, n in sorted(areas.items(), key=lambda kv: (-kv[1], kv[0]))]
    lines += ["", "## Apps", "", "| App | Crashed | Open called APIs | Imports missing |", "|---|---|---|---|"]
    for a in apps:
        crash = ("yes: " + a["crash"][:60]) if a["crashed"] else ""
        lines.append(f"| {a['id']} | {crash} | {', '.join(x[:50] for x in a['openCalled'][:4]) or '-'} | {a['imported']} |")
    return "\n".join(lines) + "\n"


# -------------------------------------------------------------------- diff

def diff(old: dict, new: dict) -> str:
    def called_set(l):
        return {(r["kind"], r["name"]): r for r in l["apis"] if r["calledBy"] and is_open(r)}
    o, n = called_set(old), called_set(new)
    gone = sorted(set(o) - set(n), key=lambda k: (-len(o[k]["calledBy"]), k))
    added = sorted(set(n) - set(o), key=lambda k: (-len(n[k]["calledBy"]), k))
    old_apps = {a["id"]: a for a in old["apps"]}
    unblocked = sorted(a["id"] for a in new["apps"] if not a["openCalled"] and old_apps.get(a["id"], {}).get("openCalled"))
    newly_blocked = sorted(a["id"] for a in new["apps"] if a["openCalled"] and a["id"] in old_apps and not old_apps[a["id"]]["openCalled"])
    lines = [f"Resolved called APIs: {len(gone)}"] + [f"  - {k[1]} ({len(o[k]['calledBy'])} apps)" for k in gone]
    lines += [f"New called APIs: {len(added)}"] + [f"  + {k[1]} ({len(n[k]['calledBy'])} apps: {', '.join(n[k]['calledBy'][:5])})" for k in added]
    lines += [f"Apps with no open called API now: {len(unblocked)}"] + [f"  * {a}" for a in unblocked]
    if newly_blocked:
        lines += [f"Apps that now hit an open API: {len(newly_blocked)}"] + [f"  ! {a}" for a in newly_blocked]
    return "\n".join(lines) + "\n"


# --------------------------------------------------------------------- run

def find_apps(root: Path) -> list[Path]:
    """Each bench folder holds one .app (possibly under Payload/), or an .ipa."""
    found = []
    for d in sorted(p for p in root.iterdir() if p.is_dir() or p.suffix == ".ipa"):
        if d.suffix in (".app", ".ipa"):
            found.append(d)
            continue
        apps = sorted(d.glob("*.app")) or sorted(d.glob("Payload/*.app")) or sorted(d.glob("*.ipa"))
        if apps:
            found.append(apps[0])
    return found


def run_one(hvm: str, app: Path, out: Path, seconds: int) -> Path:
    name = app.parent.name if app.parent.name not in ("Payload",) else app.parent.parent.name
    name = re.sub(r"[^A-Za-z0-9._-]", "_", name if app.suffix == ".app" else app.stem)
    report = out / f"{name}.json"
    home = out / "homes" / name
    home.mkdir(parents=True, exist_ok=True)
    cmd = [hvm, "run", str(app), "--home", str(home), "--keep-going", "--run-for", str(seconds), "--report", str(report)]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=seconds + 90, check=False)
        (out / f"{name}.log").write_text(r.stdout + r.stderr)
    except subprocess.TimeoutExpired:
        (out / f"{name}.log").write_text(f"timed out after {seconds + 90} s\n")
    return report


def cmd_run(args) -> int:
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    apps = find_apps(Path(args.apps))
    if args.only:
        apps = [a for a in apps if any(s.lower() in str(a).lower() for s in args.only)]
    print(f"running {len(apps)} apps for {args.seconds} s each, {args.jobs} at a time", file=sys.stderr)
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for report in pool.map(lambda a: run_one(args.hvm, a, out, args.seconds), apps):
            print(f"  {report.name}{'' if report.exists() else ' (no report)'}", file=sys.stderr)
    shutil.rmtree(out / "homes", ignore_errors=True)
    args.inputs = [str(out) + "/" + p.name for p in sorted(out.glob("*.json"))]
    return cmd_scan(args)


def cmd_scan(args) -> int:
    inputs = args.inputs
    raw = collect(inputs)
    if not raw:
        print("no HydrogenVM reports or logs found in the inputs", file=sys.stderr)
        return 2
    support = load_support(getattr(args, "support", None), getattr(args, "hvm", None))
    ledger = build_ledger(merge_apps(raw), support)
    if args.json:
        Path(args.json).write_text(json.dumps(ledger, indent=1, sort_keys=True) + "\n")
    md = markdown(ledger, args.limit)
    if args.md:
        Path(args.md).write_text(md)
    if not args.quiet:
        sys.stdout.write(md)
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def outputs(p):
        p.add_argument("--support", help="support manifest from `hvm --dump-support`")
        p.add_argument("--json", help="write the ledger as JSON (for `diff`)")
        p.add_argument("--md", help="write the markdown table to this file")
        p.add_argument("--limit", type=int, default=40, help="rows per table (default 40)")
        p.add_argument("--quiet", action="store_true", help="do not print the table")

    s = sub.add_parser("scan", help="rank missing APIs from reports, logs and export zips")
    s.add_argument("inputs", nargs="+", help="files or folders: hvm --report JSON, Test All report.json, *.log, export *.zip")
    s.add_argument("--hvm", help="hvm binary: its --dump-support says what the current build implements")
    outputs(s)
    s.set_defaults(func=cmd_scan)

    r = sub.add_parser("run", help="run every app in a bench folder with --report, then scan the reports")
    r.add_argument("--hvm", required=True)
    r.add_argument("--apps", required=True, help="folder of app folders (each with a .app, Payload/*.app or .ipa)")
    r.add_argument("--out", required=True, help="where the reports go")
    r.add_argument("--seconds", type=int, default=20)
    r.add_argument("--jobs", type=int, default=3)
    r.add_argument("--only", nargs="*", help="substrings of app paths to run")
    outputs(r)
    r.set_defaults(func=cmd_run)

    d = sub.add_parser("diff", help="compare two ledger JSON files")
    d.add_argument("old")
    d.add_argument("new")
    d.set_defaults(func=lambda a: sys.stdout.write(diff(json.loads(Path(a.old).read_text()), json.loads(Path(a.new).read_text()))) and 0)

    args = ap.parse_args(argv)
    return args.func(args) or 0


if __name__ == "__main__":
    sys.exit(main())
