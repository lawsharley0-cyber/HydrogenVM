#pragma once
#include <memory>
#include <string>
namespace Hydrogen::Runtime::Foundation {
// Predicate MATCHES uses ICU semantics, including Unicode categories and
// inline flags. Apple delegates to native Foundation; other hosts use ICU.
class HostPredicateRegex {
public:
    virtual ~HostPredicateRegex() = default;
    virtual bool matches(const std::string&) const = 0;
};
std::shared_ptr<const HostPredicateRegex> compilePredicateRegex(const std::string&,bool insensitive);
}
