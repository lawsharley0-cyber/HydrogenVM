#include "runtime/android/dex/Machine.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>

using namespace Hydrogen::Runtime::Android::Dex;
namespace {

int failures = 0;
void check(const char* test, bool passed) {
    std::cout << (passed ? "ok " : "FAIL ") << test << '\n';
    failures += !passed;
}

bool value(const Execution& result, unsigned expected) {
    const auto* actual = std::get_if<Value>(&result);
    return actual && actual->kind == ValueKind::Word && actual->bits == expected;
}

bool exception(const Execution& result, std::u16string_view expected) {
    const auto* thrown = std::get_if<JavaException>(&result);
    return thrown && thrown->type == expected;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path fixtures = argv[1];
    unsigned loaded = 0;
    const auto classes = std::make_shared<ClassPath>(std::vector<std::string>{"classes2.dex", "classes.dex"},
        [&](const auto& name) {
            const auto filename = name == "classes.dex" ? "linkage-base.dex" : "linkage-child.dex";
            ++loaded;
            std::ifstream input(fixtures / filename, std::ios::binary);
            if (!input) throw FormatError("missing linked fixture");
            return std::vector<uint8_t>{std::istreambuf_iterator<char>(input), {}};
        });
    std::vector<std::jthread> threads;
    Machine::Host host;
    host.call = [&](std::u16string_view owner, std::u16string_view name, std::u16string_view sig,
                    bool isStatic, bool native, std::span<const Value>) -> std::optional<Value> {
        if (!isStatic && !native && owner == u"Landroid/app/Activity;") {
            if (name == u"getFilesDir" && sig == u"()I") return Value{ValueKind::Word, 40};
            if (name == u"onCreate" && sig == u"()I") return Value{ValueKind::Word, 2};
        }
        return std::nullopt;
    };
    Machine vm(classes, [&](std::function<void()> task) {
        threads.emplace_back(std::move(task));
        return true;
    }, host);
    check("superclass initializer runs before subclass once across DEX images",
          value(vm.callStatic(u"Ltest/Child;", u"readOrder", u"()I"), 12) &&
          value(vm.callStatic(u"Ltest/Child;", u"readOrder", u"()I"), 12) && loaded == 2);
    check("interface dispatch finds subclass method and invoke-super uses the parent",
          value(vm.callStatic(u"Ltest/Child;", u"throughInterface", u"()I"), 12));
    check("virtual dispatch uses the vtable of the actual receiver",
          value(vm.callStatic(u"Ltest/Child;", u"throughBase", u"()I"), 11));
    // JLS 15.12.4.4 selects the implementation on the actual receiver.
    check("platform Callable and Object calls dispatch to DEX overrides",
          value(vm.callStatic(u"Ltest/Child;", u"platformDispatch", u"()I"), 71));
    check("a DEX class is assignable to its platform interface",
          vm.heap().assignable(u"Ltest/Child;", u"Ljava/util/concurrent/Callable;"));
    check("Java exception base classes participate in transitive matching",
          vm.heap().assignable(u"Ljava/lang/RuntimeException;", u"Ljava/lang/Throwable;") &&
          vm.heap().assignable(u"Ljava/lang/IllegalThreadStateException;", u"Ljava/lang/IllegalArgumentException;") &&
          vm.heap().assignable(u"Ljava/lang/NoSuchFieldError;", u"Ljava/lang/IncompatibleClassChangeError;") &&
          !vm.heap().assignable(u"Ljava/lang/LinkageError;", u"Ljava/lang/Exception;"));
    const auto fresh = vm.heap().newString(u"same");
    check("equal freshly allocated strings retain distinct Java identities",
          fresh != vm.heap().newString(u"same") && fresh != vm.heap().intern(u"same") &&
          vm.heap().string(fresh) == u"same");
    check("filled-new-array returns a reference through move-result-object",
          value(vm.callStatic(u"Ltest/Child;", u"arraySum", u"()I"), 7));
    check("failed class initialization is reported and never rerun",
          exception(vm.callStatic(u"Ltest/BadInit;", u"use", u"()V"), u"Ljava/lang/ExceptionInInitializerError;") &&
          exception(vm.callStatic(u"Ltest/BadInit;", u"use", u"()V"), u"Ljava/lang/NoClassDefFoundError;"));
    check("two Java Thread.start calls contend on a class monitor and join",
          value(vm.callStatic(u"Ltest/Worker;", u"parallel", u"()I"), 1000) && threads.size() == 2);
    check("subclass-owned Thread.start and join route through unlinked Thread ancestor",
          value(vm.callStatic(u"Ltest/Worker;", u"startThroughSubclass", u"()I"), 1500) && threads.size() == 3);
    check("inherited virtual and super calls on unlinked platform superclass reach host boundary",
          value(vm.callStatic(u"Ltest/PlatformChild;", u"callInherited", u"()I"), 42));
    const Value childRef{ValueKind::Reference, vm.heap().object(u"Ltest/Child;")};
    check("virtual JNI callInstance falls back to localMethod for private instance methods",
          vm.hasMethod(u"Ltest/Child;", u"secret", u"()I", false) &&
          value(vm.callInstance(u"Ltest/Child;", u"secret", u"()I", {&childRef, 1}, false), 99));
    std::cout << "done: " << failures << " failure(s)\n";
    return failures != 0;
}
