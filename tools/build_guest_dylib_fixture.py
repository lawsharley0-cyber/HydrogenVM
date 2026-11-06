#!/usr/bin/env python3
"""Build a deterministic ARMv7 Mach-O executable + dependent MH_DYLIB fixture for S1 tests.

Generates:
  <out>/GuestDylibFixture.app/Info.plist
  <out>/GuestDylibFixture.app/GuestDylibFixture   (MH_EXECUTE, ARMv7)
  <out>/guestlibs/libguest_a.dylib                (MH_DYLIB, ARMv7, depends on libguest_b.dylib)
  <out>/guestlibs/libguest_b.dylib                (MH_DYLIB, ARMv7, depends on libSystem.B.dylib)

Dependency & initializer chain:
  1. libguest_b.dylib initializer (_guest_b_init) rebases _guest_b_state_ptr and updates
     *_guest_b_state_ptr from 10 to 40.
  2. libguest_a.dylib initializer (_guest_a_init) calls _guest_b_get() (imported from
     libguest_b.dylib) and stores 40 + 2 = 42 through its rebased _guest_a_val_ptr.
  3. GuestDylibFixture initializer (_exe_init) sets _exe_bonus = 5, and _main calls
     _guest_a_compute(_exe_bonus) -> 42 + 5 = 47.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import struct
from typing import Iterable

MH_MAGIC = 0xFEEDFACE
CPU_TYPE_ARM = 12
CPU_SUBTYPE_ARM_V7 = 9
MH_EXECUTE = 2
MH_DYLIB = 6
MH_NOUNDEFS = 0x1
MH_DYLDLINK = 0x4
MH_TWOLEVEL = 0x80
MH_PIE = 0x200000

LC_SEGMENT = 0x1
LC_SYMTAB = 0x2
LC_DYSYMTAB = 0xB
LC_LOAD_DYLIB = 0xC
LC_ID_DYLIB = 0xD
LC_REQ_DYLD = 0x80000000
LC_DYLD_INFO_ONLY = 0x22 | LC_REQ_DYLD
LC_MAIN = 0x28 | LC_REQ_DYLD

S_REGULAR = 0x0
S_NON_LAZY_SYMBOL_POINTERS = 0x6
S_MOD_INIT_FUNC_POINTERS = 0x9
S_ATTR_PURE_INSTRUCTIONS = 0x80000000
S_ATTR_SOME_INSTRUCTIONS = 0x00000400

N_UNDF = 0x0
N_SECT = 0xE
N_EXT = 0x1


def _pad(b: bytes, align: int = 4) -> bytes:
    rem = len(b) % align
    return b if rem == 0 else b + b"\x00" * (align - rem)


def _fixed16(s: str) -> bytes:
    raw = s.encode("ascii")[:16]
    return raw + b"\x00" * (16 - len(raw))


def _uleb(val: int) -> bytes:
    out = bytearray()
    while True:
        byte = val & 0x7F
        val >>= 7
        if val:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def _dylib_cmd(cmd: int, path: str) -> bytes:
    str_off = 24
    payload = path.encode("utf-8") + b"\x00"
    total = (str_off + len(payload) + 3) & ~3
    hdr = struct.pack("<IIIIII", cmd, total, str_off, 2, 0x00010000, 0x00010000)
    return _pad(hdr + payload, 4)


def _segment_cmd(
    segname: str,
    vmaddr: int,
    vmsize: int,
    fileoff: int,
    filesize: int,
    maxprot: int,
    initprot: int,
    sections: list[tuple[str, str, int, int, int, int, int, int, int]],
) -> bytes:
    out = bytearray(
        struct.pack(
            "<II16sIIIIIIII",
            LC_SEGMENT,
            56 + 68 * len(sections),
            _fixed16(segname),
            vmaddr,
            vmsize,
            fileoff,
            filesize,
            maxprot,
            initprot,
            len(sections),
            0,
        )
    )
    for sectname, s_seg, addr, size, offset, align, flags, r1, r2 in sections:
        out.extend(
            struct.pack(
                "<16s16sIIIIIIIII",
                _fixed16(sectname),
                _fixed16(s_seg),
                addr,
                size,
                offset,
                align,
                0,
                0,
                flags,
                r1,
                r2,
            )
        )
    return bytes(out)


def _build_symtab(
    exports: list[tuple[str, int, int]],
    undefs: list[tuple[str, int]],
) -> tuple[bytes, bytes, int, int]:
    """Return (symtab_bytes, strtab_bytes, nextdef, nundef)."""
    strtab = bytearray(b"\x00")
    nlists = bytearray()
    for name, sect, value in exports:
        strx = len(strtab)
        strtab.extend(name.encode("utf-8") + b"\x00")
        nlists.extend(struct.pack("<IBBHI", strx, N_SECT | N_EXT, sect, 0, value))
    for name, ordinal in undefs:
        strx = len(strtab)
        strtab.extend(name.encode("utf-8") + b"\x00")
        n_desc = (ordinal & 0xFF) << 8
        nlists.extend(struct.pack("<IBBHI", strx, N_UNDF | N_EXT, 0, n_desc, 0))
    return bytes(nlists), _pad(bytes(strtab), 4), len(exports), len(undefs)


def _rebase_stream(seg_index: int, offsets: Iterable[int]) -> bytes:
    out = bytearray()
    out.append(0x11)  # REBASE_OPCODE_SET_TYPE_IMM(REBASE_TYPE_POINTER=1)
    for off in offsets:
        out.append(0x20 | (seg_index & 0xF))  # REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB
        out.extend(_uleb(off))
        out.append(0x51)  # REBASE_OPCODE_DO_REBASE_IMM_TIMES(1)
    out.append(0x00)  # REBASE_OPCODE_DONE
    return _pad(bytes(out), 4)


def _bind_stream(seg_index: int, binds: Iterable[tuple[int, str, int]]) -> bytes:
    """binds: list of (dylib_ordinal, symbol_name, offset_in_seg)."""
    out = bytearray()
    out.append(0x51)  # BIND_OPCODE_SET_TYPE_IMM(BIND_TYPE_POINTER=1)
    for ordinal, sym, off in binds:
        out.append(0x10 | (ordinal & 0xF))  # BIND_OPCODE_SET_DYLIB_ORDINAL_IMM
        out.append(0x40)  # BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM(0)
        out.extend(sym.encode("utf-8") + b"\x00")
        out.append(0x70 | (seg_index & 0xF))  # BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB
        out.extend(_uleb(off))
        out.append(0x90)  # BIND_OPCODE_DO_BIND
    out.append(0x00)  # BIND_OPCODE_DONE
    return _pad(bytes(out), 4)


def build_libguest_b() -> bytes:
    # _guest_b_init at 0x0400, _guest_b_get at 0x0420
    # __DATA at 0x1000:
    #   0x1000: _guest_b_state = 10
    #   0x1004: _guest_b_state_ptr = 0x00001000 (rebased)
    #   0x1008: __mod_init_func[0] = 0x00000400 (rebased)
    init_code = struct.pack(
        "<IIIIII",
        0xE28F0EBF,  # add r0, pc, #0xBF0   (0x0408 + 0xBF0 = 0x0FF8)
        0xE590000C,  # ldr r0, [r0, #12]    (*(0x1004) = _guest_b_state_ptr)
        0xE5901000,  # ldr r1, [r0]         (*_guest_b_state_ptr = 10)
        0xE281101E,  # add r1, r1, #30      (40)
        0xE5801000,  # str r1, [r0]
        0xE12FFF1E,  # bx lr
    )
    get_code = struct.pack(
        "<IIII",
        0xE28F0EBD,  # add r0, pc, #0xBD0   (0x0428 + 0xBD0 = 0x0FF8)
        0xE590000C,  # ldr r0, [r0, #12]    (*(0x1004) = _guest_b_state_ptr)
        0xE5900000,  # ldr r0, [r0]
        0xE12FFF1E,  # bx lr
    )

    rebase = _rebase_stream(1, [0x0004, 0x0008])
    symtab, strtab, nextdef, nundef = _build_symtab(
        exports=[
            ("_guest_b_init", 1, 0x0400),
            ("_guest_b_get", 1, 0x0420),
            ("_guest_b_state", 2, 0x1000),
        ],
        undefs=[],
    )

    linkedit_off = 0x2000
    rebase_off = linkedit_off
    symoff = rebase_off + len(rebase)
    stroff = symoff + len(symtab)
    linkedit_size = (stroff + len(strtab)) - linkedit_off

    cmds = [
        _segment_cmd(
            "__TEXT",
            0x0000,
            0x1000,
            0x0000,
            0x1000,
            7,
            5,
            [
                (
                    "__text",
                    "__TEXT",
                    0x0400,
                    0x0030,
                    0x0400,
                    2,
                    S_REGULAR | S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS,
                    0,
                    0,
                )
            ],
        ),
        _segment_cmd(
            "__DATA",
            0x1000,
            0x1000,
            0x1000,
            0x1000,
            7,
            3,
            [
                ("__data", "__DATA", 0x1000, 0x0008, 0x1000, 2, S_REGULAR, 0, 0),
                (
                    "__mod_init_func",
                    "__DATA",
                    0x1008,
                    0x0004,
                    0x1008,
                    2,
                    S_MOD_INIT_FUNC_POINTERS,
                    0,
                    0,
                ),
            ],
        ),
        _segment_cmd("__LINKEDIT", 0x2000, 0x1000, linkedit_off, linkedit_size, 7, 1, []),
        _dylib_cmd(LC_ID_DYLIB, "/usr/lib/libguest_b.dylib"),
        _dylib_cmd(LC_LOAD_DYLIB, "/usr/lib/libSystem.B.dylib"),
        struct.pack(
            "<IIIIIIIIIIII",
            LC_DYLD_INFO_ONLY,
            48,
            rebase_off,
            len(rebase),
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
        ),
        struct.pack("<IIIIII", LC_SYMTAB, 24, symoff, nextdef + nundef, stroff, len(strtab)),
        struct.pack(
            "<IIIIIIIIIIIIIIIIIIII",
            LC_DYSYMTAB,
            80,
            0,
            0,
            0,
            nextdef,
            nextdef,
            nundef,
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
        ),
    ]
    cmds_blob = b"".join(cmds)
    header = struct.pack(
        "<IIIIIII",
        MH_MAGIC,
        CPU_TYPE_ARM,
        CPU_SUBTYPE_ARM_V7,
        MH_DYLIB,
        len(cmds),
        len(cmds_blob),
        MH_NOUNDEFS | MH_DYLDLINK | MH_TWOLEVEL,
    )

    img = bytearray(0x2000)
    img[0 : len(header)] = header
    img[len(header) : len(header) + len(cmds_blob)] = cmds_blob
    img[0x0400 : 0x0400 + len(init_code)] = init_code
    img[0x0420 : 0x0420 + len(get_code)] = get_code
    struct.pack_into("<III", img, 0x1000, 10, 0x00001000, 0x00000400)
    img.extend(rebase)
    img.extend(symtab)
    img.extend(strtab)
    return bytes(img)


def build_libguest_a() -> bytes:
    # _guest_a_init at 0x0400, _guest_a_compute at 0x0420
    # __DATA at 0x1000:
    #   0x1000: _guest_a_val = 0
    #   0x1004: _guest_a_val_ptr = 0x00001000 (rebased)
    #   0x1008: __mod_init_func[0] = 0x00000400 (rebased)
    #   0x100C: __nl_symbol_ptr[0] = _guest_b_get (bound from ordinal 1)
    init_code = struct.pack(
        "<IIIIIIII",
        0xE92D4010,  # push {r4, lr}
        0xE28F4EBF,  # add r4, pc, #0xBF0   (0x040C + 0xBF0 = 0x0FFC)
        0xE5941010,  # ldr r1, [r4, #16]    (*(0x100C) = &_guest_b_get)
        0xE12FFF31,  # blx r1               (r0 = _guest_b_get() = 40)
        0xE2800002,  # add r0, r0, #2       (r0 = 42)
        0xE5941008,  # ldr r1, [r4, #8]     (*(0x1004) = _guest_a_val_ptr)
        0xE5810000,  # str r0, [r1]         (*_guest_a_val_ptr = 42)
        0xE8BD8010,  # pop {r4, pc}
    )
    compute_code = struct.pack(
        "<IIIII",
        0xE28F1EBD,  # add r1, pc, #0xBD0   (0x0428 + 0xBD0 = 0x0FF8)
        0xE591100C,  # ldr r1, [r1, #12]    (*(0x1004) = _guest_a_val_ptr)
        0xE5911000,  # ldr r1, [r1]         (*_guest_a_val_ptr = 42)
        0xE0810000,  # add r0, r1, r0       (42 + x)
        0xE12FFF1E,  # bx lr
    )

    rebase = _rebase_stream(1, [0x0004, 0x0008])
    bind = _bind_stream(1, [(1, "_guest_b_get", 0x000C)])
    symtab, strtab, nextdef, nundef = _build_symtab(
        exports=[
            ("_guest_a_init", 1, 0x0400),
            ("_guest_a_compute", 1, 0x0420),
            ("_guest_a_val", 2, 0x1000),
        ],
        undefs=[("_guest_b_get", 1)],
    )

    linkedit_off = 0x2000
    rebase_off = linkedit_off
    bind_off = rebase_off + len(rebase)
    symoff = bind_off + len(bind)
    stroff = symoff + len(symtab)
    linkedit_size = (stroff + len(strtab)) - linkedit_off

    cmds = [
        _segment_cmd(
            "__TEXT",
            0x0000,
            0x1000,
            0x0000,
            0x1000,
            7,
            5,
            [
                (
                    "__text",
                    "__TEXT",
                    0x0400,
                    0x0034,
                    0x0400,
                    2,
                    S_REGULAR | S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS,
                    0,
                    0,
                )
            ],
        ),
        _segment_cmd(
            "__DATA",
            0x1000,
            0x1000,
            0x1000,
            0x1000,
            7,
            3,
            [
                ("__data", "__DATA", 0x1000, 0x0008, 0x1000, 2, S_REGULAR, 0, 0),
                (
                    "__mod_init_func",
                    "__DATA",
                    0x1008,
                    0x0004,
                    0x1008,
                    2,
                    S_MOD_INIT_FUNC_POINTERS,
                    0,
                    0,
                ),
                (
                    "__nl_symbol_ptr",
                    "__DATA",
                    0x100C,
                    0x0004,
                    0x100C,
                    2,
                    S_NON_LAZY_SYMBOL_POINTERS,
                    0,
                    0,
                ),
            ],
        ),
        _segment_cmd("__LINKEDIT", 0x2000, 0x1000, linkedit_off, linkedit_size, 7, 1, []),
        _dylib_cmd(LC_ID_DYLIB, "@rpath/libguest_a.dylib"),
        _dylib_cmd(LC_LOAD_DYLIB, "/usr/lib/libguest_b.dylib"),
        _dylib_cmd(LC_LOAD_DYLIB, "/usr/lib/libSystem.B.dylib"),
        struct.pack(
            "<IIIIIIIIIIII",
            LC_DYLD_INFO_ONLY,
            48,
            rebase_off,
            len(rebase),
            bind_off,
            len(bind),
            0,
            0,
            0,
            0,
            0,
            0,
        ),
        struct.pack("<IIIIII", LC_SYMTAB, 24, symoff, nextdef + nundef, stroff, len(strtab)),
        struct.pack(
            "<IIIIIIIIIIIIIIIIIIII",
            LC_DYSYMTAB,
            80,
            0,
            0,
            0,
            nextdef,
            nextdef,
            nundef,
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
        ),
    ]
    cmds_blob = b"".join(cmds)
    header = struct.pack(
        "<IIIIIII",
        MH_MAGIC,
        CPU_TYPE_ARM,
        CPU_SUBTYPE_ARM_V7,
        MH_DYLIB,
        len(cmds),
        len(cmds_blob),
        MH_NOUNDEFS | MH_DYLDLINK | MH_TWOLEVEL,
    )

    img = bytearray(0x2000)
    img[0 : len(header)] = header
    img[len(header) : len(header) + len(cmds_blob)] = cmds_blob
    img[0x0400 : 0x0400 + len(init_code)] = init_code
    img[0x0420 : 0x0420 + len(compute_code)] = compute_code
    struct.pack_into("<IIII", img, 0x1000, 0, 0x00001000, 0x00000400, 0)
    img.extend(rebase)
    img.extend(bind)
    img.extend(symtab)
    img.extend(strtab)
    return bytes(img)


def build_executable() -> bytes:
    # __PAGEZERO at 0x0000..0x1000
    # __TEXT at 0x1000..0x2000 (fileoff 0x0000)
    #   0x1400 (file 0x0400): _exe_init sets *(0x2000) = 5
    #   0x1420 (file 0x0420): _main calls _guest_a_compute(*(0x2000)) -> 47
    # __DATA at 0x2000..0x3000 (fileoff 0x1000)
    #   0x2000: _exe_bonus = 0
    #   0x2004: __mod_init_func[0] = 0x00001400
    #   0x2008: __nl_symbol_ptr[0] = _guest_a_compute (bound from ordinal 1)
    init_code = struct.pack(
        "<IIII",
        0xE28F0EBF,  # add r0, pc, #0xBF0   (0x1408 + 0xBF0 = 0x1FF8)
        0xE3A01005,  # mov r1, #5
        0xE5801008,  # str r1, [r0, #8]     (*(0x2000) = 5)
        0xE12FFF1E,  # bx lr
    )
    main_code = struct.pack(
        "<IIIIII",
        0xE92D4010,  # push {r4, lr}
        0xE28F4EBD,  # add r4, pc, #0xBD0   (0x142C + 0xBD0 = 0x1FFC)
        0xE5940004,  # ldr r0, [r4, #4]     (*(0x2000) = 5)
        0xE594100C,  # ldr r1, [r4, #12]    (*(0x2008) = &_guest_a_compute)
        0xE12FFF31,  # blx r1               (r0 = _guest_a_compute(5) = 47)
        0xE8BD8010,  # pop {r4, pc}
    )

    rebase = _rebase_stream(2, [0x0004])
    bind = _bind_stream(2, [(1, "_guest_a_compute", 0x0008)])
    symtab, strtab, nextdef, nundef = _build_symtab(
        exports=[
            ("_exe_init", 1, 0x1400),
            ("_main", 1, 0x1420),
        ],
        undefs=[("_guest_a_compute", 1)],
    )

    linkedit_off = 0x2000
    rebase_off = linkedit_off
    bind_off = rebase_off + len(rebase)
    symoff = bind_off + len(bind)
    stroff = symoff + len(symtab)
    linkedit_size = (stroff + len(strtab)) - linkedit_off

    cmds = [
        _segment_cmd("__PAGEZERO", 0x0000, 0x1000, 0, 0, 0, 0, []),
        _segment_cmd(
            "__TEXT",
            0x1000,
            0x1000,
            0x0000,
            0x1000,
            7,
            5,
            [
                (
                    "__text",
                    "__TEXT",
                    0x1400,
                    0x0038,
                    0x0400,
                    2,
                    S_REGULAR | S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS,
                    0,
                    0,
                )
            ],
        ),
        _segment_cmd(
            "__DATA",
            0x2000,
            0x1000,
            0x1000,
            0x1000,
            7,
            3,
            [
                ("__data", "__DATA", 0x2000, 0x0004, 0x1000, 2, S_REGULAR, 0, 0),
                (
                    "__mod_init_func",
                    "__DATA",
                    0x2004,
                    0x0004,
                    0x1004,
                    2,
                    S_MOD_INIT_FUNC_POINTERS,
                    0,
                    0,
                ),
                (
                    "__nl_symbol_ptr",
                    "__DATA",
                    0x2008,
                    0x0004,
                    0x1008,
                    2,
                    S_NON_LAZY_SYMBOL_POINTERS,
                    0,
                    0,
                ),
            ],
        ),
        _segment_cmd("__LINKEDIT", 0x3000, 0x1000, linkedit_off, linkedit_size, 7, 1, []),
        _dylib_cmd(LC_LOAD_DYLIB, "@rpath/libguest_a.dylib"),
        _dylib_cmd(LC_LOAD_DYLIB, "/usr/lib/libSystem.B.dylib"),
        struct.pack("<IIQQ", LC_MAIN, 24, 0x0420, 0),
        struct.pack(
            "<IIIIIIIIIIII",
            LC_DYLD_INFO_ONLY,
            48,
            rebase_off,
            len(rebase),
            bind_off,
            len(bind),
            0,
            0,
            0,
            0,
            0,
            0,
        ),
        struct.pack("<IIIIII", LC_SYMTAB, 24, symoff, nextdef + nundef, stroff, len(strtab)),
        struct.pack(
            "<IIIIIIIIIIIIIIIIIIII",
            LC_DYSYMTAB,
            80,
            0,
            0,
            0,
            nextdef,
            nextdef,
            nundef,
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
        ),
    ]
    cmds_blob = b"".join(cmds)
    header = struct.pack(
        "<IIIIIII",
        MH_MAGIC,
        CPU_TYPE_ARM,
        CPU_SUBTYPE_ARM_V7,
        MH_EXECUTE,
        len(cmds),
        len(cmds_blob),
        MH_NOUNDEFS | MH_DYLDLINK | MH_TWOLEVEL | MH_PIE,
    )

    img = bytearray(0x2000)
    img[0 : len(header)] = header
    img[len(header) : len(header) + len(cmds_blob)] = cmds_blob
    img[0x0400 : 0x0400 + len(init_code)] = init_code
    img[0x0420 : 0x0420 + len(main_code)] = main_code
    struct.pack_into("<III", img, 0x1000, 0, 0x00001400, 0)
    img.extend(rebase)
    img.extend(bind)
    img.extend(symtab)
    img.extend(strtab)
    return bytes(img)


INFO_PLIST = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>GuestDylibFixture</string>
    <key>CFBundleIdentifier</key>
    <string>com.hydrogenvm.test.guestdylib</string>
    <key>CFBundleName</key>
    <string>GuestDylibFixture</string>
    <key>CFBundleVersion</key>
    <string>1.0</string>
</dict>
</plist>
"""


def generate_fixture(out_dir: Path, malformed: bool = False) -> dict[str, Path]:
    app_dir = out_dir / "GuestDylibFixture.app"
    lib_dir = out_dir / "guestlibs"
    app_dir.mkdir(parents=True, exist_ok=True)
    lib_dir.mkdir(parents=True, exist_ok=True)

    plist_path = app_dir / "Info.plist"
    exe_path = app_dir / "GuestDylibFixture"
    lib_a_path = lib_dir / "libguest_a.dylib"
    lib_b_path = lib_dir / "libguest_b.dylib"

    plist_path.write_text(INFO_PLIST, encoding="utf-8")
    exe_path.write_bytes(build_executable())
    if malformed:
        lib_a_path.write_bytes(b"\xce\xfa\xed\xfe\x0c\x00\x00\x00corrupt-macho-dylib")
    else:
        lib_a_path.write_bytes(build_libguest_a())
    lib_b_path.write_bytes(build_libguest_b())

    return {
        "app": app_dir,
        "exe": exe_path,
        "lib_dir": lib_dir,
        "lib_a": lib_a_path,
        "lib_b": lib_b_path,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path, help="Output directory")
    parser.add_argument(
        "--malformed",
        action="store_true",
        help="Write a truncated/corrupt libguest_a.dylib to test honest error reporting",
    )
    args = parser.parse_args()
    generate_fixture(args.out, malformed=args.malformed)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
