// HydrogenVM — the iPhone's HTTP client for a game's Foundation downloads, on the host's own
// NSURLSession. See src/runtime/libc/HttpClient.hpp. Redirects are followed and gzip bodies
// decoded by NSURLSession; nothing is cached or kept in cookies between requests.
#import <Foundation/Foundation.h>

#include "HttpClientApple.h"

namespace {

struct AppleHttpClient final : Hydrogen::Runtime::HttpClient {
    NSURLSession* session = nil;
    AppleHttpClient() {
        NSURLSessionConfiguration* c = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        c.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
        c.HTTPCookieStorage = nil;
        c.URLCache = nil;
        session = [NSURLSession sessionWithConfiguration:c];
    }
    ~AppleHttpClient() override { [session finishTasksAndInvalidate]; }

    void fetch(Hydrogen::Runtime::HttpRequest rq, std::function<void(Hydrogen::Runtime::HttpResponse)> done) override {
        NSURL* url = [NSURL URLWithString:[NSString stringWithUTF8String:rq.url.c_str()] ?: @""];
        if (!url) {
            Hydrogen::Runtime::HttpResponse r;
            r.url = rq.url;
            r.errorCode = -1002;
            r.error = "unsupported URL";
            done(std::move(r));
            return;
        }
        NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:url
                                                           cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                                       timeoutInterval:rq.timeoutSeconds > 0 ? rq.timeoutSeconds : 60];
        req.HTTPMethod = [NSString stringWithUTF8String:rq.method.c_str()];
        for (const auto& [k, v] : rq.headers) {
            NSString* key = [NSString stringWithUTF8String:k.c_str()];
            NSString* value = [NSString stringWithUTF8String:v.c_str()];
            if (key && value) [req setValue:value forHTTPHeaderField:key];
        }
        if (!rq.body.empty()) req.HTTPBody = [NSData dataWithBytes:rq.body.data() length:rq.body.size()];
        auto callback = std::make_shared<std::function<void(Hydrogen::Runtime::HttpResponse)>>(std::move(done));
        const std::string requested = rq.url;
        NSURLSessionDataTask* task = [session dataTaskWithRequest:req
                                                completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
            Hydrogen::Runtime::HttpResponse r;
            r.url = response.URL.absoluteString ? std::string(response.URL.absoluteString.UTF8String) : requested;
            if (error && !response) {
                r.errorCode = int(error.code);
                r.error = error.localizedDescription.UTF8String ?: "network error";
            } else if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
                NSHTTPURLResponse* http = (NSHTTPURLResponse*)response;
                r.status = int(http.statusCode);
                [http.allHeaderFields enumerateKeysAndObjectsUsingBlock:^(id key, id value, BOOL*) {
                    NSString* k = [key description], *v = [value description];
                    // NSURLSession already decoded the body: its size, not the wire's.
                    if ([k caseInsensitiveCompare:@"Content-Encoding"] == NSOrderedSame) return;
                    if ([k caseInsensitiveCompare:@"Content-Length"] == NSOrderedSame) return;
                    if ([k caseInsensitiveCompare:@"Transfer-Encoding"] == NSOrderedSame) return;
                    r.headers.push_back({k.UTF8String ?: "", v.UTF8String ?: ""});
                }];
            } else {
                r.status = 200;   // a non-HTTP URL the session could load
            }
            if (data.length) r.body.assign(static_cast<const char*>(data.bytes), data.length);
            if (r.status) r.headers.push_back({"Content-Length", std::to_string(r.body.size())});
            (*callback)(std::move(r));
        }];
        [task resume];
    }
};

}  // namespace

namespace Hydrogen::App {
std::shared_ptr<Hydrogen::Runtime::HttpClient> makeAppleHttpClient() { return std::make_shared<AppleHttpClient>(); }
}
