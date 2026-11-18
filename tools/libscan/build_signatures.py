"""Build pinned historical library sources and verify the derived ARM signatures."""

import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tarfile
import urllib.request

from scan import Signature, elf_headers, elf_sections, scan_binary, span


# Release tags (or a commit for Tremor, whose releases used an SVN checkout).
TREMOR = "18a673fc97d95dffff3ccdeea7d493b2a6ab4481"
SOURCES = {
    ("zlib", "1.2.8"): ("madler/zlib", "refs/tags/v1.2.8", "e380bd1bdb6447508beaa50efc653fe45f4edc1dafe11a251ae093e0ee97db9a"),
    ("zlib", "1.2.7"): ("madler/zlib", "refs/tags/v1.2.7", "4d23292a2de63365795b4ae8249fd9415df60cce7120baa89576cbee5f09b3d7"),
    ("libpng", "1.6.16"): ("pnggroup/libpng", "refs/tags/v1.6.16", "6b4aeacdda7ba0047e15848d0ec6af0425961e07ebb1b913ed55005bf463945c"),
    ("libpng", "1.6.37"): ("pnggroup/libpng", "refs/tags/v1.6.37", "ca74a0dace179a8422187671aee97dd3892b53e168627145271cad5b5ac81307"),
    ("libvorbis", "1.3.5"): ("xiph/vorbis", "refs/tags/v1.3.5", "8a189c02c050e4006552e8d565cd52a2b6034ce5b9ddc1582994684d8a6e4066"),
    ("tremor", TREMOR): ("sezero/tremor", TREMOR, "824b274b9decedbe6889bd66c0925b7c0c612497e90e058a648b9216250d2ff3"),
    ("libogg", "1.3.2"): ("xiph/ogg", "refs/tags/v1.3.2", "6f0a276f697b427dea3535317322433656cb65473d0b08fbf3c163b15d70d986"),
}
TARGETS = {
    "zlib": {"inflate.c": ("inflate",), "inffast.c": ("inflate_fast",),
             "crc32.c": ("crc32",), "adler32.c": ("adler32",)},
    "libpng": {"pngrutil.c": tuple("png_read_filter_row_" + variant for variant in
                                  ("sub", "up", "avg", "paeth_1byte_pixel", "paeth_multibyte_pixel"))},
    "libvorbis": {"lib/synthesis.c": ("vorbis_synthesis",), "lib/mdct.c": ("mdct_backward",)},
    "tremor": {"synthesis.c": ("vorbis_synthesis", "_vorbis_synthesis1"), "mdct.c": ("mdct_backward",)},
}
# llvm-objdump prints A32 words or Thumb halfwords in processor order.
INSTRUCTION = re.compile(r"^\s*([0-9a-f]+):\s+([0-9a-f]{4,8})(?: ([0-9a-f]{4}))?\s+(\S+)\s*(.*)$")
SECTION = re.compile(r"^Disassembly of section (.+):$")


def run(*args):
    return subprocess.run(args, check=True, stdout=subprocess.PIPE, text=True).stdout


def unpack(key, work):
    project, ref, digest = SOURCES[key]
    name = "-".join(key)
    archive = work / (name + ".tgz")
    if not archive.exists():
        with urllib.request.urlopen(f"https://codeload.github.com/{project}/tar.gz/{ref}", timeout=60) as source:
            archive.write_bytes(source.read())
    if hashlib.sha256(archive.read_bytes()).hexdigest() != digest:
        raise ValueError(f"source archive checksum mismatch for {name}")
    destination = work / name
    if not destination.is_dir():
        with tarfile.open(archive) as tar:
            folders = {member.name.split("/")[0] for member in tar.getmembers()}
            if len(folders) != 1:
                raise ValueError(f"expected one archive root for {name}")
            tar.extractall(work, filter="data")
        extracted = work / folders.pop()
        if extracted != destination:
            extracted.rename(destination)
    return destination


def cstring(table, index):
    end = table.index(b"\0", index)
    return table[index:end].decode()


def symbols(data, sections):
    for section in sections:
        if section[1] != 2:  # SHT_SYMTAB
            continue
        strings = sections[section[6]]
        names = span(data, strings[4], strings[5])
        records = span(data, section[4], section[5])
        for index, value, size, info, _, ndx in struct.iter_unpack("<IIIBBH", records):
            if ndx and ndx < len(sections) and info & 15 == 2 and size:
                yield cstring(names, index), value, size, ndx


def instruction_bits(word, half, mnemonic, operands):
    """Bits that survive a change to a PC-relative target."""
    thumb = len(word) == 4
    op = mnemonic.split(".")[0]
    raw = int(word, 16)
    if not thumb:
        if raw & 0x0E000000 == 0x0A000000:  # B/BL/BLX immediate
            return (0xFE000000 if raw >> 28 == 15 else 0xFF000000).to_bytes(4, "little")
        if "[pc" in operands and raw & 0x0E000000 == 0x04000000:  # literal LDR
            return (0xFF7FF000).to_bytes(4, "little")
        if op.startswith("adr") or re.search(r"^r\w+, pc, #", operands):
            return (0xFE0FF000).to_bytes(4, "little")
        return b"\xff" * 4
    if half is None:
        if raw & 0xF800 == 0xE000:
            return (0xF800).to_bytes(2, "little")
        if raw & 0xF000 == 0xD000 and raw & 0x0F00 < 0x0E00:
            return (0xFF00).to_bytes(2, "little")
        if raw & 0xF500 == 0xB100:  # CBZ / CBNZ
            return (0xFD07).to_bytes(2, "little")
        if raw & 0xF800 in (0x4800, 0xA000):  # LDR literal or ADR
            return (0xFF00).to_bytes(2, "little")
        return b"\xff" * 2
    if re.fullmatch(r"b(?:l|lx|eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)?", op) and not operands.startswith("r"):
        first = 0xF800 if op in ("b", "bl", "blx") else 0xFBC0
        second = 0xD000 if op not in ("bl", "blx") else 0xC000
        return struct.pack("<HH", first, second)
    if op.startswith("ldr") and "[pc" in operands:
        return struct.pack("<HH", 0xFF7F, 0xF000)
    if op == "adr" or re.search(r"^r\w+, pc, #", operands):
        return struct.pack("<HH", 0xFBEF, 0x8F00)
    return b"\xff" * 4


def object_patterns(path, objdump, purpose):
    data = path.read_bytes()
    header = elf_headers(data)
    sections = elf_sections(data, header)
    names = sections[header[12]]
    names = span(data, names[4], names[5])
    masks = defaultdict(list)
    section = ""
    for line in run(objdump, "-d", path).splitlines():
        match = SECTION.match(line)
        if match:
            section = match[1]
        match = INSTRUCTION.match(line)
        if match:
            offset, word, half, mnemonic, operands = match.groups()
            masks[section].append((int(offset, 16), instruction_bits(word, half, mnemonic, operands)))
    relocations = defaultdict(list)
    for section in sections:
        if section[1] != 9:  # SHT_REL
            continue
        for offset, info in struct.iter_unpack("<II", span(data, section[4], section[5])):
            relocations[section[7]].append((offset, info & 255))
    seen = set()
    for name, value, size, ndx in symbols(data, sections):
        if name not in purpose:
            continue
        section = sections[ndx]
        start = value & ~1
        raw = span(data, section[4] + start, size)
        mask = bytearray(b"\xff" * size)
        for offset, bits in masks[cstring(names, section[0])]:
            if start <= offset and offset + len(bits) <= start + size:
                mask[offset - start:offset - start + len(bits)] = bits
        for offset, kind in relocations[ndx]:
            if start <= offset <= start + size - 4:
                # Preserve the instruction for branch relocations already handled above.
                # Mark a relocated word (ABS/REL/GOT, literal pool, MOVW/MOVT) as variable.
                if kind not in (1, 10, 28, 29, 30, 51):
                    mask[offset - start:offset - start + 4] = b"\0" * 4
        # A bounded function prefix is enough for the long functions. The
        # short Tremor call stub also needs its destination's full signature.
        size = min(size, 256)
        yield name, "thumb" if value & 1 else "arm", bytes(b & m for b, m in zip(raw[:size], mask)), bytes(mask[:size])
        seen.add(name)
    if seen != set(purpose):
        raise ValueError(f"{path}: missing compiled symbols {set(purpose) - seen}")


def macho_text(data):
    _, _, _, _, num_cmds, cmd_len, _ = struct.unpack_from("<7I", data)
    span(data, 28, cmd_len)
    offset = 28
    sections = []
    symtab = None
    for _ in range(num_cmds):
        kind, size = struct.unpack_from("<II", data, offset)
        if kind == 1:  # LC_SEGMENT
            section_count, = struct.unpack_from("<I", data, offset + 48)
            for i in range(section_count):
                sections.append(struct.unpack_from("<16s16s9I", data, offset + 56 + 68 * i))
        elif kind == 2:  # LC_SYMTAB
            symtab = struct.unpack_from("<4I", data, offset + 8)
        offset += size
    text_index, code = next((i, row) for i, row in enumerate(sections, 1)
                            if row[0].rstrip(b"\0") == b"__text" and row[1].rstrip(b"\0") == b"__TEXT")
    _, _, address, size, start, _, reloc_start, reloc_count, *_ = code
    syms_at, num_syms, names_at, names_size = symtab
    names = span(data, names_at, names_size)
    symbols = {}
    for index, tag, section, desc, value in struct.iter_unpack("<IBBHI", span(data, syms_at, 12 * num_syms)):
        if tag & 0xEE == 0x0E and section == text_index:
            name = cstring(names, index)
            if name.startswith("_"):
                symbols[name[1:]] = value, "thumb" if desc & 8 else "arm"
    relocations = {}
    for word, flags in struct.iter_unpack("<II", span(data, reloc_start, 8 * reloc_count)):
        kind = (word >> 24) & 15 if word >> 31 else flags >> 28
        if kind == 1:  # ARM_RELOC_PAIR is the second half of the previous record.
            continue
        relative = word & 0xFFFFFF if word >> 31 else word
        if kind not in (5, 6) and relative < size:  # Branch immediates use the instruction mask.
            relocations[relative] = 4
    return address, span(data, start, size), symbols, relocations


def macho_patterns(path, objdump, purpose, isa):
    address, code, symbols, relocations = macho_text(path.read_bytes())
    bounds = sorted({value - address for value, _ in symbols.values()} | {len(code)})
    masks = []
    triple = ("thumb" if isa == "thumb" else "arm") + "v7-apple-ios"
    for line in run(objdump, "-d", "--triple=" + triple, "--section=__text", path).splitlines():
        match = INSTRUCTION.match(line)
        if match:
            offset, word, half, mnemonic, operands = match.groups()
            masks.append((int(offset, 16) - address, instruction_bits(word, half, mnemonic, operands)))
    for name in purpose:
        value, mode = symbols[name]
        if mode != isa:
            raise ValueError(f"{path}: unexpected instruction set for {name}")
        start = value - address
        end = next(offset for offset in bounds if offset > start)
        mask = bytearray(b"\xff" * (end - start))
        for offset, bits in masks:
            if start <= offset and offset + len(bits) <= end:
                mask[offset - start:offset - start + len(bits)] = bits
        for offset, size in relocations.items():
            if start <= offset and offset + size <= end:
                mask[offset - start:offset - start + size] = bytes(size)
        # The compiler may pad a short wrapper to align its next function.
        if name == "vorbis_synthesis" and len(mask) <= 8:
            end = min(end, start + (8 if isa == "arm" else 6))
        end = min(end, start + 256)
        mask = bytes(mask[:end - start])
        yield name, isa, bytes(raw & bits for raw, bits in zip(code[start:end], mask)), mask


def check(binary, expected, signatures, strip):
    data = binary.read_bytes()
    header = elf_headers(data)
    stripped = binary.with_name(binary.stem + ".stripped" + binary.suffix)
    if header:
        by_symbol = {(name, value & ~1, "thumb" if value & 1 else "arm")
                     for name, value, _, _ in symbols(data, elf_sections(data, header)) if name in expected}
        run(strip, "--strip-all", "-o", stripped, binary)
    else:
        _, _, names, _ = macho_text(data)
        by_symbol = {(name, value, isa) for name, (value, isa) in names.items() if name in expected}
        run(strip, "-x", "-S", "-o", stripped, binary)
    if {name for name, *_ in by_symbol} != expected:
        raise ValueError(f"{binary}: incomplete linker symbol oracle")
    by_signature = {(hit["function"], hit["address"], hit["isa"])
                    for hit in scan_binary(stripped.read_bytes(), signatures)}
    if by_signature != by_symbol:
        raise ValueError(f"{binary}: missing {by_symbol - by_signature}, unexpected {by_signature - by_symbol}")
    return len(by_symbol)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, required=True, help="external scratch directory for archives, objects and test binaries")
    parser.add_argument("--out", type=Path, default=Path(__file__).with_name("signatures.json"))
    parser.add_argument("--sysroot", type=Path, required=True, help="ARM hard-float cross sysroot containing include/")
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--gcc", default="arm-linux-gnueabihf-gcc")
    parser.add_argument("--objdump", default="llvm-objdump")
    parser.add_argument("--ld", default="ld.lld")
    parser.add_argument("--strip", default="llvm-strip")
    parser.add_argument("--ld64", type=Path, required=True, help="cctools ld64 with ARMv7 support")
    parser.add_argument("--ios-libs", type=Path, required=True, help="Test Lab stub SDK usr/lib directory")
    args = parser.parse_args()
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    source_dirs = {key: unpack(key, work) for key in SOURCES}
    ogg = source_dirs["libogg", "1.3.2"] / "include"
    (ogg / "ogg/config_types.h").write_text(
        "#include <stdint.h>\ntypedef int16_t ogg_int16_t;\ntypedef uint16_t ogg_uint16_t;\n"
        "typedef int32_t ogg_int32_t;\ntypedef uint32_t ogg_uint32_t;\ntypedef int64_t ogg_int64_t;\n")
    for (project, _), source in source_dirs.items():
        if project == "libpng":
            shutil.copyfile(source / "scripts/pnglibconf.h.prebuilt", source / "pnglibconf.h")

    ios = work / "_ios_headers"
    (ios / "sys").mkdir(parents=True, exist_ok=True)
    (ios / "sys/types.h").write_text("#pragma once\ntypedef long long off_t;\n")
    (ios / "fp.h").write_text("#include <math.h>\n")
    (ios / "libscan.h").write_text("double rint(double);\n#define alloca __builtin_alloca\n")
    repo = Path(__file__).resolve().parents[2]
    programs = {
        "clang-elf": [args.clang, "--target=arm-linux-gnueabihf"],
        "gcc-elf": [args.gcc],
        "clang-ios": [args.clang, "--target=armv7-apple-ios6.0"],
    }
    arm = ["-std=gnu99", "-march=armv7-a", "-mfpu=vfpv3-d16", "-fPIC", "-fno-stack-protector"]
    linux = ["--sysroot=" + str(args.sysroot), "-isystem", str(args.sysroot / "include"), "-mfloat-abi=hard", "-ffunction-sections"]
    darwin = ["-ffreestanding", f"-I{ios}", f"-I{repo / 'tests/ios/include'}", "-include", str(ios / "libscan.h")]
    patterns = defaultdict(lambda: defaultdict(set))
    binaries = []
    for tool, cc in programs.items():
        macho = tool.endswith("-ios")
        common = arm + (darwin if macho else linux)
        for opt in ("-O2", "-Os"):
            for isa in ("arm", "thumb"):
                for (project, version), source in source_dirs.items():
                    if project not in TARGETS:
                        continue
                    build = work / "_build" / f"{project}-{version}-{tool}-{opt[1:]}-{isa}"
                    build.mkdir(parents=True, exist_ok=True)
                    expected, objects = set(), []
                    for file, targets in TARGETS[project].items():
                        obj = build / (Path(file).stem + ".o")
                        include = [f"-I{ogg}", f"-I{source / 'include'}", f"-I{source_dirs['zlib', '1.2.8']}"]
                        if project == "tremor":
                            include.append("-DVAR_ARRAYS")
                        run(*cc, *common, opt, "-m" + isa, *include, "-c", source / file, "-o", obj)
                        derived = (macho_patterns(obj, args.objdump, targets, isa) if macho else
                                   object_patterns(obj, args.objdump, targets))
                        for name, mode, value, mask in derived:
                            patterns[project, name, mode, value, mask][version].add(f"{tool}:{opt}:{mode}")
                            if not name.startswith("_"):
                                expected.add(name)
                        objects.append(obj)
                    binary = build / ("fixture.dylib" if macho else "fixture.so")
                    if macho:
                        run(args.ld64, "-arch", "armv7", "-dylib", "-undefined", "dynamic_lookup",
                            "-iphoneos_version_min", "6.0", "-syslibroot", args.ios_libs.parent.parent, "-L" + str(args.ios_libs), "-lSystem.B", "-o", binary, *objects)
                    else:
                        run(args.ld, "-m", "armelf_linux_eabi", "-shared", "-o", binary, *objects)
                    binaries.append((binary, expected))
    signatures, rows = [], []
    for (project, name, isa, value, mask), versions in sorted(patterns.items()):
        options = {}
        stable_bits = sum(byte.bit_count() for byte in mask)
        if project == "tremor" and name == "vorbis_synthesis" and len(value) == (8 if isa == "arm" else 6):
            options["target"] = "_vorbis_synthesis1"
        elif stable_bits < 80:
            raise ValueError(f"too few stable bits in {project}:{name}:{isa}: {stable_bits}")
        if name.startswith("_"):
            options["internal"] = True
        signatures.append(Signature(project, name, tuple(sorted(versions)), isa, value, mask, **options))
        rows.append({"library": project, "function": name, "isa": isa, "versions": sorted(versions),
                     "profiles": {version: sorted(profiles) for version, profiles in sorted(versions.items())},
                     "value": value.hex(), "mask": mask.hex(), **options})
    count = sum(check(binary, expected, signatures, args.strip) for binary, expected in binaries)
    negative = work / "_build/negative.c"
    negative.write_text("unsigned crc32(unsigned a, unsigned b) { return (a ^ b) * 31; }\n"
                        "unsigned adler32(unsigned a) { return a + 7; }\n")
    run(*programs["clang-elf"], *arm, *linux, "-O2", "-c", negative, "-o", negative.with_suffix(".o"))
    run(args.ld, "-m", "armelf_linux_eabi", "-shared", "-o", negative.with_suffix(".so"), negative.with_suffix(".o"))
    if scan_binary(negative.with_suffix(".so").read_bytes(), signatures):
        raise ValueError("negative fixture matched a signature")
    tools = {tool: run(cc[0], "--version").splitlines()[0] for tool, cc in programs.items()}
    db = {"schema": 1, "compilers": tools, "signatures": rows}
    args.out.write_text(json.dumps(db, indent=2) + "\n")
    print(f"Verified {count} symbol addresses across {len(binaries)} stripped ELF and Mach-O files and one negative fixture")
    print(f"Wrote {len(rows)} distinct signatures")


if __name__ == "__main__":
    main()
