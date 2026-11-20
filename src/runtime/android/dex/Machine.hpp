// A linked set of DEX classes sharing one object arena.
#pragma once

#include "runtime/android/dex/Interpreter.hpp"

namespace Hydrogen::Runtime::Android::Dex {

class Machine {
public:
    using Scheduler = std::function<bool(std::function<void()>)>;
    using Boundary = std::function<std::optional<Value>(std::u16string_view, std::u16string_view,
                                                         std::u16string_view, bool, bool, std::span<const Value>)>;
    struct Host {
        Boundary call;
        std::function<bool(std::u16string_view)> hasClass;
        std::function<std::optional<Value>(std::u16string_view, std::u16string_view, std::u16string_view,
                                          std::optional<uint32_t>, std::optional<Value>)> field;
        std::function<void()> checkMonitorStop;
    };
    explicit Machine(std::shared_ptr<ClassPath> classPath, Scheduler scheduler = {}, Host host = {});
    ~Machine();
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;

    Execution callStatic(std::u16string_view type, std::u16string_view name,
                         std::u16string_view signature, std::span<const Value> args = {},
                         uint64_t instructionLimit = 1'000'000);
    bool hasMethod(std::u16string_view type, std::u16string_view name, std::u16string_view signature, bool isStatic);
    Execution callInstance(std::u16string_view type, std::u16string_view name,
                           std::u16string_view signature, std::span<const Value> args, bool nonvirtual);
    struct ClassInfo {
        std::u16string name;
        std::u16string superName;
        uint32_t flags = 0;
        bool defined = false;
    };
    uint32_t classMirror(std::u16string_view type);
    std::optional<std::u16string> classMirrorType(uint32_t reference) const;
    ClassInfo classInfo(std::u16string_view type);
    bool isAssignable(std::u16string_view from, std::u16string_view to);
    Execution instantiate(std::u16string_view type, std::u16string_view ctorSig,
                          std::span<const Value> ctorArgs);
    Heap& heap();

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace Hydrogen::Runtime::Android::Dex
