// HydrogenVM — AVAssetWriter family and CVPixelBuffer.
//
// Games ship video-capture SDKs (Everyplay and similar recorders) that set
// up an AVAssetWriter at launch on any device that has one, and assert when
// it is missing. Writing a movie is out of scope: the writer accepts inputs
// and frames and discards them, and finishing reports success with no file.
// CVPixelBuffers are real memory (glReadPixels into CVPixelBufferGetBaseAddress
// works), and the OpenGL ES texture cache hands out real GL textures.
#include "UIKit.hpp"

namespace Hydrogen::Runtime::UIKit {

using Foundation::autoreleased;

namespace {

// ------------------------------------------------------------ CVPixelBuffer ---
// Pixel buffers, texture caches and cache textures are Objective-C objects, so
// CFRetain/CFRelease work on them as on iOS.
constexpr int32_t kCVReturnSuccess = 0, kCVReturnInvalidArgument = -6661;

struct PixelBuffer : ObjC::HostObject {
    Process* proc = nullptr;
    uint32_t width = 0, height = 0, format = 0, bytesPerRow = 0, data = 0;
    ~PixelBuffer() override { if (proc && data) proc->heap().free(data); }
};
struct CacheTexture : ObjC::HostObject { uint32_t name = 0, target = 0; };
struct CVClasses { explicit CVClasses(Process&) {} Class pixelBuffer = 0, cache = 0, texture = 0; };
CVClasses& cvc(Ctx& x) { return x.proc.service<CVClasses>(); }

PixelBuffer* pb(Ctx& x, uint32_t ref) { return ref ? rt(x).hostIf<PixelBuffer>(ref) : nullptr; }
CacheTexture* ctex(Ctx& x, uint32_t ref) { return ref ? rt(x).hostIf<CacheTexture>(ref) : nullptr; }

uint32_t bytesPerPixel(uint32_t fmt) {
    switch (fmt) {
    case 0x00000018: return 3;                                     // k24RGBPixelFormat
    case 0x00000010: case 0x00000011: return 2;                    // 16-bit RGB
    case 0x4c303038: return 1;                                     // 'L008'
    default: return 4;                                             // 'BGRA', 'RGBA', 32ARGB...
    }
}

int32_t cv_pixelBufferCreate(Ctx& x, uint32_t, uint32_t w, uint32_t h, uint32_t fmt, uint32_t, GuestPtr out) {
    if (!out || !w || !h || w > 8192 || h > 8192) return kCVReturnInvalidArgument;
    const id obj = rt(x).createInstance(cvc(x).pixelBuffer);
    auto d = std::make_unique<PixelBuffer>();
    d->proc = &x.proc;
    d->width = w; d->height = h; d->format = fmt;
    d->bytesPerRow = (w * bytesPerPixel(fmt) + 63) & ~63u;
    d->data = x.proc.heap().allocZeroed(d->bytesPerRow * h);
    rt(x).setHost(obj, std::move(d));
    x.mem.write<uint32_t>(out, obj);
    return kCVReturnSuccess;
}
uint32_t cv_retain(Ctx& x, uint32_t ref) { return ref ? rt(x).retain(x.cpu, ref) : 0; }
void cv_release(Ctx& x, uint32_t ref) { if (ref) rt(x).release(x.cpu, ref); }
int32_t cv_lock(Ctx& x, uint32_t ref, uint32_t) { return pb(x, ref) ? kCVReturnSuccess : kCVReturnInvalidArgument; }
uint32_t cv_width(Ctx& x, uint32_t ref) { auto* d = pb(x, ref); return d ? d->width : 0; }
uint32_t cv_height(Ctx& x, uint32_t ref) { auto* d = pb(x, ref); return d ? d->height : 0; }
uint32_t cv_format(Ctx& x, uint32_t ref) { auto* d = pb(x, ref); return d ? d->format : 0; }
uint32_t cv_bytesPerRow(Ctx& x, uint32_t ref) { auto* d = pb(x, ref); return d ? d->bytesPerRow : 0; }
uint32_t cv_baseAddress(Ctx& x, uint32_t ref) { auto* d = pb(x, ref); return d ? d->data : 0; }
uint32_t cv_dataSize(Ctx& x, uint32_t ref) { auto* d = pb(x, ref); return d ? d->bytesPerRow * d->height : 0; }
uint32_t cv_isPlanar(Ctx&, uint32_t) { return 0; }
uint32_t cv_planeCount(Ctx&, uint32_t) { return 0; }
int32_t cv_poolCreatePixelBuffer(Ctx& x, uint32_t, uint32_t, GuestPtr out) {
    if (out) x.mem.write<uint32_t>(out, 0);
    return kCVReturnInvalidArgument;   // pixelBufferPool is always NULL here
}

// CVOpenGLESTextureCache: textures made from a pixel buffer are ordinary GL
// textures of the same size. What is drawn into them is not copied back into
// the buffer (the frames only went to the discarded movie anyway).
int32_t cv_textureCacheCreate(Ctx& x, uint32_t, uint32_t, uint32_t, uint32_t, GuestPtr out) {
    if (!out) return kCVReturnInvalidArgument;
    x.mem.write<uint32_t>(out, rt(x).createInstance(cvc(x).cache));
    return kCVReturnSuccess;
}
uint32_t glCall(Ctx& x, const char* name, std::initializer_list<uint32_t> args) {
    auto fn = x.proc.lookupSymbol(name);
    return fn ? uint32_t(x.proc.callGuest(x.cpu, *fn, args)) : 0;
}
int32_t cv_textureCacheCreateTexture(Ctx& x, uint32_t, uint32_t cache, uint32_t buffer, uint32_t) {
    // (allocator, cache, buffer, attributes | target, internalFormat, width, height, format, type, plane, out)
    const uint32_t sp = x.cpu.r[CPU::kSP];
    auto arg = [&](int i) { return x.mem.read<uint32_t>(sp + 4 * uint32_t(i)); };
    const uint32_t target = arg(0), internalFormat = arg(1), w = arg(2), h = arg(3), format = arg(4), type = arg(5);
    const GuestPtr out = arg(7);
    if (!out) return kCVReturnInvalidArgument;
    x.mem.write<uint32_t>(out, 0);
    if (!cache || !pb(x, buffer) || !w || !h) return kCVReturnInvalidArgument;
    const uint32_t tmp = x.proc.heap().allocZeroed(8);
    glCall(x, "_glGetIntegerv", {0x8069, tmp + 4});               // GL_TEXTURE_BINDING_2D
    glCall(x, "_glGenTextures", {1, tmp});
    const uint32_t name = x.mem.read<uint32_t>(tmp), previous = x.mem.read<uint32_t>(tmp + 4);
    glCall(x, "_glBindTexture", {target, name});
    glCall(x, "_glTexImage2D", {target, 0, internalFormat, w, h, 0, format, type, 0});
    glCall(x, "_glTexParameteri", {target, 0x2801, 0x2601});        // MIN_FILTER LINEAR
    glCall(x, "_glBindTexture", {target, previous});
    x.proc.heap().free(tmp);
    const id tex = rt(x).createInstance(cvc(x).texture);
    auto d = std::make_unique<CacheTexture>();
    d->name = name;
    d->target = target;
    rt(x).setHost(tex, std::move(d));
    x.mem.write<uint32_t>(out, tex);
    return kCVReturnSuccess;
}
uint32_t cv_textureName(Ctx& x, uint32_t t) { auto* d = ctex(x, t); return d ? d->name : 0; }
uint32_t cv_textureTarget(Ctx& x, uint32_t t) { auto* d = ctex(x, t); return d ? d->target : 0; }
uint32_t cv_textureFlipped(Ctx&, uint32_t) { return 0; }
void cv_textureCacheFlush(Ctx&, uint32_t, uint64_t) {}

// ------------------------------------------------------------ AVAssetWriter ---
struct WriterData : ObjC::HostObject {
    id url = 0, fileType = 0, inputs = 0;
    uint32_t status = 0;   // AVAssetWriterStatusUnknown
};
struct InputData : ObjC::HostObject {
    id mediaType = 0, settings = 0;
    bool realTime = false, finished = false;
};
struct AdaptorData : ObjC::HostObject { id input = 0, attributes = 0; };

id retained(Ctx& x, id o) { return o ? rt(x).retain(x.cpu, o) : 0; }

id initWriter(Ctx& x, id self, id url, id type, GuestPtr err) {
    auto& d = rt(x).host<WriterData>(self);
    d.url = retained(x, url);
    d.fileType = retained(x, type);
    d.inputs = retained(x, Foundation::makeArray(x, {}));
    if (err) x.mem.write<uint32_t>(err, 0);
    return self;
}

} // namespace

void registerAVWriter(Process& p, ObjC::Runtime& r) {
    p.def<&cv_pixelBufferCreate>("_CVPixelBufferCreate");
    p.def<&cv_retain>("_CVPixelBufferRetain");
    p.def<&cv_release>("_CVPixelBufferRelease");
    p.def<&cv_retain>("_CVBufferRetain");
    p.def<&cv_release>("_CVBufferRelease");
    p.def<&cv_lock>("_CVPixelBufferLockBaseAddress");
    p.def<&cv_lock>("_CVPixelBufferUnlockBaseAddress");
    p.def<&cv_width>("_CVPixelBufferGetWidth");
    p.def<&cv_height>("_CVPixelBufferGetHeight");
    p.def<&cv_format>("_CVPixelBufferGetPixelFormatType");
    p.def<&cv_bytesPerRow>("_CVPixelBufferGetBytesPerRow");
    p.def<&cv_baseAddress>("_CVPixelBufferGetBaseAddress");
    p.def<&cv_dataSize>("_CVPixelBufferGetDataSize");
    p.def<&cv_isPlanar>("_CVPixelBufferIsPlanar");
    p.def<&cv_planeCount>("_CVPixelBufferGetPlaneCount");
    p.def<&cv_poolCreatePixelBuffer>("_CVPixelBufferPoolCreatePixelBuffer");
    p.def<&cv_textureCacheCreate>("_CVOpenGLESTextureCacheCreate");
    p.def<&cv_textureCacheCreateTexture>("_CVOpenGLESTextureCacheCreateTextureFromImage");
    p.def<&cv_textureCacheFlush>("_CVOpenGLESTextureCacheFlush");
    p.def<&cv_textureName>("_CVOpenGLESTextureGetName");
    p.def<&cv_textureTarget>("_CVOpenGLESTextureGetTarget");
    p.def<&cv_textureFlipped>("_CVOpenGLESTextureIsFlipped");
    auto& c = p.service<CVClasses>();
    c.pixelBuffer = r.defineClass("__NSCVPixelBuffer", "NSObject").cls();
    c.cache = r.defineClass("__NSCVOpenGLESTextureCache", "NSObject").cls();
    ObjC::ClassBuilder texCls = r.defineClass("__NSCVOpenGLESTexture", "NSObject");
    c.texture = texCls.cls();
    texCls.method("dealloc", [](Ctx& x) {
        // The GL texture goes with its CVOpenGLESTextureRef, as on iOS.
        if (auto* d = ctex(x, x.cpu.r[0]); d && d->name) {
            const uint32_t tmp = x.proc.heap().allocZeroed(4);
            x.mem.write<uint32_t>(tmp, d->name);
            glCall(x, "_glDeleteTextures", {1, tmp});
            x.proc.heap().free(tmp);
            d->name = 0;
        }
        rt(x).disposeInstance(x.cpu, x.cpu.r[0]);
    }, "v@:");

    ObjC::ClassBuilder w = r.defineClass("AVAssetWriter", "NSObject");
    w.classMethod("assetWriterWithURL:fileType:error:", [](Ctx& x) {
        const id self = rt(x).createInstance(x.cpu.r[0]);
        x.cpu.r[0] = autoreleased(x, initWriter(x, self, x.cpu.r[2], x.cpu.r[3], x.mem.read<uint32_t>(x.cpu.r[CPU::kSP])));
    }, "@@:@@^@");
    w.method("initWithURL:fileType:error:", [](Ctx& x) {
        x.cpu.r[0] = initWriter(x, x.cpu.r[0], x.cpu.r[2], x.cpu.r[3], x.mem.read<uint32_t>(x.cpu.r[CPU::kSP]));
    }, "@@:@@^@");
    w.method("outputURL", [](Ctx& x) { auto* d = rt(x).hostIf<WriterData>(x.cpu.r[0]); x.cpu.r[0] = d ? d->url : 0; }, "@@:");
    w.method("outputFileType", [](Ctx& x) { auto* d = rt(x).hostIf<WriterData>(x.cpu.r[0]); x.cpu.r[0] = d ? d->fileType : 0; }, "@@:");
    w.method("inputs", [](Ctx& x) { auto* d = rt(x).hostIf<WriterData>(x.cpu.r[0]); x.cpu.r[0] = d ? d->inputs : 0; }, "@@:");
    w.method("canAddInput:", [](Ctx& x) { x.cpu.r[0] = x.cpu.r[2] != 0; }, "c@:@");
    w.method("addInput:", [](Ctx& x) {
        auto* d = rt(x).hostIf<WriterData>(x.cpu.r[0]);
        if (d && x.cpu.r[2]) {
            const id grown = id(rt(x).send(x.cpu, d->inputs, "arrayByAddingObject:", {x.cpu.r[2]}));
            rt(x).release(x.cpu, d->inputs);
            d->inputs = retained(x, grown);
        }
    }, "v@:@");
    w.method("canApplyOutputSettings:forMediaType:", [](Ctx& x) { x.cpu.r[0] = 1; }, "c@:@@");
    w.method("startWriting", [](Ctx& x) {
        if (auto* d = rt(x).hostIf<WriterData>(x.cpu.r[0])) d->status = 1;   // AVAssetWriterStatusWriting
        x.cpu.r[0] = 1;
    }, "c@:");
    w.method("startSessionAtSourceTime:", [](Ctx&) {}, "v@:{?=qiIq}");
    w.method("endSessionAtSourceTime:", [](Ctx&) {}, "v@:{?=qiIq}");
    w.method("finishWriting", [](Ctx& x) {
        if (auto* d = rt(x).hostIf<WriterData>(x.cpu.r[0])) d->status = 2;   // completed
        x.cpu.r[0] = 1;
    }, "c@:");
    w.method("finishWritingWithCompletionHandler:", [](Ctx& x) {
        if (auto* d = rt(x).hostIf<WriterData>(x.cpu.r[0])) d->status = 2;
        if (!x.cpu.r[2]) return;
        const uint32_t block = ObjC::blockCopy(x, x.cpu.r[2]);
        Process* proc = &x.proc;
        Foundation::mainRunLoop(x.proc).post([proc, block](CPU::Cpu& c) {
            Ctx cx{*proc, c, proc->mem()};
            ObjC::callBlock(cx, block, {});
            ObjC::blockRelease(cx, block);
        });
    }, "v@:@?");
    w.method("cancelWriting", [](Ctx& x) { if (auto* d = rt(x).hostIf<WriterData>(x.cpu.r[0])) d->status = 4; }, "v@:");
    w.method("status", [](Ctx& x) { auto* d = rt(x).hostIf<WriterData>(x.cpu.r[0]); x.cpu.r[0] = d ? d->status : 0; }, "i@:");
    w.method("error", [](Ctx& x) { x.cpu.r[0] = 0; }, "@@:");
    w.method("setShouldOptimizeForNetworkUse:", [](Ctx&) {}, "v@:c");
    w.method("shouldOptimizeForNetworkUse", [](Ctx& x) { x.cpu.r[0] = 0; }, "c@:");
    w.method("setMovieFragmentInterval:", [](Ctx&) {}, "v@:{?=qiIq}");
    w.method("setMovieTimeScale:", [](Ctx&) {}, "v@:i");
    w.method("setMetadata:", [](Ctx&) {}, "v@:@");
    w.method("metadata", [](Ctx& x) { x.cpu.r[0] = Foundation::makeArray(x, {}); }, "@@:");
    w.method("availableMediaTypes", [](Ctx& x) {
        x.cpu.r[0] = Foundation::makeArray(x, {Foundation::constString(x.proc, "AVMediaTypeVideo"), Foundation::constString(x.proc, "AVMediaTypeAudio")});
    }, "@@:");
    w.method("dealloc", [](Ctx& x) {
        if (auto* d = rt(x).hostIf<WriterData>(x.cpu.r[0])) {
            rt(x).release(x.cpu, d->url); rt(x).release(x.cpu, d->fileType); rt(x).release(x.cpu, d->inputs);
            d->url = d->fileType = d->inputs = 0;
        }
        rt(x).disposeInstance(x.cpu, x.cpu.r[0]);
    }, "v@:");

    ObjC::ClassBuilder in = r.defineClass("AVAssetWriterInput", "NSObject");
    auto initInput = [](Ctx& x, id self, id type, id settings) {
        auto& d = rt(x).host<InputData>(self);
        d.mediaType = retained(x, type);
        d.settings = retained(x, settings);
        return self;
    };
    in.classMethod("assetWriterInputWithMediaType:outputSettings:", [initInput](Ctx& x) {
        x.cpu.r[0] = autoreleased(x, initInput(x, rt(x).createInstance(x.cpu.r[0]), x.cpu.r[2], x.cpu.r[3]));
    }, "@@:@@");
    in.classMethod("assetWriterInputWithMediaType:outputSettings:sourceFormatHint:", [initInput](Ctx& x) {
        x.cpu.r[0] = autoreleased(x, initInput(x, rt(x).createInstance(x.cpu.r[0]), x.cpu.r[2], x.cpu.r[3]));
    }, "@@:@@^v");
    in.method("initWithMediaType:outputSettings:", [initInput](Ctx& x) { x.cpu.r[0] = initInput(x, x.cpu.r[0], x.cpu.r[2], x.cpu.r[3]); }, "@@:@@");
    in.method("initWithMediaType:outputSettings:sourceFormatHint:", [initInput](Ctx& x) { x.cpu.r[0] = initInput(x, x.cpu.r[0], x.cpu.r[2], x.cpu.r[3]); }, "@@:@@^v");
    in.method("mediaType", [](Ctx& x) { auto* d = rt(x).hostIf<InputData>(x.cpu.r[0]); x.cpu.r[0] = d ? d->mediaType : 0; }, "@@:");
    in.method("outputSettings", [](Ctx& x) { auto* d = rt(x).hostIf<InputData>(x.cpu.r[0]); x.cpu.r[0] = d ? d->settings : 0; }, "@@:");
    in.method("setExpectsMediaDataInRealTime:", [](Ctx& x) { if (auto* d = rt(x).hostIf<InputData>(x.cpu.r[0])) d->realTime = (x.cpu.r[2] & 0xFF) != 0; }, "v@:c");
    in.method("expectsMediaDataInRealTime", [](Ctx& x) { auto* d = rt(x).hostIf<InputData>(x.cpu.r[0]); x.cpu.r[0] = d && d->realTime; }, "c@:");
    in.method("isReadyForMoreMediaData", [](Ctx& x) { auto* d = rt(x).hostIf<InputData>(x.cpu.r[0]); x.cpu.r[0] = !d || !d->finished; }, "c@:");
    in.method("appendSampleBuffer:", [](Ctx& x) { x.cpu.r[0] = 1; }, "c@:^v");
    in.method("markAsFinished", [](Ctx& x) { if (auto* d = rt(x).hostIf<InputData>(x.cpu.r[0])) d->finished = true; }, "v@:");
    in.method("requestMediaDataWhenReadyOnQueue:usingBlock:", [](Ctx&) {}, "v@:@@?");
    in.method("setTransform:", [](Ctx&) {}, "v@:{CGAffineTransform=ffffff}");
    in.method("setMetadata:", [](Ctx&) {}, "v@:@");
    in.method("setMediaTimeScale:", [](Ctx&) {}, "v@:i");
    in.method("dealloc", [](Ctx& x) {
        if (auto* d = rt(x).hostIf<InputData>(x.cpu.r[0])) {
            rt(x).release(x.cpu, d->mediaType); rt(x).release(x.cpu, d->settings);
            d->mediaType = d->settings = 0;
        }
        rt(x).disposeInstance(x.cpu, x.cpu.r[0]);
    }, "v@:");

    ObjC::ClassBuilder ad = r.defineClass("AVAssetWriterInputPixelBufferAdaptor", "NSObject");
    auto initAdaptor = [](Ctx& x, id self, id input, id attrs) {
        auto& d = rt(x).host<AdaptorData>(self);
        d.input = retained(x, input);
        d.attributes = retained(x, attrs);
        return self;
    };
    ad.classMethod("assetWriterInputPixelBufferAdaptorWithAssetWriterInput:sourcePixelBufferAttributes:", [initAdaptor](Ctx& x) {
        x.cpu.r[0] = autoreleased(x, initAdaptor(x, rt(x).createInstance(x.cpu.r[0]), x.cpu.r[2], x.cpu.r[3]));
    }, "@@:@@");
    ad.method("initWithAssetWriterInput:sourcePixelBufferAttributes:", [initAdaptor](Ctx& x) {
        x.cpu.r[0] = initAdaptor(x, x.cpu.r[0], x.cpu.r[2], x.cpu.r[3]);
    }, "@@:@@");
    ad.method("assetWriterInput", [](Ctx& x) { auto* d = rt(x).hostIf<AdaptorData>(x.cpu.r[0]); x.cpu.r[0] = d ? d->input : 0; }, "@@:");
    ad.method("sourcePixelBufferAttributes", [](Ctx& x) { auto* d = rt(x).hostIf<AdaptorData>(x.cpu.r[0]); x.cpu.r[0] = d ? d->attributes : 0; }, "@@:");
    ad.method("pixelBufferPool", [](Ctx& x) { x.cpu.r[0] = 0; }, "^v@:");
    ad.method("appendPixelBuffer:withPresentationTime:", [](Ctx& x) { x.cpu.r[0] = 1; }, "c@:^v{?=qiIq}");
    ad.method("dealloc", [](Ctx& x) {
        if (auto* d = rt(x).hostIf<AdaptorData>(x.cpu.r[0])) {
            rt(x).release(x.cpu, d->input); rt(x).release(x.cpu, d->attributes);
            d->input = d->attributes = 0;
        }
        rt(x).disposeInstance(x.cpu, x.cpu.r[0]);
    }, "v@:");
}

} // namespace Hydrogen::Runtime::UIKit
