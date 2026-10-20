#!/usr/bin/env python3
"""Prepare and inspect the pinned early Clash Royale client; never patch it."""
import argparse
import collections
import csv
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import plistlib
import re
import subprocess
import urllib.request
import zipfile

from compat import macho_info

URL = "https://github.com/astralsc/clashroyale_archive/releases/download/Clients/Clash%2BRoyale%2B.Supercell.%2B.v2.1306.1%2B3GS%2BUniv%2BLP%2Bos70.-HoRRcH.rc330_902.ipa"
SHA256 = "29269ede437f39ffa62fb4234a1c323303ef417e7eb6ddc5c86f9be76e08fb3f"

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default="clash-report")
    parser.add_argument("--ipa", help="use a local copy of the same pinned IPA")
    args = parser.parse_args()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    ipa = Path(args.ipa).resolve() if args.ipa else out / "client.ipa"
    if not args.ipa:
        request = urllib.request.Request(URL, headers={"User-Agent": "HydrogenVM-compat-investigation"})
        with urllib.request.urlopen(request, timeout=120) as response, ipa.open("wb") as target:
            while True:
                chunk = response.read(1024 * 1024)
                if not chunk:
                    break
                target.write(chunk)
    digest = hashlib.file_digest(ipa.open("rb"), "sha256").hexdigest()
    if digest != SHA256:
        raise SystemExit("IPA checksum mismatch; refusing to inspect or run a different build")
    dest = out / "unpacked"
    dest.mkdir(exist_ok=True)
    inventory = []
    csv_tables = []
    with zipfile.ZipFile(ipa) as archive:
        for member in archive.infolist():
            name = PurePosixPath(member.filename)
            if name.is_absolute() or ".." in name.parts or "\\" in member.filename:
                raise SystemExit("Unsafe archive path: " + member.filename)
            mode = member.external_attr >> 16
            if mode & 0o170000 == 0o120000:
                raise SystemExit("Symlink not supported in investigation archive")
            if member.file_size > 512 * 1024 * 1024:
                raise SystemExit("Unexpectedly large archive entry")
            inventory.append({"path": member.filename, "bytes": member.file_size})
        if sum(m.file_size for m in archive.infolist()) > 1024 * 1024 * 1024:
            raise SystemExit("Unexpectedly large unpacked archive")
        for member in archive.infolist():
            if member.filename.lower().endswith(".csv"):
                raw = archive.read(member)
                rows = list(csv.reader(io.StringIO(raw.decode("utf-8-sig", errors="replace"))))
                csv_tables.append({"path": member.filename, "rows": len(rows),
                                   "header": rows[0] if rows else []})
        archive.extractall(dest)
    apps = sorted((dest / "Payload").glob("*.app"))
    if len(apps) != 1:
        raise SystemExit("Expected one top-level app bundle")
    app = apps[0]
    with (app / "Info.plist").open("rb") as stream:
        info = plistlib.load(stream)
    exe_name = info["CFBundleExecutable"]
    if Path(exe_name).name != exe_name:
        raise SystemExit("Unsafe executable name")
    executable = app / exe_name
    macho = macho_info(str(executable))
    data = executable.read_bytes()
    words = [m.group().decode("ascii") for m in re.finditer(rb"[ -~]{5,}", data)]
    # These are clues to investigate, not a recovered wire protocol.
    terms = re.compile(r"login|battle|tutorial|arena|matchmak|offline|server|socket|message", re.I)
    clues = list(dict.fromkeys(w for w in words if terms.search(w) and len(w) < 240))
    metadata = {
        "source": URL, "sha256": digest, "ipaBytes": ipa.stat().st_size,
        "bundleId": info.get("CFBundleIdentifier"),
        "version": info.get("CFBundleShortVersionString"),
        "build": info.get("CFBundleVersion"), "minimumOS": info.get("MinimumOSVersion"),
        "executable": exe_name, "macho": macho, "csvTables": csv_tables,
        "fileTypes": dict(collections.Counter(Path(m["path"]).suffix.lower() for m in inventory)),
        "matchingStringCount": len(clues),
        "limitations": ["String matches do not establish message layouts or battle authority.",
                        "A successful smoke run does not establish playable battles."]
    }
    (out / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    (out / "inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
    (out / "logic-clues.txt").write_text("\n".join(clues[:1000]) + "\n")
    for name, options in [
        ("imports.txt", ["--bind", "--lazy-bind"]),
        ("libraries.txt", ["--dylibs-used"]),
        ("headers.txt", ["--private-headers"]),
    ]:
        result = subprocess.run(["llvm-objdump", "--macho", *options, str(executable)],
                                capture_output=True, text=True, timeout=120)
        (out / name).write_text(result.stdout + result.stderr)
        if result.returncode:
            raise SystemExit("llvm-objdump failed for " + name)
    print(json.dumps(metadata, indent=2), flush=True)
    if macho["error"] or macho["encrypted"]:
        raise SystemExit("Client is unsupported or encrypted; no execution attempted")

if __name__ == "__main__":
    main()
