#if defined(__APPLE__)
#import <Foundation/Foundation.h>
#include "HostPredicateRegex.hpp"
#include <stdexcept>
namespace Hydrogen::Runtime::Foundation {
namespace {
struct Regex final : HostPredicateRegex {
    __strong NSPredicate *predicate;
    Regex(const std::string& source,bool insensitive) {
        @autoreleasepool {
            NSString *pattern=[[NSString alloc] initWithBytes:source.data() length:source.size() encoding:NSUTF8StringEncoding];
            NSError *error=nil;
            if(!pattern || ![NSRegularExpression regularExpressionWithPattern:pattern options:insensitive?NSRegularExpressionCaseInsensitive:0 error:&error])
                throw std::invalid_argument("invalid MATCHES regular expression");
            predicate=[NSPredicate predicateWithFormat:insensitive?@"SELF MATCHES[c] %@":@"SELF MATCHES %@",pattern];
        }
    }
    bool matches(const std::string& input) const override {
        @autoreleasepool {
            NSString *text=[[NSString alloc] initWithBytes:input.data() length:input.size() encoding:NSUTF8StringEncoding];
            return text && [predicate evaluateWithObject:text];
        }
    }
};
}
std::shared_ptr<const HostPredicateRegex> compilePredicateRegex(const std::string& pattern,bool insensitive) {
    return std::make_shared<Regex>(pattern,insensitive);
}
}
#endif
