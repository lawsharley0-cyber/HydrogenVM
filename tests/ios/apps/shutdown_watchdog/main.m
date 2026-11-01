#import <UIKit/UIKit.h>
#include <stdio.h>
@interface AppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation AppDelegate
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)options {
    puts("WATCHDOG_READY"); fflush(stdout); return YES;
}
- (void)applicationWillTerminate:(UIApplication *)app {
    puts("WATCHDOG_TERMINATION_ENTERED"); fflush(stdout);
    volatile unsigned counter = 0;
    for (;;) ++counter;
}
@end
int main(int argc, char **argv) {
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, @"AppDelegate"); }
}
