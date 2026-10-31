// Project-owned fixture for ObjC backtraces after the Mach-O names are stripped.
#import <Foundation/Foundation.h>
static volatile int returned;

@interface HVMTraceCallee : NSObject
+ (void)trap;
@end
@implementation HVMTraceCallee
+ (void)trap {
    __asm__ volatile("bkpt #0" ::: "memory");
    ++returned;
}
@end

@interface HVMTraceCaller : NSObject
- (void)enter;
@end
@implementation HVMTraceCaller
- (void)enter {
    [HVMTraceCallee trap];
    ++returned; // keep the caller's frame; no tail call
}
@end

int main(void) {
    @autoreleasepool {
        HVMTraceCaller *caller = [HVMTraceCaller new];
        [caller enter];
    }
    return returned;
}
