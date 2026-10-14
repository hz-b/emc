/**
 * @file testRequestAdmission.cpp
 * @brief adjustable token bucket, outstanding cap and flood isolation.
 *
 * Token refill tests inject a steady clock. Socket-pair tests drive the actual
 * request assembly/admission path without worker timing races. Worker tests
 * use real EPICS queues. Loopback tests use EMC_TEST_PORT + 60 (default 15791)
 * and a controlled blocking dispatcher; no hardware or backend is required.
 * A bind failure is a failure, not a skipped security test.
 */
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include <epicsExit.h>
#include <epicsUnitTest.h>
#include <iocsh.h>
#include <testMain.h>

#include "dispatcher.h"
#include "emc_server.h"

using Clock = std::chrono::steady_clock;

// Only access the connection map in synchronous tests, never in a live server.
struct NetLoopTestAccess {
    static void add(NetLoop& loop, int id, SOCKET socket) {
        NetLoop::Conn c;
        c.id = id;
        c.s = socket;
        c.peer.conn_id = id;
        c.peer.addr = INADDR_LOOPBACK;
        c.lastActivity = Clock::now();
        if (loop.filters_.onAccept(c.peer) != FilterVerdict::Allow)
            testAbort("test connection rejected unexpectedly");
        loop.conns_.emplace(id, std::move(c));
        loop.fd2id_[socket] = id;
    }
    static bool exists(const NetLoop& loop, int id) { return loop.conns_.count(id) != 0; }
    static std::shared_ptr<ConnectionRequests> state(NetLoop& loop, int id) {
        return loop.conns_.at(id).requests;
    }
    static void read(NetLoop& loop, int id) { loop.handleReadable(loop.conns_.at(id).s); }
    static void drop(NetLoop& loop, int id) { loop.dropConn(id); }
    static void clear(NetLoop& loop) {
        while (!loop.conns_.empty()) loop.dropConn(loop.conns_.begin()->first);
    }
};

namespace {
struct SettingsGuard {
    const double rate = EmcServer::defaultRequestRate();
    const size_t burst = EmcServer::defaultRequestBurst();
    const size_t cap = EmcServer::defaultMaxOutstandingRequests();
    ~SettingsGuard() {
        EmcServer::setDefaultRequestRate(rate);
        EmcServer::setDefaultRequestBurst(burst);
        EmcServer::setDefaultMaxOutstandingRequests(cap);
    }
};

void configure(double rate, size_t burst, size_t cap) {
    EmcServer::setDefaultRequestRate(rate);
    EmcServer::setDefaultRequestBurst(burst);
    EmcServer::setDefaultMaxOutstandingRequests(cap);
}

bool waitZero(const std::shared_ptr<ConnectionRequests>& state) {
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (state->outstanding.load() && Clock::now() < deadline) epicsThreadSleep(0.001);
    return state->outstanding.load() == 0;
}

struct Echo : Dispatcher {
    std::atomic<size_t> calls{0};
    std::string processLine(const char* line) override {
        ++calls;
        if (std::string(line) == "throw") throw std::runtime_error("test failure");
        return line;
    }
};

struct CountingFilter : ConnectionFilter {
    size_t accepts = 0, requests = 0, closes = 0;
    const char* name() const override { return "admission-test"; }
    FilterVerdict onAccept(const PeerInfo&) override { ++accepts; return FilterVerdict::Allow; }
    FilterVerdict onRequest(const PeerInfo&, const std::string& line) override {
        ++requests;
        return line == "bad" ? FilterVerdict::Reject : FilterVerdict::Allow;
    }
    void onClose(const PeerInfo&) override { ++closes; }
};

struct Fixture {
    Echo dispatcher;
    FilterChain filters;
    EmcServer server{dispatcher, filters};
    epicsMessageQueue requests, responses;
    NetLoop loop;
    std::unique_ptr<CountingFilter> filter{new CountingFilter};
    CountingFilter* counts = filter.get();

    explicit Fixture(unsigned depth = 16)
        : requests(depth, sizeof(void*)), responses(depth, sizeof(void*)),
          loop(&server, INVALID_SOCKET, &requests, &responses, filters) {
        filters.add(std::move(filter));
    }
    ~Fixture() {
        NetLoopTestAccess::clear(loop);
        void* ptr = nullptr;
        while (requests.tryReceive(&ptr, sizeof(ptr)) > 0) delete static_cast<Request*>(ptr);
        while (responses.tryReceive(&ptr, sizeof(ptr)) > 0) delete static_cast<Response*>(ptr);
    }
    SOCKET add(int id) {
        int sockets[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) testAbort("socketpair failed");
        NetLoopTestAccess::add(loop, id, sockets[0]);
        return sockets[1]; // loop owns sockets[0], caller owns sockets[1]
    }
    void send(int id, SOCKET socket, const std::string& text) {
        if (::send(socket, text.data(), text.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(text.size()))
            testAbort("could not send test command");
        NetLoopTestAccess::read(loop, id);
    }
    std::unique_ptr<Request> pop() {
        void* ptr = nullptr;
        if (requests.tryReceive(&ptr, sizeof(ptr)) <= 0) return {};
        return std::unique_ptr<Request>(static_cast<Request*>(ptr));
    }
};

struct Worker {
    std::atomic<bool> running{true};
    WorkerLoop loop;
    std::thread thread;
    explicit Worker(Fixture& f)
        : loop(&f.requests, &f.responses, &running, f.dispatcher),
          thread([this]() { loop.run(); }) {}
    ~Worker() { running = false; thread.join(); }
};

void testBucket() {
    testDiag("exact token boundaries and live changes without sleeps");
    const Clock::time_point t{};
    RequestTokenBucket bucket;
    testOk(bucket.consume(2.0, 3, t) && bucket.consume(2.0, 3, t)
           && bucket.consume(2.0, 3, t), "initial burst is accepted");
    testOk(!bucket.consume(2.0, 3, t), "one command beyond the burst is denied");
    testOk(!bucket.consume(2.0, 3, t + std::chrono::milliseconds(499)), "partial credit cannot accept a command");
    testOk(bucket.consume(2.0, 3, t + std::chrono::milliseconds(500)), "one full credit refills after exactly 500 ms");
    testOk(!bucket.consume(2.0, 3, t + std::chrono::milliseconds(500)), "accepted command consumes its credit");
    const auto later = t + std::chrono::hours(1);
    testOk(bucket.consume(2.0, 3, later) && bucket.consume(2.0, 3, later)
           && bucket.consume(2.0, 3, later) && !bucket.consume(2.0, 3, later), "long idle time accumulates only the configured burst");
    testOk(!bucket.consume(2.0, 8, later), "increasing burst does not refill an exhausted bucket");
    testOk(bucket.consume(2.0, 8, later + std::chrono::milliseconds(500)), "increased burst still permits normal refill");

    RequestTokenBucket reduced;
    reduced.consume(1.0, 10, t);
    testOk(reduced.consume(1.0, 2, t) && reduced.consume(1.0, 2, t)
           && !reduced.consume(1.0, 2, t), "reducing burst clamps existing credits");
    testOk(reduced.consume(0.0, 2, t) && reduced.consume(0.0, 2, t), "zero rate bypasses the bucket");
    testOk(reduced.consume(1.0, 2, t) && reduced.consume(1.0, 2, t)
           && !reduced.consume(1.0, 2, t), "re-enabling starts with a fresh bounded burst");

    RequestTokenBucket changed;
    changed.consume(2.0, 1, t);
    testOk(changed.consume(4.0, 1, t + std::chrono::milliseconds(500)), "rate change first refills using the old observed rate");
    testOk(!changed.consume(4.0, 1, t + std::chrono::milliseconds(749))
           && changed.consume(4.0, 1, t + std::chrono::milliseconds(750)), "subsequent refill uses the changed rate");
    RequestTokenBucket huge;
    huge.consume(std::numeric_limits<double>::max(), 1, t);
    testOk(huge.consume(std::numeric_limits<double>::max(), 1, later), "large finite rates remain clamped without overflow of the bucket");
}

void testSettings() {
    testDiag("common IOC shell commands, disabled settings and rejected values");
    testOk(EmcServer::defaultRequestRate() == EMC_DEFAULT_REQUEST_RATE
           && EmcServer::defaultRequestBurst() == EMC_DEFAULT_REQUEST_BURST
           && EmcServer::defaultMaxOutstandingRequests() == EMC_DEFAULT_MAX_OUTSTANDING_REQUESTS, "compiled defaults are active");
    SettingsGuard guard;
    emcServerLimitsRegister();
    emcServerLimitsRegister(); // idempotent
    iocshCmd("emcRequestRate 12.5");
    iocshCmd("emcRequestBurst 7");
    iocshCmd("emcMaxOutstandingRequests 4");
    testOk(EmcServer::defaultRequestRate() == 12.5 && EmcServer::defaultRequestBurst() == 7
           && EmcServer::defaultMaxOutstandingRequests() == 4, "shell commands set all three settings");
    iocshCmd("emcRequestRate");
    iocshCmd("emcRequestBurst");
    iocshCmd("emcMaxOutstandingRequests");
    testOk(EmcServer::defaultRequestRate() == 12.5 && EmcServer::defaultRequestBurst() == 7
           && EmcServer::defaultMaxOutstandingRequests() == 4, "omitted arguments show without changing values");
    for (const char* value : {"-1", "nan", "inf", "1e999", "bad", "3junk"}) {
        iocshCmd((std::string("emcRequestRate ") + value).c_str());
        testOk(EmcServer::defaultRequestRate() == 12.5, "invalid rate '%s' leaves old value intact", value);
    }
    for (const char* value : {"-1", "+2", "1.5", "bad", "99999999999999999999999999999999"}) {
        iocshCmd((std::string("emcRequestBurst ") + value).c_str());
        iocshCmd((std::string("emcMaxOutstandingRequests ") + value).c_str());
        testOk(EmcServer::defaultRequestBurst() == 7 && EmcServer::defaultMaxOutstandingRequests() == 4,
               "invalid count '%s' leaves old values intact", value);
    }
    iocshCmd("emcRequestBurst 0");
    testOk(EmcServer::defaultRequestBurst() == 7, "zero burst is rejected");
    testOk(!EmcServer::setDefaultRequestRate(-1) && !EmcServer::setDefaultRequestRate(std::numeric_limits<double>::quiet_NaN())
           && !EmcServer::setDefaultRequestRate(std::numeric_limits<double>::infinity())
           && !EmcServer::setDefaultRequestBurst(0), "C++ settings reject negative/non-finite rates and zero burst");
    iocshCmd("emcRequestRate 0");
    iocshCmd("emcMaxOutstandingRequests 0");
    testOk(EmcServer::defaultRequestRate() == 0 && EmcServer::defaultMaxOutstandingRequests() == 0,
           "zero disables rate and outstanding checks independently");
}

void testAssemblyAndIsolation() {
    SettingsGuard guard;
    configure(0.001, 2, 0); // refill cannot hide a burst violation
    Fixture f;
    const SOCKET a = f.add(1), b = f.add(2);
    const auto state = NetLoopTestAccess::state(f.loop, 1);
    f.send(1, a, "\r\n  \nfrag");
    testOk(!f.pop() && f.counts->requests == 0 && state->outstanding == 0, "blank lines and an incomplete fragment consume no admission");
    f.send(1, a, "ment\r\nB\n");
    auto first = f.pop(), second = f.pop();
    testOk(first && second && first->line == "fragment" && second->line == "B"
           && state->outstanding == 2 && f.counts->requests == 2, "fragmented CRLF and second command each consume one credit and slot");
    first.reset(); second.reset();
    testOk(state->outstanding == 0, "disposing queued work releases both outstanding slots");
    f.send(1, a, "excess\n");
    testOk(!NetLoopTestAccess::exists(f.loop, 1) && !state->open && !f.pop(), "completed commands do not restore rate credits; excess closes its connection");
    f.send(2, b, "other\n");
    auto other = f.pop();
    testOk(other && other->conn_id == 2 && NetLoopTestAccess::exists(f.loop, 2), "same-IP client has its own allowance and remains usable");
    epicsSocketDestroy(a); epicsSocketDestroy(b);
}

void testOutstandingAndFilters() {
    SettingsGuard guard;
    configure(0, 10, 2);
    Fixture f;
    const SOCKET a = f.add(1);
    const auto state = NetLoopTestAccess::state(f.loop, 1);
    f.send(1, a, "A\nbad\n");
    auto first = f.pop(), rejected = f.pop();
    testOk(first && rejected && rejected->rejected && state->outstanding == 2, "accepted and filter-rejected commands both occupy outstanding slots");
    first->complete(); first->complete();
    testOk(state->outstanding == 1, "completion releases one slot exactly once");
    f.send(1, a, "C\n");
    auto third = f.pop();
    testOk(third && state->outstanding == 2, "a released slot permits another command with rate disabled");
    EmcServer::setDefaultMaxOutstandingRequests(1);
    f.send(1, a, "excess\n");
    testOk(!state->open && !NetLoopTestAccess::exists(f.loop, 1) && !f.pop(), "lowered live cap closes on the next excess command");
    testOk(f.counts->requests == 3 && f.counts->closes == 1, "over-limit commands never reach filters; close hook runs once");
    rejected.reset(); third.reset();
    testOk(state->outstanding == 0, "closed connection state survives until its outstanding work is disposed");
    epicsSocketDestroy(a);

    configure(0.001, 1, 0);
    Fixture rate;
    const SOCKET b = rate.add(1);
    rate.send(1, b, "bad\nbad\n");
    auto bad = rate.pop();
    testOk(bad && bad->rejected && !rate.pop() && rate.counts->requests == 1
           && !NetLoopTestAccess::exists(rate.loop, 1), "filter rejections consume rate credits, including commands in the same read");
    epicsSocketDestroy(b);
}

void testQueueFailureAndWorker() {
    SettingsGuard guard;
    configure(0, 10, 0);
    {
        Fixture f(1);
        const SOCKET a = f.add(1);
        const auto state = NetLoopTestAccess::state(f.loop, 1);
        f.send(1, a, "A\nB\n");
        testOk(!state->open && state->outstanding == 1 && !NetLoopTestAccess::exists(f.loop, 1), "full global queue rolls back failed admission and closes the sender");
        f.pop().reset();
        testOk(state->outstanding == 0, "queue-full rollback leaves no outstanding-count leak");
        epicsSocketDestroy(a);
    }
    {
        Fixture f;
        const SOCKET a = f.add(1), b = f.add(2);
        const auto closed = NetLoopTestAccess::state(f.loop, 1);
        const auto live = NetLoopTestAccess::state(f.loop, 2);
        f.send(1, a, "cancelled1\ncancelled2\n");
        NetLoopTestAccess::drop(f.loop, 1);
        f.send(2, b, "throw\nbad\nafter\n");
        Worker worker(f);
        bool replies = true;
        for (const char* expected : {"ERR exception", "ERR rejected", "after"}) {
            void* ptr = nullptr;
            const int rc = f.responses.receive(&ptr, sizeof(ptr), 2.0);
            std::unique_ptr<Response> response(static_cast<Response*>(ptr));
            replies = replies && rc > 0 && response && response->conn_id == 2 && response->line == expected;
            testOk(live->outstanding <= 2, "completed slot is released before its response is visible");
        }
        testOk(replies, "exception, rejection and normal replies stay in order");
        testOk(waitZero(closed) && waitZero(live) && f.dispatcher.calls == 2, "worker skips closed-client work, releases all slots and continues after an exception");
        epicsSocketDestroy(a); epicsSocketDestroy(b);
    }
    {
        Fixture f(1);
        const SOCKET a = f.add(1);
        const auto state = NetLoopTestAccess::state(f.loop, 1);
        auto* response = new Response{99, "occupied"};
        void* ptr = response;
        if (f.responses.trySend(&ptr, sizeof(ptr)) != 0) testAbort("could not fill response queue");
        f.send(1, a, "A\n");
        Worker worker(f);
        testOk(waitZero(state) && f.dispatcher.calls == 1, "a full response queue still releases the completed outstanding slot");
        epicsSocketDestroy(a);
    }
}

struct Client {
    SOCKET socket = INVALID_SOCKET;
    ~Client() { if (socket != INVALID_SOCKET) epicsSocketDestroy(socket); }
    void connectTo(unsigned short port) {
        socket = epicsSocketCreate(AF_INET, SOCK_STREAM, 0);
        if (socket == INVALID_SOCKET) testAbort("could not create loopback client");
        timeval timeout{2, 0};
        if (setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout)) != 0)
            testAbort("could not set receive timeout");
        sockaddr_in peer{};
        peer.sin_family = AF_INET;
        peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        peer.sin_port = htons(port);
        if (::connect(socket, reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) != 0) testAbort("could not connect loopback client");
    }
    void send(const std::string& text) {
        size_t sent = 0;
        while (sent < text.size()) {
            const ssize_t n = ::send(socket, text.data() + sent, text.size() - sent, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) testAbort("could not send loopback command");
            sent += static_cast<size_t>(n);
        }
    }
    std::string line() {
        std::string result;
        char ch;
        while (recv(socket, &ch, 1, 0) == 1) {
            result += ch;
            if (ch == '\n') break;
        }
        return result;
    }
    bool closed() {
        char data[128];
        for (;;) {
            const ssize_t n = recv(socket, data, sizeof(data), 0);
            if (n == 0) return true;
            if (n < 0) return errno == ECONNRESET;
        }
    }
};

struct BlockingEcho : Dispatcher {
    epicsEvent entered, release;
    std::atomic<size_t> calls{0}, cancelledCalls{0};
    std::string processLine(const char* line) override {
        ++calls;
        const std::string command(line);
        if (command == "BLOCK") {
            entered.signal();
            release.wait(); // controlled exclusively by the test
        }
        if (command.find("queued") == 0) ++cancelledCalls;
        return command;
    }
};

void testLiveServer() {
    testDiag("loopback flood isolation, blocking recovery and stop/start");
    SettingsGuard guard;
    configure(0.001, 2, 0);
    const char* env = std::getenv("EMC_TEST_PORT");
    const long portValue = (env ? std::strtol(env, nullptr, 10) : 15731) + 60;
    if (portValue < 1 || portValue > 65535) testAbort("invalid EMC_TEST_PORT");
    const auto port = static_cast<unsigned short>(portValue);
    BlockingEcho dispatcher;
    FilterChain filters;
    EmcServer server(dispatcher, filters, port);
    if (!server.start()) testAbort("could not bind admission test port (set EMC_TEST_PORT)");
    {
        Client flood, normal;
        flood.connectTo(port); normal.connectTo(port);
        std::string commands;
        for (int i = 0; i < 100; ++i) commands += "flood\n";
        flood.send(commands);
        testOk(flood.closed(), "rate flood closes only the flooding connection");
        const auto start = Clock::now();
        normal.send("status\n");
        const auto reply = normal.line();
        const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
        testDiag("normal-client response during rate flood: %.6f seconds", elapsed);
        testOk(reply == "status\n" && elapsed < 1.0, "another same-IP client responds within the test latency bound");
        EmcServer::setDefaultRequestRate(0);
        normal.send("one\ntwo\nthree\n");
        testOk(normal.line() == "one\n" && normal.line() == "two\n" && normal.line() == "three\n", "disabling rate applies to an existing connection");
    }

    configure(0, 10, 3);
    {
        Client flood, normal;
        flood.connectTo(port); normal.connectTo(port);
        flood.send("BLOCK\n");
        const bool entered = dispatcher.entered.wait(2.0);
        testOk(entered, "controlled command occupies the sole worker");
        if (!entered) { dispatcher.release.signal(); testAbort("blocking dispatcher did not start"); }
        flood.send("queued1\nqueued2\nexcess\n");
        const bool closed = flood.closed();
        testOk(closed, "cap includes executing command and closes on the fourth outstanding request");
        normal.send("status\n");
        const auto start = Clock::now();
        dispatcher.release.signal(); // always release, including after failures
        const auto reply = normal.line();
        const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
        testDiag("normal-client response after blocked command release: %.6f seconds", elapsed);
        testOk(reply == "status\n" && elapsed < 1.0 && dispatcher.cancelledCalls == 0,
               "after release, closed-client backlog is skipped and another client recovers promptly");
        EmcServer::setDefaultMaxOutstandingRequests(1);
        bool sequential = true;
        for (int i = 0; i < 20; ++i) {
            normal.send("next\n");
            sequential = sequential && normal.line() == "next\n";
            if (!sequential) break;
        }
        testOk(sequential, "cap of one permits sequential commands immediately after replies");
    }

    // Shutdown waits for the in-flight call and discards queued work safely.
    configure(0, 10, 8);
    Client pending;
    pending.connectTo(port);
    pending.send("BLOCK\nqueuedShutdown\n");
    const bool entered = dispatcher.entered.wait(2.0);
    testOk(entered, "shutdown test enters controlled dispatcher");
    if (!entered) { dispatcher.release.signal(); testAbort("shutdown dispatcher did not start"); }
    std::atomic<bool> stopped{false};
    std::thread stopper([&]() { server.stop(); stopped = true; });
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (server.isRunning() && Clock::now() < deadline) epicsThreadSleep(0.001);
    testOk(!server.isRunning() && !stopped, "stop signals shutdown but waits for the active dispatcher call");
    dispatcher.release.signal();
    stopper.join();
    testOk(stopped && dispatcher.cancelledCalls == 0, "shutdown completes after release without executing queued work");
    testOk(server.start(), "same server restarts after cleanup");
    if (server.isRunning()) {
        Client fresh;
        fresh.connectTo(port);
        fresh.send("fresh\n");
        testOk(fresh.line() == "fresh\n", "restart serves new commands without stale requests or replies");
    } else testOk(false, "restart must serve a new client");
    server.stop();
}
} // namespace

MAIN(testRequestAdmission) {
    testPlan(0);
    testBucket();
    testSettings();
    testAssemblyAndIsolation();
    testOutstandingAndFilters();
    testQueueFailureAndWorker();
    testLiveServer();
    const int result = testDone();
    epicsExit(result);
    return result;
}
