#!/usr/bin/env python3
"""Generate a static public compatibility list page from nightly status JSON (Phase G1).

Reads nightly `status.json` (from `tools/nightly/nightly.py`) plus optional
community report JSON and produces a self-contained `index.html` + `compat.json`
suitable for GitHub Pages.

Private screenshots are excluded by default; only entries with an explicit
`publicScreenshot` URL/path (or matched via `--allow-screenshots`) are shown.
"""
import argparse
import html
import json
import re
from pathlib import Path
from typing import Any

STATUS_ORDER = ["Plays", "In-game", "Menus", "Boots", "No picture", "Hangs", "Crashes"]

RAW_TO_COMPAT = {
    "plays": "Plays",
    "in-game": "In-game",
    "menus": "Menus",
    "boots": "Boots",
    "runs": "Boots",
    "no-picture": "No picture",
    "no picture": "No picture",
    "hang": "Hangs",
    "hangs": "Hangs",
    "crash": "Crashes",
    "crashes": "Crashes",
    "load-fail": "Crashes",
}

ISSUE_RE = re.compile(r"^\d+$")


def normalize_status(entry: dict[str, Any]) -> str:
    for key in ("compatStatus", "status"):
        val = str(entry.get(key) or "").strip()
        if not val:
            continue
        mapped = RAW_TO_COMPAT.get(val.lower())
        if mapped:
            return mapped
        if val in STATUS_ORDER:
            return val
    return "Crashes"


def normalize_issues(raw: Any, repo: str) -> list[dict[str, Any]]:
    out = []
    if not isinstance(raw, list):
        return out
    seen = set()
    for item in raw:
        if isinstance(item, int) and item > 0:
            num = str(item)
            url = f"https://github.com/{repo}/issues/{num}"
        elif isinstance(item, str):
            s = item.strip().lstrip("#")
            if ISSUE_RE.match(s):
                num = s
                url = f"https://github.com/{repo}/issues/{num}"
            elif s.startswith("https://github.com/"):
                num = s.rstrip("/").split("/")[-1]
                url = s
            else:
                continue
        elif isinstance(item, dict) and item.get("number"):
            num = str(item["number"]).strip().lstrip("#")
            if not ISSUE_RE.match(num):
                continue
            url = str(item.get("url") or f"https://github.com/{repo}/issues/{num}")
        else:
            continue
        if num not in seen:
            seen.add(num)
            out.append({"number": num, "url": url})
    return out


def build_dataset(
    nightly: dict[str, Any],
    community: list[dict[str, Any]] | None = None,
    repo: str = "lawsharley0-cyber/HydrogenVM",
    allow_screenshots: set[str] | None = None,
) -> dict[str, Any]:
    commit = str(nightly.get("commit") or "")
    date = str(nightly.get("date") or "")
    allow = allow_screenshots or set()
    rows: list[dict[str, Any]] = []

    def add_entry(raw: dict[str, Any], source: str, default_date: str, default_commit: str) -> None:
        name = str(raw.get("title") or raw.get("game") or "").strip()
        if not name:
            return
        status = normalize_status(raw)
        platform = str(raw.get("platform") or "iOS").strip() or "iOS"
        engine = str(raw.get("engine") or "Unknown").strip() or "Unknown"
        shot = str(raw.get("publicScreenshot") or "").strip()
        if not shot and name in allow:
            shot = str(raw.get("screenshot") or "").strip()
        rows.append(
            {
                "game": name,
                "status": status,
                "platform": platform,
                "engine": engine,
                "commit": str(raw.get("commit") or default_commit)[:12],
                "date": str(raw.get("date") or default_date),
                "sound": bool(raw.get("sound", False)),
                "frames": int(raw.get("frames") or 0),
                "detail": str(raw.get("detail") or "")[:240],
                "screenshot": shot,
                "issues": normalize_issues(raw.get("issues"), repo),
                "source": source,
            }
        )

    for g in nightly.get("games", []):
        if isinstance(g, dict):
            add_entry(g, "nightly", date, commit)

    for g in community or []:
        if isinstance(g, dict):
            add_entry(g, "community", str(g.get("date") or ""), str(g.get("commit") or ""))

    rank = {s: i for i, s in enumerate(STATUS_ORDER)}
    rows.sort(key=lambda r: (rank.get(r["status"], 99), r["game"].lower(), r["source"]))

    counts = {s: 0 for s in STATUS_ORDER}
    for r in rows:
        counts[r["status"]] = counts.get(r["status"], 0) + 1

    return {
        "schemaVersion": 1,
        "repo": repo,
        "generatedFrom": {"commit": commit[:12], "date": date},
        "counts": counts,
        "total": len(rows),
        "games": rows,
    }


def render_html(dataset: dict[str, Any]) -> str:
    safe_json = json.dumps(dataset, ensure_ascii=False).replace("</", "<\\/")
    gen = dataset["generatedFrom"]
    commit_label = html.escape(gen.get("commit") or "unknown")
    date_label = html.escape(gen.get("date") or "unknown")
    repo_label = html.escape(dataset.get("repo") or "lawsharley0-cyber/HydrogenVM")

    return f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>HydrogenVM Compatibility List</title>
  <style>
    :root {{
      --bg: #0f131a; --card: #181e29; --line: #2a3345; --fg: #e8edf5; --muted: #9aa7bd;
      --plays: #22c55e; --ingame: #4ade80; --menus: #38bdf8; --boots: #60a5fa;
      --nopicture: #f59e0b; --hangs: #fb923c; --crashes: #f87171;
    }}
    * {{ box-sizing: border-box; }}
    body {{ margin: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
            background: var(--bg); color: var(--fg); line-height: 1.45; }}
    main {{ max-width: 1120px; margin: 0 auto; padding: 24px 16px 48px; }}
    h1 {{ margin: 0 0 6px; font-size: 1.75rem; }}
    .meta {{ color: var(--muted); font-size: 0.9rem; margin-bottom: 18px; }}
    .summary {{ display: flex; flex-wrap: wrap; gap: 8px; margin-bottom: 16px; }}
    .pill {{ background: var(--card); border: 1px solid var(--line); border-radius: 999px;
             padding: 4px 12px; font-size: 0.85rem; }}
    .controls {{ display: flex; flex-wrap: wrap; gap: 10px; margin-bottom: 16px; }}
    input, select {{ background: var(--card); color: var(--fg); border: 1px solid var(--line);
                     border-radius: 8px; padding: 8px 10px; font-size: 0.92rem; }}
    input {{ flex: 1 1 220px; }}
    table {{ width: 100%; border-collapse: collapse; background: var(--card);
             border: 1px solid var(--line); border-radius: 10px; overflow: hidden; }}
    th, td {{ text-align: left; padding: 10px 12px; border-bottom: 1px solid var(--line); vertical-align: top; }}
    th {{ font-size: 0.8rem; text-transform: uppercase; letter-spacing: 0.04em; color: var(--muted); }}
    .badge {{ display: inline-block; padding: 2px 8px; border-radius: 6px; font-weight: 600;
              font-size: 0.8rem; color: #0b1017; }}
    .s-Plays {{ background: var(--plays); }}
    .s-In-game {{ background: var(--ingame); }}
    .s-Menus {{ background: var(--menus); }}
    .s-Boots {{ background: var(--boots); }}
    .s-No-picture {{ background: var(--nopicture); }}
    .s-Hangs {{ background: var(--hangs); }}
    .s-Crashes {{ background: var(--crashes); }}
    .tag {{ display: inline-block; font-size: 0.75rem; padding: 1px 6px; border-radius: 5px;
            border: 1px solid var(--line); color: var(--muted); margin-left: 6px; }}
    .detail {{ color: var(--muted); font-size: 0.82rem; margin-top: 4px; }}
    a {{ color: #7dd3fc; text-decoration: none; }}
    a:hover {{ text-decoration: underline; }}
    img.shot {{ max-width: 120px; border-radius: 6px; display: block; margin-top: 6px; }}
  </style>
</head>
<body>
  <main>
    <h1>HydrogenVM Compatibility List</h1>
    <div class="meta">Build <code>{commit_label}</code> · Updated {date_label} ·
      <a href="https://github.com/{repo_label}">{repo_label}</a></div>
    <div id="summary" class="summary"></div>
    <div class="controls">
      <input id="q" type="search" placeholder="Search game or detail...">
      <select id="platform"><option value="">All platforms</option></select>
      <select id="engine"><option value="">All engines</option></select>
      <select id="status"><option value="">All statuses</option></select>
      <select id="source"><option value="">All sources</option><option value="nightly">Nightly</option><option value="community">Community</option></select>
    </div>
    <table>
      <thead>
        <tr><th>Game</th><th>Status</th><th>Platform</th><th>Engine</th><th>Tested</th><th>Issues</th></tr>
      </thead>
      <tbody id="rows"></tbody>
    </table>
  </main>
  <script id="compat-data" type="application/json">{safe_json}</script>
  <script>
    const data = JSON.parse(document.getElementById("compat-data").textContent);
    const $ = id => document.getElementById(id);
    const esc = s => String(s || "").replace(/[&<>"']/g, c => ({{"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}})[c]);
    function fillSelect(el, values) {{
      [...new Set(values)].sort().forEach(v => {{
        const o = document.createElement("option"); o.value = v; o.textContent = v; el.appendChild(o);
      }});
    }}
    fillSelect($("platform"), data.games.map(g => g.platform));
    fillSelect($("engine"), data.games.map(g => g.engine));
    fillSelect($("status"), data.games.map(g => g.status));

    function render() {{
      const q = $("q").value.trim().toLowerCase();
      const pf = $("platform").value, eng = $("engine").value, st = $("status").value, src = $("source").value;
      const shown = data.games.filter(g =>
        (!pf || g.platform === pf) &&
        (!eng || g.engine === eng) &&
        (!st || g.status === st) &&
        (!src || g.source === src) &&
        (!q || g.game.toLowerCase().includes(q) || g.detail.toLowerCase().includes(q) || g.engine.toLowerCase().includes(q))
      );
      const counts = {{}};
      shown.forEach(g => counts[g.status] = (counts[g.status] || 0) + 1);
      $("summary").innerHTML = `<span class="pill">Showing ${{shown.length}} / ${{data.total}}</span>` +
        Object.entries(counts).map(([k, v]) => `<span class="pill">${{esc(k)}}: ${{v}}</span>`).join("");
      $("rows").innerHTML = shown.map(g => {{
        const cls = "s-" + g.status.replace(/\\s+/g, "-");
        const srcTag = g.source === "community" ? `<span class="tag">community</span>` : "";
        const det = g.detail ? `<div class="detail">${{esc(g.detail)}}</div>` : "";
        const shot = g.screenshot ? `<img class="shot" src="${{esc(g.screenshot)}}" alt="${{esc(g.game)}} screenshot">` : "";
        const issues = (g.issues || []).map(i => `<a href="${{esc(i.url)}}">#${{esc(i.number)}}</a>`).join(" ");
        return `<tr>
          <td><strong>${{esc(g.game)}}</strong>${{srcTag}}${{det}}${{shot}}</td>
          <td><span class="badge ${{cls}}">${{esc(g.status)}}</span></td>
          <td>${{esc(g.platform)}}</td>
          <td>${{esc(g.engine)}}</td>
          <td><code>${{esc(g.commit)}}</code><div class="detail">${{esc(g.date)}}</div></td>
          <td>${{issues || "—"}}</td>
        </tr>`;
      }}).join("");
    }}
    ["q", "platform", "engine", "status", "source"].forEach(id => $(id).addEventListener("input", render));
    render();
  </script>
</body>
</html>
"""


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Build static public compatibility list from nightly status JSON.")
    ap.add_argument("--status", required=True, help="Path to nightly status.json / latest.json")
    ap.add_argument("--community", help="Optional path to community reports JSON (list or {games: [...]})")
    ap.add_argument("--out", required=True, help="Output directory for index.html and compat.json")
    ap.add_argument("--repo", default="lawsharley0-cyber/HydrogenVM", help="GitHub owner/repo for issue links")
    ap.add_argument("--allow-screenshots", nargs="*", default=[], help="Game names allowed to display screenshots")
    args = ap.parse_args(argv)

    nightly = json.loads(Path(args.status).read_text())
    community = None
    if args.community:
        raw_comm = json.loads(Path(args.community).read_text())
        community = raw_comm.get("games", []) if isinstance(raw_comm, dict) else raw_comm

    dataset = build_dataset(nightly, community=community, repo=args.repo, allow_screenshots=set(args.allow_screenshots))
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "compat.json").write_text(json.dumps(dataset, indent=2) + "\n")
    (out_dir / "index.html").write_text(render_html(dataset))
    print(f"Wrote {out_dir / 'index.html'} and {out_dir / 'compat.json'} ({dataset['total']} entries)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
