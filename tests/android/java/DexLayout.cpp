#include "runtime/android/dex/DexFile.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>

using namespace Hydrogen::Runtime::Android::Dex;

namespace {

std::vector<uint8_t> readBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open fixture");
    return {std::istreambuf_iterator<char>(in), {}};
}

uint32_t little32(const std::vector<uint8_t>& bytes, size_t pos) {
    uint32_t out = 0;
    for (unsigned n = 0; n < 4; ++n) out |= uint32_t(bytes.at(pos + n)) << (8 * n);
    return out;
}

int failures = 0;
void test(const char* name, bool pass) {
    std::cout << (pass ? "ok " : "FAIL ") << name << '\n';
    failures += !pass;
}

template<class F>
bool rejects(F fn) {
    try { fn(); }
    catch (const FormatError&) { return true; }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path directory = argv[1];
    const File file = File::read(directory / "classes.dex");
    const auto probeIndex = file.findClass(u"Ltest/Probe;");
    const auto interfaceIndex = file.findClass(u"Ltest/Marker;");
    if (!probeIndex || !interfaceIndex) return 1;
    const auto probe = file.classDef(*probeIndex);
    const auto marker = file.classDef(*interfaceIndex);
    test("class definitions and superclass", file.classCount() == 2 &&
         file.type(probe.super) == u"Ljava/lang/Object;" && file.string(probe.source) == u"Probe.smali" &&
         !file.findClass(u"Ltest/Absent;"));
    test("interface, abstract and virtual method metadata", probe.interfaces.size() == 1 &&
         file.type(probe.interfaces[0]) == u"Ltest/Marker;" && marker.virtualMethods.size() == 1 &&
         marker.virtualMethods[0].codeOffset == 0 && probe.virtualMethods.size() == 1);

    const auto utf16 = std::u16string({u'A', 0, 0xe9, 0xd83d, 0xde00});
    const auto& values = probe.staticValues;
    test("static encoded values and MUTF-8 NUL and surrogate pair", values.size() == 4 &&
         values[0].type == 4 && values[0].bits == uint64_t(-17) &&
         values[1].type == 0x17 && file.string(uint32_t(values[1].bits)) == utf16 &&
         values[2].type == 31 && values[2].bits == 1 && values[3].type == 6 &&
         values[3].bits == 0x123456789abULL);
    test("field table references", probe.instanceFields.size() == 1 &&
         file.string(file.field(probe.instanceFields[0].index).name) == u"count");

    auto direct = [&](std::u16string_view name) {
        return std::find_if(probe.directMethods.begin(), probe.directMethods.end(), [&](const auto& member) {
            return file.string(file.method(member.index).name) == name;
        });
    };
    const auto wide = direct(u"wide");
    const auto string = direct(u"string");
    if (wide == probe.directMethods.end() || string == probe.directMethods.end()) return 1;
    const auto code = file.code(wide->codeOffset);
    test("wide argument register count and hand-encoded code words",
         file.signature(file.method(wide->index).proto) == u"(JD)J" && code.incoming == 4 &&
         code.registers == 6 && code.instructions == std::vector<uint16_t>{0x16, 0xfff8, 0x9b, 0x0200, 0x10});
    const auto strCode = file.code(string->codeOffset);
    test("const-string instruction points at the UTF-16 value", strCode.instructions.size() == 3 &&
         strCode.instructions[0] == 0x1a && file.string(strCode.instructions[1]) == utf16);

    const auto guarded = file.code(probe.virtualMethods.at(0).codeOffset);
    test("typed catch followed by catch-all in code-unit coordinates", guarded.tries.size() == 1 &&
         guarded.tries[0].start == 0 && guarded.tries[0].count == 1 &&
         guarded.tries[0].catches.size() == 2 &&
         file.type(guarded.tries[0].catches[0].type) == u"Ljava/lang/ArithmeticException;" &&
         guarded.tries[0].catches[0].address == 2 && guarded.tries[0].catches[1].type == noIndex &&
         guarded.tries[0].catches[1].address == 5);

    std::vector<std::string> loaded;
    ClassPath dex({"classes2.dex", "notes.txt", "classes.dex", "classes01.dex"}, [&](const std::string& name) {
        loaded.push_back(name);
        return readBytes(directory / name);
    });
    const auto first = dex.find(u"Ltest/Probe;");
    test("primary lookup opens only classes.dex", first && loaded == std::vector<std::string>{"classes.dex"});
    std::optional<ClassLocation> second, again;
    std::thread a([&] { second = dex.find(u"Ltest/Second;"); });
    std::thread b([&] { again = dex.find(u"Ltest/Second;"); });
    a.join();
    b.join();
    test("multidex lookup loads a shared image once", second && again && second->file == again->file &&
         loaded == std::vector<std::string>{"classes.dex", "classes2.dex"} &&
         second->file->type(second->file->classDef(second->index).super) == u"Ltest/Probe;");

    const auto bytes = readBytes(directory / "classes.dex");
    auto damaged = bytes;
    damaged.pop_back();
    test("truncated DEX fails header extent check", rejects([&] { File bad(damaged); }));
    damaged = bytes;
    const size_t methodOff = probe.virtualMethods.at(0).codeOffset;
    std::fill_n(damaged.begin() + methodOff + 12, 4, uint8_t(255));
    const File lazy(damaged);
    test("malformed method fails only when its code is read",
         lazy.findClass(u"Ltest/Probe;").has_value() && rejects([&] { lazy.code(uint32_t(methodOff)); }));
    damaged = bytes;
    const auto stringTable = little32(bytes, 0x3c);
    const auto firstString = little32(bytes, stringTable);
    std::fill_n(damaged.begin() + firstString, 5, uint8_t(0xff));
    const File badString(damaged);
    test("overflowing ULEB128 rejected", rejects([&] { badString.string(0); }));
    std::cout << "done: " << failures << " failure(s)\n";
    return failures != 0;
}
