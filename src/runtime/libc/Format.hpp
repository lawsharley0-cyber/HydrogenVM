// HydrogenVM — printf-style formatting over guest arguments, shared by the
// stdio functions, NSLog and -[NSString stringWithFormat:].
#pragma once
#include "runtime/Process.hpp"
#include <functional>
#include <string>

namespace Hydrogen::Runtime {

// `objectHook` formats %@ (Objective-C objects); without one, %@ prints the
// pointer. `writeCount` handles %n (rarely used, but must not crash).
std::string formatGuest(Process& p, const std::string& fmt, ArgReader& args,
                        const std::function<std::string(uint32_t)>& objectHook = {});

// The process-wide %@ hook, installed by Foundation.
void setObjectFormatHook(Process& p, std::function<std::string(uint32_t)> hook);
const std::function<std::string(uint32_t)>& objectFormatHook(Process& p);

} // namespace Hydrogen::Runtime
