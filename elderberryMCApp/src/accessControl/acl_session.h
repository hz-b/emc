// SPDX-FileCopyrightText: 2026 Helmholtz-Zentrum Berlin für Materialien und Energie GmbH
// SPDX-License-Identifier: MIT

#ifndef ACL_SESSION_H
#define ACL_SESSION_H
/**
 * @file  acl_session.h
 * @brief One access control configuration shared by every EMC service.
 *
 * Configured in the IOC shell once, every service that accepts TCP clients applies it.
 *
 *   SharedAcl   the single, process-wide configuration (mode + rules).
 *   AclSession  one per service (sim, mono, ...). Owns that service's
 *               FilterChain and rebuilds it from the shared configuration
 *               each time the service starts.
 *
 * Usage in a service:
 *
 *   AclSession acl("mono");                        // member, declared before the server
 *   EmcServer  server(dispatcher, acl.chain(), port);
 *   ...
 *   if (!acl.begin()) return false;                // bad configuration: do not start
 *   if (!server.start()) { acl.end(); return false; }
 *   ...
 *   server.stop();
 *   acl.end();                                     // allows ACL changes again
 *
 * While any session is active (between begin() and end()) the configuration
 * is locked: emcAcl* changes are refused, so the rules a running server
 * enforces are always the ones shown by emcAclShow. A session that never
 * calls begin() has an empty chain, which allows everything, so a service
 * must always call begin() before starting its server.
 *
 * Threading: all SharedAcl and AclSession functions are thread-safe.
 * chain() must only be used by the owning server (NetLoop thread) and must
 * not be modified while that server runs. 
 * begin()/end() must be serialized with the server's start()/stop().
 */

#include <atomic>
#include <cstdio>
#include <string>

#include "access_control.h"
#include "connection_filter.h"

class AclSession;

/// Result of a configuration change.
enum class AclChange {
    Ok,       ///< applied
    Locked,   ///< refused: a service is running; nothing changed
    Invalid   ///< rejected input; recorded as an error that blocks every
              ///< service from starting until clear() (fail-closed)
};

class SharedAcl {
public:
    /// Sets the mode ("allowlist", "denylist", "default-deny", "allow-all").
    static AclChange setMode(const char* text, std::string& error);

    /// Adds a rule. @p canonical receives "a.b.c.d/n" on success.
    static AclChange addRule(AclAction action, const char* cidr,
                             std::string& canonical, std::string& error);

    /// Removes all rules and recorded errors; keeps the mode.
    static AclChange clear(std::string& error);

    /// Would @p host (one address) be accepted by the current configuration?
    /// Returns false and sets @p error if the address or configuration is invalid.
    static bool check(const char* host, bool& accepted, std::string& error);

    /// Configuration, warnings, and every service's state and counters.
    static void print(FILE* out);

    /// True while at least one service runs with the configuration.
    static bool locked();

    /// For tests: forget configuration and sessions' locks. Refused (false)
    /// while a session is active.
    static bool reset();
};

class AclSession {
public:
    /// @param name service name for log lines and emcAclShow ("mono", "sim").
    explicit AclSession(const char* name);
    /// Ends the session and unregisters it.
    ~AclSession();

    AclSession(const AclSession&) = delete;
    AclSession& operator=(const AclSession&) = delete;

    const char* name() const { return name_.c_str(); }

    /// The service's chain; stable address for the session's lifetime.
    FilterChain& chain() { return chain_; }

    /// Builds the chain from the shared config and locks it until end().
    /// On failure: log, no changes, no start. Repeatable.
    bool begin();

    /// Releases the configuration lock. Call after the server has stopped
    /// (or when starting it failed). Safe to call repeatedly.
    void end();

    bool active() const { return active_; }

    /// Counters of the current/last run; 0 before the first begin().
    unsigned long accepted() const;
    unsigned long denied() const;

private:
    friend class SharedAcl;

    const std::string name_;
    FilterChain chain_;
    AclFilter* filter_ = nullptr;      // owned by chain_
    std::atomic<bool> active_{false};
};

#endif /* ACL_SESSION_H */
