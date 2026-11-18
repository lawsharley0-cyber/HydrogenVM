// A linked set of DEX classes sharing one object arena.
#pragma once

#include "runtime/android/dex/Interpreter.hpp"

namespace Hydrogen::Runtime::Android::Dex {

class Machine {
public:
    using Scheduler = std::function<bool(std::function<void()>)>;
    explicit Machine(std::shared_ptr<ClassPath> classPath, Scheduler scheduler = {});
    ~Machine();
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;

    Execution callStatic(std::u16string_view type, std::u16string_view name,
                         std::u16string_view signature, std::span<const Value> args = {},
                         uint64_t instructionLimit = 1'000'000);
    Heap& heap();

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace Hydrogen::Runtime::Android::Dex
