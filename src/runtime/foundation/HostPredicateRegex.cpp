#include "HostPredicateRegex.hpp"
#if !defined(__APPLE__)
// ICU's C API (uregex), so the build depends only on ICU's stable C ABI.
#include <unicode/uregex.h>
#include <unicode/ustring.h>
#include <mutex>
#include <stdexcept>
#include <vector>
namespace Hydrogen::Runtime::Foundation {
namespace {
std::vector<UChar> toUtf16(const std::string& s) {
    UErrorCode status = U_ZERO_ERROR;
    int32_t length = 0;
    u_strFromUTF8(nullptr, 0, &length, s.data(), int32_t(s.size()), &status);
    if (status != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status)) throw std::invalid_argument("invalid UTF-8");
    std::vector<UChar> out(size_t(length) + 1);
    status = U_ZERO_ERROR;
    u_strFromUTF8(out.data(), int32_t(out.size()), &length, s.data(), int32_t(s.size()), &status);
    if (U_FAILURE(status)) throw std::invalid_argument("invalid UTF-8");
    out.resize(size_t(length));
    return out;
}
struct Regex final : HostPredicateRegex {
    URegularExpression* re = nullptr;
    mutable std::mutex mu;   // a URegularExpression holds its matcher state
    Regex(const std::string& source, bool insensitive) {
        const auto pattern = toUtf16(source);
        UErrorCode status = U_ZERO_ERROR;
        UParseError location{};
        re = uregex_open(pattern.data(), int32_t(pattern.size()), insensitive ? UREGEX_CASE_INSENSITIVE : 0, &location, &status);
        if (U_FAILURE(status) || !re) {
            if (re) uregex_close(re);
            throw std::invalid_argument("invalid MATCHES regular expression");
        }
    }
    ~Regex() override { if (re) uregex_close(re); }
    bool matches(const std::string& input) const override {
        const auto text = toUtf16(input);   // uregex keeps a pointer to the text while matching
        std::lock_guard<std::mutex> lk(mu);
        UErrorCode status = U_ZERO_ERROR;
        uregex_setText(re, text.data(), int32_t(text.size()), &status);
        const bool result = U_SUCCESS(status) && uregex_matches(re, 0, &status);
        if (U_FAILURE(status)) throw std::runtime_error("MATCHES evaluation failed");
        return result;
    }
};
}
std::shared_ptr<const HostPredicateRegex> compilePredicateRegex(const std::string& pattern, bool insensitive) {
    return std::make_shared<Regex>(pattern, insensitive);
}
}
#endif
