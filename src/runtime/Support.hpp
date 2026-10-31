// Explicit support declarations. Unregistered metadata means unknown, not Real.
// Kept outside Process/ObjC dispatch so independent cards can adopt it gradually.
#pragma once
#include "Process.hpp"
#include <map>
#include <optional>
#include <stdexcept>

namespace Hydrogen::Runtime {

enum class SupportStatus { Real, Approx, Stub, Unavailable };
enum class SupportKind { Function, InstanceMethod, ClassMethod };

inline const char* supportStatusName(SupportStatus status) {
    switch (status) {
    case SupportStatus::Real: return "Real";
    case SupportStatus::Approx: return "Approx";
    case SupportStatus::Stub: return "Stub";
    case SupportStatus::Unavailable: return "Unavailable";
    }
    throw std::invalid_argument("invalid support status");
}

struct SupportRecord {
    SupportKind kind;
    std::string name; // exported C symbol, or -[Class selector] / +[Class selector]
    SupportStatus status;
    std::string provenance;
};

class SupportRegistry {
public:
    explicit SupportRegistry(Process&) {}

    static void validate(const SupportRecord& r) {
        (void)supportStatusName(r.status);
        if (r.name.empty() || r.provenance.find_first_not_of(" \t\r\n") == std::string::npos ||
            r.provenance.find_first_of("\r\n") != std::string::npos)
            throw std::invalid_argument("support declaration requires a name and one-line provenance");
    }

    void record(SupportRecord r) {
        validate(r);
        std::lock_guard<std::mutex> lock(mu_);
        records_.insert_or_assign(r.name, r);
    }

    std::optional<SupportRecord> find(const std::string& name) const {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = records_.find(name);
        if (it == records_.end()) return std::nullopt;
        return it->second;
    }

    std::vector<SupportRecord> snapshot() const {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<SupportRecord> result;
        for (const auto& [_, record] : records_) result.push_back(record);
        return result;
    }

private:
    mutable std::mutex mu_;
    std::map<std::string, SupportRecord> records_;
};

inline SupportRegistry& supportRegistry(Process& p) { return p.service<SupportRegistry>(); }

template <auto F>
uint32_t supportDef(Process& p, const std::string& symbol, SupportStatus status,
                    const std::string& provenance) {
    SupportRecord r{SupportKind::Function, symbol, status, provenance};
    SupportRegistry::validate(r);
    const uint32_t address = p.def<F>(symbol);
    supportRegistry(p).record(std::move(r));
    return address;
}

// Builder is intentionally a template to avoid an ObjC.hpp include cycle.
template <typename Builder, typename Fn>
Builder& supportMethod(Process& p, Builder& builder, const std::string& className,
                       const std::string& selector, Fn fn, SupportStatus status,
                       const std::string& provenance, const std::string& types = "@@:",
                       bool classMethod = false) {
    SupportRecord r{classMethod ? SupportKind::ClassMethod : SupportKind::InstanceMethod,
                    std::string(classMethod ? "+[" : "-[") + className + " " + selector + "]",
                    status, provenance};
    SupportRegistry::validate(r);
    if (classMethod) builder.classMethod(selector, std::move(fn), types);
    else builder.method(selector, std::move(fn), types);
    supportRegistry(p).record(std::move(r));
    return builder;
}

} // namespace Hydrogen::Runtime
