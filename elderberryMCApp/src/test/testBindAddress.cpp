/**
 * @file testBindAddress.cpp
 * @brief shared IOC configuration and real TCP listener isolation.
 * Linux loopback addresses 127.0.0.1 and 127.0.0.2 emulate two local IPs.
 * Ports are allocated by the kernel; no hardware or fixed test port is needed.
 */

#include <cerrno>
#include <string>

#include <epicsUnitTest.h>
#include <iocsh.h>
#include <osiSock.h>
#include <testMain.h>

#include "access_control.h"
#include "dispatcher.h"
#include "emc_server.h"

namespace {

struct ReplyDispatcher : Dispatcher {
    explicit ReplyDispatcher(const char* name) : name(name) {}
    std::string processLine(const char* line) override {
        return name + ":" + line;
    }
    std::string name;
};

struct Socket {
    SOCKET fd = INVALID_SOCKET;
    ~Socket() { close(); }
    void close() {
        if (fd != INVALID_SOCKET) epicsSocketDestroy(fd);
        fd = INVALID_SOCKET;
    }
    bool create() {
        close();
        fd = epicsSocketCreate(AF_INET, SOCK_STREAM, 0);
        if (fd == INVALID_SOCKET) return false;
        timeval timeout{1, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        return true;
    }
    bool connectTo(epicsUInt32 address, unsigned short port,
                   epicsUInt32 source = 0) {
        if (!create()) return false;
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(source);
        if (source && bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0)
            return false;
        sockaddr_in remote{};
        remote.sin_family = AF_INET;
        remote.sin_addr.s_addr = htonl(address);
        remote.sin_port = htons(port);
        return connect(fd, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) == 0;
    }
    std::string request() {
        const char command[] = "hello\n";
        if (send(fd, command, sizeof(command) - 1, MSG_NOSIGNAL) !=
            static_cast<ssize_t>(sizeof(command) - 1))
            return "";
        std::string reply;
        char buffer[64];
        while (reply.find('\n') == std::string::npos && reply.size() < 256) {
            const ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
            if (n <= 0) break;
            reply.append(buffer, static_cast<size_t>(n));
        }
        return reply;
    }
    // Reserve a kernel-selected port. Holding it also provides a bind-conflict test.
    unsigned short reserve(epicsUInt32 address) {
        if (!create()) return 0;
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(address);
        if (bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0 ||
            listen(fd, 1) != 0) return 0;
        osiSocklen_t length = sizeof(local);
        if (getsockname(fd, reinterpret_cast<sockaddr*>(&local), &length) != 0) return 0;
        return ntohs(local.sin_port);
    }
};

constexpr epicsUInt32 localhost = 0x7f000001u;
constexpr epicsUInt32 secondAddress = 0x7f000002u;

unsigned short unusedPort() {
    Socket reservation;
    return reservation.reserve(0);
}

bool replyFrom(epicsUInt32 address, unsigned short port, const char* expected) {
    Socket client;
    return client.connectTo(address, port) && client.request() == expected;
}

bool connectionRefused(epicsUInt32 address, unsigned short port) {
    Socket client;
    const bool connected = client.connectTo(address, port);
    return !connected && errno == ECONNREFUSED;
}

void testValidation() {
    testDiag("bind-address parsing, fail-closed startup and shared IOC command");
    testOk(EmcServer::defaultBindAddress() == "0.0.0.0", "default preserves all-address binding");
    emcServerLimitsRegister();
    emcServerLimitsRegister(); // registration is idempotent
    testOk(iocshCmd("emcBindAddress 127.0.0.1") == 0 &&
           EmcServer::defaultBindAddress() == "127.0.0.1", "shared IOC command sets the address");
    testOk(iocshCmd("emcBindAddress") == 0 &&
           EmcServer::defaultBindAddress() == "127.0.0.1", "query leaves configuration unchanged");

    ReplyDispatcher dispatcher("test");
    FilterChain filters;
    EmcServer server(dispatcher, filters, unusedPort());
    const char* bad[] = {nullptr, "", "localhost", "127.1", "010.0.0.1",
                         "0x7f.0.0.1", "256.0.0.1", "127.0.0.1 ", " 127.0.0.1",
                         "127.0.0.1/32", "10.0.0.0/8", "::1", "255.255.255.255",
                         "224.0.0.1", "239.255.255.255"};
    for (const char* text : bad) {
        testOk(!EmcServer::setDefaultBindAddress(text) &&
               EmcServer::defaultBindAddress() == "invalid" &&
               !server.start() && !server.isRunning(),
               "rejects '%s' and blocks startup", text ? text : "(null)");
    }
    const char* good[] = {"0.0.0.0", "127.0.0.1", "127.0.0.2", "192.168.10.20"};
    for (const char* text : good) {
        testOk(EmcServer::setDefaultBindAddress(text) &&
               EmcServer::defaultBindAddress() == text, "accepts '%s'", text);
    }
    iocshCmd("emcBindAddress not-an-address");
    testOk(EmcServer::defaultBindAddress() == "invalid" && !server.start(),
           "invalid IOC configuration also blocks startup");
    iocshCmd("emcBindAddress 0.0.0.0");
    testOk(EmcServer::defaultBindAddress() == "0.0.0.0", "valid IOC setting clears the error");
}

void testListenerAndRestart() {
    testDiag("all-address binding, selected-address isolation and reused-server restart");
    const unsigned short port = unusedPort();
    if (!testOk(port != 0, "allocated a free test port")) return;
    ReplyDispatcher dispatcher("first");
    FilterChain filters;
    EmcServer server(dispatcher, filters, port);
    EmcServer::setDefaultBindAddress("0.0.0.0");
    if (!testOk(server.start(), "start on all IPv4 addresses")) return;
    testOk(server.bindAddress() == "0.0.0.0" &&
           replyFrom(localhost, port, "first:hello\n") &&
           replyFrom(secondAddress, port, "first:hello\n"),
           "one wildcard listener serves both local addresses");

    iocshCmd("emcBindAddress 127.0.0.1");
    testOk(server.start() && server.bindAddress() == "0.0.0.0" &&
           replyFrom(secondAddress, port, "first:hello\n"),
           "changing the shared setting does not move a running listener");
    server.stop();
    if (!testOk(server.start(), "restart existing object on selected address")) return;
    testOk(server.bindAddress() == "127.0.0.1" &&
           replyFrom(localhost, port, "first:hello\n"), "selected address serves requests");
    testOk(connectionRefused(secondAddress, port), "other local destination has no listener");

    // Invalid pending configuration must not disturb the current listener.
    iocshCmd("emcBindAddress invalid");
    testOk(server.bindAddress() == "127.0.0.1" && replyFrom(localhost, port, "first:hello\n"),
           "invalid pending setting leaves the running listener intact");
    server.stop();
    testOk(!server.start() && !server.isRunning() && connectionRefused(localhost, port),
           "invalid pending setting prevents restart without opening a listener");
    iocshCmd("emcBindAddress 127.0.0.2");
    if (!testOk(server.start(), "corrected setting allows restart on second address")) return;
    testOk(server.bindAddress() == "127.0.0.2" &&
           replyFrom(secondAddress, port, "first:hello\n") && connectionRefused(localhost, port),
           "restart switches the selected address and excludes IPv4 localhost");

    // A different dispatcher/server automatically uses the same common setting.
    const unsigned short otherPort = unusedPort();
    ReplyDispatcher otherDispatcher("second");
    FilterChain otherFilters;
    EmcServer other(otherDispatcher, otherFilters, otherPort);
    if (testOk(otherPort != 0 && other.start(), "second backend starts with shared configuration")) {
        testOk(other.bindAddress() == "127.0.0.2" &&
               replyFrom(secondAddress, otherPort, "second:hello\n") &&
               connectionRefused(localhost, otherPort), "shared setting applies to both dispatchers");
    }
    other.stop();
    server.stop();
}

void testBindFailure() {
    testDiag("failed bind closes the socket and never falls back to all addresses");
    Socket occupied;
    const unsigned short port = occupied.reserve(localhost);
    if (!testOk(port != 0, "reserve an occupied port")) return;
    ReplyDispatcher dispatcher("retry");
    FilterChain filters;
    EmcServer server(dispatcher, filters, port);
    EmcServer::setDefaultBindAddress("127.0.0.1");
    testOk(!server.start() && !server.isRunning(), "occupied address fails startup");
    testOk(!server.start() && !server.isRunning(), "repeated failure remains safe");
    occupied.close();
    if (testOk(server.start(), "same object recovers after bind conflict is removed")) {
        testOk(replyFrom(localhost, port, "retry:hello\n"), "recovered listener serves requests");
    }
    server.stop();

    // This documentation-only address is normally absent. Check that assumption.
    Socket probe;
    const epicsUInt32 nonlocal = 0xc000027bu; // 192.0.2.123
    const unsigned short probePort = probe.reserve(nonlocal);
    const int bindError = errno;
    if (probePort != 0 || bindError != EADDRNOTAVAIL) {
        testSkip(2, "host permits binding 192.0.2.123 or cannot probe it");
        return;
    }
    EmcServer::setDefaultBindAddress("192.0.2.123");
    testOk(!server.start() && !server.isRunning(), "nonlocal address fails startup");
    testOk(connectionRefused(localhost, port) && connectionRefused(secondAddress, port),
           "failed specific bind does not create a wildcard listener");
}

void testAcl() {
    testDiag("ACL admission remains active on a specific listener");
    const unsigned short port = unusedPort();
    if (!testOk(port != 0, "allocated ACL test port")) return;
    Ipv4Network allowed;
    allowed.addr = localhost;
    allowed.prefix = 32;
    ReplyDispatcher dispatcher("acl");
    FilterChain filters;
    filters.add(std::unique_ptr<ConnectionFilter>(new AclFilter(
        std::unique_ptr<const AccessPolicy>(new AllowlistPolicy({allowed})))));
    EmcServer server(dispatcher, filters, port);
    EmcServer::setDefaultBindAddress("127.0.0.1");
    if (!testOk(server.start(), "start specific listener with ACL")) return;
    testOk(replyFrom(localhost, port, "acl:hello\n"), "allowed source receives backend reply");
    Socket denied;
    const bool connected = denied.connectTo(localhost, port, secondAddress);
    char byte;
    const ssize_t received = connected ? recv(denied.fd, &byte, 1, 0) : -1;
    testOk(connected && (received == 0 || (received < 0 && errno == ECONNRESET)),
           "denied source is closed by ACL even on selected destination");
    server.stop();
}

} // namespace

MAIN(testBindAddress) {
    testPlan(0);
    osiSockAttach();
    testValidation();
    testListenerAndRestart();
    testBindFailure();
    testAcl();
    EmcServer::setDefaultBindAddress("0.0.0.0");
    osiSockRelease();
    return testDone();
}
