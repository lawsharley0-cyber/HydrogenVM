// HydrogenVM — small framework pieces apps touch at launch:
//   UIWebView (no web engine: remote loads fail offline, HTML loads "finish"),
//   CoreTelephony (no carrier), SystemConfiguration's CaptiveNetwork (no Wi-Fi info),
//   Security certificates/keys (unavailable), +attemptRotationToDeviceOrientation.
#include "UIKit.hpp"
#include <cctype>
#include <functional>

namespace Hydrogen::Runtime::UIKit {
namespace {

using ObjC::SEL;

// ---- UIWebView
struct WebData : ObjC::HostObject { id delegate = 0, request = 0; };
WebData& WD(Ctx& x, id w) { return rt(x).host<WebData>(w); }

void wv_notify(Ctx& x, id self, bool fail, id error) {
    rt(x).retain(x.cpu, self);
    if (error) rt(x).retain(x.cpu, error);
    Process* p = &x.proc;
    Foundation::mainRunLoop(x.proc).post([p, self, fail, error](CPU::Cpu& c) {
        Ctx cx{*p, c, p->mem()};
        const id d = rt(cx).hostIf<WebData>(self) ? WD(cx, self).delegate : 0;
        if (d && rt(cx).respondsTo(d, rt(cx).sel("webViewDidStartLoad:"))) rt(cx).send(c, d, "webViewDidStartLoad:", {self});
        if (d && fail && rt(cx).respondsTo(d, rt(cx).sel("webView:didFailLoadWithError:")))
            rt(cx).send(c, d, "webView:didFailLoadWithError:", {self, error});
        if (d && !fail && rt(cx).respondsTo(d, rt(cx).sel("webViewDidFinishLoad:"))) rt(cx).send(c, d, "webViewDidFinishLoad:", {self});
        if (error) rt(cx).release(c, error);
        rt(cx).release(c, self);
    });
}
id wv_delegate(Ctx& x, id self, SEL) { return WD(x, self).delegate; }
void wv_setDelegate(Ctx& x, id self, SEL, id d) { WD(x, self).delegate = d; }
id wv_request(Ctx& x, id self, SEL) { return WD(x, self).request; }
void wv_loadRequest(Ctx& x, id self, SEL, id request) {
    WebData& d = WD(x, self);
    const id old = d.request;
    d.request = request ? rt(x).retain(x.cpu, request) : 0;
    if (old) rt(x).release(x.cpu, old);
    const id url = request ? id(rt(x).send(x.cpu, request, "URL")) : 0;
    const std::string s = url ? Foundation::urlString(x, url) : "";
    if (s.rfind("file:", 0) == 0) { wv_notify(x, self, false, 0); return; }
    const id info = Foundation::makeDict(x, {{Foundation::constString(x.proc, "NSLocalizedDescription"),
                                              Foundation::makeString(x, "The Internet connection appears to be offline.")}});
    const id err = id(rt(x).send(x.cpu, rt(x).getClass("NSError"), "errorWithDomain:code:userInfo:",
                                 {Foundation::constString(x.proc, "NSURLErrorDomain"), uint32_t(-1009), info}));
    wv_notify(x, self, true, err);
}
void wv_loadHTML(Ctx& x, id self, SEL, id, id) { wv_notify(x, self, false, 0); }
void wv_loadData(Ctx& x, id self, SEL, id, id, id, id) { wv_notify(x, self, false, 0); }
id wv_eval(Ctx& x, id, SEL, id) { return Foundation::makeString(x, ""); }
void wv_ignoreBool(Ctx&, id, SEL, bool) {}
void wv_ignoreInt(Ctx&, id, SEL, int32_t) {}
void wv_noop(Ctx&, id, SEL) {}
bool wv_no(Ctx&, id, SEL) { return false; }
void wv_dealloc(Ctx& x, id self, SEL s) {
    if (WebData* d = rt(x).hostIf<WebData>(self)) if (d->request) rt(x).release(x.cpu, d->request);
    rt(x).sendSuper(x.cpu, self, rt(x).getClass("UIView"), s, {});
}

// ---- CoreTelephony: no SIM / carrier
id ct_nil(Ctx&, id, SEL) { return 0; }
void ct_ignore(Ctx&, id, SEL, uint32_t) {}

// ---- misc C functions
uint32_t z1(Ctx&, uint32_t) { return 0; }
uint32_t z2(Ctx&, uint32_t, uint32_t) { return 0; }
int32_t secUnimplemented6(Ctx&, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return -4; }   // errSecUnimplemented
int32_t secUnimplemented3(Ctx&, uint32_t, uint32_t, uint32_t) { return -4; }
void vc_attemptRotation(Ctx&, id, SEL) {}

// ---- MobileCoreServices: uniform type identifiers for common file types.
struct UTEntry { const char* uti; const char* ext; const char* mime; const char* parent; };
const UTEntry kUT[] = {
    {"public.png", "png", "image/png", "public.image"}, {"public.jpeg", "jpg", "image/jpeg", "public.image"},
    {"public.jpeg", "jpeg", "image/jpeg", "public.image"}, {"com.compuserve.gif", "gif", "image/gif", "public.image"},
    {"public.tiff", "tiff", "image/tiff", "public.image"}, {"com.microsoft.bmp", "bmp", "image/bmp", "public.image"},
    {"public.plain-text", "txt", "text/plain", "public.text"}, {"public.html", "html", "text/html", "public.text"},
    {"public.html", "htm", "text/html", "public.text"}, {"public.xml", "xml", "application/xml", "public.text"},
    {"public.json", "json", "application/json", "public.text"}, {"com.adobe.pdf", "pdf", "application/pdf", "public.data"},
    {"public.mp3", "mp3", "audio/mpeg", "public.audio"}, {"com.apple.coreaudio-format", "caf", "audio/x-caf", "public.audio"},
    {"com.microsoft.waveform-audio", "wav", "audio/wav", "public.audio"}, {"public.aiff-audio", "aiff", "audio/aiff", "public.audio"},
    {"public.mpeg-4-audio", "m4a", "audio/mp4", "public.audio"}, {"public.mpeg-4", "mp4", "video/mp4", "public.movie"},
    {"com.apple.quicktime-movie", "mov", "video/quicktime", "public.movie"}, {"com.pkware.zip-archive", "zip", "application/zip", "public.data"},
    {"com.apple.property-list", "plist", "application/x-plist", "public.data"},
};
std::string cfStr(Ctx& x, id s) { return s ? Foundation::utf8(x, s) : std::string(); }
uint32_t ut_copyTag(Ctx& x, id uti, id tagClass) {
    const std::string u = cfStr(x, uti), c = cfStr(x, tagClass);
    for (auto& e : kUT)
        if (u == e.uti) return Foundation::makeString(x, c == "public.mime-type" ? e.mime : e.ext, true);
    return 0;
}
uint32_t ut_createId(Ctx& x, id tagClass, id tag, id) {
    const std::string c = cfStr(x, tagClass);
    std::string t = cfStr(x, tag);
    for (char& ch : t) ch = char(std::tolower(uint8_t(ch)));
    for (auto& e : kUT)
        if ((c == "public.mime-type" && t == e.mime) || (c != "public.mime-type" && t == e.ext)) return Foundation::makeString(x, e.uti, true);
    return Foundation::makeString(x, "dyn.age8" + t, true);   // a dynamic UTI, like the real one for unknown tags
}
bool ut_conforms(Ctx& x, id a, id b) {
    const std::string u = cfStr(x, a), p = cfStr(x, b);
    if (u == p || p == "public.data" || p == "public.item" || p == "public.content") return true;
    for (auto& e : kUT) if (u == e.uti && p == e.parent) return true;
    return false;
}
bool ut_equal(Ctx& x, id a, id b) { return cfStr(x, a) == cfStr(x, b); }
uint32_t ut_copyDescription(Ctx& x, id uti) { return Foundation::makeString(x, cfStr(x, uti), true); }

void postNote(Ctx& x, const char* name, id object, id userInfo) {
    const id center = id(rt(x).send(x.cpu, rt(x).getClass("NSNotificationCenter"), "defaultCenter"));
    rt(x).send(x.cpu, center, "postNotificationName:object:userInfo:", {Foundation::constString(x.proc, name), object, userInfo});
}
void later(Ctx& x, id keep, double delay, std::function<void(Ctx&)> fn) {
    rt(x).retain(x.cpu, keep);
    Process* p = &x.proc;
    Foundation::mainRunLoop(x.proc).addTimer(delay, 0, false, [p, keep, fn](CPU::Cpu& c) {
        Ctx cx{*p, c, p->mem()};
        fn(cx);
        rt(cx).release(c, keep);
    });
}

// ---- UIImagePickerController: no camera or photo library available.
bool ip_sourceAvailable(Ctx&, id, SEL, int32_t) { return false; }
id ip_mediaTypes(Ctx&, id, SEL, int32_t) { return 0; }
bool ip_cameraAvailable(Ctx&, id, SEL, int32_t) { return false; }
void ip_ignoreInt(Ctx&, id, SEL, int32_t) {}
void ip_ignoreObj(Ctx&, id, SEL, id) {}
void ip_ignoreBool(Ctx&, id, SEL, bool) {}
int32_t ip_zero(Ctx&, id, SEL) { return 0; }

// ---- CLLocationManager: location services are off. Updates fail with kCLErrorDenied.
struct LocData : ObjC::HostObject { id delegate = 0; };
id loc_delegate(Ctx& x, id self, SEL) { return rt(x).host<LocData>(self).delegate; }
void loc_setDelegate(Ctx& x, id self, SEL, id d) { rt(x).host<LocData>(self).delegate = d; }
bool locc_no(Ctx&, id, SEL) { return false; }
int32_t locc_status(Ctx&, id, SEL) { return 2; }   // kCLAuthorizationStatusDenied
void loc_fail(Ctx& x, id self, SEL) {
    later(x, self, 0.1, [self](Ctx& cx) {
        const id d = rt(cx).host<LocData>(self).delegate;
        if (!d) return;
        if (rt(cx).respondsTo(d, rt(cx).sel("locationManager:didFailWithError:"))) {
            const id err = id(rt(cx).send(cx.cpu, rt(cx).getClass("NSError"), "errorWithDomain:code:userInfo:",
                                          {Foundation::constString(cx.proc, "kCLErrorDomain"), 1u /* kCLErrorDenied */, 0}));
            rt(cx).send(cx.cpu, d, "locationManager:didFailWithError:", {self, err});
        }
    });
}
void loc_requestAuth(Ctx& x, id self, SEL) {
    later(x, self, 0.05, [self](Ctx& cx) {
        const id d = rt(cx).host<LocData>(self).delegate;
        if (d && rt(cx).respondsTo(d, rt(cx).sel("locationManager:didChangeAuthorizationStatus:")))
            rt(cx).send(cx.cpu, d, "locationManager:didChangeAuthorizationStatus:", {self, 2u});
    });
}
void loc_noop(Ctx&, id, SEL) {}
void loc_ignoreDouble(Ctx&, id, SEL, double) {}
double loc_zeroDouble(Ctx&, id, SEL) { return 0; }
id loc_nil(Ctx&, id, SEL) { return 0; }
void loc_ignoreObj(Ctx&, id, SEL, id) {}
void loc_ignoreInt(Ctx&, id, SEL, int32_t) {}

// ---- MPMoviePlayerController: no video decoding. play() "finishes" right away, so
// intro/cut-scene code moves on as if the movie had played.
struct MovieData : ObjC::HostObject { id url = 0, view = 0, background = 0; int32_t state = 0; bool finished = false; };
MovieData& MD(Ctx& x, id self) { return rt(x).host<MovieData>(self); }
id mp_initWithURL(Ctx& x, id self, SEL, id url) {
    MovieData& m = MD(x, self);
    m.url = url ? rt(x).retain(x.cpu, url) : 0;
    const id view = id(rt(x).send(x.cpu, rt(x).getClass("UIView"), rt(x).selAlloc));
    m.view = id(rt(x).send(x.cpu, view, "init"));
    rt(x).send(x.cpu, m.view, "setBackgroundColor:", {id(rt(x).send(x.cpu, rt(x).getClass("UIColor"), "blackColor"))});
    return self;
}
id mp_init(Ctx& x, id self, SEL s) { return mp_initWithURL(x, self, s, 0); }
id mp_view(Ctx& x, id self, SEL) { return MD(x, self).view; }
id mp_url(Ctx& x, id self, SEL) { return MD(x, self).url; }
void mp_setURL(Ctx& x, id self, SEL, id url) {
    MovieData& m = MD(x, self);
    const id old = m.url;
    m.url = url ? rt(x).retain(x.cpu, url) : 0;
    if (old) rt(x).release(x.cpu, old);
    m.finished = false;
}
id mp_background(Ctx& x, id self, SEL) { MovieData& m = MD(x, self); return m.background ? m.background : m.view; }
void mp_play(Ctx& x, id self, SEL) {
    MovieData& m = MD(x, self);
    if (m.finished) return;
    m.finished = true;
    m.state = 1;   // MPMoviePlaybackStatePlaying, briefly
    later(x, self, 0.05, [self](Ctx& cx) {
        postNote(cx, "MPMoviePlayerLoadStateDidChangeNotification", self, 0);
        postNote(cx, "MPMoviePlayerPlaybackStateDidChangeNotification", self, 0);
    });
    later(x, self, 0.25, [self](Ctx& cx) {
        MD(cx, self).state = 0;
        const id info = Foundation::makeDict(cx, {{Foundation::constString(cx.proc, "MPMoviePlayerPlaybackDidFinishReasonUserInfoKey"),
                                                   Foundation::makeNumberInt(cx, 0 /* MPMovieFinishReasonPlaybackEnded */)}});
        postNote(cx, "MPMoviePlayerPlaybackStateDidChangeNotification", self, 0);
        postNote(cx, "MPMoviePlayerPlaybackDidFinishNotification", self, info);
    });
}
void mp_stop(Ctx& x, id self, SEL) { MD(x, self).state = 0; }
void mp_pause(Ctx& x, id self, SEL) { MD(x, self).state = 2; }
void mp_prepare(Ctx& x, id self, SEL) { later(x, self, 0.02, [self](Ctx& cx) { postNote(cx, "MPMediaPlaybackIsPreparedToPlayDidChangeNotification", self, 0); }); }
bool mp_yes(Ctx&, id, SEL) { return true; }
int32_t mp_state(Ctx& x, id self, SEL) { return MD(x, self).state; }
int32_t mp_loadState(Ctx&, id, SEL) { return 3; }   // playable | playthroughOK
double mp_zero(Ctx&, id, SEL) { return 0; }
void mp_ignoreDouble(Ctx&, id, SEL, double) {}
void mp_ignoreInt(Ctx&, id, SEL, int32_t) {}
void mp_ignoreBool(Ctx&, id, SEL, bool) {}
void mp_setFullscreen(Ctx&, id, SEL, bool, bool) {}
float mp_rate(Ctx&, id, SEL) { return 0; }
void mp_dealloc(Ctx& x, id self, SEL) {
    if (MovieData* m = rt(x).hostIf<MovieData>(self)) for (id o : {m->url, m->view}) if (o) rt(x).release(x.cpu, o);
    rt(x).disposeInstance(x.cpu, self);
}
// MPMoviePlayerViewController owns an MPMoviePlayerController.
struct MovieVCData : ObjC::HostObject { id player = 0; };
id mpvc_initWithURL(Ctx& x, id self, SEL, id url) {
    const id me = id(rt(x).sendSuper(x.cpu, self, rt(x).getClass("UIViewController"), rt(x).sel("init"), {}));
    if (!me) return 0;
    const id player = id(rt(x).send(x.cpu, rt(x).getClass("MPMoviePlayerController"), rt(x).selAlloc));
    rt(x).host<MovieVCData>(me).player = id(rt(x).send(x.cpu, player, "initWithContentURL:", {url}));
    return me;
}
id mpvc_player(Ctx& x, id self, SEL) { return rt(x).host<MovieVCData>(self).player; }
void vc_presentMovie(Ctx& x, id, SEL, id movieVC) {
    if (!movieVC) return;
    const id player = id(rt(x).send(x.cpu, movieVC, "moviePlayer"));
    if (player) rt(x).send(x.cpu, player, "play");
}
void vc_dismissMovie(Ctx&, id, SEL) {}

// ---- MPMusicPlayerController: the iPod app isn't playing (so games play their own music).
id mus_shared(Ctx& x, id cls, SEL) {
    static const char sharedKey = 0; uint32_t& shared = x.proc.processSlot(&sharedKey);
    if (!shared) { shared = rt(x).createInstance(cls); rt(x).makeImmortal(shared); }
    return shared;
}
int32_t mus_zero(Ctx&, id, SEL) { return 0; }
id mus_nil(Ctx&, id, SEL) { return 0; }
float mus_volume(Ctx&, id, SEL) { return 1.0f; }
void mus_noop(Ctx&, id, SEL) {}
void mus_ignoreObj(Ctx&, id, SEL, id) {}
void mus_ignoreInt(Ctx&, id, SEL, int32_t) {}
void mus_ignoreFloat(Ctx&, id, SEL, float) {}

} // namespace

void registerMiscFrameworks(Process& p, ObjC::Runtime& r) {
    ObjC::ClassBuilder wv = r.defineClass("UIWebView", "UIView");
    wv.m<&wv_delegate>("delegate").m<&wv_setDelegate>("setDelegate:", "v@:@").m<&wv_request>("request")
      .m<&wv_loadRequest>("loadRequest:", "v@:@").m<&wv_loadHTML>("loadHTMLString:baseURL:", "v@:@@")
      .m<&wv_loadData>("loadData:MIMEType:textEncodingName:baseURL:", "v@:@@@@")
      .m<&wv_eval>("stringByEvaluatingJavaScriptFromString:")
      .m<&wv_ignoreBool>("setScalesPageToFit:", "v@:c").m<&wv_ignoreBool>("setAllowsInlineMediaPlayback:", "v@:c")
      .m<&wv_ignoreBool>("setMediaPlaybackRequiresUserAction:", "v@:c").m<&wv_ignoreBool>("setSuppressesIncrementalRendering:", "v@:c")
      .m<&wv_ignoreBool>("setKeyboardDisplayRequiresUserAction:", "v@:c").m<&wv_ignoreInt>("setDataDetectorTypes:", "v@:i")
      .m<&wv_noop>("stopLoading", "v@:").m<&wv_noop>("reload", "v@:").m<&wv_noop>("goBack", "v@:").m<&wv_noop>("goForward", "v@:")
      .m<&wv_no>("canGoBack", "c@:").m<&wv_no>("canGoForward", "c@:").m<&wv_no>("isLoading", "c@:")
      .m<&wv_dealloc>("dealloc", "v@:");

    ObjC::ClassBuilder tn = r.defineClass("CTTelephonyNetworkInfo", "NSObject");
    tn.m<&ct_nil>("subscriberCellularProvider").m<&ct_nil>("currentRadioAccessTechnology")
      .m<&ct_ignore>("setSubscriberCellularProviderDidUpdateNotifier:", "v@:@?");
    r.defineClass("CTCarrier", "NSObject");
    for (const char* n : {"_CTRadioAccessTechnologyGPRS", "_CTRadioAccessTechnologyEdge", "_CTRadioAccessTechnologyWCDMA",
                          "_CTRadioAccessTechnologyHSDPA", "_CTRadioAccessTechnologyHSUPA", "_CTRadioAccessTechnologyLTE"}) {
        const uint32_t cell = p.defineDataBlock(n, 4);
        p.mem().write<uint32_t>(cell, Foundation::constString(p, n + 1));
    }

    r.builder(r.getClass("UIViewController"))
        .cm<&vc_attemptRotation>("attemptRotationToDeviceOrientation", "v@:")
        .m<&vc_presentMovie>("presentMoviePlayerViewControllerAnimated:", "v@:@")
        .m<&vc_dismissMovie>("dismissMoviePlayerViewControllerAnimated", "v@:");

    ObjC::ClassBuilder ip = r.defineClass("UIImagePickerController", "UINavigationController");
    ip.cm<&ip_sourceAvailable>("isSourceTypeAvailable:", "c@:i").cm<&ip_mediaTypes>("availableMediaTypesForSourceType:", "@@:i")
      .cm<&ip_cameraAvailable>("isCameraDeviceAvailable:", "c@:i").cm<&ip_cameraAvailable>("isFlashAvailableForCameraDevice:", "c@:i")
      .m<&ip_ignoreInt>("setSourceType:", "v@:i").m<&ip_zero>("sourceType", "i@:").m<&ip_ignoreObj>("setMediaTypes:", "v@:@")
      .m<&ip_ignoreBool>("setAllowsEditing:", "v@:c").m<&ip_ignoreBool>("setAllowsImageEditing:", "v@:c")
      .m<&ip_ignoreInt>("setCameraDevice:", "v@:i").m<&ip_ignoreBool>("setShowsCameraControls:", "v@:c")
      .m<&ip_ignoreObj>("setCameraOverlayView:", "v@:@").m<&ip_ignoreInt>("setVideoQuality:", "v@:i");
    for (const char* n : {"_UIImagePickerControllerOriginalImage", "_UIImagePickerControllerEditedImage", "_UIImagePickerControllerMediaType",
                          "_UIImagePickerControllerCropRect", "_UIImagePickerControllerMediaURL", "_UIImagePickerControllerReferenceURL",
                          "_UIImagePickerControllerMediaMetadata"})
        p.mem().write<uint32_t>(p.defineDataBlock(n, 4), Foundation::constString(p, n + 1));

    ObjC::ClassBuilder lm = r.defineClass("CLLocationManager", "NSObject");
    lm.cm<&locc_no>("locationServicesEnabled", "c@:").cm<&locc_no>("headingAvailable", "c@:")
      .cm<&locc_no>("significantLocationChangeMonitoringAvailable", "c@:").cm<&locc_no>("regionMonitoringAvailable", "c@:")
      .cm<&locc_status>("authorizationStatus", "i@:")
      .m<&loc_delegate>("delegate").m<&loc_setDelegate>("setDelegate:", "v@:@")
      .m<&loc_fail>("startUpdatingLocation", "v@:").m<&loc_fail>("startMonitoringSignificantLocationChanges", "v@:")
      .m<&loc_fail>("requestLocation", "v@:").m<&loc_noop>("stopUpdatingLocation", "v@:")
      .m<&loc_noop>("stopMonitoringSignificantLocationChanges", "v@:").m<&loc_noop>("startUpdatingHeading", "v@:")
      .m<&loc_noop>("stopUpdatingHeading", "v@:").m<&loc_requestAuth>("requestWhenInUseAuthorization", "v@:")
      .m<&loc_requestAuth>("requestAlwaysAuthorization", "v@:")
      .m<&loc_ignoreDouble>("setDesiredAccuracy:", "v@:d").m<&loc_zeroDouble>("desiredAccuracy", "d@:")
      .m<&loc_ignoreDouble>("setDistanceFilter:", "v@:d").m<&loc_zeroDouble>("distanceFilter", "d@:")
      .m<&loc_ignoreObj>("setPurpose:", "v@:@").m<&loc_ignoreInt>("setActivityType:", "v@:i")
      .m<&loc_nil>("location").m<&loc_nil>("heading");
    for (const char* n : {"_kCLErrorDomain"}) p.mem().write<uint32_t>(p.defineDataBlock(n, 4), Foundation::constString(p, n + 1));
    r.defineClass("CLLocation", "NSObject");
    r.defineClass("CLGeocoder", "NSObject");

    ObjC::ClassBuilder mp = r.defineClass("MPMoviePlayerController", "NSObject");
    mp.m<&mp_init>("init").m<&mp_initWithURL>("initWithContentURL:").m<&mp_view>("view").m<&mp_background>("backgroundView")
      .m<&mp_url>("contentURL").m<&mp_setURL>("setContentURL:", "v@:@")
      .m<&mp_play>("play", "v@:").m<&mp_stop>("stop", "v@:").m<&mp_pause>("pause", "v@:").m<&mp_prepare>("prepareToPlay", "v@:")
      .m<&mp_yes>("isPreparedToPlay", "c@:").m<&mp_state>("playbackState", "i@:").m<&mp_loadState>("loadState", "i@:")
      .m<&mp_zero>("currentPlaybackTime", "d@:").m<&mp_ignoreDouble>("setCurrentPlaybackTime:", "v@:d")
      .m<&mp_zero>("duration", "d@:").m<&mp_zero>("playableDuration", "d@:").m<&mp_zero>("initialPlaybackTime", "d@:")
      .m<&mp_ignoreDouble>("setInitialPlaybackTime:", "v@:d").m<&mp_ignoreDouble>("setEndPlaybackTime:", "v@:d")
      .m<&mp_rate>("currentPlaybackRate", "f@:")
      .m<&mp_ignoreInt>("setControlStyle:", "v@:i").m<&mp_ignoreInt>("setMovieControlMode:", "v@:i")
      .m<&mp_ignoreInt>("setScalingMode:", "v@:i").m<&mp_ignoreInt>("setRepeatMode:", "v@:i")
      .m<&mp_ignoreInt>("setMovieSourceType:", "v@:i").m<&mp_ignoreBool>("setShouldAutoplay:", "v@:c")
      .m<&mp_ignoreBool>("setUseApplicationAudioSession:", "v@:c").m<&mp_ignoreBool>("setAllowsAirPlay:", "v@:c")
      .m<&mp_ignoreBool>("setFullscreen:", "v@:c").m<&mp_setFullscreen>("setFullscreen:animated:", "v@:cc")
      .m<&mp_dealloc>("dealloc", "v@:");
    ObjC::ClassBuilder mpvc = r.defineClass("MPMoviePlayerViewController", "UIViewController");
    mpvc.m<&mpvc_initWithURL>("initWithContentURL:").m<&mpvc_player>("moviePlayer");
    ObjC::ClassBuilder mus = r.defineClass("MPMusicPlayerController", "NSObject");
    mus.cm<&mus_shared>("applicationMusicPlayer").cm<&mus_shared>("iPodMusicPlayer").cm<&mus_shared>("systemMusicPlayer")
       .cm<&mus_shared>("applicationQueuePlayer")
       .m<&mus_zero>("playbackState", "i@:").m<&mus_nil>("nowPlayingItem").m<&mus_volume>("volume", "f@:")
       .m<&mus_ignoreFloat>("setVolume:", "v@:f").m<&mus_zero>("shuffleMode", "i@:").m<&mus_zero>("repeatMode", "i@:")
       .m<&mus_ignoreInt>("setShuffleMode:", "v@:i").m<&mus_ignoreInt>("setRepeatMode:", "v@:i")
       .m<&mus_noop>("play", "v@:").m<&mus_noop>("pause", "v@:").m<&mus_noop>("stop", "v@:")
       .m<&mus_noop>("beginGeneratingPlaybackNotifications", "v@:").m<&mus_noop>("endGeneratingPlaybackNotifications", "v@:")
       .m<&mus_ignoreObj>("setQueueWithQuery:", "v@:@").m<&mus_ignoreObj>("setQueueWithItemCollection:", "v@:@")
       .m<&mus_ignoreObj>("setNowPlayingItem:", "v@:@");

    p.def<&ut_copyTag>("_UTTypeCopyPreferredTagWithClass");
    p.def<&ut_createId>("_UTTypeCreatePreferredIdentifierForTag");
    p.def<&ut_conforms>("_UTTypeConformsTo");
    p.def<&ut_equal>("_UTTypeEqual");
    p.def<&ut_copyDescription>("_UTTypeCopyDescription");
    const std::pair<const char*, const char*> utConsts[] = {
        {"_kUTTagClassFilenameExtension", "public.filename-extension"}, {"_kUTTagClassMIMEType", "public.mime-type"},
        {"_kUTTypeImage", "public.image"}, {"_kUTTypeMovie", "public.movie"}, {"_kUTTypeVideo", "public.video"},
        {"_kUTTypeAudio", "public.audio"}, {"_kUTTypePNG", "public.png"}, {"_kUTTypeJPEG", "public.jpeg"},
        {"_kUTTypeGIF", "com.compuserve.gif"}, {"_kUTTypeText", "public.text"}, {"_kUTTypePlainText", "public.plain-text"},
        {"_kUTTypeHTML", "public.html"}, {"_kUTTypeXML", "public.xml"}, {"_kUTTypeData", "public.data"},
        {"_kUTTypeURL", "public.url"}, {"_kUTTypeFileURL", "public.file-url"}, {"_kUTTypeMPEG4", "public.mpeg-4"},
        {"_kUTTypeQuickTimeMovie", "com.apple.quicktime-movie"}, {"_kUTTypePDF", "com.adobe.pdf"}, {"_kUTTypeMP3", "public.mp3"},
        {"_kUTTypeContent", "public.content"}, {"_kUTTypeItem", "public.item"}};
    for (auto& [n, v] : utConsts) p.mem().write<uint32_t>(p.defineDataBlock(n, 4), Foundation::constString(p, v));

    // CaptiveNetwork: no Wi-Fi interface information.
    p.def<&z1>("_CNCopySupportedInterfaces");
    p.def<&z1>("_CNCopyCurrentNetworkInfo");
    // Security: certificates and public-key operations aren't available.
    p.def<&z2>("_SecCertificateCreateWithData");
    p.def<&z1>("_SecCertificateCopySubjectSummary");
    p.def<&z1>("_SecCertificateCopyData");
    p.def<&z1>("_SecKeyGetBlockSize");
    p.def<&secUnimplemented6>("_SecKeyEncrypt");
    p.def<&secUnimplemented6>("_SecKeyDecrypt");
    p.def<&secUnimplemented6>("_SecKeyRawVerify");
    p.def<&secUnimplemented3>("_SecTrustCreateWithCertificates");
    p.def<&secUnimplemented3>("_SecTrustEvaluate");
    p.def<&z1>("_SecTrustCopyPublicKey");
    p.def<&z1>("_SecTrustGetCertificateCount");
    p.def<&z2>("_SecTrustGetCertificateAtIndex");
    p.def<&secUnimplemented3>("_SecPolicyCreateBasicX509");
}

} // namespace Hydrogen::Runtime::UIKit
