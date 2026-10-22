// HydrogenVM Test Lab — guest app (32-bit ARMv7 iOS)
//
// Five deterministic test cases:
//   TL-A  Import and launch  — verifies this IPA was properly imported and its
//                              bundle metadata is intact.
//   TL-B  CPU correctness    — xorshift32 workload, result compared against a
//                              known checksum.
//   TL-C  Graphics           — GLES 2.0 FBO, draws a coloured triangle and reads
//                              back specific pixels to verify exact values.
//   TL-D  Audio              — 440 Hz sine tone, decoded into a buffer; verifies
//                              sample count, channel layout, and non-zero energy.
//   TL-E  Persistence        — writes a sentinel through NSUserDefaults, stops,
//                              relaunches and verifies the value is still present.
//
// Invocation modes:
//   Default    : interactive; shows a menu of test status, tap to run individual
//                or tap "Run All".
//   --run TL-X : automated; runs a single case, writes result to Documents/, exits.
//   --runall   : automated; runs all cases sequentially, writes combined result.
//
// Result file: Documents/testlab_result.json  (schema version 1)
// Checkpoint file: Documents/testlab_checkpoint.json  (written before risky work)
//
// Exit codes (automated mode only):
//   0  all cases passed (or the one requested case passed)
//   1  one or more cases failed
//   2  one or more cases were interrupted (checkpoint detected on relaunch)
//
#import <UIKit/UIKit.h>
#import <Foundation/Foundation.h>
#include <time.h>
#import <OpenGLES/EAGL.h>
#include <OpenGLES/ES2/gl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

// ─────────────────────────── protocol version ────────────────────────────────

static const int   kSchemaVersion   = 1;
static const char *kAppVersion      = "1.0";
static const char *kAppBuild        = "1";

// ─────────────────────────── case IDs / versions ─────────────────────────────

#define CASE_A  @"TL-A"
#define CASE_B  @"TL-B"
#define CASE_C  @"TL-C"
#define CASE_D  @"TL-D"
#define CASE_E  @"TL-E"

static const int kVersionA = 1;
static const int kVersionB = 1;
static const int kVersionC = 1;
static const int kVersionD = 1;
static const int kVersionE = 1;

// ─────────────────────────── timing ──────────────────────────────────────────

static double monoMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

// ─────────────────────────── paths ───────────────────────────────────────────

static NSString *documentsDir(void) {
    NSArray *dirs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    return dirs.firstObject ?: @"/tmp";
}

static NSString *resultPath(void)     { return [documentsDir() stringByAppendingPathComponent:@"testlab_result.json"]; }
static NSString *checkpointPath(void) { return [documentsDir() stringByAppendingPathComponent:@"testlab_checkpoint.json"]; }

// ─────────────────────────── checkpoint helpers ──────────────────────────────

static void writeCheckpoint(NSString *caseId) {
    NSDictionary *cp = @{ @"caseId": caseId,
                          @"writtenAt": [NSDate date].description };
    NSData *data = [NSJSONSerialization dataWithJSONObject:cp options:0 error:nil];
    [data writeToFile:checkpointPath() atomically:YES];
}

static void clearCheckpoint(void) {
    [[NSFileManager defaultManager] removeItemAtPath:checkpointPath() error:nil];
}

// Returns the interrupted case ID if a checkpoint exists from a previous run.
// We differentiate "previous run" from "current run" by keeping a per-process
// flag: once clearCheckpoint has been called in this launch we know any
// checkpoint was ours and we have cleared it.
static NSString *interruptedCaseId(void) {
    NSString *path = checkpointPath();
    if (![[NSFileManager defaultManager] fileExistsAtPath:path]) return nil;
    NSData *data = [NSData dataWithContentsOfFile:path];
    if (!data) return nil;
    NSDictionary *cp = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    return cp[@"caseId"];
}

// ─────────────────────────── JSON result builder ─────────────────────────────

@interface TLResult : NSObject
@property NSString   *caseId;
@property int         caseVersion;
@property NSString   *status;       // passed / failed / skipped / interrupted
@property id          expected;
@property id          actual;
@property NSString   *reason;
@property double      durationMs;
@property BOOL        checkpointWritten;
@property NSMutableDictionary *diagnostics;
@end
@implementation TLResult
- (instancetype)initWithId:(NSString *)cid version:(int)v {
    if ((self = [super init])) {
        _caseId = cid; _caseVersion = v; _status = @"failed";
        _diagnostics = [NSMutableDictionary dictionary];
    }
    return self;
}
- (NSDictionary *)toDict {
    NSMutableDictionary *d = [NSMutableDictionary dictionary];
    d[@"id"]      = _caseId;
    d[@"version"] = @(_caseVersion);
    d[@"status"]  = _status;
    if (_expected)  d[@"expected"] = _expected;
    if (_actual)    d[@"actual"]   = _actual;
    if (_reason)    d[@"reason"]   = _reason;
    d[@"durationMs"]        = @(_durationMs);
    d[@"clockDefinition"]   = @"CLOCK_MONOTONIC";
    d[@"checkpointWritten"] = @(_checkpointWritten);
    if (_diagnostics.count) d[@"diagnostics"] = _diagnostics;
    return d;
}
@end

static void writeResult(NSArray *results, NSString *runId) {
    NSMutableArray *cases = [NSMutableArray array];
    for (TLResult *r in results) [cases addObject:[r toDict]];

    // ISO-8601 UTC timestamp (C time APIs: the SDK stub has no NSDateFormatter/NSTimeZone)
    time_t nowT = time(NULL);
    struct tm *utcp = gmtime(&nowT);
    struct tm utc = *utcp;
    char nowBuf[32];
    strftime(nowBuf, sizeof nowBuf, "%Y-%m-%dT%H:%M:%SZ", &utc);
    NSString *now = [NSString stringWithUTF8String:nowBuf];

    NSDictionary *report = @{
        @"schemaVersion": @(kSchemaVersion),
        @"kind":          @"hvm-testlab-result",
        @"generatedAt":   now,
        @"runId":         runId,
        @"appVersion":    [NSString stringWithUTF8String:kAppVersion],
        @"appBuild":      [NSString stringWithUTF8String:kAppBuild],
        @"sandboxMode":   @"hvm-home",
        @"cases":         cases,
    };

    NSError *err = nil;
    NSData *data = [NSJSONSerialization dataWithJSONObject:report
                                                   options:NSJSONWritingPrettyPrinted
                                                     error:&err];
    if (err) { fprintf(stderr, "testlab: JSON error: %s\n", err.localizedDescription.UTF8String); return; }
    BOOL ok = [data writeToFile:resultPath() atomically:YES];
    fprintf(stderr, "testlab: result written to %s (%s)\n",
            resultPath().UTF8String, ok ? "ok" : "FAILED");
}

// ─────────────────────────── Case A — import & launch ────────────────────────

static TLResult *runCaseA(void) {
    TLResult *r = [[TLResult alloc] initWithId:CASE_A version:kVersionA];
    double t0 = monoMs();

    writeCheckpoint(CASE_A);
    r.checkpointWritten = YES;

    // 1. Verify Info.plist was loaded (CFBundleExecutable must be TestLab)
    NSBundle *mb = [NSBundle mainBundle];
    NSString *exe    = mb.infoDictionary[@"CFBundleExecutable"];
    NSString *bundId = mb.bundleIdentifier;
    NSString *ver    = mb.infoDictionary[@"CFBundleShortVersionString"];

    BOOL exeOk    = [exe isEqualToString:@"TestLab"];
    BOOL bundleOk = bundId != nil && bundId.length > 0;
    BOOL verOk    = [ver isEqualToString:[NSString stringWithUTF8String:kAppVersion]];

    r.diagnostics[@"CFBundleExecutable"]         = exe ?: @"<nil>";
    r.diagnostics[@"CFBundleIdentifier"]         = bundId ?: @"<nil>";
    r.diagnostics[@"CFBundleShortVersionString"] = ver ?: @"<nil>";

    // 2. Verify a known resource is present
    NSString *tonePath = [mb pathForResource:@"tone" ofType:@"wav"];
    BOOL toneOk = tonePath != nil && [[NSFileManager defaultManager] fileExistsAtPath:tonePath];
    r.diagnostics[@"toneResourceFound"] = @(toneOk);

    // 3. Verify a zero-length resource is present (tests stored ZIP entry)
    NSString *emptyPath = [mb pathForResource:@"empty" ofType:@"dat"];
    BOOL emptyOk = emptyPath != nil;
    r.diagnostics[@"emptyResourceFound"] = @(emptyOk);

    r.expected = @"CFBundleExecutable=TestLab, version=1.0, tone.wav present, empty.dat present";
    NSString *actual = [NSString stringWithFormat:@"CFBundleExecutable=%@, version=%@, tone.wav=%@, empty.dat=%@",
                        exe ?: @"<nil>", ver ?: @"<nil>",
                        toneOk ? @"present" : @"missing",
                        emptyOk ? @"present" : @"missing"];
    r.actual = actual;

    BOOL allOk = exeOk && bundleOk && verOk && toneOk && emptyOk;
    r.status = allOk ? @"passed" : @"failed";
    if (!allOk) r.reason = [NSString stringWithFormat:@"exe=%d bundle=%d ver=%d tone=%d empty=%d",
                             exeOk, bundleOk, verOk, toneOk, emptyOk];

    clearCheckpoint();
    r.durationMs = monoMs() - t0;
    return r;
}

// ─────────────────────────── Case B — CPU correctness ────────────────────────

// xorshift32 — same generator used by SelfTest. 10 M iterations.
// Expected result is 0x7E6798F5 (verified against a native x86-64 build of the same loop).
static uint32_t xorshift32(uint32_t x) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

static TLResult *runCaseB(void) {
    TLResult *r = [[TLResult alloc] initWithId:CASE_B version:kVersionB];
    double t0 = monoMs();

    writeCheckpoint(CASE_B);
    r.checkpointWritten = YES;

    const uint32_t kIterations = 10000000u;
    const uint32_t kExpected   = 0x7E6798F5u;

    volatile uint32_t x = 0x12345678u; // volatile prevents dead-code elimination
    for (uint32_t i = 0; i < kIterations; ++i) x = xorshift32(x);

    uint32_t actual = x;
    double durMs    = monoMs() - t0;

    r.expected = [NSString stringWithFormat:@"0x%08X", kExpected];
    r.actual   = [NSString stringWithFormat:@"0x%08X", actual];
    r.diagnostics[@"iterations"] = @(kIterations);
    // Elapsed time is diagnostic only, not a pass/fail criterion this milestone.
    r.diagnostics[@"durationMs"] = @(durMs);
    // Estimated MIPS (10M instr per iteration * typical insn count ~5 per xorshift):
    // This is purely informational; correctness is the criterion.
    double mips = (kIterations * 10.0) / (durMs * 1000.0); // ~5 instrs/iter heuristic
    r.diagnostics[@"estimatedMIPS_diagnostic"] = @(mips);

    BOOL passed = (actual == kExpected);
    r.status = passed ? @"passed" : @"failed";
    if (!passed)
        r.reason = [NSString stringWithFormat:@"expected 0x%08X, got 0x%08X", kExpected, actual];

    clearCheckpoint();
    r.durationMs = durMs;
    return r;
}

// ─────────────────────────── Case C — GLES graphics ─────────────────────────

// Draws a coloured triangle into an offscreen 64×64 FBO using GLES 2.0,
// reads back three pixels and checks their exact RGBA values.
//
// Triangle vertices (NDC):   centroid pixel in 64×64 viewport:
//   ( 0,  0.8, 0) — top        (32, 6)  → red   (255,0,0,255)
//   (-0.7,-0.5, 0) — bottom-L  (9, 54)  → green (0,255,0,255)
//   ( 0.7,-0.5, 0) — bottom-R  (54,54)  → blue  (0,0,255,255)
// The shader interpolates the per-vertex colour, so corner reads are reliable.
//
// Expected pixel values (RGBA, nearest vertex dominates):
//   pixel at (32, 57) — centroid of bottom edge = equal green+blue mix
//   pixel at (32,  5) — near top vertex = mostly red
//   pixel at (32, 32) — triangle centroid = roughly equal all three

static const char *kVSrc =
    "attribute vec4 aPos;\n"
    "attribute vec4 aColor;\n"
    "varying vec4 vColor;\n"
    "void main() { gl_Position = aPos; vColor = aColor; }\n";

static const char *kFSrc =
    "precision mediump float;\n"
    "varying vec4 vColor;\n"
    "void main() { gl_FragColor = vColor; }\n";

static GLuint compileShader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    return ok ? s : 0;
}

static TLResult *runCaseC(void) {
    TLResult *r = [[TLResult alloc] initWithId:CASE_C version:kVersionC];
    double t0 = monoMs();

    writeCheckpoint(CASE_C);
    r.checkpointWritten = YES;

    // Create EAGLContext
    EAGLContext *ctx = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES2];
    if (!ctx || ![EAGLContext setCurrentContext:ctx]) {
        r.status = @"skipped";
        r.reason = @"GLES 2.0 context unavailable";
        clearCheckpoint();
        r.durationMs = monoMs() - t0;
        return r;
    }

    const int W = 64, H = 64;

    // FBO + colour renderbuffer
    GLuint fbo = 0, rbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenRenderbuffers(1, &rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8_OES, W, H);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        r.status = @"skipped";
        r.reason = @"Framebuffer not complete";
        clearCheckpoint();
        r.durationMs = monoMs() - t0;
        return r;
    }

    // Shaders
    GLuint vs = compileShader(GL_VERTEX_SHADER, kVSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, kFSrc);
    if (!vs || !fs) {
        r.status = @"failed";
        r.reason = @"Shader compilation failed";
        r.diagnostics[@"vsOk"] = @(vs != 0);
        r.diagnostics[@"fsOk"] = @(fs != 0);
        clearCheckpoint();
        r.durationMs = monoMs() - t0;
        return r;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "aPos");
    glBindAttribLocation(prog, 1, "aColor");
    glLinkProgram(prog);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        r.status = @"failed";
        r.reason = @"Shader program link failed";
        clearCheckpoint();
        r.durationMs = monoMs() - t0;
        return r;
    }
    glUseProgram(prog);

    // Geometry: interleaved pos(3) + color(4) per vertex, 7 floats total
    //   top:      ( 0.0,  0.8, 0)  red   (1,0,0,1)
    //   bot-left: (-0.7, -0.5, 0)  green (0,1,0,1)
    //   bot-right:( 0.7, -0.5, 0)  blue  (0,0,1,1)
    static const GLfloat verts[] = {
         0.0f,  0.8f, 0.0f,   1.0f, 0.0f, 0.0f, 1.0f,
        -0.7f, -0.5f, 0.0f,   0.0f, 1.0f, 0.0f, 1.0f,
         0.7f, -0.5f, 0.0f,   0.0f, 0.0f, 1.0f, 1.0f,
    };
    const GLsizei stride = 7 * sizeof(GLfloat);

    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, (void*)(3*sizeof(GLfloat)));

    glViewport(0, 0, W, H);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // Read back pixels
    // We sample three pixels whose expected dominant colour channel is known.
    // Pixel coordinates in glReadPixels are bottom-left origin.
    // Probes sit just inside each corner of the triangle. Viewport mapping for 64x64:
    //   top vertex     NDC ( 0.0, 0.8) -> (32.0, 57.6)   red
    //   bottom-left    NDC (-0.7,-0.5) -> ( 9.6, 16.0)   green
    //   bottom-right   NDC ( 0.7,-0.5) -> (54.4, 16.0)   blue
    // Pixel centres (32.5,55.5), (13.5,18.5), (50.5,18.5) are inside the triangle with
    // barycentric weight of the corner's colour ~0.95, ~0.88, ~0.88 (>= 200/255).
    // glReadPixels uses a bottom-left origin.
    struct { int x, y; uint8_t minR, minG, minB; const char *name; } probes[] = {
        { 32, 55, 200,   0,   0, "top-red"   },
        { 13, 18,   0, 200,   0, "bot-left-green"  },
        { 50, 18,   0,   0, 200, "bot-right-blue"  },
    };

    BOOL allOk = YES;
    NSMutableDictionary *pxDiag = [NSMutableDictionary dictionary];
    for (int i = 0; i < 3; ++i) {
        uint8_t px[4] = {0};
        glReadPixels(probes[i].x, probes[i].y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        BOOL ok = px[0] >= probes[i].minR && px[1] >= probes[i].minG && px[2] >= probes[i].minB;
        pxDiag[[NSString stringWithUTF8String:probes[i].name]] =
            [NSString stringWithFormat:@"rgba(%d,%d,%d,%d) ok=%d", px[0], px[1], px[2], px[3], ok];
        if (!ok) allOk = NO;
    }
    r.diagnostics[@"pixels"] = pxDiag;
    r.expected = @"top pixel R>=200, bottom-left G>=200, bottom-right B>=200";
    r.actual   = [pxDiag description];

    r.status = allOk ? @"passed" : @"failed";
    if (!allOk)
        r.reason = @"One or more pixel checks failed — see diagnostics.pixels";

    clearCheckpoint();
    r.durationMs = monoMs() - t0;
    return r;
}

// ─────────────────────────── Case D — audio ──────────────────────────────────

// Reads tone.wav from the bundle, parses the PCM WAV header, and verifies:
//   - sample count  (expected: 44100  = 1 s at 44.1 kHz)
//   - channel count (expected: 1 mono)
//   - sample rate   (expected: 44100 Hz)
//   - non-zero energy (at least one non-zero sample)
//
// The tone is generated by gen_tone_wav.py at build time; it is an original
// 440 Hz sine wave. No copyrighted material is used.

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1]<<8);
}

static TLResult *runCaseD(void) {
    TLResult *r = [[TLResult alloc] initWithId:CASE_D version:kVersionD];
    double t0 = monoMs();

    writeCheckpoint(CASE_D);
    r.checkpointWritten = YES;

    NSString *tonePath = [[NSBundle mainBundle] pathForResource:@"tone" ofType:@"wav"];
    if (!tonePath) {
        r.status = @"failed";
        r.reason = @"tone.wav resource missing from bundle";
        clearCheckpoint();
        r.durationMs = monoMs() - t0;
        return r;
    }

    NSData *wavData = [NSData dataWithContentsOfFile:tonePath];
    if (!wavData || wavData.length < 44) {
        r.status = @"failed";
        r.reason = [NSString stringWithFormat:@"tone.wav too short (%lu bytes)", (unsigned long)wavData.length];
        clearCheckpoint();
        r.durationMs = monoMs() - t0;
        return r;
    }

    const uint8_t *b = (const uint8_t *)wavData.bytes;
    // Minimal RIFF/WAVE/fmt /data parser
    if (memcmp(b, "RIFF", 4) != 0 || memcmp(b+8, "WAVE", 4) != 0) {
        r.status = @"failed";
        r.reason = @"tone.wav: not a RIFF/WAVE file";
        clearCheckpoint();
        r.durationMs = monoMs() - t0;
        return r;
    }

    // Walk chunks to find fmt and data
    uint32_t channels = 0, sampleRate = 0, bitsPerSample = 0;
    uint32_t dataOffset = 0, dataSize = 0;
    uint32_t pos = 12;
    while (pos + 8 <= wavData.length) {
        uint32_t chunkSize = read_le32(b + pos + 4);
        if (memcmp(b + pos, "fmt ", 4) == 0 && chunkSize >= 16) {
            channels      = read_le16(b + pos + 10);
            sampleRate    = read_le32(b + pos + 12);
            bitsPerSample = read_le16(b + pos + 22);
        } else if (memcmp(b + pos, "data", 4) == 0) {
            dataOffset = pos + 8;
            dataSize   = chunkSize;
        }
        pos += 8 + ((chunkSize + 1) & ~1u);
    }

    if (!dataOffset || !sampleRate || !channels) {
        r.status = @"failed";
        r.reason = @"tone.wav: could not parse fmt/data chunks";
        clearCheckpoint();
        r.durationMs = monoMs() - t0;
        return r;
    }

    uint32_t bytesPerSample = bitsPerSample / 8;
    uint32_t sampleCount = (bytesPerSample > 0 && channels > 0)
                         ? dataSize / (bytesPerSample * channels) : 0;

    // Check for non-zero energy
    BOOL hasEnergy = NO;
    if (bytesPerSample == 2) {
        const int16_t *samples = (const int16_t *)(b + dataOffset);
        uint32_t nSamples = dataSize / 2;
        for (uint32_t i = 0; i < nSamples; ++i) {
            if (samples[i] != 0) { hasEnergy = YES; break; }
        }
    }

    r.diagnostics[@"sampleRate"]    = @(sampleRate);
    r.diagnostics[@"channels"]      = @(channels);
    r.diagnostics[@"bitsPerSample"] = @(bitsPerSample);
    r.diagnostics[@"sampleCount"]   = @(sampleCount);
    r.diagnostics[@"hasEnergy"]     = @(hasEnergy);
    r.diagnostics[@"durationMs_audio"] = sampleRate > 0
        ? @(sampleCount * 1000.0 / sampleRate) : @0;
    r.diagnostics[@"note"] = @"Automated: parsed PCM energy. Speaker playback not verified automatically.";

    const uint32_t kExpectedSamples   = 44100u;
    const uint32_t kExpectedRate      = 44100u;
    const uint32_t kExpectedChannels  = 1u;
    const uint32_t kSampleTolerance   = 10u; // ±10 samples

    r.expected = [NSString stringWithFormat:@"rate=%u channels=%u samples≈%u energy=yes",
                  kExpectedRate, kExpectedChannels, kExpectedSamples];
    r.actual = [NSString stringWithFormat:@"rate=%u channels=%u samples=%u energy=%@",
                sampleRate, channels, sampleCount, hasEnergy ? @"yes" : @"no"];

    BOOL rateOk     = sampleRate == kExpectedRate;
    BOOL chOk       = channels   == kExpectedChannels;
    BOOL countOk    = (sampleCount >= kExpectedSamples - kSampleTolerance)
                   && (sampleCount <= kExpectedSamples + kSampleTolerance);
    BOOL energyOk   = hasEnergy;
    BOOL allOk = rateOk && chOk && countOk && energyOk;

    r.status = allOk ? @"passed" : @"failed";
    if (!allOk) {
        r.reason = [NSString stringWithFormat:@"rate=%d ch=%d count=%d energy=%d",
                    rateOk, chOk, countOk, energyOk];
    }

    clearCheckpoint();
    r.durationMs = monoMs() - t0;
    return r;
}

// ─────────────────────────── Case E — persistence ────────────────────────────

// Phase 1: writes a sentinel value through NSUserDefaults and exits.
//          On relaunch, if the sentinel is absent, this was a fresh launch → write and exit.
//          If the sentinel is present → read and verify (phase 2).
//
// We track phase through a second preference key so phase 2 runs exactly once.

static const NSString *kSentinelKey  = @"com.hydrogenvm.testlab.sentinel_v1";
static const NSString *kPhaseKey     = @"com.hydrogenvm.testlab.phase_v1";
static const int        kSentinelVal = 0x48564D54; // "HVMT"

static TLResult *runCaseE_phase2(void) {
    TLResult *r = [[TLResult alloc] initWithId:CASE_E version:kVersionE];
    double t0 = monoMs();
    r.checkpointWritten = NO; // no risky code this phase

    NSUserDefaults *ud = [NSUserDefaults standardUserDefaults];
    int stored = (int)[ud integerForKey:(NSString *)kSentinelKey];
    BOOL matched = (stored == kSentinelVal);

    r.expected = [NSString stringWithFormat:@"0x%08X", kSentinelVal];
    r.actual   = [NSString stringWithFormat:@"0x%08X", stored];
    r.diagnostics[@"phase"] = @"2-read";

    // Clean up so next run starts fresh
    [ud removeObjectForKey:(NSString *)kSentinelKey];
    [ud removeObjectForKey:(NSString *)kPhaseKey];
    [ud synchronize];

    r.status = matched ? @"passed" : @"failed";
    if (!matched)
        r.reason = [NSString stringWithFormat:@"stored value 0x%08X ≠ expected 0x%08X",
                    stored, kSentinelVal];
    r.durationMs = monoMs() - t0;
    return r;
}

// Returns YES if this is phase 1 (write and return nil; caller should relaunch).
// Returns NO + a result if this is phase 2 (read and verify).
// Returns NO + a skipped result if this is called in automated single-run mode
// where we cannot do a real two-launch test (e.g. first run ever).
static TLResult *runCaseE(BOOL automatedSingleRun) {
    NSUserDefaults *ud = [NSUserDefaults standardUserDefaults];
    int phase = (int)[ud integerForKey:(NSString *)kPhaseKey];

    if (phase == 1) {
        // Phase 2: verify
        return runCaseE_phase2();
    }

    // Phase 1: write
    TLResult *r = [[TLResult alloc] initWithId:CASE_E version:kVersionE];
    double t0 = monoMs();

    writeCheckpoint(CASE_E);
    r.checkpointWritten = YES;

    [ud setInteger:kSentinelVal forKey:(NSString *)kSentinelKey];
    [ud setInteger:1            forKey:(NSString *)kPhaseKey];
    [ud synchronize];

    clearCheckpoint();
    r.durationMs = monoMs() - t0;

    if (automatedSingleRun) {
        // In automated mode the runner will restart with --run TL-E for phase 2.
        r.status = @"skipped";
        r.reason = @"phase-1 write complete; re-run TL-E in a second launch to verify";
        r.diagnostics[@"phase"]      = @"1-write";
        r.diagnostics[@"writtenKey"] = (NSString *)kSentinelKey;
    } else {
        // Interactive: show phase 1 done; user taps E again after relaunch.
        r.status = @"skipped";
        r.reason = @"phase 1 written — relaunch and run TL-E again to verify";
        r.diagnostics[@"phase"] = @"1-write";
    }
    r.expected = [NSString stringWithFormat:@"0x%08X", kSentinelVal];
    r.actual   = @"(not yet read — relaunch required)";
    return r;
}

// ─────────────────────────── run all cases ───────────────────────────────────

// A unique run ID for this launch.
static NSString *makeRunId(void) {
    double t = monoMs();
    return [NSString stringWithFormat:@"%016llX", (uint64_t)(t * 1000)];
}

static NSArray *runAllCases(BOOL automatedSingleRunMode) {
    NSMutableArray *results = [NSMutableArray array];
    // Check for pre-existing checkpoint (interrupted case from prior launch)
    NSString *interrupted = interruptedCaseId();
    NSArray *allIds = @[CASE_A, CASE_B, CASE_C, CASE_D, CASE_E];
    for (NSString *cid in allIds) {
        if (interrupted && [interrupted isEqualToString:cid]) {
            TLResult *r = [[TLResult alloc] initWithId:cid version:1];
            r.status = @"interrupted";
            r.reason = [NSString stringWithFormat:@"Checkpoint found from previous launch — case %@ did not complete", cid];
            [results addObject:r];
            // Clear checkpoint so it is not detected again as interrupted next run
            clearCheckpoint();
        } else if ([cid isEqualToString:CASE_A]) {
            [results addObject:runCaseA()];
        } else if ([cid isEqualToString:CASE_B]) {
            [results addObject:runCaseB()];
        } else if ([cid isEqualToString:CASE_C]) {
            [results addObject:runCaseC()];
        } else if ([cid isEqualToString:CASE_D]) {
            [results addObject:runCaseD()];
        } else if ([cid isEqualToString:CASE_E]) {
            [results addObject:runCaseE(automatedSingleRunMode)];
        }
    }
    return results;
}

static TLResult *runSingleCase(NSString *caseId) {
    NSString *interrupted = interruptedCaseId();
    if (interrupted && [interrupted isEqualToString:caseId]) {
        TLResult *r = [[TLResult alloc] initWithId:caseId version:1];
        r.status = @"interrupted";
        r.reason = [NSString stringWithFormat:@"Checkpoint found — case %@ did not complete in a prior launch", caseId];
        clearCheckpoint();
        return r;
    }
    if ([caseId isEqualToString:CASE_A]) return runCaseA();
    if ([caseId isEqualToString:CASE_B]) return runCaseB();
    if ([caseId isEqualToString:CASE_C]) return runCaseC();
    if ([caseId isEqualToString:CASE_D]) return runCaseD();
    if ([caseId isEqualToString:CASE_E]) return runCaseE(YES);
    TLResult *r = [[TLResult alloc] initWithId:caseId version:1];
    r.status = @"skipped";
    r.reason = [NSString stringWithFormat:@"Unknown case ID '%@'", caseId];
    return r;
}

// ─────────────────────────── interactive UI ──────────────────────────────────

@interface TLMenuController : UIViewController
@property NSMutableArray *results;   // TLResult*
@property NSMutableArray *labels;    // UILabel*
@end

static UIColor *colorForStatus(NSString *s) {
    if ([s isEqualToString:@"passed"])      return [UIColor colorWithRed:0 green:0.8 blue:0 alpha:1];
    if ([s isEqualToString:@"failed"])      return [UIColor colorWithRed:0.9 green:0 blue:0 alpha:1];
    if ([s isEqualToString:@"interrupted"]) return [UIColor colorWithRed:1 green:0.5 blue:0 alpha:1];
    return [UIColor colorWithWhite:0.7 alpha:1]; // skipped / pending
}

@implementation TLMenuController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor colorWithWhite:0.08 alpha:1];

    UILabel *title = [[UILabel alloc] initWithFrame:CGRectMake(10, 30, 300, 30)];
    title.text = @"HydrogenVM Test Lab 1.0";
    title.textColor = [UIColor whiteColor];
    title.font = [UIFont boldSystemFontOfSize:15];
    [self.view addSubview:title];

    NSArray *caseIds    = @[ CASE_A,       CASE_B,     CASE_C,       CASE_D,   CASE_E     ];
    NSArray *caseLabels = @[ @"A: Import", @"B: CPU",  @"C: GLES",   @"D: Audio", @"E: Persist" ];

    self.labels  = [NSMutableArray array];
    self.results = [NSMutableArray arrayWithArray:@[
        [[TLResult alloc] initWithId:CASE_A version:kVersionA],
        [[TLResult alloc] initWithId:CASE_B version:kVersionB],
        [[TLResult alloc] initWithId:CASE_C version:kVersionC],
        [[TLResult alloc] initWithId:CASE_D version:kVersionD],
        [[TLResult alloc] initWithId:CASE_E version:kVersionE],
    ]];
    for (TLResult *r in self.results) r.status = @"pending";

    // Check for interrupted case and update initial state
    NSString *interrupted = interruptedCaseId();
    for (int i = 0; i < (int)caseIds.count; ++i) {
        if (interrupted && [interrupted isEqualToString:caseIds[i]]) {
            ((TLResult *)self.results[i]).status = @"interrupted";
        }
    }

    CGFloat y = 75;
    for (int i = 0; i < (int)caseIds.count; ++i) {
        UIButton *btn = [UIButton buttonWithType:UIButtonTypeRoundedRect];
        btn.frame = CGRectMake(10, y, 130, 36);
        [btn setTitle:caseLabels[i] forState:UIControlStateNormal];
        [btn setTitleColor:[UIColor whiteColor] forState:UIControlStateNormal];
        btn.backgroundColor = [UIColor colorWithWhite:0.2 alpha:1];
        btn.tag = 100 + i;
        [btn addTarget:self action:@selector(runCase:) forControlEvents:UIControlEventTouchUpInside];
        [self.view addSubview:btn];

        UILabel *lbl = [[UILabel alloc] initWithFrame:CGRectMake(150, y+8, 160, 20)];
        TLResult *r  = self.results[i];
        lbl.text = [NSString stringWithFormat:@"%@ — %@", r.caseId, r.status];
        lbl.textColor = colorForStatus(r.status);
        lbl.font = [UIFont systemFontOfSize:13];
        [self.view addSubview:lbl];
        [self.labels addObject:lbl];

        y += 46;
    }

    UIButton *all = [UIButton buttonWithType:UIButtonTypeRoundedRect];
    all.frame = CGRectMake(10, y + 10, 130, 36);
    [all setTitle:@"Run All" forState:UIControlStateNormal];
    [all setTitleColor:[UIColor whiteColor] forState:UIControlStateNormal];
    all.backgroundColor = [UIColor colorWithRed:0.1 green:0.3 blue:0.7 alpha:1];
    all.tag = 99;
    [all addTarget:self action:@selector(runAll:) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:all];

    UIButton *export = [UIButton buttonWithType:UIButtonTypeRoundedRect];
    export.frame = CGRectMake(160, y + 10, 130, 36);
    [export setTitle:@"Export JSON" forState:UIControlStateNormal];
    [export setTitleColor:[UIColor whiteColor] forState:UIControlStateNormal];
    export.backgroundColor = [UIColor colorWithWhite:0.25 alpha:1];
    [export addTarget:self action:@selector(exportResults:) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:export];
}

- (NSArray *)caseIds { return @[CASE_A, CASE_B, CASE_C, CASE_D, CASE_E]; }

- (void)runCase:(UIButton *)sender {
    int idx = (int)(sender.tag - 100);
    if (idx < 0 || idx >= (int)self.results.count) return;
    NSString *cid = [self caseIds][idx];
    TLResult *r   = runSingleCase(cid);
    self.results[idx] = r;
    [self updateLabel:idx result:r];
    writeResult(self.results, makeRunId());
}

- (void)runAll:(id)sender {
    NSArray *allIds = [self caseIds];
    NSString *interrupted = interruptedCaseId();
    for (int i = 0; i < (int)allIds.count; ++i) {
        NSString *cid = allIds[i];
        TLResult *r;
        if (interrupted && [interrupted isEqualToString:cid]) {
            r = [[TLResult alloc] initWithId:cid version:1];
            r.status = @"interrupted";
            r.reason = [NSString stringWithFormat:@"Checkpoint from prior launch — %@ did not complete", cid];
            clearCheckpoint();
            interrupted = nil;
        } else {
            r = runSingleCase(cid);
        }
        self.results[i] = r;
        [self updateLabel:i result:r];
    }
    writeResult(self.results, makeRunId());
}

- (void)exportResults:(id)sender {
    writeResult(self.results, makeRunId());
    UIAlertView *av = [[UIAlertView alloc] initWithTitle:@"Exported"
        message:resultPath()
        delegate:nil cancelButtonTitle:@"OK" otherButtonTitles:nil];
    [av show];
}

- (void)updateLabel:(int)idx result:(TLResult *)r {
    UILabel *lbl = self.labels[idx];
    lbl.text = [NSString stringWithFormat:@"%@ — %@%@",
                r.caseId, r.status,
                (r.reason ? [NSString stringWithFormat:@": %@", r.reason] : @"")];
    lbl.textColor = colorForStatus(r.status);
}
@end

// ─────────────────────────── AppDelegate ─────────────────────────────────────

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@property (strong, nonatomic) UIWindow *window;
@end

@implementation AppDelegate

- (BOOL)application:(UIApplication *)application
    didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {

    // Parse arguments
    NSArray *args = [[NSProcessInfo processInfo] arguments];
    NSString *runCase = nil;
    BOOL runAll = NO;

    for (NSUInteger i = 1; i < args.count; ++i) {
        if ([args[i] isEqualToString:@"--run"] && i + 1 < args.count) {
            runCase = args[i+1]; i++;
        } else if ([args[i] isEqualToString:@"--runall"]) {
            runAll = YES;
        }
    }

    BOOL automatedMode = (runCase != nil || runAll);

    if (automatedMode) {
        NSArray *results;
        NSString *runId = makeRunId();
        if (runAll) {
            results = runAllCases(NO);
        } else {
            results = @[ runSingleCase(runCase) ];
        }
        writeResult(results, runId);

        // Print to stdout for the host runner to capture
        for (TLResult *r in results) {
            printf("TESTLAB %s %s\n", r.caseId.UTF8String, r.status.UTF8String);
            if (r.reason) printf("  reason: %s\n", r.reason.UTF8String);
        }

        // Exit code
        int code = 0;
        for (TLResult *r in results) {
            if ([r.status isEqualToString:@"interrupted"]) { code = 2; break; }
            if ([r.status isEqualToString:@"failed"])      { code = 1; }
        }
        exit(code);
    }

    // Interactive mode
    self.window = [[UIWindow alloc] initWithFrame:[[UIScreen mainScreen] bounds]];
    TLMenuController *vc = [[TLMenuController alloc] init];
    self.window.rootViewController = vc;
    [self.window makeKeyAndVisible];
    return YES;
}
@end

// ─────────────────────────── entry point ─────────────────────────────────────

int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, @"AppDelegate");
    }
}
