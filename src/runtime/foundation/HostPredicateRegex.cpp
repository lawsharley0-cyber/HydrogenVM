#include "HostPredicateRegex.hpp"
#if !defined(__APPLE__)
#include <unicode/regex.h>
#include <stdexcept>
namespace Hydrogen::Runtime::Foundation {
namespace {
struct Regex final : HostPredicateRegex {
    std::unique_ptr<icu::RegexPattern> pattern;
    Regex(const std::string& source,bool insensitive) {
        UErrorCode status=U_ZERO_ERROR;UParseError location{};
        pattern.reset(icu::RegexPattern::compile(icu::UnicodeString::fromUTF8(source),
            insensitive?UREGEX_CASE_INSENSITIVE:0,location,status));
        if(U_FAILURE(status) || !pattern)throw std::invalid_argument("invalid MATCHES regular expression");
    }
    bool matches(const std::string& input) const override {
        UErrorCode status=U_ZERO_ERROR;
        // RegexMatcher retains a reference to the input string.
        const auto text=icu::UnicodeString::fromUTF8(input);
        std::unique_ptr<icu::RegexMatcher> matcher(pattern->matcher(text,status));
        if(U_FAILURE(status) || !matcher)throw std::runtime_error("MATCHES matcher allocation failed");
        const bool result=matcher->matches(status);
        if(U_FAILURE(status))throw std::runtime_error("MATCHES evaluation failed");
        return result;
    }
};
}
std::shared_ptr<const HostPredicateRegex> compilePredicateRegex(const std::string& pattern,bool insensitive) {
    return std::make_shared<Regex>(pattern,insensitive);
}
}
#endif
