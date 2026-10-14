// SPDX-FileCopyrightText: 2026 Helmholtz-Zentrum Berlin für Materialien und Energie GmbH
// SPDX-License-Identifier: MIT

/**
 * @file  acl_session.cpp
 * @brief Process-wide ACL configuration and per-service sessions.
 */

#include "acl_session.h"

#include <algorithm>
#include <exception>
#include <memory>
#include <vector>

#include <epicsGuard.h>
#include <epicsMutex.h>

// Defined and exported by emc_server.cpp; share its runtime debug switch.
extern int emc_server_debug;

#define Debug(L, ...) do { if ((L) <= emc_server_debug) { \
    std::fprintf(stderr, __VA_ARGS__); \
    std::fprintf(stderr, "\t- file : %s, line %d\n", __FILE__, __LINE__); \
} } while (false)

namespace {

struct Shared {
    epicsMutex mutex;
    AclConfig config;                    // default mode: default-deny, no rules
    std::vector<AclSession*> sessions;   // every constructed, not yet destroyed session
    unsigned active = 0;                 // sessions between begin() and end()
};

// Intentionally never destroyed: services may be torn down by epicsAtExit
// handlers or static destructors in any order, and must still find it.
Shared& shared() {
    static Shared* instance = new Shared;
    return *instance;
}

typedef epicsGuard<epicsMutex> Guard;

// Names of the sessions that currently hold the lock; caller holds the mutex.
std::string activeNames(const Shared& sh);

} // namespace

// ---------------------------------------------------------------------------
// SharedAcl
// ---------------------------------------------------------------------------
namespace {

std::string activeNames(const Shared& sh) {
    std::string names;
    for (const AclSession* s : sh.sessions) {
        if (!s->active()) continue;
        if (!names.empty()) names += ", ";
        names += s->name();
    }
    return names;
}

AclChange lockedError(const Shared& sh, const char* what, std::string& error) {
    error = std::string(what) + " refused: ACL changes need every EMC service stopped (running: " +
            activeNames(sh) + ")";
    return AclChange::Locked;
}

} // namespace

AclChange SharedAcl::setMode(const char* text, std::string& error) {
    Shared& sh = shared();
    Guard guard(sh.mutex);
    if (sh.active > 0) return lockedError(sh, "emcAclMode", error);

    AclMode mode;
    if (!parseAclMode(text, mode)) {
        sh.config.recordError();
        error = "use allowlist, denylist, default-deny or allow-all";
        return AclChange::Invalid;
    }
    sh.config.setMode(mode);
    return AclChange::Ok;
}

AclChange SharedAcl::addRule(
        AclAction action, 
        const char* cidr,
        std::string& canonical, // NOLINT(bugprone-easily-swappable-parameters)
        std::string& error) 
{
    Shared& sh = shared();
    Guard guard(sh.mutex);
    if (sh.active > 0) {
        return lockedError(sh, action == AclAction::Allow ? "emcAclAllow" : "emcAclDeny", error);
    }
    if (!sh.config.addRule(action, cidr, error)) return AclChange::Invalid;

    Ipv4Network net;
    std::string ignored;
    if (Ipv4Network::parse(cidr, net, ignored)) canonical = net.str();
    return AclChange::Ok;
}

AclChange SharedAcl::clear(std::string& error) {
    Shared& sh = shared();
    Guard guard(sh.mutex);
    if (sh.active > 0) return lockedError(sh, "emcAclClear", error);
    sh.config.clear();
    return AclChange::Ok;
}

bool SharedAcl::check(const char* host, bool& accepted, std::string& error) {
    Ipv4Network net;
    if (!Ipv4Network::parse(host, net, error) || net.prefix != 32) {
        if (error.empty()) error = "expected one address, not a network";
        return false;
    }
    Shared& sh = shared();
    Guard guard(sh.mutex);
    std::unique_ptr<AccessPolicy> policy = makeAccessPolicy(sh.config, error);
    if (!policy) return false;
    accepted = policy->isAllowed(net.addr);
    return true;
}

void SharedAcl::print(FILE* out) {
    Shared& sh = shared();
    Guard guard(sh.mutex);
    std::fprintf(out, "emc: access control list (shared by all EMC services)\n");
    printAclConfig(sh.config, out);
    if (sh.sessions.empty()) {
        std::fprintf(out, "  services: none registered yet\n");
        return;
    }
    std::fprintf(out, "  services:\n");
    for (const AclSession* s : sh.sessions) {
        if (!s->filter_) {
            std::fprintf(out, "    %-8s %-8s never started\n", s->name(),
                         s->active() ? "running" : "stopped");
            continue;
        }
        std::fprintf(out, "    %-8s %-8s %lu accepted, %lu refused%s\n", s->name(),
                     s->active() ? "running" : "stopped", s->filter_->acceptedCount(),
                     s->filter_->deniedCount(), s->active() ? "" : " (last run)");
    }
    if (sh.active == 0) {
        std::fprintf(out, "  state: changes apply when a service starts\n");
    } else {
        std::fprintf(out, "  state: in force, locked while running: %s\n", activeNames(sh).c_str());
    }
}

bool SharedAcl::locked() {
    Shared& sh = shared();
    Guard guard(sh.mutex);
    return sh.active > 0;
}

bool SharedAcl::reset() {
    Shared& sh = shared();
    Guard guard(sh.mutex);
    if (sh.active > 0) return false;
    sh.config.clear();
    sh.config.setMode(AclMode::DefaultDeny);
    return true;
}

// ---------------------------------------------------------------------------
// AclSession
// ---------------------------------------------------------------------------
AclSession::AclSession(const char* name)
: name_(name && *name ? name : "emc") {
    Shared& sh = shared();
    Guard guard(sh.mutex);
    sh.sessions.push_back(this);
}

AclSession::~AclSession() {
    Shared& sh = shared();
    Guard guard(sh.mutex);
    if (active_) {
        active_ = false;
        --sh.active;
    }
    sh.sessions.erase(std::remove(sh.sessions.begin(), sh.sessions.end(), this),
                      sh.sessions.end());
}

bool AclSession::begin() {
    Shared& sh = shared();
    Guard guard(sh.mutex);
    if (active_) return true;

    FilterChain replacement;
    AclFilter* raw = nullptr;
    std::vector<std::string> warnings;

    try {
        std::string error;
        std::unique_ptr<AccessPolicy> policy = makeAccessPolicy(sh.config, error);
        if (!policy) {
            Debug(0, "emc: [%s] ACL configuration rejected: %s",
                  name_.c_str(), error.c_str());
            return false;
        }

        std::unique_ptr<AclFilter> acl(new AclFilter(std::move(policy), name_));
        raw = acl.get();
        // Prepare everything that can allocate before changing session state.
        replacement.add(std::move(acl));
        warnings = aclWarnings(sh.config);
    } catch (const std::exception& e) {
        Debug(0, "emc: [%s] ACL setup failed: %s", name_.c_str(), e.what());
        return false;
    } catch (...) {
        Debug(0, "emc: [%s] ACL setup failed", name_.c_str());
        return false;
    }

    // Commit without allocating; replacement now owns the old filters.
    chain_.swap(replacement);
    filter_ = raw;
    active_ = true;
    ++sh.active;

    size_t allows = 0, denies = 0;
    for (const AclRule& rule : sh.config.rules()) {
        (rule.action == AclAction::Allow ? allows : denies)++;
    }
    Debug(1, "emc: [%s] ACL %s, %zu allow and %zu deny rule(s)", name_.c_str(),
          aclModeName(sh.config.mode()), allows, denies);
    for (const std::string& warning : warnings) {
        Debug(0, "emc: [%s] ACL WARNING: %s", name_.c_str(), warning.c_str());
    }
    return true;
}

void AclSession::end() {
    Shared& sh = shared();
    Guard guard(sh.mutex);
    if (!active_) return;
    active_ = false;
    --sh.active;
}

unsigned long AclSession::accepted() const {
    Guard guard(shared().mutex);
    return filter_ ? filter_->acceptedCount() : 0;
}

unsigned long AclSession::denied() const {
    Guard guard(shared().mutex);
    return filter_ ? filter_->deniedCount() : 0;
}
