// Objective-C face of the C++ engine, for Swift.
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface HVMSelfTestResult : NSObject
@property (nonatomic, readonly) BOOL passed;
@property (nonatomic, readonly) double mips;            // guest million instructions / second
@property (nonatomic, readonly) uint64_t instructions;
@property (nonatomic, readonly, copy) NSString *detail;
@end

@interface HVMEngine : NSObject
/// Whether a 4 GB guest arena can be reserved on this device (entitlement check).
+ (BOOL)canReserveGuestArena:(NSString * _Nullable * _Nullable)error;
/// Runs a small built-in ARM + Thumb program and times it. Call off the main thread.
+ (HVMSelfTestResult *)runSelfTest;
@end

NS_ASSUME_NONNULL_END
