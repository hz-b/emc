/**
 * @file testRequestLimits.cpp
 * @brief regression: bounded request assembly and absolute deadlines.
 *
 * Run with make runtests or directly. Uses real loopback sockets on
 * EMC_TEST_PORT + 30 (default 15761); binding failure is a test failure.
 * Deadline tests use the production timeout and take about 12 seconds.
 * also checks reply budgets and a client that sends without reading.
 */

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <epicsThread.h>
#include <epicsUnitTest.h>
#include <osiSock.h>
#include <testMain.h>

#include "connection_filter.h"
#include "dispatcher.h"
#include "emc_server.h"

// No worker threads or socket timing in these boundary/accounting checks.
// This test-only friend exercises the real shared-queue drain implementation.
struct NetLoopTestAccess {
    static void testReplyBudgets() {
        testDiag("reply byte boundaries and aggregate accounting");
        struct DummyDispatcher : Dispatcher {
            std::string processLine(const char*) override { return ""; }
        } dispatcher;
        FilterChain filters;
        EmcServer server(dispatcher, filters);
        epicsMessageQueue requests(4, sizeof(void*)), responses(4, sizeof(void*));
        NetLoop loop(&server, INVALID_SOCKET, &requests, &responses, filters);
        const auto addClient = [&](int id) {
            NetLoop::Conn c;
            c.id = id;
            c.peer.conn_id = id;
            loop.conns_.emplace(id, std::move(c));
        };
        const auto reply = [&](int id, size_t bytes) {
            auto* response = new Response{id, std::string(bytes, 'R')};
            void* ptr = response;
            if (responses.trySend(&ptr, sizeof(ptr)) != 0) {
                delete response;
                testAbort("cannot queue reply-budget test response");
            }
            loop.pumpResponses();
        };
        const auto accounted = [&]() {
            size_t sum = 0;
            for (const auto& entry : loop.conns_) {
                if (entry.second.outbuf.size() > EMC_MAX_CLIENT_REPLY_BYTES) return false;
                sum += entry.second.outbuf.size();
            }
            return sum == loop.pendingReplyBytes_ && sum <= EMC_MAX_TOTAL_REPLY_BYTES;
        };

        server.setReplyTerminator(ReplyTerminator::CRLF);
        addClient(1);
        reply(1, EMC_MAX_CLIENT_REPLY_BYTES - 2);
        testOk(loop.conns_.at(1).outbuf.size() == EMC_MAX_CLIENT_REPLY_BYTES && accounted(),
               "exact client limit accepts the full reply including CRLF");
        reply(1, 0); // even an empty reply needs its two-byte terminator
        testOk(loop.conns_.count(1) == 0 && loop.pendingReplyBytes_ == 0 && accounted(),
               "terminator overflow closes client and releases all pending bytes");
        reply(1, 100);
        testOk(loop.pendingReplyBytes_ == 0 && accounted(),
               "late queued replies for a closed client consume no budget");
        addClient(2);
        reply(2, EMC_MAX_CLIENT_REPLY_BYTES - 1);
        testOk(loop.conns_.count(2) == 0 && accounted(),
               "single reply one byte over the client limit is rejected before append");

        server.setReplyTerminator(ReplyTerminator::None);
        const size_t clients = EMC_MAX_TOTAL_REPLY_BYTES / EMC_MAX_CLIENT_REPLY_BYTES;
        for (size_t i = 0; i < clients; ++i) {
            const int id = static_cast<int>(i) + 10;
            addClient(id);
            reply(id, EMC_MAX_CLIENT_REPLY_BYTES - (i + 1 == clients ? 1 : 0));
        }
        addClient(3);
        reply(3, 1);
        testOk(loop.pendingReplyBytes_ == EMC_MAX_TOTAL_REPLY_BYTES && accounted(),
               "multiple clients may reach exactly the total limit");
        reply(3, 1);
        testOk(loop.conns_.count(3) == 0
                   && loop.pendingReplyBytes_ == EMC_MAX_TOTAL_REPLY_BYTES - 1 && accounted(),
               "one excess total byte closes only its client and releases its old bytes");
        addClient(4);
        server.setReplyTerminator(ReplyTerminator::CRLF);
        reply(4, 0);
        testOk(loop.conns_.count(4) == 0 && accounted(),
               "CRLF also counts toward the total budget");
        loop.dropConn(10);
        addClient(5);
        reply(5, EMC_MAX_CLIENT_REPLY_BYTES - 2);
        testOk(loop.conns_.count(5) == 1 && accounted(),
               "disconnect frees total budget for a different client");
        while (!loop.conns_.empty()) loop.dropConn(loop.conns_.begin()->first);
        testOk(loop.pendingReplyBytes_ == 0 && accounted(),
               "closing all clients returns total accounting to zero");
    }
};

namespace {
using Clock = std::chrono::steady_clock;

double elapsed(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

void testSplitterLimits() {
    testDiag("request length is checked before storing incoming bytes");
    const std::string full(EMC_MAX_REQUEST_BYTES, 'A');
    std::string line;
    RequestLineSplitter fragmented;
    bool bounded = true;
    // One byte at a time exercises the incremental search cursor as well as
    // counting bytes across reads. No complete line should be returned yet.
    for (size_t i = 0; i < full.size(); ++i) {
        bounded = fragmented.append("A", 1) && bounded;
        bounded = !fragmented.next(line) && bounded;
        bounded = fragmented.pending() == i + 1 && bounded;
    }
    testOk(bounded, "fragmented request reaches exactly the limit without dispatch");
    testOk(fragmented.append("\r", 1) && fragmented.next(line) && line == full,
           "exactly-at-limit request accepts its later CR terminator");
    testOk(!fragmented.next(line) && fragmented.pending() == 0,
           "completed request leaves no incomplete bytes");
    testOk(fragmented.append("\nB\n", 3) && fragmented.next(line) && line == "B"
               && !fragmented.next(line) && fragmented.pending() == 0,
           "LF following a split CRLF is skipped, next command is preserved");

    RequestLineSplitter overflow;
    testOk(overflow.append(full.data(), full.size()) && !overflow.next(line),
           "unterminated request is allowed to reach the exact limit");
    testOk(!overflow.append("A", 1) && overflow.pending() == EMC_MAX_REQUEST_BYTES,
           "first excess byte is rejected without increasing buffered bytes");
    testOk(!overflow.append("A\r\n", 3) && overflow.pending() == EMC_MAX_REQUEST_BYTES,
           "terminating an oversized request does not bypass the check");

    // Large individual fragments must be refused before allocating/copying
    // their contents. The existing small incomplete prefix stays unchanged.
    RequestLineSplitter huge;
    testOk(huge.append("ABC", 3) && !huge.next(line), "small prefix is buffered");
    const std::string megabyte(size_t{1024} * 1024, 'X');
    bool unchanged = true;
    for (int i = 0; i < 32; ++i) {
        unchanged = !huge.append(megabyte.data(), megabyte.size()) && unchanged;
        unchanged = huge.pending() == 3 && unchanged;
    }
    testOk(unchanged, "repeated 1 MiB unterminated fragments never grow the buffer");
    const std::string oversized = full + "X\r\n";
    RequestLineSplitter terminated;
    testOk(!terminated.append(oversized.data(), oversized.size())
               && terminated.pending() == 0,
           "oversized terminated request is refused before any bytes are stored");
    const std::string blanks(EMC_MAX_REQUEST_BYTES + 1, ' ');
    RequestLineSplitter blank;
    testOk(!blank.append(blanks.data(), blanks.size()) && blank.pending() == 0,
           "spaces count toward the limit, even for a blank request");
    const std::string zeros(EMC_MAX_REQUEST_BYTES + 1, '\0');
    RequestLineSplitter binary;
    testOk(!binary.append(zeros.data(), zeros.size()) && binary.pending() == 0,
           "embedded NUL bytes also count toward the framing limit");

    RequestLineSplitter batch;
    const std::string several = full + "\r\n" + full + "\nEND";
    testOk(batch.append(several.data(), several.size()),
           "several legal requests may exceed the limit in total");
    testOk(batch.next(line) && line == full && batch.next(line) && line == full
               && !batch.next(line) && batch.pending() == 3,
           "each batched request is separate; incomplete tail is retained");
    testOk(batch.append("\r", 1) && batch.next(line) && line == "END"
               && !batch.next(line) && batch.pending() == 0,
           "incomplete tail completes on a later read");
}

// Short replies allow the socket tests to count dispatches without accumulating
// large output buffers. Requests rejected during assembly must never arrive here.
struct CountingDispatcher : Dispatcher {
    std::atomic<size_t> calls{0};
    std::string processLine(const char* line) override {
        ++calls;
        return "bytes=" + std::to_string(std::strlen(line));
    }
};

struct Client {
    SOCKET s = INVALID_SOCKET;
    bool closed = false;
    std::string received;

    Client() = default;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    ~Client() { if (s != INVALID_SOCKET) epicsSocketDestroy(s); }

    void connectTo(unsigned short port, int receiveBytes = 0) {
        s = epicsSocketCreate(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET) testAbort("cannot create test socket");
        if (receiveBytes > 0 && setsockopt(s, SOL_SOCKET, SO_RCVBUF,
                reinterpret_cast<char*>(&receiveBytes), sizeof(receiveBytes)) != 0) {
            testAbort("cannot reduce slow-reader receive buffer");
        }
        timeval tv{0, 200000};
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&tv), sizeof(tv));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        if (connect(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            testAbort("cannot connect to request-limit test server");
        }
    }

    bool sendAll(const std::string& text) const {
        size_t pos = 0;
        while (pos < text.size()) {
            const ssize_t n = ::send(s, text.data() + pos, text.size() - pos, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            pos += static_cast<size_t>(n);
        }
        return true;
    }

    void readAvailable() {
        char buf[256];
        while (!closed) {
            const ssize_t n = recv(s, buf, sizeof(buf), MSG_DONTWAIT);
            if (n > 0) {
                received.append(buf, static_cast<size_t>(n));
            } else if (n == 0 || errno == ECONNRESET) {
                closed = true;
            } else if (errno == EINTR) {
                continue;
            } else {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    testFail("unexpected test recv error: %s", std::strerror(errno));
                }
                break;
            }
        }
    }

    std::string readLine() {
        const auto start = Clock::now();
        do {
            readAvailable();
            const size_t end = received.find('\n');
            if (end != std::string::npos) {
                const std::string line = received.substr(0, end + 1);
                received.erase(0, end + 1);
                return line;
            }
            if (closed) break;
            epicsThreadSleep(0.005);
        } while (elapsed(start) < 1.0);
        return "";
    }

    bool waitClosed(double seconds = 1.0) {
        const auto start = Clock::now();
        do {
            readAvailable();
            if (closed) return true;
            epicsThreadSleep(0.005);
        } while (elapsed(start) < seconds);
        return false;
    }
};

bool ping(Client& c) {
    return c.sendAll("ping\n") && c.readLine() == "OK\n";
}

void testSlowReader() {
    testDiag("client sends valid requests but never reads replies");
    // This test must reach the pending-reply budget, not an admission quota.
    // admission controls have their own testRequestAdmission coverage.
    struct AdmissionGuard {
        const double rate = EmcServer::defaultRequestRate();
        const size_t cap = EmcServer::defaultMaxOutstandingRequests();
        AdmissionGuard() {
            EmcServer::setDefaultRequestRate(0);
            EmcServer::setDefaultMaxOutstandingRequests(0);
        }
        ~AdmissionGuard() {
            EmcServer::setDefaultRequestRate(rate);
            EmcServer::setDefaultMaxOutstandingRequests(cap);
        }
    } admissionGuard;
    struct ReplyDispatcher : Dispatcher {
        std::string processLine(const char*) override {
            return std::string(4096, 'R'); // each reply is well below the limit
        }
    } dispatcher;
    struct CloseObserver : ConnectionFilter {
        std::atomic<size_t> closed{0};
        const char* name() const override { return "test-close-observer"; }
        void onClose(const PeerInfo&) override { ++closed; }
    };
    FilterChain filters;
    auto observer = std::make_unique<CloseObserver>();
    auto* closures = observer.get();
    filters.add(std::move(observer));
    const char* env = std::getenv("EMC_TEST_PORT");
    const unsigned short port = static_cast<unsigned short>((env ? std::atoi(env) : 15731) + 31);
    EmcServer server(dispatcher, filters, port);
    if (!testOk(server.start(), "reply security server starts on port %u", port)) return;
    Client healthy, slow;
    healthy.connectTo(port);
    slow.connectTo(port, 4096);
    testOk(ping(healthy), "second client works before slow-reader flood");
    bool otherServed = true;
    size_t sent = 0;
    const auto start = Clock::now();
    // Small paced batches let the kernel send buffer fill before application
    // output backs up. Observe closure through the filter, without reading slow.
    while (closures->closed.load() == 0 && elapsed(start) < 10.0) {
        if (!slow.sendAll("GPE\nGPE\nGPE\nGPE\n")) break;
        sent += 4;
        if (sent % 40 == 0) otherServed = ping(healthy) && otherServed;
        epicsThreadSleep(0.02);
    }
    const auto closing = Clock::now();
    while (closures->closed.load() == 0 && elapsed(closing) < 1.0) epicsThreadSleep(0.005);
    testOk(sent > 16 && closures->closed.load() == 1,
           "non-reading client is closed after repeated small replies (%zu requests)", sent);
    testOk(otherServed && ping(healthy),
           "second client remains responsive during and after slow-reader closure");
    // Reading now drains only bytes sent before closure. This also confirms
    // closure on the wire instead of relying solely on the filter notification.
    testOk(slow.waitClosed(3.0), "slow client observes TCP closure when it finally reads");
    bool draining = true;
    const std::string expected = std::string(4096, 'R') + "\n";
    for (int i = 0; i < 40; ++i) {
        draining = healthy.sendAll("GPE\n") && healthy.readLine() == expected && draining;
    }
    testOk(draining, "successful sends release budget across more than one client allowance");
    server.stop();
}

void testSocketLengths(unsigned short port, CountingDispatcher& dispatcher, Client& healthy) {
    testDiag("real TCP fragmentation and oversized requests");
    {
        Client c;
        c.connectTo(port);
        const bool sent = c.sendAll("SPE ");
        epicsThreadSleep(0.02);
        const bool sentMore = c.sendAll("400");
        epicsThreadSleep(0.02);
        c.readAvailable();
        testOk(sent && sentMore && c.received.empty() && !c.closed,
               "fragmented TCP request remains pending without a reply");
        testOk(ping(healthy), "another client is served while a request is incomplete");
        testOk(c.sendAll("\r") && c.readLine() == "bytes=7\n",
               "fragmented command is dispatched when its CR arrives");
    }
    const std::string full(EMC_MAX_REQUEST_BYTES, 'X');
    {
        Client c;
        c.connectTo(port);
        testOk(c.sendAll(full) && c.sendAll("\r")
                   && c.readLine() == "bytes=" + std::to_string(full.size()) + "\n",
               "exactly-at-limit TCP request is accepted");
        testOk(c.sendAll("\nGPE\n") && c.readLine() == "bytes=3\n",
               "CRLF split across reads preserves the next TCP request");
    }
    {
        Client c;
        c.connectTo(port);
        const std::string batch = full + "\n" + full + "\r\n";
        const std::string reply = "bytes=" + std::to_string(full.size()) + "\n";
        testOk(c.sendAll(batch) && c.readLine() == reply && c.readLine() == reply,
               "batched TCP requests are checked per line, not by total size");
    }

    const std::vector<std::string> bad = {
        full + "X\n",              // oversized, LF terminated
        full + "X\r\nGPE\n",      // oversized, CRLF terminated with a later command
        full + "X",                 // oversized, no terminator
    };
    const char* names[] = {"LF terminated", "CRLF terminated", "unterminated"};
    for (size_t i = 0; i < bad.size(); ++i) {
        Client c;
        c.connectTo(port);
        const size_t before = dispatcher.calls.load();
        // Sending may report an error if the server closes while bytes are
        // still being sent. Either way, the observable result must be closure.
        c.sendAll(bad[i]);
        testOk(c.waitClosed() && c.received.empty(),
               "oversized %s request closes its client without a reply", names[i]);
        testOk(dispatcher.calls.load() == before,
               "oversized %s request never reaches the dispatcher", names[i]);
        testOk(ping(healthy), "another client remains served after %s rejection", names[i]);
    }
}

void testSocketDeadlines(unsigned short port, CountingDispatcher& dispatcher, Client& healthy) {
    testDiag("absolute deadline, silence, trickle traffic and new lines");
    Client silent, trickle, reset, idle;
    silent.connectTo(port);
    trickle.connectTo(port);
    reset.connectTo(port);
    idle.connectTo(port);  // no incomplete request: must survive the whole test
    const size_t before = dispatcher.calls.load();
    testOk(silent.sendAll("A") && trickle.sendAll("B") && reset.sendAll("C"),
           "three incomplete requests start");
    const auto start = Clock::now();
    const double timeout = EMC_REQUEST_TIMEOUT_SECONDS;
    double silentClosed = -1.0, trickleClosed = -1.0;
    double lastTraffic = -1.0;
    bool otherServed = true, earlyTrickleSent = true;
    bool restarted = false, completed = false;
    size_t fragments = 0;

    while (elapsed(start) < timeout + 1.3) {
        const double age = elapsed(start);
        silent.readAvailable();
        trickle.readAvailable();
        if (silent.closed && silentClosed < 0) silentClosed = age;
        if (trickle.closed && trickleClosed < 0) trickleClosed = age;
        if (age - lastTraffic >= 0.2) {
            lastTraffic = age;
            if (!trickle.closed) {
                const bool sent = trickle.sendAll("B");
                if (age < timeout - 0.5) earlyTrickleSent = sent && earlyTrickleSent;
                if (sent) ++fragments;
            }
            otherServed = ping(healthy) && otherServed;
        }
        if (!restarted && age >= timeout * 0.6) {
            restarted = true;
            // Complete C, then begin D in the SAME read. D must get its own
            // timer instead of inheriting C's nearly expired timer.
            testOk(reset.sendAll("\nD") && reset.readLine() == "bytes=1\n",
                   "complete first line and start a new incomplete line");
        }
        if (!completed && age >= timeout + 1.0) {
            completed = true;
            testOk(reset.sendAll("\n") && reset.readLine() == "bytes=1\n",
                   "new line gets a fresh deadline and completes successfully");
        }
        epicsThreadSleep(0.01);
    }
    testOk(silentClosed >= timeout - 0.2 && silentClosed < timeout + 1.0
               && silent.received.empty(),
           "silent incomplete request expires at the deadline (%.3fs)", silentClosed);
    testOk(earlyTrickleSent && fragments > 10 && trickleClosed >= timeout - 0.2
               && trickleClosed < timeout + 1.0 && trickle.received.empty(),
           "small fragments never extend the deadline (%zu fragments, %.3fs)",
           fragments, trickleClosed);
    testOk(dispatcher.calls.load() == before + 2,
           "expired requests never dispatch; only the two completed lines do");
    testOk(otherServed, "another client receives replies throughout the deadline test");
    testOk(ping(idle), "idle connection without an incomplete request stays usable");
}

void testServerLimits() {
    const char* env = std::getenv("EMC_TEST_PORT");
    const unsigned short port = static_cast<unsigned short>((env ? std::atoi(env) : 15731) + 30);
    CountingDispatcher dispatcher;
    FilterChain open;  // empty test chain accepts loopback clients
    EmcServer server(dispatcher, open, port);
    if (!testOk(server.start(), "request security server starts on port %u", port)) return;
    Client healthy;
    healthy.connectTo(port);
    testOk(ping(healthy), "healthy client is served before security checks");
    testSocketLengths(port, dispatcher, healthy);
    testSocketDeadlines(port, dispatcher, healthy);
    {
        testDiag("deadline enforcement when every socket is quiet");
        Client c;
        c.connectTo(port);
        const size_t before = dispatcher.calls.load();
        const auto start = Clock::now();
        testOk(c.sendAll("GPE"), "send one incomplete request, then stop all traffic");
        const bool closed = c.waitClosed(EMC_REQUEST_TIMEOUT_SECONDS + 1.0);
        testOk(closed && c.received.empty()
                   && elapsed(start) >= EMC_REQUEST_TIMEOUT_SECONDS - 0.2,
               "deadline still closes the client without any socket activity");
        testOk(dispatcher.calls.load() == before,
               "quiet incomplete request never dispatches");
        testOk(ping(healthy), "healthy client is served after the quiet timeout");
    }
    server.stop();
}
} // namespace

MAIN(testRequestLimits) {
    testPlan(0);
    osiSockAttach();
    testSplitterLimits();
    NetLoopTestAccess::testReplyBudgets();
    testServerLimits();
    testSlowReader();
    osiSockRelease();
    return testDone();
}
