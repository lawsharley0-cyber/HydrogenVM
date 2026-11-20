#!/usr/bin/env python3
"""HydrogenVM Phase H1/I5: Automated fix-agent loop with hotspot & proof guards.

Reads gap ledgers (tools/android_gaps.py) and nightly regression reports (Phase A4),
picks the top-ranked uncovered task, enforces hotspot-file locks and per-run budgets,
and dispatches a model CLI with docs/HEAD_ENGINEER_PROMPT.md + proof-check gates.

Usage:
  python3 tools/fix_agent.py plan --ledger gaps.json --nightly status.json
  python3 tools/fix_agent.py run --ledger gaps.json --model-cmd "claude -p" --dry-run
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

HOTSPOT_FILES = frozenset(
    {
        "src/runtime/Process.cpp",
        "src/runtime/Process.hpp",
        "src/runtime/objc/Runtime.cpp",
        "src/runtime/libc/System.cpp",
        "src/core/cpu/Engine.cpp",
        "src/core/cpu/Engine.hpp",
        "src/runtime/android/Jni.cpp",
    }
)


@dataclass
class Candidate:
    kind: str  # "regression" | "gap"
    key: str
    title: str
    games_affected: int
    hit_count: int
    area: str
    target_files: list[str] = field(default_factory=list)
    evidence: dict[str, Any] = field(default_factory=dict)

    def sort_key(self) -> tuple[int, int, int, str]:
        # Regressions first, then most games affected, then hit count.
        return (0 if self.kind == "regression" else 1, -self.games_affected, -self.hit_count, self.key)


def load_candidates(
    ledger_path: Path | None = None,
    nightly_path: Path | None = None,
) -> list[Candidate]:
    out: list[Candidate] = []
    if nightly_path and nightly_path.exists():
        data = json.loads(nightly_path.read_text(encoding="utf-8"))
        for reg in data.get("regressions", []):
            game = str(reg.get("game", "unknown"))
            symptom = str(reg.get("symptom", "regression"))
            out.append(
                Candidate(
                    kind="regression",
                    key=f"regression:{game}:{symptom}",
                    title=f"Fix regression in {game}: {symptom}",
                    games_affected=1,
                    hit_count=int(reg.get("count", 1)),
                    area=str(reg.get("area", "runtime")),
                    target_files=list(reg.get("files", [])),
                    evidence=reg,
                )
            )

    if ledger_path and ledger_path.exists():
        data = json.loads(ledger_path.read_text(encoding="utf-8"))
        for entry in data.get("gaps", []):
            sym = str(entry.get("symbol") or entry.get("name") or "")
            if not sym:
                continue
            games = entry.get("games", [])
            n_games = len(games) if isinstance(games, list) else int(entry.get("games_affected", 1))
            out.append(
                Candidate(
                    kind="gap",
                    key=f"gap:{sym}",
                    title=f"Implement missing API {sym}",
                    games_affected=max(1, n_games),
                    hit_count=int(entry.get("count", 1)),
                    area=str(entry.get("area", "android")),
                    target_files=list(entry.get("files", [])),
                    evidence=entry,
                )
            )
    return out


def select_candidates(
    candidates: list[Candidate],
    open_pr_titles: list[str] | None = None,
    locked_files: set[str] | None = None,
    max_tasks: int = 1,
) -> list[Candidate]:
    """Rank candidates and filter out any covered by open PRs or hotspot locks."""
    pr_lower = [t.lower() for t in (open_pr_titles or [])]
    locks = set(locked_files or ())
    selected: list[Candidate] = []

    for cand in sorted(candidates, key=lambda c: c.sort_key()):
        if len(selected) >= max_tasks:
            break
        sym = cand.key.split(":", 1)[-1].lower()
        if any(sym in title for title in pr_lower):
            continue
        if any(f in locks and f in HOTSPOT_FILES for f in cand.target_files):
            continue
        selected.append(cand)
        for f in cand.target_files:
            if f in HOTSPOT_FILES:
                locks.add(f)
    return selected


def build_prompt(cand: Candidate, repo_root: Path) -> str:
    head_prompt = repo_root / "docs" / "HEAD_ENGINEER_PROMPT.md"
    lines = [
        f"Task: {cand.title}",
        f"Kind: {cand.kind} | Area: {cand.area} | Games affected: {cand.games_affected}",
        f"Rules: Follow {head_prompt.name} and AGENTS.md. Iteration cap: 8.",
        "Requirements (Phase I5 proof-check):",
        "  1. Add a minimal test that fails before the fix.",
        "  2. Implement a general fix (no title-specific hacks).",
        "  3. Run hvm_tests fulltest, tools/tests, Test Lab, and cpudiff (if CPU code changed).",
        "  4. Re-run the affected game/replay under --phone and attach evidence.",
        f"Evidence JSON: {json.dumps(cand.evidence, sort_keys=True)}",
    ]
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="HydrogenVM Phase H1 automated fix-agent orchestrator")
    sub = parser.add_subparsers(dest="cmd", required=True)

    for name in ("plan", "run"):
        p = sub.add_parser(name)
        p.add_argument("--ledger", type=Path, default=None)
        p.add_argument("--nightly", type=Path, default=None)
        p.add_argument("--max-tasks", type=int, default=1)
        p.add_argument("--open-pr", action="append", default=[], help="Open PR title to skip duplicates")
        p.add_argument("--locked-file", action="append", default=[], help="Hotspot file currently locked")
        if name == "run":
            p.add_argument("--model-cmd", default="", help="Model CLI command accepting prompt on stdin")
            p.add_argument("--dry-run", action="store_true")

    args = parser.parse_args(argv)
    repo_root = Path(__file__).resolve().parents[1]
    cands = load_candidates(args.ledger, args.nightly)
    picked = select_candidates(
        cands,
        open_pr_titles=args.open_pr,
        locked_files=set(args.locked_file),
        max_tasks=max(1, args.max_tasks),
    )

    plan_payload = {
        "total_candidates": len(cands),
        "selected": [
            {**asdict(c), "prompt": build_prompt(c, repo_root)}
            for c in picked
        ],
    }
    if args.cmd == "plan" or getattr(args, "dry_run", False):
        print(json.dumps(plan_payload, indent=2))
        return 0

    if not args.model_cmd:
        print("error: --model-cmd is required when not using --dry-run", file=sys.stderr)
        return 2

    for item in plan_payload["selected"]:
        proc = subprocess.run(
            args.model_cmd,
            input=item["prompt"],
            text=True,
            shell=True,
            cwd=str(repo_root),
            check=False,
        )
        if proc.returncode != 0:
            return proc.returncode
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
