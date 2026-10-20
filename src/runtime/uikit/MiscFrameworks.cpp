// HydrogenVM — small framework pieces apps touch at launch:
//   UIWebView (no web engine: remote loads fail offline, HTML loads "finish"),
//   CoreTelephony (no carrier), SystemConfiguration's CaptiveNetwork (no Wi-Fi info),
//   Security certificates/keys (unavailable), +attemptRotationToDeviceOrientation.
#include "UIKit.hpp"

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

    r.builder(r.getClass("UIViewController")).cm<&vc_attemptRotation>("attemptRotationToDeviceOrientation", "v@:");

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
