// A bounds-checked, lazy view of a standalone little-endian DEX image.
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Hydrogen::Runtime::Android::Dex {

constexpr uint32_t noIndex = 0xffffffffu;

class FormatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Prototype {
    uint32_t shorty, returnType;
    std::vector<uint16_t> parameters;
};
struct FieldId { uint16_t owner, type; uint32_t name; };
struct MethodId { uint16_t owner, proto; uint32_t name; };

struct EncodedValue {
    uint8_t type;
    uint64_t bits = 0; // Primitive bits, reference table index, or annotation type.
    std::vector<EncodedValue> values;
    std::vector<uint32_t> names; // Annotation element string indices.
};
struct EncodedField { uint32_t index, flags; };
struct EncodedMethod { uint32_t index, flags, codeOffset; };
struct Class {
    uint32_t type, flags, super, source;
    std::vector<uint16_t> interfaces;
    std::vector<EncodedField> staticFields, instanceFields;
    std::vector<EncodedMethod> directMethods, virtualMethods;
    std::vector<EncodedValue> staticValues;
};
struct Catch { uint32_t type, address; }; // noIndex denotes catch-all.
struct TryBlock {
    uint32_t start;
    uint16_t count;
    std::vector<Catch> catches;
};
struct Code {
    uint16_t registers, incoming, outgoing;
    uint32_t debugOffset;
    std::vector<uint16_t> instructions;
    std::vector<TryBlock> tries;
};

class File {
public:
    explicit File(std::vector<uint8_t> bytes);
    static File read(const std::filesystem::path& path);
    std::u16string string(uint32_t index) const;
    std::u16string type(uint32_t index) const;
    Prototype proto(uint32_t index) const;
    std::u16string signature(uint32_t protoIndex) const;
    FieldId field(uint32_t index) const;
    MethodId method(uint32_t index) const;
    std::optional<uint32_t> findClass(std::u16string_view descriptor) const;
    Class classDef(uint32_t index) const;
    Code code(uint32_t offset) const;
    uint32_t classCount() const { return tables_[5].count; }

private:
    struct Table { uint32_t count, offset; };
    class Cursor;
    size_t entry(size_t table, uint32_t index, size_t width) const;
    void range(size_t offset, size_t size) const;
    void dataRange(size_t offset, size_t size, size_t alignment = 1) const;
    uint16_t u16(size_t offset) const;
    uint32_t u32(size_t offset) const;
    std::vector<uint16_t> typeList(uint32_t offset) const;
    EncodedValue value(Cursor& in, unsigned depth) const;
    std::vector<uint8_t> bytes_;
    Table tables_[6]{};
    uint32_t dataOffset_ = 0, methodHandles_ = 0;
};

struct ClassLocation { std::shared_ptr<const File> file; uint32_t index; };

// Reads only the images a lookup reaches, in classes.dex, classes2.dex, … order.
// The caller supplies names present in the bundle and the corresponding reader.
class ClassPath {
public:
    using Reader = std::function<std::vector<uint8_t>(const std::string&)>;
    ClassPath(std::vector<std::string> names, Reader reader);
    std::optional<ClassLocation> find(std::u16string_view descriptor);
private:
    struct Image { std::string name; std::shared_ptr<const File> file; };
    std::vector<Image> images_;
    Reader reader_;
    std::mutex mutex_;
};

} // namespace Hydrogen::Runtime::Android::Dex
