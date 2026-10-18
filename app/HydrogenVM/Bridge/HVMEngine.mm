#import "HVMEngine.h"
#include "core/memory/GuestArena.hpp"
#include "selftest/SelfTest.hpp"

@interface HVMSelfTestResult ()
- (instancetype)initWithResult:(const Hydrogen::SelfTestResult &)r;
@end

@implementation HVMSelfTestResult
- (instancetype)initWithResult:(const Hydrogen::SelfTestResult &)r {
    if ((self = [super init])) {
        _passed = r.passed;
        _mips = r.mips;
        _instructions = r.instructions;
        _detail = [NSString stringWithUTF8String:r.detail.c_str()] ?: @"";
    }
    return self;
}
@end

@implementation HVMEngine

+ (BOOL)canReserveGuestArena:(NSString **)error {
    try {
        Hydrogen::Memory::GuestArena probe;
        return YES;
    } catch (const std::exception &e) {
        if (error) *error = [NSString stringWithUTF8String:e.what()];
        return NO;
    }
}

+ (HVMSelfTestResult *)runSelfTest {
    return [[HVMSelfTestResult alloc] initWithResult:Hydrogen::runSelfTest()];
}

@end
