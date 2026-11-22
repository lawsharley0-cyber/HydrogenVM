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

// True (once) when the last formatGuest on this thread met a wchar_t that is no Unicode
// character (%ls / C's %S / %lc). Apple's libc then fails the whole call: -1, errno EILSEQ.
// Only C formatting sets it; Cocoa formats (with an object hook) never do.
bool takeFormatEncodingError();

// The process-wide %@ hook, installed by Foundation.
void setObjectFormatHook(Process& p, std::function<std::string(uint32_t)> hook);
const std::function<std::string(uint32_t)>& objectFormatHook(Process& p);

} // namespace Hydrogen::Runtime
