"""Exchange Spell Lab settings between offline instances of the pinned guest."""
import argparse
import json
import os
from pathlib import Path
import struct
import tempfile

from clash_custom_cards import fnv
from clash_spell_catalog import catalog

HEADER = struct.Struct("<7I")
RECORD = struct.Struct("<6I")
MAGIC = 0x31504C53
FORMAT = "hydrogen.clash.spells/1"
FIELDS = ("damage", "radius", "duration", "speed", "cost")
DEFAULTS = (100, 100, 100, 100, 0)


def checked_records(items, definitions):
    if not isinstance(items, list) or not 1 <= len(items) <= len(definitions):
        raise ValueError("Invalid spell count")
    records = []
    for item, definition in zip(items, definitions):
        if not isinstance(item, dict) or set(item) != {"id", *FIELDS}:
            raise ValueError("Invalid spell fields")
        if type(item["id"]) is not int or item["id"] != definition["id"]:
            raise ValueError("Spell IDs must match the catalog in order")
        values = []
        for index, field in enumerate(FIELDS):
            value = item[field]
            low, high = [(10, 800), (25, 400), (25, 400), (25, 400), (0, 10)][index]
            if type(value) is not int or not low <= value <= high:
                raise ValueError(f"Invalid {field} for {definition['name']}")
            if not definition["caps"] & (1 << index) and value != DEFAULTS[index]:
                raise ValueError(f"Unsupported {field} for {definition['name']}")
            values.append(value)
        records.append(RECORD.pack(item["id"], *values))
    return b"".join(records)


def encode(preset, definitions):
    if not isinstance(preset, dict) or set(preset) != {"format", "name", "enabled", "selected", "spells"}:
        raise ValueError("Invalid preset structure")
    if preset["format"] != FORMAT or not isinstance(preset["name"], str) or not 1 <= len(preset["name"].strip()) <= 80:
        raise ValueError("Unknown format or invalid preset name")
    body = checked_records(preset["spells"], definitions)
    count = len(preset["spells"])
    selected = preset["selected"]
    if type(selected) is not int or not 0 <= selected < count or type(preset["enabled"]) is not bool:
        raise ValueError("Invalid selection or enabled flag")
    prefix = struct.pack("<6I", MAGIC, 1, count, preset["enabled"], selected, fnv(body))
    return prefix + struct.pack("<I", fnv(prefix)) + body


def decode(data, definitions, name):
    if len(data) < HEADER.size:
        raise ValueError("Truncated spell profile")
    magic, version, count, enabled, selected, body_hash, header_hash = HEADER.unpack_from(data)
    body = data[HEADER.size:]
    if (magic, version) != (MAGIC, 1) or enabled > 1 or len(body) != count * RECORD.size:
        raise ValueError("Invalid spell profile header or size")
    if fnv(data[:24]) != header_hash or fnv(body) != body_hash:
        raise ValueError("Damaged spell profile")
    spells = [dict(zip(("id", *FIELDS), row)) for row in RECORD.iter_unpack(body)]
    preset = dict(format=FORMAT, name=name, enabled=bool(enabled), selected=selected, spells=spells)
    encode(preset, definitions)
    return preset


def write_new(path, data):
    # Hard-link the complete file to publish without replacing a file created by
    # another export. Temporary and final names are on the same filesystem.
    fd, temp = tempfile.mkstemp(dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.link(temp, path)
    finally:
        os.unlink(temp)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("export", "import"))
    parser.add_argument("app", type=Path, help="Extracted pinned Clash Royale.app")
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path, help="New file to create")
    parser.add_argument("--name", help="Display name when exporting a preset")
    args = parser.parse_args()
    try:
        definitions = catalog(args.app)
        with args.source.open("rb") as stream:
            data = stream.read(65537)
        if len(data) > 65536:
            raise ValueError("Preset exceeds 64 KiB")
        if args.action == "export":
            preset = decode(data, definitions, args.name or args.output.stem)
            result = (json.dumps(preset, indent=2, ensure_ascii=False) + "\n").encode()
        else:
            result = encode(json.loads(data), definitions)
        write_new(args.output, result)
    except (ValueError, OSError) as exc:
        parser.exit(1, f"{exc}\n")


if __name__ == "__main__":
    main()
