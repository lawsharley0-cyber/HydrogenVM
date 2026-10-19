// HydrogenVM — property lists: binary (bplist00), XML, and OpenStep/.strings
// parsing, and XML writing. Used for Info.plist, resource plists loaded by
// apps (NSDictionary dictionaryWithContentsOfFile: etc.), .strings files and
// NSUserDefaults persistence.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <utility>

namespace Hydrogen::Plist {

struct PlistError : std::runtime_error { using std::runtime_error::runtime_error; };

struct Value;
using ValuePtr = std::shared_ptr<Value>;

struct Value {
    enum class Type { Null, Bool, Integer, Real, Date, Data, String, Array, Dict, Uid };
    Type type = Type::Null;
    bool b = false;
    int64_t i = 0;
    double r = 0;                       // Real, or Date (seconds since 2001-01-01)
    std::string s;                      // String (UTF-8) or Data (raw bytes)
    std::vector<ValuePtr> array;
    std::vector<std::pair<std::string, ValuePtr>> dict;   // keeps file order

    static ValuePtr makeString(std::string v) { auto p = std::make_shared<Value>(); p->type = Type::String; p->s = std::move(v); return p; }
    static ValuePtr makeInt(int64_t v) { auto p = std::make_shared<Value>(); p->type = Type::Integer; p->i = v; return p; }
    static ValuePtr makeReal(double v) { auto p = std::make_shared<Value>(); p->type = Type::Real; p->r = v; return p; }
    static ValuePtr makeBool(bool v) { auto p = std::make_shared<Value>(); p->type = Type::Bool; p->b = v; return p; }
    static ValuePtr makeData(std::string v) { auto p = std::make_shared<Value>(); p->type = Type::Data; p->s = std::move(v); return p; }
    static ValuePtr makeDate(double v) { auto p = std::make_shared<Value>(); p->type = Type::Date; p->r = v; return p; }
    static ValuePtr makeArray() { auto p = std::make_shared<Value>(); p->type = Type::Array; return p; }
    static ValuePtr makeDict() { auto p = std::make_shared<Value>(); p->type = Type::Dict; return p; }

    const Value* get(const std::string& key) const {
        for (const auto& [k, v] : dict) if (k == key) return v.get();
        return nullptr;
    }
    std::string getString(const std::string& key, const std::string& def = {}) const {
        const Value* v = get(key);
        return v && v->type == Type::String ? v->s : def;
    }
    void set(const std::string& key, ValuePtr v) {
        for (auto& [k, old] : dict) if (k == key) { old = std::move(v); return; }
        dict.emplace_back(key, std::move(v));
    }
};

// Detects the format (binary, XML or OpenStep text, UTF-8 or UTF-16).
ValuePtr parse(const std::vector<uint8_t>& data);
ValuePtr parseFile(const std::string& path);

std::string toXml(const Value& v);
void writeXmlFile(const Value& v, const std::string& path);

// UTF-16 helpers shared with the string implementation.
std::string utf16ToUtf8(const char16_t* s, size_t n);
std::u16string utf8ToUtf16(const std::string& s);

} // namespace Hydrogen::Plist
