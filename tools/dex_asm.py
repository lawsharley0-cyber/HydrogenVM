#!/usr/bin/env python3
"""Assemble small test DEX 035 images without a JDK.

Numeric access flags and a deliberately small smali-like syntax are accepted.
.words takes raw 16-bit units for tests of instructions without mnemonics.
See tests/android/java/*.smali for complete examples.
"""
import argparse
from dataclasses import dataclass, field
import hashlib
import json
from pathlib import Path
import re
import struct
import zlib


class AssemblyError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise AssemblyError(message)


def number(text, lo, hi):
    value = int(text, 0)
    require(lo <= value <= hi, f"value out of range: {text}")
    return value


def uleb(value):
    require(0 <= value <= 0xffffffff, "ULEB128 out of range")
    out = bytearray()
    while value > 127:
        out.append((value & 127) | 128)
        value >>= 7
    return bytes(out + bytes([value]))


def sleb(value):
    out = bytearray()
    while True:
        byte = value & 127
        value >>= 7
        if (value == 0 and byte < 64) or (value == -1 and byte >= 64):
            return bytes(out + bytes([byte]))
        out.append(byte | 128)


def utf16(text):
    raw = text.encode("utf-16-le", errors="surrogatepass")
    return struct.unpack(f"<{len(raw)//2}H", raw)


def mutf8(text):
    units = utf16(text)
    out = bytearray(uleb(len(units)))
    for char in units:
        if 0 < char < 128:
            out.append(char)
        elif char < 2048:
            out.extend((0xc0 | (char >> 6), 0x80 | (char & 63)))
        else:
            out.extend((0xe0 | (char >> 12), 0x80 | ((char >> 6) & 63), 0x80 | (char & 63)))
    return bytes(out + b"\0")


TYPE = r"(?:\[*[ZBCSIJFD]|\[*L[^;\s]+;)"
SIG = re.compile(rf"([^()\s]+)\(((?:{TYPE})*)\)({TYPE}|V)")


def prototype(declaration):
    match = SIG.fullmatch(declaration)
    require(match, f"invalid method signature: {declaration}")
    name, params, ret = match.groups()
    return name, (ret, tuple(re.findall(TYPE, params)))


def shorty(proto):
    ret, params = proto
    return "".join("L" if t.startswith(("L", "[")) else t for t in (ret, *params))


@dataclass
class Member:
    name: str
    type: str
    flags: int
    initial: str | None


@dataclass
class Method:
    name: str
    proto: tuple
    flags: int
    registers: int | None = None
    outgoing: int = 0
    lines: list = field(default_factory=list)
    catches: list = field(default_factory=list)

    @property
    def incoming(self):
        return (not self.flags & 8) + sum(2 if t in ("J", "D") else 1 for t in self.proto[1])

    @property
    def direct(self):
        return bool(self.flags & (8 | 2 | 0x10000) or self.name in ("<init>", "<clinit>"))


@dataclass
class Class:
    type: str
    flags: int
    parent: str | None = "Ljava/lang/Object;"
    source: str | None = None
    interfaces: list = field(default_factory=list)
    fields: list = field(default_factory=list)
    methods: list = field(default_factory=list)


def parse(text):
    classes, strings = {}, {}
    cls = method = None
    for line_number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        op, _, rest = line.partition(" ")
        args = rest.replace(",", " ").split()
        try:
            if op == ".string":
                alias, literal = rest.split(maxsplit=1)
                require(alias not in strings, f"duplicate string alias: {alias}")
                strings[alias] = json.loads(literal)
                require(isinstance(strings[alias], str), "expected a JSON string")
            elif op == ".class":
                descriptor, flags = args
                require(method is None and descriptor.startswith("L") and re.fullmatch(TYPE, descriptor)
                        and descriptor not in classes, "invalid or duplicate class")
                cls = Class(descriptor, number(flags, 0, 0xffffffff))
                classes[descriptor] = cls
            elif method is not None:
                if op == ".end":
                    require(args == ["method"], "expected .end method")
                    method = None
                elif op == ".registers":
                    require(method.registers is None and len(args) == 1, "expected one register count")
                    method.registers = number(args[0], 0, 65535)
                elif op == ".outs":
                    require(len(args) == 1, "expected one outgoing register count")
                    method.outgoing = number(args[0], 0, 65535)
                elif op == ".catch":
                    exception, start, end, target = args
                    require(exception == "*" or exception.startswith("L") and re.fullmatch(TYPE, exception),
                            "expected exception descriptor or *")
                    method.catches.append((exception, start, end, target))
                else:
                    method.lines.append((op, args))
            else:
                require(cls is not None, "expected .class")
                if op in (".super", ".implements"):
                    descriptor, = args
                    require(descriptor.startswith("L") and re.fullmatch(TYPE, descriptor)
                            or op == ".super" and descriptor == "-", "expected a class descriptor")
                    if op == ".super":
                        cls.parent = descriptor if descriptor != "-" else None
                    else:
                        cls.interfaces.append(descriptor)
                elif op == ".source":
                    cls.source = json.loads(rest)
                    require(isinstance(cls.source, str), "expected a JSON string")
                elif op == ".field":
                    declaration, flags, *tail = args
                    name, descriptor = declaration.split(":")
                    require(name and re.fullmatch(TYPE, descriptor), "invalid field signature")
                    require(not tail or len(tail) == 2 and tail[0] == "=", "expected = value")
                    flags = number(flags, 0, 0xffffffff)
                    require(not tail or flags & 8, "only static fields accept encoded values")
                    cls.fields.append(Member(name, descriptor, flags, tail[-1] if tail else None))
                elif op == ".method":
                    declaration, flags = args
                    name, proto = prototype(declaration)
                    method = Method(name, proto, number(flags, 0, 0xffffffff))
                    cls.methods.append(method)
                else:
                    raise AssemblyError(f"unknown directive: {op}")
        except ValueError as error:
            raise AssemblyError(f"line {line_number}: {error}") from error
    require(method is None, "missing .end method")
    require(classes, "no classes")
    return classes, strings


ONE = {"return": 0x0f, "return-wide": 0x10, "return-object": 0x11,
       "move-result": 0x0a, "move-result-wide": 0x0b, "move-result-object": 0x0c,
       "move-exception": 0x0d, "monitor-enter": 0x1d, "monitor-exit": 0x1e, "throw": 0x27}
TWO = {"move": 0x01, "move-wide": 0x04, "move-object": 0x07, "array-length": 0x21,
       "add-int/2addr": 0xb0, "sub-int/2addr": 0xb1, "mul-int/2addr": 0xb2, "div-int/2addr": 0xb3}
THREE = {"add-int": 0x90, "sub-int": 0x91, "mul-int": 0x92, "div-int": 0x93, "rem-int": 0x94,
         "add-long": 0x9b, "sub-long": 0x9c, "add-float": 0xa6, "add-double": 0xab,
         "aget": 0x44, "aput": 0x4b, "aget-object": 0x46, "aput-object": 0x4d,
         "aget-byte": 0x48, "aput-byte": 0x4f}
CONSTS = {"const/16": (0x13, 1), "const": (0x14, 2), "const-wide/16": (0x16, 1),
          "const-wide/32": (0x17, 2), "const-wide": (0x18, 4)}
COMPARE_BRANCHES = {"if-eq": 0x32, "if-ne": 0x33, "if-lt": 0x34,
                    "if-ge": 0x35, "if-gt": 0x36, "if-le": 0x37}
BRANCHES = {"if-eqz": 0x38, "if-nez": 0x39, "if-ltz": 0x3a, "if-gez": 0x3b,
            "if-gtz": 0x3c, "if-lez": 0x3d}


TYPE_OPS = {"const-class": 0x1c, "check-cast": 0x1f, "instance-of": 0x20,
            "new-instance": 0x22, "new-array": 0x23}
FIELD_OPS = {"iget": 0x52, "iget-wide": 0x53, "iget-object": 0x54,
             "iput": 0x59, "iput-wide": 0x5a, "iput-object": 0x5b,
             "sget": 0x60, "sget-wide": 0x61, "sget-object": 0x62,
             "sput": 0x67, "sput-wide": 0x68, "sput-object": 0x69}
INVOCATIONS = {"invoke-virtual": 0x6e, "invoke-super": 0x6f, "invoke-direct": 0x70,
               "invoke-static": 0x71, "invoke-interface": 0x72}


def member_reference(text, method):
    owner, member = text.split("->", 1)
    require(owner.startswith("L") and re.fullmatch(TYPE, owner), "invalid member owner")
    if method:
        name, proto = prototype(member)
        return owner, name, proto
    name, typ = member.split(":", 1)
    require(name and re.fullmatch(TYPE, typ), "invalid field reference")
    return owner, name, typ


def width(op, args):
    if op == ".words":
        require(args, ".words requires at least one code unit")
        return len(args)
    if op in CONSTS:
        return 1 + CONSTS[op][1]
    if op == "fill-array-data":
        return 3
    if op in TYPE_OPS or op in FIELD_OPS:
        return 2
    if op in INVOCATIONS or op == "filled-new-array":
        return 3
    if op in THREE or op in BRANCHES or op in COMPARE_BRANCHES or op in ("goto/16", "const-string"):
        return 2
    require(op in ONE or op in TWO or op in ("return-void", "nop", "const/4", "goto"), f"unknown opcode: {op}")
    return 1


def instruction(op, args, pc, labels, strings, types, fields, methods, registers):
    def reg(text, max_value):
        require(re.fullmatch(r"v[0-9]+", text), f"expected v-register: {text}")
        value = int(text[1:])
        require(value < registers and value <= max_value, f"register cannot be encoded: {text}")
        return value

    def offset(label, bits):
        require(label in labels, f"unknown label: {label}")
        delta = labels[label] - pc
        require(delta != 0 and -(1 << (bits - 1)) <= delta < 1 << (bits - 1), "branch displacement out of range")
        return delta & ((1 << bits) - 1)

    if op == ".words":
        return [number(x, 0, 65535) for x in args]
    if op in ONE:
        dest, = args
        return [ONE[op] | reg(dest, 255) << 8]
    if op in TWO:
        dest, source = args
        return [TWO[op] | reg(dest, 15) << 8 | reg(source, 15) << 12]
    if op in THREE:
        dest, first, second = args
        return [THREE[op] | reg(dest, 255) << 8, reg(first, 255) | reg(second, 255) << 8]
    if op in CONSTS:
        dest, literal = args
        opcode, words = CONSTS[op]
        bits = 16 * words
        value = number(literal, -(1 << (bits - 1)), (1 << bits) - 1)
        return [opcode | reg(dest, 255) << 8, *[(value >> (16*i)) & 65535 for i in range(words)]]
    if op == "const/4":
        dest, literal = args
        return [0x12 | reg(dest, 15) << 8 | (number(literal, -8, 7) & 15) << 12]
    if op == "const-string":
        dest, alias = args
        require(alias in strings and strings[alias] < 65536, "unknown or oversized string alias")
        return [0x1a | reg(dest, 255) << 8, strings[alias]]
    if op in TYPE_OPS:
        if op in ("check-cast", "const-class", "new-instance"):
            dest, typ = args
            regs = reg(dest, 255)
        else:
            dest, source, typ = args
            regs = reg(dest, 15) | reg(source, 15) << 4
        require(typ in types, "invalid type operand")
        return [TYPE_OPS[op] | regs << 8, types[typ]]
    if op in FIELD_OPS:
        if op.startswith("s"):
            value, name = args
            regs = reg(value, 255)
        else:
            value, receiver, name = args
            regs = reg(value, 15) | reg(receiver, 15) << 4
        return [FIELD_OPS[op] | regs << 8, fields[member_reference(name, False)]]
    if op in INVOCATIONS or op == "filled-new-array":
        name, *regs_text = args
        require(len(regs_text) <= 5, "invoke accepts at most five register words")
        registers_4 = [reg(v, 15) for v in regs_text] + [0] * (5 - len(regs_text))
        word = sum(v << (4 * i) for i, v in enumerate(registers_4[:4]))
        opcode = INVOCATIONS.get(op, 0x24)
        index = methods[member_reference(name, True)] if op in INVOCATIONS else types[name]
        return [opcode | registers_4[4] << 8 | len(regs_text) << 12, index, word]
    if op == "fill-array-data":
        dest, label = args
        delta = offset(label, 32)
        return [0x26 | reg(dest, 255) << 8, delta & 65535, delta >> 16]
    if op in COMPARE_BRANCHES:
        first, second, label = args
        code = COMPARE_BRANCHES[op] | reg(first, 15) << 8 | reg(second, 15) << 12
        return [code, offset(label, 16)]
    if op in BRANCHES:
        source, label = args
        return [BRANCHES[op] | reg(source, 255) << 8, offset(label, 16)]
    if op == "goto":
        target, = args
        return [0x28 | offset(target, 8) << 8]
    if op == "goto/16":
        target, = args
        return [0x29, offset(target, 16)]
    require(not args, f"unexpected operands: {op}")
    return [0x0e if op == "return-void" else 0]


def code_item(method, strings, types, fields, methods):
    require(method.registers is not None and method.incoming <= method.registers,
            f"missing or invalid register count for {method.name}")
    labels, instructions = {}, []
    pc = 0
    for op, args in method.lines:
        if op.startswith(":"):
            require(not args and op not in labels, f"invalid or duplicate label: {op}")
            labels[op] = pc
        else:
            instructions.append((pc, op, args))
            pc += width(op, args)
    require(pc > 0, "empty code item")
    words = []
    for pos, op, args in instructions:
        words.extend(instruction(op, args, pos, labels, strings, types, fields, methods, method.registers))
    groups = {}
    for exception, start, end, target in method.catches:
        positions = [labels.get(label, -1) for label in (start, end, target)]
        first, last, handler = positions
        require(0 <= first < last <= pc and 0 <= handler < pc and last - first <= 65535,
                "invalid catch labels or range")
        groups.setdefault((first, last), []).append((exception, handler))
    out = bytearray(struct.pack("<4H2I", method.registers, method.incoming, method.outgoing, len(groups), 0, pc))
    out.extend(struct.pack(f"<{pc}H", *words))
    if not groups:
        return out
    if pc & 1:
        out.extend(b"\0\0")
    handlers = bytearray(uleb(len(groups)))
    previous = 0
    for (first, last), catches in sorted(groups.items()):
        require(first >= previous, "overlapping try ranges")
        previous = last
        types = [t for t, _ in catches]
        require(len(types) == len(set(types)), "duplicate catch type")
        require("*" not in types or types[-1] == "*", "catch-all must be last")
        require(len(handlers) <= 65535, "catch table too large")
        out.extend(struct.pack("<IHH", first, last-first, len(handlers)))
        catch_all = types[-1] == "*"
        handlers.extend(sleb(1-len(types) if catch_all else len(types)))
        for typ, handler in catches:
            if typ != "*":
                handlers.extend(uleb(typ))
            handlers.extend(uleb(handler))
    return out + handlers


def encoded_value(member, strings):
    typ, literal = member.type, member.initial
    if typ == "Z":
        require(literal in (None, "true", "false"), "invalid boolean static value")
        return bytes([0x3f if literal == "true" else 0x1f])
    if typ.startswith(("L", "[")):
        if literal in (None, "null"):
            return b"\x1e"
        require(typ == "Ljava/lang/String;" and literal in strings, "expected null or a string alias")
        value = strings[literal]
        width = max(1, (value.bit_length() + 7)//8)
        return bytes([0x17 | (width-1) << 5]) + value.to_bytes(width, "little")
    code, width, signed = {"B": (0, 1, True), "S": (2, 2, True), "C": (3, 2, False),
                           "I": (4, 4, True), "J": (6, 8, True), "F": (16, 4, False),
                           "D": (17, 8, False)}[typ]
    bits = width * 8
    value = number(literal, -(1 << (bits-1)) if signed else 0,
                   (1 << (bits-1 if signed else bits))-1) if literal is not None else 0
    return bytes([code | (width-1) << 5]) + value.to_bytes(width, "little", signed=signed)


def ordered_classes(classes):
    done, visiting = set(), set()
    order = []

    def visit(cls):
        if cls.type in done:
            return
        require(cls.type not in visiting, "class inheritance cycle")
        visiting.add(cls.type)
        for parent in (cls.parent, *cls.interfaces):
            if parent in classes:
                visit(classes[parent])
        visiting.remove(cls.type)
        done.add(cls.type)
        order.append(cls)
    for cls in sorted(classes.values(), key=lambda cls: utf16(cls.type)):
        visit(cls)
    return order


def assemble(text):
    classes, aliases = parse(text)
    classes = ordered_classes(classes)
    types, protos, fields, methods, strings = set(), set(), [], [], set(aliases.values())
    field_refs, method_refs = set(), set()
    for cls in classes:
        types.update(t for t in (cls.type, cls.parent, *cls.interfaces) if t)
        if cls.source is not None:
            strings.add(cls.source)
        for m in cls.fields:
            fields.append((cls.type, m.name, m.type))
            types.add(m.type)
            strings.add(m.name)
        for m in cls.methods:
            methods.append((cls.type, m.name, m.proto))
            protos.add(m.proto)
            types.update((m.proto[0], *m.proto[1]))
            types.update(t for t, *_ in m.catches if t != "*")
            for op, args in m.lines:
                if op in TYPE_OPS:
                    require(args and re.fullmatch(TYPE, args[-1]), "invalid type operand")
                    types.add(args[-1])
                elif op in FIELD_OPS:
                    owner, name, typ = member_reference(args[-1], False)
                    field_refs.add((owner, name, typ))
                    types.update((owner, typ))
                    strings.add(name)
                elif op in INVOCATIONS:
                    owner, name, proto = member_reference(args[0], True)
                    method_refs.add((owner, name, proto))
                    protos.add(proto)
                    types.update((owner, proto[0], *proto[1]))
                    strings.update((name, shorty(proto)))
                elif op == "filled-new-array":
                    require(args and args[0].startswith("[") and re.fullmatch(TYPE, args[0]), "invalid array type")
                    types.add(args[0])
            strings.update((m.name, shorty(m.proto)))
    require(len(set(fields)) == len(fields) and len(set(methods)) == len(methods), "duplicate member")
    fields = list(set(fields) | field_refs)
    methods = list(set(methods) | method_refs)
    require(max(len(types), len(protos), len(fields), len(methods)) <= 65535, "DEX ID table too large")
    strings = sorted(strings | types, key=utf16)
    string_ids = {text: i for i, text in enumerate(strings)}
    alias_ids = {alias: string_ids[text] for alias, text in aliases.items()}
    types = sorted(types, key=string_ids.get)
    type_ids = {t: i for i, t in enumerate(types)}

    def type_key(items):
        return tuple(type_ids[t] for t in items)
    protos = sorted(protos, key=lambda p: (type_ids[p[0]], type_key(p[1])))
    proto_ids = {p: i for i, p in enumerate(protos)}
    fields.sort(key=lambda f: (type_ids[f[0]], string_ids[f[1]], type_ids[f[2]]))
    methods.sort(key=lambda m: (type_ids[m[0]], string_ids[m[1]], proto_ids[m[2]]))
    field_ids = {f: i for i, f in enumerate(fields)}
    method_ids = {m: i for i, m in enumerate(methods)}

    tables = [(len(strings), 4), (len(types), 4), (len(protos), 12),
              (len(fields), 8), (len(methods), 8), (len(classes), 32)]
    sections, offsets = [(0, 0, 1, 0)], []
    image = bytearray(112)
    for i, (count, wid) in enumerate(tables, 1):
        offset = len(image) if count else 0
        offsets.append(offset)
        if count:
            sections.append((i, 0, count, offset))
            image.extend(bytes(count * wid))
    data_offset = len(image)

    def align():
        image.extend(bytes((-len(image)) % 4))

    def section(kind, count):
        if count:
            sections.append((kind, 0, count, len(image)))

    def pack(fmt, *values):
        image.extend(struct.pack("<" + fmt, *values))

    def table(index, row, fmt, *values):
        fmt = "<" + fmt
        struct.pack_into(fmt, image, offsets[index] + struct.calcsize(fmt)*row, *values)

    section(0x2002, len(strings))
    for i, s in enumerate(strings):
        table(0, i, "I", len(image))
        image.extend(mutf8(s))
    for i, t in enumerate(types):
        table(1, i, "I", string_ids[t])
    lists = {type_key(p[1]) for p in protos if p[1]} | {type_key(c.interfaces) for c in classes if c.interfaces}
    align()
    section(0x1001, len(lists))
    list_offsets = {(): 0}
    for entries in sorted(lists):
        align()
        list_offsets[entries] = len(image)
        pack("I", len(entries))
        pack(f"{len(entries)}H", *entries)
    for i, (ret, params) in enumerate(protos):
        table(2, i, "3I", string_ids[shorty((ret, params))], type_ids[ret], list_offsets[type_key(params)])
    for i, (cls, name, typ) in enumerate(fields):
        table(3, i, "HHI", type_ids[cls], type_ids[typ], string_ids[name])
    for i, (cls, name, proto) in enumerate(methods):
        table(4, i, "HHI", type_ids[cls], proto_ids[proto], string_ids[name])

    align()
    code_methods = [(c, m) for c in classes for m in c.methods if not m.flags & 0x500]
    section(0x2001, len(code_methods))
    code_offsets = {}
    for cls, method in code_methods:
        align()
        code_offsets[method_ids[(cls.type, method.name, method.proto)]] = len(image)
        resolved = Method(method.name, method.proto, method.flags, method.registers, method.outgoing,
                          method.lines, [(type_ids[t] if t != "*" else t, a, b, h) for t,a,b,h in method.catches])
        image.extend(code_item(resolved, alias_ids, type_ids, field_ids, method_ids))
    require(all(not (m.lines or m.catches or m.registers is not None)
                for c in classes for m in c.methods if m.flags & 0x500), "code on native or abstract method")

    with_data = [c for c in classes if c.fields or c.methods]
    section(0x2000, len(with_data))
    data_offsets, static_values = {}, {}
    for cls in with_data:
        data_offsets[cls.type] = len(image)
        field_lists = [[], []]
        for m in cls.fields:
            field_lists[0 if m.flags & 8 else 1].append((field_ids[(cls.type, m.name, m.type)], m))
        method_lists = [[], []]
        for m in cls.methods:
            method_lists[0 if m.direct else 1].append((method_ids[(cls.type, m.name, m.proto)], m))
        for members in (*field_lists, *method_lists):
            image.extend(uleb(len(members)))
        for members in (*field_lists, *method_lists):
            prior = 0
            for idx, member in sorted(members):
                image.extend(uleb(idx - prior) + uleb(member.flags))
                prior = idx
                if isinstance(member, Method):
                    image.extend(uleb(code_offsets.get(idx, 0)))
        initializers = [member for _, member in sorted(field_lists[0])]
        while initializers and initializers[-1].initial is None:
            initializers.pop()
        if initializers:
            static_values[cls.type] = initializers
    section(0x2005, len(static_values))
    value_offsets = {}
    for cls, members in static_values.items():
        value_offsets[cls] = len(image)
        image.extend(uleb(len(members)))
        for member in members:
            image.extend(encoded_value(member, alias_ids))
    for i, cls in enumerate(classes):
        table(5, i, "8I", type_ids[cls.type], cls.flags, type_ids.get(cls.parent, 0xffffffff),
              list_offsets[type_key(cls.interfaces)], string_ids.get(cls.source, 0xffffffff), 0,
              data_offsets.get(cls.type, 0), value_offsets.get(cls.type, 0))

    align()
    map_offset = len(image)
    section(0x1000, 1)
    pack("I", len(sections))
    for item in sections:
        pack("HHII", *item)
    image[:8] = b"dex\n035\0"
    header = [len(image), 112, 0x12345678, 0, 0, map_offset]
    for (size, _), offset in zip(tables, offsets):
        header.extend([size, offset])
    header.extend([len(image) - data_offset, data_offset])
    struct.pack_into("<20I", image, 32, *header)
    image[12:32] = hashlib.sha1(image[32:]).digest()
    struct.pack_into("<I", image, 8, zlib.adler32(image[12:]))
    return bytes(image)


def main(argv=None):
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("source", type=Path)
    cli.add_argument("-o", "--output", type=Path, required=True)
    args = cli.parse_args(argv)
    try:
        data = assemble(args.source.read_text(encoding="utf-8"))
        args.output.write_bytes(data)
    except (OSError, ValueError, struct.error) as error:
        cli.exit(1, f"dex_asm: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
