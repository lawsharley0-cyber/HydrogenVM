#!/usr/bin/env python3
"""Build 32-bit iOS test apps without Xcode.

Compiles sources with clang (--target=armv7-apple-iosX), links them with
Apple's real ld64 (cctools-port, built for Linux) against a stub SDK, and
packages .app bundles and .ipa files. The stub SDK's dylibs only export
symbol names — exactly what an app binds against — so the binaries look like
real App Store builds to the loader (dyld info or classic binds, LC_MAIN or
LC_UNIXTHREAD, ObjC metadata, __cfstring, etc).

usage:
  tools/ios_build.py app --name Foo --out build/ios [--min-ios 6.0] [--no-pie]
                         [--plist-extra key=value ...] [--resource path ...] src.m src.c ...
"""
import argparse, os, plistlib, re, shutil, subprocess, sys, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LD = os.environ.get("HVM_LD64", "/opt/cctools/bin/arm-apple-darwin11-ld")
CLANG = os.environ.get("HVM_CLANG", "clang")
NM = os.environ.get("HVM_NM", "llvm-nm")
SDK_SRC = os.path.join(ROOT, "tests", "ios", "sdk")
HEADERS = os.path.join(ROOT, "tests", "ios", "include")

# Which stub library a symbol lives in (only affects the load commands and
# dylib ordinals; HydrogenVM resolves imports by name).
LIBS = [
    ("/usr/lib/libobjc.A.dylib", re.compile(r"^(_objc_|_OBJC_(META)?CLASS_\$_NSObject$|__objc_|_sel_|_class_|_object_|_protocol_|_ivar_|_method_|_property_|_imp_)")),
    ("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", re.compile(r"^(_CF|_kCF|___CF)")),
    ("/System/Library/Frameworks/UIKit.framework/UIKit", re.compile(r"^(_OBJC_(META)?CLASS_\$_UI|_UI|_OBJC_IVAR_\$_UI)")),
    ("/System/Library/Frameworks/QuartzCore.framework/QuartzCore", re.compile(r"^(_OBJC_(META)?CLASS_\$_CA|_CA|_kCA)")),
    ("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", re.compile(r"^(_CG|_kCG)")),
    ("/System/Library/Frameworks/OpenGLES.framework/OpenGLES", re.compile(r"^(_gl|_OBJC_(META)?CLASS_\$_EAGL|_kEAGL)")),
    ("/System/Library/Frameworks/Foundation.framework/Foundation", re.compile(r"^(_OBJC_(META)?CLASS_\$_NS|_NS|__NS)")),
    ("/usr/lib/libstdc++.6.dylib", re.compile(r"^(__Z|___cxa|___gxx|__Unwind|___dynamic_cast)")),
    ("/usr/lib/libSystem.B.dylib", re.compile(r".")),
]

def run(cmd, **kw):
    p = subprocess.run(cmd, capture_output=True, text=True, **kw)
    err = "\n".join(l for l in (p.stderr or "").splitlines() if "directory not found for option" not in l and "was built for newer iOS version" not in l)
    if p.returncode != 0:
        sys.stderr.write(" ".join(cmd) + "\n" + err + "\n" + (p.stdout or ""))
        raise SystemExit(1)
    if err.strip():
        sys.stderr.write(err + "\n")
    return p.stdout

def compile_sources(srcs, objdir, min_ios, extra_cflags):
    objs = []
    for s in srcs:
        o = os.path.join(objdir, os.path.basename(s) + ".o")
        flags = [CLANG, f"--target=armv7-apple-ios{min_ios}", "-isystem", HEADERS, "-O1", "-g0",
                 "-fno-stack-check", "-Wno-objc-root-class", "-Wno-incompatible-library-redeclaration",
                 "-Wno-builtin-requires-header", "-mfloat-abi=softfp", "-mfpu=neon", "-c", s, "-o", o]
        if s.endswith(".m") or s.endswith(".mm"):
            flags.insert(1, "-fobjc-arc" if "-fno-objc-arc" not in extra_cflags else "-fno-objc-arc")
        if s.endswith(".mm") or s.endswith(".cpp"):
            flags.insert(1, "-std=c++11")
        flags += [f for f in extra_cflags if f != "-fno-objc-arc"]
        run(flags)
        objs.append(o)
    return objs

def undefined_symbols(objs):
    out = run([NM, "-u", "--just-symbol-name"] + objs)
    syms = set()
    for l in out.splitlines():
        l = l.strip()
        if l and not l.endswith(":"):
            syms.add(l)
    return syms

def build_stub_sdk(sdk, syms, min_ios):
    """Create stub dylibs exporting `syms` (each as a small data object)."""
    by_lib = {}
    for s in sorted(syms):
        for lib, rx in LIBS:
            if rx.search(s):
                by_lib.setdefault(lib, []).append(s)
                break
    by_lib.setdefault("/usr/lib/libSystem.B.dylib", []).append("dyld_stub_binder")
    os.makedirs(os.path.join(sdk, "usr/lib/system"), exist_ok=True)
    tmp = os.path.join(sdk, "_src")
    os.makedirs(tmp, exist_ok=True)

    def make(install, symbols, extra_args=()):
        asm = os.path.join(tmp, re.sub(r"[^A-Za-z0-9]", "_", install) + ".s")
        with open(asm, "w") as f:
            f.write("\t.data\n")
            for s in symbols:
                f.write(f"\t.globl {s}\n{s}:\n\t.space 8\n")
        obj = asm + ".o"
        run([CLANG, f"--target=armv7-apple-ios{min_ios}", "-c", asm, "-o", obj])
        out = os.path.join(sdk, install.lstrip("/"))
        os.makedirs(os.path.dirname(out), exist_ok=True)
        run([LD, "-arch", "armv7", "-ios_version_min", min_ios, "-dylib", "-install_name", install,
             "-syslibroot", sdk, obj, "-o", out] + list(extra_args))
        return out

    kernel = make("/usr/lib/system/libsystem_kernel.dylib", by_lib.pop("/usr/lib/libSystem.B.dylib"))
    libsys = make("/usr/lib/libSystem.B.dylib", [], ["-reexport_library", kernel])
    for name in ("libSystem.dylib",):
        dst = os.path.join(sdk, "usr/lib", name)
        if os.path.lexists(dst):
            os.remove(dst)
        os.symlink("libSystem.B.dylib", dst)
    libs = []
    for install, symbols in by_lib.items():
        libs.append(make(install, symbols, ["-lSystem"]))
    return libs

def link(objs, out, sdk, libs, min_ios, pie, legacy):
    args = [LD, "-arch", "armv7", "-ios_version_min", min_ios, "-syslibroot", sdk, "-o", out]
    if legacy:
        crt = os.path.join(sdk, "_src", "crt1.o")
        run([CLANG, "--target=armv7-apple-ios3.0", "-c", os.path.join(SDK_SRC, "crt1.s"), "-o", crt])
        args += [crt, "-e", "start"]
    if pie:
        args.append("-pie")
    elif float(min_ios) >= 4.3:
        args.append("-no_pie")
    args += objs + libs + ["-lSystem"]
    run(args)

def build_app(a):
    out = os.path.abspath(a.out)
    work = os.path.join(out, "_work", a.name)
    if os.path.exists(work):
        shutil.rmtree(work)
    os.makedirs(work)
    objs = compile_sources(a.sources, work, a.min_ios, a.cflags or [])
    legacy = float(a.min_ios) < 4.3
    syms = undefined_symbols(objs)
    if legacy:
        syms |= {"_exit"}
    sdk = os.path.join(work, "sdk")
    libs = build_stub_sdk(sdk, syms, "6.0" if not legacy else "4.0")
    app = os.path.join(out, a.name + ".app")
    if os.path.exists(app):
        shutil.rmtree(app)
    os.makedirs(app)
    exe = os.path.join(app, a.name)
    link(objs, exe, sdk, libs, a.min_ios, not a.no_pie, legacy)
    info = {
        "CFBundleExecutable": a.name,
        "CFBundleIdentifier": f"com.hydrogenvm.test.{a.name.lower()}",
        "CFBundleName": a.name,
        "CFBundleDisplayName": a.display or a.name,
        "CFBundleVersion": "1.0",
        "CFBundleShortVersionString": "1.0",
        "CFBundlePackageType": "APPL",
        "MinimumOSVersion": a.min_ios,
        "UIDeviceFamily": [1],
    }
    for kv in a.plist_extra or []:
        k, v = kv.split("=", 1)
        info[k] = v
    with open(os.path.join(app, "Info.plist"), "wb") as f:
        plistlib.dump(info, f, fmt=plistlib.FMT_BINARY if a.binary_plist else plistlib.FMT_XML)
    for r in a.resource or []:
        dst = os.path.join(app, os.path.basename(r))
        (shutil.copytree if os.path.isdir(r) else shutil.copy)(r, dst)
    ipa = os.path.join(out, a.name + ".ipa")
    with zipfile.ZipFile(ipa, "w", zipfile.ZIP_DEFLATED) as z:
        for dp, _, files in os.walk(app):
            for fn in files:
                full = os.path.join(dp, fn)
                z.write(full, os.path.join("Payload", os.path.relpath(full, out)))
    print(ipa)

def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("app")
    b.add_argument("--name", required=True)
    b.add_argument("--display")
    b.add_argument("--out", required=True)
    b.add_argument("--min-ios", default="6.0")
    b.add_argument("--no-pie", action="store_true")
    b.add_argument("--binary-plist", action="store_true")
    b.add_argument("--plist-extra", action="append")
    b.add_argument("--resource", action="append")
    b.add_argument("--cflags", action="append")
    b.add_argument("sources", nargs="+")
    a = ap.parse_args()
    if a.cmd == "app":
        build_app(a)

if __name__ == "__main__":
    main()
