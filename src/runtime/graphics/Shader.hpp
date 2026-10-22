#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Hydrogen::Runtime::Shader {

// GLSL ES 1.00 translator. The preprocessor follows the ES 1.00 directive set.
// The language covers declarations, user functions, control flow, and the ES 1.00
// built-in library. ES 1.00 grammar places invariant, then storage, then precision,
// but shipped shaders put precision first (`mediump varying`, `highp attribute`);
// qualifiers are accepted in any order and the invariant bit is stored on the variable.
// Unsupported syntax fails compilation and never selects rendering by identifier name.
// GL_OES_standard_derivatives (dFdx, dFdy, fwidth) is accepted when that extension
// is enabled. `#extension name : warn` records a warning and still enables a
// supported extension. The rasterizer evaluates those ops across 2x2 pixel quads.
// Array sizes are integral constant expressions (literals, const values,
// constructors, and constant-foldable built-ins) whose element count fits in MaxSlots.
// Draws pass one shared instruction budget into execute(); exhausting it aborts the draw.
constexpr int MaxSlots = 2048;
enum class Kind { Float, Int, Bool, Void };
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
        Kind kind = Kind::Float;
        bool cube = false;
        bool operator==(const Field&) const = default;
    };
    int width = 1, matrix = 0;
    bool sampler = false;
    std::vector<Field> fields;
    Kind kind = Kind::Float;
    bool cube = false;
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
    bool invariant = false;
};
enum class Op {
    Constant, Move, Add, Subtract, Multiply, Divide, Texture,
    Jump, JumpIfFalse, Discard, Select,
    Abs, Floor, Ceil, Sqrt, InverseSqrt, Pow, Exp, Log, Exp2, Log2,
    Sin, Cos, Tan, Asin, Acos, Atan, Atan2,
    Less, LessEqual, Greater, GreaterEqual, Equal, NotEqual, Not,
    IntDivide, TextureCube, DFdx, DFdy
};
struct Instruction {
    Op op;
    int dst = 0, a = 0, b = 0, c = 0;
    float value = 0;
    int d = 0;
};
using Values = std::array<float, MaxSlots>;
using Sample = void (*)(void*, int unit, float u, float v, float* rgba);
struct Program {
    bool vertex = false;
    int slots = 0, output = 0;
    std::vector<Variable> variables;
    std::vector<Instruction> code;
    std::string error;
    // Non-fatal diagnostics, including `#extension name : warn`. Empty when the
    // shader did not warn. Compilation can still succeed.
    std::string warnings;
    bool usesDerivatives = false;
    bool compile(const std::string& source, bool isVertex);
    void initialize(Values& values) const;
    // Returns false when a fragment discards, or when `budget` is exhausted.
    // A null budget keeps a per-invocation safety cap and still returns true,
    // matching direct unit-test execution. A draw passes one shared counter;
    // on exhaustion the counter is set negative and the invocation is dropped.
    bool execute(Values& values, Sample sample = nullptr, void* context = nullptr, int64_t* budget = nullptr) const;
    // Lanes are window pixels (x,y), (x+1,y), (x,y+1), (x+1,y+1) with x and y even.
    // activeMask selects lanes that run. discardedMask marks lanes that discarded.
    // Returns false when the shared budget is exhausted; the caller writes nothing
    // for that quad and aborts the rest of the draw.
    bool executeQuad(std::array<Values, 4>& lanes, Sample sample, void* context, int64_t* budget,
                     int activeMask, int& discardedMask) const;
    const Variable* find(const std::string& name) const;
};
} // namespace Hydrogen::Runtime::Shader
