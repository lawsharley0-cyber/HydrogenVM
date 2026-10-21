#pragma once
#include <array>
#include <string>
#include <vector>

namespace Hydrogen::Runtime::Shader {

// Bounded GLSL ES 1.00 straight-line subset. The preprocessor accepts the ES 1.00
// directive set, and globals accept precision, const initializers, structs, arrays,
// invariant, and qualifiers in any order. Unsupported syntax fails compilation;
// it must never select guessed rendering by identifier name.
constexpr int MaxSlots = 2048;
struct Type {
    struct Field {
        std::string name;
        int offset = 0;
        int width = 1;
        int count = 1;
        int matrix = 0;
        bool sampler = false;
        bool array = false;
        std::vector<Field> fields;
        bool operator==(const Field&) const = default;
    };
    int width = 1, matrix = 0;
    bool sampler = false;
    std::vector<Field> fields;
    bool operator==(const Type&) const = default;
    bool isStruct() const { return !fields.empty(); }
};
enum class Storage { Local, Attribute, Uniform, Varying, Output };
struct Variable {
    std::string name;
    Type type;
    Storage storage = Storage::Local;
    int slot = 0, count = 1;
    bool readOnly = false;
    bool array = false;
};
enum class Op { Constant, Move, Add, Subtract, Multiply, Divide, Texture };
struct Instruction {
    Op op;
    int dst = 0, a = 0, b = 0, c = 0;
    float value = 0;
};
using Values = std::array<float, MaxSlots>;
using Sample = void (*)(void*, int unit, float u, float v, float* rgba);
struct Program {
    bool vertex = false;
    int slots = 0, output = 0;
    std::vector<Variable> variables;
    std::vector<Instruction> code;
    std::string error;
    bool compile(const std::string& source, bool isVertex);
    void initialize(Values& values) const;
    void execute(Values& values, Sample sample = nullptr, void* context = nullptr) const;
    const Variable* find(const std::string& name) const;
};
} // namespace Hydrogen::Runtime::Shader
