// HydrogenVM — HostGL platform on OpenGLES.framework.
//
// Each guest EAGLContext gets a host EAGLContext of the same API in a shared
// EAGLSharegroup. The guest's drawable renderbuffer gets its storage from a host
// CAEAGLLayer sized in pixels, and presentRenderbuffer: is the real one: Core
// Animation shows the frame with no copy. The shell places that layer under the
// software-composited UIKit overlay (which leaves a transparent hole over it).
//
// OpenGL ES is deprecated on iOS but still present; if it ever disappears,
// makeEaglPlatform returns null and the software rasterizer is used.
#import "HostGLEAGL.h"
#import <OpenGLES/EAGL.h>
#import <OpenGLES/EAGLDrawable.h>
#import <OpenGLES/ES2/gl.h>
#import <QuartzCore/QuartzCore.h>
#include <dlfcn.h>
#include <mutex>
#include <unordered_map>

namespace Hydrogen::App {
namespace {

namespace HG = Hydrogen::Runtime::HostGL;

class EaglPlatform final : public HG::Platform {
public:
    explicit EaglPlatform(std::function<void(CALayer*)> onLayer) : onLayer_(std::move(onLayer)) {}

    bool init() {
        lib_ = dlopen("/System/Library/Frameworks/OpenGLES.framework/OpenGLES", RTLD_LAZY | RTLD_LOCAL);
        if (!lib_) return false;
        // Make sure contexts can actually be created on this OS.
        EAGLContext *probe = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES2];
        return probe != nil;
    }

    const char *name() const override { return "OpenGLES.framework"; }

    void *createContext(int api, void *share, bool &desktop) override {
        desktop = false;
        const EAGLRenderingAPI a = api == 1 ? kEAGLRenderingAPIOpenGLES1
                                 : api == 3 ? kEAGLRenderingAPIOpenGLES3 : kEAGLRenderingAPIOpenGLES2;
        EAGLContext *shareCtx = share ? (__bridge EAGLContext *)share : nil;
        EAGLContext *c = shareCtx ? [[EAGLContext alloc] initWithAPI:a sharegroup:shareCtx.sharegroup]
                                  : [[EAGLContext alloc] initWithAPI:a];
        if (!c && api == 3) c = shareCtx ? [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES2 sharegroup:shareCtx.sharegroup]
                                         : [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES2];
        if (!c) return nullptr;
        return (__bridge_retained void *)c;
    }

    void destroyContext(void *ctx) override {
        if (!ctx) return;
        EAGLContext *c = (__bridge_transfer EAGLContext *)ctx;
        if ([EAGLContext currentContext] == c) [EAGLContext setCurrentContext:nil];
        (void)c;
    }

    bool makeCurrent(void *ctx) override {
        return [EAGLContext setCurrentContext:ctx ? (__bridge EAGLContext *)ctx : nil];
    }

    void *proc(void *, const char *name) override { return lib_ ? dlsym(lib_, name) : nullptr; }

    bool drawableStorage(void *ctx, uint32_t layerKey, int width, int height, bool &handled) override {
        handled = true;
        EAGLContext *c = (__bridge EAGLContext *)ctx;
        if (!layerKey) return [c renderbufferStorage:GL_RENDERBUFFER fromDrawable:nil];
        CAEAGLLayer *layer = nil;
        bool created = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = layers_.find(layerKey);
            if (it != layers_.end()) layer = (__bridge CAEAGLLayer *)it->second;
        }
        if (!layer) {
            layer = [CAEAGLLayer layer];
            layer.opaque = YES;
            layer.drawableProperties = @{kEAGLDrawablePropertyRetainedBacking: @NO,
                                         kEAGLDrawablePropertyColorFormat: kEAGLColorFormatRGBA8};
            std::lock_guard<std::mutex> lk(mu_);
            layers_[layerKey] = (__bridge_retained void *)layer;
            created = true;
        }
        // The drawable is sized in pixels: bounds in pixels at scale 1. The shell
        // scales the layer to the viewport with a transform.
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        layer.contentsScale = 1;
        layer.bounds = CGRectMake(0, 0, width, height);
        [CATransaction commit];
        const BOOL ok = [c renderbufferStorage:GL_RENDERBUFFER fromDrawable:layer];
        [CATransaction flush];
        if (created || ok) {
            auto notify = onLayer_;
            CALayer *shown = layer;
            dispatch_async(dispatch_get_main_queue(), ^{ if (notify) notify(shown); });
        }
        return ok;
    }

    bool presentDirect(void *ctx, uint32_t) override {
        EAGLContext *c = (__bridge EAGLContext *)ctx;
        const BOOL ok = [c presentRenderbuffer:GL_RENDERBUFFER];
        // The guest thread has no run loop, so its implicit Core Animation
        // transaction (which carries the new layer contents) is never committed
        // on its own: commit it now or the layer stays blank.
        [CATransaction flush];
        return ok;
    }

    void releaseLayer(uint32_t layerKey) override {
        void *gone = nullptr;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = layers_.find(layerKey);
            if (it == layers_.end()) return;
            gone = it->second;
            layers_.erase(it);
        }
        CAEAGLLayer *layer = (__bridge_transfer CAEAGLLayer *)gone;
        auto notify = onLayer_;
        dispatch_async(dispatch_get_main_queue(), ^{
            [layer removeFromSuperlayer];
            if (notify) notify(nil);
        });
    }

    ~EaglPlatform() override {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto &[k, v] : layers_) {
            CAEAGLLayer *layer = (__bridge_transfer CAEAGLLayer *)v;
            dispatch_async(dispatch_get_main_queue(), ^{ [layer removeFromSuperlayer]; });
        }
        layers_.clear();
    }

private:
    void *lib_ = nullptr;
    std::function<void(CALayer *)> onLayer_;
    std::mutex mu_;
    std::unordered_map<uint32_t, void *> layers_;   // guest CAEAGLLayer -> retained host CAEAGLLayer
};

} // namespace

std::shared_ptr<HG::Platform> makeEaglPlatform(std::function<void(CALayer *)> onLayer) {
    auto p = std::make_shared<EaglPlatform>(std::move(onLayer));
    if (!p->init()) return nullptr;
    return p;
}

} // namespace Hydrogen::App
