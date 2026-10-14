// SPDX-FileCopyrightText: 2026 Helmholtz-Zentrum Berlin für Materialien und Energie GmbH
// SPDX-License-Identifier: MIT

#ifndef ACCESS_CONTROL_H
#define ACCESS_CONTROL_H
/**
 * @file  access_control.h
 * @brief IPv4 access control list for the EMC server.
 *
 * Pieces, from the bottom up:
 *  - Ipv4Network  strict CIDR value type ("10.0.0.0/8", "192.168.1.5").
 *  - AccessPolicy Strategy interface: one allow/deny decision per peer
 *                 address. Four implementations, one per AclMode.
 *  - AclConfig    what the operator entered (mode + rules + any errors).
 *  - makeAccessPolicy()  validates an AclConfig and builds the policy.
 *  - AclFilter    the ConnectionFilter that applies a policy at accept time.
 *
 * Modes:
 *  - allowlist     only allow rules; a peer is accepted iff an allow rule matches.
 *  - denylist      only deny rules; a peer is refused iff a deny rule matches.
 *  - default-deny  allow and deny rules; the most specific matching rule
 *                  (longest prefix) decides; a tie is decided as deny, and
 *                  no match means deny. Rule order does not matter.
 *  - allow-all     no checks; must have no rules.
 *
 * Safety properties:
 *  - Fail-closed: invalid or contradictory configuration makes
 *    makeAccessPolicy() fail, so the server does not start.
 *  - The parser rejects every form inet_aton() would reinterpret
 *    ("10.1", "0x0a.0.0.1", "010.0.0.1") and networks with host bits set.
 *  - Policies are immutable after construction and safe to read from
 *    any thread.
 */

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include <epicsTime.h>
#include <epicsTypes.h>

#include "connection_filter.h"

// ---------------------------------------------------------------------------
// Ipv4Network
// ---------------------------------------------------------------------------
/// Manual construction must preserve these invariants: prefix is 0..32,
/// addr is in host byte order, and all host bits are zero.
/// Prefer parse() for checked input. Policy constructors reject invalid networks.
struct Ipv4Network {
    epicsUInt32 addr = 0;   ///< network address, host byte order, host bits zero
    unsigned    prefix = 32; ///< 0..32

    /// Throws std::invalid_argument if prefix is above 32.
    epicsUInt32 mask() const;
    bool contains(epicsUInt32 host) const;
    std::string str() const;   ///< always "a.b.c.d/n"

    bool operator==(const Ipv4Network& o) const {
        return addr == o.addr && prefix == o.prefix;
    }

    /// Strict parser: four decimal octets without leading zeros, optionally
    /// followed by "/0".."/32". A bare address is a /32. On failure returns
    /// false, leaves @p out unchanged and describes the problem in @p error.
    static bool parse(const char* text, Ipv4Network& out, std::string& error);
};

// ---------------------------------------------------------------------------
// Modes and rules
// ---------------------------------------------------------------------------
enum class AclAction { Allow, Deny };
enum class AclMode { Allowlist, Denylist, DefaultDeny, AllowAll };

const char* aclActionName(AclAction action);
const char* aclModeName(AclMode mode);
/// Case-insensitive; accepts "allowlist", "denylist", "default-deny",
/// "allow-all" (the last two also without the hyphen).
bool parseAclMode(const char* text, AclMode& out);

struct AclRule {
    AclAction   action;
    Ipv4Network net;
};

// ---------------------------------------------------------------------------
// Strategy: AccessPolicy and its four implementations
// ---------------------------------------------------------------------------
class AccessPolicy {
public:
    virtual ~AccessPolicy() = default;
    virtual AclMode mode() const = 0;
    /// @param addr IPv4 peer address, host byte order.
    virtual bool isAllowed(epicsUInt32 addr) const = 0;
};

/// Accepts every peer.
class AllowAllPolicy : public AccessPolicy {
public:
    AclMode mode() const override { return AclMode::AllowAll; }
    bool isAllowed(epicsUInt32) const override { return true; }
};

/// Accepts a peer only if one of the networks contains it. Empty: refuses all.
class AllowlistPolicy : public AccessPolicy {
public:
    /// Throws std::invalid_argument for an invalid network.
    explicit AllowlistPolicy(std::vector<Ipv4Network> allowed);
    AclMode mode() const override { return AclMode::Allowlist; }
    bool isAllowed(epicsUInt32 addr) const override;
private:
    const std::vector<Ipv4Network> allowed_;
};

/// Refuses a peer if one of the networks contains it. Empty: accepts all.
class DenylistPolicy : public AccessPolicy {
public:
    /// Throws std::invalid_argument for an invalid network.
    explicit DenylistPolicy(std::vector<Ipv4Network> denied);
    AclMode mode() const override { return AclMode::Denylist; }
    bool isAllowed(epicsUInt32 addr) const override;
private:
    const std::vector<Ipv4Network> denied_;
};

/// Most specific matching rule decides; ties and no match deny.
class DefaultDenyPolicy : public AccessPolicy {
public:
    /// Throws std::invalid_argument for an invalid network.
    explicit DefaultDenyPolicy(std::vector<AclRule> rules);
    AclMode mode() const override { return AclMode::DefaultDeny; }
    bool isAllowed(epicsUInt32 addr) const override;
private:
    const std::vector<AclRule> rules_;
};

// ---------------------------------------------------------------------------
// AclConfig: operator input, validated by makeAccessPolicy()
// ---------------------------------------------------------------------------
class AclConfig {
public:
    /// Upper bound on rules; each accept scans the list once.
    static const size_t kMaxRules = 256;

    AclMode mode() const { return mode_; }
    void setMode(AclMode mode) { mode_ = mode; }
    const std::vector<AclRule>& rules() const { return rules_; }

    /// Parses and appends a rule. An exact duplicate is accepted and ignored.
    /// On failure the error is counted (see errorCount()) and false returned.
    bool addRule(AclAction action, const char* cidr, std::string& error);

    /// Counts a configuration command that failed (e.g. an unknown mode), so
    /// that a mistyped line in st.cmd blocks the start instead of being lost.
    void recordError() { ++errors_; }
    unsigned errorCount() const { return errors_; }

    /// Removes all rules and recorded errors. Keeps the mode.
    void clear();

private:
    AclMode mode_ = AclMode::DefaultDeny;  // unconfigured server accepts nobody
    std::vector<AclRule> rules_;
    unsigned errors_ = 0;
};

/// Validates @p config and builds the matching policy. Returns nullptr and
/// explains why in @p error when the configuration is rejected:
///  - any recorded configuration error,
///  - allow-all with rules, allowlist with deny rules, denylist with allow rules.
std::unique_ptr<AccessPolicy> makeAccessPolicy(const AclConfig& config, std::string& error);

/// Legal but probably unintended configurations, e.g. "every connection
/// will be rejected". Empty when there is nothing to warn about.
std::vector<std::string> aclWarnings(const AclConfig& config);

/// Mode, rules, errors and warnings, one item per line.
void printAclConfig(const AclConfig& config, FILE* out);

// ---------------------------------------------------------------------------
// AclFilter: the chain link
// ---------------------------------------------------------------------------
/// Applies an AccessPolicy in onAccept; refused peers are dropped without a
/// reply. With emc_server_debug >= 1, denials are logged to stderr at most
/// once per second, with a count of suppressed messages, so a connection
/// flood cannot flood the IOC log.
class AclFilter : public ConnectionFilter {
public:
    /// @param label service name shown in log lines (e.g. "mono"); may be empty.
    /// Throws std::invalid_argument for a null policy.
    explicit AclFilter(std::unique_ptr<const AccessPolicy> policy,
                       const std::string& label = std::string());

    const char* name() const override { return "acl"; }
    FilterVerdict onAccept(const PeerInfo& peer) override;

    AclMode mode() const { return policy_->mode(); }
    /// Counters since construction; safe to read from any thread.
    unsigned long acceptedCount() const { return accepted_.load(); }
    unsigned long deniedCount() const { return denied_.load(); }

private:
    const std::unique_ptr<const AccessPolicy> policy_;
    const std::string label_;   // "[name] " or empty, prefix for log lines
    std::atomic<unsigned long> accepted_{0};
    std::atomic<unsigned long> denied_{0};

    // Denial log throttling; NetLoop thread only.
    bool           logged_ = false;
    epicsTimeStamp lastLog_;
    unsigned long  suppressed_ = 0;

    void logDenied(const PeerInfo& peer);
};

#endif /* ACCESS_CONTROL_H */
