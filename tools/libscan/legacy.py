"""Corroborate bundled code in older Mach-O files using symbols and literals."""

from bisect import bisect_right
from collections import defaultdict
from dataclasses import dataclass
import struct

ARM_SUBTYPES = {0, 6, 9, 10, 11, 12}
MACH_FAT = (b"\xca\xfe\xba\xbe", b"\xca\xfe\xba\xbf",
            b"\xbe\xba\xfe\xca", b"\xbf\xba\xfe\xca")
MACH_THIN = b"\xce\xfa\xed\xfe"

# Diagnostic strings from upstream zlib inflate.c/inffast.c and libpng
# pngread.c/pngrutil.c. Check references to the exact terminated strings.
ANCHORS = {
    "z_header": b"incorrect header check\0",
    "z_method": b"unknown compression method\0",
    "z_window": b"invalid window size\0",
    "z_distance": b"invalid distance code\0",
    "z_length": b"invalid literal/length code\0",
    "png_header": b"Not a PNG file\0",
    "png_order": b"Missing IHDR before IDAT\0",
    "png_ihdr": b"Invalid IHDR chunk\0",
    "png_ihdr_order": b"Out of place IHDR\0",
    "png_filter": b"Ignoring bad adaptive filter type\0",
    # First eight entries of the IEEE reflected CRC-32 lookup table.
    "z_crc": struct.pack("<8I", 0, 0x77073096, 0xEE0E612C, 0x990951BA,
                        0x076DC419, 0x706AF48F, 0xE963A535, 0x9E6495A3),
}
NAMED = {
    "zlib": {"inflate", "inflate_fast", "crc32", "adler32"},
    "libpng": {"png_read_info", "png_read_row", "png_read_filter_row", "png_handle_IHDR",
               "png_read_filter_row_sub", "png_read_filter_row_up", "png_read_filter_row_avg",
               "png_read_filter_row_paeth_1byte_pixel", "png_read_filter_row_paeth_multibyte_pixel"},
    # The C names for libvorbis and Tremor overlap. Their byte signatures
    # distinguish the implementations, so symbol names alone are ambiguous.
}
SYMBOL_LIB = {name: library for library, names in NAMED.items() for name in names}

U32 = struct.Struct("<I")
U16 = struct.Struct("<H")


def u32(data, at):
    return U32.unpack_from(data, at)[0]


def u16(data, at):
    return U16.unpack_from(data, at)[0]

def region(data, offset, size):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise ValueError("region extends beyond Mach-O image")
    return data[offset:offset + size]


def macho_slices(data):
    if data[:4] == MACH_THIN:
        yield data
    elif data[:4] in MACH_FAT:
        endian = ">" if data[0] == 0xCA else "<"
        fmt = endian + ("IIQQII" if 0xBF in data[:4] else "IIIII")
        count = struct.unpack_from(endian + "I", data, 4)[0]
        for cpu, subtype, offset, size, *_ in struct.iter_unpack(
                fmt, region(data, 8, struct.calcsize(fmt) * count)):
            if cpu == 12 and (subtype & 0xFFFFFF) in ARM_SUBTYPES:
                yield region(data, offset, size)


@dataclass(frozen=True)
class Text:
    address: int
    data: bytes


def function_starts(encoded, address):
    starts = {}
    delta = shift = 0
    for byte in encoded:
        delta |= (byte & 0x7F) << shift
        if byte & 0x80:
            shift += 7
            if shift >= 35:
                raise ValueError("invalid Mach-O function start offset")
            continue
        if not delta:
            break
        address += delta
        if address >= 2**32:
            raise ValueError("Mach-O function start exceeds ARM address space")
        starts[address & ~1] = "thumb" if address & 1 else "arm"
        delta = shift = 0
    return starts


def macho_metadata(data):
    _, cpu, subtype, _, count, command_bytes, _ = struct.unpack_from("<7I", data)
    if cpu != 12 or (subtype & 0xFFFFFF) not in ARM_SUBTYPES:
        return None
    commands_end = 28 + len(region(data, 28, command_bytes))
    sections = []
    symtab = starts = None
    text_base = 0
    offset = 28
    for _ in range(count):
        kind, size = struct.unpack_from("<II", data, offset)
        if size < 8 or offset + size > commands_end:
            raise ValueError("invalid Mach-O load command size")
        if kind == 1:  # LC_SEGMENT
            if size < 56:
                raise ValueError("invalid Mach-O segment size")
            segment, address, _, _, _, _, flags, nsects, _ = struct.unpack_from("<16s8I", data, offset + 8)
            if 56 + 68 * nsects > size:
                raise ValueError("invalid Mach-O section count")
            if segment.rstrip(b"\0") == b"__TEXT":
                text_base = address
            for i in range(nsects):
                name, _, address, length, start, _, _, _, section_flags, _, _ = struct.unpack_from(
                    "<16s16s9I", data, offset + 56 + i * 68)
                executable = bool(flags & 4 and section_flags & 0x80000400)
                sections.append((name.rstrip(b"\0"), address, length, start, section_flags, executable))
        elif kind == 2 and size >= 24:  # LC_SYMTAB
            symtab = struct.unpack_from("<4I", data, offset + 8)
        elif kind == 0x26 and size >= 16:  # LC_FUNCTION_STARTS
            starts = struct.unpack_from("<2I", data, offset + 8)
        elif kind == 0x21 and size >= 20 and u32(data, offset + 16):  # LC_ENCRYPTION_INFO
            return None
        offset += size

    text = next((Text(address, region(data, start, length)) for name, address, length, start, _, executable
                in sections if name == b"__text" and executable and length), None)
    if text is None:
        return None
    boundaries = function_starts(region(data, *starts), text_base) if starts else {}
    symbols = {}
    if symtab:
        at, number, names_at, names_size = symtab
        names = region(data, names_at, names_size)
        for name_at, flags, section, desc, address in struct.iter_unpack("<IBBHI", region(data, at, number * 12)):
            # Defined n_sect entries in __text. Undefined imports and stubs
            # have no bearing on whether a static copy is present.
            if flags & 0xEE != 0x0E or not 0 < section <= len(sections) or sections[section - 1][0] != b"__text":
                continue
            end = names.find(b"\0", name_at)
            if end < 0 or names[name_at:name_at + 1] != b"_":
                continue
            name = names[name_at + 1:end].decode("ascii", errors="replace")
            if name in SYMBOL_LIB and text.address <= (address & ~1) < text.address + len(text.data):
                symbols[name] = (address & ~1, "thumb" if (desc & 8 or address & 1) else "arm")
    anchors = defaultdict(set)
    for _, address, length, start, flags, _ in sections:
        if not start or not length or flags & 255 in (1, 12, 18):  # zero-fill sections
            continue
        if start + length > len(data):
            raise ValueError("section extends beyond Mach-O image")
        for name, pattern in ANCHORS.items():
            position = start
            while (position := data.find(pattern, position, start + length)) != -1:
                anchors[address + position - start].add(name)
                position += 1
    return text, boundaries, symbols, anchors


def references(text, isa, anchors):
    """Resolve common ARMv6/7 and Thumb PC-relative constants without emulation."""
    data, base = text.data, text.address
    values = {}
    refs = defaultdict(set)

    def use(pc, value):
        if value in anchors:
            refs[pc].update(anchors[value])

    def store(reg, pc, value):
        if value is not None:
            values[reg] = pc, value
            use(pc, value)

    def loaded(reg, pc):
        at, value = values.get(reg, (-100, 0))
        return value if pc - at <= 64 else None

    def literal(offset):
        return u32(data, offset) if 0 <= offset <= len(data) - 4 else None

    if isa == "arm":
        for offset, (word,) in enumerate(U32.iter_unpack(data[:len(data) & ~3])):
            pc = base + 4 * offset
            if word & 0x0F7F0000 == 0x051F0000:  # LDR literal
                imm = word & 4095
                store((word >> 12) & 15, pc, literal(4 * offset + 8 + (imm if word & 0x800000 else -imm)))
            elif word & 0x0FE00FF0 == 0x00800000:  # ADD register, one operand is PC
                first, second = (word >> 16) & 15, word & 15
                reg = second if first == 15 else first if second == 15 else None
                value = loaded(reg, pc) if reg is not None else None
                if value is not None:
                    use(pc, (pc + 8 + value) & 0xFFFFFFFF)
            elif word & 0x0FFF0000 in (0x028F0000, 0x024F0000):  # ADR
                imm, rotation = word & 255, (word >> 7) & 30
                imm = ((imm >> rotation) | (imm << (32 - rotation))) & 0xFFFFFFFF
                use(pc, (pc + 8 + (imm if word & 0x00400000 == 0 else -imm)) & 0xFFFFFFFF)
            elif word & 0x0FF00000 in (0x03000000, 0x03400000):  # MOVW/MOVT
                reg, imm = (word >> 12) & 15, ((word >> 4) & 0xF000) | (word & 4095)
                if word & 0x00400000 == 0:
                    store(reg, pc, imm)
                elif (low := loaded(reg, pc)) is not None:
                    store(reg, pc, (imm << 16) | (low & 0xFFFF))
    else:
        for offset, (half,) in enumerate(U16.iter_unpack(data[:len(data) & ~1])):
            pc = base + 2 * offset
            aligned_pc = (pc + 4) & ~3
            if half & 0xF800 == 0x4800:  # LDR literal T1
                store((half >> 8) & 7, pc, literal(aligned_pc - base + (half & 255) * 4))
            elif half & 0xFF00 == 0x4400 and (half >> 3) & 15 == 15:  # ADD register, PC source
                reg = (half & 7) | ((half >> 4) & 8)
                if (value := loaded(reg, pc)) is not None:
                    use(pc, (pc + 4 + value) & 0xFFFFFFFF)
            elif half & 0xF800 == 0xA000:  # ADR T1
                use(pc, aligned_pc + (half & 255) * 4)
            elif 2 * offset + 4 <= len(data):
                if half & 0xFF7F == 0xF85F:  # LDR literal T2
                    second = u16(data, 2 * offset + 2)
                    imm = second & 4095
                    store(second >> 12, pc, literal(aligned_pc - base + (imm if half & 128 else -imm)))
                elif half & 0xFBF0 in (0xF240, 0xF2C0):  # MOVW/MOVT
                    second = u16(data, 2 * offset + 2)
                    reg = (second >> 8) & 15
                    imm = ((half & 15) << 12) | ((half & 0x400) << 1) | ((second & 0x7000) >> 4) | (second & 255)
                    if half & 0x80 == 0:
                        store(reg, pc, imm)
                    elif (low := loaded(reg, pc)) is not None:
                        store(reg, pc, (imm << 16) | (low & 0xFFFF))
    return refs


def prologues(text, isa):
    if isa == "arm":
        return [text.address + 4 * index for index, (word,) in enumerate(
                U32.iter_unpack(text.data[:len(text.data) & ~3])) if word & 0xFFFF4000 == 0xE92D4000]
    return [text.address + 2 * index for index, (half,) in enumerate(
            U16.iter_unpack(text.data[:len(text.data) & ~1])) if half & 0xFF00 == 0xB500]


def signed(value, bits):
    sign = 1 << (bits - 1)
    return (value ^ sign) - sign


def direct_calls(data, address, isa):
    if isa == "arm":
        for index, (word,) in enumerate(U32.iter_unpack(data[:len(data) & ~3])):
            if word >> 28 != 15 and word & 0x0F000000 == 0x0B000000:
                yield address + index * 4 + 8 + signed((word & 0xFFFFFF) << 2, 26)
    else:
        for offset in range(0, len(data) - 3, 2):
            first, second = u16(data, offset), u16(data, offset + 2)
            if first & 0xF800 == 0xF000 and second & 0xD000 == 0xD000:
                sign = (first >> 10) & 1
                j1, j2 = (second >> 13) & 1, (second >> 11) & 1
                imm = ((sign << 24) | ((1 ^ j1 ^ sign) << 23) | ((1 ^ j2 ^ sign) << 22)
                       | ((first & 1023) << 12) | ((second & 2047) << 1))
                yield address + offset + 4 + signed(imm, 25)


def crc_shape(data, isa):
    if isa == "arm":
        return any(word & 0x0FE00FF0 == 0x00200420 for (word,) in
                   U32.iter_unpack(data[:len(data) & ~3]))  # EOR with LSR #8
    return any(u16(data, i) & 0xFFE0 == 0xEA80 and u16(data, i + 2) & 0x70F0 == 0x2010
               for i in range(0, len(data) - 3, 2))  # EOR.W with LSR #8


def anchor_hits(text, boundaries, anchors):
    present = {label for labels in anchors.values() for label in labels}
    libpng_present = {"png_header", "png_order"} <= present or {"png_ihdr", "png_ihdr_order"} <= present
    zlib_present = "z_header" in present and bool(present & {"z_method", "z_window", "z_distance", "z_length"})
    if not (libpng_present or zlib_present):
        return []
    end = text.address + len(text.data)
    valid_starts = sorted(start for start in boundaries if text.address <= start < end)
    modes = {boundaries[start] for start in valid_starts}
    hits = []
    for isa in ("arm", "thumb"):
        if modes and isa not in modes:
            continue
        refs = references(text, isa, anchors)
        if not refs:
            continue
        starts = valid_starts or prologues(text, isa)
        grouped = defaultdict(set)
        for address, evidence in refs.items():
            index = bisect_right(starts, address) - 1
            if index >= 0:
                start = starts[index]
                if boundaries.get(start, isa) == isa and address - start < 16384:
                    grouped[start].update(evidence)

        def code_at(start):
            index = bisect_right(starts, start)
            limit = starts[index] if index < len(starts) else end
            return text.data[start - text.address:min(limit, start + 16384) - text.address]

        def emit(library, name, address):
            hits.append({"library": library, "function": name, "address": address, "isa": isa,
                         "confidence": "medium", "version_guess": [], "evidence": "constant-references"})

        inflates, fast_paths, crcs = set(), set(), set()
        png_filters = set()
        png_read_path = False
        for start, found in grouped.items():
            if "z_header" in found and found.intersection({"z_method", "z_window", "z_distance", "z_length"}):
                inflates.add(start)
            elif {"z_distance", "z_length"} <= found:
                fast_paths.add(start)
            if "z_crc" in found and crc_shape(code_at(start), isa):
                crcs.add(start)
            if {"png_header", "png_order"} <= found:
                emit("libpng", "png_read_info", start)
                png_read_path = True
            if {"png_ihdr", "png_ihdr_order"} <= found:
                emit("libpng", "png_handle_IHDR", start)
                png_read_path = True
            if "png_filter" in found:
                png_filters.add(start)
        if png_read_path:
            for start in sorted(png_filters):
                emit("libpng", "png_read_filter_row", start)

        for start in sorted(inflates):
            emit("zlib", "inflate", start)

        # A CRC table can also be used by an unrelated library. Prefer the
        # implementation the confirmed inflate function calls. An isolated
        # CRC loop is sufficient only with other evidence of a zlib decoder.
        calls = set()
        for start in inflates:
            calls.update(direct_calls(code_at(start), start, isa))
        crc_candidates = crcs & calls
        if not crc_candidates and len(crcs) == 1 and inflates:
            crc_candidates = crcs
        for start in sorted(crc_candidates):
            emit("zlib", "crc32", start)

        # Older zlib routes inflate through inflate_blocks and inflate_codes
        # before calling inflate_fast. Follow up to three direct calls. The
        # terminal routine uses both error strings and calls no other candidate.
        if fast_paths:
            starts_in_mode = {start for start in starts if boundaries.get(start, isa) == isa}
            reachable, frontier = set(), calls & starts_in_mode
            for _ in range(3):
                if len(reachable) + len(frontier) > 256:
                    break  # abandon overly broad call graphs
                reachable.update(frontier)
                targets = {target for start in frontier for target in direct_calls(code_at(start), start, isa)}
                frontier = (targets & starts_in_mode) - reachable
            for start in sorted(fast_paths & reachable):
                targets = set(direct_calls(code_at(start), start, isa))
                if not (targets & (fast_paths - {start})):
                    emit("zlib", "inflate_fast", start)
    return hits


def legacy_matches(data):
    hits = []
    try:
        for image in macho_slices(data):
            metadata = macho_metadata(image)
            if not metadata:
                continue
            text, starts, symbols, anchors = metadata
            named = set()
            for name, (address, isa) in symbols.items():
                key = SYMBOL_LIB[name], name
                named.add(key)
                hits.append({"library": key[0], "function": name, "address": address, "isa": isa,
                             "confidence": "high", "version_guess": [], "evidence": "defined-symbol"})
            for hit in anchor_hits(text, starts, anchors):
                if (hit["library"], hit["function"]) not in named:
                    hits.append(hit)
    except struct.error as error:
        raise ValueError("truncated Mach-O metadata") from error
    return hits
