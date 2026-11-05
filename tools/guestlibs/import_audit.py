#!/usr/bin/env python3
"""Audit undefined libSystem.B.dylib imports of the guest C++ runtime dylibs (S4).

Reads undefined symbols from the built guest C++ runtime dylibs
(libgcc_s.1.dylib, libstdc++.6.dylib, libc++abi.dylib, libc++.1.dylib) — either
from Mach-O binaries in --dylib-dir or from the deterministic reference manifest
matching tools/guestlibs/build_guestlibs.py — and cross-checks each symbol
against HydrogenVM's symbol registration tables and supportDef annotations in
src/runtime/.

Each symbol is classified as:
  - present             : Registered with a real or honest-approx implementation
  - stubbed-dishonestly : Registered as SupportStatus::Stub / Unavailable, or
                          bound to a no-op constant-return helper (s_zero, m_ok2,
                          m_ok3, s_ok2, k_zero4) or returning a hardcoded C locale
                          pointer without implementing the contract
  - missing             : Not registered in HydrogenVM's symbol table
"""

from __future__ import annotations

import argparse
import json
import re
import struct
import subprocess
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(REPO_ROOT / "tools"))
import lint_stubs


def constant_stub_handlers(source: str) -> set[str]:
    ts = lint_stubs.tokens(source)
    pairs = lint_stubs.pairs(ts)
    result = set()
    for i in range(len(ts) - 2):
        if ts[i].kind != "word" or ts[i + 1].value != "(":
            continue
        end = pairs.get(i + 1)
        if end is None or end + 1 >= len(ts) or ts[end + 1].value != "{":
            continue
        close = pairs.get(end + 1)
        if close is None:
            continue
        body = ts[end + 2:close]
        code = " ".join(t.value for t in body)
        if not body or re.fullmatch(r"return (?:0|1|true|YES|0u|1u) ;", code):
            result.add(ts[i].value)
    return result


MH_MAGIC = 0xFEEDFACE
LC_SYMTAB = 0x2
LC_LOAD_DYLIB = 0xC
LC_LOAD_WEAK_DYLIB = 0x80000018
LC_REEXPORT_DYLIB = 0x8000001f
LC_LAZY_LOAD_DYLIB = 0x20
LC_LOAD_UPWARD_DYLIB = 0x80000023
N_EXT = 0x01
N_TYPE = 0x0E
N_UNDF = 0x0

DEFAULT_LIBRARY_IMPORTS: dict[str, list[str]] = {
    "libgcc_s.1.dylib": [
        "___error",
        "_abort",
        "_calloc",
        "_dladdr",
        "_free",
        "_malloc",
        "_memcpy",
        "_memset",
        "_pthread_getspecific",
        "_pthread_key_create",
        "_pthread_mutex_lock",
        "_pthread_mutex_unlock",
        "_pthread_once",
        "_pthread_setspecific",
    ],
    "libstdc++.6.dylib": [
        "___error",
        "___mb_cur_max",
        "___stderrp",
        "___stdinp",
        "___stdoutp",
        "_abort",
        "_btowc",
        "_calloc",
        "_close",
        "_fflush",
        "_fgetc",
        "_fputc",
        "_fread",
        "_free",
        "_fseek",
        "_ftell",
        "_fwrite",
        "_getc",
        "_localeconv",
        "_lseek",
        "_malloc",
        "_mbrtowc",
        "_memchr",
        "_memcmp",
        "_memcpy",
        "_memmove",
        "_memset",
        "_open",
        "_poll",
        "_pthread_cond_broadcast",
        "_pthread_cond_destroy",
        "_pthread_cond_init",
        "_pthread_cond_signal",
        "_pthread_cond_timedwait",
        "_pthread_cond_wait",
        "_pthread_getspecific",
        "_pthread_key_create",
        "_pthread_key_delete",
        "_pthread_mutex_destroy",
        "_pthread_mutex_init",
        "_pthread_mutex_lock",
        "_pthread_mutex_trylock",
        "_pthread_mutex_unlock",
        "_pthread_once",
        "_pthread_setspecific",
        "_putc",
        "_read",
        "_realloc",
        "_setlocale",
        "_snprintf",
        "_sprintf",
        "_sscanf",
        "_strchr",
        "_strcmp",
        "_strcoll",
        "_strcpy",
        "_strerror",
        "_strftime",
        "_strlen",
        "_strncmp",
        "_strtod",
        "_strtof",
        "_strtol",
        "_strtoll",
        "_strtoul",
        "_strtoull",
        "_strxfrm",
        "_ungetc",
        "_wcrtomb",
        "_wcscmp",
        "_wcslen",
        "_wmemchr",
        "_wmemcmp",
        "_wmemcpy",
        "_wmemmove",
        "_wmemset",
        "_write",
    ],
    "libc++abi.dylib": [
        "___cxa_atexit",
        "___error",
        "___stderrp",
        "_abort",
        "_aligned_alloc",
        "_calloc",
        "_dladdr",
        "_dyld_register_func_for_add_image",
        "_fprintf",
        "_fputs",
        "_free",
        "_fwrite",
        "_malloc",
        "_memcmp",
        "_memcpy",
        "_memmove",
        "_memset",
        "_posix_memalign",
        "_pthread_cond_broadcast",
        "_pthread_cond_wait",
        "_pthread_getspecific",
        "_pthread_key_create",
        "_pthread_mutex_init",
        "_pthread_mutex_lock",
        "_pthread_mutex_unlock",
        "_pthread_once",
        "_pthread_self",
        "_pthread_setspecific",
        "_realloc",
        "_sched_yield",
        "_snprintf",
        "_strcmp",
        "_strlen",
        "_strncmp",
        "_vfprintf",
    ],
    "libc++.1.dylib": [
        "___cxa_atexit",
        "___error",
        "___mb_cur_max",
        "___stderrp",
        "___stdinp",
        "___stdoutp",
        "_abort",
        "_asprintf",
        "_btowc",
        "_calloc",
        "_catclose",
        "_catgets",
        "_catopen",
        "_clock_gettime",
        "_close",
        "_duplocale",
        "_fflush",
        "_fgetc",
        "_fputc",
        "_fread",
        "_free",
        "_freelocale",
        "_fseek",
        "_fseeko",
        "_ftell",
        "_ftello",
        "_fwrite",
        "_getc",
        "_gettimeofday",
        "_isalnum_l",
        "_isalpha_l",
        "_isblank_l",
        "_iscntrl_l",
        "_isdigit_l",
        "_isgraph_l",
        "_islower_l",
        "_isprint_l",
        "_ispunct_l",
        "_isspace_l",
        "_isupper_l",
        "_iswalpha_l",
        "_iswblank_l",
        "_iswcntrl_l",
        "_iswdigit_l",
        "_iswgraph_l",
        "_iswlower_l",
        "_iswprint_l",
        "_iswpunct_l",
        "_iswspace_l",
        "_iswupper_l",
        "_iswxdigit_l",
        "_isxdigit_l",
        "_localeconv",
        "_localeconv_l",
        "_lseek",
        "_mach_absolute_time",
        "_mach_timebase_info",
        "_malloc",
        "_mbrlen",
        "_mbrtowc",
        "_mbsnrtowcs",
        "_mbsrtowcs",
        "_mbtowc",
        "_memchr",
        "_memcmp",
        "_memcpy",
        "_memmove",
        "_memset",
        "_nanosleep",
        "_newlocale",
        "_open",
        "_posix_memalign",
        "_pthread_cond_broadcast",
        "_pthread_cond_destroy",
        "_pthread_cond_init",
        "_pthread_cond_signal",
        "_pthread_cond_timedwait",
        "_pthread_cond_wait",
        "_pthread_detach",
        "_pthread_equal",
        "_pthread_getspecific",
        "_pthread_join",
        "_pthread_key_create",
        "_pthread_key_delete",
        "_pthread_mutex_destroy",
        "_pthread_mutex_init",
        "_pthread_mutex_lock",
        "_pthread_mutex_trylock",
        "_pthread_mutex_unlock",
        "_pthread_mutexattr_destroy",
        "_pthread_mutexattr_init",
        "_pthread_mutexattr_settype",
        "_pthread_once",
        "_pthread_self",
        "_pthread_setspecific",
        "_putc",
        "_read",
        "_realloc",
        "_sched_yield",
        "_setlocale",
        "_snprintf",
        "_snprintf_l",
        "_sscanf",
        "_sscanf_l",
        "_stat",
        "_strchr",
        "_strcmp",
        "_strcoll",
        "_strcoll_l",
        "_strcpy",
        "_strerror",
        "_strerror_r",
        "_strftime",
        "_strftime_l",
        "_strlen",
        "_strncmp",
        "_strtod",
        "_strtod_l",
        "_strtof",
        "_strtof_l",
        "_strtold",
        "_strtold_l",
        "_strtoll",
        "_strtoll_l",
        "_strtoull",
        "_strtoull_l",
        "_strxfrm",
        "_strxfrm_l",
        "_sysconf",
        "_tolower_l",
        "_toupper_l",
        "_towlower_l",
        "_towupper_l",
        "_ungetc",
        "_uselocale",
        "_vasprintf",
        "_vsnprintf",
        "_vsscanf",
        "_wcrtomb",
        "_wcscmp",
        "_wcscoll",
        "_wcscoll_l",
        "_wcslen",
        "_wcsnrtombs",
        "_wcsrtombs",
        "_wcstod",
        "_wcstof",
        "_wcstold",
        "_wcstoll",
        "_wcstoull",
        "_wcsxfrm",
        "_wcsxfrm_l",
        "_wctob",
        "_wmemchr",
        "_wmemcmp",
        "_wmemcpy",
        "_wmemmove",
        "_wmemset",
        "_write",
    ],
}

# Symbols inside libstdc++.6.dylib / libc++.1.dylib that are resolved by their peer C++
# ABI dylib (libgcc_s.1.dylib / libc++abi.dylib) rather than libSystem.B.dylib when a
# flat-namespace or ordinal-0 symbol table is inspected.
PEER_CXX_EXPORTS: set[str] = {
    "__Unwind_Resume",
    "__Unwind_SjLj_Register",
    "__Unwind_SjLj_Resume",
    "__Unwind_SjLj_Unregister",
    "__Unwind_DeleteException",
    "___cxa_allocate_exception",
    "___cxa_begin_catch",
    "___cxa_end_catch",
    "___cxa_free_exception",
    "___cxa_guard_abort",
    "___cxa_guard_acquire",
    "___cxa_guard_release",
    "___cxa_pure_virtual",
    "___cxa_rethrow",
    "___cxa_throw",
    "___gxx_personality_sj0",
}

# Known stubbed-dishonestly handlers in HydrogenVM's libc layer.
KNOWN_STUB_HANDLERS: set[str] = {
    "s_zero",
    "s_ok2",
    "m_ok2",
    "m_ok3",
    "k_zero4",
}

# Handler-verified override for registered functions whose body returns a fixed C-locale string.
HANDLER_OVERRIDES: dict[str, tuple[str, str, str]] = {
    "_setlocale": (
        "p_setlocale",
        "stubbed-dishonestly",
        'Handler `p_setlocale` returns fixed `"C"` string regardless of requested category/locale',
    ),
}

_CODE_TOKEN_RE = re.compile(r'"(?:\\.|[^"\\\n])*"|//[^\n]*|/\*.*?\*/', re.S)


def _strip_comments(text: str) -> str:
    """Replace C/C++ comment contents with spaces while preserving string literals and line numbers."""

    def repl(m: re.Match[str]) -> str:
        s = m.group(0)
        return s if s.startswith('"') else re.sub(r"[^\n]", " ", s)

    return _CODE_TOKEN_RE.sub(repl, text)


def parse_macho_undef_symbols(dylib_path: Path) -> list[str]:
    """Read undefined external libSystem symbols from a 32-bit ARM Mach-O dylib."""
    data = dylib_path.read_bytes()
    if len(data) < 28:
        raise ValueError(f"{dylib_path}: file too small for Mach-O header")
    magic, _cputype, _cpusubtype, _filetype, ncmds, sizeofcmds, _flags = struct.unpack_from(
        "<IIIIIII", data, 0
    )
    if magic != MH_MAGIC:
        raise ValueError(f"{dylib_path}: unexpected magic 0x{magic:08x}")
    if 28 + sizeofcmds > len(data):
        raise ValueError(f"{dylib_path}: load commands exceed file size")

    # First pass: collect 1-based LC_LOAD_DYLIB paths so we can distinguish libSystem from peer dylibs
    loaded_dylibs: list[str] = []
    off = 28
    for _ in range(ncmds):
        if off + 8 > len(data):
            raise ValueError(f"{dylib_path}: truncated load command at offset {off}")
        cmd, cmdsize = struct.unpack_from("<II", data, off)
        if cmdsize < 8 or off + cmdsize > len(data):
            raise ValueError(f"{dylib_path}: invalid load command size {cmdsize} at offset {off}")
        if cmd in (LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB, LC_REEXPORT_DYLIB, LC_LAZY_LOAD_DYLIB, LC_LOAD_UPWARD_DYLIB):
            if cmdsize < 24:
                raise ValueError(f"{dylib_path}: truncated dylib load command")
            str_off = struct.unpack_from("<I", data, off + 8)[0]
            if str_off >= cmdsize:
                raise ValueError(f"{dylib_path}: invalid dylib name offset")
            raw = data[off + str_off : off + cmdsize]
            end = raw.find(b"\x00")
            if end < 0:
                raise ValueError(f"{dylib_path}: unterminated dylib install name")
            loaded_dylibs.append(raw[:end].decode("utf-8", errors="replace"))
        off += cmdsize

    libsystem_ordinals: set[int] = {
        idx + 1
        for idx, dep in enumerate(loaded_dylibs)
        if Path(dep).name in ("libSystem.B.dylib", "libSystem.dylib")
    }

    # Second pass: read LC_SYMTAB undefined external symbols
    off = 28
    undefs: list[str] = []
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", data, off)
        if cmd == LC_SYMTAB:
            if cmdsize < 24:
                raise ValueError(f"{dylib_path}: truncated LC_SYMTAB command")
            symoff, nsyms, stroff, strsize = struct.unpack_from("<IIII", data, off + 8)
            if symoff + nsyms * 12 > len(data) or stroff + strsize > len(data):
                raise ValueError(f"{dylib_path}: LC_SYMTAB exceeds file size")
            for i in range(nsyms):
                n_strx, n_type, _n_sect, n_desc, _n_value = struct.unpack_from(
                    "<IBBHI", data, symoff + i * 12
                )
                if (n_type & N_EXT) and ((n_type & N_TYPE) == N_UNDF):
                    if n_strx >= strsize:
                        raise ValueError(f"{dylib_path}: symbol string offset out of range")
                    end = data.find(b"\x00", stroff + n_strx, stroff + strsize)
                    if end < 0:
                        raise ValueError(f"{dylib_path}: unterminated symbol name")
                    sym = data[stroff + n_strx : end].decode("utf-8", errors="replace")
                    if not sym or sym == "dyld_stub_binder":
                        continue
                    ordinal = (n_desc >> 8) & 0xFF
                    if libsystem_ordinals and ordinal != 0:
                        if ordinal not in libsystem_ordinals:
                            continue
                    elif sym in PEER_CXX_EXPORTS or sym.startswith("__Z"):
                        continue
                    undefs.append(sym)
        off += cmdsize
    return sorted(set(undefs))


def scan_hvm_registrations(repo_root: Path) -> dict[str, dict[str, str]]:
    """Scan src/runtime/ for registered symbols and their implementation classification."""
    runtime_dir = repo_root / "src" / "runtime"
    registrations: dict[str, dict[str, str]] = {}

    def apply_override(sym: str, fn_name: str, status: str, note: str) -> tuple[str, str]:
        override = HANDLER_OVERRIDES.get(sym)
        if override and override[0] == fn_name:
            return override[1], override[2]
        return status, note

    # 1. Match p.def<&fn>("_sym")
    def_re = re.compile(r'p\.def<&\s*([A-Za-z0-9_:]+)\s*>\(\s*"([^"]+)"\s*\)')
    # 2. Match for (const char* n : {"_a", "_b"}) p.def<&fn>(n)
    loop_def_re = re.compile(
        r'for\s*\(\s*const\s+char\*\s+\w+\s*:\s*\{([^}]+)\}\s*\)\s*p\.def<&\s*([A-Za-z0-9_:]+)\s*>\(\s*\w+\s*\)'
    )
    # 3. Match supportDef<&fn>(p, "_sym", SupportStatus::Kind, "note")
    support_def_re = re.compile(
        r'supportDef<&\s*([A-Za-z0-9_:]+)\s*>\(\s*p\s*,\s*"([^"]+)"\s*,\s*SupportStatus::(Real|Approx|Stub|Unavailable)\s*,\s*"([^"]*)"'
    )
    # 4. Match p.defineData("_sym", ...) or p.defineDataBlock("_sym", ...)
    data_re = re.compile(r'p\.defineData(?:Block)?\(\s*"([^"]+)"')
    # 5. Match cellOf(x, "...", ...) or stdio stream setup in Stdio.cpp
    stdio_re = re.compile(r'"(___std(?:in|out|err)p|___mb_cur_max)"')

    for cpp_path in sorted(runtime_dir.rglob("*.cpp")):
        rel_path = cpp_path.relative_to(repo_root).as_posix()
        text = _strip_comments(cpp_path.read_text(encoding="utf-8", errors="replace"))

        constant_stubs = constant_stub_handlers(text)

        for m in def_re.finditer(text):
            fn_name, sym = m.group(1), m.group(2)
            line_no = text.count("\n", 0, m.start()) + 1
            status = "stubbed-dishonestly" if fn_name in (KNOWN_STUB_HANDLERS | constant_stubs) else "present"
            note = f"Handler `{fn_name}`"
            status, note = apply_override(sym, fn_name, status, note)
            registrations[sym] = {
                "status": status,
                "source": f"{rel_path}:{line_no}",
                "handler": fn_name,
                "note": note,
            }

        for m in loop_def_re.finditer(text):
            names_blob, fn_name = m.group(1), m.group(2)
            line_no = text.count("\n", 0, m.start()) + 1
            for sym_m in re.finditer(r'"([^"]+)"', names_blob):
                sym = sym_m.group(1)
                status = "stubbed-dishonestly" if fn_name in (KNOWN_STUB_HANDLERS | constant_stubs) else "present"
                note = f"Handler `{fn_name}`"
                status, note = apply_override(sym, fn_name, status, note)
                registrations[sym] = {
                    "status": status,
                    "source": f"{rel_path}:{line_no}",
                    "handler": fn_name,
                    "note": note,
                }

        for m in support_def_re.finditer(text):
            fn_name, sym, sup_status, sup_note = m.group(1), m.group(2), m.group(3), m.group(4)
            line_no = text.count("\n", 0, m.start()) + 1
            if sup_status in ("Stub", "Unavailable") or fn_name in (KNOWN_STUB_HANDLERS | constant_stubs):
                status = "stubbed-dishonestly"
            else:
                status = "present"
            note = f"`SupportStatus::{sup_status}` — {sup_note}"
            status, note = apply_override(sym, fn_name, status, note)
            registrations[sym] = {
                "status": status,
                "source": f"{rel_path}:{line_no}",
                "handler": fn_name,
                "note": note,
            }

        for m in data_re.finditer(text):
            sym = m.group(1)
            line_no = text.count("\n", 0, m.start()) + 1
            registrations.setdefault(
                sym,
                {
                    "status": "present",
                    "source": f"{rel_path}:{line_no}",
                    "handler": "defineData",
                    "note": "Guest data symbol",
                },
            )

        for m in stdio_re.finditer(text):
            sym = m.group(1)
            line_no = text.count("\n", 0, m.start()) + 1
            registrations.setdefault(
                sym,
                {
                    "status": "present",
                    "source": f"{rel_path}:{line_no}",
                    "handler": "defineData",
                    "note": "Guest global data cell",
                },
            )

    return registrations


def run_audit(
    repo_root: Path = REPO_ROOT,
    dylib_dir: Path | None = None,
    hvm_bin: Path | None = None,
) -> dict[str, Any]:
    """Cross-check all 4 guest C++ runtime libraries' libSystem imports against HydrogenVM."""
    registrations = scan_hvm_registrations(repo_root)

    if hvm_bin is not None:
        if not hvm_bin.is_file():
            raise FileNotFoundError(f"--hvm-bin not found: {hvm_bin}")
        try:
            dump_raw = subprocess.check_output(
                [str(hvm_bin), "--dump-support"], text=True, timeout=60
            )
            dump = json.loads(dump_raw)
        except (subprocess.SubprocessError, json.JSONDecodeError) as exc:
            raise RuntimeError(f"Failed to query {hvm_bin} --dump-support: {exc}") from exc
        if not isinstance(dump, dict) or not isinstance(dump.get("symbols"), list):
            raise RuntimeError("Malformed --dump-support payload: expected object with symbols array")
        for entry in dump["symbols"]:
            if (not isinstance(entry, dict) or not isinstance(entry.get("name"), str)
                    or not entry["name"] or entry.get("status") not in ("Real", "Approx", "Stub", "Unavailable")):
                raise RuntimeError("Malformed --dump-support symbol: expected name and valid status")
            sym = entry["name"]
            sup_status = entry.get("status", "Real")
            if sym not in registrations or sup_status in ("Stub", "Unavailable"):
                registrations[sym] = {
                    "status": "stubbed-dishonestly"
                    if sup_status in ("Stub", "Unavailable")
                    else "present",
                    "source": "hvm --dump-support",
                    "handler": entry.get("kind", "function"),
                    "note": entry.get("note", f"SupportStatus::{sup_status}"),
                }

    import_source = "macho" if dylib_dir is not None else "manifest"
    lib_imports: dict[str, list[str]] = {}
    for lib_name, default_list in DEFAULT_LIBRARY_IMPORTS.items():
        if dylib_dir is not None:
            path = dylib_dir / lib_name
            if not path.is_file():
                raise FileNotFoundError(f"{path} not found in --dylib-dir")
            lib_imports[lib_name] = parse_macho_undef_symbols(path)
        else:
            lib_imports[lib_name] = sorted(set(default_list))

    all_symbols: dict[str, dict[str, Any]] = {}
    for lib_name, syms in lib_imports.items():
        for sym in syms:
            entry = all_symbols.setdefault(
                sym,
                {
                    "importedBy": [],
                    "status": "missing",
                    "source": "—",
                    "note": "Not registered in HydrogenVM symbol tables",
                },
            )
            entry["importedBy"].append(lib_name)

    for sym, entry in all_symbols.items():
        reg = registrations.get(sym)
        if not reg and sym == "_dyld_register_func_for_add_image":
            reg = registrations.get("__dyld_register_func_for_add_image")
            if reg:
                reg = {
                    **reg,
                    "status": "missing",
                    "note": "Registered only under `__dyld_register_func_for_add_image` (2 leading underscores); single-underscore alias `_dyld_register_func_for_add_image` is missing (S5 scope)",
                }
        if reg:
            entry["status"] = reg["status"]
            entry["source"] = reg["source"]
            entry["note"] = reg["note"]

    libraries_summary: dict[str, dict[str, int]] = {}
    for lib_name, syms in lib_imports.items():
        counts = {"totalImports": len(syms), "present": 0, "missing": 0, "stubbed-dishonestly": 0}
        for sym in syms:
            st = all_symbols[sym]["status"]
            counts[st] += 1
        libraries_summary[lib_name] = counts

    totals = {"totalUniqueImports": len(all_symbols), "present": 0, "missing": 0, "stubbed-dishonestly": 0}
    for info in all_symbols.values():
        totals[info["status"]] += 1

    return {
        "importSource": import_source,
        "totals": totals,
        "libraries": libraries_summary,
        "symbols": dict(sorted(all_symbols.items())),
    }


def _symbols_for_lib_by_status(
    symbols: dict[str, dict[str, Any]], lib_name: str, status: str
) -> list[str]:
    return sorted(
        sym
        for sym, info in symbols.items()
        if lib_name in info["importedBy"] and info["status"] == status
    )


def render_markdown_report(audit: dict[str, Any]) -> str:
    """Format the audit dictionary as docs/research/guestlib_imports.md."""
    totals = audit["totals"]
    libs = audit["libraries"]
    symbols = audit["symbols"]

    lines: list[str] = [
        "# Guest C++ Runtime `libSystem.B.dylib` Import Audit (S4)",
        "",
        "**Initiative:** Guest-Dylib Initiative — Wave 1 (`S4`)  ",
        "**Tool:** `tools/guestlibs/import_audit.py`  ",
        "**Refs:** `#311`, `#330`",
        "",
        "## 1. Executive Summary",
        "",
        "Before enabling guest C++ runtime dynamic libraries (`libgcc_s.1.dylib`, `libstdc++.6.dylib`, `libc++abi.dylib`, `libc++.1.dylib`) behind the per-app `guestLibs` switch, every external C/POSIX/Mach symbol those libraries import from `/usr/lib/libSystem.B.dylib` must be cross-checked against HydrogenVM's host-side `src/runtime/libc/` registrations.",
        "",
        f"Across all four ARMv7 C++ runtime libraries, there are **{totals['totalUniqueImports']} unique `libSystem.B.dylib` imports**:",
        f"- **`present`**: **{totals['present']}** symbols (implemented in `src/runtime/libc/` with real or honest-approx behaviour)",
        f"- **`stubbed-dishonestly`**: **{totals['stubbed-dishonestly']}** symbols (registered in `src/runtime/libc/`, but returning a fixed `\"C\"` locale pointer or no-op success without full state)",
        f"- **`missing`**: **{totals['missing']}** symbols (not registered in HydrogenVM's symbol table; tracked in `#330`)",
        "",
        "### Per-Library Summary",
        "",
        "| Guest Dylib | Target Family | Total `libSystem` Imports | `present` | `stubbed-dishonestly` | `missing` |",
        "|---|---|---:|---:|---:|---:|",
    ]

    target_labels = {
        "libgcc_s.1.dylib": "Target 1 (iOS 3–5 GCC unwinder)",
        "libstdc++.6.dylib": "Target 1 (iOS 3–5 GNU libstdc++)",
        "libc++abi.dylib": "Target 2 (iOS 5+ LLVM libc++abi)",
        "libc++.1.dylib": "Target 2 (iOS 5+ LLVM libc++)",
    }

    for lib_name in (
        "libgcc_s.1.dylib",
        "libstdc++.6.dylib",
        "libc++abi.dylib",
        "libc++.1.dylib",
    ):
        c = libs[lib_name]
        lines.append(
            f"| `{lib_name}` | {target_labels[lib_name]} | {c['totalImports']} | {c['present']} | {c['stubbed-dishonestly']} | {c['missing']} |"
        )

    gcc_c = libs["libgcc_s.1.dylib"]
    abi_c = libs["libc++abi.dylib"]
    std_c = libs["libstdc++.6.dylib"]
    cxx_c = libs["libc++.1.dylib"]

    abi_missing = _symbols_for_lib_by_status(symbols, "libc++abi.dylib", "missing")
    std_missing = _symbols_for_lib_by_status(symbols, "libstdc++.6.dylib", "missing")
    std_stubbed = _symbols_for_lib_by_status(symbols, "libstdc++.6.dylib", "stubbed-dishonestly")
    cxx_missing = _symbols_for_lib_by_status(symbols, "libc++.1.dylib", "missing")
    cxx_stubbed = _symbols_for_lib_by_status(symbols, "libc++.1.dylib", "stubbed-dishonestly")

    xlocale_lifecycle = [
        s for s in cxx_missing if s in {"_newlocale", "_duplocale", "_freelocale", "_uselocale"}
    ]
    xlocale_ctype = [
        s
        for s in cxx_missing
        if s.endswith("_l")
        and (s.startswith("_is") or s.startswith("_to") or s == "_localeconv_l")
    ]
    xlocale_format = [
        s
        for s in cxx_missing
        if s.endswith("_l") and s not in xlocale_ctype
    ]
    xlocale_total = len(xlocale_lifecycle) + len(xlocale_ctype) + len(xlocale_format)
    cat_missing = [s for s in cxx_missing if s.startswith("_cat")]
    ldbl_missing = [s for s in cxx_missing if s in {"_strtold", "_wcstold"}]
    wchar_and_coll_names = {
        "_btowc", "_mbrlen", "_mbrtowc", "_mbsnrtowcs", "_mbsrtowcs", "_mbtowc",
        "_strcoll", "_strxfrm", "_wcrtomb", "_wcscoll", "_wcsnrtombs", "_wcsrtombs",
        "_wcsxfrm", "_wctob"
    }
    wchar_and_coll_missing = [s for s in cxx_missing if s in wchar_and_coll_names]

    other_missing = [
        s for s in cxx_missing
        if s not in set(xlocale_lifecycle + xlocale_ctype + xlocale_format + cat_missing + ldbl_missing + wchar_and_coll_missing)
    ]

    def fmt_list(items: list[str]) -> str:
        return ", ".join(f"`{s}`" for s in items) if items else "none"

    lines.extend(
        [
            "",
            "---",
            "",
            "## 2. Key Findings & Actionable Gaps",
            "",
            f"### 2.1 `libgcc_s.1.dylib` Coverage (`{gcc_c['present']} / {gcc_c['totalImports']} present`, `{gcc_c['stubbed-dishonestly']} stubbed-dishonestly`, `{gcc_c['missing']} missing`)",
            "",
            "Every `libSystem.B.dylib` import required by `libgcc_s.1.dylib` is classified in the symbol table below.",
            f"- **Stubbed dishonestly**: {fmt_list(_symbols_for_lib_by_status(symbols, 'libgcc_s.1.dylib', 'stubbed-dishonestly'))}.",
            "",
            f"### 2.2 `libc++abi.dylib` Coverage (`{abi_c['present']} / {abi_c['totalImports']} present`, `{abi_c['stubbed-dishonestly']} stubbed-dishonestly`, `{abi_c['missing']} missing`)",
            "",
            f"- **Missing ({len(abi_missing)})**: {fmt_list(abi_missing)}.",
            f"- **Stubbed dishonestly**: {fmt_list(_symbols_for_lib_by_status(symbols, 'libc++abi.dylib', 'stubbed-dishonestly'))}.",
            "",
            f"### 2.3 `libstdc++.6.dylib` Coverage (`{std_c['present']} / {std_c['totalImports']} present`, `{std_c['stubbed-dishonestly']} stubbed-dishonestly`, `{std_c['missing']} missing`)",
            "",
            f"- **Missing ({len(std_missing)})**: {fmt_list(std_missing)}.",
            f"- **Stubbed dishonestly ({len(std_stubbed)})**: {fmt_list(std_stubbed)}.",
            "",
            f"### 2.4 `libc++.1.dylib` Missing Symbol Clusters (`{cxx_c['present']} / {cxx_c['totalImports']} present`, `{cxx_c['stubbed-dishonestly']} stubbed-dishonestly`, `{cxx_c['missing']} missing`)",
            "",
            f"- **Stubbed dishonestly ({len(cxx_stubbed)})**: {fmt_list(cxx_stubbed)}.",
            f"- **Extended Locale (`<xlocale.h>`) Lifecycle & `*_l` Functions ({xlocale_total} symbols)**:",
            f"  - Locale lifecycle ({len(xlocale_lifecycle)}): {fmt_list(xlocale_lifecycle)}.",
            f"  - Character classification, case mapping & `localeconv_l` ({len(xlocale_ctype)}): {fmt_list(xlocale_ctype)}.",
            f"  - Numeric/string formatting & collation with explicit `locale_t` ({len(xlocale_format)}): {fmt_list(xlocale_format)}.",
            f"- **Multibyte / Wide-Character Conversion & Collation (`<wchar.h>` / `<string.h>`) Functions ({len(wchar_and_coll_missing)} symbols)**: {fmt_list(wchar_and_coll_missing)}.",
            f"- **Long-Double String Parsers ({len(ldbl_missing)} symbols)**: {fmt_list(ldbl_missing)}.",
            f"- **POSIX Message Catalogs ({len(cat_missing)} symbols)**: {fmt_list(cat_missing)}.",
            f"- **Other Unclassified Symbols ({len(other_missing)} symbols)**: {fmt_list(other_missing)}.",
            "",
            "---",
            "",
            "## 3. Complete Symbol-by-Symbol Audit Table",
            "",
            "| Symbol | Imported By | Status | HydrogenVM Source | Notes |",
            "|---|---|---|---|---|",
        ]
    )

    for sym, info in symbols.items():
        libs_str = ", ".join(f"`{lib}`" for lib in info["importedBy"])
        lines.append(
            f"| `{sym}` | {libs_str} | `{info['status']}` | `{info['source']}` | {info['note']} |"
        )

    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--dylib-dir",
        type=Path,
        default=None,
        help="Optional directory containing built guest .dylib files to inspect",
    )
    parser.add_argument(
        "--hvm-bin",
        type=Path,
        default=None,
        help="Optional path to compiled `hvm` binary for `--dump-support` cross-check",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=REPO_ROOT / "docs" / "research" / "guestlib_imports.md",
        help="Output path for Markdown audit report",
    )
    parser.add_argument(
        "--json",
        type=Path,
        default=None,
        help="Optional output path for JSON audit report",
    )
    args = parser.parse_args()

    audit = run_audit(REPO_ROOT, dylib_dir=args.dylib_dir, hvm_bin=args.hvm_bin)
    md = render_markdown_report(audit)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(md, encoding="utf-8")
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")

    t = audit["totals"]
    print(
        f"Audited {t['totalUniqueImports']} unique libSystem imports across 4 guest C++ dylibs: "
        f"present={t['present']}, stubbed-dishonestly={t['stubbed-dishonestly']}, missing={t['missing']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
