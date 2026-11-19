// HydrogenVM — the iPhone's TLS bridge (TlsBridgeApple.mm). See src/runtime/libc/TlsBridge.hpp.
#pragma once
#include <memory>
#include "runtime/libc/TlsBridge.hpp"

namespace Hydrogen::App {
std::shared_ptr<Hydrogen::Runtime::TlsBridge> makeAppleTlsBridge();
}
