// Host DEX verification of java.util.regex.Pattern/Matcher, String.split/replaceAll, and java.util.Locale.
#include "runtime/android/dex/CoreLang.hpp"
#include "runtime/android/dex/CoreRegex.hpp"
#include "runtime/android/dex/Machine.hpp"

#include <array>
#include <cstdint>
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
Value ref(uint32_t id) { return {ValueKind::Reference, id}; }

bool isWord(const Execution& result, int32_t expected) {
    const auto* v = std::get_if<Value>(&result);
    return v && v->kind == ValueKind::Word && std::bit_cast<int32_t>(uint32_t(v->bits)) == expected;
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
            std::ifstream input(fixtures / "regex.dex", std::ios::binary);
            if (!input) throw FormatError("missing regex.dex fixture");
            return std::vector<uint8_t>{std::istreambuf_iterator<char>(input), {}};
        });

    CoreLang lang;
    CoreRegex regex;
    Machine* vmPtr = nullptr;
    Machine::Host host;
    host.hasClass = [](std::u16string_view type) {
        return CoreLang::hasClass(type) || CoreRegex::hasClass(type);
    };
    host.call = [&](std::u16string_view type, std::u16string_view name, std::u16string_view sig,
                    bool isStatic, bool isNative, std::span<const Value> args) -> std::optional<Value> {
        if (isNative) return {};
        if (auto v = lang.invoke(vmPtr->heap(), type, name, sig, isStatic, args)) return v;
        return regex.invoke(vmPtr->heap(), lang, type, name, sig, isStatic, args);
    };
    Machine vm(classes, {}, host);
    vmPtr = &vm;
    auto& heap = vm.heap();
    const auto cls = u"Ltest/RegexChecks;";

    // Pattern flags: CASE_INSENSITIVE (2) and DOTALL (32), plus invalid pattern error.
    bool patOk = true;
    patOk &= isWord(vm.callStatic(cls, u"compileAndMatch", u"(Ljava/lang/String;ILjava/lang/String;)Z",
                                  std::array{ref(heap.newString(u"hero.*win")), word(34),
                                             ref(heap.newString(u"HERO\nWIN"))}), 1);
    patOk &= isWord(vm.callStatic(cls, u"compileAndMatch", u"(Ljava/lang/String;ILjava/lang/String;)Z",
                                  std::array{ref(heap.newString(u"hero.*win")), word(2),
                                             ref(heap.newString(u"HERO\nWIN"))}), 0);
    patOk &= isWord(vm.callStatic(cls, u"compileAndMatch", u"(Ljava/lang/String;ILjava/lang/String;)Z",
                                  std::array{ref(heap.newString(u"\\p{InHiragana}+")), word(0),
                                             ref(heap.newString(u"\u3042\u3044\u3046"))}), 1);
    patOk &= isWord(vm.callStatic(cls, u"compileAndMatch", u"(Ljava/lang/String;ILjava/lang/String;)Z",
                                  std::array{ref(heap.newString(u"(?<=hero)[0-9]+")), word(0),
                                             ref(heap.newString(u"42"))}), 0);
    patOk &= isException(vm.callStatic(cls, u"compileAndMatch", u"(Ljava/lang/String;ILjava/lang/String;)Z",
                                       std::array{ref(heap.newString(u"[unclosed")), word(0),
                                                  ref(heap.newString(u"abc"))}),
                         u"Ljava/util/regex/PatternSyntaxException;");
    check("Pattern.compile enforces CASE_INSENSITIVE, DOTALL, Unicode blocks, lookbehind, and PatternSyntaxException", patOk);

    // Matcher.find + capturing group extraction.
    bool groupOk = true;
    groupOk &= isString(heap, vm.callStatic(cls, u"findGroup", u"(Ljava/lang/String;Ljava/lang/String;I)Ljava/lang/String;",
                                            std::array{ref(heap.newString(u"([a-z]+)=([0-9]+)")),
                                                       ref(heap.newString(u"hp=42 mp=7")), word(2)}), u"42");
    check("Matcher.find and Matcher.group(index) extract capturing groups", groupOk);

    // String.split with limit semantics and String.replaceAll.
    bool splitOk = true;
    const auto splitRes = vm.callStatic(cls, u"splitString", u"(Ljava/lang/String;Ljava/lang/String;I)[Ljava/lang/String;",
                                        std::array{ref(heap.newString(u"a:b::")), ref(heap.newString(u":")), word(0)});
    if (const auto* v = std::get_if<Value>(&splitRes); v && v->bits) {
        const uint32_t arr = uint32_t(v->bits);
        splitOk &= heap.length(arr) == 2;
        if (heap.length(arr) == 2) {
            splitOk &= heap.string(uint32_t(heap.element(arr, 0).bits)) == u"a";
            splitOk &= heap.string(uint32_t(heap.element(arr, 1).bits)) == u"b";
        }
    } else {
        splitOk = false;
    }
    splitOk &= isString(heap, vm.callStatic(cls, u"replaceAll", u"(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
                                            std::array{ref(heap.newString(u"floor 12 room 3")),
                                                       ref(heap.newString(u"[0-9]+")), ref(heap.newString(u"#"))}),
                        u"floor # room #");
    check("String.split drops trailing empties for limit 0 and String.replaceAll replaces matches", splitOk);

    // Locale normalization and toString.
    bool localeOk = isString(heap, vm.callStatic(cls, u"makeLocale", u"(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
                                                 std::array{ref(heap.newString(u"EN")), ref(heap.newString(u"us"))}),
                             u"en_US");
    check("Locale normalizes language/country casing and formats toString", localeOk);

    std::cout << "done: " << failures << " failures\n";
    return failures ? 1 : 0;
}
