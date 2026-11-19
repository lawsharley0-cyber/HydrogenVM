#pragma once
#include <string>

namespace Hydrogen::Runtime::Shader {
// Translate unconditional late extension directives at the GLES boundary, as
// accepted by legacy mobile drivers. Keep the strict language compiler unchanged.
std::string normalizeMobileSource(const std::string& source);
}
