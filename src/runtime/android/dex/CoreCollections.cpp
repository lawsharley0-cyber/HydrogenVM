#include "runtime/android/dex/CoreCollections.hpp"
#include "runtime/android/dex/DexFile.hpp"

#include <algorithm>
#include <bit>

namespace Hydrogen::Runtime::Android::Dex {
namespace {

int32_t integer(Value value) {
    if (value.kind != ValueKind::Word) throw FormatError("collection argument is not an int");
    return std::bit_cast<int32_t>(uint32_t(value.bits));
}
uint32_t reference(Value value) {
    if (value.kind != ValueKind::Reference) throw FormatError("collection argument is not a reference");
    return uint32_t(value.bits);
}
Value word(uint32_t n) { return {ValueKind::Word, n}; }
Value ref(uint32_t id) { return {ValueKind::Reference, id}; }

void checkIndex(int32_t i, size_t size, bool allowEnd) {
    if (i < 0 || size_t(i) > size || (!allowEnd && size_t(i) == size))
        throw JavaException{u"Ljava/lang/IndexOutOfBoundsException;"};
}

bool equal(Heap& heap, uint32_t lhs, uint32_t rhs) {
    if (lhs == rhs) return true;
    return lhs && rhs && heap.typeOf(lhs) == u"Ljava/lang/String;" &&
        heap.typeOf(rhs) == u"Ljava/lang/String;" && heap.string(lhs) == heap.string(rhs);
}

std::optional<Value> arrays(Heap& heap, std::u16string_view name, std::u16string_view sig,
                            std::span<const Value> args) {
    const bool sort = name == u"sort" && sig == u"([I)V";
    const bool fill = name == u"fill" && sig == u"([II)V";
    const bool copy = name == u"copyOf" && sig == u"([II)[I";
    const bool equals = name == u"equals" && sig == u"([I[I)Z";
    if (!sort && !fill && !copy && !equals) return {};
    if (args.size() != (sort ? 1 : 2)) throw FormatError("invalid Arrays arguments");
    const auto a = reference(args.front());
    if (equals) {
        const auto b = reference(args[1]);
        if (a == b) return word(1);
        if (!a || !b) return word(0);
        const auto size = heap.length(a);
        if (size != heap.length(b)) return word(0);
        for (uint32_t i = 0; i < size; ++i)
            if (heap.element(a, int32_t(i)).bits != heap.element(b, int32_t(i)).bits) return word(0);
        return word(1);
    }
    const auto length = heap.length(a);
    if (copy) {
        const auto newLength = integer(args[1]);
        const auto b = heap.array(u"[I", newLength);
        for (uint32_t i = 0; i < std::min(length, uint32_t(newLength)); ++i)
            heap.setElement(b, int32_t(i), heap.element(a, int32_t(i)));
        return ref(b);
    }
    if (fill) {
        for (uint32_t i = 0; i < length; ++i) heap.setElement(a, int32_t(i), args[1]);
    } else {
        std::vector<int32_t> items;
        items.reserve(length);
        for (uint32_t i = 0; i < length; ++i) items.push_back(integer(heap.element(a, int32_t(i))));
        std::sort(items.begin(), items.end());
        for (uint32_t i = 0; i < length; ++i) heap.setElement(a, int32_t(i), word(uint32_t(items[i])));
    }
    return Value{ValueKind::Void};
}

} // namespace

bool CoreCollections::hasClass(std::u16string_view type) {
    return type == u"Ljava/util/Arrays;" || type == u"Ljava/util/ArrayList;";
}

std::optional<Value> CoreCollections::invoke(Heap& heap, std::u16string_view type,
                                              std::u16string_view name, std::u16string_view sig,
                                              bool isStatic, std::span<const Value> args) {
    if (isStatic)
        return type == u"Ljava/util/Arrays;" ? arrays(heap, name, sig, args) : std::nullopt;
    if (args.empty()) return {};
    const auto list = reference(args.front());
    if (!list || heap.typeOf(list) != u"Ljava/util/ArrayList;") return {};
    std::lock_guard lock(mutex_);
    if (name == u"<init>" && (sig == u"()V" || sig == u"(I)V")) {
        if (sig == u"(I)V" && (args.size() != 2 || integer(args[1]) < 0))
            throw JavaException{u"Ljava/lang/IllegalArgumentException;"};
        lists_.try_emplace(list);
        return Value{ValueKind::Void};
    }
    const auto found = lists_.find(list);
    if (found == lists_.end()) throw FormatError("ArrayList used before its constructor");
    auto& contents = found->second;
    if (name == u"size" && sig == u"()I") return word(uint32_t(contents.size()));
    if (name == u"isEmpty" && sig == u"()Z") return word(contents.empty());
    if (name == u"clear" && sig == u"()V") {
        contents.clear();
        return Value{ValueKind::Void};
    }
    if (name == u"add" && sig == u"(Ljava/lang/Object;)Z") {
        if (args.size() != 2) throw FormatError("missing ArrayList.add argument");
        contents.push_back(reference(args[1]));
        return word(1);
    }
    if ((name == u"contains" && sig == u"(Ljava/lang/Object;)Z") ||
        (name == u"indexOf" && sig == u"(Ljava/lang/Object;)I") ||
        (name == u"remove" && sig == u"(Ljava/lang/Object;)Z")) {
        if (args.size() != 2) throw FormatError("missing ArrayList search argument");
        const auto needle = reference(args[1]);
        const auto it = std::find_if(contents.begin(), contents.end(), [&](uint32_t element) { return equal(heap, element, needle); });
        const bool present = it != contents.end();
        if (name == u"indexOf") return word(present ? uint32_t(it - contents.begin()) : UINT32_MAX);
        if (present && name == u"remove") contents.erase(it);
        return word(present);
    }
    const bool insert = name == u"add" && sig == u"(ILjava/lang/Object;)V";
    const bool get = name == u"get" && sig == u"(I)Ljava/lang/Object;";
    const bool set = name == u"set" && sig == u"(ILjava/lang/Object;)Ljava/lang/Object;";
    const bool remove = name == u"remove" && sig == u"(I)Ljava/lang/Object;";
    if (!insert && !get && !set && !remove) return {};
    if (args.size() != (insert || set ? 3 : 2)) throw FormatError("invalid ArrayList indexed arguments");
    const auto index = integer(args[1]);
    checkIndex(index, contents.size(), insert);
    const auto it = contents.begin() + index;
    if (insert) {
        contents.insert(it, reference(args[2]));
        return Value{ValueKind::Void};
    }
    const auto prior = *it;
    if (set) *it = reference(args[2]);
    if (remove) contents.erase(it);
    return ref(prior);
}

} // namespace Hydrogen::Runtime::Android::Dex
