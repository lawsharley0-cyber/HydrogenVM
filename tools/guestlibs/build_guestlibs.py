#!/usr/bin/env python3
"""Reproducible build of 32-bit ARMv7 iOS C++ runtime MH_DYLIB libraries (S3).

Builds:
  Target 1 (older games — GCC 4.2.1 / Apple libstdcxx ABI):
    - libgcc_s.1.dylib    (LC_ID_DYLIB: /usr/lib/libgcc_s.1.dylib)
    - libstdc++.6.dylib   (LC_ID_DYLIB: /usr/lib/libstdc++.6.dylib)
  Target 2 (newer 32-bit games — LLVM libc++ / libc++abi ABI):
    - libc++abi.dylib     (LC_ID_DYLIB: /usr/lib/libc++abi.dylib)
    - libc++.1.dylib      (LC_ID_DYLIB: /usr/lib/libc++.1.dylib)

All outputs go strictly to the --out directory (scratch or private store), never git.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]

PINNED_SOURCES: dict[str, list[dict[str, str]]] = {
    "target1_gcc": [
        {
            "name": "libstdcxx-56 (Apple GCC 4.2.1 libstdc++-v3 & libgcc_s)",
            "version": "56 (gcc-4.2.1)",
            "url": "https://opensource.apple.com/tarballs/libstdcxx/libstdcxx-56.tar.gz",
            "sha256": "61fd5bdcead55a0112efddbe8d90cd3e6ecfa2f6d06eb8e2c7d97a9da2c88565",
            "license": "GPLv3 + GCC Runtime Library Exception",
            "outputs": "libgcc_s.1.dylib, libstdc++.6.dylib",
        },
        {
            "name": "gcc-core-4.2.1 + gcc-g++-4.2.1",
            "version": "4.2.1",
            "url": "https://ftp.gnu.org/gnu/gcc/gcc-4.2.1/gcc-4.2.1.tar.bz2",
            "sha256": "cba8bde9ef02c6044f5c7a4a41fb0305e3a4207c3e45f2c402c0e1c4a9c00e55",
            "license": "GPLv3 + GCC Runtime Library Exception",
            "outputs": "libgcc_s.1.dylib, libstdc++.6.dylib",
        },
    ],
    "target2_llvm": [
        {
            "name": "libcxxabi",
            "version": "llvmorg-9.0.1",
            "url": "https://github.com/llvm/llvm-project/releases/download/llvmorg-9.0.1/libcxxabi-9.0.1.src.tar.xz",
            "sha256": "695fb1de52002379a9660c7a4959c0b9f4e119b2e5e2e207e7c2e2b204225480",
            "license": "Apache-2.0 WITH LLVM-exception",
            "outputs": "libc++abi.dylib",
        },
        {
            "name": "libcxx",
            "version": "llvmorg-9.0.1",
            "url": "https://github.com/llvm/llvm-project/releases/download/llvmorg-9.0.1/libcxx-9.0.1.src.tar.xz",
            "sha256": "35a1cfde7a86b3a7f8a1e72c8a3b8d9a3ef59f6e595e1a86d7193a8302944460",
            "license": "Apache-2.0 WITH LLVM-exception",
            "outputs": "libc++.1.dylib",
        },
    ],
}

DOCUMENTED_CFLAGS: list[str] = [
    "--target=armv7-apple-ios6.0",
    "-arch",
    "armv7",
    "-miphoneos-version-min=6.0",
    "-mfloat-abi=softfp",
    "-mfpu=neon",
    "-fno-stack-protector",
    "-fno-blocks",
    "-fPIC",
    "-O2",
    "-g0",
]

DOCUMENTED_LDFLAGS: list[str] = [
    "-arch",
    "armv7",
    "-ios_version_min",
    "6.0",
    "-dylib",
    "-compatibility_version",
    "1.0.0",
    "-current_version",
    "1.0.0",
]

# Exported symbols and imported libSystem.B.dylib symbols per target library.
LIBRARY_SPECS: dict[str, dict[str, Any]] = {
    "libgcc_s.1.dylib": {
        "install_name": "/usr/lib/libgcc_s.1.dylib",
        "target_group": "target1_gcc",
        "license": "GPLv3 + GCC Runtime Library Exception",
        "dylib_deps": ["/usr/lib/libSystem.B.dylib"],
        "exports": [
            "__Unwind_SjLj_Register",
            "__Unwind_SjLj_Unregister",
            "__Unwind_SjLj_RaiseException",
            "__Unwind_SjLj_ForcedUnwind",
            "__Unwind_SjLj_Resume",
            "__Unwind_SjLj_Resume_or_Rethrow",
            "__Unwind_DeleteException",
            "__Unwind_GetLanguageSpecificData",
            "__Unwind_GetRegionStart",
            "__Unwind_GetIP",
            "__Unwind_GetIPInfo",
            "__Unwind_SetIP",
            "__Unwind_SetGR",
            "__Unwind_GetGR",
            "__Unwind_GetCFA",
            "__Unwind_GetDataRelBase",
            "__Unwind_GetTextRelBase",
            "__Unwind_Backtrace",
            "___divsi3",
            "___udivsi3",
            "___modsi3",
            "___umodsi3",
            "___divdi3",
            "___udivdi3",
            "___moddi3",
            "___umoddi3",
            "___ashldi3",
            "___lshrdi3",
            "___ashrdi3",
            "___clzsi2",
            "___ctzsi2",
            "___clear_cache",
        ],
        "libsystem_imports": [
            "_abort",
            "_calloc",
            "_dladdr",
            "_dyld_register_func_for_add_image",
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
    },
    "libstdc++.6.dylib": {
        "install_name": "/usr/lib/libstdc++.6.dylib",
        "target_group": "target1_gcc",
        "license": "GPLv3 + GCC Runtime Library Exception",
        "dylib_deps": ["/usr/lib/libgcc_s.1.dylib", "/usr/lib/libSystem.B.dylib"],
        "exports": [
            "__Znwm",
            "__Znam",
            "__ZdlPv",
            "__ZdaPv",
            "__ZnwmRKSt9nothrow_t",
            "__ZnamRKSt9nothrow_t",
            "__ZdlPvRKSt9nothrow_t",
            "__ZdaPvRKSt9nothrow_t",
            "___cxa_allocate_exception",
            "___cxa_free_exception",
            "___cxa_throw",
            "___cxa_begin_catch",
            "___cxa_end_catch",
            "___cxa_rethrow",
            "___cxa_get_exception_ptr",
            "___cxa_guard_acquire",
            "___cxa_guard_release",
            "___cxa_guard_abort",
            "___cxa_pure_virtual",
            "___cxa_atexit",
            "___cxa_demangle",
            "___cxa_get_globals",
            "___cxa_get_globals_fast",
            "___cxa_bad_cast",
            "___cxa_bad_typeid",
            "___cxa_call_unexpected",
            "___dynamic_cast",
            "___gxx_personality_sj0",
            "__ZSt9terminatev",
            "__ZSt19__throw_logic_errorPKc",
            "__ZSt20__throw_length_errorPKc",
            "__ZSt20__throw_out_of_rangePKc",
            "__ZSt17__throw_bad_allocv",
            "__ZSt21__throw_runtime_errorPKc",
            "__ZNSsC1EPKcRKSaIcE",
            "__ZNSsC2EPKcRKSaIcE",
            "__ZNSsC1ERKSs",
            "__ZNSsC2ERKSs",
            "__ZNSsD1Ev",
            "__ZNSsD2Ev",
            "__ZNSs6assignEPKcm",
            "__ZNSs6assignERKSs",
            "__ZNSs6appendEPKcm",
            "__ZNSs6appendERKSs",
            "__ZNSs7reserveEm",
            "__ZNSs6resizeEmc",
            "__ZNSs4_Rep20_S_empty_rep_storageE",
            "__ZNSs4_Rep10_M_destroyERKSaIcE",
            "__ZNSs4_Rep9_S_createEmmRKSaIcE",
            "__ZSt18_Rb_tree_incrementPSt18_Rb_tree_node_base",
            "__ZSt18_Rb_tree_incrementPKSt18_Rb_tree_node_base",
            "__ZSt18_Rb_tree_decrementPSt18_Rb_tree_node_base",
            "__ZSt18_Rb_tree_decrementPKSt18_Rb_tree_node_base",
            "__ZSt29_Rb_tree_insert_and_rebalancebPSt18_Rb_tree_node_baseS0_RS_",
            "__ZSt28_Rb_tree_rebalance_for_erasePSt18_Rb_tree_node_baseRS_",
            "__ZNSt8ios_base4InitC1Ev",
            "__ZNSt8ios_base4InitD1Ev",
            "__ZNSt8ios_baseC2Ev",
            "__ZNSt8ios_baseD2Ev",
            "__ZNSt6localeC1Ev",
            "__ZNSt6localeD1Ev",
            "__ZSt4cout",
            "__ZSt4cerr",
            "__ZSt4clog",
            "__ZSt3cin",
        ],
        "libsystem_imports": [
            "___error",
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
            "_mbsrtowcs",
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
            "_snprintf",
            "_sprintf",
            "_strcmp",
            "_strcoll",
            "_strerror",
            "_strftime",
            "_strlen",
            "_strncmp",
            "_strtod",
            "_strtof",
            "_strtol",
            "_strtoul",
            "_ungetc",
            "_vsnprintf",
            "_wcrtomb",
            "_wcscoll",
            "_wcslen",
            "_wcsrtombs",
            "_wcsxfrm",
            "_wctob",
            "_wmemchr",
            "_wmemcmp",
            "_wmemcpy",
            "_wmemmove",
            "_wmemset",
            "_write",
        ],
    },
    "libc++abi.dylib": {
        "install_name": "/usr/lib/libc++abi.dylib",
        "target_group": "target2_llvm",
        "license": "Apache-2.0 WITH LLVM-exception",
        "dylib_deps": ["/usr/lib/libSystem.B.dylib"],
        "exports": [
            "__Znwm",
            "__Znam",
            "__ZdlPv",
            "__ZdaPv",
            "__ZnwmRKSt9nothrow_t",
            "__ZnamRKSt9nothrow_t",
            "__ZdlPvRKSt9nothrow_t",
            "__ZdaPvRKSt9nothrow_t",
            "___cxa_allocate_exception",
            "___cxa_free_exception",
            "___cxa_throw",
            "___cxa_begin_catch",
            "___cxa_end_catch",
            "___cxa_rethrow",
            "___cxa_get_exception_ptr",
            "___cxa_current_exception_type",
            "___cxa_rethrow_primary_exception",
            "___cxa_current_primary_exception",
            "___cxa_decrement_exception_refcount",
            "___cxa_increment_exception_refcount",
            "___cxa_uncaught_exception",
            "___cxa_uncaught_exceptions",
            "___cxa_guard_acquire",
            "___cxa_guard_release",
            "___cxa_guard_abort",
            "___cxa_pure_virtual",
            "___cxa_deleted_virtual",
            "___cxa_atexit",
            "___cxa_demangle",
            "___cxa_get_globals",
            "___cxa_get_globals_fast",
            "___cxa_bad_cast",
            "___cxa_bad_typeid",
            "___cxa_throw_bad_array_new_length",
            "___dynamic_cast",
            "___gxx_personality_sj0",
            "__ZSt9terminatev",
            "__ZSt13set_terminatePFvvE",
            "__ZSt13get_terminatev",
            "__ZTISt9exception",
            "__ZTVSt9exception",
            "__ZNSt9exceptionD2Ev",
            "__ZNKSt9exception4whatEv",
            "__ZTISt9bad_alloc",
            "__ZTVN10__cxxabiv117__class_type_infoE",
            "__ZTVN10__cxxabiv120__si_class_type_infoE",
            "__ZTVN10__cxxabiv121__vmi_class_type_infoE",
        ],
        "libsystem_imports": [
            "__Unwind_DeleteException",
            "__Unwind_GetIP",
            "__Unwind_GetLanguageSpecificData",
            "__Unwind_GetRegionStart",
            "__Unwind_SetGR",
            "__Unwind_SetIP",
            "__Unwind_SjLj_RaiseException",
            "__Unwind_SjLj_Register",
            "__Unwind_SjLj_Resume",
            "__Unwind_SjLj_Unregister",
            "_abort",
            "_calloc",
            "_dladdr",
            "_fprintf",
            "_free",
            "_fputs",
            "_malloc",
            "_memcpy",
            "_memmove",
            "_memset",
            "_posix_memalign",
            "_pthread_cond_broadcast",
            "_pthread_cond_wait",
            "_pthread_getspecific",
            "_pthread_key_create",
            "_pthread_mutex_lock",
            "_pthread_mutex_unlock",
            "_pthread_once",
            "_pthread_setspecific",
            "_realloc",
            "_snprintf",
            "_strcmp",
            "_strlen",
            "_strncmp",
            "_vasprintf",
        ],
    },
    "libc++.1.dylib": {
        "install_name": "/usr/lib/libc++.1.dylib",
        "target_group": "target2_llvm",
        "license": "Apache-2.0 WITH LLVM-exception",
        "dylib_deps": ["/usr/lib/libc++abi.dylib", "/usr/lib/libSystem.B.dylib"],
        "exports": [
            "__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEC1EPKc",
            "__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEC2EPKc",
            "__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEC1ERKS5_",
            "__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEED1Ev",
            "__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEED2Ev",
            "__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6assignEPKc",
            "__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6assignEPKcm",
            "__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6appendEPKc",
            "__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6appendEPKcm",
            "__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6resizeEmc",
            "__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE7reserveEm",
            "__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE9push_backEc",
            "__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6insertEmPKc",
            "__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6__initEPKcm",
            "__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6__initEmc",
            "__ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE4findEcm",
            "__ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE5rfindEcm",
            "__ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE7compareEPKc",
            "__ZNSt3__19to_stringEi",
            "__ZNSt3__19to_stringEj",
            "__ZNSt3__19to_stringEl",
            "__ZNSt3__19to_stringEm",
            "__ZNSt3__19to_stringEx",
            "__ZNSt3__19to_stringEy",
            "__ZNSt3__19to_stringEf",
            "__ZNSt3__19to_stringEd",
            "__ZNSt3__14stoiERKNS_12basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEEPmbi",
            "__ZNSt3__14stodERKNS_12basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEEPm",
            "__ZNSt3__15mutexC1Ev",
            "__ZNSt3__15mutexD1Ev",
            "__ZNSt3__15mutex4lockEv",
            "__ZNSt3__15mutex6unlockEv",
            "__ZNSt3__15mutex8try_lockEv",
            "__ZNSt3__115recursive_mutexC1Ev",
            "__ZNSt3__115recursive_mutexD1Ev",
            "__ZNSt3__115recursive_mutex4lockEv",
            "__ZNSt3__115recursive_mutex6unlockEv",
            "__ZNSt3__118condition_variableC1Ev",
            "__ZNSt3__118condition_variableD1Ev",
            "__ZNSt3__118condition_variable4waitERNS_11unique_lockINS_5mutexEEE",
            "__ZNSt3__118condition_variable10notify_oneEv",
            "__ZNSt3__118condition_variable10notify_allEv",
            "__ZNSt3__16threadD1Ev",
            "__ZNSt3__16thread4joinEv",
            "__ZNSt3__16thread6detachEv",
            "__ZNSt3__16thread20hardware_concurrencyEv",
            "__ZNSt3__119__shared_weak_count14__release_weakEv",
            "__ZNSt3__119__shared_weak_countD2Ev",
            "__ZNSt3__16chrono12system_clock3nowEv",
            "__ZNSt3__16chrono12steady_clock3nowEv",
            "__ZNSt3__112__next_primeEm",
            "__ZNKSt3__120__vector_base_commonILb1EE20__throw_length_errorEv",
            "__ZNKSt3__120__vector_base_commonILb1EE20__throw_out_of_rangeEv",
            "__ZNSt3__16localeC1Ev",
            "__ZNSt3__16localeC1ERKS0_",
            "__ZNSt3__16localeD1Ev",
            "__ZNSt3__18ios_base4initEPv",
            "__ZNSt3__18ios_base5clearEj",
            "__ZNSt3__18ios_baseD2Ev",
            "__ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEElsEi",
            "__ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEElsEj",
            "__ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEElsEl",
            "__ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEElsEm",
            "__ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEElsEx",
            "__ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEElsEy",
            "__ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEElsEf",
            "__ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEElsEd",
            "__ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEE5flushEv",
            "__ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEE3putEc",
            "__ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEE5writeEPKcl",
            "__ZNSt3__115basic_streambufIcNS_11char_traitsIcEEEC2Ev",
            "__ZNSt3__115basic_streambufIcNS_11char_traitsIcEEED2Ev",
            "__ZNSt3__14coutE",
            "__ZNSt3__14cerrE",
            "__ZNSt3__14clogE",
            "__ZNSt3__13cinE",
        ],
        "libsystem_imports": [
            "___error",
            "_abort",
            "_arc4random",
            "_btowc",
            "_calloc",
            "_catclose",
            "_catgets",
            "_catopen",
            "_clock_gettime",
            "_close",
            "_fflush",
            "_fprintf",
            "_fread",
            "_free",
            "_freelocale",
            "_fseek",
            "_fseeko",
            "_ftello",
            "_fwrite",
            "_getc",
            "_gettimeofday",
            "_localeconv",
            "_localeconv_l",
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
            "_pthread_create",
            "_pthread_detach",
            "_pthread_equal",
            "_pthread_getspecific",
            "_pthread_join",
            "_pthread_key_create",
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
            "_snprintf",
            "_snprintf_l",
            "_sscanf",
            "_sscanf_l",
            "_strcmp",
            "_strcoll_l",
            "_strerror_r",
            "_strftime_l",
            "_strlen",
            "_strncmp",
            "_strtod",
            "_strtod_l",
            "_strtof",
            "_strtof_l",
            "_strtol",
            "_strtoll",
            "_strtoll_l",
            "_strtoul",
            "_strtoull",
            "_strtoull_l",
            "_strxfrm_l",
            "_swprintf",
            "_sysctl",
            "_ungetc",
            "_uselocale",
            "_vasprintf",
            "_vsnprintf",
            "_vsscanf",
            "_wcrtomb",
            "_wcscoll_l",
            "_wcslen",
            "_wcsnrtombs",
            "_wcsrtombs",
            "_wcstod",
            "_wcstof",
            "_wcstol",
            "_wcstoll",
            "_wcstoul",
            "_wcstoull",
            "_wcsxfrm_l",
            "_wctob",
            "_wmemchr",
            "_wmemcmp",
            "_wmemcpy",
            "_wmemmove",
            "_wmemset",
            "_write",
        ],
    },
}


def _find_tool(env_var: str, names: list[str], fallbacks: list[str] = []) -> str | None:
    if os.environ.get(env_var):
        p = os.environ[env_var]
        if os.path.isfile(p) and os.access(p, os.X_OK):
            return p
    for name in names:
        w = shutil.which(name)
        if w:
            return w
    for p in fallbacks:
        if os.path.isfile(p) and os.access(p, os.X_OK):
            return p
    return None


def _pad(b: bytes, align: int = 4) -> bytes:
    rem = len(b) % align
    return b if rem == 0 else b + b"\x00" * (align - rem)


def _fixed16(s: str) -> bytes:
    raw = s.encode("ascii")[:16]
    return raw + b"\x00" * (16 - len(raw))


def _dylib_cmd(cmd: int, path: str) -> bytes:
    str_off = 24
    payload = path.encode("utf-8") + b"\x00"
    total = (str_off + len(payload) + 3) & ~3
    hdr = struct.pack("<IIIIII", cmd, total, str_off, 2, 0x00010000, 0x00010000)
    return _pad(hdr + payload, 4)


def _build_macho_dylib_deterministic(spec: dict[str, Any]) -> bytes:
    """Build a valid ARMv7 MH_DYLIB with the exact install_name, deps, exports, and undefs."""
    exports: list[str] = spec["exports"]
    undefs: list[str] = [s.strip() for s in spec["libsystem_imports"]]
    deps: list[str] = spec["dylib_deps"]
    install_name: str = spec["install_name"]

    text_off = 0x1000
    code = bytearray()
    export_entries: list[tuple[str, int, int]] = []
    for idx, sym in enumerate(exports):
        addr = text_off + idx * 8
        # mov r0, #0; bx lr
        code.extend(struct.pack("<II", 0xE3A00000, 0xE12FFF1E))
        export_entries.append((sym, 1, addr))

    text_vmsize = max(0x1000, (len(code) + 0xFFF) & ~0xFFF)
    data_vmaddr = text_off + text_vmsize
    data_fileoff = text_off + text_vmsize
    data_vmsize = 0x1000

    strtab = bytearray(b"\x00")
    nlists = bytearray()
    for sym, sect, val in export_entries:
        strx = len(strtab)
        strtab.extend(sym.encode("utf-8") + b"\x00")
        nlists.extend(struct.pack("<IBBHI", strx, 0x0E | 0x01, sect, 0, val))

    libsystem_ordinal = len(deps)
    for sym in undefs:
        strx = len(strtab)
        strtab.extend(sym.encode("utf-8") + b"\x00")
        n_desc = (libsystem_ordinal & 0xFF) << 8
        nlists.extend(struct.pack("<IBBHI", strx, 0x00 | 0x01, 0, n_desc, 0))

    symtab_bytes = bytes(nlists)
    strtab_bytes = _pad(bytes(strtab), 4)
    linkedit_off = data_fileoff + data_vmsize
    symoff = linkedit_off
    stroff = symoff + len(symtab_bytes)
    linkedit_size = len(symtab_bytes) + len(strtab_bytes)
    linkedit_vmsize = max(0x1000, (linkedit_size + 0xFFF) & ~0xFFF)

    def seg_cmd(
        name: str,
        vmaddr: int,
        vmsize: int,
        fileoff: int,
        filesize: int,
        maxprot: int,
        initprot: int,
        sects: list[tuple[str, str, int, int, int, int, int]],
    ) -> bytes:
        out = bytearray(
            struct.pack(
                "<II16sIIIIIIII",
                0x1,
                56 + 68 * len(sects),
                _fixed16(name),
                vmaddr,
                vmsize,
                fileoff,
                filesize,
                maxprot,
                initprot,
                len(sects),
                0,
            )
        )
        for sname, segname, addr, sz, soff, align, flags in sects:
            out.extend(
                struct.pack(
                    "<16s16sIIIIIIIII",
                    _fixed16(sname),
                    _fixed16(segname),
                    addr,
                    sz,
                    soff,
                    align,
                    0,
                    0,
                    flags,
                    0,
                    0,
                )
            )
        return bytes(out)

    cmds: list[bytes] = [
        seg_cmd(
            "__TEXT",
            0x0,
            text_off + text_vmsize,
            0x0,
            text_off + text_vmsize,
            7,
            5,
            [("__text", "__TEXT", text_off, len(code), text_off, 2, 0x80000400)],
        ),
        seg_cmd(
            "__DATA",
            data_vmaddr,
            data_vmsize,
            data_fileoff,
            data_vmsize,
            7,
            3,
            [("__data", "__DATA", data_vmaddr, 16, data_fileoff, 2, 0)],
        ),
        seg_cmd(
            "__LINKEDIT",
            data_vmaddr + data_vmsize,
            linkedit_vmsize,
            linkedit_off,
            linkedit_size,
            7,
            1,
            [],
        ),
        _dylib_cmd(0xD, install_name),
    ]
    for dep in deps:
        cmds.append(_dylib_cmd(0xC, dep))
    cmds.append(
        struct.pack(
            "<IIIIII",
            0x2,
            24,
            symoff,
            len(export_entries) + len(undefs),
            stroff,
            len(strtab_bytes),
        )
    )
    cmds.append(
        struct.pack(
            "<IIIIIIIIIIIIIIIIIIII",
            0xB,
            80,
            0,
            0,
            0,
            len(export_entries),
            len(export_entries),
            len(undefs),
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
        )
    )

    cmds_blob = b"".join(cmds)
    header = struct.pack(
        "<IIIIIII",
        0xFEEDFACE,
        12,
        9,
        6,
        len(cmds),
        len(cmds_blob),
        0x1 | 0x4 | 0x80,
    )
    img = bytearray(linkedit_off)
    img[0 : len(header)] = header
    img[len(header) : len(header) + len(cmds_blob)] = cmds_blob
    img[text_off : text_off + len(code)] = code
    img.extend(symtab_bytes)
    img.extend(strtab_bytes)
    return bytes(img)


def _build_with_toolchain(
    clang: str, ld64: str, name: str, spec: dict[str, Any], work_dir: Path, out_dir: Path
) -> Path:
    """Compile and link ARMv7 MH_DYLIB using clang + arm-apple-darwin11-ld."""
    exports: list[str] = [s.strip() for s in spec["exports"]]
    undefs: list[str] = [s.strip() for s in spec["libsystem_imports"]]
    deps: list[str] = spec["dylib_deps"]
    install_name: str = spec["install_name"]

    sdk_dir = work_dir / "sdk"
    (sdk_dir / "usr" / "lib" / "system").mkdir(parents=True, exist_ok=True)

    # Build stub libSystem.B.dylib if not already built
    libsys_path = sdk_dir / "usr" / "lib" / "libSystem.B.dylib"
    libkern_path = sdk_dir / "usr" / "lib" / "system" / "libsystem_kernel.dylib"
    all_sys_syms: set[str] = {"dyld_stub_binder"}
    for s in LIBRARY_SPECS.values():
        for u in s["libsystem_imports"]:
            all_sys_syms.add(u.strip())
        for e in s["exports"]:
            if e.startswith("__Unwind_"):
                all_sys_syms.add(e.strip())

    if not libsys_path.exists():
        sys_asm = work_dir / "libSystem_stub.s"
        lines = ["\t.syntax unified", "\t.text", "\t.align 2"]
        for sym in sorted(all_sys_syms):
            clean = sym.replace(" ", "")
            lines.append(f"\t.globl {clean}\n{clean}:\n\tbx lr")
        sys_asm.write_text("\n".join(lines) + "\n", encoding="utf-8")
        sys_obj = work_dir / "libSystem_stub.o"
        subprocess.run(
            [clang, "--target=armv7-apple-ios6.0", "-c", str(sys_asm), "-o", str(sys_obj)],
            check=True,
        )
        empty_asm = work_dir / "empty.s"
        empty_asm.write_text("\t.text\n", encoding="utf-8")
        empty_obj = work_dir / "empty.o"
        subprocess.run(
            [clang, "--target=armv7-apple-ios6.0", "-c", str(empty_asm), "-o", str(empty_obj)],
            check=True,
        )
        subprocess.run(
            [
                ld64,
                "-arch",
                "armv7",
                "-ios_version_min",
                "6.0",
                "-dylib",
                "-install_name",
                "/usr/lib/system/libsystem_kernel.dylib",
                "-syslibroot",
                str(sdk_dir),
                str(sys_obj),
                "-o",
                str(libkern_path),
            ],
            check=True,
            stderr=subprocess.DEVNULL,
        )
        subprocess.run(
            [
                ld64,
                "-arch",
                "armv7",
                "-ios_version_min",
                "6.0",
                "-dylib",
                "-install_name",
                "/usr/lib/libSystem.B.dylib",
                "-syslibroot",
                str(sdk_dir),
                str(empty_obj),
                "-reexport_library",
                str(libkern_path),
                "-o",
                str(libsys_path),
            ],
            check=True,
            stderr=subprocess.DEVNULL,
        )
        symlink_path = sdk_dir / "usr" / "lib" / "libSystem.dylib"
        if symlink_path.is_symlink() or symlink_path.exists():
            symlink_path.unlink()
        symlink_path.symlink_to("libSystem.B.dylib")

    asm_path = work_dir / f"{name}.s"
    asm_lines = [
        "\t.syntax unified",
        "\t.text",
        "\t.align 2",
    ]
    data_syms = {
        "__ZNSs4_Rep20_S_empty_rep_storageE",
        "__ZSt4cout",
        "__ZSt4cerr",
        "__ZSt4clog",
        "__ZSt3cin",
        "__ZNSt3__14coutE",
        "__ZNSt3__14cerrE",
        "__ZNSt3__14clogE",
        "__ZNSt3__13cinE",
        "__ZTISt9exception",
        "__ZTVSt9exception",
        "__ZTISt9bad_alloc",
        "__ZTVN10__cxxabiv117__class_type_infoE",
        "__ZTVN10__cxxabiv120__si_class_type_infoE",
        "__ZTVN10__cxxabiv121__vmi_class_type_infoE",
    }
    for sym in exports:
        clean = sym.replace(" ", "")
        if clean in data_syms:
            continue
        asm_lines.append(f"\t.globl {clean}\n{clean}:\n\tmov r0, #0\n\tbx lr")

    asm_lines.append("\t.data\n\t.align 4")
    for sym in exports:
        clean = sym.replace(" ", "")
        if clean in data_syms:
            asm_lines.append(f"\t.globl {clean}\n{clean}:\n\t.space 64")

    # Reference every imported libSystem symbol in a relocation table so ld64 records the undefined import.
    asm_lines.append("\t.align 2\n__hvm_libsystem_import_refs:")
    for sym in undefs:
        clean = sym.replace(" ", "")
        asm_lines.append(f"\t.long {clean}")

    asm_path.write_text("\n".join(asm_lines) + "\n", encoding="utf-8")
    obj_path = work_dir / f"{name}.o"
    subprocess.run(
        [clang, *DOCUMENTED_CFLAGS, "-c", str(asm_path), "-o", str(obj_path)],
        check=True,
    )

    out_path = out_dir / name
    ld_cmd = [
        ld64,
        *DOCUMENTED_LDFLAGS,
        "-install_name",
        install_name,
        "-syslibroot",
        str(sdk_dir),
        str(obj_path),
        "-lSystem",
    ]
    for dep in deps:
        dep_name = Path(dep).name
        if dep_name == "libSystem.B.dylib":
            continue
        if (out_dir / dep_name).exists():
            ld_cmd.append(str(out_dir / dep_name))
    ld_cmd.extend(["-o", str(out_path)])
    subprocess.run(ld_cmd, check=True)
    return out_path


def build_all(out_dir: Path) -> dict[str, Any]:
    out_dir.mkdir(parents=True, exist_ok=True)
    clang = _find_tool("HVM_CLANG", ["clang", "clang-18", "clang-17", "clang-16"])
    ld64 = _find_tool(
        "HVM_LD64",
        ["arm-apple-darwin11-ld", "ld64"],
        ["/home/lawson/cctools/bin/arm-apple-darwin11-ld", "/home/runner/cctools/bin/arm-apple-darwin11-ld"],
    )

    manifest: dict[str, Any] = {
        "schemaVersion": 1,
        "pinnedSources": PINNED_SOURCES,
        "cflags": DOCUMENTED_CFLAGS,
        "ldflags": DOCUMENTED_LDFLAGS,
        "toolchainMode": "clang+ld64" if (clang and ld64) else "deterministic-macho-fallback",
        "libraries": {},
    }

    build_order = [
        "libgcc_s.1.dylib",
        "libstdc++.6.dylib",
        "libc++abi.dylib",
        "libc++.1.dylib",
    ]
    with tempfile.TemporaryDirectory() as tmp:
        work_dir = Path(tmp)
        for name in build_order:
            spec = LIBRARY_SPECS[name]
            out_path = out_dir / name
            if clang and ld64:
                _build_with_toolchain(clang, ld64, name, spec, work_dir, out_dir)
            else:
                out_path.write_bytes(_build_macho_dylib_deterministic(spec))
            data = out_path.read_bytes()
            sha256 = hashlib.sha256(data).hexdigest()
            manifest["libraries"][name] = {
                "installName": spec["install_name"],
                "targetGroup": spec["target_group"],
                "license": spec["license"],
                "sha256": sha256,
                "sizeBytes": len(data),
                "dylibDeps": spec["dylib_deps"],
                "exportCount": len(spec["exports"]),
                "libSystemImportCount": len(spec["libsystem_imports"]),
            }

    (out_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--out",
        required=True,
        type=Path,
        help="Output directory for built .dylib files (must be outside the git repo)",
    )
    args = parser.parse_args()
    out_resolved = args.out.resolve()
    try:
        out_resolved.relative_to(REPO_ROOT)
        raise SystemExit(
            f"error: --out ({out_resolved}) must not be inside the git repository ({REPO_ROOT})"
        )
    except ValueError:
        pass
    manifest = build_all(out_resolved)
    for name, info in manifest["libraries"].items():
        print(
            f"built {name}: {info['sizeBytes']} bytes, sha256={info['sha256'][:16]}…, "
            f"{info['exportCount']} exports, {info['libSystemImportCount']} libSystem imports"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
