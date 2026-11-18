#!/usr/bin/env python3
"""Android gap finder: what each game needs that HydrogenVM doesn't have, ranked across games,
and the labelled placeholders that let games get past those gaps while they are filled in.

  python3 tools/android_gaps.py scan APK_OR_DIR... [--hvm build/hvm] [--run SECONDS] [--out DIR]
      For every APK: the engine (and whether HydrogenVM has a driver for it), the native
      functions its libraries import that nothing provides, the Java methods it declares as
      native (its Java -> native entry points), and, with --run, what a short run actually
      hit (missing functions and Java methods, with signatures). Writes DIR/games/<apk>.json,
      DIR/ledger.json and DIR/ledger.md: every gap with the games that need it, most-needed first.

  python3 tools/android_gaps.py placeholders DIR/ledger.json [--write]
      Turns the ledger's gaps into placeholder entries in
      src/runtime/android/placeholders/{jni,native}.inc (merged with what is there; sorted;
      never removes an entry). Without --write it prints what would be added.

A placeholder is not support. Each one is registered as SupportStatus::Stub, answers a safe
default (Java: 0 / false / "" / an object of the declared type for a game's own plugin classes,
null for platform objects; native: -1 with errno ENOSYS for calls that report failure that way,
0 otherwise), and is reported as "placeholder" when a game hits it, so reports keep showing it as
work to do. A real implementation always takes precedence over a placeholder.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import zipfile
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PLACEHOLDER_DIR = ROOT / "src/runtime/android/placeholders"

# --------------------------------------------------------------------------------------- DEX --


def _uleb(data: bytes, pos: int) -> tuple[int, int]:
    result = shift = 0
    while True:
        b = data[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        if not b & 0x80:
            return result, pos
        shift += 7


def _mutf8(data: bytes, off: int) -> str:
    _, pos = _uleb(data, off)              # utf16 length
    end = data.index(b"\0", pos)
    raw = data[pos:end]
    # Modified UTF-8: NUL is C0 80, supplementary characters are surrogate pairs.
    return raw.replace(b"\xc0\x80", b"\0").decode("utf-8", errors="replace")


@dataclass
class DexInfo:
    classes: int = 0
    methods: int = 0
    natives: list = field(default_factory=list)          # (class, name, signature)
    framework_refs: dict = field(default_factory=dict)   # framework class -> referenced method count


def _descriptor_class(desc: str) -> str:
    return desc[1:-1] if desc.startswith("L") and desc.endswith(";") else desc


def parse_dex(data: bytes) -> DexInfo:
    """The class count, method references, native method declarations and framework (android.*,
    java.*) method references of one classes.dex."""
    if data[:4] != b"dex\n":
        raise ValueError("not a dex file")
    (string_ids_size, string_ids_off, type_ids_size, type_ids_off, proto_ids_size, proto_ids_off,
     _field_ids_size, _field_ids_off, method_ids_size, method_ids_off, class_defs_size,
     class_defs_off) = struct.unpack_from("<12I", data, 0x38)
    strings = [_mutf8(data, struct.unpack_from("<I", data, string_ids_off + 4 * i)[0])
               for i in range(string_ids_size)]
    types = [strings[struct.unpack_from("<I", data, type_ids_off + 4 * i)[0]] for i in range(type_ids_size)]

    def proto_sig(idx: int) -> str:
        _shorty, ret, params_off = struct.unpack_from("<III", data, proto_ids_off + 12 * idx)
        params = ""
        if params_off:
            (n,) = struct.unpack_from("<I", data, params_off)
            params = "".join(types[struct.unpack_from("<H", data, params_off + 4 + 2 * k)[0]] for k in range(n))
        return f"({params}){types[ret]}"

    methods = []
    for i in range(method_ids_size):
        cls, proto, name = struct.unpack_from("<HHI", data, method_ids_off + 8 * i)
        methods.append((_descriptor_class(types[cls]), strings[name], proto))

    info = DexInfo(classes=class_defs_size, methods=method_ids_size)
    for cls, _name, _proto in methods:
        if cls.startswith(("android/", "java/", "javax/", "dalvik/")):
            info.framework_refs[cls] = info.framework_refs.get(cls, 0) + 1

    for c in range(class_defs_size):
        class_data_off = struct.unpack_from("<I", data, class_defs_off + 32 * c + 24)[0]
        if not class_data_off:
            continue
        pos = class_data_off
        sf, pos = _uleb(data, pos)
        inf, pos = _uleb(data, pos)
        dm, pos = _uleb(data, pos)
        vm, pos = _uleb(data, pos)
        for _ in range(sf + inf):
            _, pos = _uleb(data, pos)
            _, pos = _uleb(data, pos)
        for count in (dm, vm):
            idx = 0
            for _ in range(count):
                diff, pos = _uleb(data, pos)
                flags, pos = _uleb(data, pos)
                _code, pos = _uleb(data, pos)
                idx += diff
                if flags & 0x100:                     # ACC_NATIVE
                    cls, name, proto = methods[idx]
                    info.natives.append((cls, name, proto_sig(proto)))
    return info

# ----------------------------------------------------------------------- binary AndroidManifest --


def parse_axml(data: bytes) -> list[tuple[str, dict]]:
    """The start elements of a compiled (binary) Android XML file: [(tag, {attribute: value})].
    Values are strings when the file stores them as strings, else "#<type>:<data>"."""
    if len(data) < 8 or struct.unpack_from("<H", data, 0)[0] != 0x0003:
        raise ValueError("not a binary XML file")
    strings: list[str] = []
    elements = []
    pos = struct.unpack_from("<H", data, 2)[0]
    while pos + 8 <= len(data):
        ctype, header_size, size = struct.unpack_from("<HHI", data, pos)
        if size == 0:
            break
        if ctype == 0x0001:                           # string pool
            count, _styles, flags, strings_start, _ = struct.unpack_from("<IIIII", data, pos + 8)
            utf8 = bool(flags & 0x100)
            for i in range(count):
                off = pos + strings_start + struct.unpack_from("<I", data, pos + header_size + 4 * i)[0]
                if utf8:
                    n = data[off]
                    off += 2 if n & 0x80 else 1
                    n = data[off]
                    if n & 0x80:
                        n = ((n & 0x7F) << 8) | data[off + 1]
                        off += 2
                    else:
                        off += 1
                    strings.append(data[off:off + n].decode("utf-8", errors="replace"))
                else:
                    n = struct.unpack_from("<H", data, off)[0]
                    off += 2
                    if n & 0x8000:
                        n = ((n & 0x7FFF) << 16) | struct.unpack_from("<H", data, off)[0]
                        off += 2
                    strings.append(data[off:off + 2 * n].decode("utf-16-le", errors="replace"))
        elif ctype == 0x0102:                         # start element
            name_idx, attr_start, attr_size, attr_count = struct.unpack_from("<IHHH", data, pos + 20)
            attrs = {}
            for a in range(attr_count):
                apos = pos + 16 + attr_start + a * attr_size
                _ns, aname, raw, _vsize, _res0, vtype, vdata = struct.unpack_from("<IIIHBBI", data, apos)
                key = strings[aname] if aname < len(strings) else f"#{aname}"
                if raw != 0xFFFFFFFF and raw < len(strings):
                    attrs[key] = strings[raw]
                elif vtype == 0x03 and vdata < len(strings):
                    attrs[key] = strings[vdata]
                else:
                    attrs[key] = f"#{vtype}:{vdata}"
            elements.append((strings[name_idx] if name_idx < len(strings) else "?", attrs))
        pos += size
    return elements

# -------------------------------------------------------------------------------------- engines --

# (engine, supported by a HydrogenVM driver, test on the APK)
ENGINES = [
    ("Unity (IL2CPP)", False, lambda libs, s: "libil2cpp.so" in libs),
    ("Unity (Mono)", True, lambda libs, s: "libunity.so" in libs and "libmono.so" in libs),
    ("Unity", True, lambda libs, s: "libunity.so" in libs),
    ("Unreal Engine 4", False, lambda libs, s: "libUE4.so" in libs),
    ("Unreal Engine 3", False, lambda libs, s: any(n.startswith("libUnrealEngine3") or n == "libUE3.so" for n in libs)),
    ("EA Blast", True, lambda libs, s: "com/ea/blast/" in s),
    ("Ubisoft UAF", True, lambda libs, s: "libuaf.so" in libs),
    ("Cocos2d-x", False, lambda libs, s: any("cocos" in n.lower() for n in libs) or "org/cocos2dx/" in s),
    ("GameMaker", False, lambda libs, s: "libyoyo.so" in libs),
    ("libGDX", False, lambda libs, s: "libgdx.so" in libs or "com/badlogic/gdx/" in s),
    ("Adobe AIR", False, lambda libs, s: "libCore.so" in libs and "com/adobe/air/" in s),
    ("Corona", False, lambda libs, s: "libcorona.so" in libs),
    ("Marmalade", False, lambda libs, s: any(n.startswith("libs3e") for n in libs)),
]


def detect_engine(libs: set[str], dex_text: str, entry_points: set[str], native_activity: bool) -> tuple[str, str]:
    """(engine name, support): support is "driver" (HydrogenVM has one), "planned" or "none"."""
    for name, supported, test in ENGINES:
        if test(libs, dex_text):
            return name, "driver" if supported else "none"
    if native_activity or "ANativeActivity_onCreate" in entry_points:
        return "NativeActivity", "planned"
    if not libs:
        return "Java only", "none"
    return "Java + native (custom)", "none"

# ------------------------------------------------------------------------------- native semantics --

# Calls whose documented failure is -1 with errno: a placeholder answers ENOSYS rather than a
# success it can't honour.
_FAIL_WITH_ERRNO = re.compile(
    r"^(socket|socketpair|connect|bind|listen|accept4?|send|sendto|sendmsg|recv|recvfrom|recvmsg|"
    r"setsockopt|getsockopt|getsockname|getpeername|shutdown|epoll_\w+|eventfd|inotify_\w+|"
    r"timerfd_\w+|signalfd|pipe2?|ioctl|prctl|ptrace|mlock\w*|munlock\w*|madvise|mincore|"
    r"sched_\w+|setpriority|setrlimit|getrlimit|syscall|sendfile|splice|tee|fallocate|"
    r"posix_fadvise|sync_file_range|flock|statfs|fstatfs|statvfs|fstatvfs|mkfifo|mknod)$")


def native_kind(symbol: str) -> str:
    """How a native placeholder answers: "fail" (-1, errno ENOSYS) or "zero"."""
    return "fail" if _FAIL_WITH_ERRNO.match(symbol) else "zero"

# ------------------------------------------------------------------------------------------ scan --

_JNI_MISSING = re.compile(r"^JNI (?P<cls>[\w/$]+)\.(?P<name>[\w<>$]+)(?P<sig>\([^)]*\)\S+)")


def run_hvm_json(hvm: str, args: list[str], timeout: int) -> dict | None:
    try:
        out = subprocess.run([hvm, *args], capture_output=True, text=True, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired):
        return None
    try:
        return json.loads(out.stdout)
    except json.JSONDecodeError:
        return None


def scan_apk(apk: Path, hvm: str | None, run_seconds: float, work: Path) -> dict:
    game: dict = {"apk": apk.name, "package": "", "engine": "", "support": "", "abi": "",
                  "libraries": [], "nativeMissing": [], "javaNatives": [], "dex": {},
                  "runtimeMissing": {"jni": [], "native": [], "other": []}, "notes": []}
    with zipfile.ZipFile(apk) as z:
        names = z.namelist()
        abis = sorted({n.split("/")[1] for n in names if n.startswith("lib/") and n.count("/") >= 2})
        game["abis"] = abis
        abi = next((a for a in ("armeabi-v7a", "armeabi") if a in abis), "")
        game["abi"] = abi
        if abis and not abi:
            game["notes"].append("no 32-bit ARM libraries (" + ", ".join(abis) + "): HydrogenVM's CPU is 32-bit")
        libs = {n.split("/")[-1] for n in names if abi and n.startswith(f"lib/{abi}/") and n.endswith(".so")}
        game["libraries"] = sorted(libs)
        if any(n.endswith(".obb") for n in names) or any(n.startswith("Android/obb/") for n in names):
            game["notes"].append("ships expansion (.obb) data")
        dex_text = ""
        info = DexInfo()
        for n in sorted(x for x in names if re.fullmatch(r"classes\d*\.dex", x)):
            try:
                d = parse_dex(z.read(n))
            except (ValueError, struct.error, IndexError) as e:
                game["notes"].append(f"{n}: unreadable ({e})")
                continue
            info.classes += d.classes
            info.methods += d.methods
            info.natives += d.natives
            for k, v in d.framework_refs.items():
                info.framework_refs[k] = info.framework_refs.get(k, 0) + v
            dex_text += "\n".join(f"{c}" for c, _, _ in d.natives) + "\n"
            dex_text += z.read(n).decode("latin-1")
        native_activity = False
        if "AndroidManifest.xml" in names:
            try:
                for tag, attrs in parse_axml(z.read("AndroidManifest.xml")):
                    if tag == "manifest":
                        game["package"] = attrs.get("package", "")
                    if tag == "activity" and attrs.get("name", "").endswith("NativeActivity"):
                        native_activity = True
            except (ValueError, struct.error, IndexError) as e:
                game["notes"].append(f"manifest unreadable ({e})")
        game["javaNatives"] = sorted({f"{c}.{m}{s}" for c, m, s in info.natives})
        game["dex"] = {"classes": info.classes, "methodRefs": info.methods,
                       "frameworkClasses": len(info.framework_refs),
                       "frameworkMethodRefs": sum(info.framework_refs.values())}

    entry_points: set[str] = set()
    if hvm and abi:
        imports = run_hvm_json(hvm, ["android-imports", str(apk), str(work / "apps"), "--json"], 300)
        if imports:
            for lib in imports.get("libraries", []):
                entry_points.update(lib.get("entryPoints", []))
                for m in lib.get("missing", []):
                    game["nativeMissing"].append({"symbol": m["symbol"], "library": lib["name"], "weak": m.get("weak", False)})
        else:
            game["notes"].append("hvm android-imports failed")
    game["engine"], game["support"] = detect_engine(libs, dex_text, entry_points, native_activity)

    if hvm and run_seconds > 0 and abi:
        home = work / "home" / apk.stem
        home.mkdir(parents=True, exist_ok=True)
        report = home / "report.json"
        try:
            subprocess.run([hvm, "run", str(apk), "--home", str(home), "--keep-going", "--gpu", "--run-for",
                            str(run_seconds), "--report", str(report)],
                           capture_output=True, timeout=run_seconds + 300)
        except (OSError, subprocess.TimeoutExpired):
            game["notes"].append("run did not finish")
        if report.exists():
            r = json.loads(report.read_text())
            game["run"] = {"crashed": bool(r.get("crashed")), "exitStatus": r.get("exitStatus"),
                           "framesPresented": (r.get("perf") or {}).get("framesPresented", 0)}
            for entry in r.get("missingApis", []):
                name = entry.get("name", "")
                m = _JNI_MISSING.match(name)
                if m:
                    game["runtimeMissing"]["jni"].append({"class": m["cls"], "name": m["name"], "sig": m["sig"],
                                                          "count": entry.get("count", 1)})
                elif name.startswith("JNI "):
                    game["runtimeMissing"]["other"].append(name)
                else:
                    game["runtimeMissing"]["native"].append(name.split(" ")[0])
    return game


def build_ledger(games: list[dict]) -> dict:
    """Every gap with the games that need it, most-needed first."""
    def add(table: dict, key: str, game: str, extra: dict) -> None:
        e = table.setdefault(key, {"key": key, "games": [], **extra})
        if game not in e["games"]:
            e["games"].append(game)

    native: dict = {}
    jni: dict = {}
    engines: dict = {}
    for g in games:
        name = g["package"] or g["apk"]
        engines.setdefault(g["engine"], {"engine": g["engine"], "support": g["support"], "games": []})["games"].append(name)
        for m in g["nativeMissing"]:
            add(native, m["symbol"], name, {"kind": native_kind(m["symbol"]), "source": "import"})
        for sym in g["runtimeMissing"]["native"]:
            add(native, sym, name, {"kind": native_kind(sym), "source": "run"})
        for m in g["runtimeMissing"]["jni"]:
            add(jni, f'{m["class"]}.{m["name"]}{m["sig"]}', name, {"class": m["class"], "name": m["name"], "sig": m["sig"]})

    def ranked(t: dict) -> list:
        return sorted(t.values(), key=lambda e: (-len(e["games"]), e["key"]))

    return {"games": len(games), "engines": sorted(engines.values(), key=lambda e: (-len(e["games"]), e["engine"])),
            "native": ranked(native), "jni": ranked(jni)}


def ledger_markdown(ledger: dict, games: list[dict]) -> str:
    lines = ["# Android gap ledger", "", f"{ledger['games']} games scanned.", "", "## Engines", "",
             "| Engine | HydrogenVM | Games |", "|---|---|---|"]
    for e in ledger["engines"]:
        lines.append(f"| {e['engine']} | {e['support']} | {len(e['games'])}: {', '.join(e['games'])} |")
    lines += ["", "## Games", "", "| Game | Engine | Missing imports | Hit in a run (Java / native) | Notes |",
              "|---|---|---|---|---|"]
    for g in games:
        rm = g["runtimeMissing"]
        lines.append(f"| {g['package'] or g['apk']} | {g['engine']} | {len(g['nativeMissing'])} | "
                     f"{len(rm['jni'])} / {len(rm['native'])} | {'; '.join(g['notes'])} |")
    for title, key in (("Missing native functions", "native"), ("Missing Java methods (hit in runs)", "jni")):
        lines += ["", f"## {title}", "", "| Needed by | Function | Games |", "|---|---|---|"]
        for e in ledger[key][:200]:
            lines.append(f"| {len(e['games'])} | `{e['key']}` | {', '.join(e['games'][:6])}{' …' if len(e['games']) > 6 else ''} |")
    return "\n".join(lines) + "\n"

# -------------------------------------------------------------------------------- placeholders --

JNI_INC = "jni.inc"
NATIVE_INC = "native.inc"
_JNI_LINE = re.compile(r'^\s*\{"(?P<cls>[^"]+)", "(?P<name>[^"]+)", "(?P<sig>[^"]+)"\},')
_NATIVE_LINE = re.compile(r'^\s*\{"(?P<sym>[^"]+)", Native::(?P<kind>\w+)\},')


def read_jni_inc(path: Path) -> set[tuple[str, str, str]]:
    if not path.exists():
        return set()
    return {(m["cls"], m["name"], m["sig"]) for line in path.read_text().splitlines() if (m := _JNI_LINE.match(line))}


def read_native_inc(path: Path) -> dict[str, str]:
    if not path.exists():
        return {}
    return {m["sym"]: m["kind"] for line in path.read_text().splitlines() if (m := _NATIVE_LINE.match(line))}


def format_jni_inc(entries: set[tuple[str, str, str]]) -> str:
    out = ["// Generated by tools/android_gaps.py placeholders; edit by re-running it, or delete a line",
           "// once the method is really implemented. {class, method, JNI signature}.", ""]
    out += [f'{{"{c}", "{n}", "{s}"}},' for c, n, s in sorted(entries)]
    return "\n".join(out) + "\n"


def format_native_inc(entries: dict[str, str]) -> str:
    out = ["// Generated by tools/android_gaps.py placeholders; edit by re-running it, or delete a line",
           "// once the function is really implemented. {symbol, how it answers}.", ""]
    out += [f'{{"{s}", Native::{k}}},' for s, k in sorted(entries.items())]
    return "\n".join(out) + "\n"


def placeholders_from_ledger(ledger: dict, jni: set, native: dict) -> tuple[set, dict]:
    new_jni = set(jni)
    for e in ledger["jni"]:
        new_jni.add((e["class"], e["name"], e["sig"]))
    new_native = dict(native)
    for e in ledger["native"]:
        if re.fullmatch(r"[A-Za-z_]\w*", e["key"]):
            new_native.setdefault(e["key"], "Fail" if e["kind"] == "fail" else "Zero")
    return new_jni, new_native

# ------------------------------------------------------------------------------------------- CLI --


def cmd_scan(a: argparse.Namespace) -> int:
    apks: list[Path] = []
    for p in map(Path, a.paths):
        apks += sorted(p.rglob("*.apk")) if p.is_dir() else [p]
    if not apks:
        print("no .apk files", file=sys.stderr)
        return 2
    out = Path(a.out)
    (out / "games").mkdir(parents=True, exist_ok=True)
    hvm = a.hvm if a.hvm and Path(a.hvm).exists() else None
    if not hvm:
        print("note: no hvm binary; native imports and runs are skipped", file=sys.stderr)
    games = []
    with tempfile.TemporaryDirectory(prefix="hvm-gaps-") as tmp:
        for apk in apks:
            print(f"scanning {apk.name}", file=sys.stderr)
            g = scan_apk(apk, hvm, a.run, Path(tmp))
            games.append(g)
            (out / "games" / f"{apk.stem}.json").write_text(json.dumps(g, indent=1))
    ledger = build_ledger(games)
    (out / "ledger.json").write_text(json.dumps(ledger, indent=1))
    (out / "ledger.md").write_text(ledger_markdown(ledger, games))
    print(f"{len(games)} games: {len(ledger['native'])} missing native functions, "
          f"{len(ledger['jni'])} missing Java methods -> {out / 'ledger.md'}")
    return 0


def cmd_placeholders(a: argparse.Namespace) -> int:
    ledger = json.loads(Path(a.ledger).read_text())
    jni_path, native_path = PLACEHOLDER_DIR / JNI_INC, PLACEHOLDER_DIR / NATIVE_INC
    jni, native = read_jni_inc(jni_path), read_native_inc(native_path)
    new_jni, new_native = placeholders_from_ledger(ledger, jni, native)
    added_jni = sorted(new_jni - jni)
    added_native = sorted(set(new_native) - set(native))
    for c, n, s in added_jni:
        print(f"+ jni     {c}.{n}{s}")
    for s in added_native:
        print(f"+ native  {s} ({new_native[s]})")
    if a.write:
        PLACEHOLDER_DIR.mkdir(parents=True, exist_ok=True)
        jni_path.write_text(format_jni_inc(new_jni))
        native_path.write_text(format_native_inc(new_native))
        print(f"wrote {len(added_jni)} Java and {len(added_native)} native placeholders; rebuild hvm")
    else:
        print(f"{len(added_jni)} Java and {len(added_native)} native placeholders would be added (--write to save)")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("scan", help="scan APKs and write the gap ledger")
    s.add_argument("paths", nargs="+")
    s.add_argument("--hvm", default=str(ROOT / "build/hvm"))
    s.add_argument("--run", type=float, default=0, help="also run each game this many seconds")
    s.add_argument("--out", default=str(ROOT / "build/android-gaps"))
    s.set_defaults(fn=cmd_scan)
    p = sub.add_parser("placeholders", help="write placeholder entries from a ledger")
    p.add_argument("ledger")
    p.add_argument("--write", action="store_true")
    p.set_defaults(fn=cmd_placeholders)
    a = ap.parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
