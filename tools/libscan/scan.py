"""Report statically linked libraries in little-endian ARM guest images."""

import argparse
from collections import defaultdict
from dataclasses import dataclass
import json
from pathlib import Path, PurePosixPath
import struct
import sys
import zipfile

from legacy import ARM_SUBTYPES, MACH_FAT, MACH_THIN, legacy_matches, macho_slices


@dataclass(frozen=True)
class Section:
    address: int
    data: bytes


def span(data, offset, size):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise ValueError("region extends beyond binary")
    return data[offset:offset + size]


def elf_headers(data):
    if data[:7] != b"\x7fELF\x01\x01\x01":
        return None
    header = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    return header if header[1] == 40 else None  # EM_ARM


def elf_sections(data, header):
    offset, size, count = header[5], header[10], header[11]
    if not offset:
        return []
    if size < 40:
        raise ValueError("invalid ELF section header size")
    if not count:
        count = struct.unpack_from("<I", data, offset + 20)[0]
    span(data, offset, size * count)
    return [struct.unpack_from("<10I", data, offset + i * size) for i in range(count)]


def elf_code(data):
    header = elf_headers(data)
    if not header or header[0] not in (2, 3):  # ET_EXEC or ET_DYN
        return
    sections = elf_sections(data, header)
    if sections:
        for _, kind, flags, address, offset, size, *_ in sections:
            if kind == 1 and flags & 4 and size:  # SHT_PROGBITS, SHF_EXECINSTR
                yield Section(address, span(data, offset, size))
        return
    offset, size, count = header[4], header[8], header[9]
    if count and size < 32:
        raise ValueError("invalid ELF program header size")
    for i in range(count):
        kind, start, address, _, length, _, flags, _ = struct.unpack_from("<8I", data, offset + i * size)
        if kind == 1 and flags & 1 and length:  # PT_LOAD, PF_X
            yield Section(address, span(data, start, length))


def macho_code(data, universal=True):
    if universal and data[:4] in MACH_FAT:
        for image in macho_slices(data):
            yield from macho_code(image, universal=False)
        return
    if data[:4] != MACH_THIN:
        return
    _, cpu, subtype, _, count, command_bytes, _ = struct.unpack_from("<7I", data)
    if cpu != 12 or (subtype & 0xFFFFFF) not in ARM_SUBTYPES:
        return
    commands_end = 28 + len(span(data, 28, command_bytes))
    offset = 28
    for _ in range(count):
        kind, length = struct.unpack_from("<II", data, offset)
        if length < 8 or offset + length > commands_end:
            raise ValueError("invalid Mach-O load command size")
        if kind == 1:  # LC_SEGMENT
            if length < 56:
                raise ValueError("invalid Mach-O segment size")
            initprot, nsects = struct.unpack_from("<II", data, offset + 44)
            if 56 + 68 * nsects > length:
                raise ValueError("invalid Mach-O section count")
            if initprot & 4:
                for i in range(nsects):
                    section = offset + 56 + i * 68
                    address, size, start, _, _, _, flags = struct.unpack_from("<7I", data, section + 32)
                    if size and flags & 0x80000400:  # PURE_INSTRUCTIONS or SOME_INSTRUCTIONS
                        yield Section(address, span(data, start, size))
        offset += length


def code_sections(data):
    try:
        if data.startswith(b"\x7fELF"):
            return list(elf_code(data))
        return list(macho_code(data))
    except struct.error as error:
        raise ValueError("truncated binary header") from error


def app_executables(path):
    if path.is_file():
        yield path.name, path.read_bytes()
        return
    for entry in sorted(path.rglob("*")):
        if entry.is_file() and not entry.is_symlink():
            with entry.open("rb") as stream:
                if stream.read(4) in (MACH_THIN, *MACH_FAT):
                    stream.seek(0)
                    yield entry.relative_to(path).as_posix(), stream.read()


def package_executables(path):
    if path.suffix.lower() not in (".apk", ".ipa"):
        yield from app_executables(path)
        return
    with zipfile.ZipFile(path) as archive:
        for entry in sorted(archive.infolist(), key=lambda item: item.filename):
            name = PurePosixPath(entry.filename)
            if entry.is_dir():
                continue
            if path.suffix.lower() == ".apk":
                if name.parts[:2] != ("lib", "armeabi-v7a") or name.suffix != ".so":
                    continue
            elif name.parts[:1] != ("Payload",) or len(name.parts) < 3 or not name.parts[1].endswith(".app"):
                continue
            with archive.open(entry) as stream:
                header = stream.read(4)
                if header in (b"\x7fELF", MACH_THIN, *MACH_FAT):
                    yield str(name), header + stream.read()


@dataclass(frozen=True)
class Signature:
    library: str
    function: str
    versions: tuple
    isa: str
    value: bytes
    mask: bytes
    target: str | None = None
    internal: bool = False

    def matches(self, section):
        # Find the longest run of fixed bytes first, then check all remaining bits.
        # Keeping the anchor as plain bytes lets bytes.find do the scan in native code.
        # A zero offset is valid; use a sentinel to finish the final run.
        longest, start, current = 0, 0, 0
        for i, mask in enumerate(self.mask + b"\x00"):
            if mask == 255:
                continue
            if i - current > longest:
                longest, start = i - current, current
            current = i + 1
        if longest < 2:
            return
        anchor = self.value[start:start + longest]
        offset = section.data.find(anchor, start)
        while offset != -1:
            base = offset - start
            address = section.address + base
            if (address % (4 if self.isa == "arm" else 2) == 0 and
                    base + len(self.value) <= len(section.data) and
                    all((actual & mask) == value for actual, mask, value in
                        zip(memoryview(section.data)[base:base + len(self.value)], self.mask, self.value))):
                yield address
            offset = section.data.find(anchor, offset + 1)


def tail_target(section, address, isa):
    """Return the destination of the last instruction in Tremor's synthesis wrapper."""
    offset = address - section.address
    if isa == "arm":
        branch, = struct.unpack_from("<I", section.data, offset + 4)
        displacement, width, pc = (branch & 0xFFFFFF) << 2, 26, address + 12
    else:
        first, second = struct.unpack_from("<HH", section.data, offset + 2)
        sign = (first >> 10) & 1
        j1, j2 = (second >> 13) & 1, (second >> 11) & 1
        displacement = ((sign << 24) | ((1 ^ j1 ^ sign) << 23) | ((1 ^ j2 ^ sign) << 22) |
                        ((first & 0x3FF) << 12) | ((second & 0x7FF) << 1))
        width, pc = 25, address + 6
    sign_bit = 1 << (width - 1)
    return pc + (displacement ^ sign_bit) - sign_bit


def load_signatures(path):
    with path.open() as stream:
        db = json.load(stream)
    if db["schema"] != 1:
        raise ValueError("unsupported signature database schema")
    return [Signature(row["library"], row["function"], tuple(row["versions"]), row["isa"],
                      bytes.fromhex(row["value"]), bytes.fromhex(row["mask"]), row.get("target"),
                      row.get("internal", False)) for row in db["signatures"]]


def scan_binary(data, signatures):
    matches = defaultdict(set)
    helpers = defaultdict(set)
    wrappers = []
    for section in code_sections(data):
        for signature in signatures:
            for address in signature.matches(section):
                key = (signature.library, signature.function, address, signature.isa)
                if signature.target:
                    wrappers.append((key, signature, tail_target(section, address, signature.isa)))
                elif signature.internal:
                    helpers[key].update(signature.versions)
                else:
                    matches[key].update(signature.versions)
    for key, signature, destination in wrappers:
        versions = helpers.get((signature.library, signature.target, destination, signature.isa), ())
        confirmed = set(signature.versions).intersection(versions)
        if confirmed:
            matches[key].update(confirmed)
    return [
        {"library": library, "function": function, "address": address, "isa": isa,
         "confidence": "high", "version_guess": sorted(versions)}
        for (library, function, address, isa), versions in sorted(matches.items())
    ]


def identify_libraries(data, signatures):
    matches = scan_binary(data, signatures)
    # Keep exact byte evidence and its version candidates when both paths
    # identify the same entry point.
    def key(hit):
        return hit["library"], hit["function"], hit["address"], hit["isa"]

    by_address = {key(hit): hit for hit in matches}
    for hit in legacy_matches(data):
        by_address.setdefault(key(hit), hit)
    return [by_address[k] for k in sorted(by_address)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="+", type=Path, metavar="IPA|APK|APP|BINARY")
    parser.add_argument("--signatures", type=Path, default=Path(__file__).with_name("signatures.json"))
    args = parser.parse_args()
    try:
        signatures = load_signatures(args.signatures)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.error(str(error))
    results = []
    failed = False
    for path in args.inputs:
        result = {"input": str(path), "binaries": [], "errors": []}
        try:
            for name, data in package_executables(path):
                try:
                    matches = identify_libraries(data, signatures)
                    result["binaries"].append({"path": name, "matches": matches})
                except ValueError as error:
                    result["errors"].append({"path": name, "error": str(error)})
        except (OSError, RuntimeError, zipfile.BadZipFile) as error:
            result["errors"].append({"path": str(path), "error": str(error)})
        result["match_count"] = sum(len(image["matches"]) for image in result["binaries"])
        failed |= bool(result["errors"])
        results.append(result)
    json.dump({"schema": 1, "inputs": results}, sys.stdout, indent=2)
    print()
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
