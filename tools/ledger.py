#!/usr/bin/env python3
"""Cross-game failure ledger.

Keys failures by signature across games and tags each with a failure class
(libc/libstdc++, opengl-es, foundation/uikit/objc, unity/mono, cpu, etc.).
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys


FAILURE_CLASSES = [
    ("libstdc++ ABI", [r"std::", r"std::string", r"_ZSt", r"__cxa", r"typeinfo", r"vtable", r"_Rb_tree", r"basic_string"]),
    ("OpenGL ES", [r"\bgl[A-Z]", r"OpenGLES", r"HostGL", r"Shader", r"EAGL", r"Framebuffer", r"texture", r"\bFBO\b"]),
    ("Foundation/UIKit/ObjC", [r"\bNS[A-Z]", r"\bUI[A-Z]", r"ObjC", r"objc_msgSend", r"unrecognized selector", r"-\[", r"\+\["]),
    ("Unity/Mono", [r"mono", r"Unity", r"il2cpp", r"libmono"]),
    ("CPU", [r"UndefinedInstruction", r"Unpredictable", r"GuestFault", r"cpu_fault", r"invalid opcode"]),
    ("libc", [r"\blibc\b", r"\babort\b", r"malloc", r"free", r"printf", r"sprintf", r"pthread", r"syscall", r"mprotect", r"SIGSEGV", r"SIGABRT"]),
    ("Audio", [r"Audio", r"AudioUnit", r"OpenAL", r"Mixer", r"AVAudio"]),
    ("Network", [r"Network", r"SCNetwork", r"socket", r"CFSocket"])
]


def classify_failure(signature: str, text: str) -> str:
    combined = f"{signature} {text}"
    for cls_name, patterns in FAILURE_CLASSES:
        for pat in patterns:
            if re.search(pat, combined, re.I):
                return cls_name
    return "other"


def normalize_signature(failure: dict) -> str:
    sig = failure.get("signature") or failure.get("crashSignature") or failure.get("key")
    if sig:
        return str(sig).strip()
    
    # Derive deterministic signature from summary/message
    summary = failure.get("summary") or failure.get("crashReport") or failure.get("message") or failure.get("text") or "unknown"
    first_line = str(summary).split("\n")[0].strip()
    norm = re.sub(r"0x[0-9a-fA-F]+", "<addr>", first_line)
    return "sig-" + hashlib.sha1(norm.encode()).hexdigest()[:12]


def group_failures(failures: list[dict]) -> list[dict]:
    """Group a list of failures by normalized signature across games/apps."""
    groups = {}
    for f in failures:
        app = f.get("app") or f.get("bundleId") or f.get("game") or f.get("id") or "unknown"
        sig = normalize_signature(f)
        full = str(f.get("summary") or f.get("crashReport") or f.get("message") or f.get("text") or sig)
        summary = full.split("\n")[0].strip()
        sev = int(f.get("severity", 1))

        cls = classify_failure(sig, full)   # classify the whole diagnostic: the telling line is not always the first
        if sig not in groups:
            groups[sig] = {
                "signature": sig,
                "failure_class": cls,
                "apps": set(),
                "severity": sev,
                "summaries": set()
            }
        g = groups[sig]
        if g["failure_class"] == "other" and cls != "other":
            g["failure_class"] = cls   # a later failure with the same signature identified the class
        g["apps"].add(app)
        g["severity"] = max(g["severity"], sev)
        if summary:
            g["summaries"].add(summary)

    # Convert sets to sorted lists
    result = []
    for g in groups.values():
        result.append({
            "signature": g["signature"],
            "failure_class": g["failure_class"],
            "apps": sorted(list(g["apps"])),
            "count": len(g["apps"]),
            "severity": g["severity"],
            "summaries": sorted(list(g["summaries"]))
        })
    
    return sorted(result, key=lambda x: (-x["count"], -x["severity"], x["signature"]))


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("reports", nargs="*", help="JSON report or failure files to ingest")
    p.add_argument("--json", action="store_true", help="Output JSON")
    args = p.parse_args(argv)

    failures = []
    for r_path in args.reports:
        data = json.loads(Path(r_path).read_text(encoding="utf-8"))
        if isinstance(data, list):
            failures.extend(data)
        elif isinstance(data, dict):
            if "results" in data:
                # A probe summary.json lists runs; each run carries its own failures in `issues`.
                for run in data["results"]:
                    if isinstance(run, dict) and "issues" in run:
                        for issue in run["issues"]:
                            f = {**issue, "app": run.get("bundleId") or issue.get("app")}
                            if issue.get("kind") == "crash" and run.get("signature"):
                                f["signature"] = run["signature"]   # the report's crash signature beats the first log line
                            failures.append(f)
                    else:
                        failures.append(run)
            else:
                failures.append(data)

    grouped = group_failures(failures)

    if args.json:
        print(json.dumps(grouped, indent=2))
        return 0

    print(f"{'Signature':<24} {'Class':<20} {'Count':<6} {'Apps':<30} {'Example':<30}")
    print("-" * 114)
    for g in grouped:
        apps_str = ", ".join(g["apps"][:3])
        if len(g["apps"]) > 3:
            apps_str += f" +{len(g['apps']) - 3}"
        summary = g["summaries"][0] if g["summaries"] else ""
        print(f"{g['signature']:<24} {g['failure_class']:<20} {g['count']:<6} {apps_str[:30]:<30} {summary[:30]}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
