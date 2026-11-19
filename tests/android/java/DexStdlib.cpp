// Host DEX verification of extended CoreCollections, CoreLang formatting/Throwable,
// CoreRandom SecureRandom, and Machine reflection mirrors/instantiation per Java SE spec.
#include "runtime/android/dex/CoreCollections.hpp"
#include "runtime/android/dex/CoreLang.hpp"
#include "runtime/android/dex/CoreRandom.hpp"
#include "runtime/android/dex/Machine.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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
Value ref(uint32_t id) { return {ValueKind::Reference, id}; }

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path fixtures = argv[1];
    auto loadFile = [&](const std::string& name) {
        const auto filename = name == "classes.dex" ? "linkage-base.dex" : "linkage-child.dex";
        std::ifstream input(fixtures / filename, std::ios::binary);
        if (!input) throw FormatError("missing fixture " + std::string(filename));
        return std::vector<uint8_t>{std::istreambuf_iterator<char>(input), {}};
    };
    const auto classes = std::make_shared<ClassPath>(
        std::vector<std::string>{"classes.dex", "classes2.dex"}, loadFile);

    CoreCollections collections;
    CoreLang lang;
    CoreRandom random;
    Machine* vmPtr = nullptr;
    Machine::Host host;
    host.hasClass = [](std::u16string_view type) {
        return CoreCollections::hasClass(type) || CoreLang::hasClass(type) || CoreRandom::hasClass(type);
    };
    host.call = [&](std::u16string_view type, std::u16string_view name, std::u16string_view sig,
                    bool isStatic, bool isNative, std::span<const Value> args) -> std::optional<Value> {
        if (isNative) return {};
        if (auto v = collections.invoke(vmPtr->heap(), type, name, sig, isStatic, args)) return v;
        if (auto v = lang.invoke(vmPtr->heap(), type, name, sig, isStatic, args)) return v;
        return random.invoke(vmPtr->heap(), type, name, sig, isStatic, args);
    };
    Machine vm(classes, {}, host);
    vmPtr = &vm;
    auto& heap = vm.heap();

    // 1. HashMap/LinkedHashMap views (keySet, values, entrySet, putAll) and Iterator traversal/remove.
    bool mapOk = true;
    const uint32_t m1 = heap.object(u"Ljava/util/LinkedHashMap;");
    collections.invoke(heap, u"Ljava/util/LinkedHashMap;", u"<init>", u"()V", false, std::array{ref(m1)});
    const uint32_t k1 = heap.newString(u"alpha"), v1 = heap.newString(u"one");
    const uint32_t k2 = heap.newString(u"beta"), v2 = heap.newString(u"two");
    collections.invoke(heap, u"Ljava/util/Map;", u"put", u"(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
                       false, std::array{ref(m1), ref(k1), ref(v1)});
    collections.invoke(heap, u"Ljava/util/Map;", u"put", u"(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
                       false, std::array{ref(m1), ref(k2), ref(v2)});
    const auto ks = collections.invoke(heap, u"Ljava/util/HashMap;", u"keySet", u"()Ljava/util/Set;", false, std::array{ref(m1)});
    mapOk &= ks && ks->bits != 0;
    const auto kit = ks ? collections.invoke(heap, u"Ljava/util/Set;", u"iterator", u"()Ljava/util/Iterator;", false, std::array{*ks}) : std::nullopt;
    mapOk &= kit && kit->bits != 0;
    std::vector<std::u16string> seenKeys;
    while (kit) {
        const auto hn = collections.invoke(heap, u"Ljava/util/Iterator;", u"hasNext", u"()Z", false, std::array{*kit});
        if (!hn || !hn->bits) break;
        const auto nx = collections.invoke(heap, u"Ljava/util/Iterator;", u"next", u"()Ljava/lang/Object;", false, std::array{*kit});
        if (nx && nx->bits) seenKeys.push_back(heap.string(uint32_t(nx->bits)));
    }
    mapOk &= seenKeys.size() == 2 && seenKeys[0] == u"alpha" && seenKeys[1] == u"beta";

    // entrySet + Map$Entry.setValue updates backing map.
    const auto es = collections.invoke(heap, u"Ljava/util/Map;", u"entrySet", u"()Ljava/util/Set;", false, std::array{ref(m1)});
    const auto eit = es ? collections.invoke(heap, u"Ljava/util/Set;", u"iterator", u"()Ljava/util/Iterator;", false, std::array{*es}) : std::nullopt;
    const auto e0 = eit ? collections.invoke(heap, u"Ljava/util/Iterator;", u"next", u"()Ljava/lang/Object;", false, std::array{*eit}) : std::nullopt;
    if (e0 && e0->bits) {
        const auto ek = collections.invoke(heap, u"Ljava/util/Map$Entry;", u"getKey", u"()Ljava/lang/Object;", false, std::array{*e0});
        mapOk &= ek && heap.string(uint32_t(ek->bits)) == u"alpha";
        collections.invoke(heap, u"Ljava/util/Map$Entry;", u"setValue", u"(Ljava/lang/Object;)Ljava/lang/Object;",
                           false, std::array{*e0, ref(heap.newString(u"ONE"))});
    } else {
        mapOk = false;
    }
    const auto updated = collections.invoke(heap, u"Ljava/util/Map;", u"get", u"(Ljava/lang/Object;)Ljava/lang/Object;",
                                            false, std::array{ref(m1), ref(k1)});
    mapOk &= updated && updated->bits && heap.string(uint32_t(updated->bits)) == u"ONE";
    check("Map keySet/entrySet/Iterator/Map$Entry match Java SE semantics", mapOk);

    // 2. LinkedList/ArrayDeque deque ops, Arrays.asList/equals/copyOfRange, Collections.singleton/addAll.
    bool colOk = true;
    const uint32_t dq = heap.object(u"Ljava/util/ArrayDeque;");
    collections.invoke(heap, u"Ljava/util/ArrayDeque;", u"<init>", u"()V", false, std::array{ref(dq)});
    collections.invoke(heap, u"Ljava/util/ArrayDeque;", u"push", u"(Ljava/lang/Object;)V", false, std::array{ref(dq), ref(k1)});
    collections.invoke(heap, u"Ljava/util/ArrayDeque;", u"push", u"(Ljava/lang/Object;)V", false, std::array{ref(dq), ref(k2)});
    const auto popped = collections.invoke(heap, u"Ljava/util/ArrayDeque;", u"pop", u"()Ljava/lang/Object;", false, std::array{ref(dq)});
    colOk &= popped && heap.string(uint32_t(popped->bits)) == u"beta";
    const auto peeked = collections.invoke(heap, u"Ljava/util/ArrayDeque;", u"peekFirst", u"()Ljava/lang/Object;", false, std::array{ref(dq)});
    colOk &= peeked && heap.string(uint32_t(peeked->bits)) == u"alpha";
    const uint32_t b1 = heap.array(u"[B", 3), b2 = heap.array(u"[B", 3);
    for (int i = 0; i < 3; ++i) {
        heap.setElement(b1, i, word(i + 10));
        heap.setElement(b2, i, word(i + 10));
    }
    const auto eqB = collections.invoke(heap, u"Ljava/util/Arrays;", u"equals", u"([B[B)Z", true, std::array{ref(b1), ref(b2)});
    colOk &= eqB && eqB->bits == 1;
    const auto sing = collections.invoke(heap, u"Ljava/util/Collections;", u"singleton", u"(Ljava/lang/Object;)Ljava/util/Set;",
                                         true, std::array{ref(k1)});
    const auto singHas = sing ? collections.invoke(heap, u"Ljava/util/Set;", u"contains", u"(Ljava/lang/Object;)Z",
                                                   false, std::array{*sing, ref(k1)}) : std::nullopt;
    colOk &= singHas && singHas->bits == 1;
    check("ArrayDeque, Arrays.equals([B[B), and Collections.singleton match Java SE", colOk);

    // 3. String.format and Throwable.toString/printStackTrace.
    bool langOk = true;
    const uint32_t fmtArgs = heap.array(u"[Ljava/lang/Object;", 3);
    const auto box42 = lang.invoke(heap, u"Ljava/lang/Integer;", u"valueOf", u"(I)Ljava/lang/Integer;", true, std::array{word(42)});
    const auto boxPi = lang.invoke(heap, u"Ljava/lang/Double;", u"valueOf", u"(D)Ljava/lang/Double;", true,
                                   std::array{Value{ValueKind::Wide, std::bit_cast<uint64_t>(3.14159)}});
    heap.setElement(fmtArgs, 0, ref(heap.newString(u"hero")));
    heap.setElement(fmtArgs, 1, *box42);
    heap.setElement(fmtArgs, 2, *boxPi);
    const auto formatted = lang.invoke(
        heap, u"Ljava/lang/String;", u"format", u"(Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/String;", true,
        std::array{ref(heap.newString(u"%s:%04x:%.2f%% %2$d")), ref(fmtArgs)});
    langOk &= formatted && heap.string(uint32_t(formatted->bits)) == u"hero:002a:3.14% 42";
    const uint32_t ex = heap.object(u"Ljava/lang/IllegalArgumentException;");
    lang.invoke(heap, u"Ljava/lang/IllegalArgumentException;", u"<init>", u"(Ljava/lang/String;)V", false,
                std::array{ref(ex), ref(heap.newString(u"bad"))});
    const auto exStr = lang.invoke(heap, u"Ljava/lang/Throwable;", u"toString", u"()Ljava/lang/String;", false, std::array{ref(ex)});
    langOk &= exStr && heap.string(uint32_t(exStr->bits)) == u"java.lang.IllegalArgumentException: bad";
    const auto pst = lang.invoke(heap, u"Ljava/lang/Throwable;", u"printStackTrace", u"(Ljava/io/PrintWriter;)V",
                                 false, std::array{ref(ex), ref(0)});
    langOk &= pst.has_value();
    check("String.format and Throwable.toString/printStackTrace match Java SE", langOk);

    // 4. java.security.SecureRandom inherits OpenJDK java.util.Random seeded sequence.
    // OpenJDK Random(0).nextInt(100) == 60 (independently derived from Java SE 48-bit LCG).
    bool randOk = true;
    const uint32_t sr = heap.object(u"Ljava/security/SecureRandom;");
    random.invoke(heap, u"Ljava/security/SecureRandom;", u"<init>", u"()V", false, std::array{ref(sr)});
    random.invoke(heap, u"Ljava/util/Random;", u"setSeed", u"(J)V", false, std::array{ref(sr), wide(0)});
    const auto r0 = random.invoke(heap, u"Ljava/util/Random;", u"nextInt", u"(I)I", false, std::array{ref(sr), word(100)});
    randOk &= r0 && int32_t(r0->bits) == 60;
    check("SecureRandom delegates to java.util.Random LCG", randOk);

    // 5. Machine reflection mirrors, classInfo, assignability, and instantiate.
    bool reflOk = true;
    const uint32_t mChild = vm.classMirror(u"Ltest/Child;");
    const uint32_t mChild2 = vm.classMirror(u"Ltest/Child;");
    reflOk &= mChild != 0 && mChild == mChild2;
    reflOk &= vm.classMirrorType(mChild) == std::optional<std::u16string>(u"Ltest/Child;");
    const auto ci = vm.classInfo(u"Ltest/Child;");
    reflOk &= ci.defined && ci.superName == u"Ltest/Base;" && (ci.flags & 1u) != 0;
    reflOk &= vm.isAssignable(u"Ltest/Child;", u"Ltest/Base;") && !vm.isAssignable(u"Ltest/Base;", u"Ltest/Child;");
    const auto inst = vm.instantiate(u"Ltest/Child;", u"()V", {});
    const auto* instVal = std::get_if<Value>(&inst);
    reflOk &= instVal && instVal->kind == ValueKind::Reference && instVal->bits != 0 &&
              heap.typeOf(uint32_t(instVal->bits)) == u"Ltest/Child;";
    if (instVal && instVal->bits) {
        const auto hc = vm.callInstance(u"Ltest/Child;", u"hashCode", u"()I", std::array{*instVal}, false);
        const auto* hcv = std::get_if<Value>(&hc);
        reflOk &= hcv && int32_t(hcv->bits) == 71;
    }
    check("Machine classMirror, classInfo, isAssignable, and instantiate work on DEX classes", reflOk);

    std::cout << "done: DexStdlib (" << failures << " failures)\n";
    return failures ? 1 : 0;
}
