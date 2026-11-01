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

// A category +load is called by the runtime, never attached as a method, so its frame needs its own name.
// The category extends a class from another image (NSObject): the linker merges a category into a class
// defined in the same image, which would leave the frame named after the class and lose the category name.
// It starts the whole chain: the guest traps below it, before main runs.
@interface NSObject (HVMBoot)
@end
@implementation NSObject (HVMBoot)
+ (void)load {
    HVMTraceCaller *caller = [HVMTraceCaller new];
    [caller enter];
    ++returned; // keep this frame; no tail call
}
@end

int main(void) {
    return returned;
}
