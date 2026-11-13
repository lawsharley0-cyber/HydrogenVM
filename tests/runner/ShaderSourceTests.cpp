#include "runtime/graphics/Shader.hpp"
#include "runtime/gl/MobileShaderSource.hpp"
#include <cstdio>

using namespace Hydrogen::Runtime;
int main() {
    const std::string extension = "#extension GL_OES_standard_derivatives : enable\n";
    const std::string shader = "precision mediump float;\nuniform vec4 tint;\n" + extension +
        "void main(){ gl_FragColor = tint * fwidth(gl_FragCoord.x); }\n";
    Shader::Program direct, translated;
    if (direct.compile(shader, false)) { std::puts("FAIL: strict compiler unexpectedly accepts late extension"); return 1; }
    const auto normalized = Shader::normalizeMobileSource(shader);
    if (normalized == shader || !translated.compile(normalized, false)) { std::puts("FAIL: mobile late-extension translation"); return 1; }
    const auto versioned = Shader::normalizeMobileSource("#version 100\n" + shader);
    if (versioned.find("#version 100\n") != 0 || !translated.compile(versioned, false)) { std::puts("FAIL: preserve GLSL version"); return 1; }
    const std::string conditional = "precision mediump float;\n#if 0\n" + extension + "#endif\nvoid main(){}\n";
    const std::string inFunction = "void main(){\n" + extension + "gl_FragColor=vec4(1.0);}\n";
    const std::string comment = "/*\n" + extension + "*/\nvoid main(){}\n";
    if (Shader::normalizeMobileSource(conditional) != conditional || Shader::normalizeMobileSource(inFunction) != inFunction ||
        Shader::normalizeMobileSource(comment) != comment) { std::puts("FAIL: must preserve conditional, function and commented directives"); return 1; }
    std::puts("mobile shader source: passed");
}
