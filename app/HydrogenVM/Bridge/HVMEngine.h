// Objective-C face of the C++ engine, for Swift.
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <QuartzCore/QuartzCore.h>

NS_ASSUME_NONNULL_BEGIN

@interface HVMSelfTestResult : NSObject
@property (nonatomic, readonly) BOOL passed;
@property (nonatomic, readonly) double mips;            // guest million instructions / second
@property (nonatomic, readonly) uint64_t instructions;
@property (nonatomic, readonly, copy) NSString *detail;
@end

/// What the loader found in an app bundle.
@interface HVMAppInfo : NSObject
@property (nonatomic, readonly, copy) NSString *bundlePath;
@property (nonatomic, readonly, copy) NSString *bundleIdentifier;
@property (nonatomic, readonly, copy) NSString *name;
@property (nonatomic, readonly, copy) NSString *displayName;
@property (nonatomic, readonly, copy) NSString *version;
@property (nonatomic, readonly, copy) NSString *minimumOSVersion;
@property (nonatomic, readonly, copy, nullable) NSString *iconPath;
@property (nonatomic, readonly) BOOL runnable;
@property (nonatomic, readonly) BOOL encrypted;
/// Why the app can't run (encrypted, 64-bit only, missing executable…), empty when runnable.
@property (nonatomic, readonly, copy) NSString *problem;
@end

typedef NS_ENUM(NSInteger, HVMLogLevel) {
    HVMLogLevelDebug, HVMLogLevelInfo, HVMLogLevelWarning, HVMLogLevelError, HVMLogLevelStdout, HVMLogLevelStderr,
};

typedef NS_ENUM(NSInteger, HVMTouchPhase) { HVMTouchPhaseBegan = 0, HVMTouchPhaseMoved = 1, HVMTouchPhaseEnded = 3, HVMTouchPhaseCancelled = 4 };

/// One running guest app. Callbacks arrive on the main queue.
@interface HVMSession : NSObject
- (instancetype)initWithBundlePath:(NSString *)bundlePath homePath:(NSString *)homePath;
- (instancetype)init NS_UNAVAILABLE;

@property (nonatomic, copy, nullable) void (^onFrame)(CGImageRef image);
/// The layer that shows the guest's OpenGL ES drawable when it renders on the GPU
/// (useGPU). onFrame images are then an overlay with a transparent hole over it.
/// Sized in drawable pixels; the player scales it like the frame image.
@property (nonatomic, copy, nullable) void (^onGLLayer)(CALayer *_Nullable layer);
@property (nonatomic, readonly, nullable) CALayer *glLayer;
/// Render OpenGL ES on the GPU (default YES). NO uses the software rasterizer.
/// Takes effect at start.
@property (nonatomic) BOOL useGPU;
@property (nonatomic, copy, nullable) void (^onLog)(NSString *line, HVMLogLevel level);
/// exitCode is the guest's exit status; crashReport is non-nil when it crashed (or failed to load).
@property (nonatomic, copy, nullable) void (^onExit)(NSInteger exitCode, NSString *_Nullable crashReport);
/// Emulated screen in points (known once the app has started; 320x480 or 320x568).
@property (nonatomic, readonly) CGSize screenSize;
/// Guest UIInterfaceOrientation (1...4), captured with the delivered frame.
@property (nonatomic, readonly) NSInteger interfaceOrientation;
/// Lifecycle background acknowledgement; not a filesystem replacement barrier.
@property (nonatomic, readonly, getter=isBackgrounded) BOOL backgrounded;
/// Guest frame-hook work, lifetime p95 in milliseconds; zero before samples.
@property (nonatomic, readonly) double frameTimeP95Milliseconds;
/// Host output gain, 0...1. Does not change guest audio source gains.
@property (nonatomic) float outputVolume;
@property (nonatomic, readonly, getter=isRunning) BOOL running;
/// When YES, missing functions log and return 0 instead of stopping the app.
@property (nonatomic) BOOL keepGoing;

- (void)start;
/// Stops immediately (no chance to save).
- (void)stop;
/// Tells the app it's being closed (resign active, enter background, will terminate) so it
/// can save, then stops it. onExit follows; a hung app is stopped after a few seconds.
- (void)stopGracefully;
/// HydrogenVM went to the background: the app gets its background callbacks (and saves),
/// then is paused. enterForeground resumes it.
- (void)enterBackground;
- (void)enterForeground;
/// Touch in emulated-screen points (portrait).
- (void)touch:(HVMTouchPhase)phase x:(float)x y:(float)y finger:(NSUInteger)finger;
/// Accelerometer in g, device axes (portrait), like UIAcceleration.
- (void)setAccelerationX:(double)x y:(double)y z:(double)z;
@end

@interface HVMEngine : NSObject
/// Whether a 4 GB guest arena can be reserved on this device (entitlement check).
+ (BOOL)canReserveGuestArena:(NSString *_Nullable *_Nullable)error;
/// Runs a small built-in ARM + Thumb program and times it. Call off the main thread.
+ (HVMSelfTestResult *)runSelfTest;
/// Extract an .ipa into appsRoot/<bundle id>/<Name>.app. Call off the main thread.
+ (nullable HVMAppInfo *)importIPA:(NSString *)ipaPath intoDirectory:(NSString *)appsRoot error:(NSError **)error;
/// Inspect an already extracted .app.
+ (nullable HVMAppInfo *)appInfoAtPath:(NSString *)appPath;
/// Decode an image file with the engine's decoder (handles Xcode-crushed CgBI PNGs).
+ (nullable CGImageRef)decodeImageAtPath:(NSString *)path CF_RETURNS_RETAINED;
/// Install the CoreText text renderer and ImageIO decoder (call once at launch).
+ (void)installPlatformServices;
@end

NS_ASSUME_NONNULL_END
