// SPDX-FileCopyrightText: 2026 Helmholtz-Zentrum Berlin für Materialien und Energie GmbH
// SPDX-License-Identifier: MIT

// IOC shell commands for the access control list shared by all EMC services.
//
//   emcAclMode  <allowlist|denylist|default-deny|allow-all>
//   emcAclAllow <a.b.c.d[/n]>
//   emcAclDeny  <a.b.c.d[/n]>
//   emcAclClear                  remove all rules and recorded errors
//   emcAclShow                   configuration, warnings, per-service state
//   emcAclCheck <a.b.c.d>        would this address be accepted?
//
// One configuration applies to every EMC service (emcSimStart, emcSomeServerStart,
// ...). It is read when a service starts and is locked while any service
// runs. Without any ACL command the mode is default-deny with no rules, so
// every client is refused. A failed ACL command blocks the start of all
// services until emcAclClear, so a typo in st.cmd cannot silently open a
// server.
//
// Example st.cmd:
//   emcAclMode default-deny
//   emcAclAllow 127.0.0.1
//   emcAclAllow 192.168.2.0/24
//   emcAclDeny  192.168.2.66
//   emcSimStart    5002
//
#include "acl_session.h"

#include <cstdio>
#include <string>
#include <iocsh.h>
#include <epicsExport.h>

namespace {

const char* const kBlocked = "service start is blocked until emcAclClear";

void report(const char* command, const char* argument, AclChange result,
            const std::string& error) {
    if (result == AclChange::Invalid) {
        std::fprintf(stderr, "emc: %s '%s' rejected: %s; %s\n", command,
                     argument ? argument : "", error.c_str(), kBlocked);
    } else if (result == AclChange::Locked) {
        std::fprintf(stderr, "emc: %s\n", error.c_str());
    }
}

void aclModeCall(const iocshArgBuf* args) {
    const char* text = args ? args[0].sval : nullptr;
    std::string error;
    const AclChange result = SharedAcl::setMode(text, error);
    if (result != AclChange::Ok) {
        report("emcAclMode", text, result, error);
        return;
    }
    AclMode mode;
    parseAclMode(text, mode);
    std::printf("emc: ACL mode %s\n", aclModeName(mode));
}

void addRule(const char* command, AclAction action, const iocshArgBuf* args) {
    const char* text = args ? args[0].sval : nullptr;
    std::string canonical, error;
    const AclChange result = SharedAcl::addRule(action, text, canonical, error);
    if (result != AclChange::Ok) {
        report(command, text, result, error);
        return;
    }
    std::printf("emc: ACL %s %s\n", aclActionName(action), canonical.c_str());
}

void aclAllowCall(const iocshArgBuf* args) { addRule("emcAclAllow", AclAction::Allow, args); }
void aclDenyCall(const iocshArgBuf* args)  { addRule("emcAclDeny", AclAction::Deny, args); }

void aclClearCall(const iocshArgBuf*) {
    std::string error;
    const AclChange result = SharedAcl::clear(error);
    if (result != AclChange::Ok) {
        report("emcAclClear", "", result, error);
        return;
    }
    std::printf("emc: ACL rules and errors cleared\n");
}

void aclShowCall(const iocshArgBuf*) { SharedAcl::print(stdout); }

void aclCheckCall(const iocshArgBuf* args) {
    const char* text = args ? args[0].sval : nullptr;
    bool accepted = false;
    std::string error;
    if (!SharedAcl::check(text, accepted, error)) {
        std::fprintf(stderr, "emc: emcAclCheck '%s' failed: %s\n", text ? text : "", error.c_str());
        return;
    }
    std::printf("emc: ACL would %s %s\n", accepted ? "ACCEPT" : "REFUSE", text);
}

const iocshArg modeArg = {"allowlist|denylist|default-deny|allow-all", iocshArgString};
const iocshArg netArg  = {"a.b.c.d[/prefix]", iocshArgString};
const iocshArg hostArg = {"a.b.c.d", iocshArgString};
const iocshArg* const modeArgs[] = {&modeArg};
const iocshArg* const netArgs[]  = {&netArg};
const iocshArg* const hostArgs[] = {&hostArg};
const iocshFuncDef aclModeDef  = {"emcAclMode", 1, modeArgs};
const iocshFuncDef aclAllowDef = {"emcAclAllow", 1, netArgs};
const iocshFuncDef aclDenyDef  = {"emcAclDeny", 1, netArgs};
const iocshFuncDef aclClearDef = {"emcAclClear", 0, nullptr};
const iocshFuncDef aclShowDef  = {"emcAclShow", 0, nullptr};
const iocshFuncDef aclCheckDef = {"emcAclCheck", 1, hostArgs};

} // namespace

extern "C" void emcAclRegister(void) {
    static bool registered = false;
    if (registered) return;
    iocshRegister(&aclModeDef, aclModeCall);
    iocshRegister(&aclAllowDef, aclAllowCall);
    iocshRegister(&aclDenyDef, aclDenyCall);
    iocshRegister(&aclClearDef, aclClearCall);
    iocshRegister(&aclShowDef, aclShowCall);
    iocshRegister(&aclCheckDef, aclCheckCall);
    registered = true;
}
