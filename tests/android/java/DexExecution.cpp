#include "runtime/android/dex/Interpreter.hpp"

#include <algorithm>
#include <bit>
#include <barrier>
#include <thread>
#include <iostream>

using namespace Hydrogen::Runtime::Android::Dex;
namespace {

int failures = 0;
void test(const char* name, bool passed) {
    std::cout << (passed ? "ok " : "FAIL ") << name << '\n';
    failures += !passed;
}

Value word(uint32_t bits) { return {ValueKind::Word, bits}; }
Value ref(uint32_t reference) { return {ValueKind::Reference, reference}; }
Value wide(uint64_t bits) { return {ValueKind::Wide, bits}; }

bool resultIs(const Execution& result, Value expected) {
    const auto* value = std::get_if<Value>(&result);
    return value && value->kind == expected.kind && value->bits == expected.bits;
}

bool thrownIs(const Execution& result, std::u16string_view type) {
    const auto* exception = std::get_if<JavaException>(&result);
    return exception && exception->type == type;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const File file = File::read(std::filesystem::path(argv[1]) / "arithmetic.dex");
    const auto index = file.findClass(u"Ltest/Arithmetic;");
    if (!index) return 2;
    const auto cls = file.classDef(*index);
    Arena heap;
    auto run = [&](std::u16string_view name, std::initializer_list<Value> args = {}, uint64_t fuel = 1000) {
        const auto found = std::find_if(cls.directMethods.begin(), cls.directMethods.end(), [&](const auto& method) {
            return file.string(file.method(method.index).name) == name;
        });
        if (found == cls.directMethods.end()) throw FormatError("missing arithmetic fixture");
        return execute(heap, file, *found, args, fuel);
    };

    test("int add wraps at 2^31", resultIs(run(u"wrapInt"), word(0x80000000)));
    test("int MIN_VALUE divided by -1 wraps", resultIs(run(u"divide", {word(0x80000000), word(0xffffffff)}), word(0x80000000)));
    test("long MIN_VALUE divided by -1 wraps across a register pair",
         resultIs(run(u"wide", {wide(0x8000000000000000ULL), wide(UINT64_MAX)}), wide(0x8000000000000000ULL)));
    test("integer divide by zero raises ArithmeticException",
         thrownIs(run(u"divide", {word(7), word(0)}), u"Ljava/lang/ArithmeticException;"));
    test("float one divided by negative zero is negative infinity",
         resultIs(run(u"divFloat", {word(0x3f800000), word(0x80000000)}), word(0xff800000)));
    test("double negative zero survives multiplication by three",
         resultIs(run(u"mulDouble", {wide(0x8000000000000000ULL), wide(0x4008000000000000ULL)}),
                  wide(0x8000000000000000ULL)));
    test("cmpg treats NaN as greater",
         resultIs(run(u"cmpg", {word(0x7fc00000), word(0x3f800000)}), word(1)));
    test("double to long saturates positive infinity and truncates negative fractions",
         resultIs(run(u"toLong", {wide(0x7ff0000000000000ULL)}), wide(INT64_MAX)) &&
         resultIs(run(u"toLong", {wide(std::bit_cast<uint64_t>(-3.75))}), wide(uint64_t(-3))));
    test("nested catch-all cleanup runs once then rethrows to the typed handler",
         resultIs(run(u"catchFinally"), word(6)));
    test("throwing null raises NullPointerException",
         thrownIs(run(u"throwNull"), u"Ljava/lang/NullPointerException;"));
    test("packed switch matches and falls through",
         resultIs(run(u"packed", {word(7)}), word(40)) &&
         resultIs(run(u"packed", {word(8)}), word(UINT32_MAX)));
    test("sparse switch matches a negative key and falls through",
         resultIs(run(u"sparse", {word(uint32_t(-19))}), word(45)) &&
         resultIs(run(u"sparse", {word(19)}), word(UINT32_MAX)));
    bool stopped = false;
    try { run(u"loop", {}, 16); }
    catch (const FormatError& error) { stopped = std::string_view(error.what()) == "Dalvik instruction limit exceeded"; }
    test("instruction limit bounds a nonterminating method", stopped);
    const File objects = File::read(std::filesystem::path(argv[1]) / "objects.dex");
    const auto classId = objects.findClass(u"Ltest/Objects;");
    if (!classId) return 2;
    const auto methods = objects.classDef(*classId).directMethods;
    auto runObject = [&](std::u16string_view name, std::initializer_list<Value> args = {}) {
        const auto found = std::find_if(methods.begin(), methods.end(), [&](const auto& method) {
            return objects.string(objects.method(method.index).name) == name;
        });
        if (found == methods.end()) throw FormatError("missing object fixture");
        return execute(heap, objects, *found, args, 10000);
    };
    const auto text = runObject(u"text"), sameText = runObject(u"text");
    const auto* str = std::get_if<Value>(&text);
    test("UTF-16 string constants preserve NUL and surrogate pairs and share identity",
         str && str->kind == ValueKind::Reference && resultIs(sameText, *str) &&
         heap.string(uint32_t(str->bits)) == std::u16string({u'H', 0, 0xd83d, 0xde00}));
    test("fill-array-data loads signed byte elements", resultIs(runObject(u"filled"), word(uint32_t(-128))));
    const auto arr = runObject(u"array", {word(1)});
    const auto* value = std::get_if<Value>(&arr);
    if (!value || value->kind != ValueKind::Reference) return 1;
    test("new-array defaults all elements to zero", resultIs(runObject(u"read", {*value, word(0)}), word(0)));
    test("out-of-range index raises ArrayIndexOutOfBoundsException",
         thrownIs(runObject(u"read", {*value, word(1)}), u"Ljava/lang/ArrayIndexOutOfBoundsException;"));
    test("null array raises NullPointerException",
         thrownIs(runObject(u"read", {ref(0), word(0)}), u"Ljava/lang/NullPointerException;"));
    test("check-cast rejects String as int array", thrownIs(runObject(u"cast"), u"Ljava/lang/ClassCastException;"));
    test("every array is an Object and null is not an instance",
         resultIs(runObject(u"probe", {*value}), word(1)) && resultIs(runObject(u"probe", {ref(0)}), word(0)));
    test("arena rejects a huge Java array with OutOfMemoryError",
         thrownIs(runObject(u"array", {word(INT32_MAX)}), u"Ljava/lang/OutOfMemoryError;"));

    std::barrier start(3);
    Execution first{Value{ValueKind::Void}}, second{Value{ValueKind::Void}};
    auto worker = [&](Execution& result) {
        start.arrive_and_wait();
        result = runObject(u"add", {*value});
    };
    std::thread a(worker, std::ref(first)), b(worker, std::ref(second));
    start.arrive_and_wait();
    a.join();
    b.join();
    test("two interpreter frames serialize through a recursively entered monitor",
         resultIs(first, {ValueKind::Void}) && resultIs(second, {ValueKind::Void}) &&
         resultIs(runObject(u"read", {*value, word(0)}), word(1000)));
    std::cout << "done: " << failures << " failure(s)\n";
    return failures != 0;
}
