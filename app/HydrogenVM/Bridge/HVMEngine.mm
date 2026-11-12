#import "HVMEngine.h"
#import "HostGLEAGL.h"
#import <QuartzCore/QuartzCore.h>
#import <CoreText/CoreText.h>
#import <ImageIO/ImageIO.h>
#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>
#include "core/memory/GuestArena.hpp"
#include "loader/AppBundle.hpp"
#include "runtime/Process.hpp"
#include "runtime/audio/Audio.hpp"
#include "runtime/graphics/Graphics.hpp"
#include "runtime/uikit/UIKit.hpp"
#include "selftest/SelfTest.hpp"
#include <pthread.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace RT = Hydrogen::Runtime;
namespace G = Hydrogen::Graphics;

static NSString *str(const std::string &s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding]
        ?: [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSISOLatin1StringEncoding] ?: @"";
}

// ------------------------------------------------------------ self test ---

@interface HVMSelfTestResult ()
- (instancetype)initWithResult:(const Hydrogen::SelfTestResult &)r;
@end

@implementation HVMSelfTestResult
- (instancetype)initWithResult:(const Hydrogen::SelfTestResult &)r {
    if ((self = [super init])) {
        _passed = r.passed;
        _mips = r.mips;
        _instructions = r.instructions;
        _detail = str(r.detail);
    }
    return self;
}
@end

// ------------------------------------------------------------- app info ---

@interface HVMAppInfo ()
- (instancetype)initWithInfo:(const Hydrogen::Loader::AppInfo &)a;
@end

@implementation HVMAppInfo
- (instancetype)initWithInfo:(const Hydrogen::Loader::AppInfo &)a {
    if ((self = [super init])) {
        _bundlePath = str(a.bundlePath);
        _bundleIdentifier = str(a.bundleId);
        _name = str(a.name);
        _displayName = a.displayName.empty() ? _name : str(a.displayName);
        _version = str(a.version);
        _minimumOSVersion = str(a.minimumOS);
        _iconPath = a.iconPaths.empty() ? nil : str(a.iconPaths.front());
        _runnable = a.runnable;
        _encrypted = a.encrypted;
        _problem = str(a.problem);
    }
    return self;
}
@end

// ------------------------------------------------------- platform hooks ---

namespace {

// CoreText-backed text: real fonts instead of the built-in bitmap font.
struct CoreTextBackend final : G::TextBackend {
    std::mutex mu;
    NSCache<NSString *, id> *fonts = [NSCache new];

    static NSString *normalized(NSString *n) {
        return [[n.lowercaseString stringByReplacingOccurrencesOfString:@" " withString:@""]
            stringByReplacingOccurrencesOfString:@"-" withString:@""];
    }

    // Returns a +1 font.
    CTFontRef font(const G::FontDesc &f, float scale) {
        NSString *key = [NSString stringWithFormat:@"%s|%g|%d%d", f.name.c_str(), f.size * scale, f.bold, f.italic];
        std::lock_guard<std::mutex> lk(mu);
        if (id cached = [fonts objectForKey:key]) return (CTFontRef)CFRetain((__bridge CFTypeRef)cached);
        const CGFloat size = std::max(1.0f, f.size * scale);
        CTFontRef base = nullptr;
        if (!f.name.empty()) {
            NSString *want = str(f.name);
            base = CTFontCreateWithName((__bridge CFStringRef)want, size, nullptr);
            // CoreText substitutes silently for unknown names; prefer the system font then.
            NSString *ps = normalized((__bridge_transfer NSString *)CTFontCopyPostScriptName(base));
            NSString *fam = normalized((__bridge_transfer NSString *)CTFontCopyFamilyName(base));
            NSString *w = normalized(want);
            NSString *wFamily = normalized([want componentsSeparatedByString:@"-"].firstObject ?: want);
            if (![ps hasPrefix:wFamily] && ![fam hasPrefix:wFamily] && ![w hasPrefix:fam]) {
                CFRelease(base);
                base = nullptr;
            }
        }
        if (!base) base = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, size, nullptr);
        CTFontSymbolicTraits traits = (f.bold ? kCTFontTraitBold : 0) | (f.italic ? kCTFontTraitItalic : 0);
        if (traits) {
            if (CTFontRef styled = CTFontCreateCopyWithSymbolicTraits(base, size, nullptr, traits, traits)) {
                CFRelease(base);
                base = styled;
            }
        }
        [fonts setObject:(__bridge id)base forKey:key];
        return base;
    }

    std::vector<CTLineRef> lines(const std::string &s, CTFontRef ft) {
        std::vector<CTLineRef> out;
        NSString *text = str(s);
        NSDictionary *attrs = @{(__bridge id)kCTFontAttributeName: (__bridge id)ft,
                                (__bridge id)kCTForegroundColorFromContextAttributeName: @YES};
        for (NSString *ln in [text componentsSeparatedByString:@"\n"]) {
            NSAttributedString *as = [[NSAttributedString alloc] initWithString:ln attributes:attrs];
            out.push_back(CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)as));
        }
        return out;
    }

    G::TextLayout layoutFor(const std::vector<CTLineRef> &ls, CTFontRef ft, float scale) {
        G::TextLayout l;
        l.ascent = float(CTFontGetAscent(ft)) / scale;
        l.descent = float(CTFontGetDescent(ft)) / scale;
        l.lineHeight = float(std::ceil(CTFontGetAscent(ft) + CTFontGetDescent(ft) + CTFontGetLeading(ft))) / scale;
        for (CTLineRef line : ls) l.width = std::max(l.width, float(CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr)) / scale);
        l.height = l.lineHeight * float(ls.size());
        return l;
    }

    G::TextLayout measure(const std::string &s, const G::FontDesc &f) override {
        @autoreleasepool {
            CTFontRef ft = font(f, 1);
            auto ls = lines(s, ft);
            G::TextLayout l = layoutFor(ls, ft, 1);
            for (CTLineRef line : ls) CFRelease(line);
            CFRelease(ft);
            return l;
        }
    }

    std::shared_ptr<G::Image> render(const std::string &s, const G::FontDesc &f, float scale, G::TextLayout *out) override {
        @autoreleasepool {
            if (scale <= 0) scale = 1;
            CTFontRef ft = font(f, scale);
            auto ls = lines(s, ft);
            // Layout in points (measured at device size for crisp metrics).
            G::TextLayout l = layoutFor(ls, ft, scale);
            if (out) *out = l;
            auto img = std::make_shared<G::Image>();
            img->width = std::max(1, int(std::ceil(l.width * scale)) + 1);
            img->height = std::max(1, int(std::ceil(l.height * scale)));
            img->rgba.assign(size_t(img->width) * size_t(img->height) * 4, 0);
            CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
            CGContextRef ctx = CGBitmapContextCreate(img->rgba.data(), size_t(img->width), size_t(img->height), 8, size_t(img->width) * 4, cs,
                                                     kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
            CGColorSpaceRelease(cs);
            if (ctx) {
                CGContextSetRGBFillColor(ctx, 1, 1, 1, 1);
                CGContextSetShouldSmoothFonts(ctx, false);
                const CGFloat lh = CGFloat(l.lineHeight * scale);
                for (size_t i = 0; i < ls.size(); ++i) {
                    // CG is y-up: baseline of line i sits ascent below the top of its line box.
                    const CGFloat baseline = CGFloat(img->height) - (CGFloat(i) * lh + CTFontGetAscent(ft));
                    CGContextSetTextPosition(ctx, 0, baseline);
                    CTLineDraw(ls[i], ctx);
                }
                CGContextRelease(ctx);
            }
            for (CTLineRef line : ls) CFRelease(line);
            CFRelease(ft);
            return img;
        }
    }
};

// ImageIO for JPEG, GIF, TIFF, BMP, ... (PNG goes through the engine's own decoder first).
G::ImagePtr decodeWithImageIO(const uint8_t *data, size_t n) {
    @autoreleasepool {
        NSData *d = [NSData dataWithBytesNoCopy:(void *)data length:n freeWhenDone:NO];
        CGImageSourceRef src = CGImageSourceCreateWithData((__bridge CFDataRef)d, nullptr);
        if (!src) return nullptr;
        CGImageRef cg = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
        CFRelease(src);
        if (!cg) return nullptr;
        auto img = std::make_shared<G::Image>();
        img->width = int(CGImageGetWidth(cg));
        img->height = int(CGImageGetHeight(cg));
        img->rgba.assign(size_t(img->width) * size_t(img->height) * 4, 0);
        const CGImageAlphaInfo ai = CGImageGetAlphaInfo(cg);
        img->opaque = ai == kCGImageAlphaNone || ai == kCGImageAlphaNoneSkipLast || ai == kCGImageAlphaNoneSkipFirst;
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx = CGBitmapContextCreate(img->rgba.data(), size_t(img->width), size_t(img->height), 8, size_t(img->width) * 4, cs,
                                                 kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
        CGColorSpaceRelease(cs);
        if (ctx) {
            CGContextSetBlendMode(ctx, kCGBlendModeCopy);
            CGContextDrawImage(ctx, CGRectMake(0, 0, img->width, img->height), cg);
            CGContextRelease(ctx);
        }
        CGImageRelease(cg);
        return ctx ? img : nullptr;
    }
}

// Run `fn` on a fresh thread with a big stack (guest calls nest deeply).
void spawnBigStack(void (^fn)(void)) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16u << 20);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    void *ctx = (__bridge_retained void *)[fn copy];
    pthread_t t;
    int rc = pthread_create(&t, &attr, [](void *p) -> void * {
        pthread_setname_np("HydrogenVM main");
        void (^block)(void) = (__bridge_transfer void (^)(void))p;
        @autoreleasepool { block(); }
        return nullptr;
    }, ctx);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        void (^block)(void) = (__bridge_transfer void (^)(void))ctx;
        block();
    }
}

} // namespace

// -------------------------------------------------------------- session ---

namespace {
// Shared between the session object and its run thread (which may outlive it).
struct SessionCore {
    std::mutex mu;
    std::shared_ptr<RT::Process> proc;
    std::atomic<bool> ready{false};       // load() finished: input handlers are installed
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> framePending{false};
    std::atomic<float> outputVolume{1};
    // Log batching: lines are flushed to the main queue a few times a second.
    std::mutex logMu;
    std::vector<std::pair<std::string, RT::LogLevel>> logs;
    bool logFlushScheduled = false;
    AVAudioEngine *audioEngine = nil;   // pulls from the guest's mixer
    std::mutex audioMu;
    bool micOn = false;                  // input tap installed (the guest has recorded this run)
    std::atomic<bool> micWanted{false};  // the guest is recording right now: the tap's sound reaches it
    id audioConfigObserver = nil;        // restarts the engine after the system reconfigures or interrupts it
    id audioInterruptObserver = nil;
    // Per-run log file in Documents/Logs (visible in the Files app). Written and
    // flushed line by line on the engine thread, so it survives a hard crash.
    FILE *logFile = nullptr;
    ~SessionCore() { if (logFile) fclose(logFile); }
    void logToFile(const char *tag, const std::string &msg) {
        if (!logFile) return;
        fputs(tag, logFile);
        fputs(msg.c_str(), logFile);
        if (msg.empty() || msg.back() != '\n') fputc('\n', logFile);
        fflush(logFile);
    }
};

// Opens Documents/Logs/<date>-<app>.log and keeps the 30 newest logs. Games the player plays
// log to Documents/Logs/Play, so a Test All Games run (one log per game) never pushes them out.
FILE *openRunLog(const std::string &bundlePath, bool play) {
    NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    if (!docs) return nullptr;
    NSString *dir = [docs stringByAppendingPathComponent:play ? @"Logs/Play" : @"Logs"];
    NSFileManager *fm = NSFileManager.defaultManager;
    [fm createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    NSArray<NSString *> *old = [[fm contentsOfDirectoryAtPath:dir error:nil] sortedArrayUsingSelector:@selector(compare:)];
    NSMutableArray<NSString *> *logs = [NSMutableArray array];
    for (NSString *f in old) if ([f hasSuffix:@".log"]) [logs addObject:f];
    while (logs.count >= 30) {
        // Names start with the date stamp, so the first one is the oldest.
        [fm removeItemAtPath:[dir stringByAppendingPathComponent:logs.firstObject] error:nil];
        [logs removeObjectAtIndex:0];
    }
    NSDateFormatter *df = [[NSDateFormatter alloc] init];
    df.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    df.dateFormat = @"yyyyMMdd-HHmmss";
    NSString *app = [[str(bundlePath) lastPathComponent] stringByDeletingPathExtension];
    NSString *name = [NSString stringWithFormat:@"%@-%@.log", [df stringFromDate:NSDate.date], app];
    FILE *f = fopen([dir stringByAppendingPathComponent:name].fileSystemRepresentation, "w");
    if (f) {
        NSDictionary *info = NSBundle.mainBundle.infoDictionary;
        UIDevice *dev = UIDevice.currentDevice;
        fprintf(f, "HydrogenVM %s (%s) on %s %s\nguest: %s\n\n",
                [info[@"CFBundleShortVersionString"] UTF8String] ?: "?", [info[@"CFBundleVersion"] UTF8String] ?: "?",
                dev.systemName.UTF8String, dev.systemVersion.UTF8String, bundlePath.c_str());
        fflush(f);
    }
    return f;
}
}

namespace {
// Game audio: one AVAudioEngine whose source node renders the guest's mixer on
// the audio thread. The playback category keeps it audible with the ring/silent
// switch on (as a media app), mixing with other audio.
// The microphone tap: the input node's first channel goes to the guest while it is recording.
// Installed with the bus's own format (nil), which is the only one valid after a route change.
// Caller holds audioMu.
bool installMicTap(const std::shared_ptr<SessionCore> &core, AVAudioEngine *engine) {
    std::weak_ptr<SessionCore> weak = core;
    AVAudioInputNode *input = engine.inputNode;
    AVAudioFormat *fmt = [input outputFormatForBus:0];
    if (fmt.sampleRate <= 0 || fmt.channelCount == 0) {   // no usable input: a tap would raise
        NSLog(@"HydrogenVM: no microphone input is available; the app records silence");
        return false;
    }
    @try {
        [input installTapOnBus:0 bufferSize:1024 format:nil block:^(AVAudioPCMBuffer *buffer, AVAudioTime *) {
            auto c = weak.lock();
            if (!c || !c->micWanted.load() || !buffer.floatChannelData || buffer.format.sampleRate <= 0) return;
            std::shared_ptr<RT::Process> p;
            { std::lock_guard<std::mutex> plk(c->mu); p = c->proc; }
            if (p) p->pushMicrophone(buffer.floatChannelData[0], buffer.frameLength, buffer.format.sampleRate);
        }];
    } @catch (NSException *e) {
        NSLog(@"HydrogenVM: microphone tap failed: %@", e.reason);
        return false;
    }
    return true;
}
// Starts the engine again if the system stopped it. Main queue.
void resumeAudio(const std::shared_ptr<SessionCore> &core) {
    std::lock_guard<std::mutex> lk(core->audioMu);
    AVAudioEngine *engine = core->audioEngine;
    if (!engine || engine.isRunning) return;
    [AVAudioSession.sharedInstance setActive:YES error:nil];
    if (core->micOn) {   // the input format may have changed with the hardware
        [engine.inputNode removeTapOnBus:0];
        core->micOn = installMicTap(core, engine);
    }
    engine.mainMixerNode.outputVolume = core->outputVolume.load();
    NSError *err = nil;
    if (![engine startAndReturnError:&err]) NSLog(@"HydrogenVM: audio engine failed to resume: %@", err);
}
void startAudio(const std::shared_ptr<SessionCore> &core, std::shared_ptr<Hydrogen::Runtime::Audio::Mixer> mixer) {
    std::lock_guard<std::mutex> lk(core->audioMu);
    if (core->audioEngine) return;
    AVAudioSession *session = AVAudioSession.sharedInstance;
    [session setCategory:AVAudioSessionCategoryPlayback withOptions:AVAudioSessionCategoryOptionMixWithOthers error:nil];
    [session setActive:YES error:nil];
    std::weak_ptr<Hydrogen::Runtime::Audio::Mixer> weak = mixer;
    AVAudioFormat *fmt = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:Hydrogen::Runtime::Audio::Mixer::kRate channels:2];
    AVAudioSourceNode *src = [[AVAudioSourceNode alloc] initWithFormat:fmt renderBlock:^OSStatus(BOOL *isSilence, const AudioTimeStamp *, AVAudioFrameCount frames, AudioBufferList *abl) {
        float *l = static_cast<float *>(abl->mBuffers[0].mData);
        float *r = abl->mNumberBuffers > 1 ? static_cast<float *>(abl->mBuffers[1].mData) : nullptr;
        auto m = weak.lock();
        if (!m || !l) {
            for (UInt32 b = 0; b < abl->mNumberBuffers; ++b) std::memset(abl->mBuffers[b].mData, 0, abl->mBuffers[b].mDataByteSize);
            *isSilence = YES;
            return noErr;
        }
        if (r) m->render(l, r, frames);
        else {
            std::vector<float> right(frames);
            m->render(l, right.data(), frames);
            for (AVAudioFrameCount i = 0; i < frames; ++i) l[i] = 0.5f * (l[i] + right[i]);
        }
        return noErr;
    }];
    AVAudioEngine *engine = [[AVAudioEngine alloc] init];
    [engine attachNode:src];
    [engine connect:src to:engine.mainMixerNode format:fmt];
    engine.mainMixerNode.outputVolume = core->outputVolume.load();
    NSError *err = nil;
    if (![engine startAndReturnError:&err]) NSLog(@"HydrogenVM: audio engine failed to start: %@", err);
    core->audioEngine = engine;
    // The system stops an AVAudioEngine whenever the audio hardware is reconfigured (the
    // session switching to play-and-record for a game's microphone, headphones plugged in, a
    // Bluetooth route) or interrupted (a call, Siri, an alarm), and never starts it again.
    // Nothing restarted it, so a game's sound went silent for the rest of the run.
    std::weak_ptr<SessionCore> weakCore = core;
    core->audioConfigObserver = [NSNotificationCenter.defaultCenter addObserverForName:AVAudioEngineConfigurationChangeNotification
                                                                               object:engine queue:NSOperationQueue.mainQueue
                                                                           usingBlock:^(NSNotification *) {
        if (auto c = weakCore.lock()) resumeAudio(c);
    }];
    core->audioInterruptObserver = [NSNotificationCenter.defaultCenter addObserverForName:AVAudioSessionInterruptionNotification
                                                                                  object:nil queue:NSOperationQueue.mainQueue
                                                                              usingBlock:^(NSNotification *note) {
        if ([note.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue] != AVAudioSessionInterruptionTypeEnded) return;
        if (auto c = weakCore.lock()) resumeAudio(c);
    }];
}
void stopAudio(const std::shared_ptr<SessionCore> &core) {
    std::lock_guard<std::mutex> lk(core->audioMu);
    if (core->audioConfigObserver) [NSNotificationCenter.defaultCenter removeObserver:core->audioConfigObserver];
    if (core->audioInterruptObserver) [NSNotificationCenter.defaultCenter removeObserver:core->audioInterruptObserver];
    core->audioConfigObserver = nil;
    core->audioInterruptObserver = nil;
    const bool hadMic = core->micOn;
    if (core->micOn) [core->audioEngine.inputNode removeTapOnBus:0];
    core->micOn = false;
    core->micWanted = false;
    [core->audioEngine stop];
    core->audioEngine = nil;
    if (hadMic) [AVAudioSession.sharedInstance setCategory:AVAudioSessionCategoryPlayback withOptions:AVAudioSessionCategoryOptionMixWithOthers error:nil];
}

// Microphone: the first time the guest records, the session switches to play-and-record
// (still out of the speaker) and the engine's input node is tapped, so apps that never
// record never ask for the microphone. The tap then stays for the rest of the run and only
// its delivery follows the guest: a Talking game turns recording off and on around every
// sound it plays, and restarting the engine each time cut those sounds and its listening.
void beginCapture(const std::shared_ptr<SessionCore> &core) {
    std::lock_guard<std::mutex> lk(core->audioMu);
    AVAudioEngine *engine = core->audioEngine;
    if (!engine || core->micOn) return;
    AVAudioSession *session = AVAudioSession.sharedInstance;
    [engine stop];
    // AllowBluetoothA2DP keeps Bluetooth headphones as the output while the phone's own
    // microphone records; without it iOS moves a play-and-record session to the speaker.
    // DefaultToSpeaker only replaces the earpiece, so wired headphones stay in use too.
    [session setCategory:AVAudioSessionCategoryPlayAndRecord
             withOptions:AVAudioSessionCategoryOptionDefaultToSpeaker | AVAudioSessionCategoryOptionMixWithOthers |
                         AVAudioSessionCategoryOptionAllowBluetoothA2DP | AVAudioSessionCategoryOptionAllowAirPlay
                   error:nil];
    [session setActive:YES error:nil];
    core->micOn = installMicTap(core, engine);
    NSError *err = nil;
    if (![engine startAndReturnError:&err]) NSLog(@"HydrogenVM: audio engine failed to restart for recording: %@", err);
}
void setMicrophone(const std::shared_ptr<SessionCore> &core, bool on) {
    core->micWanted = on;
    if (!on) return;
    [AVAudioApplication requestRecordPermissionWithCompletionHandler:^(BOOL granted) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (granted) beginCapture(core);
            else NSLog(@"HydrogenVM: microphone permission denied; the app records silence");
        });
    }];
}
}

@interface HVMSession ()
@property (nonatomic, readwrite, copy, nullable) NSString *crashBundle;
@property (nonatomic, readwrite) CGSize screenSize;
@property (nonatomic, readwrite) NSInteger interfaceOrientation;
@property (nonatomic, readwrite, getter=isRunning) BOOL running;
@property (nonatomic, readwrite, nullable) CALayer *glLayer;
@end

@interface HVMPhotoPickerRequest ()
- (instancetype)initWithActive:(BOOL)active requestId:(uint64_t)requestId;
@end

@interface HVMDocumentPickerRequest ()
- (instancetype)initWithRequest:(const RT::UIKit::DocumentPickerRequest &)q;
@end

@interface HVMMovieRequest ()
- (instancetype)initWithActive:(BOOL)active requestId:(uint64_t)requestId path:(NSString *)path;
@end

@interface HVMTextInputRequest ()
- (instancetype)initWithRequest:(const RT::UIKit::TextInputRequest &)q;
@end

@implementation HVMSession {
    NSString *_bundlePath;
    NSString *_homePath;
    std::shared_ptr<SessionCore> _core;
}

- (instancetype)initWithBundlePath:(NSString *)bundlePath homePath:(NSString *)homePath {
    if ((self = [super init])) {
        _bundlePath = [bundlePath copy];
        _homePath = [homePath copy];
        _core = std::make_shared<SessionCore>();
        _screenSize = CGSizeMake(320, 480);
        _interfaceOrientation = 1;
        _useGPU = YES;
    }
    return self;
}

- (void)dealloc {
    // Let a game that's still open save before it goes (the engine stops it after a few seconds at most).
    [self stopGracefully];
}

- (void)start {
    if (_running) return;
    _running = YES;
    auto core = _core;
    __weak HVMSession *weakSelf = self;
    const std::string bundle = _bundlePath.UTF8String, home = _homePath.UTF8String;
    const bool keepGoing = _keepGoing;
    const bool useGPU = _useGPU;
    const bool playSession = _playSession;
    const bool glTrace = _glTrace;

    auto flushLogs = [core, weakSelf]() {
        std::vector<std::pair<std::string, RT::LogLevel>> batch;
        {
            std::lock_guard<std::mutex> lk(core->logMu);
            batch.swap(core->logs);
            core->logFlushScheduled = false;
        }
        HVMSession *s = weakSelf;
        if (!s || !s.onLog) return;
        for (auto &[line, lvl] : batch) s.onLog(str(line), HVMLogLevel(int(lvl)));
    };

    core->logFile = openRunLog(bundle, playSession);

    spawnBigStack(^{
        RT::ProcessConfig cfg;
        cfg.bundlePath = bundle;
        cfg.homePath = home;
        // Debug: keep freed Objective-C objects and name them when messaged (Library menu toggle).
        cfg.enableZombies = [[NSUserDefaults standardUserDefaults] boolForKey:@"hvm.zombies"];
        cfg.stopOnMissingFunction = !keepGoing;
        // Offline protocol lives inside the host process; no Python or remote
        // server is needed. Process::load verifies the exact executable hash.
        // An unreadable Info.plist is Process::load's error to report, not this thread's.
        try {
            auto offlineInfo = Hydrogen::Plist::parseFile(bundle + "/Info.plist");
            cfg.offlineClash = offlineInfo && offlineInfo->getString("CFBundleIdentifier") == "com.supercell.scroll" &&
                offlineInfo->getString("CFBundleShortVersionString") == "2.1306.1";
        } catch (...) {
            cfg.offlineClash = false;
        }
        cfg.glTrace = glTrace;
        if (useGPU) {
            // GLES runs on the GPU through OpenGLES.framework; the host layer is
            // handed to the player view, which puts it under the UIKit overlay.
            cfg.hostGL = Hydrogen::App::makeEaglPlatform([weakSelf](CALayer *layer) {
                HVMSession *s = weakSelf;
                if (!s) return;
                s.glLayer = layer;
                if (s.onGLLayer) s.onGLLayer(layer);
            });
        }
        cfg.screenWidth = 320;
        cfg.screenHeight = 568;   // UIKit drops to 480 unless the app opts into 4"
        cfg.screenScale = 2;
        auto sessionLog = [core, flushLogs](RT::LogLevel lvl, const std::string &msg) {
            std::lock_guard<std::mutex> lk(core->logMu);
            core->logToFile("", msg);
            if (core->logs.size() < 5000) core->logs.emplace_back(msg, lvl);
            if (!core->logFlushScheduled) {
                core->logFlushScheduled = true;
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ flushLogs(); });
            }
        };
        cfg.log = sessionLog;
        // Process::log already records runtime lines. Only direct bridge logs
        // need forwarding here, otherwise every runtime line would be doubled.
        auto hostLog = [sessionLog](RT::LogLevel lvl, const std::string &msg) {
            Hydrogen::CPU::Engine::recordLogLineAsyncSafe(msg.data(), msg.size());
            sessionLog(lvl, msg);
        };
        cfg.microphone = [core](bool on) { setMicrophone(core, on); };
        cfg.present = [core, weakSelf](const uint8_t *rgba, int w, int h) {
            if (core->framePending.exchange(true)) return;   // the UI hasn't taken the last one: drop
            // Read UIKit only on this guest render thread. Capture metadata with
            // the pixels, rather than reading mutable state from the host queue.
            std::shared_ptr<RT::Process> p;
            { std::lock_guard<std::mutex> lk(core->mu); p = core->proc; }
            const NSInteger orientation = p ? RT::UIKit::ui(*p).statusBarOrientation : 1;
            const CGSize size = CGSizeMake(float(w) / 2, float(h) / 2);
            NSData *pixels = [NSData dataWithBytes:rgba length:size_t(w) * size_t(h) * 4];
            CGDataProviderRef dp = CGDataProviderCreateWithCFData((__bridge CFDataRef)pixels);
            CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
            CGImageRef img = CGImageCreate(size_t(w), size_t(h), 8, 32, size_t(w) * 4, cs,
                                           kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big, dp, nullptr, false, kCGRenderingIntentDefault);
            CGColorSpaceRelease(cs);
            CGDataProviderRelease(dp);
            dispatch_async(dispatch_get_main_queue(), ^{
                core->framePending = false;
                HVMSession *s = weakSelf;
                if (s) {
                    s.screenSize = size;
                    s.interfaceOrientation = orientation;
                    if (s.onFrame && img) s.onFrame(img);
                }
                if (img) CGImageRelease(img);
            });
        };
        int code = -1;
        std::string crash, bundleJSON;
        std::shared_ptr<RT::Process> proc;
        try {
            proc = std::make_shared<RT::Process>(std::move(cfg));
            {
                std::lock_guard<std::mutex> lk(core->mu);
                core->proc = proc;
            }
            RT::UIKit::setTextInputObserver(*proc, [weakSelf](const RT::UIKit::TextInputRequest &q) {
                HVMTextInputRequest *req = [[HVMTextInputRequest alloc] initWithRequest:q];
                dispatch_async(dispatch_get_main_queue(), ^{
                    HVMSession *s = weakSelf;
                    if (s && s.onTextInput) s.onTextInput(req);
                });
            });
            RT::UIKit::setDocumentPickerObserver(*proc, [weakSelf](const RT::UIKit::DocumentPickerRequest &q) {
                HVMDocumentPickerRequest *req = [[HVMDocumentPickerRequest alloc] initWithRequest:q];
                dispatch_async(dispatch_get_main_queue(), ^{
                    HVMSession *s = weakSelf;
                    if (s && s.onDocumentPicker) s.onDocumentPicker(req);
                });
            });
            RT::UIKit::setPhotoPickerObserver(*proc, [weakSelf](const RT::UIKit::PhotoPickerRequest &q) {
                HVMPhotoPickerRequest *req = [[HVMPhotoPickerRequest alloc] initWithActive:q.active requestId:q.requestId];
                dispatch_async(dispatch_get_main_queue(), ^{
                    HVMSession *s = weakSelf;
                    if (s && s.onPhotoPicker) s.onPhotoPicker(req);
                });
            });
            RT::UIKit::setMovieObserver(*proc, [weakSelf](const RT::UIKit::MovieRequest &q) {
                NSString *path = [NSString stringWithUTF8String:q.hostPath.c_str()] ?: @"";
                HVMMovieRequest *req = [[HVMMovieRequest alloc] initWithActive:q.active requestId:q.requestId path:path];
                dispatch_async(dispatch_get_main_queue(), ^{
                    HVMSession *s = weakSelf;
                    if (s && s.onMovie) s.onMovie(req);
                    // Nobody to show it (a batch test run): the movie is over at once.
                    else if (s && req.active) [s movieFinished:req.requestId];
                });
            });
            startAudio(core, proc->mixerPtr());
            if (!core->stopRequested) {
                proc->load();
                core->ready = true;
                if (core->stopRequested) proc->requestStop();
                hostLog(RT::LogLevel::Info, "[teardown] waiting for the guest run to finish");
                code = proc->run();
                hostLog(RT::LogLevel::Info, "[teardown] guest run returned status " + std::to_string(code));
                if (proc->crashed()) {
                    crash = proc->crashReport().empty() ? "The app crashed." : proc->crashReport();
                    bundleJSON = proc->crashBundle();
                }
            }
            core->ready = false;
            {
                std::lock_guard<std::mutex> lk(core->mu);
                core->proc.reset();
            }
            hostLog(RT::LogLevel::Info, "[teardown] destroying the guest process");
            proc.reset();
            hostLog(RT::LogLevel::Info, "[teardown] guest process destroyed; live arenas " +
                                            std::to_string(Hydrogen::Memory::GuestArena::liveCount()));
        } catch (const std::exception &e) {
            core->ready = false;
            crash = std::string("Could not start the app: ") + e.what();
            {
                std::lock_guard<std::mutex> lk(core->mu);
                core->proc.reset();
            }
            hostLog(RT::LogLevel::Info, "[teardown] destroying the guest process after an error");
            proc.reset();
            hostLog(RT::LogLevel::Info, "[teardown] guest process destroyed after an error; live arenas " +
                                            std::to_string(Hydrogen::Memory::GuestArena::liveCount()));
        }
        hostLog(RT::LogLevel::Info, "[teardown] stopping host audio");
        stopAudio(core);
        hostLog(RT::LogLevel::Info, "[teardown] host audio stopped; queueing the exit callback");
        const bool userStopped = core->stopRequested;
        {
            std::lock_guard<std::mutex> lk(core->logMu);
            core->logToFile("\n[exit] ", "status " + std::to_string(code) + (userStopped ? " (stopped by user)" : ""));
            if (!crash.empty()) core->logToFile("[crash]\n", crash);
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            hostLog(RT::LogLevel::Info, "[teardown] delivering the exit callback");
            flushLogs();
            HVMSession *s = weakSelf;
            if (!s) return;
            s.running = NO;
            s.crashBundle = bundleJSON.empty() ? nil : str(bundleJSON);
            if (s.onExit) s.onExit(code, crash.empty() ? nil : str(crash));
        });
    });
}

- (void)stop {
    _core->stopRequested = true;
    std::shared_ptr<RT::Process> p;
    {
        std::lock_guard<std::mutex> lk(_core->mu);
        p = _core->proc;
    }
    if (p && _core->ready) p->requestStop();
}

- (void)stopGracefully {
    _core->stopRequested = true;   // the user closed it: no crash alert for the stop itself
    std::shared_ptr<RT::Process> p;
    {
        std::lock_guard<std::mutex> lk(_core->mu);
        p = _core->proc;
    }
    if (p && _core->ready) p->requestGracefulStop();
    else if (p) p->requestStop();
}

- (void)enterBackground {
    std::shared_ptr<RT::Process> p;
    {
        std::lock_guard<std::mutex> lk(_core->mu);
        p = _core->proc;
    }
    if (p && _core->ready) p->enterBackground();
}

- (BOOL)isBackgrounded {
    std::shared_ptr<RT::Process> p;
    { std::lock_guard<std::mutex> lk(_core->mu); p = _core->proc; }
    return p && _core->ready && p->backgrounded();
}

- (double)frameTimeP95Milliseconds {
    std::shared_ptr<RT::Process> p;
    { std::lock_guard<std::mutex> lk(_core->mu); p = _core->proc; }
    return p ? double(p->perf().frameTimes.percentile(95)) / 1e6 : 0;
}

- (NSDictionary<NSString *, NSNumber *> *)perfDetail {
    std::shared_ptr<RT::Process> p;
    { std::lock_guard<std::mutex> lk(_core->mu); p = _core->proc; }
    if (!p) return @{};
    auto& pf = p->perf();
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - pf.origin).count();
    const uint64_t frames = pf.frames.load(), insns = p->guestInstructions();
    return @{
        @"wallSeconds": @(wall), @"guestMIPS": @(wall > 0 ? double(insns) / 1e6 / wall : 0),
        @"frames": @(frames), @"framesPresented": @(pf.framesPresented.load()),
        @"avgFrameMs": @(frames ? double(pf.frameNs.load()) / 1e6 / double(frames) : 0),
        @"p50FrameMs": @(double(pf.frameTimes.percentile(50)) / 1e6),
        @"p95FrameMs": @(double(pf.frameTimes.percentile(95)) / 1e6),
        @"p99FrameMs": @(double(pf.frameTimes.percentile(99)) / 1e6),
        @"maxFrameMs": @(double(pf.maxFrameNs.load()) / 1e6), @"slowFrames": @(pf.slowFrames.load()),
        @"compositeMs": @(double(pf.compositeNs.load()) / 1e6), @"presentWaitMs": @(double(pf.presentWaitNs.load()) / 1e6),
    };
}

- (NSInteger)interfaceOrientation {
    std::shared_ptr<RT::Process> p;
    { std::lock_guard<std::mutex> lk(_core->mu); p = _core->proc; }
    // UIKit state exists once a frame has been shown; before that keep the captured value.
    if (p && p->perf().framesPresented.load() > 0) {
        const int32_t o = RT::UIKit::ui(*p).statusBarOrientation;
        if (o >= 1 && o <= 4) return o;
    }
    return _interfaceOrientation;
}

+ (NSString *)emulatorCommit {
#ifdef HVM_COMMIT
    return @HVM_COMMIT;
#else
    return @"unknown";
#endif
}

- (NSDictionary<NSString *, NSNumber *> *)diagnostics {
    std::shared_ptr<RT::Process> p;
    { std::lock_guard<std::mutex> lk(_core->mu); p = _core->proc; }
    if (!p) return @{};
    const auto ms = p->mixer().stats();
    auto& pf = p->perf();
    const uint64_t ttff = pf.timeToFirstFrameNs.load();
    return @{
        @"audioFramesRendered": @(ms.framesRendered), @"audibleFrames": @(ms.audibleFrames),
        @"musicFrames": @(ms.musicFrames), @"effectFrames": @(ms.effectFrames),
        @"musicVoices": @(ms.musicVoices), @"effectVoices": @(ms.effectVoices), @"audioPeak": @(ms.peak),
        @"offscreenPresents": @(pf.offscreenPresents.load()), @"faultsToApp": @(pf.faultsToApp.load()),
        @"missingCount": @(p->missingCount()), @"framesPresented": @(pf.framesPresented.load()),
        @"timeToFirstFrameMs": @(ttff == ~uint64_t{0} ? -1.0 : double(ttff) / 1e6),
        @"glErrors": @(pf.glErrors.load()), @"glDraws": @(pf.glDraws.load()),
        @"glPresents": @(pf.glPresents.load()), @"glEmptyPresents": @(pf.glEmptyPresents.load()), @"glLastFrameDraws": @(pf.glLastFrameDraws.load()),
    };
}

- (NSArray<NSString *> *)glErrorLog {
    std::shared_ptr<RT::Process> p;
    { std::lock_guard<std::mutex> lk(_core->mu); p = _core->proc; }
    if (!p) return @[];
    NSMutableArray<NSString *> *out = [NSMutableArray array];
    for (const auto& line : p->glErrorLog()) [out addObject:str(line)];
    return out;
}

- (uint64_t)framesPresented {
    std::shared_ptr<RT::Process> p;
    { std::lock_guard<std::mutex> lk(_core->mu); p = _core->proc; }
    return p ? p->perf().framesPresented.load() : 0;
}

- (float)outputVolume { return _core->outputVolume.load(); }
- (void)setOutputVolume:(float)value {
    if (!std::isfinite(value)) return;
    value = std::clamp(value, 0.0f, 1.0f);
    std::lock_guard<std::mutex> lk(_core->audioMu);
    _core->outputVolume = value;
    _core->audioEngine.mainMixerNode.outputVolume = value;
}

- (void)enterForeground {
    std::shared_ptr<RT::Process> p;
    {
        std::lock_guard<std::mutex> lk(_core->mu);
        p = _core->proc;
    }
    if (p && _core->ready) p->enterForeground();
    // Sound the system stopped while the app was away starts again.
    std::shared_ptr<SessionCore> core = _core;
    dispatch_async(dispatch_get_main_queue(), ^{ resumeAudio(core); });
}

- (void)submitDocument:(NSString *)sourcePath requestId:(uint64_t)requestId {
    if (!_core->ready) return;
    std::shared_ptr<RT::Process> p;
    { std::lock_guard<std::mutex> lock(_core->mu); p = _core->proc; }
    if (p) RT::UIKit::submitDocumentPickerResult(*p, requestId, sourcePath ? sourcePath.UTF8String : "");
}

- (void)submitPhoto:(NSData *)imageData requestId:(uint64_t)requestId {
    if (!_core->ready) return;
    std::shared_ptr<RT::Process> p;
    { std::lock_guard<std::mutex> lk(_core->mu); p = _core->proc; }
    if (!p) return;
    std::vector<uint8_t> data;
    if (imageData && imageData.length <= 16u * 1024 * 1024) {
        const auto *bytes = static_cast<const uint8_t *>(imageData.bytes);
        data.assign(bytes, bytes + imageData.length);
    }
    RT::UIKit::submitPhotoPickerResult(*p, requestId, std::move(data));
}

- (void)movieFinished:(uint64_t)requestId {
    if (!_core->ready) return;
    std::shared_ptr<RT::Process> p;
    { std::lock_guard<std::mutex> lk(_core->mu); p = _core->proc; }
    if (!p) return;
    RT::UIKit::movieFinished(*p, requestId);
}

- (void)submitTextInput:(RT::UIKit::TextInputEvent)ev {
    if (!_core->ready) return;
    std::shared_ptr<RT::Process> p;
    { std::lock_guard<std::mutex> lk(_core->mu); p = _core->proc; }
    if (p) RT::UIKit::submitTextInput(*p, std::move(ev));
}
- (void)submitText:(NSString *)text focusId:(uint64_t)focusId sequence:(uint64_t)sequence {
    RT::UIKit::TextInputEvent ev{RT::UIKit::TextInputEvent::SetText, std::string(text.UTF8String ?: ""), focusId, sequence};
    [self submitTextInput:ev];
}
- (void)submitReturnKeyForFocus:(uint64_t)focusId {
    RT::UIKit::TextInputEvent ev{RT::UIKit::TextInputEvent::Return, std::string(), focusId, 0};
    [self submitTextInput:ev];
}
- (void)insertText:(NSString *)text focusId:(uint64_t)focusId {
    RT::UIKit::TextInputEvent ev{RT::UIKit::TextInputEvent::InsertText, std::string(text.UTF8String ?: ""), focusId, 0};
    [self submitTextInput:ev];
}
- (void)deleteBackwardForFocus:(uint64_t)focusId {
    RT::UIKit::TextInputEvent ev{RT::UIKit::TextInputEvent::DeleteBackward, std::string(), focusId, 0};
    [self submitTextInput:ev];
}
- (void)dismissKeyboardForFocus:(uint64_t)focusId {
    RT::UIKit::TextInputEvent ev{RT::UIKit::TextInputEvent::Dismiss, std::string(), focusId, 0};
    [self submitTextInput:ev];
}

- (void)touch:(HVMTouchPhase)phase x:(float)x y:(float)y finger:(NSUInteger)finger {
    if (!_core->ready) return;
    std::shared_ptr<RT::Process> p;
    {
        std::lock_guard<std::mutex> lk(_core->mu);
        p = _core->proc;
    }
    if (p) p->injectTouch(RT::TouchPhase(int(phase)), x, y, uint32_t(finger));
}


- (void)setAccelerationX:(double)x y:(double)y z:(double)z {
    if (!_core->ready) return;
    std::shared_ptr<RT::Process> p;
    {
        std::lock_guard<std::mutex> lk(_core->mu);
        p = _core->proc;
    }
    if (p) p->setAcceleration(float(x), float(y), float(z));
}

@end

// --------------------------------------------------------------- engine ---

@implementation HVMDocumentPickerRequest
- (instancetype)initWithRequest:(const RT::UIKit::DocumentPickerRequest &)q {
    if ((self = [super initWithActive:q.active requestId:q.requestId])) {
        NSMutableArray<NSString *> *types = [NSMutableArray array];
        for (const auto &type : q.types) [types addObject:[NSString stringWithUTF8String:type.c_str()]];
        _types = [types copy];
    }
    return self;
}
@end

@implementation HVMPhotoPickerRequest
- (instancetype)initWithActive:(BOOL)active requestId:(uint64_t)requestId {
    if ((self = [super init])) { _active = active; _requestId = requestId; }
    return self;
}
@end

@implementation HVMMovieRequest
- (instancetype)initWithActive:(BOOL)active requestId:(uint64_t)requestId path:(NSString *)path {
    if ((self = [super init])) { _active = active; _requestId = requestId; _path = [path copy]; }
    return self;
}
@end

@implementation HVMTextInputRequest
- (instancetype)initWithRequest:(const RT::UIKit::TextInputRequest &)q {
    if ((self = [super init])) {
        _active = q.active;
        _focusId = q.focusId; _ackSeq = q.ackSeq;
        _text = str(q.text);
        _placeholder = str(q.placeholder);
        _keyboardType = q.keyboardType; _returnKeyType = q.returnKeyType;
        _autocapitalizationType = q.autocapitalization; _autocorrectionType = q.autocorrection;
        _secure = q.secure; _multiline = q.multiline;
        _keyInput = q.keyInput; _hasText = q.hasText;
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
        if (error) *error = str(e.what());
        return NO;
    }
}

+ (NSUInteger)liveGuestArenaCount {
    return Hydrogen::Memory::GuestArena::liveCount();
}

+ (BOOL)waitForGuestArenaRelease:(NSTimeInterval)timeout {
    return Hydrogen::Memory::GuestArena::waitForRelease(timeout, 0);
}

+ (BOOL)waitForGuestArenaRelease:(NSTimeInterval)timeout targetCount:(NSUInteger)targetCount {
    return Hydrogen::Memory::GuestArena::waitForRelease(timeout, targetCount);
}

+ (HVMSelfTestResult *)runSelfTest {
    return [[HVMSelfTestResult alloc] initWithResult:Hydrogen::runSelfTest()];
}

+ (HVMAppInfo *)importIPA:(NSString *)ipaPath intoDirectory:(NSString *)appsRoot error:(NSError **)error {
    try {
        auto info = Hydrogen::Loader::importIpa(ipaPath.UTF8String, appsRoot.UTF8String);
        return [[HVMAppInfo alloc] initWithInfo:info];
    } catch (const std::exception &e) {
        if (error)
            *error = [NSError errorWithDomain:@"HydrogenVM" code:1 userInfo:@{NSLocalizedDescriptionKey: str(e.what())}];
        return nil;
    }
}

+ (HVMAppInfo *)appInfoAtPath:(NSString *)appPath {
    try {
        return [[HVMAppInfo alloc] initWithInfo:Hydrogen::Loader::readBundle(appPath.UTF8String)];
    } catch (...) {
        return nil;
    }
}

+ (CGImageRef)decodeImageAtPath:(NSString *)path {
    NSData *d = [NSData dataWithContentsOfFile:path];
    if (!d.length) return nullptr;
    auto img = G::decodeImage(static_cast<const uint8_t *>(d.bytes), d.length);
    if (!img || img->width <= 0 || img->height <= 0) return nullptr;
    NSData *pixels = [NSData dataWithBytes:img->rgba.data() length:img->rgba.size()];
    CGDataProviderRef dp = CGDataProviderCreateWithCFData((__bridge CFDataRef)pixels);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGImageRef out = CGImageCreate(size_t(img->width), size_t(img->height), 8, 32, size_t(img->width) * 4, cs,
                                   kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big, dp, nullptr, true, kCGRenderingIntentDefault);
    CGColorSpaceRelease(cs);
    CGDataProviderRelease(dp);
    return out;
}

+ (void)installPlatformServices {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        G::setTextBackend(std::make_shared<CoreTextBackend>());
        G::setPlatformImageDecoder(&decodeWithImageIO);
    });
}

@end
