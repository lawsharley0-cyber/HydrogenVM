// Synthetic guest with a planted crash after four completed gestures.
#import <UIKit/UIKit.h>
#include <stdio.h>
#include <stdlib.h>

@interface Gate : UIView
@end
@implementation Gate
- (void)touchesEnded:(NSSet *)touches withEvent:(UIEvent *)event {
    static int step = 0;
    printf("AUTOPLAYER_GATE %d\n", ++step);
    fflush(stdout);
    if (step == 4) abort();
}
@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow *window;
@end
@implementation AppDelegate
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)options {
    CGRect bounds = [[UIScreen mainScreen] bounds];
    self.window = [[UIWindow alloc] initWithFrame:bounds];
    Gate *gate = [[Gate alloc] initWithFrame:bounds];
    [gate setBackgroundColor:[UIColor whiteColor]];
    [self.window addSubview:gate];
    [self.window makeKeyAndVisible];
    return YES;
}
@end

int main(int argc, char **argv) {
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, @"AppDelegate"); }
}
