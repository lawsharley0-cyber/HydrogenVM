"""Classifying one night's run of a game and comparing two nights. No I/O beyond reading
screenshots, so the logic is unit-tested (tools/tests/test_nightly.py)."""
import json
from pathlib import Path

# Worst first. A game "got worse" when it moves to an earlier entry.
STATUSES = ["load-fail", "crash", "hang", "no-picture", "runs"]
SCREENSHOT_THRESHOLD = 0.25     # mean per-pixel difference (0..1) that counts as "looks different"


def classify(report: dict, exit_code, log_tail: str) -> dict:
    """One game's result from hvm's --report JSON and its exit code."""
    perf = report.get("perf", {})
    mixer = report.get("mixer", {})
    audio = report.get("audio", {})
    frames = int(perf.get("framesPresented") or perf.get("frames") or 0)
    sound = int(mixer.get("audibleFrames") or 0) > 0 or int(audio.get("nonSilentFrames") or 0) > 0
    missing = sorted({m.get("name", "") for m in report.get("missingApis", [])} - {""})
    defaulted = sorted({m.get("name", "") for m in report.get("defaultedApis", [])} - {""})

    if not report or report.get("loadError") or exit_code == 3:
        status = "load-fail"
        detail = report.get("loadError") or _last_line(log_tail)
    elif report.get("crashed"):
        status = "crash"
        detail = _first_line(report.get("crashReport", "")) or report.get("signature", "")
    elif exit_code is None:
        status = "hang"
        detail = "did not stop when asked"
    elif frames == 0:
        status = "no-picture"
        detail = "no frames presented"
    else:
        status = "runs"
        detail = ""
    return {"status": status, "detail": detail, "frames": frames, "sound": sound,
            "firstFrameMs": round(float(perf.get("timeToFirstFrameMs") or 0)),
            "missingApis": missing, "defaultedApis": defaulted}


def _first_line(text: str) -> str:
    return next((l.strip() for l in text.splitlines() if l.strip()), "")[:200]


def _last_line(text: str) -> str:
    lines = [l.strip() for l in text.splitlines() if l.strip()]
    return lines[-1][:200] if lines else ""


def load(path: Path):
    try:
        return json.loads(Path(path).read_text())
    except (OSError, ValueError):
        return None


def screenshot_difference(a: str, b: str):
    """Mean per-pixel difference of two screenshots (0 same .. 1 opposite), or None."""
    if not a or not b or not Path(a).exists() or not Path(b).exists():
        return None
    try:
        from PIL import Image, ImageChops, ImageStat
    except ImportError:
        return None
    try:
        size = (160, 90)
        x = Image.open(a).convert("L").resize(size)
        y = Image.open(b).convert("L").resize(size)
        return ImageStat.Stat(ImageChops.difference(x, y)).mean[0] / 255.0
    except OSError:
        return None


def compare(previous, current: dict, image_diff=screenshot_difference) -> dict:
    """What changed from the previous night. Regressions open an issue; notices are only listed."""
    before = {g["game"]: g for g in (previous or {}).get("games", [])}
    regressions, fixes, notices, new_games = [], [], [], []
    for game in current["games"]:
        name = game["game"]
        old = before.get(name)
        if old is None:
            new_games.append(name)
            continue
        rank_old, rank_new = STATUSES.index(old["status"]), STATUSES.index(game["status"])
        if rank_new < rank_old:
            regressions.append({"game": name, "what": f"{old['status']} → {game['status']}", "detail": game["detail"]})
        elif rank_new > rank_old:
            fixes.append({"game": name, "what": f"{old['status']} → {game['status']}"})
        if old.get("sound") and not game.get("sound") and game["status"] == "runs":
            regressions.append({"game": name, "what": "sound stopped", "detail": ""})
        added = sorted(set(game["missingApis"]) - set(old.get("missingApis", [])))
        if added:
            regressions.append({"game": name, "what": "new missing APIs", "detail": ", ".join(added[:12])})
        if game["status"] == "runs" and old["status"] == "runs":
            d = image_diff(old.get("screenshot", ""), game.get("screenshot", ""))
            if d is not None and d > SCREENSHOT_THRESHOLD:
                notices.append({"game": name, "what": f"screenshot looks different ({d:.0%})"})
    gone = sorted(set(before) - {g["game"] for g in current["games"]})
    return {"date": current["date"][:10],
            "commitRange": [(previous or {}).get("commit", ""), current.get("commit", "")],
            "regressions": regressions, "fixes": fixes, "notices": notices,
            "newGames": new_games, "removedGames": gone,
            "counts": {s: sum(g["status"] == s for g in current["games"]) for s in STATUSES}}


def summary_markdown(summary: dict) -> str:
    old, new = summary["commitRange"]
    lines = [f"## HydrogenVM nightly — {summary['date']}", ""]
    counts = summary["counts"]
    lines.append(" · ".join(f"{counts[s]} {s}" for s in reversed(STATUSES) if counts[s]))
    if old and new and old != new:
        lines.append(f"\nCommits since last night: `{old[:10]}..{new[:10]}` (bisect here)")
    elif new:
        lines.append(f"\nCommit: `{new[:10]}` (same as last night)" if old == new else f"\nCommit: `{new[:10]}`")
    for title, key in (("Got worse", "regressions"), ("Got better", "fixes"), ("Worth a look", "notices")):
        if summary[key]:
            lines += ["", f"### {title}"]
            for item in summary[key]:
                extra = f" — {item['detail']}" if item.get("detail") else ""
                lines.append(f"- **{item['game']}**: {item['what']}{extra}")
    if summary["newGames"]:
        lines += ["", "### New tonight", *(f"- {g}" for g in summary["newGames"])]
    if summary["removedGames"]:
        lines += ["", "### Not run tonight", *(f"- {g}" for g in summary["removedGames"])]
    if not (summary["regressions"] or summary["fixes"] or summary["notices"]):
        lines += ["", "Nothing changed."]
    return "\n".join(lines) + "\n"
