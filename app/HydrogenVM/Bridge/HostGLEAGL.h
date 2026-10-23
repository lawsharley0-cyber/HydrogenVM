// HostGL platform on the device: Apple's OpenGLES.framework (EAGL).
#pragma once
#include "runtime/gl/HostGL.hpp"
#include <functional>
#include <memory>

#ifdef __OBJC__
@class CALayer;
#else
typedef struct objc_object CALayer;
#endif

namespace Hydrogen::App {

// Creates the EAGL platform. `onLayer` runs on the main queue whenever the
// layer that shows the guest's GL drawable appears (or changes); the shell puts
// it under the composited UIKit overlay. Returns null if OpenGLES.framework
// isn't usable on this OS.
std::shared_ptr<Hydrogen::Runtime::HostGL::Platform> makeEaglPlatform(std::function<void(CALayer*)> onLayer);

} // namespace Hydrogen::App
