// SPDX-FileCopyrightText: 2026 Helmholtz-Zentrum Berlin für Materialien und Energie GmbH
// SPDX-License-Identifier: MIT

/**
 * @file  access_control.cpp
 * @brief CIDR parsing, the four ACL strategies, validation and AclFilter.
 */

#include "access_control.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <utility>

// Shared switch defined and exported by emc_server.cpp.
extern int emc_server_debug;

#define Debug(L, ...) do { \
    if ((L) <= emc_server_debug) std::fprintf(stderr, __VA_ARGS__); \
} while (0)

const size_t AclConfig::kMaxRules;

// ---------------------------------------------------------------------------
// Ipv4Network
// ---------------------------------------------------------------------------
epicsUInt32 Ipv4Network::mask() const {
    if (prefix > 32u) {
        throw std::invalid_argument("IPv4 prefix length out of range 0..32");
    }
    // A shift by 32 is undefined, so /0 is handled explicitly.
    if (prefix == 0) return 0u;
    return static_cast<epicsUInt32>(0xFFFFFFFFu << (32u - prefix));
}

bool Ipv4Network::contains(epicsUInt32 host) const {
    return (host & mask()) == addr;
}

std::string Ipv4Network::str() const {
    char text[4];
    std::snprintf(text, sizeof(text), "%u", prefix);
    return formatIpv4(addr) + "/" + text;
}

namespace {

void validateNetwork(const Ipv4Network& net) {
    // mask() checks the prefix before performing any shift.
    if ((net.addr & ~net.mask()) != 0u) {
        throw std::invalid_argument("IPv4 network address has host bits set");
    }
}

bool isDecimalDigit(char c) { return c >= '0' && c <= '9'; }

// Reads one decimal number of at most maxDigits digits without a leading zero.
bool readDecimal(const char*& p, unsigned maxDigits, unsigned& value, std::string& error,
                 const char* what) {
    if (!isDecimalDigit(*p)) {
        error = std::string("expected a decimal ") + what;
        return false;
    }
    if (*p == '0' && isDecimalDigit(p[1])) {
        // "010" would be octal to inet_aton(); refuse the ambiguity.
        error = std::string("leading zeros are not allowed in the ") + what;
        return false;
    }
    unsigned v = 0, digits = 0;
    while (isDecimalDigit(*p)) {
        if (++digits > maxDigits) {
            error = std::string(what) + " has too many digits";
            return false;
        }
        v = v * 10u + static_cast<unsigned>(*p - '0');
        ++p;
    }
    value = v;
    return true;
}

} // namespace

bool Ipv4Network::parse(const char* text, Ipv4Network& out, std::string& error) {
    if (!text || !*text) {
        error = "missing address";
        return false;
    }
    const char* p = text;
    epicsUInt32 addr = 0;
    for (int i = 0; i < 4; ++i) {
        if (i > 0) {
            if (*p != '.') {
                error = "expected four dot-separated octets (a.b.c.d)";
                return false;
            }
            ++p;
        }
        unsigned octet = 0;
        if (!readDecimal(p, 3, octet, error, "octet")) return false;
        if (octet > 255u) {
            error = "octet out of range 0..255";
            return false;
        }
        addr = (addr << 8) | static_cast<epicsUInt32>(octet);
    }

    unsigned prefix = 32;
    if (*p == '/') {
        ++p;
        if (!readDecimal(p, 2, prefix, error, "prefix length")) return false;
        if (prefix > 32u) {
            error = "prefix length out of range 0..32";
            return false;
        }
    }
    if (*p != '\0') {
        error = "unexpected characters after the address";
        return false;
    }

    Ipv4Network net;
    net.addr = addr;
    net.prefix = prefix;
    if ((addr & ~net.mask()) != 0u) {
        // "10.0.0.5/8" is usually a typo in either the address or the prefix.
        Ipv4Network masked = net;
        masked.addr = addr & net.mask();
        error = "host bits are set; did you mean " + masked.str() + " or " +
                formatIpv4(addr) + "/32?";
        return false;
    }
    out = net;
    return true;
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------
const char* aclActionName(AclAction action) {
    return action == AclAction::Allow ? "allow" : "deny";
}

const char* aclModeName(AclMode mode) {
    switch (mode) {
    case AclMode::Allowlist:   return "allowlist";
    case AclMode::Denylist:    return "denylist";
    case AclMode::DefaultDeny: return "default-deny";
    case AclMode::AllowAll:    return "allow-all";
    }
    return "default-deny";
}

bool parseAclMode(const char* text, AclMode& out) {
    if (!text) return false;
    std::string key;
    for (const char* p = text; *p; ++p) {
        key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*p))));
    }
    if (key == "allowlist")                             { out = AclMode::Allowlist;   return true; }
    if (key == "denylist")                              { out = AclMode::Denylist;    return true; }
    if (key == "default-deny" || key == "defaultdeny")  { out = AclMode::DefaultDeny; return true; }
    if (key == "allow-all" || key == "allowall")        { out = AclMode::AllowAll;    return true; }
    return false;
}

// ---------------------------------------------------------------------------
// Strategies
// ---------------------------------------------------------------------------
AllowlistPolicy::AllowlistPolicy(std::vector<Ipv4Network> allowed)
: allowed_(std::move(allowed)) {
    for (const Ipv4Network& net : allowed_) {
        validateNetwork(net);
    }
}

bool AllowlistPolicy::isAllowed(epicsUInt32 addr) const {
    return std::any_of(allowed_.begin(), allowed_.end(),
                       [addr](const Ipv4Network& net) { return net.contains(addr); });
}

DenylistPolicy::DenylistPolicy(std::vector<Ipv4Network> denied)
: denied_(std::move(denied)) {
    for (const Ipv4Network& net : denied_) {
        validateNetwork(net);
    }
}

bool DenylistPolicy::isAllowed(epicsUInt32 addr) const {
    return !std::any_of(denied_.begin(), denied_.end(),
                        [addr](const Ipv4Network& net) { return net.contains(addr); });
}

DefaultDenyPolicy::DefaultDenyPolicy(std::vector<AclRule> rules)
: rules_(std::move(rules)) {
    for (const AclRule& rule : rules_) {
        validateNetwork(rule.net);
    }
}

bool DefaultDenyPolicy::isAllowed(epicsUInt32 addr) const {
    int  bestPrefix = -1;
    bool allowed = false;
    for (const AclRule& rule : rules_) {
        if (!rule.net.contains(addr)) continue;
        const int prefix = static_cast<int>(rule.net.prefix);
        if (prefix > bestPrefix) {
            bestPrefix = prefix;
            allowed = (rule.action == AclAction::Allow);
        } else if (prefix == bestPrefix && rule.action == AclAction::Deny) {
            allowed = false;   // equally specific allow and deny: deny wins
        }
    }
    return bestPrefix >= 0 && allowed;
}

// ---------------------------------------------------------------------------
// AclConfig
// ---------------------------------------------------------------------------
bool AclConfig::addRule(AclAction action, const char* cidr, std::string& error) {
    Ipv4Network net;
    if (!Ipv4Network::parse(cidr, net, error)) {
        ++errors_;
        return false;
    }
    if (std::any_of(rules_.begin(), rules_.end(), [&](const AclRule& rule) {
            return rule.action == action && rule.net == net;
        })) {
        return true;  // duplicate
    }
    if (rules_.size() >= kMaxRules) {
        error = "too many rules (maximum 256); use broader networks";
        ++errors_;
        return false;
    }
    rules_.push_back(AclRule{action, net});
    return true;
}

void AclConfig::clear() {
    rules_.clear();
    errors_ = 0;
}

namespace {

size_t countRules(const AclConfig& config, AclAction action) {
    const auto& rules = config.rules();
    return static_cast<size_t>(std::count_if(rules.begin(), rules.end(),
        [action](const AclRule& rule) { return rule.action == action; }));
}

const AclRule* firstRule(const AclConfig& config, AclAction action) {
    const auto& rules = config.rules();
    const auto it = std::find_if(rules.begin(), rules.end(),
        [action](const AclRule& rule) { return rule.action == action; });
    return it != rules.end() ? &*it : nullptr;
}

} // namespace

std::unique_ptr<AccessPolicy> makeAccessPolicy(const AclConfig& config, std::string& error) {
    if (config.errorCount() > 0) {
        char count[16];
        std::snprintf(count, sizeof(count), "%u", config.errorCount());
        error = std::string(count) + " ACL command(s) failed; correct them, run emcAclClear "
                "and enter the rules again";
        return nullptr;
    }

    const std::vector<AclRule>& rules = config.rules();
    switch (config.mode()) {
    case AclMode::AllowAll:
        if (!rules.empty()) {
            error = "mode allow-all ignores rules, but rules are configured; "
                    "run emcAclClear or choose another mode";
            return nullptr;
        }
        return std::unique_ptr<AccessPolicy>(new AllowAllPolicy());

    case AclMode::Allowlist: {
        if (const AclRule* deny = firstRule(config, AclAction::Deny)) {
            error = "mode allowlist accepts only allow rules, but 'deny " + deny->net.str() +
                    "' is configured; use mode default-deny to combine both";
            return nullptr;
        }
        std::vector<Ipv4Network> nets;
        nets.reserve(rules.size());
        std::transform(rules.begin(), rules.end(), std::back_inserter(nets),
                       [](const AclRule& rule) { return rule.net; });
        return std::unique_ptr<AccessPolicy>(new AllowlistPolicy(std::move(nets)));
    }

    case AclMode::Denylist: {
        if (const AclRule* allow = firstRule(config, AclAction::Allow)) {
            error = "mode denylist accepts only deny rules, but 'allow " + allow->net.str() +
                    "' is configured; use mode default-deny to combine both";
            return nullptr;
        }
        std::vector<Ipv4Network> nets;
        nets.reserve(rules.size());
        std::transform(rules.begin(), rules.end(), std::back_inserter(nets),
                       [](const AclRule& rule) { return rule.net; });
        return std::unique_ptr<AccessPolicy>(new DenylistPolicy(std::move(nets)));
    }

    case AclMode::DefaultDeny:
        return std::unique_ptr<AccessPolicy>(new DefaultDenyPolicy(rules));
    }

    error = "unknown ACL mode";
    return nullptr;
}

std::vector<std::string> aclWarnings(const AclConfig& config) {
    std::vector<std::string> warnings;
    const size_t allows = countRules(config, AclAction::Allow);
    const size_t denies = countRules(config, AclAction::Deny);

    switch (config.mode()) {
    case AclMode::AllowAll:
        warnings.push_back("access control is off: every client will be accepted");
        break;
    case AclMode::Allowlist:
    case AclMode::DefaultDeny:
        if (allows == 0) {
            warnings.push_back("no allow rules: every client will be rejected");
        }
        break;
    case AclMode::Denylist:
        if (denies == 0) {
            warnings.push_back("denylist is empty: every client will be accepted");
        }
        break;
    }

    for (const AclRule& rule : config.rules()) {
        if (rule.action == AclAction::Allow && rule.net.prefix == 0 &&
            config.mode() != AclMode::Denylist) {
            warnings.push_back("allow " + rule.net.str() + " matches every address");
        }
        if (rule.action == AclAction::Allow && config.mode() == AclMode::DefaultDeny) {
            const auto& rules = config.rules();
            if (std::any_of(rules.begin(), rules.end(), [&rule](const AclRule& other) {
                    return other.action == AclAction::Deny && other.net == rule.net;
                })) {
                warnings.push_back("allow " + rule.net.str() +
                                   " has no effect: deny wins over an equally specific allow");
            }
        }
    }
    return warnings;
}

void printAclConfig(const AclConfig& config, FILE* out) {
    std::fprintf(out, "  mode:  %s\n", aclModeName(config.mode()));
    if (config.rules().empty()) {
        std::fprintf(out, "  rules: none\n");
    } else {
        std::fprintf(out, "  rules:\n");
        for (const AclRule& rule : config.rules()) {
            std::fprintf(out, "    %-5s %s\n", aclActionName(rule.action), rule.net.str().c_str());
        }
    }
    if (config.errorCount() > 0) {
        std::fprintf(out, "  ERRORS: %u failed command(s); the server will not start\n",
                config.errorCount());
    }
    for (const std::string& warning : aclWarnings(config)) {
        std::fprintf(out, "  WARNING: %s\n", warning.c_str());
    }
}

// ---------------------------------------------------------------------------
// AclFilter
// ---------------------------------------------------------------------------
AclFilter::AclFilter(std::unique_ptr<const AccessPolicy> policy, const std::string& label)
: policy_(std::move(policy)),
  label_(label.empty() ? std::string() : "[" + label + "] ") {
    if (!policy_) throw std::invalid_argument("AclFilter: null policy");
    std::memset(&lastLog_, 0, sizeof(lastLog_));
}

FilterVerdict AclFilter::onAccept(const PeerInfo& peer) {
    if (policy_->isAllowed(peer.addr)) {
        ++accepted_;
        return FilterVerdict::Allow;
    }
    ++denied_;
    logDenied(peer);
    return FilterVerdict::Drop;
}

void AclFilter::logDenied(const PeerInfo& peer) {
    if (emc_server_debug < 1) return;

    epicsTimeStamp now;
    const bool haveTime = epicsTimeGetCurrent(&now) == epicsTimeOK;
    bool due = !logged_ || !haveTime;
    if (!due) {
        // A clock step backwards gives a negative difference; log then too.
        const double elapsed = epicsTimeDiffInSeconds(&now, &lastLog_);
        due = elapsed >= 1.0 || elapsed < 0.0;
    }
    if (!due) {
        ++suppressed_;
        return;
    }
    if (suppressed_ > 0) {
        Debug(1, "emc: %sACL (%s) refused %s; %lu earlier refusals not shown\n",
              label_.c_str(), aclModeName(policy_->mode()), peer.str().c_str(),
              suppressed_);
    } else {
        Debug(1, "emc: %sACL (%s) refused %s\n",
              label_.c_str(), aclModeName(policy_->mode()), peer.str().c_str());
    }
    logged_ = true;
    if (haveTime) lastLog_ = now;
    suppressed_ = 0;
}
