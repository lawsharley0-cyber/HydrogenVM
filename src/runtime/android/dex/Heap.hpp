// Object identities seen by Dalvik code. Arena implements this interface until
// tracing GC is available.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <functional>
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

// JNI and the interpreter use the same backing once an array crosses the boundary.
struct ArrayBacking {
    std::function<Value(uint32_t)> read;
    std::function<void(uint32_t, Value)> write;
};

class Heap {
public:
    virtual ~Heap() = default;
    virtual uint32_t object(std::u16string type) = 0;
    virtual uint32_t newString(std::u16string text) = 0;
    virtual uint32_t intern(std::u16string text) = 0;
    virtual uint32_t array(std::u16string type, int32_t length) = 0;
    virtual std::u16string typeOf(uint32_t reference) const = 0;
    virtual std::u16string string(uint32_t reference) const = 0;
    virtual void setString(uint32_t reference, std::u16string text) = 0;
    virtual uint32_t length(uint32_t reference) const = 0;
    virtual Value element(uint32_t reference, int32_t index) const = 0;
    virtual void setElement(uint32_t reference, int32_t index, Value value) = 0;
    virtual void bindArray(uint32_t reference, std::shared_ptr<ArrayBacking> backing, bool copyElements) = 0;
    virtual void enter(uint32_t reference) = 0;
    virtual void leave(uint32_t reference, bool requireOwner = true) = 0;
    virtual void wait(uint32_t reference, int64_t millis, int32_t nanos) = 0;
    virtual void notify(uint32_t reference, bool all) = 0;
    virtual bool assignable(std::u16string_view from, std::u16string_view to) const = 0;
};

class Arena final : public Heap {
public:
    using ClassAssignable = std::function<bool(std::u16string_view, std::u16string_view)>;
    explicit Arena(size_t byteLimit = 128 * 1024 * 1024, ClassAssignable classAssignable = {},
                   std::function<void()> checkMonitorStop = {});
    ~Arena() override;
    uint32_t object(std::u16string type) override;
    uint32_t newString(std::u16string text) override;
    uint32_t intern(std::u16string text) override;
    uint32_t array(std::u16string type, int32_t length) override;
    std::u16string typeOf(uint32_t reference) const override;
    std::u16string string(uint32_t reference) const override;
    void setString(uint32_t reference, std::u16string text) override;
    uint32_t length(uint32_t reference) const override;
    Value element(uint32_t reference, int32_t index) const override;
    void setElement(uint32_t reference, int32_t index, Value value) override;
    void bindArray(uint32_t reference, std::shared_ptr<ArrayBacking> backing, bool copyElements) override;
    void enter(uint32_t reference) override;
    void leave(uint32_t reference, bool requireOwner = true) override;
    void wait(uint32_t reference, int64_t millis, int32_t nanos) override;
    void notify(uint32_t reference, bool all) override;
    bool assignable(std::u16string_view from, std::u16string_view to) const override;

private:
    struct Storage;
    std::unique_ptr<Storage> storage_;
    ClassAssignable classAssignable_;
    std::function<void()> checkMonitorStop_;
};

} // namespace Hydrogen::Runtime::Android::Dex
