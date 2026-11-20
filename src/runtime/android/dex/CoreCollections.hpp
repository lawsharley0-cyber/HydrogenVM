// Core Java collections backed by DEX heap identities.
#pragma once
#include "runtime/android/dex/Heap.hpp"

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace Hydrogen::Runtime::Android::Dex {

class CoreLang;

class CoreCollections {
public:
    using CompareFn = std::function<int32_t(uint32_t cmp, uint32_t a, uint32_t b)>;
    static bool hasClass(std::u16string_view type);
    void setLang(CoreLang* lang) { lang_ = lang; }
    void setComparator(CompareFn fn) { compare_ = std::move(fn); }
    std::optional<Value> invoke(Heap& heap, std::u16string_view type,
                                std::u16string_view name, std::u16string_view signature,
                                bool isStatic, std::span<const Value> args);

private:
    bool equal(Heap& heap, uint32_t lhs, uint32_t rhs) const;
    using Entries = std::vector<std::pair<uint32_t, uint32_t>>;
    struct IteratorState {
        std::vector<uint32_t> snapshot;
        size_t pos = 0;
        uint32_t ownerId = 0;
        enum class Kind { List, Set, MapKeys, MapValues, MapEntries } kind = Kind::List;
        bool canRemove = false;
    };
    struct EntryState {
        uint32_t mapId = 0;
        uint32_t key = 0;
        uint32_t value = 0;
    };

    std::vector<uint32_t> elementsOf(uint32_t id) const;
    uint32_t makeIterator(Heap& heap, std::vector<uint32_t> items, uint32_t ownerId, IteratorState::Kind kind);
    std::optional<Value> mapMethod(Heap& heap, std::u16string_view name,
                                   std::u16string_view signature, std::span<const Value> args);
    std::optional<Value> setMethod(Heap& heap, std::u16string_view name,
                                   std::u16string_view signature, std::span<const Value> args);
    std::optional<Value> listMethod(Heap& heap, std::u16string_view receiverType,
                                    std::u16string_view name, std::u16string_view signature,
                                    std::span<const Value> args);
    std::optional<Value> iteratorMethod(Heap& heap, std::u16string_view name,
                                        std::u16string_view signature, std::span<const Value> args);
    std::optional<Value> entryMethod(std::u16string_view name, std::u16string_view signature,
                                     std::span<const Value> args);
    std::optional<Value> listAlgorithm(Heap& heap, std::u16string_view name,
                                       std::u16string_view signature, std::span<const Value> args);
    std::optional<Value> arraysMethod(Heap& heap, std::u16string_view name,
                                      std::u16string_view signature, std::span<const Value> args);

    std::mutex mutex_;
    std::map<uint32_t, Entries> maps_;
    std::map<uint32_t, std::vector<uint32_t>> lists_;
    std::map<uint32_t, std::vector<uint32_t>> sets_;
    std::map<uint32_t, IteratorState> iterators_;
    std::map<uint32_t, EntryState> entries_;
    CoreLang* lang_ = nullptr;
    CompareFn compare_;
};

} // namespace Hydrogen::Runtime::Android::Dex
