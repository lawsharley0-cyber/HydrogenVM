#!/usr/bin/env python3
"""Linux validation: Test Lab A/A equality and an actual planted runtime bug.

Only generated files under --out are changed. The planted build's NSBundle
infoDictionary implementation returns nil for every guest. Production sources,
the supplied original build, and the Test Lab guest remain unchanged.
"""
import argparse
import json
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import ab_diff


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build", required=True, type=Path)
    ap.add_argument("--guest", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    args = ap.parse_args()
    build, out = args.build.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    commands = subprocess.check_output(["ninja", "-C", str(build), "-t", "commands", "hvm"], text=True).splitlines()
    compile_argv = shlex.split(next(c for c in commands if " -c " in c and "/foundation/Bundle.cpp" in c))
    source = Path(compile_argv[compile_argv.index("-c") + 1])
    copied_source = out / "Bundle-regression.cpp"
    text = source.read_text()
    marker = "id b_infoDictionary(Ctx& x, id self, SEL) {"
    if text.count(marker) != 1:
        raise RuntimeError("cannot locate unique mutation point")
    copied_source.write_text(text.replace(marker, marker + "\n    return 0; // planted regression, generated test build only"))
    object_path = out / "Bundle.cpp.o"
    compile_argv[compile_argv.index("-c") + 1] = str(copied_source)
    compile_argv[compile_argv.index("-o") + 1] = str(object_path)
    compile_argv += ["-I", str(source.parent)]
    subprocess.run(compile_argv, cwd=build, check=True)
    archive = out / "libhvm_core.a"
    shutil.copyfile(build / "libhvm_core.a", archive)
    subprocess.run(["ar", "r", str(archive), str(object_path)], check=True)
    link = shlex.split(commands[-1])
    if link[:2] != [":", "&&"] or link[-2:] != ["&&", ":"]:
        raise RuntimeError("unexpected Ninja link command")
    link = link[2:-2]
    link[link.index("-o") + 1] = str(out / "hvm")
    link[link.index("libhvm_core.a")] = str(archive)
    subprocess.run(link, cwd=build, check=True)
    common = ["--guest", str(args.guest.resolve()), "--seconds", "3", "--timeout", "10",
              "--guest-arg=--run", "--guest-arg=TL-A"]
    equal = ab_diff.main([str(build), str(build), "--out", str(out / "identical"), *common])
    changed = ab_diff.main([str(build), str(out / "hvm"), "--out", str(out / "planted"), *common])
    old_log = (out / "planted/old/stdout.log").read_text()
    new_log = (out / "planted/new/stdout.log").read_text()
    ok = equal == 0 and changed == 1 and "TESTLAB TL-A passed" in old_log and "TESTLAB TL-A failed" in new_log
    (out / "validation.json").write_text(json.dumps({"identicalExit": equal, "plantedExit": changed,
                                                     "guestChangedFromPassToFail": "TESTLAB TL-A failed" in new_log,
                                                     "ok": ok}, indent=2) + "\n")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
