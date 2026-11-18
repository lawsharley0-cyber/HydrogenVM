#include "runtime/android/dex/Interpreter.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <type_traits>


namespace Hydrogen::Runtime::Android::Dex {
namespace {

template <class A, class B> A as(B value) { return std::bit_cast<A>(value); }

ValueKind kindFor(const File& file, uint32_t type) {
    switch (file.type(type).at(0)) {
        case u'V': return ValueKind::Void;
        case u'J': case u'D': return ValueKind::Wide;
        case u'L': case u'[': return ValueKind::Reference;
        default: return ValueKind::Word;
    }
}

class Frame {
public:
    explicit Frame(const Code& code) : words_(code.registers), references_(code.registers) {}

    uint32_t word(unsigned reg) const {
        check(reg, 1);
        if (references_[reg]) throw FormatError("scalar operation on a reference register");
        return words_[reg];
    }
    uint64_t wide(unsigned reg) const {
        check(reg, 2);
        return word(reg) | uint64_t(word(reg + 1)) << 32;
    }
    uint32_t reference(unsigned reg) const {
        check(reg, 1);
        if (!references_[reg] && words_[reg]) throw FormatError("object operation on a scalar register");
        return words_[reg];
    }
    uint32_t raw(unsigned reg) const {
        check(reg, 1);
        return words_[reg];
    }
    void put(unsigned reg, Value value) {
        const bool wide = value.kind == ValueKind::Wide;
        check(reg, wide ? 2 : 1);
        words_[reg] = uint32_t(value.bits);
        references_[reg] = value.kind == ValueKind::Reference;
        if (wide) {
            words_[reg + 1] = uint32_t(value.bits >> 32);
            references_[reg + 1] = false;
        }
    }
    void word(unsigned reg, uint32_t bits) { put(reg, {ValueKind::Word, bits}); }
    void wide(unsigned reg, uint64_t bits) { put(reg, {ValueKind::Wide, bits}); }

private:
    void check(size_t reg, size_t width) const {
        if (reg >= words_.size() || width > words_.size() - reg)
            throw FormatError("Dalvik register outside frame");
    }
    std::vector<uint32_t> words_;
    std::vector<bool> references_;
};

template <class U>
U integer(unsigned operation, U left, U right) {
    using S = std::make_signed_t<U>;
    const unsigned distance = unsigned(right) & (sizeof(U) * 8 - 1);
    switch (operation) {
        case 0: return left + right;
        case 1: return left - right;
        case 2: return left * right;
        case 3: case 4: {
            if (!right) throw JavaException{u"Ljava/lang/ArithmeticException;"};
            const S lhs = as<S>(left), rhs = as<S>(right);
            if (lhs == std::numeric_limits<S>::min() && rhs == -1) return operation == 3 ? left : 0;
            return U(operation == 3 ? lhs / rhs : lhs % rhs);
        }
        case 5: return left & right;
        case 6: return left | right;
        case 7: return left ^ right;
        case 8: return left << distance;
        case 9: return U(as<S>(left) >> distance);
        case 10: return left >> distance;
        default: throw FormatError("invalid integer operation");
    }
}

template <class Float>
Float floating(unsigned operation, Float l, Float r) {
    switch (operation) {
        case 0: return l + r;
        case 1: return l - r;
        case 2: return l * r;
        case 3: return l / r;
        case 4: return std::fmod(l, r);
        default: throw FormatError("invalid floating-point operation");
    }
}

template <class Int, class Float>
Int javaInt(Float value) {
    if (std::isnan(value)) return 0;
    const auto upper = std::ldexp(Float(1), std::numeric_limits<Int>::digits);
    if (value >= upper) return std::numeric_limits<Int>::max();
    if (value <= -upper) return std::numeric_limits<Int>::min();
    return Int(value);
}

template <class T> int32_t compare(T a, T b) { return int32_t(a > b) - int32_t(a < b); }

template <class F> int32_t cmpFloat(F a, F b, bool nanGreater) {
    return std::isnan(a) || std::isnan(b) ? (nanGreater ? 1 : -1) : compare(a, b);
}

void unary(Frame& f, unsigned op, unsigned a, unsigned b) {
    switch (op) {
        case 0x7b: f.word(a, 0u - f.word(b)); return;
        case 0x7c: f.word(a, ~f.word(b)); return;
        case 0x7d: f.wide(a, 0ull - f.wide(b)); return;
        case 0x7e: f.wide(a, ~f.wide(b)); return;
        case 0x7f: f.word(a, f.word(b) ^ 0x80000000u); return;
        case 0x80: f.wide(a, f.wide(b) ^ 0x8000000000000000ull); return;
        case 0x81: f.wide(a, uint64_t(as<int32_t>(f.word(b)))); return;
        case 0x82: f.word(a, as<uint32_t>(float(as<int32_t>(f.word(b))))); return;
        case 0x83: f.wide(a, as<uint64_t>(double(as<int32_t>(f.word(b))))); return;
        case 0x84: f.word(a, uint32_t(f.wide(b))); return;
        case 0x85: f.word(a, as<uint32_t>(float(as<int64_t>(f.wide(b))))); return;
        case 0x86: f.wide(a, as<uint64_t>(double(as<int64_t>(f.wide(b))))); return;
        case 0x87: f.word(a, uint32_t(javaInt<int32_t>(as<float>(f.word(b))))); return;
        case 0x88: f.wide(a, uint64_t(javaInt<int64_t>(as<float>(f.word(b))))); return;
        case 0x89: f.wide(a, as<uint64_t>(double(as<float>(f.word(b))))); return;
        case 0x8a: f.word(a, uint32_t(javaInt<int32_t>(as<double>(f.wide(b))))); return;
        case 0x8b: f.wide(a, uint64_t(javaInt<int64_t>(as<double>(f.wide(b))))); return;
        case 0x8c: f.word(a, as<uint32_t>(float(as<double>(f.wide(b))))); return;
        case 0x8d: f.word(a, uint32_t(as<int8_t>(uint8_t(f.word(b))))); return;
        case 0x8e: f.word(a, uint16_t(f.word(b))); return;
        case 0x8f: f.word(a, uint32_t(as<int16_t>(uint16_t(f.word(b))))); return;
        default: throw FormatError("invalid unary operation");
    }
}

void binary(Frame& f, unsigned op, unsigned a, unsigned b, unsigned c) {
    if (op <= 0x9a) {
        f.word(a, integer(op - 0x90, f.word(b), f.word(c)));
    } else if (op <= 0xa5) {
        const uint64_t right = op >= 0xa3 ? f.word(c) : f.wide(c);
        f.wide(a, integer(op - 0x9b, f.wide(b), right));
    } else if (op <= 0xaa) {
        f.word(a, as<uint32_t>(floating(op - 0xa6, as<float>(f.word(b)), as<float>(f.word(c)))));
    } else {
        f.wide(a, as<uint64_t>(floating(op - 0xab, as<double>(f.wide(b)), as<double>(f.wide(c)))));
    }
}

bool condition(unsigned op, uint32_t a, uint32_t b) {
    switch (op) {
        case 0: return a == b;
        case 1: return a != b;
        case 2: return as<int32_t>(a) < as<int32_t>(b);
        case 3: return as<int32_t>(a) >= as<int32_t>(b);
        case 4: return as<int32_t>(a) > as<int32_t>(b);
        case 5: return as<int32_t>(a) <= as<int32_t>(b);
        default: throw FormatError("invalid conditional");
    }
}

std::optional<uint32_t> exceptionTarget(Heap& heap, const File& file, const Code& code, uint32_t pc,
                                        const JavaException& exception) {
    for (const auto& block : code.tries) {
        if (pc < block.start || pc - block.start >= block.count) continue;
        for (const auto& handler : block.catches)
            if (handler.type == noIndex || heap.assignable(exception.type, file.type(handler.type))) return handler.address;
        break;
    }
    return {};
}

uint16_t unit(const Code& code, size_t pc) {
    if (pc >= code.instructions.size()) throw FormatError("instruction extends beyond code item");
    return code.instructions[pc];
}

uint32_t units32(const Code& code, size_t pc) {
    return unit(code, pc) | (uint32_t(unit(code, pc + 1)) << 16);
}

uint32_t target(const Code& code, uint32_t pc, int32_t delta) {
    const int64_t out = int64_t(pc) + delta;
    if (out < 0 || uint64_t(out) >= code.instructions.size()) throw FormatError("branch outside code item");
    return uint32_t(out);
}

uint32_t switchTarget(const Code& code, uint32_t pc, bool sparse, int32_t key) {
    const auto payload = target(code, pc, as<int32_t>(units32(code, size_t(pc) + 1)));
    if (payload % 2 || unit(code, payload) != (sparse ? 0x0200 : 0x0100))
        throw FormatError("invalid switch payload");
    const auto size = unit(code, size_t(payload) + 1);
    const size_t entries = size_t(payload) + 2;
    const size_t targets = entries + (sparse ? size_t(size) * 2 : 2);
    if (targets > code.instructions.size() || size > (code.instructions.size() - targets) / 2)
        throw FormatError("truncated switch payload");
    if (!sparse) {
        const int64_t index = int64_t(key) - as<int32_t>(units32(code, entries));
        if (index >= 0 && index < size)
            return target(code, pc, as<int32_t>(units32(code, targets + 2 * index)));
    } else {
        for (size_t i = 0; i < size; ++i) {
            if (key != as<int32_t>(units32(code, entries + 2 * i))) continue;
            return target(code, pc, as<int32_t>(units32(code, targets + 2 * i)));
        }
    }
    return pc + 3;
}

class MonitorScope {
public:
    explicit MonitorScope(Heap& heap) : heap_(heap) {}
    ~MonitorScope() {
        for (auto it = held_.rbegin(); it != held_.rend(); ++it) heap_.leave(*it);
    }
    void enter(uint32_t ref) {
        held_.push_back(ref);
        try { heap_.enter(ref); }
        catch (...) { held_.pop_back(); throw; }
    }
    void leave(uint32_t ref) {
        if (!ref) throw JavaException{u"Ljava/lang/NullPointerException;"};
        auto found = std::find(held_.rbegin(), held_.rend(), ref);
        if (found == held_.rend()) throw JavaException{u"Ljava/lang/IllegalMonitorStateException;"};
        heap_.leave(ref);
        held_.erase(std::prev(found.base()));
    }

private:
    Heap& heap_;
    std::vector<uint32_t> held_;
};

Value arrayValue(const Frame& frame, unsigned reg, char16_t component) {
    if (component == u'[' || component == u'L') return {ValueKind::Reference, frame.reference(reg)};
    if (component == u'J' || component == u'D') return {ValueKind::Wide, frame.wide(reg)};
    return {ValueKind::Word, frame.word(reg)};
}

char16_t component(Heap& heap, uint32_t ref) {
    const auto descriptor = heap.typeOf(ref);
    if (descriptor.size() < 2 || descriptor[0] != u'[') throw FormatError("expected array reference");
    return descriptor[1];
}

void arrayOp(Heap& heap, Frame& f, unsigned op, unsigned value, unsigned arr, unsigned subscript) {
    const uint32_t ref = f.reference(arr);
    const int32_t index = as<int32_t>(f.word(subscript));
    const auto item = component(heap, ref);
    const bool put = op >= 0x4b;
    constexpr std::u16string_view types[]{u"IF", u"JD", u"[L", u"Z", u"B", u"C", u"S"};
    if (types[op - (put ? 0x4b : 0x44)].find(item) == std::u16string_view::npos)
        throw FormatError("array opcode disagrees with component type");
    if (put) heap.setElement(ref, index, arrayValue(f, value, item));
    else f.put(value, heap.element(ref, index));
}

void fillData(Heap& heap, const Code& code, uint32_t pc, uint32_t ref) {
    const auto item = component(heap, ref);
    const auto payload = target(code, pc, as<int32_t>(units32(code, size_t(pc) + 1)));
    if (payload % 2 || unit(code, payload) != 0x0300) throw FormatError("invalid fill-array payload");
    const auto width = unit(code, size_t(payload) + 1);
    const auto expected = item == u'J' || item == u'D' ? 8u : item == u'I' || item == u'F' ? 4u :
                          item == u'S' || item == u'C' ? 2u : item == u'Z' || item == u'B' ? 1u : 0u;
    if (!expected || width != expected) throw FormatError("fill-array width disagrees with component");
    const auto count = units32(code, size_t(payload) + 2);
    const size_t start = size_t(payload) + 4;
    if (uint64_t(count) * width > (code.instructions.size() - start) * 2ull)
        throw FormatError("truncated fill-array payload");
    if (count > heap.length(ref)) throw JavaException{u"Ljava/lang/ArrayIndexOutOfBoundsException;"};
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t bits = 0;
        for (unsigned j = 0; j < width; ++j) {
            const size_t offset = size_t(i) * width + j;
            bits |= uint64_t((unit(code, start + offset / 2) >> (8 * (offset & 1))) & 255) << (8 * j);
        }
        heap.setElement(ref, int32_t(i), {width == 8 ? ValueKind::Wide : ValueKind::Word, bits});
    }
}

} // namespace

Execution executeLinked(Heap& heap, Linkage* linkage, const File& file, const EncodedMethod& method,
                        std::span<const Value> args, Budget& budget) {
    if (budget.depth >= 256) throw JavaException{u"Ljava/lang/StackOverflowError;"};
    struct Depth {
        Budget& budget;
        explicit Depth(Budget& b) : budget(b) { ++budget.depth; }
        ~Depth() { --budget.depth; }
    } depth(budget);
    auto services = [&]() -> Linkage& {
        if (!linkage) throw FormatError("instruction requires a linked DEX class path");
        return *linkage;
    };
    if (!method.codeOffset) throw FormatError("method has no code");
    const auto code = file.code(method.codeOffset);
    const auto proto = file.proto(file.method(method.index).proto);
    const auto returns = kindFor(file, proto.returnType);
    Frame f(code);
    const bool receiver = !(method.flags & 8);
    if (args.size() != proto.parameters.size() + receiver) throw FormatError("wrong Dalvik argument count");
    unsigned reg = code.registers - code.incoming;
    for (size_t i = 0; i < args.size(); ++i) {
        const auto expected = receiver && i == 0 ? ValueKind::Reference
                                                  : kindFor(file, proto.parameters[i - receiver]);
        if (args[i].kind != expected || expected == ValueKind::Void)
            throw FormatError("wrong Dalvik argument kind");
        f.put(reg, args[i]);
        reg += expected == ValueKind::Wide ? 2 : 1;
    }
    if (reg != code.registers) throw FormatError("code item argument words disagree with prototype");
    if (receiver && args[0].bits == 0) return JavaException{u"Ljava/lang/NullPointerException;"};

    MonitorScope monitors(heap);
    std::optional<JavaException> pending;
    std::optional<Value> result;
    uint32_t pc = 0;
    while (budget.remaining != 0) {
        --budget.remaining;
        const auto first = unit(code, pc);
        const unsigned op = first & 255, a8 = first >> 8, a4 = a8 & 15, b4 = a8 >> 4;
        const auto n = [&](size_t k) { return unit(code, size_t(pc) + k); };
        uint32_t next = pc + 1;
        if (op != 0x0d) pending.reset();
        if (op < 0x0a || op > 0x0c) result.reset();
        try {
            if (op >= 0x7b && op <= 0x8f) {
                unary(f, op, a4, b4);
            } else if (op >= 0x90 && op <= 0xaf) {
                const auto regs = n(1);
                binary(f, op, a8, regs & 255, regs >> 8);
                next = pc + 2;
            } else if (op >= 0xb0 && op <= 0xcf) {
                binary(f, op - 0x20, a4, a4, b4);
            } else if (op >= 0xd0 && op <= 0xe2) {
                const bool lit8 = op >= 0xd8;
                const auto second = n(1);
                const unsigned operation = op - (lit8 ? 0xd8 : 0xd0);
                const uint32_t left = f.word(lit8 ? second & 255 : b4);
                const uint32_t right = lit8 ? uint32_t(as<int8_t>(uint8_t(second >> 8)))
                                            : uint32_t(as<int16_t>(second));
                f.word(lit8 ? a8 : a4, operation == 1 ? right - left : integer(operation, left, right));
                next = pc + 2;
            } else {
                switch (op) {
                    case 0x00:
                        if (first) throw FormatError("entered Dalvik data payload");
                        break;
                    case 0x01: case 0x02: case 0x03:
                    case 0x04: case 0x05: case 0x06:
                    case 0x07: case 0x08: case 0x09: {
                        const unsigned encoding = (op - 1) % 3;
                        const unsigned dest = encoding == 0 ? a4 : encoding == 1 ? a8 : n(1);
                        const unsigned source = encoding == 0 ? b4 : n(encoding);
                        if (op <= 3) f.word(dest, f.word(source));
                        else if (op <= 6) f.wide(dest, f.wide(source));
                        else f.put(dest, {ValueKind::Reference, f.reference(source)});
                        next = pc + encoding + 1;
                        break;
                    }
                    case 0x0a: case 0x0b: case 0x0c: {
                        constexpr ValueKind kinds[]{ValueKind::Word, ValueKind::Wide, ValueKind::Reference};
                        if (!result || result->kind != kinds[op - 0x0a]) throw FormatError("invalid move-result");
                        f.put(a8, *result);
                        result.reset();
                        break;
                    }
                    case 0x0d:
                        if (!pending) throw FormatError("move-exception outside catch handler");
                        f.put(a8, {ValueKind::Reference, pending->reference});
                        pending.reset();
                        break;
                    case 0x0e: case 0x0f: case 0x10: case 0x11: {
                        constexpr ValueKind kinds[]{ValueKind::Void, ValueKind::Word, ValueKind::Wide, ValueKind::Reference};
                        const auto kind = kinds[op - 0x0e];
                        if (kind != returns) throw FormatError("return opcode disagrees with prototype");
                        switch (kind) {
                            case ValueKind::Void: return Value{kind};
                            case ValueKind::Word: return Value{kind, f.word(a8)};
                            case ValueKind::Wide: return Value{kind, f.wide(a8)};
                            case ValueKind::Reference: return Value{kind, f.reference(a8)};
                        }
                        break;
                    }
                    case 0x12:
                        f.word(a4, uint32_t(b4 < 8 ? int32_t(b4) : int32_t(b4) - 16));
                        break;
                    case 0x13: case 0x15:
                        f.word(a8, op == 0x13 ? uint32_t(as<int16_t>(n(1))) : uint32_t(n(1)) << 16);
                        next = pc + 2;
                        break;
                    case 0x14:
                        f.word(a8, units32(code, size_t(pc) + 1));
                        next = pc + 3;
                        break;
                    case 0x16: case 0x19:
                        f.wide(a8, op == 0x16 ? uint64_t(as<int16_t>(n(1))) : uint64_t(n(1)) << 48);
                        next = pc + 2;
                        break;
                    case 0x17:
                        f.wide(a8, uint64_t(as<int32_t>(units32(code, size_t(pc) + 1))));
                        next = pc + 3;
                        break;
                    case 0x18:
                        f.wide(a8, units32(code, size_t(pc) + 1) | uint64_t(units32(code, size_t(pc) + 3)) << 32);
                        next = pc + 5;
                        break;
                    case 0x1a: case 0x1b: {
                        const auto index = op == 0x1a ? uint32_t(n(1)) : units32(code, size_t(pc) + 1);
                        f.put(a8, {ValueKind::Reference, heap.intern(file.string(index))});
                        next = pc + (op == 0x1a ? 2 : 3);
                        break;
                    }
                    case 0x1c:
                        f.put(a8, {ValueKind::Reference, services().classObject(file.type(n(1)))});
                        next = pc + 2;
                        break;
                    case 0x1d: monitors.enter(f.reference(a8)); break;
                    case 0x1e: monitors.leave(f.reference(a8)); break;
                    case 0x1f: case 0x20: {
                        const auto ref = f.reference(op == 0x1f ? a8 : b4);
                        const bool matches = ref && heap.assignable(heap.typeOf(ref), file.type(n(1)));
                        if (op == 0x20) f.word(a4, matches);
                        if (op == 0x1f && ref && !matches) throw JavaException{u"Ljava/lang/ClassCastException;"};
                        next = pc + 2;
                        break;
                    }
                    case 0x21: f.word(a4, heap.length(f.reference(b4))); break;
                    case 0x22:
                        f.put(a8, {ValueKind::Reference, services().newInstance(file.type(n(1)), budget)});
                        next = pc + 2;
                        break;
                    case 0x23:
                        f.put(a4, {ValueKind::Reference, heap.array(file.type(n(1)), as<int32_t>(f.word(b4)))});
                        next = pc + 2;
                        break;
                    case 0x24: case 0x25: {
                        const bool range = op == 0x25;
                        const unsigned count = range ? a8 : b4;
                        const auto descriptor = file.type(n(1));
                        if (descriptor.size() < 2 || descriptor[0] != u'[' || descriptor[1] == u'J' ||
                            descriptor[1] == u'D' || (!range && count > 5))
                            throw FormatError("invalid filled-new-array operands");
                        const auto reference = heap.array(descriptor, int32_t(count));
                        const unsigned registers = n(2);
                        for (unsigned i = 0; i < count; ++i) {
                            const unsigned source = range ? registers + i : i == 4 ? a4 : (registers >> (4 * i)) & 15;
                            heap.setElement(reference, int32_t(i), arrayValue(f, source, descriptor[1]));
                        }
                        result = Value{ValueKind::Reference, reference};
                        next = pc + 3;
                        break;
                    }
                    case 0x26:
                        fillData(heap, code, pc, f.reference(a8));
                        next = pc + 3;
                        break;
                    case 0x27: {
                        const auto ref = f.reference(a8);
                        if (!ref) throw JavaException{u"Ljava/lang/NullPointerException;"};
                        const auto type = heap.typeOf(ref);
                        if (!heap.assignable(type, u"Ljava/lang/Throwable;")) throw FormatError("throw requires a Throwable");
                        throw JavaException{type, ref};
                    }
                    case 0x28: next = target(code, pc, as<int8_t>(uint8_t(a8))); break;
                    case 0x29: next = target(code, pc, as<int16_t>(n(1))); break;
                    case 0x2a: next = target(code, pc, as<int32_t>(units32(code, size_t(pc) + 1))); break;
                    case 0x2b: case 0x2c:
                        next = switchTarget(code, pc, op == 0x2c, as<int32_t>(f.word(a8)));
                        break;
                    case 0x2d: case 0x2e: case 0x2f: case 0x30: case 0x31: {
                        const auto second = n(1);
                        const unsigned b = second & 255, c = second >> 8;
                        const int32_t result = op <= 0x2e ? cmpFloat(as<float>(f.word(b)), as<float>(f.word(c)), op == 0x2e)
                            : op <= 0x30 ? cmpFloat(as<double>(f.wide(b)), as<double>(f.wide(c)), op == 0x30)
                            : compare(as<int64_t>(f.wide(b)), as<int64_t>(f.wide(c)));
                        f.word(a8, uint32_t(result));
                        next = pc + 2;
                        break;
                    }
                    case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37:
                    case 0x38: case 0x39: case 0x3a: case 0x3b: case 0x3c: case 0x3d: {
                        const bool zero = op >= 0x38;
                        const unsigned predicate = op - (zero ? 0x38 : 0x32);
                        const unsigned lhs = zero ? a8 : a4;
                        const uint32_t left = predicate < 2 ? f.raw(lhs) : f.word(lhs);
                        const uint32_t right = zero ? 0 : predicate < 2 ? f.raw(b4) : f.word(b4);
                        next = condition(predicate, left, right) ? target(code, pc, as<int16_t>(n(1))) : pc + 2;
                        break;
                    }
                    case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4a:
                    case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f: case 0x50: case 0x51:
                        arrayOp(heap, f, op, a8, n(1) & 255, n(1) >> 8);
                        next = pc + 2;
                        break;
                    case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58:
                    case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f:
                    case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66:
                    case 0x67: case 0x68: case 0x69: case 0x6a: case 0x6b: case 0x6c: case 0x6d: {
                        const bool stat = op >= 0x60;
                        const bool put = stat ? op >= 0x67 : op >= 0x59;
                        const unsigned reg = stat ? a8 : a4;
                        const auto index = n(1);
                        const auto type = file.type(file.field(index).type);
                        const std::optional<uint32_t> receiver = stat ? std::nullopt : std::optional(f.reference(b4));
                        std::optional<Value> write;
                        if (put) write = arrayValue(f, reg, type.at(0));
                        const auto value = services().field(file, index, receiver, write, budget);
                        if (!put) f.put(reg, value);
                        next = pc + 2;
                        break;
                    }
                    case 0x6e: case 0x6f: case 0x70: case 0x71: case 0x72:
                    case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: {
                        const bool range = op >= 0x74;
                        const auto kind = InvokeKind(op - (range ? 0x74 : 0x6e));
                        const unsigned count = range ? a8 : b4;
                        const unsigned registers = n(2);
                        if (!range && count > 5) throw FormatError("invalid invoke register count");
                        const auto argReg = [&](unsigned i) {
                            if (i >= count) throw FormatError("invoke argument outside register list");
                            return range ? registers + i : i == 4 ? a4 : (registers >> (4 * i)) & 15;
                        };
                        std::vector<Value> call;
                        unsigned i = 0;
                        if (kind != InvokeKind::Static)
                            call.push_back({ValueKind::Reference, f.reference(argReg(i++))});
                        for (const auto t : file.proto(file.method(n(1)).proto).parameters) {
                            const auto param = file.type(t);
                            const unsigned reg = argReg(i++);
                            if ((param == u"J" || param == u"D") && argReg(i++) != reg + 1)
                                throw FormatError("wide invoke register pair is discontiguous");
                            call.push_back(arrayValue(f, reg, param.at(0)));
                        }
                        if (i != count) throw FormatError("invoke argument word count disagrees with prototype");
                        result = services().invoke(file, method, n(1), kind, call, budget);
                        next = pc + 3;
                        break;
                    }
                    default: throw FormatError("unsupported Dalvik opcode " + std::to_string(op));
                }
            }
        } catch (JavaException& exception) {
            const auto handler = exceptionTarget(heap, file, code, op == 0x1e ? pc + 1 : pc, exception);
            if (!handler) return exception;
            if (!exception.reference) exception.reference = heap.object(exception.type);
            pending = exception;
            next = *handler;
        }
        pc = next;
    }
    throw FormatError("Dalvik instruction limit exceeded");
}

Execution execute(Heap& heap, const File& file, const EncodedMethod& method,
                  std::span<const Value> args, uint64_t instructionLimit) {
    Budget budget{instructionLimit};
    return executeLinked(heap, nullptr, file, method, args, budget);
}

} // namespace Hydrogen::Runtime::Android::Dex
