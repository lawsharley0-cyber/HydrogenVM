#include "runtime/android/dex/DexFile.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <fstream>
#include <map>
#include <utility>

namespace Hydrogen::Runtime::Android::Dex {
namespace {

void check(bool valid, const char* message) {
    if (!valid) throw FormatError(message);
}

uint32_t deltaIndex(uint32_t previous, uint32_t delta, uint32_t count, uint32_t position) {
    check(previous < count && delta < count - previous && (!position || delta), "invalid member index delta");
    return previous + delta;
}

std::optional<uint32_t> dexOrdinal(std::string_view name) {
    if (name == "classes.dex") return 1;
    if (!name.starts_with("classes") || !name.ends_with(".dex")) return {};
    name = name.substr(7, name.size() - 11);
    if (name.empty() || name.front() == '0') return {};
    uint32_t ordinal = 0;
    const auto result = std::from_chars(name.data(), name.data() + name.size(), ordinal);
    if (result.ec != std::errc{} || result.ptr != name.data() + name.size() || ordinal < 2) return {};
    return ordinal;
}

} // namespace

class File::Cursor {
public:
    Cursor(const File& file, size_t offset) : position(offset), file_(file) { file_.dataRange(offset, 1); }
    uint8_t byte() {
        file_.dataRange(position, 1);
        return file_.bytes_[position++];
    }
    size_t remaining() const { return file_.bytes_.size() - position; }
    uint32_t uleb() {
        uint32_t result = 0;
        for (unsigned shift = 0; shift <= 28; shift += 7) {
            const auto b = byte();
            check(shift < 28 || b <= 15, "ULEB128 overflow");
            result |= uint32_t(b & 127) << shift;
            if (!(b & 128)) return result;
        }
        throw FormatError("unterminated ULEB128");
    }
    int32_t sleb() {
        uint32_t result = 0;
        for (unsigned shift = 0; shift <= 28; shift += 7) {
            const auto b = byte();
            check(shift < 28 || b <= 7 || (b >= 0x78 && b < 0x80), "SLEB128 overflow");
            result |= uint32_t(b & 127) << shift;
            if (!(b & 128)) {
                if ((b & 64) && shift < 28) result |= ~uint32_t(0) << (shift + 7);
                return std::bit_cast<int32_t>(result);
            }
        }
        throw FormatError("unterminated SLEB128");
    }
    size_t position;
private:
    const File& file_;
};

File::File(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {
    range(0, 112);
    const std::string_view magic(reinterpret_cast<const char*>(bytes_.data()), 8);
    check(magic.substr(0, 4) == "dex\n" && magic.back() == 0, "invalid DEX magic");
    const auto version = magic.substr(4, 3);
    check(version == "035" || version == "037" || version == "038" || version == "039" || version == "040",
          "unsupported DEX version");
    check(u32(0x20) == bytes_.size() && u32(0x24) == 112, "invalid DEX header size");
    check(u32(0x28) == 0x12345678, "DEX requires little-endian encoding");
    dataOffset_ = u32(0x6c);
    check(dataOffset_ >= 112 && dataOffset_ % 4 == 0 &&
          uint64_t(dataOffset_) + u32(0x68) == bytes_.size(), "invalid DEX data extent");

    const std::array<size_t, 6> widths{4, 4, 12, 8, 8, 32};
    size_t lastEnd = 112;
    for (size_t i = 0; i < widths.size(); ++i) {
        auto& t = tables_[i];
        t = {u32(0x38 + 8 * i), u32(0x3c + 8 * i)};
        if (!t.count) {
            check(t.offset == 0, "offset for empty ID table");
            continue;
        }
        check(t.offset >= lastEnd && t.offset % 4 == 0 && t.offset <= dataOffset_ &&
              t.count <= (dataOffset_ - t.offset) / widths[i], "invalid DEX ID table");
        lastEnd = t.offset + t.count * widths[i];
    }
    check(tables_[1].count <= 65535 && tables_[2].count <= 65535, "DEX type or proto table too large");

    const auto mapOffset = u32(0x34);
    dataRange(mapOffset, 4, 4);
    const auto items = u32(mapOffset);
    dataRange(size_t(mapOffset) + 4, uint64_t(items) * 12);
    for (uint32_t i = 0; i < items; ++i) {
        const size_t pos = size_t(mapOffset) + 4 + size_t(i) * 12;
        if (u16(pos) != 8) continue; // method_handle_item
        methodHandles_ = u32(pos + 4);
        const size_t offset = u32(pos + 8);
        check(offset % 4 == 0 && offset >= 112 && methodHandles_ <= bytes_.size() / 8, "invalid method handles");
        range(offset, size_t(methodHandles_) * 8);
    }
}

File File::read(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) throw FormatError("cannot open DEX file");
    const auto size = in.tellg();
    check(size >= 112 && uint64_t(size) <= UINT32_MAX, "unsupported DEX file size");
    in.seekg(0);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) throw FormatError("cannot read DEX file");
    return File(std::move(bytes));
}

void File::range(size_t offset, size_t size) const {
    check(offset <= bytes_.size() && size <= bytes_.size() - offset, "DEX range outside image");
}

void File::dataRange(size_t offset, size_t size, size_t alignment) const {
    check(offset >= dataOffset_ && offset % alignment == 0, "invalid DEX data offset");
    range(offset, size);
}

size_t File::entry(size_t table, uint32_t index, size_t width) const {
    const auto& t = tables_[table];
    check(index < t.count, "invalid DEX table index");
    return t.offset + size_t(index) * width;
}

uint16_t File::u16(size_t offset) const {
    range(offset, 2);
    return uint16_t(bytes_[offset] | (uint16_t(bytes_[offset + 1]) << 8));
}

uint32_t File::u32(size_t offset) const {
    range(offset, 4);
    return u16(offset) | (uint32_t(u16(offset + 2)) << 16);
}

std::u16string File::string(uint32_t index) const {
    Cursor in(*this, u32(entry(0, index, 4)));
    const auto units = in.uleb();
    check(units < in.remaining(), "impossible MUTF-8 length");
    std::u16string result;
    result.reserve(units);
    auto continuation = [&] {
        const auto b = in.byte();
        check((b & 0xc0) == 0x80, "invalid MUTF-8 continuation");
        return unsigned(b & 63);
    };
    for (uint32_t i = 0; i < units; ++i) {
        const unsigned a = in.byte();
        unsigned decoded = a;
        if ((a & 0xe0) == 0xc0) {
            decoded = ((a & 31) << 6) | continuation();
            check(decoded == 0 || decoded >= 0x80, "overlong MUTF-8 sequence");
        } else if ((a & 0xf0) == 0xe0) {
            const auto b = continuation();
            decoded = ((a & 15) << 12) | (b << 6) | continuation();
            check(decoded >= 0x800, "overlong MUTF-8 sequence");
        } else {
            check(a && a < 128, "invalid MUTF-8 lead byte");
        }
        result.push_back(char16_t(decoded));
    }
    check(in.byte() == 0, "MUTF-8 length mismatch");
    return result;
}

std::u16string File::type(uint32_t index) const { return string(u32(entry(1, index, 4))); }

std::vector<uint16_t> File::typeList(uint32_t offset) const {
    if (!offset) return {};
    dataRange(offset, 4, 4);
    const auto count = u32(offset);
    const size_t start = size_t(offset) + 4;
    check(count <= (bytes_.size() - start) / 2, "invalid type list size");
    std::vector<uint16_t> result;
    result.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const auto t = u16(start + size_t(i) * 2);
        check(t < tables_[1].count, "invalid type reference");
        result.push_back(t);
    }
    return result;
}

Prototype File::proto(uint32_t index) const {
    const auto p = entry(2, index, 12);
    const auto shorty = u32(p), ret = u32(p + 4);
    check(shorty < tables_[0].count && ret < tables_[1].count, "invalid proto reference");
    return {shorty, ret, typeList(u32(p + 8))};
}

std::u16string File::signature(uint32_t protoIndex) const {
    const auto id = proto(protoIndex);
    std::u16string sig = u"(";
    for (const auto t : id.parameters) sig += type(t);
    sig += u')';
    return sig + type(id.returnType);
}

FieldId File::field(uint32_t index) const {
    const auto p = entry(3, index, 8);
    const FieldId id{u16(p), u16(p + 2), u32(p + 4)};
    check(id.owner < tables_[1].count && id.type < tables_[1].count && id.name < tables_[0].count,
          "invalid field reference");
    return id;
}

MethodId File::method(uint32_t index) const {
    const auto p = entry(4, index, 8);
    const MethodId id{u16(p), u16(p + 2), u32(p + 4)};
    check(id.owner < tables_[1].count && id.proto < tables_[2].count && id.name < tables_[0].count,
          "invalid method reference");
    return id;
}

std::optional<uint32_t> File::findClass(std::u16string_view descriptor) const {
    // class_defs may be ordered by dependencies, rather than by class_idx.
    for (uint32_t i = 0; i < classCount(); ++i)
        if (type(u32(entry(5, i, 32))) == descriptor) return i;
    return {};
}

EncodedValue File::value(Cursor& in, unsigned depth) const {
    check(depth < 64, "nested DEX value too deep");
    const uint8_t header = in.byte(), arg = header >> 5;
    EncodedValue out{uint8_t(header & 31), 0, {}, {}};
    auto readBits = [&](uint8_t maxArg) {
        check(arg <= maxArg, "invalid encoded value width");
        uint64_t bits = 0;
        for (unsigned i = 0; i <= arg; ++i) bits |= uint64_t(in.byte()) << (8 * i);
        return bits;
    };
    auto signedBits = [&](uint8_t maxArg) {
        const auto bits = readBits(maxArg);
        const unsigned shift = 56 - 8 * arg;
        return uint64_t(std::bit_cast<int64_t>(bits << shift) >> shift);
    };
    auto reference = [&](uint32_t count) {
        const auto index = readBits(3);
        check(index < count, "invalid encoded reference");
        return index;
    };
    switch (out.type) {
        case 0x00: out.bits = signedBits(0); break; // byte
        case 0x02: out.bits = signedBits(1); break; // short
        case 0x03: out.bits = readBits(1); break;   // char
        case 0x04: out.bits = signedBits(3); break; // int
        case 0x06: out.bits = signedBits(7); break; // long
        case 0x10: out.bits = readBits(3) << (24 - 8 * arg); break; // float, right-zero-extended
        case 0x11: out.bits = readBits(7) << (56 - 8 * arg); break; // double
        case 0x15: out.bits = reference(tables_[2].count); break;
        case 0x16: out.bits = reference(methodHandles_); break;
        case 0x17: out.bits = reference(tables_[0].count); break;
        case 0x18: out.bits = reference(tables_[1].count); break;
        case 0x19: case 0x1b: out.bits = reference(tables_[3].count); break;
        case 0x1a: out.bits = reference(tables_[4].count); break;
        case 0x1c: case 0x1d: {
            check(!arg, "invalid aggregate value header");
            const bool annotation = out.type == 0x1d;
            if (annotation) {
                out.bits = in.uleb();
                check(out.bits < tables_[1].count, "invalid annotation type");
            }
            const uint32_t count = in.uleb();
            check(count <= in.remaining() / (annotation ? 2 : 1), "invalid encoded value count");
            for (uint32_t i = 0; i < count; ++i) {
                if (annotation) {
                    const auto name = in.uleb();
                    check(name < tables_[0].count && (out.names.empty() || out.names.back() < name),
                          "invalid annotation element");
                    out.names.push_back(name);
                }
                out.values.push_back(value(in, depth + 1));
            }
            break;
        }
        case 0x1e: check(!arg, "invalid null value"); break;
        case 0x1f: check(arg <= 1, "invalid boolean value"); out.bits = arg; break;
        default: throw FormatError("invalid encoded value type");
    }
    return out;
}

Class File::classDef(uint32_t index) const {
    const auto offset = entry(5, index, 32);
    Class cls{u32(offset), u32(offset + 4), u32(offset + 8), u32(offset + 16),
              typeList(u32(offset + 12)), {}, {}, {}, {}, {}};
    check(cls.type < tables_[1].count && (cls.super == noIndex || cls.super < tables_[1].count) &&
          (cls.source == noIndex || cls.source < tables_[0].count), "invalid class reference");
    if (const auto data = u32(offset + 24)) {
        Cursor in(*this, data);
        const auto statics = in.uleb(), instances = in.uleb(), directs = in.uleb(), virtuals = in.uleb();
        check(uint64_t(statics) + instances <= tables_[3].count &&
              uint64_t(directs) + virtuals <= tables_[4].count, "too many encoded members");
        auto fields = [&](uint32_t count, auto& to) {
            uint32_t member = 0;
            for (uint32_t i = 0; i < count; ++i) {
                member = deltaIndex(member, in.uleb(), tables_[3].count, i);
                check(field(member).owner == cls.type, "field belongs to another class");
                to.push_back({member, in.uleb()});
            }
        };
        auto methods = [&](uint32_t count, auto& to) {
            uint32_t member = 0;
            for (uint32_t i = 0; i < count; ++i) {
                member = deltaIndex(member, in.uleb(), tables_[4].count, i);
                check(method(member).owner == cls.type, "method belongs to another class");
                const auto flags = in.uleb(), codeOffset = in.uleb();
                constexpr uint32_t nativeOrAbstract = 0x100 | 0x400;
                check((codeOffset == 0) == bool(flags & nativeOrAbstract), "method code disagrees with access flags");
                if (codeOffset) dataRange(codeOffset, 16, 4);
                to.push_back({member, flags, codeOffset});
            }
        };
        fields(statics, cls.staticFields);
        fields(instances, cls.instanceFields);
        methods(directs, cls.directMethods);
        methods(virtuals, cls.virtualMethods);
    }
    if (const auto values = u32(offset + 28)) {
        Cursor in(*this, values);
        const auto count = in.uleb();
        check(count <= cls.staticFields.size(), "too many encoded static values");
        for (uint32_t i = 0; i < count; ++i) cls.staticValues.push_back(value(in, 0));
    }
    return cls;
}

Code File::code(uint32_t offset) const {
    dataRange(offset, 16, 4);
    const size_t p = offset;
    Code code{u16(p), u16(p + 2), u16(p + 4), u32(p + 8), {}, {}};
    check(code.incoming <= code.registers, "incoming registers exceed frame");
    if (code.debugOffset) dataRange(code.debugOffset, 1);
    const auto tries = u16(p + 6);
    const auto units = u32(p + 12);
    const size_t start = p + 16;
    check(units <= (bytes_.size() - start) / 2, "invalid instruction count");
    code.instructions.reserve(units);
    for (uint32_t i = 0; i < units; ++i) code.instructions.push_back(u16(start + size_t(i) * 2));
    if (!tries) return code;
    const size_t end = start + size_t(units) * 2;
    if (units & 1) check(u16(end) == 0, "invalid code alignment padding");
    const size_t table = end + 2 * (units & 1);
    dataRange(table, size_t(tries) * 8, 4);
    const size_t handlers = table + size_t(tries) * 8;
    Cursor in(*this, handlers);
    const auto entries = in.uleb();
    check(entries <= in.remaining() / 2, "invalid handler list size");
    std::map<size_t, std::vector<Catch>> lists;
    for (uint32_t i = 0; i < entries; ++i) {
        const size_t location = in.position - handlers;
        const auto length = in.sleb();
        const auto typed = length < 0 ? uint64_t(-int64_t(length)) : uint64_t(length);
        check(typed <= in.remaining() / 2, "invalid typed catch count");
        auto& list = lists[location];
        auto add = [&](uint32_t t) {
            const auto address = in.uleb();
            check(address < units, "catch handler outside method");
            list.push_back({t, address});
        };
        for (uint64_t n = 0; n < typed; ++n) {
            const auto t = in.uleb();
            check(t < tables_[1].count, "invalid catch type");
            add(t);
        }
        if (length <= 0) add(noIndex);
    }
    uint32_t previousEnd = 0;
    for (unsigned i = 0; i < tries; ++i) {
        const size_t pos = table + size_t(i) * 8;
        const auto first = u32(pos);
        const auto count = u16(pos + 4);
        const auto list = lists.find(u16(pos + 6));
        check(first >= previousEnd && first < units && count && count <= units - first &&
              list != lists.end(), "invalid try range or handler offset");
        code.tries.push_back({first, count, list->second});
        previousEnd = first + count;
    }
    return code;
}

ClassPath::ClassPath(std::vector<std::string> names, Reader reader) : reader_(std::move(reader)) {
    std::map<uint32_t, std::string> ordered;
    for (auto& name : names)
        if (const auto ordinal = dexOrdinal(name)) ordered.emplace(*ordinal, std::move(name));
    for (auto& [ordinal, name] : ordered) images_.push_back({std::move(name), {}});
}

std::optional<ClassLocation> ClassPath::find(std::u16string_view descriptor) {
    std::lock_guard lock(mutex_);
    for (auto& image : images_) {
        if (!image.file) image.file = std::make_shared<File>(reader_(image.name));
        if (auto id = image.file->findClass(descriptor)) return ClassLocation{image.file, *id};
    }
    return {};
}

} // namespace Hydrogen::Runtime::Android::Dex
