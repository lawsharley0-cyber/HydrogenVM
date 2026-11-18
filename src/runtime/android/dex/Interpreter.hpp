// Register interpreter for Dalvik code items.
#pragma once

#include "runtime/android/dex/DexFile.hpp"
#include "runtime/android/dex/Heap.hpp"

#include <span>
#include <variant>

namespace Hydrogen::Runtime::Android::Dex {

using Execution = std::variant<Value, JavaException>;

// Arguments are Java values in declaration order; a nonstatic receiver is first.
// This execution slice covers scalar operations, control flow, arrays, strings, and monitors. Unimplemented
// opcodes raise FormatError; Java exceptions use the separate JavaException result.
Execution execute(Heap& heap, const File& file, const EncodedMethod& method,
                  std::span<const Value> args = {}, uint64_t instructionLimit = 1'000'000);

} // namespace Hydrogen::Runtime::Android::Dex
