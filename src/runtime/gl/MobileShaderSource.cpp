#include "MobileShaderSource.hpp"
#include <algorithm>
#include <cctype>
#include <vector>

namespace Hydrogen::Runtime::Shader {
std::string normalizeMobileSource(const std::string& source) {
    // Mobile engines prepend precision/uniform declarations to shader bodies
    // whose extension directives were originally at the beginning. Translate
    // only unconditional directives, before any function or struct body.
    if (source.find("\\\n") != std::string::npos || source.find("\\\r\n") != std::string::npos) return source;
    struct Line { size_t begin, end; std::string text; };
    std::vector<Line> extensions;
    bool blockComment = false, code = false, late = false, body = false;
    int conditional = 0;
    size_t insertion = 0;
    for (size_t begin = 0; begin < source.size();) {
        size_t end = source.find('\n', begin);
        if (end == std::string::npos) end = source.size();
        const bool beganInComment = blockComment;
        std::string clean;
        for (size_t i = begin; i < end; ++i) {
            if (blockComment) {
                if (source[i] == '*' && i + 1 < end && source[i + 1] == '/') { blockComment = false; ++i; clean += ' '; }
            } else if (source[i] == '/' && i + 1 < end && source[i + 1] == '/') break;
            else if (source[i] == '/' && i + 1 < end && source[i + 1] == '*') { blockComment = true; ++i; clean += ' '; }
            else clean += source[i];
        }
        size_t p = clean.find_first_not_of(" \t\r");
        const size_t next = end < source.size() ? end + 1 : end;
        if (p != std::string::npos && clean[p] == '#') {
            p = clean.find_first_not_of(" \t", p + 1);
            const size_t start = p;
            while (p != std::string::npos && p < clean.size() && std::isalpha(static_cast<unsigned char>(clean[p]))) ++p;
            const std::string directive = start == std::string::npos ? "" : clean.substr(start, p - start);
            if (directive == "if" || directive == "ifdef" || directive == "ifndef") ++conditional;
            else if (directive == "endif") --conditional;
            else if (directive == "line") return source;
            else if (directive == "version") {
                if (code || conditional || blockComment) return source;
                insertion = next;
            } else if (directive == "extension") {
                if (conditional || beganInComment || blockComment || body) return source;
                late |= code;
                extensions.push_back({begin, next, clean});
            }
        } else if (p != std::string::npos) {
            code = true;
            body |= clean.find('{') != std::string::npos;
        }
        begin = next;
    }
    if (!late || blockComment || conditional) return source;
    std::string result = source, header;
    for (const auto& line : extensions) {
        header += line.text + '\n';
        for (size_t i = line.begin; i < line.end; ++i) if (result[i] != '\n') result[i] = ' ';
    }
    const auto originalLine = 1 + std::count(source.begin(), source.begin() + insertion, '\n');
    header += "#line " + std::to_string(originalLine) + "\n";
    result.insert(insertion, header);
    return result;
}
}
