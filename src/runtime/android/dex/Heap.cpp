#include "runtime/android/dex/Heap.hpp"
#include "runtime/android/dex/DexFile.hpp"

#include <bit>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace Hydrogen::Runtime::Android::Dex {
namespace {

struct Object {
    explicit Object(std::u16string descriptor) : type(std::move(descriptor)) {}
    std::u16string type;
    std::u16string text;
    std::vector<Value> elements;
    std::shared_ptr<ArrayBacking> backing;
    std::mutex mutex;
    std::condition_variable available;
    std::thread::id owner;
    unsigned depth = 0;
};

void expectArray(const Object& object) {
    if (!object.type.starts_with(u'[')) throw FormatError("array instruction on non-array");
}

ValueKind elementKind(char16_t element) {
    if (element == u'[' || element == u'L') return ValueKind::Reference;
    if (element == u'D' || element == u'J') return ValueKind::Wide;
    if (std::u16string_view(u"ZBCSIF").find(element) == std::u16string_view::npos)
        throw FormatError("invalid array element type");
    return ValueKind::Word;
}

void bounds(const Object& object, int32_t index) {
    expectArray(object);
    if (index < 0 || uint32_t(index) >= object.elements.size())
        throw JavaException{u"Ljava/lang/ArrayIndexOutOfBoundsException;"};
}

} // namespace

struct Arena::Storage {
    explicit Storage(size_t budget) : available(budget) {}
    size_t available;
    void requireBytes(size_t bytes) const {
        if (bytes > available) throw JavaException{u"Ljava/lang/OutOfMemoryError;"};
    }
    mutable std::mutex mutex;
    std::vector<std::unique_ptr<Object>> objects;
    std::map<std::u16string, uint32_t> interned;

    uint32_t add(std::unique_ptr<Object> object, size_t charge) {
        if (objects.size() >= UINT32_MAX) throw JavaException{u"Ljava/lang/OutOfMemoryError;"};
        objects.push_back(std::move(object));
        available -= charge;
        return uint32_t(objects.size());
    }

    uint32_t addString(std::u16string text, size_t chargedCopies) {
        const auto charge = sizeof(Object) + text.size() * sizeof(char16_t) * chargedCopies;
        requireBytes(charge);
        auto object = std::make_unique<Object>(u"Ljava/lang/String;");
        object->text = std::move(text);
        return add(std::move(object), charge);
    }

    Object& get(uint32_t ref) const {
        if (!ref) throw JavaException{u"Ljava/lang/NullPointerException;"};
        std::lock_guard lock(mutex);
        if (ref > objects.size()) throw FormatError("invalid Dalvik heap reference");
        return *objects[ref - 1];
    }
};

Arena::Arena(size_t byteLimit, ClassAssignable classAssignable)
    : storage_(std::make_unique<Storage>(byteLimit)), classAssignable_(std::move(classAssignable)) {}
Arena::~Arena() = default;

uint32_t Arena::object(std::u16string type) {
    std::lock_guard lock(storage_->mutex);
    const auto charge = sizeof(Object) + type.size() * sizeof(char16_t);
    storage_->requireBytes(charge);
    return storage_->add(std::make_unique<Object>(std::move(type)), charge);
}

uint32_t Arena::newString(std::u16string text) {
    std::lock_guard lock(storage_->mutex);
    return storage_->addString(std::move(text), 1);
}

uint32_t Arena::intern(std::u16string text) {
    std::lock_guard lock(storage_->mutex);
    if (const auto it = storage_->interned.find(text); it != storage_->interned.end()) return it->second;
    const auto ref = storage_->addString(text, 2);
    storage_->interned.emplace(std::move(text), ref);
    return ref;
}

uint32_t Arena::array(std::u16string type, int32_t length) {
    if (!type.starts_with(u'[') || type.size() < 2) throw FormatError("invalid array descriptor");
    if (length < 0) throw JavaException{u"Ljava/lang/NegativeArraySizeException;"};
    const auto kind = elementKind(type[1]);
    const auto charge = uint64_t(sizeof(Object)) + type.size() * sizeof(char16_t) + uint64_t(length) * sizeof(Value);
    std::lock_guard lock(storage_->mutex);
    if (charge > storage_->available) throw JavaException{u"Ljava/lang/OutOfMemoryError;"};
    auto arr = std::make_unique<Object>(std::move(type));
    try { arr->elements.resize(size_t(length), {kind, 0}); }
    catch (const std::bad_alloc&) { throw JavaException{u"Ljava/lang/OutOfMemoryError;"}; }
    catch (const std::length_error&) { throw JavaException{u"Ljava/lang/OutOfMemoryError;"}; }
    return storage_->add(std::move(arr), size_t(charge));
}

std::u16string Arena::typeOf(uint32_t reference) const { return storage_->get(reference).type; }

std::u16string Arena::string(uint32_t reference) const {
    auto& object = storage_->get(reference);
    if (object.type != u"Ljava/lang/String;") throw FormatError("reference is not a string");
    std::lock_guard lock(object.mutex);
    return object.text;
}

void Arena::setString(uint32_t reference, std::u16string text) {
    auto& object = storage_->get(reference);
    if (object.type != u"Ljava/lang/String;") throw FormatError("reference is not a string");
    std::lock_guard lock(object.mutex);
    object.text = std::move(text);
}

uint32_t Arena::length(uint32_t reference) const {
    const auto& array = storage_->get(reference);
    expectArray(array);
    return uint32_t(array.elements.size());
}

Value Arena::element(uint32_t reference, int32_t index) const {
    auto& array = storage_->get(reference);
    bounds(array, index);
    std::shared_ptr<ArrayBacking> backing;
    {
        std::lock_guard lock(array.mutex);
        backing = array.backing;
        if (!backing) return array.elements[index];
    }
    return backing->read(uint32_t(index));
}

void Arena::setElement(uint32_t reference, int32_t index, Value value) {
    auto& array = storage_->get(reference);
    bounds(array, index);
    const auto element = array.type[1];
    if (value.kind != elementKind(element)) throw FormatError("array value kind disagrees with descriptor");
    switch (element) {
        case u'Z': value.bits &= 1; break;
        case u'B': value.bits = uint32_t(std::bit_cast<int8_t>(uint8_t(value.bits))); break;
        case u'C': value.bits = uint16_t(value.bits); break;
        case u'S': value.bits = uint32_t(std::bit_cast<int16_t>(uint16_t(value.bits))); break;
        case u'[': case u'L':
            if (value.bits && !assignable(typeOf(uint32_t(value.bits)), std::u16string_view(array.type).substr(1)))
                throw JavaException{u"Ljava/lang/ArrayStoreException;"};
            break;
    }
    std::shared_ptr<ArrayBacking> backing;
    {
        std::lock_guard lock(array.mutex);
        backing = array.backing;
        if (!backing) {
            array.elements[index] = value;
            return;
        }
    }
    backing->write(uint32_t(index), value);
}

void Arena::bindArray(uint32_t reference, std::shared_ptr<ArrayBacking> backing, bool copyElements) {
    auto& array = storage_->get(reference);
    expectArray(array);
    std::lock_guard lock(array.mutex);
    if (array.backing) throw FormatError("array already has shared backing");
    if (copyElements)
        for (uint32_t i = 0; i < array.elements.size(); ++i) backing->write(i, array.elements[i]);
    array.backing = std::move(backing);
}

void Arena::enter(uint32_t reference) {
    auto& object = storage_->get(reference);
    std::unique_lock lock(object.mutex);
    const auto self = std::this_thread::get_id();
    object.available.wait(lock, [&] { return object.depth == 0 || object.owner == self; });
    object.owner = self;
    ++object.depth;
}

void Arena::leave(uint32_t reference) {
    auto& object = storage_->get(reference);
    std::lock_guard lock(object.mutex);
    if (!object.depth || object.owner != std::this_thread::get_id())
        throw JavaException{u"Ljava/lang/IllegalMonitorStateException;"};
    if (--object.depth == 0) {
        object.owner = {};
        object.available.notify_one();
    }
}

bool Arena::assignable(std::u16string_view from, std::u16string_view to) const {
    if (from == to || to == u"Ljava/lang/Object;") return true;
    if (from.starts_with(u'[')) {
        if (to == u"Ljava/lang/Cloneable;" || to == u"Ljava/io/Serializable;") return true;
        if (to.starts_with(u'[') && from.size() > 1 && to.size() > 1 &&
            (from[1] == u'L' || from[1] == u'[') && (to[1] == u'L' || to[1] == u'['))
            return assignable(from.substr(1), to.substr(1));
        return false;
    }
    if (to == u"Ljava/lang/CharSequence;" &&
        (from == u"Ljava/lang/String;" || from == u"Ljava/lang/StringBuilder;" || from == u"Ljava/lang/StringBuffer;"))
        return true;
    // Bootstrap exceptions and java.lang core classes supplied by the VM, without class_def entries.
    static const std::map<std::u16string_view, std::u16string_view> parents{
        {u"Ljava/lang/AbstractMethodError;", u"Ljava/lang/IncompatibleClassChangeError;"},
        {u"Ljava/lang/ArithmeticException;", u"Ljava/lang/RuntimeException;"},
        {u"Ljava/lang/ArrayIndexOutOfBoundsException;", u"Ljava/lang/IndexOutOfBoundsException;"},
        {u"Ljava/lang/ArrayStoreException;", u"Ljava/lang/RuntimeException;"},
        {u"Ljava/lang/Byte;", u"Ljava/lang/Number;"},
        {u"Ljava/lang/ClassCastException;", u"Ljava/lang/RuntimeException;"},
        {u"Ljava/lang/Double;", u"Ljava/lang/Number;"},
        {u"Ljava/lang/Error;", u"Ljava/lang/Throwable;"},
        {u"Ljava/lang/Exception;", u"Ljava/lang/Throwable;"},
        {u"Ljava/lang/ExceptionInInitializerError;", u"Ljava/lang/LinkageError;"},
        {u"Ljava/lang/Float;", u"Ljava/lang/Number;"},
        {u"Ljava/lang/IllegalArgumentException;", u"Ljava/lang/RuntimeException;"},
        {u"Ljava/lang/IllegalMonitorStateException;", u"Ljava/lang/RuntimeException;"},
        {u"Ljava/lang/IllegalStateException;", u"Ljava/lang/RuntimeException;"},
        {u"Ljava/lang/IllegalThreadStateException;", u"Ljava/lang/IllegalArgumentException;"},
        {u"Ljava/lang/IncompatibleClassChangeError;", u"Ljava/lang/LinkageError;"},
        {u"Ljava/lang/IndexOutOfBoundsException;", u"Ljava/lang/RuntimeException;"},
        {u"Ljava/lang/InstantiationError;", u"Ljava/lang/IncompatibleClassChangeError;"},
        {u"Ljava/lang/Integer;", u"Ljava/lang/Number;"},
        {u"Ljava/lang/LinkageError;", u"Ljava/lang/Error;"},
        {u"Ljava/lang/Long;", u"Ljava/lang/Number;"},
        {u"Ljava/lang/NegativeArraySizeException;", u"Ljava/lang/RuntimeException;"},
        {u"Ljava/lang/NoClassDefFoundError;", u"Ljava/lang/LinkageError;"},
        {u"Ljava/lang/NoSuchFieldError;", u"Ljava/lang/IncompatibleClassChangeError;"},
        {u"Ljava/lang/NoSuchMethodError;", u"Ljava/lang/IncompatibleClassChangeError;"},
        {u"Ljava/lang/NullPointerException;", u"Ljava/lang/RuntimeException;"},
        {u"Ljava/lang/NumberFormatException;", u"Ljava/lang/IllegalArgumentException;"},
        {u"Ljava/lang/OutOfMemoryError;", u"Ljava/lang/VirtualMachineError;"},
        {u"Ljava/lang/RuntimeException;", u"Ljava/lang/Exception;"},
        {u"Ljava/lang/Short;", u"Ljava/lang/Number;"},
        {u"Ljava/lang/StackOverflowError;", u"Ljava/lang/VirtualMachineError;"},
        {u"Ljava/lang/StringIndexOutOfBoundsException;", u"Ljava/lang/IndexOutOfBoundsException;"},
        {u"Ljava/lang/UnsatisfiedLinkError;", u"Ljava/lang/LinkageError;"},
        {u"Ljava/lang/UnsupportedOperationException;", u"Ljava/lang/RuntimeException;"},
        {u"Ljava/lang/VirtualMachineError;", u"Ljava/lang/Error;"},
    };
    auto type = from;
    while (true) {
        const auto parent = parents.find(type);
        if (parent == parents.end()) break;
        type = parent->second;
        if (type == to) return true;
    }
    return classAssignable_ && classAssignable_(from, to);
}

} // namespace Hydrogen::Runtime::Android::Dex
