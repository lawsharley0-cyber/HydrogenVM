"""Write reproducible issue markdown. Dry-run by default.

Nothing in this module contacts GitHub or any other tracker. ``--write``
only creates local files.
"""

import os
import re

_SLUG = re.compile(r"[^a-z0-9]+")


def generate(report: dict, *, write_dir: str | None = None) -> list[dict]:
    apps = {a["id"]: a for a in ((report.get("suites") or {}).get("apps") or {}).get("apps") or []}
    clusters = report.get("clusters") or []
    docs = []
    for cluster in clusters:
        body = render_issue(cluster, apps, report)
        name = _slug(f"{cluster.get('kind', 'issue')}-{cluster.get('key', 'item')}") + ".md"
        docs.append({"filename": name, "body": body, "filed": False})
    if write_dir:
        os.makedirs(write_dir, exist_ok=True)
        for doc in docs:
            path = os.path.join(write_dir, doc["filename"])
            with open(path, "w", encoding="utf-8") as f:
                f.write(doc["body"])
            doc["path"] = path
    return docs


def render_issue(cluster: dict, apps: dict, report: dict) -> str:
    kind = cluster.get("kind", "issue")
    key = cluster.get("key", "")
    severity = cluster.get("severity", "failure")
    owner = cluster.get("likelyOwner") or "unassigned"
    group = cluster.get("group") or "(none — may run beside groups A–E if the file list is respected)"
    app_ids = cluster.get("apps") or []
    lines = [
        f"# [compat] {kind}: {key}",
        "",
        "Dry-run issue. This file was generated locally and was not filed.",
        "",
        "## Classification",
        "",
        f"- Kind: `{kind}`",
        f"- Key: `{key}`",
        f"- Severity: `{severity}` ({'confirmed failure shape' if severity == 'failure' else 'rendering or stub signal; confirm before treating it as a defect'})",
        f"- Area: `{cluster.get('area', '')}`",
        f"- Suggested owner: `{owner}`",
        f"- No-parallel group: {group}",
        f"- Detail: {cluster.get('detail') or '(none)'}",
        "",
        "## Apps",
        "",
    ]
    if not app_ids:
        lines.append("No app id was attached.")
    for ident in app_ids:
        app = apps.get(ident) or {}
        lines.append(f"### {ident}")
        lines.append("")
        lines.append(f"- SHA-256: `{app.get('sha256', '')}`")
        lines.append(f"- Source: `{app.get('source')}`")
        lines.append(f"- Path: `{app.get('displayPath') or '(unresolved)'}`")
        lines.append(f"- Status: `{app.get('status')}`")
        lines.append(f"- Launch success: `{app.get('launchSuccess')}`")
        lines.append(f"- Crashed: `{app.get('crashed')}`")
        imports = app.get("unimplementedImports") or []
        if imports:
            lines.append("- Unimplemented imports: " + ", ".join(f"`{n}`" for n in imports))
        frame = app.get("frame") or {}
        if frame.get("signal") and frame.get("signal") != "not-run":
            lines.append(
                f"- Frame signal: `{frame.get('signal')}` "
                f"(black {frame.get('blackFraction')}, white {frame.get('whiteFraction')}, "
                f"other {frame.get('otherFraction')}, quantized colors {frame.get('uniqueQuantizedColors')})"
            )
        audio = app.get("audio") or {}
        if audio:
            lines.append(
                f"- Audio: measured={audio.get('measured')} signal={audio.get('signal')} "
                f"frames={audio.get('frames')} nonSilent={audio.get('nonSilentFrames')} "
                f"peak={audio.get('peak')} transportError={audio.get('transportError')}"
            )
        crash = (app.get("crashReport") or "").strip()
        if crash:
            lines.extend(["", "Crash report (truncated):", "", "```", crash[:2000], "```"])
        log = (app.get("logExcerpt") or "").strip()
        if log:
            lines.extend(["", "Log excerpt (truncated):", "", "```", log[:1500], "```"])
        lines.append("")
    commit = ((report.get("git") or {}).get("commit")) or "HEAD"
    lines.extend([
        "## Repro",
        "",
        "Fixtures are hashed in `tools/compat_harness/manifests/apps.json`.",
        "Point `HVM_FIXTURE_ROOT` at a directory that contains `fixtureRelPath`,",
        "or, only for the three grandfathered IPAs already in the clone, leave",
        "legacy mode at its default. Do not commit new IPA files.",
        "",
        "```sh",
        "python3 tools/hvm_compat.py apps \\",
        f"  --only {','.join(app_ids) if app_ids else '<app-id>'} \\",
        "  --out build/compat-repro",
        "```",
        "",
        f"Report under test was generated at commit `{commit}`.",
        "The harness does not decrypt FairPlay images and does not file this issue.",
        "",
        "## Owner notes",
        "",
        "- Confirm the suggested owner against `docs/dev-system/ARCHITECTURE_MAP.md`.",
        "- Stay out of that role's no-parallel group while the fix is in progress.",
        "- Re-run the command above and `python3 tools/hvm_compat.py diff` against the baseline.",
        "",
    ])
    return "\n".join(lines)


def _slug(text: str) -> str:
    slug = _SLUG.sub("-", text.lower()).strip("-")
    return slug[:80] or "issue"
