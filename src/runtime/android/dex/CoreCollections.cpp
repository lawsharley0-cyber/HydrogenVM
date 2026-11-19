#include "runtime/android/dex/CoreCollections.hpp"
#include "runtime/android/dex/DexFile.hpp"

#include <algorithm>
#include <bit>
#include <random>

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

bool isMapType(std::u16string_view t) {
    return t == u"Ljava/util/HashMap;" || t == u"Ljava/util/LinkedHashMap;" ||
           t == u"Ljava/util/TreeMap;" || t == u"Ljava/util/WeakHashMap;" ||
           t == u"Ljava/util/AbstractMap;" || t == u"Ljava/util/Map;";
}

bool isSetType(std::u16string_view t) {
    return t == u"Ljava/util/HashSet;" || t == u"Ljava/util/LinkedHashSet;" ||
           t == u"Ljava/util/Set;";
}

bool isListType(std::u16string_view t) {
    return t == u"Ljava/util/ArrayList;" || t == u"Ljava/util/LinkedList;" ||
           t == u"Ljava/util/ArrayDeque;" || t == u"Ljava/util/List;" ||
           t == u"Ljava/util/Collection;" || t == u"Ljava/util/Deque;" ||
           t == u"Ljava/util/Queue;" || t == u"Ljava/util/AbstractList;" ||
           t == u"Ljava/util/AbstractCollection;";
}

uint32_t copyToArray(Heap& heap, const std::vector<uint32_t>& items, uint32_t targetArr) {
    if (!targetArr) {
        const uint32_t out = heap.array(u"[Ljava/lang/Object;", int32_t(items.size()));
        for (size_t i = 0; i < items.size(); ++i) heap.setElement(out, int32_t(i), ref(items[i]));
        return out;
    }
    const auto arrType = heap.typeOf(targetArr);
    const uint32_t curLen = heap.length(targetArr);
    uint32_t out = targetArr;
    if (curLen < items.size()) out = heap.array(arrType, int32_t(items.size()));
    for (size_t i = 0; i < items.size(); ++i) heap.setElement(out, int32_t(i), ref(items[i]));
    if (curLen > items.size()) heap.setElement(out, int32_t(items.size()), ref(0));
    return out;
}

} // namespace

bool CoreCollections::hasClass(std::u16string_view type) {
    return type == u"Ljava/util/Arrays;" || type == u"Ljava/util/Collections;" ||
           type == u"Ljava/util/Iterator;" || type == u"Ljava/util/Map$Entry;" ||
           isMapType(type) || isSetType(type) || isListType(type);
}

std::vector<uint32_t> CoreCollections::elementsOf(uint32_t id) const {
    if (auto it = lists_.find(id); it != lists_.end()) return it->second;
    if (auto it = sets_.find(id); it != sets_.end()) return it->second;
    return {};
}

uint32_t CoreCollections::makeIterator(Heap& heap, std::vector<uint32_t> items,
                                       uint32_t ownerId, IteratorState::Kind kind) {
    const uint32_t iter = heap.object(u"Ljava/util/Iterator;");
    iterators_.insert_or_assign(iter, IteratorState{std::move(items), 0, ownerId, kind, false});
    return iter;
}

std::optional<Value> CoreCollections::arraysMethod(Heap& heap, std::u16string_view name,
                                                   std::u16string_view sig, std::span<const Value> args) {
    if (name == u"asList" && sig == u"([Ljava/lang/Object;)Ljava/util/List;" && args.size() == 1) {
        const uint32_t arr = reference(args[0]);
        if (!arr) throw JavaException{u"Ljava/lang/NullPointerException;"};
        const uint32_t len = heap.length(arr);
        std::vector<uint32_t> items;
        items.reserve(len);
        for (uint32_t i = 0; i < len; ++i) items.push_back(uint32_t(heap.element(arr, int32_t(i)).bits));
        const uint32_t listId = heap.object(u"Ljava/util/ArrayList;");
        std::lock_guard lock(mutex_);
        lists_.insert_or_assign(listId, std::move(items));
        return ref(listId);
    }
    if (name == u"equals" && (sig == u"([I[I)Z" || sig == u"([B[B)Z") && args.size() == 2) {
        const auto a = reference(args[0]), b = reference(args[1]);
        if (a == b) return word(1);
        if (!a || !b) return word(0);
        const auto size = heap.length(a);
        if (size != heap.length(b)) return word(0);
        for (uint32_t i = 0; i < size; ++i)
            if (heap.element(a, int32_t(i)).bits != heap.element(b, int32_t(i)).bits) return word(0);
        return word(1);
    }
    if (name == u"copyOf" && args.size() == 2 &&
        (sig == u"([II)[I" || sig == u"([ZI)[Z" || sig == u"([Ljava/lang/Object;I)[Ljava/lang/Object;")) {
        const auto a = reference(args[0]);
        const auto newLength = integer(args[1]);
        const auto length = heap.length(a);
        const auto arrType = heap.typeOf(a);
        const auto b = heap.array(arrType, newLength);
        for (uint32_t i = 0; i < std::min(length, uint32_t(newLength)); ++i)
            heap.setElement(b, int32_t(i), heap.element(a, int32_t(i)));
        return ref(b);
    }
    if (name == u"copyOfRange" && sig == u"([Ljava/lang/Object;II)[Ljava/lang/Object;" && args.size() == 3) {
        const auto a = reference(args[0]);
        const int32_t from = integer(args[1]), to = integer(args[2]);
        if (from > to) throw JavaException{u"Ljava/lang/IllegalArgumentException;"};
        const uint32_t length = heap.length(a);
        const int32_t newLength = to - from;
        const auto b = heap.array(heap.typeOf(a), newLength);
        for (int32_t i = 0; i < newLength && uint32_t(from + i) < length; ++i)
            heap.setElement(b, i, heap.element(a, from + i));
        return ref(b);
    }
    if (name == u"fill") {
        if ((sig == u"([II)V" || sig == u"([BB)V" || sig == u"([FF)V" || sig == u"([ZZ)V" ||
             sig == u"([Ljava/lang/Object;Ljava/lang/Object;)V") && args.size() == 2) {
            const auto a = reference(args[0]);
            const auto length = heap.length(a);
            for (uint32_t i = 0; i < length; ++i) heap.setElement(a, int32_t(i), args[1]);
            return Value{ValueKind::Void};
        }
        if ((sig == u"([IIII)V" || sig == u"([Ljava/lang/Object;IILjava/lang/Object;)V") && args.size() == 4) {
            const auto a = reference(args[0]);
            const int32_t from = integer(args[1]), to = integer(args[2]);
            const uint32_t length = heap.length(a);
            if (from < 0 || from > to || uint32_t(to) > length)
                throw JavaException{u"Ljava/lang/ArrayIndexOutOfBoundsException;"};
            for (int32_t i = from; i < to; ++i) heap.setElement(a, i, args[3]);
            return Value{ValueKind::Void};
        }
    }
    if (name == u"sort" && sig == u"([I)V" && args.size() == 1) {
        const auto a = reference(args[0]);
        const auto length = heap.length(a);
        std::vector<int32_t> items;
        items.reserve(length);
        for (uint32_t i = 0; i < length; ++i) items.push_back(integer(heap.element(a, int32_t(i))));
        std::sort(items.begin(), items.end());
        for (uint32_t i = 0; i < length; ++i) heap.setElement(a, int32_t(i), word(uint32_t(items[i])));
        return Value{ValueKind::Void};
    }
    return {};
}

std::optional<Value> CoreCollections::mapMethod(Heap& heap, std::u16string_view name,
                                                std::u16string_view sig, std::span<const Value> args) {
    const auto id = reference(args.front());
    std::lock_guard lock(mutex_);
    if (name == u"<init>") {
        if (sig == u"()V" || sig == u"(I)V" || sig == u"(IF)V" || sig == u"(IFZ)V") {
            if (sig != u"()V" && integer(args[1]) < 0)
                throw JavaException{u"Ljava/lang/IllegalArgumentException;"};
            maps_.try_emplace(id);
            return Value{ValueKind::Void};
        }
        if (sig == u"(Ljava/util/Map;)V") {
            auto& dst = maps_[id];
            dst.clear();
            const uint32_t srcId = reference(args[1]);
            if (auto it = maps_.find(srcId); it != maps_.end()) dst = it->second;
            return Value{ValueKind::Void};
        }
        return {};
    }
    const auto table = maps_.find(id);
    if (table == maps_.end()) {
        // Subclasses or uninitialized map references: lazily initialize if empty.
        maps_.try_emplace(id);
    }
    auto& entries = maps_[id];
    if (name == u"size" && sig == u"()I") return word(uint32_t(entries.size()));
    if (name == u"isEmpty" && sig == u"()Z") return word(entries.empty());
    if (name == u"clear" && sig == u"()V") {
        entries.clear();
        return Value{ValueKind::Void};
    }
    if (name == u"putAll" && sig == u"(Ljava/util/Map;)V" && args.size() == 2) {
        const uint32_t srcId = reference(args[1]);
        if (auto sit = maps_.find(srcId); sit != maps_.end()) {
            const auto copy = sit->second;
            for (const auto& kv : copy) {
                auto it = std::find_if(entries.begin(), entries.end(), [&](const auto& e) { return equal(heap, kv.first, e.first); });
                if (it == entries.end()) entries.emplace_back(kv.first, kv.second);
                else it->second = kv.second;
            }
        }
        return Value{ValueKind::Void};
    }
    if (name == u"keySet" && sig == u"()Ljava/util/Set;") {
        const uint32_t setId = heap.object(u"Ljava/util/HashSet;");
        std::vector<uint32_t> keys;
        keys.reserve(entries.size());
        for (const auto& [k, _] : entries) keys.push_back(k);
        sets_.insert_or_assign(setId, std::move(keys));
        return ref(setId);
    }
    if (name == u"values" && sig == u"()Ljava/util/Collection;") {
        const uint32_t listId = heap.object(u"Ljava/util/ArrayList;");
        std::vector<uint32_t> vals;
        vals.reserve(entries.size());
        for (const auto& [_, v] : entries) vals.push_back(v);
        lists_.insert_or_assign(listId, std::move(vals));
        return ref(listId);
    }
    if (name == u"entrySet" && sig == u"()Ljava/util/Set;") {
        const uint32_t setId = heap.object(u"Ljava/util/HashSet;");
        std::vector<uint32_t> es;
        es.reserve(entries.size());
        for (const auto& [k, v] : entries) {
            const uint32_t eid = heap.object(u"Ljava/util/Map$Entry;");
            entries_.insert_or_assign(eid, EntryState{id, k, v});
            es.push_back(eid);
        }
        sets_.insert_or_assign(setId, std::move(es));
        return ref(setId);
    }
    const bool put = name == u"put" && sig == u"(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;";
    const bool get = name == u"get" && sig == u"(Ljava/lang/Object;)Ljava/lang/Object;";
    const bool remove = name == u"remove" && sig == u"(Ljava/lang/Object;)Ljava/lang/Object;";
    const bool containsKey = name == u"containsKey" && sig == u"(Ljava/lang/Object;)Z";
    const bool containsValue = name == u"containsValue" && sig == u"(Ljava/lang/Object;)Z";
    if (!put && !get && !remove && !containsKey && !containsValue) return {};
    const auto key = reference(args[1]);
    const auto it = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
        return equal(heap, key, containsValue ? entry.second : entry.first);
    });
    if (containsKey || containsValue) return word(it != entries.end());
    const uint32_t previous = it == entries.end() ? 0 : it->second;
    if (put) {
        const uint32_t value = reference(args[2]);
        if (it == entries.end()) entries.emplace_back(key, value);
        else it->second = value;
    } else if (remove && it != entries.end()) {
        entries.erase(it);
    }
    return ref(previous);
}

std::optional<Value> CoreCollections::setMethod(Heap& heap, std::u16string_view name,
                                                std::u16string_view sig, std::span<const Value> args) {
    const auto id = reference(args.front());
    std::lock_guard lock(mutex_);
    if (name == u"<init>") {
        if (sig == u"()V" || sig == u"(I)V" || sig == u"(IF)V") {
            if (sig != u"()V" && integer(args[1]) < 0)
                throw JavaException{u"Ljava/lang/IllegalArgumentException;"};
            sets_.try_emplace(id);
            return Value{ValueKind::Void};
        }
        if (sig == u"(Ljava/util/Collection;)V") {
            auto& dst = sets_[id];
            dst.clear();
            for (uint32_t el : elementsOf(reference(args[1]))) {
                if (std::none_of(dst.begin(), dst.end(), [&](uint32_t e) { return equal(heap, el, e); }))
                    dst.push_back(el);
            }
            return Value{ValueKind::Void};
        }
        return {};
    }
    if (sets_.find(id) == sets_.end()) sets_.try_emplace(id);
    auto& entries = sets_[id];
    if (name == u"size" && sig == u"()I") return word(uint32_t(entries.size()));
    if (name == u"isEmpty" && sig == u"()Z") return word(entries.empty());
    if (name == u"clear" && sig == u"()V") {
        entries.clear();
        return Value{ValueKind::Void};
    }
    if (name == u"iterator" && sig == u"()Ljava/util/Iterator;") {
        return ref(makeIterator(heap, entries, id, IteratorState::Kind::Set));
    }
    if (name == u"toArray" && sig == u"()[Ljava/lang/Object;") {
        return ref(copyToArray(heap, entries, 0));
    }
    if (name == u"toArray" && sig == u"([Ljava/lang/Object;)[Ljava/lang/Object;") {
        return ref(copyToArray(heap, entries, reference(args[1])));
    }
    if (name == u"addAll" && sig == u"(Ljava/util/Collection;)Z") {
        bool changed = false;
        for (uint32_t el : elementsOf(reference(args[1]))) {
            if (std::none_of(entries.begin(), entries.end(), [&](uint32_t e) { return equal(heap, el, e); })) {
                entries.push_back(el);
                changed = true;
            }
        }
        return word(changed ? 1u : 0u);
    }
    if (name == u"removeAll" && sig == u"(Ljava/util/Collection;)Z") {
        const auto other = elementsOf(reference(args[1]));
        const size_t before = entries.size();
        entries.erase(std::remove_if(entries.begin(), entries.end(), [&](uint32_t e) {
            return std::any_of(other.begin(), other.end(), [&](uint32_t o) { return equal(heap, e, o); });
        }), entries.end());
        return word(entries.size() != before ? 1u : 0u);
    }
    if (sig != u"(Ljava/lang/Object;)Z" ||
        (name != u"contains" && name != u"add" && name != u"remove")) return {};
    const auto value = reference(args[1]);
    const auto it = std::find_if(entries.begin(), entries.end(),
                                 [&](uint32_t entry) { return equal(heap, value, entry); });
    const bool present = it != entries.end();
    if (name == u"add") {
        if (!present) entries.push_back(value);
        return word(!present);
    }
    if (name == u"remove" && present) entries.erase(it);
    return word(present);
}

std::optional<Value> CoreCollections::listMethod(Heap& heap, std::u16string_view receiverType,
                                                 std::u16string_view name, std::u16string_view sig,
                                                 std::span<const Value> args) {
    const auto list = reference(args.front());
    std::lock_guard lock(mutex_);
    if (name == u"<init>") {
        if (sig == u"()V" || sig == u"(I)V") {
            if (sig == u"(I)V" && (args.size() != 2 || integer(args[1]) < 0))
                throw JavaException{u"Ljava/lang/IllegalArgumentException;"};
            lists_.try_emplace(list);
            return Value{ValueKind::Void};
        }
        if (sig == u"(Ljava/util/Collection;)V") {
            lists_.insert_or_assign(list, elementsOf(reference(args[1])));
            return Value{ValueKind::Void};
        }
        return {};
    }
    if (lists_.find(list) == lists_.end()) lists_.try_emplace(list);
    auto& contents = lists_[list];
    if (name == u"size" && sig == u"()I") return word(uint32_t(contents.size()));
    if (name == u"isEmpty" && sig == u"()Z") return word(contents.empty());
    if (name == u"clear" && sig == u"()V") {
        contents.clear();
        return Value{ValueKind::Void};
    }
    if (name == u"iterator" && sig == u"()Ljava/util/Iterator;") {
        return ref(makeIterator(heap, contents, list, IteratorState::Kind::List));
    }
    if (name == u"toArray" && sig == u"()[Ljava/lang/Object;") {
        return ref(copyToArray(heap, contents, 0));
    }
    if (name == u"toArray" && sig == u"([Ljava/lang/Object;)[Ljava/lang/Object;") {
        return ref(copyToArray(heap, contents, reference(args[1])));
    }
    if (name == u"addAll" && sig == u"(Ljava/util/Collection;)Z") {
        const auto items = elementsOf(reference(args[1]));
        contents.insert(contents.end(), items.begin(), items.end());
        return word(!items.empty() ? 1u : 0u);
    }
    if (name == u"addAll" && sig == u"(ILjava/util/Collection;)Z") {
        const int32_t idx = integer(args[1]);
        checkIndex(idx, contents.size(), true);
        const auto items = elementsOf(reference(args[2]));
        contents.insert(contents.begin() + idx, items.begin(), items.end());
        return word(!items.empty() ? 1u : 0u);
    }
    if (name == u"removeAll" && sig == u"(Ljava/util/Collection;)Z") {
        const auto other = elementsOf(reference(args[1]));
        const size_t before = contents.size();
        contents.erase(std::remove_if(contents.begin(), contents.end(), [&](uint32_t e) {
            return std::any_of(other.begin(), other.end(), [&](uint32_t o) { return equal(heap, e, o); });
        }), contents.end());
        return word(contents.size() != before ? 1u : 0u);
    }
    if ((name == u"add" || name == u"offer" || name == u"offerLast") && sig == u"(Ljava/lang/Object;)Z") {
        if (args.size() != 2) throw FormatError("missing List.add argument");
        contents.push_back(reference(args[1]));
        return word(1);
    }
    if (name == u"addLast" && sig == u"(Ljava/lang/Object;)V") {
        contents.push_back(reference(args[1]));
        return Value{ValueKind::Void};
    }
    if ((name == u"addFirst" || name == u"push") && sig == u"(Ljava/lang/Object;)V") {
        contents.insert(contents.begin(), reference(args[1]));
        return Value{ValueKind::Void};
    }
    if (name == u"offerFirst" && sig == u"(Ljava/lang/Object;)Z") {
        contents.insert(contents.begin(), reference(args[1]));
        return word(1);
    }
    if ((name == u"peek" || name == u"peekFirst") && sig == u"()Ljava/lang/Object;") {
        return ref(contents.empty() ? 0u : contents.front());
    }
    if (name == u"peekLast" && sig == u"()Ljava/lang/Object;") {
        return ref(contents.empty() ? 0u : contents.back());
    }
    if ((name == u"poll" || name == u"pollFirst") && sig == u"()Ljava/lang/Object;") {
        if (contents.empty()) return ref(0);
        const uint32_t v = contents.front();
        contents.erase(contents.begin());
        return ref(v);
    }
    if (name == u"pollLast" && sig == u"()Ljava/lang/Object;") {
        if (contents.empty()) return ref(0);
        const uint32_t v = contents.back();
        contents.pop_back();
        return ref(v);
    }
    if (name == u"getFirst" && sig == u"()Ljava/lang/Object;") {
        if (contents.empty()) throw JavaException{u"Ljava/util/NoSuchElementException;"};
        return ref(contents.front());
    }
    if (name == u"getLast" && sig == u"()Ljava/lang/Object;") {
        if (contents.empty()) throw JavaException{u"Ljava/util/NoSuchElementException;"};
        return ref(contents.back());
    }
    if ((name == u"removeFirst" || name == u"pop") && sig == u"()Ljava/lang/Object;") {
        if (contents.empty()) throw JavaException{u"Ljava/util/NoSuchElementException;"};
        const uint32_t v = contents.front();
        contents.erase(contents.begin());
        return ref(v);
    }
    if (name == u"removeLast" && sig == u"()Ljava/lang/Object;") {
        if (contents.empty()) throw JavaException{u"Ljava/util/NoSuchElementException;"};
        const uint32_t v = contents.back();
        contents.pop_back();
        return ref(v);
    }
    if ((name == u"contains" && sig == u"(Ljava/lang/Object;)Z") ||
        (name == u"indexOf" && sig == u"(Ljava/lang/Object;)I") ||
        (name == u"lastIndexOf" && sig == u"(Ljava/lang/Object;)I") ||
        (name == u"remove" && sig == u"(Ljava/lang/Object;)Z")) {
        if (args.size() != 2) throw FormatError("missing List search argument");
        const auto needle = reference(args[1]);
        if (name == u"lastIndexOf") {
            for (size_t i = contents.size(); i-- > 0;)
                if (equal(heap, contents[i], needle)) return word(uint32_t(i));
            return word(UINT32_MAX);
        }
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
    if (!insert && !get && !set && !remove) {
        (void)receiverType;
        return {};
    }
    if (args.size() != (insert || set ? 3 : 2)) throw FormatError("invalid List indexed arguments");
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

std::optional<Value> CoreCollections::iteratorMethod(Heap& heap, std::u16string_view name,
                                                     std::u16string_view sig, std::span<const Value> args) {
    const uint32_t id = reference(args.front());
    std::lock_guard lock(mutex_);
    auto it = iterators_.find(id);
    if (it == iterators_.end()) return {};
    auto& st = it->second;
    if (name == u"hasNext" && sig == u"()Z") return word(st.pos < st.snapshot.size() ? 1u : 0u);
    if (name == u"next" && sig == u"()Ljava/lang/Object;") {
        if (st.pos >= st.snapshot.size()) throw JavaException{u"Ljava/util/NoSuchElementException;"};
        st.canRemove = true;
        return ref(st.snapshot[st.pos++]);
    }
    if (name == u"remove" && sig == u"()V") {
        if (!st.canRemove || st.pos == 0) throw JavaException{u"Ljava/lang/IllegalStateException;"};
        const uint32_t item = st.snapshot[st.pos - 1];
        st.canRemove = false;
        if (st.kind == IteratorState::Kind::List) {
            if (auto lit = lists_.find(st.ownerId); lit != lists_.end()) {
                auto& v = lit->second;
                auto pos = std::find_if(v.begin(), v.end(), [&](uint32_t e) { return equal(heap, e, item); });
                if (pos != v.end()) v.erase(pos);
            }
        } else if (st.kind == IteratorState::Kind::Set) {
            if (auto sit = sets_.find(st.ownerId); sit != sets_.end()) {
                auto& v = sit->second;
                auto pos = std::find_if(v.begin(), v.end(), [&](uint32_t e) { return equal(heap, e, item); });
                if (pos != v.end()) v.erase(pos);
            }
        }
        return Value{ValueKind::Void};
    }
    return {};
}

std::optional<Value> CoreCollections::entryMethod(std::u16string_view name, std::u16string_view sig,
                                                  std::span<const Value> args) {
    const uint32_t id = reference(args.front());
    std::lock_guard lock(mutex_);
    auto it = entries_.find(id);
    if (it == entries_.end()) return {};
    if (name == u"getKey" && sig == u"()Ljava/lang/Object;") return ref(it->second.key);
    if (name == u"getValue" && sig == u"()Ljava/lang/Object;") return ref(it->second.value);
    if (name == u"setValue" && sig == u"(Ljava/lang/Object;)Ljava/lang/Object;" && args.size() == 2) {
        const uint32_t prev = it->second.value;
        it->second.value = reference(args[1]);
        if (auto mit = maps_.find(it->second.mapId); mit != maps_.end()) {
            for (auto& [k, v] : mit->second)
                if (k == it->second.key) { v = it->second.value; break; }
        }
        return ref(prev);
    }
    return {};
}

std::optional<Value> CoreCollections::listAlgorithm(Heap& heap, std::u16string_view name,
                                                    std::u16string_view sig, std::span<const Value> args) {
    if (name == u"singleton" && sig == u"(Ljava/lang/Object;)Ljava/util/Set;" && args.size() == 1) {
        const uint32_t setId = heap.object(u"Ljava/util/HashSet;");
        std::lock_guard lock(mutex_);
        sets_[setId] = {reference(args[0])};
        return ref(setId);
    }
    if (name == u"emptyList" && sig == u"()Ljava/util/List;") {
        const uint32_t id = heap.object(u"Ljava/util/ArrayList;");
        std::lock_guard lock(mutex_);
        lists_.try_emplace(id);
        return ref(id);
    }
    if (name == u"emptySet" && sig == u"()Ljava/util/Set;") {
        const uint32_t id = heap.object(u"Ljava/util/HashSet;");
        std::lock_guard lock(mutex_);
        sets_.try_emplace(id);
        return ref(id);
    }
    if (name == u"emptyMap" && sig == u"()Ljava/util/Map;") {
        const uint32_t id = heap.object(u"Ljava/util/HashMap;");
        std::lock_guard lock(mutex_);
        maps_.try_emplace(id);
        return ref(id);
    }
    if (name == u"addAll" && sig == u"(Ljava/util/Collection;[Ljava/lang/Object;)Z" && args.size() == 2) {
        const uint32_t col = reference(args[0]);
        const uint32_t arr = reference(args[1]);
        if (!col || !arr) throw JavaException{u"Ljava/lang/NullPointerException;"};
        const uint32_t len = heap.length(arr);
        std::lock_guard lock(mutex_);
        bool changed = false;
        if (auto lit = lists_.find(col); lit != lists_.end()) {
            for (uint32_t i = 0; i < len; ++i) {
                lit->second.push_back(uint32_t(heap.element(arr, int32_t(i)).bits));
                changed = true;
            }
        } else if (auto sit = sets_.find(col); sit != sets_.end()) {
            for (uint32_t i = 0; i < len; ++i) {
                const uint32_t el = uint32_t(heap.element(arr, int32_t(i)).bits);
                if (std::none_of(sit->second.begin(), sit->second.end(), [&](uint32_t e) { return equal(heap, e, el); })) {
                    sit->second.push_back(el);
                    changed = true;
                }
            }
        }
        return word(changed ? 1u : 0u);
    }
    const bool reverse = name == u"reverse" && sig == u"(Ljava/util/List;)V";
    const bool swap = name == u"swap" && sig == u"(Ljava/util/List;II)V";
    const bool shuffle = name == u"shuffle" && (sig == u"(Ljava/util/List;)V" || sig == u"(Ljava/util/List;Ljava/util/Random;)V");
    if (!reverse && !swap && !shuffle) return {};
    const auto id = reference(args.front());
    std::lock_guard lock(mutex_);
    const auto list = lists_.find(id);
    if (list == lists_.end()) return {};
    auto& values = list->second;
    if (reverse) std::reverse(values.begin(), values.end());
    else if (shuffle) {
        std::mt19937 gen(std::random_device{}());
        std::shuffle(values.begin(), values.end(), gen);
    } else {
        const auto first = integer(args[1]), second = integer(args[2]);
        checkIndex(first, values.size(), false);
        checkIndex(second, values.size(), false);
        std::swap(values[first], values[second]);
    }
    return Value{ValueKind::Void};
}

std::optional<Value> CoreCollections::invoke(Heap& heap, std::u16string_view type,
                                             std::u16string_view name, std::u16string_view sig,
                                             bool isStatic, std::span<const Value> args) {
    if (isStatic && type == u"Ljava/util/Collections;") return listAlgorithm(heap, name, sig, args);
    if (isStatic)
        return type == u"Ljava/util/Arrays;" ? arraysMethod(heap, name, sig, args) : std::nullopt;
    if (args.empty()) return {};
    const auto id = reference(args.front());
    if (!id) return {};
    const auto receiverType = heap.typeOf(id);
    if (receiverType == u"Ljava/util/Iterator;" || type == u"Ljava/util/Iterator;") return iteratorMethod(heap, name, sig, args);
    if (receiverType == u"Ljava/util/Map$Entry;" || type == u"Ljava/util/Map$Entry;") return entryMethod(name, sig, args);
    if (isMapType(receiverType) || isMapType(type)) return mapMethod(heap, name, sig, args);
    if (isSetType(receiverType) || isSetType(type)) return setMethod(heap, name, sig, args);
    if (isListType(receiverType) || isListType(type)) return listMethod(heap, receiverType, name, sig, args);
    return {};
}

} // namespace Hydrogen::Runtime::Android::Dex
