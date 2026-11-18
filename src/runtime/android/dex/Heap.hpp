// Object identities seen by Dalvik code. Arena implements this interface until
// tracing GC is available.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace Hydrogen::Runtime::Android::Dex {

enum class ValueKind { Void, Word, Wide, Reference };
struct Value {
    ValueKind kind;
    uint64_t bits = 0;
};
struct JavaException {
    std::u16string type;
    uint32_t reference = 0;
};

class Heap {
public:
    virtual ~Heap() = default;
    virtual uint32_t object(std::u16string type) = 0;
    virtual uint32_t intern(std::u16string text) = 0;
    virtual uint32_t array(std::u16string type, int32_t length) = 0;
    virtual std::u16string typeOf(uint32_t reference) const = 0;
    virtual std::u16string string(uint32_t reference) const = 0;
    virtual uint32_t length(uint32_t reference) const = 0;
    virtual Value element(uint32_t reference, int32_t index) const = 0;
    virtual void setElement(uint32_t reference, int32_t index, Value value) = 0;
    virtual void enter(uint32_t reference) = 0;
    virtual void leave(uint32_t reference) = 0;
    virtual bool assignable(std::u16string_view from, std::u16string_view to) const = 0;
};

class Arena final : public Heap {
public:
    explicit Arena(size_t byteLimit = 128 * 1024 * 1024);
    ~Arena() override;
    uint32_t object(std::u16string type) override;
    uint32_t intern(std::u16string text) override;
    uint32_t array(std::u16string type, int32_t length) override;
    std::u16string typeOf(uint32_t reference) const override;
    std::u16string string(uint32_t reference) const override;
    uint32_t length(uint32_t reference) const override;
    Value element(uint32_t reference, int32_t index) const override;
    void setElement(uint32_t reference, int32_t index, Value value) override;
    void enter(uint32_t reference) override;
    void leave(uint32_t reference) override;
    bool assignable(std::u16string_view from, std::u16string_view to) const override;

private:
    struct Storage;
    std::unique_ptr<Storage> storage_;
};

} // namespace Hydrogen::Runtime::Android::Dex
