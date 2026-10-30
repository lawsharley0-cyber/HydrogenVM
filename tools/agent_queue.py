#!/usr/bin/env python3
"""Shared work queue for coding agents (Claude, Codex, Antigravity, ...).

The queue is GitHub issues labelled `card`. Agents pull their own work, so no
bot has to drive anyone's account:

    python3 tools/agent_queue.py status
    python3 tools/agent_queue.py next  --agent codex
    python3 tools/agent_queue.py claim 12 --agent codex
    python3 tools/agent_queue.py release 12 --agent codex [--blocked]

A card is an issue with the labels: `card`, one `status:*` label, optionally
`needs:game-run` (must run real games) and `hotspot` (edits the shared hotspot
files, one at a time). Dependencies are a line `Depends-on: #3, #4` in the body.
Per-agent capability and weekly budget live in tools/agent_queue.json.

Selection (`pick`) is pure so it can be unit tested; only `gh` calls touch GitHub.
Uses the REST API through `gh api` (GraphQL is not needed).
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

CONFIG = Path(__file__).with_name("agent_queue.json")


def load_config(path: Path = CONFIG) -> dict:
    return json.loads(path.read_text())


def labels(issue: dict) -> set[str]:
    return {l["name"] if isinstance(l, dict) else l for l in issue.get("labels", [])}


def deps(issue: dict) -> list[int]:
    m = re.search(r"^Depends-on:\s*(.+)$", issue.get("body") or "", re.M | re.I)
    return [int(n) for n in re.findall(r"#(\d+)", m.group(1))] if m else []


def card_id(issue: dict) -> str:
    m = re.match(r"\s*(T\d+)", issue.get("title", ""))
    return m.group(1) if m else f"#{issue['number']}"


def pick(agent: str, open_cards: list[dict], closed_numbers: set[int], used_this_week: dict[str, int], cfg: dict):
    """Return (issue, reason) for the best ready card for `agent`, or (None, why)."""
    a = cfg["agents"].get(agent)
    if a is None:
        return None, f"unknown agent '{agent}' (known: {', '.join(cfg['agents'])})"
    in_progress = [c for c in open_cards if "status:in-progress" in labels(c)]
    if len(in_progress) >= cfg["wip_limit"]:
        return None, f"work limit reached: {len(in_progress)} cards in progress (limit {cfg['wip_limit']})"
    if used_this_week.get(agent, 0) >= a["weekly_budget"]:
        return None, f"{agent} has used its weekly budget ({a['weekly_budget']} cards); another agent can take the card"
    hotspot_taken = any("hotspot" in labels(c) for c in in_progress)
    open_numbers = {c["number"] for c in open_cards}
    best, best_score = None, None
    for c in open_cards:
        lab = labels(c)
        if "card" not in lab or "status:ready" not in lab:
            continue
        if any(d in open_numbers and d not in closed_numbers for d in deps(c)):
            continue
        if "needs:game-run" in lab and not a["can_run_games"]:
            continue
        if "hotspot" in lab and hotspot_taken:
            continue
        pref = a["prefers"]
        cid = card_id(c)
        score = (pref.index(cid) if cid in pref else len(pref), c["number"])
        if best_score is None or score < best_score:
            best, best_score = c, score
    if best is None:
        return None, "no ready card matches this agent right now (blocked, taken, or needs a different capability)"
    return best, f"{card_id(best)} is ready; dependencies done; fits {agent}"


def gh(*args: str, input_: str | None = None) -> str:
    r = subprocess.run(["gh", *args], capture_output=True, text=True, input=input_)
    if r.returncode:
        sys.exit(f"gh {' '.join(args)} failed: {r.stderr.strip()}")
    return r.stdout


def repo_slug() -> str:
    return json.loads(gh("api", "repos/{owner}/{repo}"))["full_name"]


def fetch_cards(repo: str):
    issues = json.loads(gh("api", "--paginate", f"repos/{repo}/issues?state=all&labels=card&per_page=100"))
    issues = [i for i in issues if "pull_request" not in i]
    open_cards = [i for i in issues if i["state"] == "open"]
    closed = {i["number"] for i in issues if i["state"] == "closed"}
    return open_cards, closed


def used_this_week(repo: str, cfg: dict) -> dict[str, int]:
    from datetime import datetime, timedelta, timezone
    since = (datetime.now(timezone.utc) - timedelta(days=7)).strftime("%Y-%m-%dT%H:%M:%SZ")
    out = {}
    for agent in cfg["agents"]:
        res = json.loads(gh("api", f"repos/{repo}/issues?state=all&labels=card,agent:{agent}&since={since}&per_page=100"))
        out[agent] = len([i for i in res if "pull_request" not in i])
    return out


def cmd_status(args):
    repo = repo_slug(); cfg = load_config()
    open_cards, closed = fetch_cards(repo)
    used = used_this_week(repo, cfg)
    print(f"Work limit {cfg['wip_limit']}; weekly use: " + ", ".join(f"{a} {used[a]}/{cfg['agents'][a]['weekly_budget']}" for a in cfg["agents"]))
    for c in sorted(open_cards, key=lambda c: c["number"]):
        st = sorted(l for l in labels(c) if l.startswith(("status:", "agent:", "needs:")) or l == "hotspot")
        print(f"#{c['number']:<4} {card_id(c):<4} {', '.join(st):<60} deps={deps(c) or '-'}")


def cmd_next(args):
    repo = repo_slug(); cfg = load_config()
    open_cards, closed = fetch_cards(repo)
    issue, why = pick(args.agent, open_cards, closed, used_this_week(repo, cfg), cfg)
    if issue is None:
        print(f"NOTHING: {why}"); return 1
    print(f"NEXT: #{issue['number']} {issue['title']}\n{issue['html_url']}\nwhy: {why}\nclaim it: python3 tools/agent_queue.py claim {issue['number']} --agent {args.agent}")


def set_labels(repo, n, add=(), remove=()):
    for l in remove:
        subprocess.run(["gh", "api", "-X", "DELETE", f"repos/{repo}/issues/{n}/labels/{l}"], capture_output=True)
    if add:
        gh("api", f"repos/{repo}/issues/{n}/labels", "--input", "-", input_=json.dumps({"labels": list(add)}))


def cmd_claim(args):
    repo = repo_slug(); cfg = load_config()
    open_cards, closed = fetch_cards(repo)
    issue = next((c for c in open_cards if c["number"] == args.number), None)
    if issue is None or "status:ready" not in labels(issue):
        sys.exit(f"#{args.number} is not a ready card (already claimed, blocked or closed)")
    pick_issue, why = pick(args.agent, open_cards, closed, used_this_week(repo, cfg), cfg)
    if pick_issue is None or pick_issue["number"] != args.number:
        sys.exit(f"{args.agent} may not claim #{args.number} now: {why}")
    set_labels(repo, args.number, add=[f"agent:{args.agent}", "status:in-progress"], remove=["status:ready"])
    gh("api", f"repos/{repo}/issues/{args.number}/comments", "-f", f"body=Claimed by {args.agent}. Plan comment to follow before coding (AGENTS.md rule 1).")
    print(f"claimed #{args.number} for {args.agent}")


def cmd_release(args):
    repo = repo_slug()
    new = "status:blocked" if args.blocked else "status:ready"
    set_labels(repo, args.number, add=[new], remove=["status:in-progress", f"agent:{args.agent}"])
    gh("api", f"repos/{repo}/issues/{args.number}/comments", "-f", f"body={args.agent} released this card ({'blocked' if args.blocked else 'ready for another agent'}). Add what you learned as a comment.")
    print(f"released #{args.number} -> {new}")


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("status").set_defaults(fn=cmd_status)
    for name, fn in (("next", cmd_next), ("claim", cmd_claim), ("release", cmd_release)):
        s = sub.add_parser(name); s.set_defaults(fn=fn)
        if name != "next":
            s.add_argument("number", type=int)
        s.add_argument("--agent", required=True)
        if name == "release":
            s.add_argument("--blocked", action="store_true")
    args = p.parse_args(argv)
    return args.fn(args) or 0


if __name__ == "__main__":
    sys.exit(main())
