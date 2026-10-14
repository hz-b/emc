// SPDX-FileCopyrightText: 2026 Helmholtz-Zentrum Berlin für Materialien und Energie GmbH
// SPDX-License-Identifier: MIT

#include "emc_server.h"
#include "acl_session.h"
#include "mono_sim_dispatcher.h"
#include <cstdio>
#include <exception>
#include <memory>
#include <epicsExit.h>
#include <iocsh.h>
#include <epicsExport.h>

extern "C" void emcAclRegister(void);

#define Debug(L, ...) do { if ((L) <= MonoSimDispatcher::debugLevel()) { \
    std::fprintf(stderr, "[emcSim] "); \
    std::fprintf(stderr, __VA_ARGS__); \
    std::fprintf(stderr, "\t- file: %s, line %d\n", __FILE__, __LINE__); \
} } while (false)

namespace {
// Statics are destroyed in reverse order: the server (declared last) goes
// first, then its ACL session, then the dispatcher.
// The dispatcher outlives its server and keeps energy across stop/start.
MonoSimDispatcher dispatcher;
AclSession aclSession("sim");
// Survives server re-creation (port change), like the dispatcher state.
ReplyTerminator terminator = ReplyTerminator::LF;
std::unique_ptr<EmcServer> server;
int serverPort = 0;

void shutdown(void*) { server.reset(); } // Destructor stops and joins workers.

void startCall(const iocshArgBuf* args) {
    const int port = args && args[0].ival ? args[0].ival : 5001;
    if (port < 1 || port > 65535) {
        Debug(0, "invalid port=%d: use 1..65535 (0 selects 5001)", port);
        return;
    }
    Debug(1, "starting port=%d previous-port=%d", port, serverPort);
    if (server && server->isRunning()) {
        std::printf("emc: simulation already running on port %d%s\n", serverPort,
                    port == serverPort ? "" : "; stop it before changing ports");
        return;
    }
    // A rejected ACL configuration must not start the server (fail closed).
    if (!aclSession.begin()) {
        Debug(0, "simulation NOT started on port=%d: ACL rejected configuration", port);
        return;
    }
    bool started = false;
    try {
        if (!server || port != serverPort) {
            server = std::make_unique<EmcServer>(dispatcher, aclSession.chain(),
                                                 static_cast<unsigned short>(port));
            serverPort = port;
        }
        server->setReplyTerminator(terminator);
        started = server->start();
        std::printf("emc: simulation %s on port %d, reply terminator %s\n",
                    started ? "running" : "FAILED", serverPort, replyTerminatorName(terminator));
    } catch (const std::exception& error) {
        Debug(0, "startup failed port=%d error=%s", port, error.what());
    } catch (...) {
        Debug(0, "startup failed port=%d: unknown exception", port);
    }
    if (!started) aclSession.end();
}

void stopCall(const iocshArgBuf*) {
    Debug(1, "stopping port=%d running=%d", serverPort, server && server->isRunning());
    if (server) server->stop();
    aclSession.end();   // the ACL may be changed again
    std::printf("emc: simulation stopped\n");
}

void terminatorCall(const iocshArgBuf* args) {
    const char* text = args ? args[0].sval : nullptr;
    if (!text || !*text) {
        std::printf("emc: simulation reply terminator %s\n", replyTerminatorName(terminator));
        return;
    }
    ReplyTerminator parsed;
    if (!parseReplyTerminator(text, parsed)) {
        Debug(0, "terminator '%s' rejected: use lf, crlf, cr or none (unchanged: %s)",
              text, replyTerminatorName(terminator));
        return;
    }
    terminator = parsed;
    if (server) server->setReplyTerminator(terminator);   // thread-safe, also while running
    std::printf("emc: simulation reply terminator %s%s\n", replyTerminatorName(terminator),
                server && server->isRunning() ? " (applies to the next reply)" : "");
}

void debugCall(const iocshArgBuf* args) {
    const int level = args ? args[0].ival : 0;
    if (level < 0) {
        std::fprintf(stderr, "emc: invalid simulation debug level=%d (use 0 or greater)\n", level);
        return;
    }
    MonoSimDispatcher::setDebugLevel(level);
    std::printf("emc: simulation debug level %d\n", MonoSimDispatcher::debugLevel());
}

const iocshArg portArg = {"port (default 5001)", iocshArgInt};
const iocshArg* const startArgs[] = {&portArg};
const iocshFuncDef startDef = {"emcSimStart", 1, startArgs};
const iocshFuncDef stopDef = {"emcSimStop", 0, nullptr};
const iocshArg termArg = {"lf|crlf|cr|none (empty: show)", iocshArgString};
const iocshArg* const termArgs[] = {&termArg};
const iocshFuncDef termDef = {"emcSimTerminator", 1, termArgs};
const iocshArg debugArg = {"level (0: quiet, 1: changes/errors, 2: requests)", iocshArgInt};
const iocshArg* const debugArgs[] = {&debugArg};
const iocshFuncDef debugDef = {"emcSimDebug", 1, debugArgs};
} // namespace

extern "C" void emcCoreRegister(void) {
    static bool registered = false;
    if (registered) return;
    epicsAtExit(shutdown, nullptr);
    emcAclRegister();   // shared ACL commands, registered once for all services
    emcServerLimitsRegister(); // common configuration, independent of simulation
    iocshRegister(&startDef, startCall);
    iocshRegister(&stopDef, stopCall);
    iocshRegister(&termDef, terminatorCall);
    iocshRegister(&debugDef, debugCall);
    registered = true;
}

epicsExportRegistrar(emcCoreRegister);
