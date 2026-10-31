#!/usr/bin/env python3
"""Validate TL-S--TL-W against the original CLI and a generated faulty CLI.

Only copies under --out are changed. Five public API wrappers in a copied
project-owned Api.cpp are disabled, its object replaces a copied archive member,
and a separate CLI is linked. No production source/build or Apple source changes.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from hvm_testlab import OBJC_CASE_IDS, run_case

FAULTS = {
    "TL-S": ("s_register", "return 0;", "registeredNonNull"),
    "TL-T": ("c_superclass", "return 0;", "childSuperclass"),
    "TL-U": ("m_getImplementation", "return 0;", "baseInstance"),
    "TL-V": ("c_addMethod", "return false;", "addInheritedOverride"),
    "TL-W": ("as_set", "return;", "associationKeepsValueAlive"),
}


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            h.update(chunk)
    return h.hexdigest()


def make_faulty_cli(build, out):
    commands = subprocess.check_output(["ninja", "-C", str(build), "-t", "commands", "hvm"], text=True).splitlines()
    compile_args = shlex.split(next(c for c in commands if " -c " in c and "/objc/Api.cpp" in c))
    source = Path(compile_args[compile_args.index("-c") + 1])
    generated = source.read_text()
    for function, body, _ in FAULTS.values():
        pattern = rf"(?m)^((?:SEL|Class|IMP|bool|void) {function}\([^{{}}]*\)\s*\{{)"
        generated, count = re.subn(pattern, lambda m: m[1] + " " + body + " /* generated fault */", generated)
        if count != 1:
            raise RuntimeError(f"expected one mutation point for {function}, found {count}")
    copy = out / "Api-faults.cpp"
    copy.write_text(generated)
    original_object = build / compile_args[compile_args.index("-o") + 1]
    original_member = original_object.name
    protected = [original_object, build / "libhvm_core.a", build / "hvm"]
    if "-MF" in compile_args:
        protected.append(build / compile_args[compile_args.index("-MF") + 1])
    before = {str(p): digest(p) if p.exists() else None for p in protected}
    object_path = out / original_member
    compile_args[compile_args.index("-c") + 1] = str(copy)
    compile_args[compile_args.index("-o") + 1] = str(object_path)
    # CMake's dependency flags are independent of -o. Redirect those too;
    # otherwise a direct compiler invocation can overwrite the real build's .d.
    if "-MF" in compile_args:
        compile_args[compile_args.index("-MF") + 1] = str(out / (original_member + ".d"))
    if "-MT" in compile_args:
        compile_args[compile_args.index("-MT") + 1] = str(object_path)
    compile_args += ["-I", str(source.parent)]
    subprocess.run(compile_args, cwd=build, check=True)
    archive = out / "libhvm_core.a"
    shutil.copyfile(build / "libhvm_core.a", archive)
    members = subprocess.check_output(["ar", "t", str(archive)], text=True).splitlines()
    if members.count(original_member) != 1:
        raise RuntimeError("archive member is missing or ambiguous")
    subprocess.run(["ar", "r", str(archive), str(object_path)], check=True)
    link_args = shlex.split(commands[-1])
    if link_args[:2] != [":", "&&"] or link_args[-2:] != ["&&", ":"]:
        raise RuntimeError("unexpected Ninja link command")
    link_args = link_args[2:-2]
    cli = out / "hvm-faults"
    link_args[link_args.index("-o") + 1] = str(cli)
    link_args[link_args.index("libhvm_core.a")] = str(archive)
    subprocess.run(link_args, cwd=build, check=True)
    after = {str(p): digest(p) if p.exists() else None for p in protected}
    if before != after:
        raise RuntimeError("fault validation changed the original build")
    return cli, digest(source), before == after


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build", required=True, type=Path)
    ap.add_argument("--ipa", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    args = ap.parse_args()
    build, ipa, out = args.build.resolve(), args.ipa.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    rows = []
    for case_id in OBJC_CASE_IDS:
        baseline = run_case(str(build / "hvm"), str(ipa), case_id, str(out / "original" / case_id), 15)
        rows.append({"id": case_id, "baseline": baseline})
    if any(r["baseline"]["status"] != "passed" for r in rows):
        (out / "report.json").write_text(json.dumps({"cases": rows, "summary": {"ok": False, "reason": "baseline not green"}}, indent=2))
        return 1
    faulty, source_sha, build_unchanged = make_faulty_cli(build, out)
    for row in rows:
        case_id = row["id"]
        result = run_case(str(faulty), str(ipa), case_id, str(out / "faulty" / case_id), 15)
        function, _, check = FAULTS[case_id]
        caught = (result["status"] == "failed" and result.get("diagnostics", {}).get("hvmExitCode") == 1
                  and check in result.get("actual", {}) and check in result.get("expected", {})
                  and result["actual"][check] != result["expected"][check])
        row.update(faulty=result, disabledFunction=function, requiredFailingCheck=check, caught=caught)
        print(f"{case_id}: original passed; faulty {result['status']}; detected={caught}", flush=True)
    ok = all(r["caught"] for r in rows)
    report = {"schemaVersion": 1, "kind": "hvm-objc-conformance-mutations",
              "host": {"system": platform.system(), "machine": platform.machine(), "pageSize": os.sysconf("SC_PAGE_SIZE")},
              "ipaSha256": digest(ipa), "originalBinarySha256": digest(build / "hvm"),
              "faultyBinarySha256": digest(faulty), "sourceSha256": source_sha,
              "originalBuildPreserved": build_unchanged,
              "cases": rows, "summary": {"ok": ok, "originalPassed": len(rows),
                                         "faultsDetected": sum(r["caught"] for r in rows)}}
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
