"""Group harness findings by likely root cause.

Clustering reads the harness app record (itself built from ``hvm run
--report``). It does not decide that a signal is a merge blocker; see
``diffing.py`` for that. Owners match docs/dev-system/ARCHITECTURE_MAP.md.
Group letters are the audit's no-parallel sets. An empty group means the
change can run beside those sets if it stays inside the listed files.
"""
from __future__ import annotations

import re

_PC_LINE = re.compile(r"^\s*pc\s+(.+)$", re.MULTILINE)
_HEX = re.compile(r"0x[0-9a-fA-F]+")
_CLASS = re.compile(r"Missing class (\S+)")
_DATA = re.compile(r"Missing data symbol (.+?) \(using zeros\)")
_UNAVAILABLE = re.compile(
    r" \(([^)]*(?:disabled|offline|unavailable|not supported)[^)]*)\)$",
    re.IGNORECASE,
)
_GL = re.compile(r"^_?(gl|EAGL)")
_AUDIO = re.compile(r"^_?(al[A-Z]|alc|Audio|ExtAudio|AU[A-Z]|AV[A-Z])")
_TEXT = re.compile(r"^_?(CG|CT|kCG|kCT)")
_FRAMEWORK = re.compile(r"^_?(NS|CF|UI|CA|kCF)")
_OBJC = re.compile(r"^[+-]\[")


def owner_for_symbol(name: str) -> tuple[str, str, str]:
    """Return (area, likelyOwner, parallelGroup)."""
    if _OBJC.match(name) or _FRAMEWORK.match(name):
        return "framework", "guest-frameworks", "D"
    if _GL.match(name):
        return "gl", "graphics-audio-input", "B"
    if _AUDIO.match(name):
        return "audio", "graphics-audio-input", ""
    if _TEXT.match(name):
        return "text", "guest-frameworks", "D"
    return "libc", "guest-frameworks", ""


def clusters_for_app(app: dict) -> list[dict]:
    found: list[dict] = []
    seen: set[tuple] = set()

    def add(item: dict) -> None:
        key = (item["kind"], item["key"])
        if key in seen:
            return
        seen.add(key)
        item.setdefault("severity", "failure")
        item.setdefault("detail", "")
        found.append(item)

    status = app.get("status")
    if status == "error":
        add({
            "kind": "fixture",
            "key": "hash-or-path",
            "area": "harness",
            "likelyOwner": "test-regression",
            "group": "",
            "severity": "failure",
            "detail": app.get("reason") or "fixture resolution failed",
        })
    load = (app.get("loadError") or "").strip()
    if load:
        encrypted = "encrypt" in load.lower() or "fairplay" in load.lower()
        add({
            "kind": "encrypted" if encrypted else "load",
            "key": _squash(load)[:160],
            "area": "loader",
            "likelyOwner": "app-compatibility",
            "group": "",
            "severity": "failure",
            "detail": load[:500],
        })
    if app.get("crashed") or (app.get("crashReport") or "").strip():
        add(_fault_cluster(app.get("crashReport") or ""))
    for name in app.get("unimplementedImports") or []:
        _add_symbol(add, str(name), from_import=True)
    for name in app.get("missingFunctions") or []:
        _add_symbol(add, str(name), from_import=False)
    for warning in app.get("warnings") or []:
        message = warning.get("message", "") if isinstance(warning, dict) else str(warning)
        match = _CLASS.match(message)
        if match:
            add({
                "kind": "missing_class",
                "key": match.group(1),
                "area": "framework",
                "likelyOwner": "guest-frameworks",
                "group": "D",
                "severity": "signal",
                "detail": message[:300],
            })
        data = _DATA.match(message)
        if data:
            area, owner, group = owner_for_symbol(data.group(1))
            add({
                "kind": "missing_data",
                "key": data.group(1),
                "area": area,
                "likelyOwner": owner,
                "group": group,
                "severity": "signal",
                "detail": message[:300],
            })
    frame = app.get("frame") or {}
    signal = frame.get("signal")
    if signal in ("all-black", "all-white", "no-frames"):
        add({
            "kind": "render",
            "key": signal,
            "area": "framebuffer",
            "likelyOwner": "graphics-audio-input",
            "group": "B",
            "severity": "failure" if signal == "no-frames" else "signal",
            "detail": (
                f"black={frame.get('blackFraction')} white={frame.get('whiteFraction')} "
                f"other={frame.get('otherFraction')} colors={frame.get('uniqueQuantizedColors')}"
            ),
        })
    elif signal == "mostly-one-color":
        add({
            "kind": "render",
            "key": "mostly-one-color",
            "area": "framebuffer",
            "likelyOwner": "graphics-audio-input",
            "group": "B",
            "severity": "signal",
            "detail": f"dominantShare={frame.get('dominantShare')}",
        })
    audio = app.get("audio") or {}
    if audio.get("transportError"):
        add({
            "kind": "audio",
            "key": f"transport-{audio.get('transportError')}",
            "area": "audio",
            "likelyOwner": "graphics-audio-input",
            "group": "",
            "severity": "failure",
            "detail": "mixer datagram send failed; guest audio itself may still be fine",
        })
    return found


def _add_symbol(add, name: str, *, from_import: bool) -> None:
    note = _UNAVAILABLE.search(name)
    bare = _UNAVAILABLE.sub("", name) if note else re.sub(r" \([^)]*\)$", "", name)
    area, owner, group = owner_for_symbol(bare)
    if note:
        add({
            "kind": "unavailable",
            "key": bare,
            "area": area,
            "likelyOwner": owner,
            "group": group,
            "severity": "signal",
            "detail": note.group(1)[:300],
        })
        return
    if bare.startswith(("-[", "+[")):
        kind = "missing_method"
    elif area == "gl":
        kind = "gl_feature"
    else:
        kind = "missing_symbol"
    add({
        "kind": kind,
        "key": bare if not from_import else name,
        "area": area,
        "likelyOwner": owner,
        "group": group,
        "severity": "failure",
        "detail": name[:300],
    })


def aggregate(apps: list[dict]) -> list[dict]:
    grouped: dict[tuple, dict] = {}
    for app in apps:
        for item in app.get("clusters") or []:
            key = (item["kind"], item["key"])
            slot = grouped.get(key)
            if slot is None:
                slot = {
                    "kind": item["kind"],
                    "key": item["key"],
                    "area": item.get("area", ""),
                    "likelyOwner": item.get("likelyOwner", ""),
                    "group": item.get("group", ""),
                    "severity": item.get("severity", "failure"),
                    "detail": item.get("detail", ""),
                    "apps": [],
                }
                grouped[key] = slot
            if app["id"] not in slot["apps"]:
                slot["apps"].append(app["id"])
            if item.get("severity") == "failure":
                slot["severity"] = "failure"
    return [grouped[k] for k in sorted(grouped)]


def _fault_cluster(report: str) -> dict:
    head = report.splitlines()[0].strip() if report.strip() else "crash"
    pc_match = _PC_LINE.search(report)
    pc = pc_match.group(1).strip() if pc_match else ""
    area, owner, group = "guest", "app-compatibility", ""
    if "NULL pointer" in head or re.search(r"at 0x0{4,8}\b", head):
        area, owner, group = "null", "runtime-cpu-memory", "A"
    elif "missing data symbol" in head or pc.startswith("[HydrogenVM]"):
        symbol = pc.split("]", 1)[-1].strip() if pc.startswith("[HydrogenVM]") else head
        sub, owner, group = owner_for_symbol(symbol)
        area = "hle-" + sub
    else:
        addr_match = re.search(r"0x([0-9a-fA-F]+)", pc)
        symbol = ""
        if addr_match:
            addr = int(addr_match.group(1), 16)
            rest = pc.split(None, 1)
            symbol = rest[1] if len(rest) == 2 else ""
            if addr < 0x1000:
                area, owner, group = "null", "runtime-cpu-memory", "A"
            elif addr >= 0xFFFF0000:
                area, owner, group = "hle", "guest-frameworks", "D"
            elif symbol:
                sub, owner, group = owner_for_symbol(symbol)
                area = "guest-" + sub
        elif symbol:
            sub, owner, group = owner_for_symbol(symbol)
            area = "guest-" + sub
    flat_pc = _HEX.sub("0x", pc)[:160]
    if "unsupported instruction" in head:
        key = "undefined:" + (flat_pc or _squash(head)[:120])
    elif "bad memory access" in head:
        key = "memfault:" + area + ":" + (flat_pc or "unknown")
    else:
        key = "exit:" + _squash(head)[:160]
    return {
        "kind": "fault",
        "key": key,
        "area": area,
        "likelyOwner": owner,
        "group": group,
        "severity": "failure",
        "detail": head[:400],
    }


def _squash(text: str) -> str:
    return _HEX.sub("0x", " ".join(text.split()))
