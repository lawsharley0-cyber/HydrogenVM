#!/usr/bin/env python3
"""Package the pinned client for HydrogenVM; never patch executable/assets.

This guest IPA requests HydrogenVM's embedded backend. It is not a native
modern-iOS port and cannot supply that backend to an older emulator.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path, PurePosixPath
import plistlib
import tempfile
import zipfile

SOURCE_SHA256 = "29269ede437f39ffa62fb4234a1c323303ef417e7eb6ddc5c86f9be76e08fb3f"
EXECUTABLE_SHA256 = "3519d7954cb3dc16b9d7746cc5dbd2521d0209eea2bbc163f6ab7cdfa5e52397"
BACKEND = "clash-2.1306.1"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def package(source, output, separate_profile=False):
    source, output = Path(source), Path(output)
    if output.exists():
        raise ValueError("Output already exists; choose a new path")
    if digest(source.read_bytes()) != SOURCE_SHA256:
        raise ValueError("Expected the documented pinned source IPA; input was not changed")
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(source) as original:
        names = original.namelist()
        if len(names) != len(set(names)):
            raise ValueError("Duplicate archive entries")
        for name in names:
            path = PurePosixPath(name)
            if path.is_absolute() or ".." in path.parts or "\\" in name:
                raise ValueError("Unsafe archive path")
        infos = [n for n in names if n.startswith("Payload/") and
                 len(PurePosixPath(n).parts) == 3 and n.endswith(".app/Info.plist")]
        if len(infos) != 1:
            raise ValueError("Expected one top-level app")
        info_name = infos[0]
        info = plistlib.loads(original.read(info_name))
        if (info.get("CFBundleIdentifier"), info.get("CFBundleShortVersionString")) != (
                "com.supercell.scroll", "2.1306.1"):
            raise ValueError("Wrong client identity/version")
        exe_name = str(PurePosixPath(info_name).parent / info["CFBundleExecutable"])
        if digest(original.read(exe_name)) != EXECUTABLE_SHA256:
            raise ValueError("Wrong client executable")
        info["CFBundleDisplayName"] = "Clash Royale Offline"
        if separate_profile:
            info["CFBundleIdentifier"] = "com.hydrogenvm.clashroyale.offline"
        info["HydrogenVMOfflineBackend"] = BACKEND
        info["HydrogenVMOfflinePackageVersion"] = 1
        modified = plistlib.dumps(info, fmt=plistlib.FMT_BINARY)
        # Keep all source members, permissions and contents except Info.plist.
        # A private temporary archive is verified before publishing its filename.
        with tempfile.TemporaryDirectory(prefix="hvm-clash-package-", dir=output.parent) as tmp:
            candidate = Path(tmp) / "offline.ipa"
            with zipfile.ZipFile(candidate, "w") as target:
                for member in original.infolist():
                    target.writestr(copy.copy(member), modified if member.filename == info_name
                                    else original.read(member.filename))
            with zipfile.ZipFile(candidate) as target:
                if target.namelist() != names or target.testzip() is not None:
                    raise ValueError("Output archive verification failed")
                for name in names:
                    if name != info_name and digest(target.read(name)) != digest(original.read(name)):
                        raise ValueError("Unexpected member change: " + name)
                packaged = plistlib.loads(target.read(info_name))
                if packaged != info:
                    raise ValueError("Output metadata verification failed")
            # Exclusive creation also prevents overwriting a concurrent output.
            with output.open("xb") as dest, candidate.open("rb") as src:
                import shutil
                shutil.copyfileobj(src, dest)
    return {
        "ipa": output.name, "sha256": digest(output.read_bytes()),
        "sourceIpaSHA256": SOURCE_SHA256, "executableSHA256": EXECUTABLE_SHA256,
        "backend": BACKEND, "packageVersion": 1, "architecture": "armv7 guest",
        "changedMembers": [info_name], "otherMembersByteIdentical": True,
        "bundleIdentifier": info["CFBundleIdentifier"], "appVersion": "2.1306.1",
        "separateProfile": separate_profile,
        "requires": "HydrogenVM with package-marker support and the embedded Clash backend",
        "nativeIOSApp": False, "deviceTested": False,
        "note": "Metadata was changed; existing signatures are not valid. Import into HydrogenVM. "
                "Packaging alone does not resolve the reported iPhone 50% login failure.",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source_ipa", type=Path)
    parser.add_argument("output_ipa", type=Path)
    parser.add_argument("--separate-profile", action="store_true",
                        help="Create its own emulator library entry and save, keeping original Clash separate")
    args = parser.parse_args()
    report = package(args.source_ipa, args.output_ipa, args.separate_profile)
    manifest = args.output_ipa.with_suffix(".manifest.json")
    with manifest.open("x", encoding="utf-8") as out:
        json.dump(report, out, indent=2)
        out.write("\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
