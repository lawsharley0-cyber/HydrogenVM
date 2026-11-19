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
    Machine vm(classes, [&](std::function<void()> task) {
        threads.emplace_back(std::move(task));
        return true;
    });
    check("superclass initializer runs before subclass once across DEX images",
          value(vm.callStatic(u"Ltest/Child;", u"readOrder", u"()I"), 12) &&
          value(vm.callStatic(u"Ltest/Child;", u"readOrder", u"()I"), 12) && loaded == 2);
    check("interface dispatch finds subclass method and invoke-super uses the parent",
          value(vm.callStatic(u"Ltest/Child;", u"throughInterface", u"()I"), 12));
    check("virtual dispatch uses the vtable of the actual receiver",
          value(vm.callStatic(u"Ltest/Child;", u"throughBase", u"()I"), 11));
    check("filled-new-array returns a reference through move-result-object",
          value(vm.callStatic(u"Ltest/Child;", u"arraySum", u"()I"), 7));
    check("failed class initialization is reported and never rerun",
          exception(vm.callStatic(u"Ltest/BadInit;", u"use", u"()V"), u"Ljava/lang/ExceptionInInitializerError;") &&
          exception(vm.callStatic(u"Ltest/BadInit;", u"use", u"()V"), u"Ljava/lang/NoClassDefFoundError;"));
    check("two Java Thread.start calls contend on a class monitor and join",
          value(vm.callStatic(u"Ltest/Worker;", u"parallel", u"()I"), 1000) && threads.size() == 2);
    std::cout << "done: " << failures << " failure(s)\n";
    return failures != 0;
}
