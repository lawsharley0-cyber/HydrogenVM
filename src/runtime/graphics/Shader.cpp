#include "Shader.hpp"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace Hydrogen::Runtime::Shader {
namespace {
struct Expression { Type type; std::vector<int> slots; };
struct Compiler {
    Program& p;
    std::vector<std::string> tokens;
    size_t at = 0;
    std::unordered_map<std::string, size_t> names;
    bool wroteOutput = false;
    int expressionDepth = 0;
    [[noreturn]] void fail(const std::string& why) const {
        throw std::runtime_error(why + " near token " + std::to_string(at) + " ('" + peek() + "')");
    }
    std::string peek() const { return at < tokens.size() ? tokens[at] : "<end>"; }
    bool take(const std::string& s) { if (peek() != s) return false; ++at; return true; }
    void need(const std::string& s) { if (!take(s)) fail("expected " + s); }
    std::string identifier() {
        auto s = peek();
        if (s.empty() || !(std::isalpha(uint8_t(s[0])) || s[0] == '_')) fail("expected identifier");
        ++at; return s;
    }
    int alloc(int n = 1) {
        if (n < 1 || p.slots + n > MaxSlots) fail("shader register limit exceeded");
        int out = p.slots; p.slots += n; return out;
    }
    int constant(float f) {
        int out = alloc(); p.code.push_back({Op::Constant, out, 0, 0, 0, f}); return out;
    }
    int binary(Op op, int a, int b) {
        int out = alloc(); p.code.push_back({op, out, a, b}); return out;
    }
    static bool precision(const std::string& s) { return s == "lowp" || s == "mediump" || s == "highp"; }
    static bool typeName(const std::string& s) {
        return s == "float" || s == "vec2" || s == "vec3" || s == "vec4" ||
               s == "mat2" || s == "mat3" || s == "mat4" || s == "sampler2D";
    }
    Type type() {
        if (precision(peek())) ++at;
        std::string t = peek();
        if (!typeName(t)) fail("unsupported type");
        ++at;
        if (t == "sampler2D") return {1, 0, true};
        if (t == "float") return {};
        int n = t.back() - '0';
        return t[0] == 'm' ? Type{n * n, n, false} : Type{n, 0, false};
    }
    Variable& declare(const std::string& name, Type t, Storage storage, int count, bool ro) {
        if (names.count(name) || (name.starts_with("gl_") && storage != Storage::Output)) fail("duplicate/reserved variable " + name);
        if (count < 1 || count > 32) fail("array size outside supported range");
        int slot = alloc(t.width * count);
        names[name] = p.variables.size();
        p.variables.push_back({name, t, storage, slot, count, ro});
        return p.variables.back();
    }
    Expression reference(const Variable& v) {
        int base = v.slot;
        if (take("[")) {
            auto n = peek(); char* end = nullptr;
            long idx = std::strtol(n.c_str(), &end, 10);
            if (!end || *end || idx < 0 || idx >= v.count) fail("array index must be an in-range literal");
            ++at; need("]"); base += int(idx) * v.type.width;
        } else if (v.count != 1) fail("array requires literal index");
        Expression e{v.type, {}};
        for (int i = 0; i < v.type.width; ++i) e.slots.push_back(base + i);
        return e;
    }
    Expression swizzle(Expression e) {
        while (take(".")) {
            if (e.type.matrix || e.type.sampler || e.type.width == 1) fail("swizzle requires vector");
            std::string mask = identifier();
            if (mask.empty() || mask.size() > 4) fail("invalid swizzle length");
            const std::string sets[] = {"xyzw", "rgba", "stpq"};
            int set = -1;
            Expression out{{int(mask.size()), 0, false}, {}};
            for (char ch : mask) {
                int component = -1;
                for (int s = 0; s < 3; ++s) {
                    auto k = sets[s].find(ch);
                    if (k != std::string::npos) {
                        if (set != -1 && set != s) fail("mixed swizzle alphabets");
                        set = s; component = int(k); break;
                    }
                }
                if (component < 0 || component >= e.type.width) fail("swizzle component out of range");
                out.slots.push_back(e.slots[size_t(component)]);
            }
            e = std::move(out);
        }
        return e;
    }
    Expression arithmetic(const std::string& op, Expression a, Expression b) {
        if (a.type.sampler || b.type.sampler) fail("arithmetic on sampler");
        if (op == "*" && a.type.matrix && b.type.width == a.type.matrix && !b.type.matrix) {
            int n = a.type.matrix;
            Expression out{{n, 0, false}, {}};
            for (int row = 0; row < n; ++row) {
                int sum = binary(Op::Multiply, a.slots[size_t(row)], b.slots[0]);
                for (int col = 1; col < n; ++col)
                    sum = binary(Op::Add, sum, binary(Op::Multiply, a.slots[size_t(col * n + row)], b.slots[size_t(col)]));
                out.slots.push_back(sum);
            }
            return out;
        }
        if (op == "*" && b.type.matrix && a.type.width == b.type.matrix && !a.type.matrix) {
            int n = b.type.matrix;
            Expression out{{n, 0, false}, {}};
            for (int col = 0; col < n; ++col) {
                int sum = binary(Op::Multiply, a.slots[0], b.slots[size_t(col * n)]);
                for (int row = 1; row < n; ++row)
                    sum = binary(Op::Add, sum, binary(Op::Multiply, a.slots[size_t(row)], b.slots[size_t(col * n + row)]));
                out.slots.push_back(sum);
            }
            return out;
        }
        if (a.type.matrix || b.type.matrix) fail("unsupported matrix arithmetic");
        if (a.type.width != b.type.width && a.type.width != 1 && b.type.width != 1) fail("incompatible operand widths");
        int n = std::max(a.type.width, b.type.width);
        Expression out{{n, 0, false}, {}};
        Op code = op == "+" ? Op::Add : op == "-" ? Op::Subtract : op == "*" ? Op::Multiply : Op::Divide;
        for (int i = 0; i < n; ++i)
            out.slots.push_back(binary(code, a.slots[size_t(a.type.width == 1 ? 0 : i)], b.slots[size_t(b.type.width == 1 ? 0 : i)]));
        return out;
    }
    Expression primary() {
        Expression e;
        if (take("-")) return arithmetic("*", {{}, {constant(-1)}}, expression(3));
        if (take("+")) return expression(3);
        if (take("(")) { e = expression(); need(")"); return swizzle(std::move(e)); }
        std::string tok = peek();
        if (!tok.empty() && (std::isdigit(uint8_t(tok[0])) || tok[0] == '.')) {
            char* end = nullptr;
            float v = std::strtof(tok.c_str(), &end);
            if (!end || *end) fail("invalid numeric literal");
            ++at; return {{}, {constant(v)}};
        }
        if (typeName(tok) || tok == "texture2D" || tok == "dot") {
            Type t{};
            bool ctor = typeName(tok);
            if (ctor) t = type(); else ++at;
            need("(");
            std::vector<Expression> args;
            if (!take(")")) {
                do { args.push_back(expression()); } while (take(","));
                need(")");
            }
            if (ctor) {
                if (t.sampler || args.empty()) fail("unsupported constructor");
                e.type = t;
                for (auto& a : args) {
                    if (a.type.sampler || a.type.matrix) fail("unsupported constructor argument");
                    e.slots.insert(e.slots.end(), a.slots.begin(), a.slots.end());
                }
                if (e.slots.size() == 1 && t.width > 1) {
                    int scalar = e.slots[0];
                    e.slots.assign(size_t(t.width), scalar);
                    if (t.matrix) for (int i = 0; i < t.width; ++i)
                        if (i / t.matrix != i % t.matrix) e.slots[size_t(i)] = constant(0);
                }
                if (int(e.slots.size()) != t.width) fail("constructor component count mismatch");
            } else if (tok == "dot") {
                if (args.size() != 2 || args[0].type != args[1].type || args[0].type.matrix || args[0].type.sampler) fail("invalid dot arguments");
                int sum = binary(Op::Multiply, args[0].slots[0], args[1].slots[0]);
                for (int i = 1; i < args[0].type.width; ++i)
                    sum = binary(Op::Add, sum, binary(Op::Multiply, args[0].slots[size_t(i)], args[1].slots[size_t(i)]));
                e = {{}, {sum}};
            } else {
                if (args.size() != 2 || !args[0].type.sampler || args[1].type != Type{2, 0, false}) fail("invalid texture2D arguments");
                int out = alloc(4);
                p.code.push_back({Op::Texture, out, args[0].slots[0], args[1].slots[0], args[1].slots[1]});
                e = {{4, 0, false}, {out, out + 1, out + 2, out + 3}};
            }
        } else {
            std::string name = identifier();
            if (!names.count(name)) fail("unknown variable/function " + name);
            e = reference(p.variables[names.at(name)]);
        }
        return swizzle(std::move(e));
    }
    Expression expression(int minPrec = 0) {
        if (++expressionDepth > 64) fail("expression nesting limit exceeded");
        auto lhs = primary();
        while (true) {
            auto op = peek();
            int prec = op == "+" || op == "-" ? 1 : op == "*" || op == "/" ? 2 : -1;
            if (prec < minPrec) break;
            ++at;
            lhs = arithmetic(op, std::move(lhs), expression(prec + 1));
        }
        --expressionDepth;
        return lhs;
    }
    void assign(Expression dst, Expression src) {
        if (dst.type != src.type) fail("assignment type mismatch");
        // Snapshot RHS before any stores, including overlapping swizzles.
        std::vector<int> temp;
        for (int s : src.slots) { int t = alloc(); p.code.push_back({Op::Move, t, s}); temp.push_back(t); }
        for (size_t i = 0; i < dst.slots.size(); ++i) p.code.push_back({Op::Move, dst.slots[i], temp[i]});
    }
    void declaration(Storage storage, bool local) {
        bool ro = take("const");
        Type t = type();
        do {
            std::string name = identifier();
            int count = 1;
            if (take("[")) {
                std::string n = peek(); char* end = nullptr;
                long k = std::strtol(n.c_str(), &end, 10);
                if (!end || *end || k < 1 || k > 32) fail("invalid array size");
                ++at; count = int(k); need("]");
                if (storage != Storage::Varying) fail("only varying arrays supported");
            }
            if ((t.matrix || t.sampler) && storage == Storage::Varying) fail("unsupported varying type");
            if (storage == Storage::Attribute && (t.matrix || t.sampler || !p.vertex)) fail("unsupported attribute type/stage");
            auto& v = declare(name, t, storage, count, ro || storage == Storage::Uniform || storage == Storage::Attribute || (storage == Storage::Varying && !p.vertex));
            int slot = v.slot;
            if (take("=")) {
                if (!local || count != 1) fail("unsupported global initializer");
                Expression dst{t, {}};
                for (int i = 0; i < t.width; ++i) dst.slots.push_back(slot + i);
                assign(dst, expression());
            } else if (ro) fail("const requires initializer");
        } while (take(","));
        need(";");
    }
    void tokenize(const std::string& source) {
        if (source.size() > 65536) fail("shader source too large");
        std::string clean;
        for (size_t i = 0; i < source.size();) {
            if (source.compare(i, 2, "//") == 0) { while (i < source.size() && source[i] != '\n') ++i; }
            else if (source.compare(i, 2, "/*") == 0) {
                auto end = source.find("*/", i + 2);
                if (end == std::string::npos) fail("unterminated comment");
                for (; i < end + 2; ++i) clean += source[i] == '\n' ? '\n' : ' ';
            } else clean += source[i++];
        }
        struct Conditional { bool parent, condition, hadElse; };
        std::vector<Conditional> stack;
        bool active = true;
        std::istringstream input(clean);
        clean.clear();
        for (std::string line; std::getline(input, line);) {
            auto first = line.find_first_not_of(" \t\r");
            if (first != std::string::npos && line[first] == '#') {
                std::istringstream directive(line.substr(first + 1));
                std::string op, arg, extra; directive >> op >> arg >> extra;
                if (op == "ifdef" || op == "ifndef") {
                    if (arg != "GL_ES" || !extra.empty()) fail("only GL_ES conditionals supported");
                    bool condition = op == "ifdef";
                    stack.push_back({active, condition, false}); active = active && condition;
                } else if (op == "else") {
                    if (stack.empty() || stack.back().hadElse || !arg.empty()) fail("unmatched #else");
                    auto& s = stack.back(); s.hadElse = true; active = s.parent && !s.condition;
                } else if (op == "endif") {
                    if (stack.empty() || !arg.empty()) fail("unmatched #endif");
                    active = stack.back().parent; stack.pop_back();
                } else if (active && !(op == "version" && arg == "100" && extra.empty())) fail("unsupported preprocessor directive");
            } else if (active) clean += line + '\n';
        }
        if (!stack.empty()) fail("unterminated conditional");
        for (size_t i = 0; i < clean.size();) {
            unsigned char c = uint8_t(clean[i]);
            if (std::isspace(c)) { ++i; continue; }
            size_t start = i++;
            if (std::isalpha(c) || c == '_') {
                while (i < clean.size() && (std::isalnum(uint8_t(clean[i])) || clean[i] == '_')) ++i;
            } else if (std::isdigit(c) || (c == '.' && i < clean.size() && std::isdigit(uint8_t(clean[i])))) {
                while (i < clean.size() && (std::isdigit(uint8_t(clean[i])) || clean[i] == '.')) ++i;
                if (i < clean.size() && (clean[i] == 'e' || clean[i] == 'E')) {
                    ++i;
                    if (i < clean.size() && (clean[i] == '+' || clean[i] == '-')) ++i;
                    while (i < clean.size() && std::isdigit(uint8_t(clean[i]))) ++i;
                }
            } else if (i < clean.size() && clean[i] == '=' && (c == '+' || c == '-' || c == '*' || c == '/')) ++i;
            tokens.push_back(clean.substr(start, i - start));
        }
    }
    void run(const std::string& source) {
        tokenize(source);
        auto& output = declare(p.vertex ? "gl_Position" : "gl_FragColor", {4, 0, false}, Storage::Output, 1, false);
        p.output = output.slot;
        while (peek() != "void" && at < tokens.size()) {
            if (take("precision")) { if (!precision(peek())) fail("expected precision"); ++at; type(); need(";"); continue; }
            Storage storage;
            if (take("attribute")) storage = Storage::Attribute;
            else if (take("uniform")) storage = Storage::Uniform;
            else if (take("varying")) storage = Storage::Varying;
            else fail("unsupported global declaration");
            declaration(storage, false);
        }
        need("void"); need("main"); need("("); take("void"); need(")"); need("{");
        while (!take("}")) {
            if (at >= tokens.size()) fail("unterminated main");
            if (peek() == "const" || precision(peek()) || typeName(peek())) { declaration(Storage::Local, true); continue; }
            if (take(";")) continue;
            auto name = identifier();
            if (!names.count(name)) fail("unknown assignment target " + name);
            auto v = p.variables[names.at(name)];
            if (v.readOnly) fail("assignment to read-only variable");
            auto dst = swizzle(reference(v));
            auto sorted = dst.slots; std::sort(sorted.begin(), sorted.end());
            if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) fail("duplicate destination swizzle");
            std::string op = peek();
            if (op != "=" && op != "+=" && op != "-=" && op != "*=" && op != "/=") fail("unsupported statement");
            ++at; auto rhs = expression();
            if (op != "=") rhs = arithmetic(op.substr(0, 1), dst, rhs);
            assign(dst, rhs); need(";");
            if (v.storage == Storage::Output) wroteOutput = true;
        }
        if (at != tokens.size()) fail("unsupported code after main");
        if (!wroteOutput) fail("shader never writes stage output");
    }
};
} // namespace

bool Program::compile(const std::string& source, bool isVertex) {
    *this = {}; vertex = isVertex;
    try { Compiler{*this, {}, 0, {}, false, 0}.run(source); return true; }
    catch (const std::exception& e) { error = e.what(); code.clear(); return false; }
}
const Variable* Program::find(const std::string& name) const {
    for (auto& v : variables) if (v.name == name) return &v;
    return nullptr;
}
void Program::initialize(Values& v) const { std::fill_n(v.begin(), slots, 0.0f); }
void Program::execute(Values& v, Sample sample, void* context) const {
    for (auto& i : code) {
        switch (i.op) {
            case Op::Constant: v[i.dst] = i.value; break;
            case Op::Move: v[i.dst] = v[i.a]; break;
            case Op::Add: v[i.dst] = v[i.a] + v[i.b]; break;
            case Op::Subtract: v[i.dst] = v[i.a] - v[i.b]; break;
            case Op::Multiply: v[i.dst] = v[i.a] * v[i.b]; break;
            case Op::Divide: v[i.dst] = v[i.a] / v[i.b]; break;
            case Op::Texture:
                if (sample) sample(context, std::isfinite(v[i.a]) && v[i.a] >= 0 && v[i.a] < 8 ? int(v[i.a]) : -1, v[i.b], v[i.c], &v[i.dst]);
                else std::fill_n(&v[i.dst], 4, 0.0f);
                break;
        }
    }
}
} // namespace Hydrogen::Runtime::Shader
