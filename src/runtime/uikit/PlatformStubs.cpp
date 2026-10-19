// HydrogenVM — quiet stand-ins for frameworks that apps touch at launch but
// that HydrogenVM doesn't emulate yet (audio, Game Center, StoreKit, mail,
// reachability). They succeed or report "unavailable" the way a device without
// that capability would, and log once, so apps get past startup.
#include "UIKit.hpp"

namespace Hydrogen::Runtime::UIKit {

using Foundation::autoreleased;
using Foundation::makeString;

namespace {

void noteOnce(Ctx& x, const char* what) { x.proc.reportMissing(what); }

// ------------------------------------------------------------- AudioToolbox ---
int32_t as_ok0(Ctx& x) { noteOnce(x, "AudioSession (audio is not emulated yet: sound is silent)"); return 0; }
int32_t as_init(Ctx& x, uint32_t, uint32_t, uint32_t, uint32_t) { return as_ok0(x); }
int32_t as_setProperty(Ctx&, uint32_t, uint32_t, uint32_t) { return 0; }
int32_t as_getProperty(Ctx& x, uint32_t prop, GuestPtr size, GuestPtr out) {
    // kAudioSessionProperty_OtherAudioIsPlaying 'othr' → 0; sample rate etc → sensible values
    if (out && size) {
        const uint32_t n = x.mem.read<uint32_t>(size);
        if (prop == 0x63687372 /* 'chsr' hw sample rate */ && n >= 8) x.mem.write<double>(out, 44100.0);
        else if (n >= 4) x.mem.write<uint32_t>(out, 0);
    }
    return 0;
}
int32_t as_setActive(Ctx&, bool) { return 0; }
int32_t as_addListener(Ctx&, uint32_t, uint32_t, uint32_t) { return 0; }
int32_t ss_create(Ctx& x, uint32_t, GuestPtr outId) { static uint32_t next = 1; if (outId) x.mem.write<uint32_t>(outId, next++); noteOnce(x, "AudioServices system sounds (silent)"); return 0; }
void ss_play(Ctx&, uint32_t) {}
int32_t ss_dispose(Ctx&, uint32_t) { return 0; }
int32_t ss_addCompletion(Ctx&, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }

// ----------------------------------------------------------- AVAudioSession ---
id avs_shared(Ctx& x, id self, SEL) {
    static id shared = 0;
    if (!shared) { shared = rt(x).createInstance(self); rt(x).makeImmortal(shared); }
    return shared;
}
bool avs_yesErr(Ctx&, id, SEL, id, GuestPtr err) { (void)err; return true; }
bool avs_yesBoolErr(Ctx&, id, SEL, bool, GuestPtr) { return true; }
bool avs_yesErr2(Ctx&, id, SEL, id, uint32_t, GuestPtr) { return true; }
bool avs_no(Ctx&, id, SEL) { return false; }
id avs_category(Ctx& x, id, SEL) { return makeString(x, "AVAudioSessionCategorySoloAmbient"); }
void avs_setDelegate(Ctx&, id, SEL, id) {}
double avs_rate(Ctx&, id, SEL) { return 44100; }
float avs_volume(Ctx&, id, SEL) { return 1; }
bool avs_yesDoubleErr(Ctx&, id, SEL, double, GuestPtr) { return true; }

// ------------------------------------------------------------ AVAudioPlayer ---
// Plays nothing but keeps state, so games' audio managers work.
struct PlayerData : ObjC::HostObject { id url = 0, delegate = 0; bool playing = false; float volume = 1, pan = 0; int32_t loops = 0; double time = 0; };
PlayerData& PD(Ctx& x, id p) { return rt(x).host<PlayerData>(p); }
id ap_initURL(Ctx& x, id self, SEL, id url, GuestPtr) {
    noteOnce(x, "AVAudioPlayer (audio is not emulated yet: sound is silent)");
    PD(x, self).url = url ? rt(x).retain(x.cpu, url) : 0;
    return self;
}
id ap_initData(Ctx& x, id self, SEL, id, GuestPtr) { noteOnce(x, "AVAudioPlayer (audio is not emulated yet: sound is silent)"); PD(x, self); return self; }
bool ap_play(Ctx& x, id self, SEL) { PD(x, self).playing = true; return true; }
bool ap_playAt(Ctx& x, id self, SEL, double) { PD(x, self).playing = true; return true; }
void ap_pause(Ctx& x, id self, SEL) { PD(x, self).playing = false; }
void ap_stop(Ctx& x, id self, SEL) { PD(x, self).playing = false; }
bool ap_prepare(Ctx&, id, SEL) { return true; }
bool ap_isPlaying(Ctx& x, id self, SEL) { return PD(x, self).playing; }
float ap_volume(Ctx& x, id self, SEL) { return PD(x, self).volume; }
void ap_setVolume(Ctx& x, id self, SEL, float v) { PD(x, self).volume = v; }
float ap_pan(Ctx& x, id self, SEL) { return PD(x, self).pan; }
void ap_setPan(Ctx& x, id self, SEL, float v) { PD(x, self).pan = v; }
int32_t ap_loops(Ctx& x, id self, SEL) { return PD(x, self).loops; }
void ap_setLoops(Ctx& x, id self, SEL, int32_t n) { PD(x, self).loops = n; }
double ap_time(Ctx& x, id self, SEL) { return PD(x, self).time; }
void ap_setTime(Ctx& x, id self, SEL, double t) { PD(x, self).time = t; }
double ap_duration(Ctx&, id, SEL) { return 0; }
id ap_delegate(Ctx& x, id self, SEL) { return PD(x, self).delegate; }
void ap_setDelegate(Ctx& x, id self, SEL, id d) { PD(x, self).delegate = d; }
id ap_url(Ctx& x, id self, SEL) { return PD(x, self).url; }
void ap_ignoreBool(Ctx&, id, SEL, bool) {}
void ap_ignoreFloat(Ctx&, id, SEL, float) {}
void ap_updateMeters(Ctx&, id, SEL) {}
float ap_power(Ctx&, id, SEL, uint32_t) { return -160; }
uint32_t ap_channels(Ctx&, id, SEL) { return 2; }
void ap_dealloc(Ctx& x, id self, SEL) {
    if (PlayerData* d = rt(x).hostIf<PlayerData>(self)) { const id u = d->url; d->url = 0; if (u) rt(x).release(x.cpu, u); }
    rt(x).disposeInstance(x.cpu, self);
}

// ---------------------------------------------------------------- GameKit ---
id gk_local(Ctx& x, id self, SEL) {
    static id shared = 0;
    if (!shared) { shared = rt(x).createInstance(self); rt(x).makeImmortal(shared); }
    return shared;
}
bool gk_no(Ctx&, id, SEL) { return false; }
void gk_authenticate(Ctx& x, id, SEL, uint32_t handler) {
    noteOnce(x, "Game Center (not available: sign-in fails)");
    if (!handler) return;
    const uint32_t b = ObjC::blockCopy(x, handler);
    Process* p = &x.proc;
    Foundation::mainRunLoop(x.proc).post([p, b](CPU::Cpu& c) {
        Ctx cx{*p, c, p->mem()};
        const id err = id(rt(cx).send(c, rt(cx).getClass("NSError"), "errorWithDomain:code:userInfo:",
                                      {makeString(cx, "GKErrorDomain"), 15, 0}));   // GKErrorGameUnrecognized-ish
        ObjC::callBlock(cx, b, {err});
        ObjC::blockRelease(cx, b);
    });
}
void gk_setHandler(Ctx& x, id self, SEL s, uint32_t handler) {
    // iOS 6 authenticateHandler: (UIViewController *vc, NSError *error)
    if (!handler) return;
    noteOnce(x, "Game Center (not available: sign-in fails)");
    const uint32_t b = ObjC::blockCopy(x, handler);
    Process* p = &x.proc;
    Foundation::mainRunLoop(x.proc).post([p, b](CPU::Cpu& c) {
        Ctx cx{*p, c, p->mem()};
        const id err = id(rt(cx).send(c, rt(cx).getClass("NSError"), "errorWithDomain:code:userInfo:", {makeString(cx, "GKErrorDomain"), 2, 0}));
        ObjC::callBlock(cx, b, {0, err});
        ObjC::blockRelease(cx, b);
    });
    (void)self; (void)s;
}
id gk_nil(Ctx&, id, SEL) { return 0; }

// --------------------------------------------------------------- StoreKit ---
bool sk_canPay(Ctx& x, id, SEL) { noteOnce(x, "StoreKit (in-app purchases are disabled)"); return false; }
id sk_queue(Ctx& x, id self, SEL) {
    static id shared = 0;
    if (!shared) { shared = rt(x).createInstance(self); rt(x).makeImmortal(shared); }
    return shared;
}
void sk_ignoreObj(Ctx&, id, SEL, id) {}
void sk_noop(Ctx&, id, SEL) {}
id sk_empty(Ctx& x, id, SEL) { return Foundation::makeArray(x, {}); }

// ------------------------------------------------------------- MessageUI ---
bool mf_no(Ctx&, id, SEL) { return false; }

// ---------------------------------------------------- SystemConfiguration ---
uint32_t scr_create(Ctx& x, uint32_t, GuestPtr) { noteOnce(x, "SCNetworkReachability (network reported offline)"); return x.proc.heap().allocZeroed(16); }
uint32_t scr_createWithAddress(Ctx& x, uint32_t a, GuestPtr p) { return scr_create(x, a, p); }
bool scr_getFlags(Ctx& x, uint32_t, GuestPtr flags) { if (flags) x.mem.write<uint32_t>(flags, 0); return true; }
bool scr_setCallback(Ctx&, uint32_t, uint32_t, uint32_t) { return true; }
bool scr_schedule(Ctx&, uint32_t, uint32_t, uint32_t) { return true; }

} // namespace

void registerPlatformStubs(Process& p, ObjC::Runtime& r) {
    p.def<&as_init>("_AudioSessionInitialize");
    p.def<&as_setProperty>("_AudioSessionSetProperty");
    p.def<&as_getProperty>("_AudioSessionGetProperty");
    p.def<&as_getProperty>("_AudioSessionGetPropertySize");
    p.def<&as_setActive>("_AudioSessionSetActive");
    p.def<&as_addListener>("_AudioSessionAddPropertyListener");
    p.def<&as_addListener>("_AudioSessionRemovePropertyListenerWithUserData");
    p.def<&ss_create>("_AudioServicesCreateSystemSoundID");
    p.def<&ss_play>("_AudioServicesPlaySystemSound");
    p.def<&ss_play>("_AudioServicesPlayAlertSound");
    p.def<&ss_dispose>("_AudioServicesDisposeSystemSoundID");
    p.def<&ss_addCompletion>("_AudioServicesAddSystemSoundCompletion");

    ObjC::ClassBuilder avs = r.defineClass("AVAudioSession", "NSObject");
    avs.cm<&avs_shared>("sharedInstance");
    avs.m<&avs_yesErr>("setCategory:error:", "c@:@^@").m<&avs_yesErr2>("setCategory:withOptions:error:", "c@:@I^@")
       .m<&avs_yesBoolErr>("setActive:error:", "c@:c^@").m<&avs_yesErr2>("setActive:withOptions:error:", "c@:cI^@")
       .m<&avs_yesErr>("setMode:error:", "c@:@^@").m<&avs_category>("category").m<&avs_no>("isOtherAudioPlaying", "c@:")
       .m<&avs_no>("secondaryAudioShouldBeSilencedHint", "c@:").m<&avs_setDelegate>("setDelegate:", "v@:@")
       .m<&avs_rate>("sampleRate", "d@:").m<&avs_rate>("currentHardwareSampleRate", "d@:").m<&avs_rate>("preferredSampleRate", "d@:")
       .m<&avs_yesDoubleErr>("setPreferredSampleRate:error:", "c@:d^@").m<&avs_yesDoubleErr>("setPreferredHardwareSampleRate:error:", "c@:d^@")
       .m<&avs_yesDoubleErr>("setPreferredIOBufferDuration:error:", "c@:d^@").m<&avs_volume>("outputVolume", "f@:");
    for (const char* n : {"AVAudioSessionCategoryAmbient", "AVAudioSessionCategorySoloAmbient", "AVAudioSessionCategoryPlayback",
                          "AVAudioSessionCategoryRecord", "AVAudioSessionCategoryPlayAndRecord", "AVAudioSessionModeDefault",
                          "AVAudioSessionInterruptionNotification", "AVAudioSessionRouteChangeNotification"}) {
        const uint32_t cell = p.defineDataBlock(std::string("_") + n, 4);
        p.mem().write<uint32_t>(cell, Foundation::constString(p, n));
    }

    ObjC::ClassBuilder ap = r.defineClass("AVAudioPlayer", "NSObject");
    ap.m<&ap_initURL>("initWithContentsOfURL:error:").m<&ap_initData>("initWithData:error:").m<&ap_play>("play", "c@:")
      .m<&ap_playAt>("playAtTime:", "c@:d").m<&ap_pause>("pause", "v@:").m<&ap_stop>("stop", "v@:").m<&ap_prepare>("prepareToPlay", "c@:")
      .m<&ap_isPlaying>("isPlaying", "c@:").m<&ap_volume>("volume", "f@:").m<&ap_setVolume>("setVolume:", "v@:f")
      .m<&ap_pan>("pan", "f@:").m<&ap_setPan>("setPan:", "v@:f").m<&ap_loops>("numberOfLoops", "i@:").m<&ap_setLoops>("setNumberOfLoops:", "v@:i")
      .m<&ap_time>("currentTime", "d@:").m<&ap_setTime>("setCurrentTime:", "v@:d").m<&ap_duration>("duration", "d@:")
      .m<&ap_delegate>("delegate").m<&ap_setDelegate>("setDelegate:", "v@:@").m<&ap_url>("url")
      .m<&ap_ignoreBool>("setMeteringEnabled:", "v@:c").m<&ap_ignoreBool>("setEnableRate:", "v@:c").m<&ap_ignoreFloat>("setRate:", "v@:f")
      .m<&ap_updateMeters>("updateMeters", "v@:").m<&ap_power>("averagePowerForChannel:", "f@:I").m<&ap_power>("peakPowerForChannel:", "f@:I")
      .m<&ap_channels>("numberOfChannels", "I@:").m<&ap_dealloc>("dealloc", "v@:");

    ObjC::ClassBuilder gk = r.defineClass("GKLocalPlayer", "NSObject");
    gk.cm<&gk_local>("localPlayer");
    gk.m<&gk_no>("isAuthenticated", "c@:").m<&gk_authenticate>("authenticateWithCompletionHandler:", "v@:@?")
      .m<&gk_setHandler>("setAuthenticateHandler:", "v@:@?").m<&gk_nil>("playerID").m<&gk_nil>("alias").m<&gk_nil>("displayName")
      .m<&gk_no>("isUnderage", "c@:");

    ObjC::ClassBuilder skq = r.defineClass("SKPaymentQueue", "NSObject");
    skq.cm<&sk_canPay>("canMakePayments", "c@:").cm<&sk_queue>("defaultQueue");
    skq.m<&sk_ignoreObj>("addTransactionObserver:", "v@:@").m<&sk_ignoreObj>("removeTransactionObserver:", "v@:@")
       .m<&sk_ignoreObj>("addPayment:", "v@:@").m<&sk_ignoreObj>("finishTransaction:", "v@:@")
       .m<&sk_noop>("restoreCompletedTransactions", "v@:").m<&sk_empty>("transactions");

    for (const char* n : {"MFMailComposeViewController", "MFMessageComposeViewController"}) {
        ObjC::ClassBuilder mf = r.defineClass(n, "UIViewController");
        mf.cm<&mf_no>(std::string(n) == "MFMailComposeViewController" ? "canSendMail" : "canSendText", "c@:");
    }

    p.def<&scr_create>("_SCNetworkReachabilityCreateWithName");
    p.def<&scr_createWithAddress>("_SCNetworkReachabilityCreateWithAddress");
    p.def<&scr_getFlags>("_SCNetworkReachabilityGetFlags");
    p.def<&scr_setCallback>("_SCNetworkReachabilitySetCallback");
    p.def<&scr_schedule>("_SCNetworkReachabilityScheduleWithRunLoop");
    p.def<&scr_schedule>("_SCNetworkReachabilityUnscheduleFromRunLoop");
}

} // namespace Hydrogen::Runtime::UIKit
