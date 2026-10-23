"""Read-only documentation checks. Passing tests suggest review, never closure."""
from __future__ import annotations

import re
import subprocess
from pathlib import Path

DOCUMENTS = ("BACKLOG.md", "STATUS.md", "IMPLEMENTATION_PLAN.md")
COMMIT = re.compile(r"`([0-9a-fA-F]{7,40})`")
BACKLOG_ID = re.compile(r"\bB-\d{3}\b")


def _git(root: Path, *args: str) -> bool:
    return subprocess.run(["git", "-C", str(root), *args], capture_output=True,
                          check=False).returncode == 0


def inspect(root: Path, report: dict | None = None) -> list[dict]:
    findings = []
    texts = {}
    known_commits = {}
    for name in DOCUMENTS:
        path = root / "docs" / "dev-system" / name
        try:
            text = path.read_text(encoding="utf-8")
        except OSError as error:
            findings.append({"kind": "missing-document", "file": name, "detail": str(error)})
            continue
        texts[name] = text
        for line_number, line in enumerate(text.splitlines(), 1):
            for sha in COMMIT.findall(line):
                if sha not in known_commits:
                    known_commits[sha] = _git(root, "cat-file", "-e", sha + "^{commit}")
                if not known_commits[sha]:
                    findings.append({"kind": "unavailable-commit", "file": name,
                                     "line": line_number, "detail": sha + " (fetch its history before declaring the reference invalid)"})
    if report is None:
        return findings
    git = report.get("git", {})
    commit = git.get("commit", "")
    # Old, dirty, unrelated, or failing reports cannot establish current evidence.
    if (not report.get("summary", {}).get("ok") or git.get("dirty") is not False
            or not re.fullmatch(r"[0-9a-fA-F]{40}", commit)
            or not _git(root, "cat-file", "-e", commit + "^{commit}")
            or not _git(root, "merge-base", "--is-ancestor", commit, "HEAD")):
        findings.append({"kind": "unusable-evidence", "detail": "Use a passing, clean report from a commit in this checkout's history."})
        return findings
    results: dict[str, list[str]] = {}
    for suite in report.get("suites", {}).values():
        if not isinstance(suite, dict):
            continue
        for case in suite.get("cases", []):
            # Only an explicit backlog ID binds a case to a task. A green fulltest
            # or smoke does not establish unrelated behavior or device support.
            for task in set(BACKLOG_ID.findall(case.get("id", ""))):
                results.setdefault(task, []).append(case.get("status", "unknown"))
    for line_number, line in enumerate(texts.get("BACKLOG.md", "").splitlines(), 1):
        if not line.startswith("| B-"):
            continue
        columns = [part.strip() for part in line.split("|")]
        task, state = columns[1], columns[-2]
        if state.startswith("Planned") and results.get(task) and all(status == "pass" for status in results[task]):
            findings.append({"kind": "review-stale-status", "file": "BACKLOG.md", "line": line_number,
                             "detail": f"{task}: explicitly linked cases pass at {commit[:12]}; review scope and limits before changing status."})
    return findings
