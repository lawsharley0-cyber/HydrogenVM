// Register interpreter for Dalvik code items.
#pragma once

#include "runtime/android/dex/DexFile.hpp"
#include "runtime/android/dex/Heap.hpp"

#include <span>
#include <variant>

namespace Hydrogen::Runtime::Android::Dex {

using Execution = std::variant<Value, JavaException>;

enum class InvokeKind { Virtual, Super, Direct, Static, Interface };
struct Budget { uint64_t remaining; unsigned depth = 0; };

// Services shared by linked interpreter frames.
class Linkage {
public:
    virtual ~Linkage() = default;
    virtual uint32_t newInstance(std::u16string type, Budget& budget) = 0;
    virtual uint32_t classObject(std::u16string type) = 0;
    virtual Value field(const File& file, uint32_t index, std::optional<uint32_t> receiver,
                        std::optional<Value> write, Budget& budget) = 0;
    virtual Value invoke(const File& file, const EncodedMethod& caller, uint32_t methodIndex,
                         InvokeKind kind, std::span<const Value> args, Budget& budget) = 0;
};

Execution executeLinked(Heap& heap, Linkage* linkage, const File& file, const EncodedMethod& method,
                        std::span<const Value> args, Budget& budget);

// Arguments are Java values in declaration order; a nonstatic receiver is first.
// This execution slice covers scalar operations, control flow, arrays, strings, and monitors. Unimplemented
// opcodes raise FormatError; Java exceptions use the separate JavaException result.
Execution execute(Heap& heap, const File& file, const EncodedMethod& method,
                  std::span<const Value> args = {}, uint64_t instructionLimit = 1'000'000);

} // namespace Hydrogen::Runtime::Android::Dex
