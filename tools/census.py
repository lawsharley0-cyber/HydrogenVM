#!/usr/bin/env python3
"""Per-IPA import census tool.

Lists imported non-ObjC symbols, referenced ObjC classes and selectors from
the declared Mach-O slice using llvm-nm / llvm-objdump, and compares them
against the support manifest emitted by `hvm --dump-support`.

Reproduces reference numbers from tools/research_census/README.md for the
committed repo-root IPAs.
"""
import argparse
import collections
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import plistlib
import zipfile

FIXTURE_MANIFEST = {
    "AngryBirds_v1.0_os30.ipa": {
        "sha256": "cdea1290dcaa67ec75cf555e78caaa8d90cfaa90021ec7f05e40089fb87944cf",
        "slice": "armv6",
        "thin": True,
        "payload_path": "Payload/AngryBirds.app/AngryBirds",
        "reference": {
            "imported_non_objc_symbols": 237,
            "unimplemented_symbols": 0,
            "missing_or_stubbed_classes": 0,
            "missing_selectors_classes_constants": 0,
        }
    },
    "Art_Of_Glow_v1.1.2_os313.ipa": {
        "sha256": "79266fe649b0449aaedefa7228c3d242e2f39b34d8cb2a1f1802467851b96f0e",
        "slice": "armv7",
        "thin": False,
        "payload_path": "Payload/ArtOfGlowAds.app/ArtOfGlowAds",
        "reference": {
            "imported_non_objc_symbols": 435,
            "unimplemented_symbols": 12,
            "missing_or_stubbed_classes": 2,
            "missing_selectors_classes_constants": 88,
        }
    },
    "Tiny_Tower_v1.0_os30.ipa": {
        "sha256": "7f84f2d2c561373917a05866337bdf8d8653b3ac8b801248d298f20b5c649578",
        "slice": "armv7",
        "thin": False,
        "payload_path": "Payload/Tiny Tower.app/Tiny Tower",
        "reference": {
            "imported_non_objc_symbols": 330,
            "unimplemented_symbols": 0,
            "missing_or_stubbed_classes": 7,
            "missing_selectors_classes_constants": 53,
        }
    }
}


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(1024 * 1024):
            h.update(chunk)
    return h.hexdigest().lower()


def slice_bytes(data: bytes, arch: str | None) -> bytes:
    magic = struct.unpack(">I", data[:4])[0]
    if magic == 0xcafebabe:
        n = struct.unpack(">I", data[4:8])[0]
        for i in range(n):
            ct, cs, off, size, al = struct.unpack(">5I", data[8 + 20 * i : 28 + 20 * i])
            if (arch == "armv7" and cs == 9) or (arch is None):
                return data[off : off + size]
    return data


def parse_sections(m: bytes) -> dict:
    ncmds = struct.unpack("<I", m[16:20])[0]
    off = 28
    secs = {}
    for _ in range(ncmds):
        cmd, sz = struct.unpack("<II", m[off : off + 8])
        if cmd == 1:  # LC_SEGMENT
            nsects = struct.unpack("<I", m[off + 48 : off + 52])[0]
            so = off + 56
            for _ in range(nsects):
                sn = m[so : so + 16].rstrip(b"\0").decode("latin1")
                sg = m[so + 16 : so + 32].rstrip(b"\0").decode("latin1")
                addr, size, foff = struct.unpack("<III", m[so + 32 : so + 44])
                secs[(sg, sn)] = (addr, size, foff)
                so += 68
        off += sz
    return secs


def cstr_at(m: bytes, secs: dict, vm: int) -> str | None:
    for (sg, sn), (a, s, f) in secs.items():
        if a <= vm < a + s and sn in ("__objc_methname", "__cstring"):
            e = m.find(b"\0", f + vm - a)
            if e != -1:
                return m[f + vm - a : e].decode("utf8", "replace")
    return None


def get_support_manifest(hvm_binary: str | None = None, manifest_path: str | None = None) -> dict:
    if manifest_path and Path(manifest_path).exists():
        return json.loads(Path(manifest_path).read_text())
    
    bin_path = hvm_binary or "build/hvm"
    if not Path(bin_path).exists():
        raise FileNotFoundError(f"HydrogenVM binary not found at {bin_path}; build it first")
    
    r = subprocess.run([bin_path, "--dump-support"], capture_output=True, text=True, check=True)
    return json.loads(r.stdout)


MAX_EXECUTABLE_BYTES = 512 * 1024 * 1024   # refuse a zip entry that would expand to more than this


def census_ipa(ipa_path: Path, manifest: dict, arch_override: str | None = None) -> dict:
    ipa_path = Path(ipa_path)
    if not ipa_path.exists():
        raise FileNotFoundError(f"IPA not found: {ipa_path}")
    
    file_hash = sha256_file(ipa_path)
    ipa_name = ipa_path.name
    
    fixture = FIXTURE_MANIFEST.get(ipa_name)
    if fixture:
        if file_hash != fixture["sha256"]:
            raise ValueError(
                f"SHA-256 hash mismatch for {ipa_name}: got {file_hash}, expected {fixture['sha256']}"
            )
        arch = arch_override or (None if fixture["thin"] else fixture["slice"])
    else:
        arch = arch_override or "armv7"

    with zipfile.ZipFile(ipa_path) as z:
        exec_path = None
        if fixture and fixture["payload_path"] in z.namelist():
            exec_path = fixture["payload_path"]
        else:
            # The executable is named by CFBundleExecutable in the app's Info.plist, not by zip order.
            for n in z.namelist():
                parts = n.split("/")
                if len(parts) == 3 and parts[0] == "Payload" and parts[1].endswith(".app") and parts[2] == "Info.plist":
                    info = plistlib.loads(z.read(n))
                    name = info.get("CFBundleExecutable")
                    if name and f"Payload/{parts[1]}/{name}" in z.namelist():
                        exec_path = f"Payload/{parts[1]}/{name}"
                    break
        if not exec_path:
            raise ValueError(f"Could not locate app executable in {ipa_path}")

        if z.getinfo(exec_path).file_size > MAX_EXECUTABLE_BYTES:
            raise ValueError(f"App executable in {ipa_path} is larger than {MAX_EXECUTABLE_BYTES} bytes")
        raw_bytes = z.read(exec_path)
        with tempfile.TemporaryDirectory() as tmpdir:
            bin_file = Path(tmpdir) / "app_bin"
            bin_file.write_bytes(raw_bytes)

            # llvm-nm
            cmd_nm = ["llvm-nm", "-m", "-u"]
            if arch and not (fixture and fixture["thin"]):
                cmd_nm += ["--arch", arch]
            cmd_nm.append(str(bin_file))
            p_nm = subprocess.run(cmd_nm, capture_output=True, text=True)
            if p_nm.returncode != 0:
                raise RuntimeError(f"llvm-nm failed (code {p_nm.returncode}): {p_nm.stderr}")
            
            # llvm-objdump
            cmd_obj = ["llvm-objdump", "--macho", "--objc-meta-data"]
            if arch and not (fixture and fixture["thin"]):
                cmd_obj += ["--arch", arch]
            cmd_obj.append(str(bin_file))
            p_obj = subprocess.run(cmd_obj, capture_output=True, text=True)
            if p_obj.returncode != 0:
                raise RuntimeError(f"llvm-objdump failed (code {p_obj.returncode}): {p_obj.stderr}")

    # Parse nm imports
    imports = []
    for line in p_nm.stdout.splitlines():
        m = re.match(r"\s+\(undefined[^)]*\) (?:weak )?external (\S+)", line)
        if m:
            imports.append(m.group(1))

    imported_non_objc = sum(1 for s in imports if not s.startswith("_OBJC_"))

    # Dynamic evaluation against manifest
    supported_symbols = {s["name"]: s for s in manifest.get("symbols", [])}
    supported_classes = {c["name"]: c for c in manifest.get("classes", [])}
    supported_selectors = {s["name"]: s for s in manifest.get("selectors", [])}

    m_slice = slice_bytes(raw_bytes, arch)
    secs = parse_sections(m_slice)
    selrefs = set()
    if ("__DATA", "__objc_selrefs") in secs:
        a, s, f = secs[("__DATA", "__objc_selrefs")]
        for i in range(s // 4):
            v = struct.unpack("<I", m_slice[f + 4 * i : f + 4 * i + 4])[0]
            n = cstr_at(m_slice, secs, v)
            if n:
                selrefs.add(n)

    defined = set(re.findall(r"\n\t\t\s+name 0x[0-9a-f]+ (\S+)\n\t\t\s+types", p_obj.stdout))

    miss = []
    for s in imports:
        if s.startswith("_OBJC_CLASS_$_"):
            cn = s.split("$_", 1)[1]
            c_info = supported_classes.get(cn)
            if not c_info or c_info.get("status") in ("Stub", "stub"):
                miss.append(("C", cn))
        elif s.startswith("_OBJC_"):
            pass
        else:
            if s not in supported_symbols:
                miss.append(("S", s))

    for s in selrefs:
        if s in defined:
            continue
        if s not in supported_selectors:
            miss.append(("M", s))

    unimp_syms = sum(1 for k, _ in miss if k == "S")
    miss_classes = sum(1 for k, _ in miss if k == "C")

    return {
        "ipa": ipa_name,
        "sha256": file_hash,
        "slice": arch or "armv7",
        "imported_non_objc_symbols": imported_non_objc,
        "unimplemented_symbols": unimp_syms,
        "missing_or_stubbed_classes": miss_classes,
        "missing_selectors_classes_constants": len(miss),
        "is_fixture": bool(fixture),
        # The recorded reference numbers (research_census). Computed values are always reported; the reference is
        # only for comparison, since the manifest changes as support is added.
        "reference": fixture["reference"] if fixture else None,
    }


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("ipa", nargs="?", help="Path to .ipa file")
    p.add_argument("--all", action="store_true", help="Run census on all fixture IPAs in repo root")
    p.add_argument("--hvm", default="build/hvm", help="Path to hvm binary (default: build/hvm)")
    p.add_argument("--manifest", help="Path to pre-dumped support manifest JSON")
    p.add_argument("--json", action="store_true", help="Output results as JSON")
    args = p.parse_args(argv)

    try:
        manifest = get_support_manifest(args.hvm, args.manifest)
    except Exception as e:
        sys.exit(f"Failed to load support manifest: {e}")

    results = []
    if args.all or not args.ipa:
        repo_root = Path(__file__).resolve().parent.parent
        for ipa_name in FIXTURE_MANIFEST:
            ipa_path = repo_root / ipa_name
            if not ipa_path.exists():
                sys.exit(f"Fixture IPA missing: {ipa_path}")
            res = census_ipa(ipa_path, manifest)
            results.append(res)
    else:
        results.append(census_ipa(Path(args.ipa), manifest))

    if args.json:
        print(json.dumps(results, indent=2))
        return 0

    print(f"{'IPA':<32} {'Non-ObjC':<10} {'Unimp Sym':<11} {'Missing Cls':<13} {'Total Gaps':<12}")
    print("-" * 80)
    for r in results:
        print(
            f"{r['ipa']:<32} {r['imported_non_objc_symbols']:<10} {r['unimplemented_symbols']:<11} "
            f"{r['missing_or_stubbed_classes']:<13} {r['missing_selectors_classes_constants']:<12}"
        )

    return 0


if __name__ == "__main__":
    sys.exit(main())
