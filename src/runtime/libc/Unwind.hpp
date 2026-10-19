// HydrogenVM — raising guest exceptions from host code (see Unwind.cpp).
#pragma once
#include "runtime/Abi.hpp"

namespace Hydrogen::Runtime {

// Thrown (as a C++ exception) by host code to raise an exception in the guest:
// it unwinds the host frames, and the host-function entry that the guest
// called lands in the guest's handler (Process::registerHostFunction).
struct GuestThrow { uint32_t ue; };

// An Objective-C exception for `obj` if a guest @catch inside the current
// host->guest call would take it, else 0 (the exception is uncaught).
uint32_t prepareGuestObjCException(Ctx& x, uint32_t obj);
// Lands in the handler found by prepareGuestObjCException().
void landGuestException(Ctx& x, uint32_t ue);

} // namespace Hydrogen::Runtime
