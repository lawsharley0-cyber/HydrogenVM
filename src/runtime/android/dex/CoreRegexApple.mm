#if defined(__APPLE__)
#import <Foundation/Foundation.h>
#include "runtime/android/dex/CoreRegex.hpp"

namespace Hydrogen::Runtime::Android::Dex {
namespace {

NSString* toNSString(std::u16string_view s) {
    return [[NSString alloc] initWithCharacters:reinterpret_cast<const unichar*>(s.data()) length:s.size()];
}

std::u16string fromNSString(NSString* s) {
    if (!s) return {};
    const NSUInteger len = s.length;
    std::u16string out(len, u'\0');
    [s getCharacters:reinterpret_cast<unichar*>(out.data()) range:NSMakeRange(0, len)];
    return out;
}

NSRegularExpressionOptions toAppleOptions(int32_t javaFlags) {
    NSRegularExpressionOptions opts = 0;
    if (javaFlags & 0x01) opts |= NSRegularExpressionUseUnixLineSeparators;
    if (javaFlags & 0x02) opts |= NSRegularExpressionCaseInsensitive;
    if (javaFlags & 0x04) opts |= NSRegularExpressionAllowCommentsAndWhitespace;
    if (javaFlags & 0x08) opts |= NSRegularExpressionAnchorsMatchLines;
    if (javaFlags & 0x10) opts |= NSRegularExpressionIgnoreMetacharacters;
    if (javaFlags & 0x20) opts |= NSRegularExpressionDotMatchesLineSeparators;
    return opts;
}

class AppleRegexPattern final : public HostRegexPattern {
public:
    AppleRegexPattern(std::u16string_view pattern, int32_t javaFlags) {
        @autoreleasepool {
            NSError* err = nil;
            regex_ = [NSRegularExpression regularExpressionWithPattern:toNSString(pattern)
                                                               options:toAppleOptions(javaFlags)
                                                                 error:&err];
            if (!regex_ || err) throw JavaException{u"Ljava/util/regex/PatternSyntaxException;"};
        }
    }

    int32_t groupCount() const override { return int32_t(regex_.numberOfCaptureGroups); }

    bool matches(std::u16string_view input, std::vector<RegexGroup>& groups) const override {
        @autoreleasepool {
            NSString* text = toNSString(input);
            NSTextCheckingResult* match = [regex_ firstMatchInString:text options:0 range:NSMakeRange(0, text.length)];
            if (!match || match.range.location != 0 || match.range.length != text.length) return false;
            capture(match, groups);
            return true;
        }
    }

    bool find(std::u16string_view input, size_t startPos, std::vector<RegexGroup>& groups) const override {
        @autoreleasepool {
            if (startPos > input.size()) return false;
            NSString* text = toNSString(input);
            NSTextCheckingResult* match = [regex_ firstMatchInString:text
                                                             options:0
                                                               range:NSMakeRange(startPos, text.length - startPos)];
            if (!match) return false;
            capture(match, groups);
            return true;
        }
    }

    std::u16string replace(std::u16string_view input, std::u16string_view replacement,
                           bool firstOnly) const override {
        @autoreleasepool {
            NSString* text = toNSString(input);
            NSString* templ = toNSString(replacement);
            if (!firstOnly) {
                return fromNSString([regex_ stringByReplacingMatchesInString:text
                                                                     options:0
                                                                       range:NSMakeRange(0, text.length)
                                                                withTemplate:templ]);
            }
            NSTextCheckingResult* match = [regex_ firstMatchInString:text options:0 range:NSMakeRange(0, text.length)];
            if (!match) return std::u16string(input);
            NSString* rep = [regex_ replacementStringForResult:match inString:text offset:0 template:templ];
            NSMutableString* mut = [text mutableCopy];
            [mut replaceCharactersInRange:match.range withString:rep];
            return fromNSString(mut);
        }
    }

private:
    static void capture(NSTextCheckingResult* match, std::vector<RegexGroup>& groups) {
        groups.clear();
        const NSUInteger n = match.numberOfRanges;
        groups.reserve(n);
        for (NSUInteger i = 0; i < n; ++i) {
            const NSRange r = [match rangeAtIndex:i];
            if (r.location == NSNotFound) groups.push_back({false, 0, 0});
            else groups.push_back({true, size_t(r.location), size_t(r.location + r.length)});
        }
    }

    __strong NSRegularExpression* regex_;
};

} // namespace

std::shared_ptr<const HostRegexPattern> compileHostRegex(std::u16string_view pattern, int32_t javaFlags) {
    return std::make_shared<AppleRegexPattern>(pattern, javaFlags);
}

} // namespace Hydrogen::Runtime::Android::Dex
#endif
