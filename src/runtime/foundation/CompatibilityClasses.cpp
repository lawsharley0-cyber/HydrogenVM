// Cocoa data-model classes used by Unity. Unsupported predicate syntax raises explicitly.
#include "Foundation.hpp"
#include "runtime/libc/Format.hpp"
#include <algorithm>
#include <cctype>
#include <regex>

namespace Hydrogen::Runtime::Foundation {
namespace {
uint32_t send(Ctx& x, id object, const char* selector, std::initializer_list<uint32_t> args = {}) { return uint32_t(rt(x).send(x.cpu, object, selector, args)); }
id value(Ctx& x, id object, const std::string& key) {
    if (key.empty() || key == "SELF") return object;
    size_t begin = 0;
    while (begin < key.size() && object) {
        const auto end = key.find('.', begin);
        object = send(x, object, "valueForKey:", {makeString(x, key.substr(begin, end - begin))});
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return object;
}
int compare(Ctx& x, id a, id b) {
    if (objectsEqual(x, a, b)) return 0;
    if (!a || !b) return a ? 1 : -1;
    if (isNumber(x, a) && isNumber(x, b)) return numberDouble(x, a) < numberDouble(x, b) ? -1 : 1;
    const int32_t result = int32_t(send(x, a, "compare:", {b}));
    return (result > 0) - (result < 0);
}
struct Predicate : ObjC::HostObject { std::string key, operation; id rhs = 0; bool constant = false, result = false; };
void predicate_free(Ctx& x, id self, SEL) { auto& d = rt(x).host<Predicate>(self); if (d.rhs) rt(x).release(x.cpu, d.rhs); d.rhs = 0; rt(x).disposeInstance(x.cpu, self); }
id predicate_value(Ctx& x, Class cls, SEL, bool value) { const id object = rt(x).createInstance(cls); auto& d = rt(x).host<Predicate>(object); d.constant = true; d.result = value; return autoreleased(x, object); }
template <class Next> id parse(Ctx& x, Class cls, id format, Next next) {
    const std::string text = utf8(x, format);
    if (text == "TRUEPREDICATE" || text == "FALSEPREDICATE") return predicate_value(x, cls, 0, text == "TRUEPREDICATE");
    static const std::regex expression(R"(^\s*(SELF|%K|[A-Za-z_][A-Za-z_0-9.]*)\s*(==|!=|>=|<=|>|<)\s*(%@|%[difu]|nil|NULL|YES|NO|true|false|[-+]?[0-9]+(?:\.[0-9]+)?|'[^']*'|"[^"]*")\s*$)");
    std::smatch match;
    if (!std::regex_match(text, match, expression)) throw ObjC::ObjCError("unsupported NSPredicate format: " + text);
    const id object = rt(x).createInstance(cls); auto& d = rt(x).host<Predicate>(object);
    d.key = match[1] == "%K" ? utf8(x, next('@')) : match[1].str(); d.operation = match[2];
    const std::string rhs = match[3];
    if (rhs[0] == '%') d.rhs = next(rhs[1]);
    else if (rhs[0] == '\'' || rhs[0] == '"') d.rhs = makeString(x, rhs.substr(1, rhs.size() - 2));
    else if (rhs == "YES" || rhs == "true" || rhs == "NO" || rhs == "false") d.rhs = makeBool(x, rhs == "YES" || rhs == "true");
    else if (rhs != "nil" && rhs != "NULL") d.rhs = makeNumberDouble(x, std::stod(rhs));
    if (d.rhs) rt(x).retain(x.cpu, d.rhs);
    return autoreleased(x, object);
}
id predicate_format(Ctx& x, Class cls, SEL, id format, VarArgs args) {
    return parse(x, cls, format, [&](char type) -> id {
        if (type == 'f') { const auto raw = args.args.dword(); double v; std::memcpy(&v, &raw, 8); return makeNumberDouble(x, v); }
        const uint32_t raw = args.args.word();
        return type == '@' ? raw : type == 'u' ? makeNumberInt(x, raw) : makeNumberInt(x, int32_t(raw));
    });
}
id predicate_array(Ctx& x, Class cls, SEL, id format, id arguments) {
    auto values = arrayItems(x, arguments); size_t i = 0;
    return parse(x, cls, format, [&](char) -> id { if (i >= values.size()) throw ObjC::ObjCError("not enough NSPredicate arguments"); return values[i++]; });
}
bool evaluate(Ctx& x, id self, SEL, id object) {
    auto& d = rt(x).host<Predicate>(self); if (d.constant) return d.result;
    const id lhs = value(x, object, d.key);
    if (d.operation == "==") return objectsEqual(x, lhs, d.rhs);
    if (d.operation == "!=") return !objectsEqual(x, lhs, d.rhs);
    const int order = compare(x, lhs, d.rhs);
    if (d.operation == "<") return order < 0;
    if (d.operation == ">") return order > 0;
    return d.operation == "<=" ? order <= 0 : order >= 0;
}
id filtered(Ctx& x, id self, SEL, id predicate) { std::vector<id> result; for (id item : arrayItems(x, self)) if (send(x, predicate, "evaluateWithObject:", {item})) result.push_back(item); return makeArray(x, result); }
void filter(Ctx& x, id self, SEL, id predicate) { send(x, self, "setArray:", {filtered(x, self, 0, predicate)}); }

// Weak entries use libobjc's real zeroing-weak slots, not dangling host addresses.
struct Cell { id strong = 0; uint32_t weak = 0; };
struct Table : ObjC::HostObject { uint32_t keys = 0, values = 0; bool map = false; std::vector<std::pair<Cell, Cell>> items; };
void validateOptions(uint32_t options) {
    const auto memory = options & 0xff, personality = options & 0xff00;
    if ((memory != 0 && memory != 1 && memory != 5) || (personality != 0 && personality != 0x100) || (options & ~0xffffu))
        throw ObjC::ObjCError("unsupported NSPointerFunctions options");
}
Cell hold(Ctx& x, id object, uint32_t options) {
    Cell c; const auto memory = options & 0xff;
    if (memory == 1 || memory == 5) { c.weak = x.proc.heap().allocZeroed(4); rt(x).storeWeak(c.weak, object); }
    else { c.strong = object; if (memory == 0 && object) rt(x).retain(x.cpu, object); }
    return c;
}
id get(Ctx& x, const Cell& c) { return c.weak ? rt(x).loadWeak(c.weak) : c.strong; }
void drop(Ctx& x, Cell& c, uint32_t options) {
    if (c.weak) { rt(x).clearWeakSlot(c.weak); x.proc.heap().free(c.weak); }
    else if (!(options & 0xff) && c.strong) rt(x).release(x.cpu, c.strong);
    c = {};
}
void prune(Ctx& x, Table& t) {
    for (auto i = t.items.begin(); i != t.items.end();) {
        if (!get(x, i->first) || (t.map && !get(x, i->second))) { drop(x, i->first, t.keys); drop(x, i->second, t.values); i = t.items.erase(i); }
        else ++i;
    }
}
void table_clear(Ctx& x, id self, SEL) { auto& t = rt(x).host<Table>(self); for (auto& [key, val] : t.items) { drop(x, key, t.keys); drop(x, val, t.values); } t.items.clear(); }
void table_free(Ctx& x, id self, SEL) { table_clear(x, self, 0); rt(x).disposeInstance(x.cpu, self); }
id hash_init(Ctx& x, id self, SEL, uint32_t options, uint32_t) { validateOptions(options); table_clear(x, self, 0); auto& t = rt(x).host<Table>(self); t.keys = options; t.map = false; return self; }
id map_init(Ctx& x, id self, SEL, uint32_t keys, uint32_t values, uint32_t) { validateOptions(keys); validateOptions(values); table_clear(x, self, 0); auto& t = rt(x).host<Table>(self); t.map = true; t.keys = keys; t.values = values; return self; }
id hash_factory(Ctx& x, Class cls, SEL, uint32_t options) { return autoreleased(x, hash_init(x, rt(x).createInstance(cls), 0, options, 0)); }
id hash_weak(Ctx& x, Class cls, SEL) { return hash_factory(x, cls, 0, 5); }
id map_factory(Ctx& x, Class cls, SEL, uint32_t keys, uint32_t vals) { return autoreleased(x, map_init(x, rt(x).createInstance(cls), 0, keys, vals, 0)); }
id map_ss(Ctx& x, Class cls, SEL) { return map_factory(x, cls, 0, 0, 0); }
id map_sw(Ctx& x, Class cls, SEL) { return map_factory(x, cls, 0, 0, 5); }
id map_ws(Ctx& x, Class cls, SEL) { return map_factory(x, cls, 0, 5, 0); }
id map_ww(Ctx& x, Class cls, SEL) { return map_factory(x, cls, 0, 5, 5); }
size_t find(Ctx& x, Table& t, id key) { prune(x, t); for (size_t i = 0; i < t.items.size(); ++i) { const id stored = get(x, t.items[i].first); if ((t.keys & 0xff00) ? stored == key : objectsEqual(x, stored, key)) return i; } return t.items.size(); }
void table_put(Ctx& x, id self, SEL, id object, id key) {
    if (!key || !object) throw ObjC::ObjCError("nil NSMapTable key or value");
    auto& t = rt(x).host<Table>(self); t.map = true; const auto i = find(x, t, key);
    Cell retained = hold(x, object, t.values);
    if (i == t.items.size()) t.items.emplace_back(hold(x, key, t.keys), retained);
    else { drop(x, t.items[i].second, t.values); t.items[i].second = retained; }
}
void hash_add(Ctx& x, id self, SEL, id object) { if (!object) throw ObjC::ObjCError("nil NSHashTable object"); auto& t = rt(x).host<Table>(self); if (find(x, t, object) == t.items.size()) t.items.emplace_back(hold(x, object, t.keys), Cell{}); }
id table_get(Ctx& x, id self, SEL, id key) { auto& t = rt(x).host<Table>(self); auto i = find(x, t, key); return i == t.items.size() ? 0 : get(x, t.map ? t.items[i].second : t.items[i].first); }
bool hash_contains(Ctx& x, id self, SEL, id object) { return table_get(x, self, 0, object) != 0; }
void table_remove(Ctx& x, id self, SEL, id key) { auto& t = rt(x).host<Table>(self); const auto i = find(x, t, key); if (i != t.items.size()) { drop(x, t.items[i].first, t.keys); drop(x, t.items[i].second, t.values); t.items.erase(t.items.begin() + i); } }
uint32_t table_count(Ctx& x, id self, SEL) { auto& t = rt(x).host<Table>(self); prune(x, t); return uint32_t(t.items.size()); }
id table_array(Ctx& x, id self, SEL) { auto& t = rt(x).host<Table>(self); prune(x, t); std::vector<id> result; for (auto& item : t.items) result.push_back(get(x, t.map ? item.second : item.first)); return makeArray(x, result); }
id table_enumerator(Ctx& x, id self, SEL) { return send(x, table_array(x, self, 0), "objectEnumerator"); }
id table_keys(Ctx& x, id self, SEL) { auto& t = rt(x).host<Table>(self); prune(x, t); std::vector<id> keys; for (auto& item : t.items) keys.push_back(get(x, item.first)); return send(x, makeArray(x, keys), "objectEnumerator"); }
id table_any(Ctx& x, id self, SEL) { auto a = arrayItems(x, table_array(x, self, 0)); return a.empty() ? 0 : a[0]; }
id table_dictionary(Ctx& x, id self, SEL) { auto& t = rt(x).host<Table>(self); prune(x, t); std::vector<std::pair<id,id>> result; for (auto& item : t.items) result.emplace_back(get(x, item.first), get(x, item.second)); return makeDict(x, result); }

// Attribute indices and ranges use UTF-16 code units, as NSString does.
struct Attributed : ObjC::HostObject { std::u16string text; std::vector<id> attributes; };
void attr_clear(Ctx& x, Attributed& a) { for (id d : a.attributes) if (d) rt(x).release(x.cpu, d); a.attributes.clear(); }
void attr_free(Ctx& x, id self, SEL) { attr_clear(x, rt(x).host<Attributed>(self)); rt(x).disposeInstance(x.cpu, self); }
void rangeCheck(const Attributed& a, uint32_t loc, uint32_t len) { if (loc > a.text.size() || len > a.text.size() - loc) throw ObjC::ObjCError("NSAttributedString range out of bounds"); }
id attr_init(Ctx& x, id self, SEL, id string, id attributes) {
    if (!string) throw ObjC::ObjCError("nil attributed string");
    auto text = stringValue(x, string); id snapshot = makeDict(x, attributes ? dictEntries(x, attributes) : std::vector<std::pair<id,id>>{});
    auto& a = rt(x).host<Attributed>(self); attr_clear(x, a); a.text = std::move(text); a.attributes.assign(a.text.size(), snapshot);
    for (id d : a.attributes) rt(x).retain(x.cpu, d);
    return self;
}
id attr_init_string(Ctx& x, id self, SEL, id string) { return attr_init(x, self, 0, string, 0); }
id attr_init_attr(Ctx& x, id self, SEL, id other) {
    auto text = stringValue(x, send(x, other, "string")); std::vector<id> attrs;
    for (uint32_t i = 0; i < text.size(); ++i) { id d = send(x, other, "attributesAtIndex:effectiveRange:", {i, 0}); attrs.push_back(makeDict(x, dictEntries(x, d), true)); }
    auto& a = rt(x).host<Attributed>(self); attr_clear(x, a); a.text = std::move(text); a.attributes = std::move(attrs); return self;
}
id attr_string(Ctx& x, id self, SEL) { return makeString(x, rt(x).host<Attributed>(self).text); }
uint32_t attr_length(Ctx& x, id self, SEL) { return uint32_t(rt(x).host<Attributed>(self).text.size()); }
id attr_attributes(Ctx& x, id self, SEL, uint32_t index, GuestPtr out) {
    auto& a = rt(x).host<Attributed>(self); if (index >= a.text.size()) throw ObjC::ObjCError("NSAttributedString index out of bounds");
    uint32_t begin = index, end = index + 1;
    while (begin && objectsEqual(x, a.attributes[begin - 1], a.attributes[index])) --begin;
    while (end < a.text.size() && objectsEqual(x, a.attributes[end], a.attributes[index])) ++end;
    if (out) writeRange(x, out, {begin, end - begin});
    return a.attributes[index];
}
id attr_attribute(Ctx& x, id self, SEL, id name, uint32_t index, GuestPtr out) {
    auto& a = rt(x).host<Attributed>(self); if (index >= a.text.size()) throw ObjC::ObjCError("NSAttributedString index out of bounds");
    id result = dictGet(x, a.attributes[index], name); uint32_t begin = index, end = index + 1;
    while (begin && objectsEqual(x, dictGet(x, a.attributes[begin - 1], name), result)) --begin;
    while (end < a.text.size() && objectsEqual(x, dictGet(x, a.attributes[end], name), result)) ++end;
    if (out) writeRange(x, out, {begin, end - begin});
    return result;
}
void attr_set_attrs(Ctx& x, id self, SEL, id attrs, uint32_t loc, uint32_t len) {
    auto& a = rt(x).host<Attributed>(self); rangeCheck(a, loc, len); id snapshot = makeDict(x, attrs ? dictEntries(x, attrs) : std::vector<std::pair<id,id>>{});
    for (uint32_t i = loc; i < loc + len; ++i) { rt(x).retain(x.cpu, snapshot); rt(x).release(x.cpu, a.attributes[i]); a.attributes[i] = snapshot; }
}
void attr_add_attrs(Ctx& x, id self, SEL, id attrs, uint32_t loc, uint32_t len) {
    auto& a = rt(x).host<Attributed>(self); rangeCheck(a, loc, len);
    for (uint32_t i = loc; i < loc + len; ++i) { id d = makeDict(x, dictEntries(x, a.attributes[i]), false, true); for (auto [k,v] : dictEntries(x, attrs)) send(x, d, "setObject:forKey:", {v,k}); attr_set_attrs(x, self, 0, d, i, 1); }
}
void attr_add(Ctx& x, id self, SEL, id name, id val, uint32_t loc, uint32_t len) { if (!name || !val) throw ObjC::ObjCError("nil attributed string attribute"); attr_add_attrs(x, self, 0, makeDict(x, {{name,val}}), loc, len); }
void attr_remove(Ctx& x, id self, SEL, id name, uint32_t loc, uint32_t len) {
    auto& a = rt(x).host<Attributed>(self); rangeCheck(a, loc, len);
    for (uint32_t i = loc; i < loc + len; ++i) { auto entries = dictEntries(x, a.attributes[i]); std::erase_if(entries, [&](const auto& e) { return objectsEqual(x,e.first,name); }); attr_set_attrs(x,self,0,makeDict(x,entries),i,1); }
}
void attr_replace(Ctx& x, id self, SEL, uint32_t loc, uint32_t len, id string) {
    auto& a = rt(x).host<Attributed>(self); rangeCheck(a, loc, len); auto text = stringValue(x, string);
    id inherited = a.attributes.empty() ? makeDict(x,{}) : a.attributes[loc < a.text.size() ? loc : loc - 1];
    for (size_t i = 0; i < text.size(); ++i) rt(x).retain(x.cpu, inherited);
    for (uint32_t i = loc; i < loc + len; ++i) rt(x).release(x.cpu, a.attributes[i]);
    a.attributes.erase(a.attributes.begin()+loc,a.attributes.begin()+loc+len); a.attributes.insert(a.attributes.begin()+loc,text.size(),inherited); a.text.replace(loc,len,text);
}
void attr_replace_attr(Ctx& x, id self, SEL, uint32_t loc, uint32_t len, id other) {
    // Snapshot first: self replacement and overlapping ranges must work.
    id snapshot = rt(x).createInstance(rt(x).getClass("NSAttributedString")); attr_init_attr(x,snapshot,0,other);
    auto& b = rt(x).host<Attributed>(snapshot); attr_replace(x,self,0,loc,len,makeString(x,b.text));
    for (uint32_t i = 0; i < b.text.size(); ++i) attr_set_attrs(x,self,0,b.attributes[i],loc+i,1);
    rt(x).release(x.cpu,snapshot);
}
void attr_append(Ctx& x,id self,SEL,id other) { attr_replace_attr(x,self,0,attr_length(x,self,0),0,other); }
void attr_insert(Ctx& x,id self,SEL,id other,uint32_t index) { attr_replace_attr(x,self,0,index,0,other); }
void attr_delete(Ctx& x,id self,SEL,uint32_t loc,uint32_t len) { attr_replace(x,self,0,loc,len,makeString(x,"")); }
void attr_set(Ctx& x,id self,SEL,id other) { attr_replace_attr(x,self,0,0,attr_length(x,self,0),other); }
id attr_substring(Ctx& x,id self,SEL,uint32_t loc,uint32_t len) {
    auto& a = rt(x).host<Attributed>(self); rangeCheck(a,loc,len); id result = rt(x).createInstance(rt(x).getClass("NSAttributedString"));
    attr_init(x,result,0,makeString(x,a.text.substr(loc,len)),0); for(uint32_t i=0;i<len;++i) attr_set_attrs(x,result,0,a.attributes[loc+i],i,1); return autoreleased(x,result);
}
id attr_copy(Ctx& x,id self,SEL,uint32_t) { id result=rt(x).createInstance(rt(x).getClass("NSAttributedString")); return attr_init_attr(x,result,0,self); }
id attr_mutable_copy(Ctx& x,id self,SEL,uint32_t) { id result=rt(x).createInstance(rt(x).getClass("NSMutableAttributedString")); return attr_init_attr(x,result,0,self); }
void attr_edit(Ctx&,id,SEL) {} // Edits are immediate; begin/endEditing only batch notifications.
struct StringView : ObjC::HostObject { id owner=0; };
id view_owner(Ctx& x,id self) { return rt(x).host<StringView>(self).owner; }
id attr_mutable_string(Ctx& x,id self,SEL) { id result=rt(x).createInstance(rt(x).getClass("_HVMAttributedStringView")); rt(x).host<StringView>(result).owner=rt(x).retain(x.cpu,self); return autoreleased(x,result); }
void view_free(Ctx& x,id self,SEL) { rt(x).release(x.cpu,view_owner(x,self)); rt(x).disposeInstance(x.cpu,self); }
uint32_t view_length(Ctx& x,id self,SEL) { return attr_length(x,view_owner(x,self),0); }
uint16_t view_character(Ctx& x,id self,SEL,uint32_t index) { auto& a=rt(x).host<Attributed>(view_owner(x,self)); if(index>=a.text.size()) throw ObjC::ObjCError("string index out of bounds"); return a.text[index]; }
void view_replace(Ctx& x,id self,SEL,uint32_t loc,uint32_t len,id string) { attr_replace(x,view_owner(x,self),0,loc,len,string); }
void view_set(Ctx& x,id self,SEL,id string) { id owner=view_owner(x,self); attr_replace(x,owner,0,0,attr_length(x,owner,0),string); }
void view_append(Ctx& x,id self,SEL,id string) { id owner=view_owner(x,self); attr_replace(x,owner,0,attr_length(x,owner,0),0,string); }
void view_insert(Ctx& x,id self,SEL,id string,uint32_t at) { attr_replace(x,view_owner(x,self),0,at,0,string); }
void view_delete(Ctx& x,id self,SEL,uint32_t loc,uint32_t len) { attr_delete(x,view_owner(x,self),0,loc,len); }
void view_append_format(Ctx& x,id self,SEL,id format,VarArgs args) { view_append(x,self,0,makeString(x,formatGuest(x.proc,utf8(x,format),args.args))); }
uint32_t view_replace_occurrences(Ctx&,id,SEL,id,id,uint32_t,uint32_t,uint32_t) { throw ObjC::ObjCError("mutable attributed-string view replaceOccurrences unsupported"); }

struct Paragraph : ObjC::HostObject { std::unordered_map<std::string,uint32_t> integers; std::unordered_map<std::string,float> floats; };
std::string property(Ctx& x,SEL selector) { std::string name=rt(x).selName(selector); if(name.starts_with("set") && name.back()==':') { name=name.substr(3,name.size()-4); name[0]=char(std::tolower(uint8_t(name[0]))); } return name; }
uint32_t paragraph_get(Ctx& x,id self,SEL selector) { auto key=property(x,selector); auto& d=rt(x).host<Paragraph>(self); if(auto i=d.integers.find(key);i!=d.integers.end()) return i->second; return key=="baseWritingDirection" ? uint32_t(-1) : key=="alignment" ? 4 : 0; }
void paragraph_set(Ctx& x,id self,SEL selector,uint32_t val) { rt(x).host<Paragraph>(self).integers[property(x,selector)]=val; }
float paragraph_float(Ctx& x,id self,SEL selector) { auto key=property(x,selector); auto& d=rt(x).host<Paragraph>(self); if(auto i=d.floats.find(key);i!=d.floats.end()) return i->second; return 0; }
void paragraph_set_float(Ctx& x,id self,SEL selector,float val) { rt(x).host<Paragraph>(self).floats[property(x,selector)]=val; }
id paragraph_copy_to(Ctx& x,id self,const char* cls) { id result=rt(x).createInstance(rt(x).getClass(cls)); auto& a=rt(x).host<Paragraph>(self); auto& b=rt(x).host<Paragraph>(result); b.integers=a.integers; b.floats=a.floats; return result; }
id paragraph_copy(Ctx& x,id self,SEL,uint32_t) { return paragraph_copy_to(x,self,"NSParagraphStyle"); }
id paragraph_mutable_copy(Ctx& x,id self,SEL,uint32_t) { return paragraph_copy_to(x,self,"NSMutableParagraphStyle"); }
id paragraph_default(Ctx& x,Class cls,SEL) { return autoreleased(x,rt(x).createInstance(cls)); }
void paragraph_style(Ctx& x,id self,SEL,id other) { auto& a=rt(x).host<Paragraph>(self); auto& b=rt(x).host<Paragraph>(other); a.integers=b.integers; a.floats=b.floats; }

struct Credential : ObjC::HostObject { id user=0,password=0; uint32_t persistence=0; };
id credential_init(Ctx& x,id self,SEL,id user,id password,uint32_t persistence) { auto u=user?makeString(x,stringValue(x,user),true):0; auto p=password?makeString(x,stringValue(x,password),true):0; auto& c=rt(x).host<Credential>(self); if(c.user)rt(x).release(x.cpu,c.user); if(c.password)rt(x).release(x.cpu,c.password); c.user=u;c.password=p;c.persistence=persistence;return self; }
id credential_make(Ctx& x,Class cls,SEL,id user,id password,uint32_t persistence) { return autoreleased(x,credential_init(x,rt(x).createInstance(cls),0,user,password,persistence)); }
void credential_free(Ctx& x,id self,SEL) { auto& c=rt(x).host<Credential>(self); if(c.user)rt(x).release(x.cpu,c.user);if(c.password)rt(x).release(x.cpu,c.password);rt(x).disposeInstance(x.cpu,self); }
id credential_user(Ctx& x,id self,SEL) { return rt(x).host<Credential>(self).user; }
id credential_password(Ctx& x,id self,SEL) { return rt(x).host<Credential>(self).password; }
uint32_t credential_persistence(Ctx& x,id self,SEL) { return rt(x).host<Credential>(self).persistence; }
bool credential_has_password(Ctx& x,id self,SEL) { return rt(x).host<Credential>(self).password!=0; }
id immutable_copy(Ctx& x,id self,SEL,uint32_t) { return rt(x).retain(x.cpu,self); }
bool proxy(Ctx&,id,SEL) { return true; }
id collation_shared(Ctx& x,Class cls,SEL) { return autoreleased(x,rt(x).createInstance(cls)); }
id collation_titles(Ctx& x,id,SEL) { std::vector<id> titles; for(char c='A';c<='Z';++c)titles.push_back(makeString(x,std::string(1,c)));titles.push_back(makeString(x,"#"));return makeArray(x,titles); }
uint32_t collation_index(Ctx&,id,SEL,uint32_t index) { if(index>26)throw ObjC::ObjCError("collation index out of bounds");return index; }
uint32_t collation_section(Ctx& x,id,SEL,id object,SEL selector) {auto text=utf8(x,uint32_t(rt(x).send(x.cpu,object,selector,{})));if(text.empty())return 26;const unsigned char first=uint8_t(text[0]);return first>='A'&&first<='Z'?first-'A':first>='a'&&first<='z'?first-'a':26;}
id collation_sort(Ctx& x,id,SEL,id array,SEL selector) {auto items=arrayItems(x,array);std::stable_sort(items.begin(),items.end(),[&](id a,id b){return compare(x,uint32_t(rt(x).send(x.cpu,a,selector,{})),uint32_t(rt(x).send(x.cpu,b,selector,{})))<0;});return makeArray(x,items);}

} // namespace

void registerCompatibilityClasses(Process&, ObjC::Runtime& r) {
    r.defineClass("NSPredicate","NSObject")
      .cm<&predicate_value>("predicateWithValue:").cm<&predicate_format>("predicateWithFormat:")
      .cm<&predicate_array>("predicateWithFormat:argumentArray:").m<&evaluate>("evaluateWithObject:")
      .m<&immutable_copy>("copyWithZone:").m<&immutable_copy>("copy").m<&predicate_free>("dealloc");
    r.defineClass("NSArray","NSObject").m<&filtered>("filteredArrayUsingPredicate:");
    r.defineClass("NSMutableArray","NSArray").m<&filter>("filterUsingPredicate:");
    r.defineClass("NSHashTable","NSObject")
      .cm<&hash_factory>("hashTableWithOptions:").cm<&hash_weak>("weakObjectsHashTable")
      .m<&hash_init>("initWithOptions:capacity:").m<&hash_add>("addObject:").m<&table_get>("member:")
      .m<&hash_contains>("containsObject:").m<&table_remove>("removeObject:").m<&table_clear>("removeAllObjects")
      .m<&table_count>("count").m<&table_array>("allObjects").m<&table_enumerator>("objectEnumerator")
      .m<&table_any>("anyObject").m<&table_free>("dealloc");
    r.defineClass("NSMapTable","NSObject")
      .cm<&map_factory>("mapTableWithKeyOptions:valueOptions:").cm<&map_ss>("strongToStrongObjectsMapTable")
      .cm<&map_sw>("strongToWeakObjectsMapTable").cm<&map_ws>("weakToStrongObjectsMapTable").cm<&map_ww>("weakToWeakObjectsMapTable")
      .m<&map_init>("initWithKeyOptions:valueOptions:capacity:").m<&table_put>("setObject:forKey:")
      .m<&table_get>("objectForKey:").m<&table_remove>("removeObjectForKey:").m<&table_clear>("removeAllObjects")
      .m<&table_count>("count").m<&table_enumerator>("objectEnumerator").m<&table_keys>("keyEnumerator")
      .m<&table_dictionary>("dictionaryRepresentation").m<&table_free>("dealloc");
    r.defineClass("NSAttributedString","NSObject")
      .m<&attr_init>("initWithString:attributes:").m<&attr_init_string>("initWithString:").m<&attr_init_attr>("initWithAttributedString:")
      .m<&attr_string>("string").m<&attr_length>("length").m<&attr_attributes>("attributesAtIndex:effectiveRange:")
      .m<&attr_attribute>("attribute:atIndex:effectiveRange:").m<&attr_substring>("attributedSubstringFromRange:")
      .m<&attr_copy>("copyWithZone:").m<&attr_copy>("copy").m<&attr_mutable_copy>("mutableCopyWithZone:")
      .m<&attr_mutable_copy>("mutableCopy").m<&attr_free>("dealloc");
    r.defineClass("NSMutableAttributedString","NSAttributedString")
      .m<&attr_set_attrs>("setAttributes:range:").m<&attr_add_attrs>("addAttributes:range:").m<&attr_add>("addAttribute:value:range:")
      .m<&attr_remove>("removeAttribute:range:").m<&attr_replace>("replaceCharactersInRange:withString:")
      .m<&attr_replace_attr>("replaceCharactersInRange:withAttributedString:").m<&attr_append>("appendAttributedString:")
      .m<&attr_insert>("insertAttributedString:atIndex:").m<&attr_delete>("deleteCharactersInRange:")
      .m<&attr_set>("setAttributedString:").m<&attr_mutable_string>("mutableString").m<&attr_edit>("beginEditing").m<&attr_edit>("endEditing");
    r.defineClass("_HVMAttributedStringView","NSMutableString")
      .m<&view_length>("length").m<&view_character>("characterAtIndex:")
      .m<&view_replace>("replaceCharactersInRange:withString:").m<&view_set>("setString:").m<&view_append>("appendString:")
      .m<&view_append_format>("appendFormat:").m<&view_replace_occurrences>("replaceOccurrencesOfString:withString:options:range:")
      .m<&view_insert>("insertString:atIndex:").m<&view_delete>("deleteCharactersInRange:").m<&view_free>("dealloc");
    auto ps=r.defineClass("NSParagraphStyle","NSObject");
    ps.cm<&paragraph_default>("defaultParagraphStyle").m<&paragraph_copy>("copyWithZone:").m<&paragraph_copy>("copy")
      .m<&paragraph_mutable_copy>("mutableCopyWithZone:").m<&paragraph_mutable_copy>("mutableCopy");
    auto mp=r.defineClass("NSMutableParagraphStyle","NSParagraphStyle"); mp.m<&paragraph_style>("setParagraphStyle:");
    for (const char* key : {"alignment","lineBreakMode","baseWritingDirection"}) { ps.m<&paragraph_get>(key); std::string setter="set"+std::string(key);setter[3]=char(std::toupper(uint8_t(setter[3])));setter+=':';mp.m<&paragraph_set>(setter); }
    for (const char* key : {"lineSpacing","paragraphSpacing","firstLineHeadIndent","headIndent","tailIndent","minimumLineHeight","maximumLineHeight","lineHeightMultiple","paragraphSpacingBefore","hyphenationFactor"}) { ps.m<&paragraph_float>(key); std::string setter="set"+std::string(key);setter[3]=char(std::toupper(uint8_t(setter[3])));setter+=':';mp.m<&paragraph_set_float>(setter); }
    r.defineClass("NSURLCredential","NSObject")
      .cm<&credential_make>("credentialWithUser:password:persistence:").m<&credential_init>("initWithUser:password:persistence:")
      .m<&credential_user>("user").m<&credential_password>("password").m<&credential_persistence>("persistence")
      .m<&credential_has_password>("hasPassword").m<&immutable_copy>("copyWithZone:").m<&credential_free>("dealloc");
    r.defineClass("NSProxy","NSObject").m<&proxy>("isProxy");
    // The emulator's deterministic English A-Z/# index; locale tailoring is not implemented.
    r.defineClass("UILocalizedIndexedCollation","NSObject").cm<&collation_shared>("currentCollation")
      .m<&collation_titles>("sectionTitles").m<&collation_titles>("sectionIndexTitles")
      .m<&collation_index>("sectionForSectionIndexTitleAtIndex:")
      .m<&collation_section>("sectionForObject:collationStringSelector:").m<&collation_sort>("sortedArrayFromArray:collationStringSelector:");
}
} // namespace Hydrogen::Runtime::Foundation
