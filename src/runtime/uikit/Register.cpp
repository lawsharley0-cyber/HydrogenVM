// HydrogenVM — UIKit / QuartzCore / CoreGraphics registration.
#include "UIKit.hpp"

namespace Hydrogen::Runtime {

namespace UIKit {
UIState& ui(Process& p) { return p.service<UIState>(); }
}

void registerUIKit(Process& p) {
    ObjC::Runtime& r = ObjC::rt(p);
    CG::registerCoreGraphics(p, r);
    UIKit::registerQuartzCore(p, r);
    UIKit::registerUIView(p, r);         // UIResponder, UIView, UIWindow, UIScreen, UIViewController
    UIKit::registerUIGraphics(p, r);     // UIColor, UIFont, UIImage, drawing helpers
    UIKit::registerUIControls(p, r);     // UIControl, UILabel, UIImageView, UIButton, alerts
    UIKit::registerUIApplication(p, r);  // UIApplication, events, UIDevice, UIApplicationMain
    UIKit::registerGestures(p, r);
    UIKit::registerNib(p, r);
    UIKit::registerPlatformStubs(p, r);
}

} // namespace Hydrogen::Runtime
