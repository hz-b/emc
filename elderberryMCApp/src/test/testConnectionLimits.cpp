/**
 * @file testConnectionLimits.cpp
 * @brief connection caps, released capacity and optional idle expiry.
 *
 * Run with make runtests or directly. Admission uses a synchronous loopback
 * listener. The global-cap test seeds socket-free peers to avoid exhausting
 * FD_SETSIZE with both ends of 512 sockets in one process. Timeout boundaries
 * use an injected clock value, plus real server tests on EMC_TEST_PORT + 40
 * (default 15771). Binding failure is a test failure, never a skip.
 */
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <epicsThread.h>
#include <epicsUnitTest.h>
#include <epicsExit.h>
#include <osiSock.h>
#include <iocsh.h>
#include <testMain.h>

#include "dispatcher.h"
#include "emc_server.h"

extern "C" void emcCoreRegister(void);

using Clock = std::chrono::steady_clock;

// Only used with the synchronous fixture, never against a running worker.
struct NetLoopTestAccess {
    static size_t size(const NetLoop& loop) { return loop.conns_.size(); }
    static void accept(NetLoop& loop) { loop.acceptOne(); }
    static int newest(const NetLoop& loop) { return loop.conns_.rbegin()->first; }
    static void drop(NetLoop& loop, int id) { loop.dropConn(id); }
    static void clear(NetLoop& loop) {
        while (!loop.conns_.empty()) loop.dropConn(loop.conns_.begin()->first);
    }
    static void seed(NetLoop& loop, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            NetLoop::Conn c;
            c.id = loop.nextId_++;
            c.peer.conn_id = c.id;
            c.peer.addr = 0x0a000001u + static_cast<epicsUInt32>(i);
            c.lastActivity = Clock::now();
            if (loop.filters_.onAccept(c.peer) != FilterVerdict::Allow) {
                testAbort("seeded test peer unexpectedly refused by filter");
            }
            loop.conns_.emplace(c.id, std::move(c));
        }
    }
    static bool expireIdle(NetLoop& loop, int id, Clock::time_point now) {
        return loop.dropIdleConnection(loop.conns_.at(id), now, loop.srv_->idleTimeout());
    }
    static bool expireRequest(NetLoop& loop, int id, Clock::time_point now) {
        return loop.dropExpiredRequest(loop.conns_.at(id), now);
    }
    static Clock::time_point activity(const NetLoop& loop, int id) {
        return loop.conns_.at(id).lastActivity;
    }
    static void setActivity(NetLoop& loop, int id, Clock::time_point when) {
        loop.conns_.at(id).lastActivity = when;
    }
    static void read(NetLoop& loop, int id) {
        const SOCKET socket = loop.conns_.at(id).s;
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(socket, &readable);
        timeval timeout{2, 0};
        if (select(socket + 1, &readable, nullptr, nullptr, &timeout) != 1) {
            testAbort("test data did not reach the accepted socket");
        }
        loop.handleReadable(socket);
    }
};

namespace {
struct Echo : Dispatcher {
    std::string processLine(const char* line) override {
        if (std::string(line) == "slow") epicsThreadSleep(0.6);
        return line;
    }
};

struct CountingFilter : ConnectionFilter {
    std::atomic<size_t> accepts{0}, closes{0};
    const char* name() const override { return "connection-test"; }
    FilterVerdict onAccept(const PeerInfo&) override { ++accepts; return FilterVerdict::Allow; }
    void onClose(const PeerInfo&) override { ++closes; }
};

struct Client {
    SOCKET s = INVALID_SOCKET;
    Client() = default;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    ~Client() { close(); }
    void close() {
        if (s != INVALID_SOCKET) epicsSocketDestroy(s);
        s = INVALID_SOCKET;
    }
    void connectTo(unsigned short port, epicsUInt32 source = INADDR_LOOPBACK) {
        s = epicsSocketCreate(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET) testAbort("cannot create connection-limit client");
        timeval tv{2, 0};
        if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&tv), sizeof(tv)) != 0) {
            testAbort("cannot set client receive timeout");
        }
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(source);
        if (bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
            testAbort("cannot bind client loopback address");
        }
        sockaddr_in peer{};
        peer.sin_family = AF_INET;
        peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        peer.sin_port = htons(port);
        if (connect(s, reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) != 0) {
            testAbort("cannot connect connection-limit client");
        }
    }
    void send(const std::string& text) {
        size_t sent = 0;
        while (sent < text.size()) {
            const ssize_t n = ::send(s, text.data() + sent, text.size() - sent, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) testAbort("cannot send connection-limit test data");
            sent += static_cast<size_t>(n);
        }
    }
    bool closed() {
        char ch;
        const ssize_t n = recv(s, &ch, 1, 0);
        return n == 0 || (n < 0 && (errno == ECONNRESET || errno == EPIPE));
    }
    bool ping() {
        send("ping\n");
        return readLine() == "OK\n";
    }
    std::string readLine() {
        std::string reply;
        char ch;
        while (recv(s, &ch, 1, 0) == 1) {
            reply += ch;
            if (ch == '\n') break;
        }
        return reply;
    }
};

// No worker threads: acceptance/expiry are deterministic, with real sockets.
struct Fixture {
    Echo echo;
    FilterChain filters;
    CountingFilter* counter = new CountingFilter;
    EmcServer server{echo, filters};
    epicsMessageQueue requests{1024, sizeof(void*)}, responses{1024, sizeof(void*)};
    SOCKET listener = INVALID_SOCKET;
    unsigned short port = 0;
    std::unique_ptr<NetLoop> loop;

    Fixture() {
        filters.add(std::unique_ptr<ConnectionFilter>(counter));
        listener = epicsSocketCreate(AF_INET, SOCK_STREAM, 0);
        if (listener == INVALID_SOCKET) testAbort("cannot create test listener");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0
            || listen(listener, 16) != 0) testAbort("cannot bind/listen on test port");
        osiSocklen_t len = sizeof(address);
        if (getsockname(listener, reinterpret_cast<sockaddr*>(&address), &len) != 0) {
            testAbort("cannot get test listener port");
        }
        port = ntohs(address.sin_port);
        const int flags = fcntl(listener, F_GETFL, 0);
        if (flags < 0 || fcntl(listener, F_SETFL, flags | O_NONBLOCK) != 0) {
            testAbort("cannot make test listener nonblocking");
        }
        loop.reset(new NetLoop(&server, listener, &requests, &responses, filters));
    }
    ~Fixture() {
        NetLoopTestAccess::clear(*loop);
        epicsSocketDestroy(listener);
    }
    void connect(Client& client, epicsUInt32 source = INADDR_LOOPBACK) {
        client.connectTo(port, source);
        NetLoopTestAccess::accept(*loop);
    }
};

void testConfiguration() {
    testDiag("idle timeout validation and independent server settings");
    Echo echo;
    FilterChain a, b;
    EmcServer first(echo, a), second(echo, b);
    testOk(first.idleTimeout() == 0.0 && second.idleTimeout() == 0.0,
           "idle expiry disabled by default");
    testOk(first.setIdleTimeout(1800.5) && first.idleTimeout() == 1800.5
           && second.idleTimeout() == 0.0, "timeout is per server and accepts fractions");
    for (double bad : {-1.0, std::numeric_limits<double>::infinity(),
                       -std::numeric_limits<double>::infinity(),
                       std::numeric_limits<double>::quiet_NaN()}) {
        testOk(!first.setIdleTimeout(bad) && first.idleTimeout() == 1800.5,
               "negative/non-finite timeout leaves configuration intact");
    }
    testOk(EmcServer::setDefaultIdleTimeout(12.5) && second.idleTimeout() == 12.5
           && first.idleTimeout() == 1800.5,
           "common setting updates existing inherited servers, preserving explicit overrides");
    for (double bad : {-1.0, std::numeric_limits<double>::infinity(),
                       -std::numeric_limits<double>::infinity(),
                       std::numeric_limits<double>::quiet_NaN()}) {
        testOk(!EmcServer::setDefaultIdleTimeout(bad)
               && EmcServer::defaultIdleTimeout() == 12.5,
               "invalid common timeout leaves the shared setting intact");
    }
    first.useDefaultIdleTimeout();
    testOk(first.idleTimeout() == 12.5, "per-server override can return to the common setting");
    EmcServer::setDefaultIdleTimeout(0.0);
    testOk(first.setIdleTimeout(0.0) && first.idleTimeout() == 0.0,
           "zero disables expiry again");
}

void testAddressCap() {
    testDiag("64 real clients from one address, then refusal and released capacity");
    Fixture f;
    std::vector<std::unique_ptr<Client>> clients;
    for (size_t i = 0; i < EMC_MAX_CONNECTIONS_PER_ADDRESS; ++i) {
        clients.emplace_back(new Client);
        f.connect(*clients.back());
    }
    testOk(NetLoopTestAccess::size(*f.loop) == 64 && f.counter->accepts == 64,
           "exactly 64 clients from the same source are accepted");
    Client excess;
    f.connect(excess);
    testOk(excess.closed() && NetLoopTestAccess::size(*f.loop) == 64,
           "65th connection from that source is closed");
    testOk(f.counter->accepts == 64 && f.counter->closes == 0,
           "cap refusal invokes neither filter acceptance nor close");
    Client other;
    f.connect(other, 0x7f000002u);
    testOk(NetLoopTestAccess::size(*f.loop) == 65,
           "another source address retains capacity");
    clients.front()->close();
    NetLoopTestAccess::read(*f.loop, 1); // EOF releases the slot through production cleanup
    Client replacement;
    f.connect(replacement);
    testOk(NetLoopTestAccess::size(*f.loop) == 65 && f.counter->closes == 1,
           "closing a client releases its per-address slot");
    NetLoopTestAccess::clear(*f.loop);
    testOk(f.counter->accepts == f.counter->closes,
           "all accepted clients receive exactly one filter close");
}

void testGlobalCap() {
    testDiag("global 512-client boundary through the real accept path");
    Fixture f;
    NetLoopTestAccess::seed(*f.loop, EMC_MAX_CONNECTIONS - 1);
    Client last;
    f.connect(last);
    const int lastId = NetLoopTestAccess::newest(*f.loop);
    testOk(NetLoopTestAccess::size(*f.loop) == 512,
           "512th connection is accepted");
    Client excess;
    f.connect(excess, 0x7f000002u);
    testOk(excess.closed() && NetLoopTestAccess::size(*f.loop) == 512,
           "513th connection is refused even from a different address");
    testOk(f.counter->accepts == 512 && f.counter->closes == 0,
           "global-cap refusal does not enter the filter chain");
    last.close();
    NetLoopTestAccess::read(*f.loop, lastId);
    Client replacement;
    f.connect(replacement, 0x7f000002u);
    testOk(NetLoopTestAccess::size(*f.loop) == 512 && f.counter->closes == 1,
           "closing a client releases global capacity");
}

void testIdleBoundaries() {
    testDiag("deterministic idle and incomplete-request deadlines");
    Fixture f;
    Client client;
    f.connect(client);
    const int id = NetLoopTestAccess::newest(*f.loop);
    const auto start = NetLoopTestAccess::activity(*f.loop, id);
    testOk(!NetLoopTestAccess::expireIdle(*f.loop, id, start + std::chrono::hours(24 * 30)),
           "disabled idle expiry preserves a month of silence");
    f.server.setIdleTimeout(10.0);
    testOk(!NetLoopTestAccess::expireIdle(*f.loop, id, start + std::chrono::seconds(9)),
           "client survives just below the configured timeout");
    testOk(NetLoopTestAccess::expireIdle(*f.loop, id, start + std::chrono::seconds(10))
           && client.closed(), "client expires at the timeout boundary");
    testOk(f.counter->closes == 1 && NetLoopTestAccess::size(*f.loop) == 0,
           "idle expiry releases capacity and closes filter state");

    Client reader;
    f.connect(reader);
    const int readerId = NetLoopTestAccess::newest(*f.loop);
    NetLoopTestAccess::setActivity(*f.loop, readerId, Clock::now() - std::chrono::seconds(9));
    reader.send(" \n"); // blank line: socket activity, no queued backend request
    const auto beforeRead = Clock::now();
    NetLoopTestAccess::read(*f.loop, readerId);
    testOk(NetLoopTestAccess::activity(*f.loop, readerId) >= beforeRead
           && !NetLoopTestAccess::expireIdle(*f.loop, readerId, beforeRead + std::chrono::seconds(9)),
           "a successful read refreshes socket activity");

    f.server.setIdleTimeout(0.0);
    reader.send("G");
    NetLoopTestAccess::read(*f.loop, readerId);
    const auto lineStart = NetLoopTestAccess::activity(*f.loop, readerId);
    reader.send("P");
    NetLoopTestAccess::read(*f.loop, readerId);
    testOk(NetLoopTestAccess::expireRequest(*f.loop, readerId,
               lineStart + std::chrono::seconds(5)) && reader.closed(),
           "fragments cannot extend the 5-second request deadline with idle expiry disabled");
}

void testRunningServer() {
    testDiag("real NetLoop: expiry, continued service, live changes and restart");
    const char* env = std::getenv("EMC_TEST_PORT");
    const unsigned short port = static_cast<unsigned short>((env ? std::atoi(env) : 15731) + 40);
    Echo echo;
    FilterChain filters;
    auto* counter = new CountingFilter;
    filters.add(std::unique_ptr<ConnectionFilter>(counter));
    EmcServer server(echo, filters, port);
    server.setIdleTimeout(0.3);
    if (!server.start()) testAbort("cannot bind idle-timeout test port (set EMC_TEST_PORT)");
    Client idle, active;
    idle.connectTo(port);
    active.connectTo(port);
    bool responsive = active.ping();
    for (int i = 0; i < 6; ++i) {
        epicsThreadSleep(0.1);
        responsive = active.ping() && responsive;
    }
    testOk(idle.closed(), "silent client expires with no readable sockets");
    testOk(responsive, "active client continues to receive replies while another expires");
    testOk(counter->closes >= 1, "expiry runs filter cleanup");
    server.setIdleTimeout(1.0);
    active.send("slow\n");
    const bool gotSlowReply = active.readLine() == "slow\n";
    epicsThreadSleep(0.6); // >1 s since request RX, <1 s since reply TX
    testOk(gotSlowReply && active.ping(), "a successful write refreshes socket activity");
    server.setIdleTimeout(0.0);
    Client retained;
    retained.connectTo(port);
    if (!retained.ping()) testAbort("cannot initialize retained idle client");
    epicsThreadSleep(0.5);
    testOk(active.ping(), "disabling expiry while running preserves a silent client");
    server.setIdleTimeout(0.1);
    testOk(retained.closed(), "new timeout applies to time already spent idle");
    testOk(active.closed(), "enabling expiry while running closes the next idle client");
    server.stop();
    testOk(server.idleTimeout() == 0.1 && counter->accepts == counter->closes,
           "stop retains configuration and closes every accepted filter session");
    if (!server.start()) testAbort("cannot restart idle-timeout test server");
    Client restarted;
    restarted.connectTo(port);
    testOk(restarted.closed(), "restart applies the retained idle timeout");
    server.stop();
}

void testIocShell() {
    testDiag("common IOC configuration across backends, validation and port changes");
    const char* env = std::getenv("EMC_TEST_PORT");
    const int port = (env ? std::atoi(env) : 15731) + 41;
    Echo echo;
    FilterChain filters;
    EmcServer otherBackend(echo, filters, static_cast<unsigned short>(port + 2));
    emcServerLimitsRegister(); // common command works without the simulation registrar
    iocshCmd("emcIdleTimeout 0.1");
    iocshCmd("emcIdleTimeout"); // show must leave it unchanged
    testOk(otherBackend.idleTimeout() == 0.1,
           "common command updates an existing non-simulation server without extra wiring");
    for (const char* bad : {"-1", "nan", "inf", "garbage", "0.2junk", "1e9999"}) {
        iocshCmd((std::string("emcIdleTimeout ") + bad).c_str());
    }
    if (!otherBackend.start()) testAbort("cannot start common-configuration test backend");
    Client nonSim;
    nonSim.connectTo(static_cast<unsigned short>(port + 2));
    testOk(nonSim.closed(), "non-simulation backend enforces the common IOC timeout");
    emcCoreRegister();
    iocshCmd("emcAclMode allow-all");
    iocshCmd(("emcSimStart " + std::to_string(port)).c_str());
    Client first;
    first.connectTo(static_cast<unsigned short>(port));
    testOk(first.closed(), "IOC shell rejects invalid values and retains the pre-start timeout");
    iocshCmd("emcSimStop");
    iocshCmd(("emcSimStart " + std::to_string(port + 1)).c_str());
    Client changedPort;
    changedPort.connectTo(static_cast<unsigned short>(port + 1));
    testOk(changedPort.closed(), "IOC shell timeout survives server recreation on a new port");
    iocshCmd("emcIdleTimeout 0");
    Client persistent;
    persistent.connectTo(static_cast<unsigned short>(port + 1));
    if (!persistent.ping()) testAbort("cannot initialize persistent IOC shell client");
    Client commonPersistent;
    commonPersistent.connectTo(static_cast<unsigned short>(port + 2));
    if (!commonPersistent.ping()) testAbort("cannot initialize common backend client");
    epicsThreadSleep(0.3);
    testOk(persistent.ping() && commonPersistent.ping(),
           "common IOC shell zero disables expiry for both running backends");
    iocshCmd("emcSimStop");
    otherBackend.stop();
}
} // namespace

MAIN(testConnectionLimits) {
    testPlan(0);
    osiSockAttach();
    testConfiguration();
    testAddressCap();
    testGlobalCap();
    testIdleBoundaries();
    testRunningServer();
    testIocShell();
    const int result = testDone();
    // IOC shell registration installs an EPICS shutdown hook. Run it while
    // the C++ service statics are alive, matching normal IOC epicsExit().
    epicsExitCallAtExits();
    osiSockRelease();
    return result;
}
