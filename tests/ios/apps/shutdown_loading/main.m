#import <UIKit/UIKit.h>
#include <stdio.h>
// A loading screen that never returns to the run loop (Minecraft PE draws its loading screen
// in a loop until its assets are in): the request to close is never delivered. The watchdog
// must close it without a crash report, as iOS would, since nothing was being saved.
@interface AppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation AppDelegate
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)options {
    puts("LOADING_STARTED"); fflush(stdout);
    volatile unsigned counter = 0;
    for (;;) ++counter;
    return YES;
}
- (void)applicationWillTerminate:(UIApplication *)app {
    puts("LOADING_TERMINATION_ENTERED"); fflush(stdout);
}
@end
int main(int argc, char **argv) {
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, @"AppDelegate"); }
}
