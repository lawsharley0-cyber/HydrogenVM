// Host DEX verification of CoreLang (wrappers, String, StringBuilder, System.arraycopy) per Java SE spec.
#include "runtime/android/dex/CoreLang.hpp"
#include "runtime/android/dex/Machine.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <limits>
#include <thread>
#include <vector>

using namespace Hydrogen::Runtime::Android::Dex;
namespace {

int failures = 0;
void check(const char* test, bool passed) {
    std::cout << (passed ? "ok " : "FAIL ") << test << '\n';
    failures += !passed;
}

Value word(int32_t v) { return {ValueKind::Word, uint32_t(v)}; }
Value wide(int64_t v) { return {ValueKind::Wide, uint64_t(v)}; }
Value f32(float v) { return {ValueKind::Word, std::bit_cast<uint32_t>(v)}; }
Value ref(uint32_t id) { return {ValueKind::Reference, id}; }

bool isWord(const Execution& result, int32_t expected) {
    const auto* v = std::get_if<Value>(&result);
    return v && v->kind == ValueKind::Word && std::bit_cast<int32_t>(uint32_t(v->bits)) == expected;
}

bool isWide(const Execution& result, int64_t expected) {
    const auto* v = std::get_if<Value>(&result);
    return v && v->kind == ValueKind::Wide && std::bit_cast<int64_t>(v->bits) == expected;
}

bool isString(Heap& heap, const Execution& result, std::u16string_view expected) {
    const auto* v = std::get_if<Value>(&result);
    return v && v->kind == ValueKind::Reference && v->bits != 0 && heap.string(uint32_t(v->bits)) == expected;
}

bool isException(const Execution& result, std::u16string_view type) {
    const auto* ex = std::get_if<JavaException>(&result);
    return ex && ex->type == type;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path fixtures = argv[1];
    const auto classes = std::make_shared<ClassPath>(std::vector<std::string>{"classes.dex"},
        [&](const auto&) {
            std::ifstream input(fixtures / "lang.dex", std::ios::binary);
            if (!input) throw FormatError("missing lang.dex fixture");
            return std::vector<uint8_t>{std::istreambuf_iterator<char>(input), {}};
        });

    CoreLang lang;
    Machine* vmPtr = nullptr;
    Machine::Host host;
    host.hasClass = [](std::u16string_view type) { return CoreLang::hasClass(type); };
    host.call = [&](std::u16string_view type, std::u16string_view name, std::u16string_view sig,
                    bool isStatic, bool isNative, std::span<const Value> args) -> std::optional<Value> {
        if (isNative) return {};
        return lang.invoke(vmPtr->heap(), type, name, sig, isStatic, args);
    };
    Machine vm(classes, {}, host);
    vmPtr = &vm;
    auto& heap = vm.heap();
    const auto cls = u"Ltest/LangChecks;";

    // JLS 17.2 specifies monitor ownership and per-object wait sets.
    using namespace std::chrono_literals;
    const auto observed = heap.object(u"Ljava/lang/Object;");
    const auto other = heap.object(u"Ljava/lang/Object;");
    auto objectCall = [&](std::u16string_view name, std::u16string_view signature,
                          std::span<const Value> args) {
        return lang.invoke(heap, u"Ljava/lang/Object;", name, signature, false, args);
    };
    auto rejectsNonOwner = [&](std::u16string_view name) {
        try {
            objectCall(name, u"()V", std::array{ref(observed)});
        } catch (const JavaException& ex) {
            return ex.type == u"Ljava/lang/IllegalMonitorStateException;";
        }
        return false;
    };
    check("Object wait, notify and notifyAll require monitor ownership",
          rejectsNonOwner(u"wait") && rejectsNonOwner(u"notify") && rejectsNonOwner(u"notifyAll"));

    std::promise<void> entered;
    std::promise<bool> finished;
    auto enteredFuture = entered.get_future();
    auto finishedFuture = finished.get_future();
    std::thread waiter([&] {
        try {
            heap.enter(observed);
            heap.enter(observed);
            entered.set_value();
            objectCall(u"wait", u"(J)V", std::array{ref(observed), wide(2000)});
            heap.leave(observed);
            heap.leave(observed);
            finished.set_value(true);
        } catch (...) {
            finished.set_exception(std::current_exception());
        }
    });
    enteredFuture.wait();
    heap.enter(observed);
    heap.leave(observed);
    heap.enter(other);
    objectCall(u"notifyAll", u"()V", std::array{ref(other)});
    heap.leave(other);
    check("Object wait releases its monitor and ignores notification on another object",
          finishedFuture.wait_for(40ms) == std::future_status::timeout);
    heap.enter(observed);
    objectCall(u"notify", u"()V", std::array{ref(observed)});
    check("Object wait reacquires its monitor before returning",
          finishedFuture.wait_for(40ms) == std::future_status::timeout);
    heap.leave(observed);
    const bool woke = finishedFuture.wait_for(500ms) == std::future_status::ready;
    waiter.join();
    check("Object notify wakes a waiter with its recursive ownership restored",
          woke && finishedFuture.get());

    heap.enter(observed);
    const auto start = std::chrono::steady_clock::now();
    objectCall(u"wait", u"(J)V", std::array{ref(observed), wide(25)});
    heap.leave(observed);
    check("Object timed wait returns after its deadline",
          std::chrono::steady_clock::now() - start >= 25ms);

    struct MonitorStopped {};
    std::atomic<bool> stopping = false;
    Arena cancellable(1024 * 1024, {}, [&] {
        if (stopping.load()) throw MonitorStopped{};
    });
    const auto blocked = cancellable.object(u"Ljava/lang/Object;");
    std::promise<void> acquired;
    auto acquiredFuture = acquired.get_future();
    auto cancelled = std::async(std::launch::async, [&] {
        cancellable.enter(blocked);
        acquired.set_value();
        try {
            cancellable.wait(blocked, 2000, 0);
        } catch (const MonitorStopped&) {
            return true;
        }
        cancellable.leave(blocked);
        return false;
    });
    acquiredFuture.get();
    cancellable.enter(blocked);
    stopping.store(true);
    const bool stopped = cancelled.wait_for(500ms) == std::future_status::ready;
    cancellable.leave(blocked);
    check("VM shutdown cancels an Object wait even while another thread holds the monitor",
          cancelled.get() && stopped);

    // Bit operations on Long and Integer per Java SE spec.
    bool bitsOk = true;
    bitsOk &= isWord(vm.callStatic(cls, u"longLeadingZeros", u"(J)I", std::array{wide(0)}), 64);
    bitsOk &= isWord(vm.callStatic(cls, u"longLeadingZeros", u"(J)I", std::array{wide(1)}), 63);
    bitsOk &= isWord(vm.callStatic(cls, u"longLeadingZeros", u"(J)I", std::array{wide(INT64_MIN)}), 0);
    bitsOk &= isWord(vm.callStatic(cls, u"intTrailingZeros", u"(I)I", std::array{word(0)}), 32);
    bitsOk &= isWord(vm.callStatic(cls, u"intTrailingZeros", u"(I)I", std::array{word(0x18)}), 3);
    bitsOk &= isWord(vm.callStatic(cls, u"intReverseBytes", u"(I)I", std::array{word(0x12345678)}), 0x78563412);
    check("Long/Integer bit helpers match Java SE counts and byte order", bitsOk);

    // Radix parsing, MIN_VALUE boundary, overflow rejection, and formatting.
    bool parseOk = true;
    parseOk &= isWide(vm.callStatic(cls, u"parseLongRadix", u"(Ljava/lang/String;I)J",
                                    std::array{ref(heap.newString(u"-9223372036854775808")), word(10)}), INT64_MIN);
    parseOk &= isWide(vm.callStatic(cls, u"parseLongRadix", u"(Ljava/lang/String;I)J",
                                    std::array{ref(heap.newString(u"7f")), word(16)}), 127);
    parseOk &= isException(vm.callStatic(cls, u"parseLongRadix", u"(Ljava/lang/String;I)J",
                                         std::array{ref(heap.newString(u"9223372036854775808")), word(10)}),
                           u"Ljava/lang/NumberFormatException;");
    parseOk &= isString(heap, vm.callStatic(cls, u"longToRadix", u"(JI)Ljava/lang/String;",
                                            std::array{wide(-255), word(16)}), u"-ff");
    check("Long.parseLong and Long.toString enforce radix, MIN_VALUE, and overflow", parseOk);

    // Float canonical NaN bits, Java total-order compare, and Integer->Number polymorphic unboxing.
    const float signalingNaN = std::bit_cast<float>(0x7f800001u);
    bool floatBoxOk = true;
    floatBoxOk &= isWord(vm.callStatic(cls, u"floatToBits", u"(F)I", std::array{f32(signalingNaN)}), 0x7fc00000);
    floatBoxOk &= isWord(vm.callStatic(cls, u"floatCompare", u"(FF)I", std::array{f32(-0.0f), f32(0.0f)}), -1);
    floatBoxOk &= isWord(vm.callStatic(cls, u"floatCompare", u"(FF)I", std::array{f32(signalingNaN), f32(1e30f)}), 1);
    floatBoxOk &= isWide(vm.callStatic(cls, u"boxRoundTrip", u"(I)J", std::array{word(-12345)}), -12345);
    check("Float canonical NaN/compare and Number boxing dispatch match Java SE", floatBoxOk);

    // StringBuilder and String operations (including exact Java hashCode).
    bool strOk = true;
    strOk &= isString(heap, vm.callStatic(cls, u"buildGreeting", u"(Ljava/lang/String;IZ)Ljava/lang/String;",
                                          std::array{ref(heap.newString(u"level")), word(7), word(1)}),
                      u"level:7=true");
    // "abc".hashCode() == 97*31*31 + 98*31 + 99 == 96354
    strOk &= isWord(vm.callStatic(cls, u"stringHash", u"(Ljava/lang/String;)I",
                                  std::array{ref(heap.newString(u"abc"))}), 96354);
    strOk &= isString(heap, vm.callStatic(cls, u"stringSubstring", u"(Ljava/lang/String;II)Ljava/lang/String;",
                                          std::array{ref(heap.newString(u"dungeon")), word(1), word(5)}), u"unge");
    strOk &= isString(heap, vm.callStatic(cls, u"stringReplace",
                                          u"(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
                                          std::array{ref(heap.newString(u"a-b-c")), ref(heap.newString(u"-")),
                                                     ref(heap.newString(u"::"))}), u"a::b::c");
    check("StringBuilder and String hash/substring/replace match Java SE", strOk);

    // System.arraycopy with overlapping forward shift in the same array.
    const uint32_t arr = heap.array(u"[I", 5);
    for (int32_t i = 0; i < 5; ++i) heap.setElement(arr, i, word(i + 1));
    const auto copyRes = vm.callStatic(cls, u"overlapCopy", u"([IIII)V",
                                       std::array{ref(arr), word(0), word(1), word(4)});
    bool copyOk = std::holds_alternative<Value>(copyRes);
    const std::array<int32_t, 5> expected{1, 1, 2, 3, 4};
    for (int32_t i = 0; i < 5; ++i)
        copyOk &= std::bit_cast<int32_t>(uint32_t(heap.element(arr, i).bits)) == expected[size_t(i)];
    check("System.arraycopy handles overlapping intra-array shifts", copyOk);

    std::cout << "done: " << failures << " failures\n";
    return failures ? 1 : 0;
}
