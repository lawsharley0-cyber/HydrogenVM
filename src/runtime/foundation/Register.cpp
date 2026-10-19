// HydrogenVM — Foundation / CoreFoundation registration.
#include "Foundation.hpp"

namespace Hydrogen::Runtime {

void registerFoundation(Process& p) {
    using namespace Foundation;
    ObjC::Runtime& r = ObjC::rt(p);
    // Order matters only where one module's registration uses another's
    // classes or constants (strings first: everything makes constant strings).
    registerStrings(p, r);
    registerNumbers(p, r);
    registerCollections(p, r);
    registerData(p, r);
    registerRunLoop(p, r);
    registerMisc(p, r);
    registerNotifications(p, r);
    registerBundle(p, r);
    registerDefaults(p, r);
    registerDispatch(p, r);
    registerCoreFoundation(p, r);
}

} // namespace Hydrogen::Runtime
