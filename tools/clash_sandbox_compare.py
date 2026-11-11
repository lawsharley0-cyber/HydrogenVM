"""Run a touch plan in two independent copies of a closed guest home."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys

from clash_spell_catalog import catalog
from clash_spell_preset import encode


def compare(hvm, app, home, plan, output, variant, seconds):
    home, output = home.resolve(), output.resolve()
    if not app.is_dir() or not (app / "Info.plist").is_file():
        raise ValueError("Supply an extracted app bundle so HVM uses the exact guest sandbox")
    if not (home / "Documents").is_dir() or output.is_relative_to(home) or output.is_relative_to(app.resolve()):
        raise ValueError("Use a guest home with Documents and a separate new output directory")
    if any(path.is_symlink() for path in home.rglob("*")):
        raise ValueError("Guest home contains symbolic links; supply a self-contained snapshot")
    if not isinstance(json.loads(plan.read_text())["steps"], list):
        raise ValueError("Invalid UI plan")
    replacement = encode(json.loads(variant.read_text(encoding="utf-8")), catalog(app)) if variant else None
    output.mkdir(parents=True, exist_ok=False)
    baseline, modified = output / "baseline", output / "variant"
    # Finish both copies before HVM can write either one.
    shutil.copytree(home, baseline / "home")
    shutil.copytree(baseline / "home", modified / "home")
    if replacement is not None:
        (modified / "home/Documents/ClashSpellLab-v1.bin").write_bytes(replacement)
    for run in (baseline, modified):
        subprocess.run([
            sys.executable, str(Path(__file__).with_name("clash_guest_ui_probe.py")),
            "--hvm", str(hvm), "--app", str(app), "--home", str(run / "home"),
            "--plan", str(plan), "--out", str(run / "capture"), "--gpu", "--capture-audio",
            "--seconds", str(seconds), "--ready-log", "GuestOffline: queued response=24101",
            "--ready-timeout", "180",
        ], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("hvm", "app", "home", "plan", "out"):
        parser.add_argument("--" + option, required=True, type=Path)
    parser.add_argument("--variant", type=Path, help="Optional exported Spell Lab preset")
    parser.add_argument("--seconds", type=int, default=600)
    args = parser.parse_args()
    try:
        compare(args.hvm, args.app, args.home, args.plan, args.out, args.variant, args.seconds)
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"{exc}\n")


if __name__ == "__main__":
    main()
