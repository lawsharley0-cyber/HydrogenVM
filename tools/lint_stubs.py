#!/usr/bin/env python3
"""Find unclassified constant-success HLE registrations (no compiler required).

This is a lexical audit, not a proof of API correctness. It follows direct
def/m/cm registrations to same-file function bodies and method/classMethod
lambdas. It ignores comments/string contents and balances nested delimiters.
Macros, indirect registration helpers and cross-file bodies need manual review.
All findings are printed, including acknowledged legacy debt. --baseline fails
on new/changed findings, so existing debt cannot hide new plausible success.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import re
from dataclasses import asdict, dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOKEN = re.compile(
    r'(?P<comment>//[^\n]*|/\*[\s\S]*?\*/)'
    r'|(?P<string>R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})\([\s\S]*?\)(?P=delimiter)"'
    r'|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\')'
    r'|(?P<word>[A-Za-z_]\w*|\d+(?:[uUlL]*))|(?P<punct>[^\s])'
)


@dataclass(frozen=True)
class Tok:
    value: str
    pos: int
    kind: str


@dataclass(frozen=True)
class Finding:
    path: str
    line: int
    registration: str
    function: str
    reason: str
    body_sha256: str

    def key(self):
        # Lines are deliberately excluded: moving code is not a new stub.
        return (self.path, self.registration, self.function, self.reason, self.body_sha256)


def tokens(source: str) -> list[Tok]:
    return [Tok(m.group(), m.start(), m.lastgroup or "string")
            for m in TOKEN.finditer(source) if m.lastgroup != "comment"]


def pairs(ts: list[Tok]) -> dict[int, int]:
    stack, result = [], {}
    for i, t in enumerate(ts):
        if t.value in ("(", "{", "["):
            stack.append(i)
        elif t.value in (")", "}", "]"):
            if stack and ts[stack[-1]].value == {")": "(", "}": "{", "]": "["}[t.value]:
                j = stack.pop()
                result[j] = i
                result[i] = j
    return result


def constant_success(body: list[Tok]) -> str | None:
    code = " ".join(t.value if t.kind != "string" else '"..."' for t in body)
    if not code:
        return "empty no-op body"
    returns = re.findall(r"\breturn\s+([^;]+);", code)
    if returns and all(r.strip() in {"0", "1", "true", "YES", "0u", "1u"} for r in returns):
        return "only constant success/boolean returns"
    # Includes capability queries expressed through the raw HLE register ABI.
    assignments = re.findall(r"\bcpu\s*\.\s*r\s*\[\s*0\s*\]\s*=\s*([^;]+);", code)
    if assignments and all(v.strip() in {"0", "1", "true", "YES"} for v in assignments):
        return "constant HLE result register"
    # GLES query stubs have void returns and write a fabricated success value.
    if re.search(r"\bwrite\s*<[^>]+>\s*\([^;]*,\s*1\s*\)", code):
        return "constant success output parameter"
    return None


def scan(source: str, path: str = "fixture.cpp") -> list[Finding]:
    ts = tokens(source)
    ps = pairs(ts)
    functions: dict[str, list[list[Tok]]] = {}
    controls = {"if", "for", "while", "switch", "catch", "sizeof", "alignof"}
    for i in range(len(ts) - 2):
        if ts[i].kind != "word" or ts[i].value in controls or ts[i + 1].value != "(":
            continue
        end = ps.get(i + 1)
        if end is not None and end + 1 < len(ts) and ts[end + 1].value == "{":
            close = ps.get(end + 1)
            if close is not None:
                functions.setdefault(ts[i].value, []).append(ts[end + 2:close])

    found = []

    def add(index, registration, function, body):
        reason = constant_success(body)
        if reason:
            canonical = " ".join(t.value for t in body)
            found.append(Finding(path, source.count("\n", 0, ts[index].pos) + 1,
                                 registration, function, reason,
                                 hashlib.sha256(canonical.encode()).hexdigest()))

    for i, token in enumerate(ts):
        # Explicit supportDef/supportMethod calls are separate APIs and cannot
        # accidentally exempt an unlabelled alias of the same function.
        if i == 0 or ts[i - 1].value != ".":
            continue
        if token.value in {"def", "m", "cm"}:
            # .def<&handler>("_symbol") / .m<&handler>("selector")
            if i + 7 >= len(ts):
                continue
            chunk = [t.value for t in ts[i + 1:i + 6]]
            if chunk[0:2] != ["<", "&"] or chunk[3:5] != [">", "("]:
                continue
            function = chunk[2]
            name = ts[i + 6]
            if name.kind == "string":
                for body in functions.get(function, []):
                    add(i, name.value.strip('"'), function, body)
        elif token.value in {"method", "classMethod", "defineFunction", "registerHostFunction"}:
            if i + 2 >= len(ts) or ts[i + 1].value != "(" or ts[i + 2].kind != "string":
                continue
            end = ps.get(i + 1, i + 1)
            name = ts[i + 2].value.strip('"')
            # A directly named HostFn follows the first comma.
            if i + 4 < end and ts[i + 3].value == "," and ts[i + 4].kind == "word":
                for body in functions.get(ts[i + 4].value, []):
                    add(i, name, ts[i + 4].value, body)
            # Only direct lambdas qualify; nested callbacks are not API bodies.
            start = i + 4
            if start < end and ts[start].value == "[":
                capture_end = ps.get(start, start)
                j = capture_end + 1
                if j < end and ts[j].value == "(":
                    j = ps.get(j, j) + 1
                if j < end and ts[j].value == "mutable":
                    j += 1
                if j < end and ts[j].value == "{":
                    close = ps.get(j)
                    if close is not None:
                        add(i, name, "<lambda>", ts[j + 1:close])
    return found


def audit(root: Path) -> list[Finding]:
    return sorted((finding for path in sorted((root / "src/runtime").rglob("*.cpp"))
                   for finding in scan(path.read_text(encoding="utf-8"), path.relative_to(root).as_posix())),
                  key=lambda f: (f.path, f.line, f.registration))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=ROOT)
    ap.add_argument("--baseline", type=Path)
    ap.add_argument("--write-baseline", type=Path)
    ap.add_argument("--json", type=Path, help="write every finding as structured JSON")
    args = ap.parse_args(argv)
    findings = audit(args.root)
    report = {"schemaVersion": 1, "findings": [asdict(f) for f in findings]}
    for f in findings:
        print(f"{f.path}:{f.line}: UNLABELLED {f.registration} ({f.function}): {f.reason}")
    for output in (args.json, args.write_baseline):
        if output:
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    old = Counter()
    if args.baseline:
        old = Counter(Finding(**f).key() for f in json.loads(args.baseline.read_text(encoding="utf-8"))["findings"])
    new = []
    for finding in findings:
        key = finding.key()
        if old[key]:
            old[key] -= 1
        else:
            new.append(finding)
    print(f"Stub audit: {len(findings)} unlabelled registrations; {len(new)} new/changed.")
    return int(bool(new) and not args.write_baseline)


if __name__ == "__main__":
    raise SystemExit(main())
