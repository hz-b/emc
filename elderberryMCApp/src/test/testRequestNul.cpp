/**
 * @file testRequestNul.cpp
 * @brief reject embedded NUL before request filters and dispatch.
 *
 * Uses the real server over loopback and a stateful C-string dispatcher.
 * Run with "make runtests" or directly. Uses EMC_TEST_PORT + 60 (default 15791).
 */

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include <epicsUnitTest.h>
#include <osiSock.h>
#include <testMain.h>

#include "connection_filter.h"
#include "dispatcher.h"
#include "emc_server.h"

namespace {

// Preserve embedded NULs; constructing std::string from just a char* would
// reproduce the truncation in the client instead of testing the server.
template <size_t N>
std::string bytes(const char (&text)[N]) {
    return std::string(text, N - 1);
}

struct CountingFilter : ConnectionFilter {
    std::atomic<unsigned> requests{0};
    const char* name() const override { return "nul-test"; }
    FilterVerdict onRequest(const PeerInfo&, const std::string&) override {
        ++requests;
        return FilterVerdict::Allow;
    }
};

// SSV changes a setting, SGV reads it. A truncated "SSV 2\0..." really
// changes this setting without the fix, so the test checks an observable effect.
struct StatefulDispatcher : Dispatcher {
    std::atomic<unsigned> calls{0};
    std::atomic<int> speed{1};

    std::string processLine(const char* line) override {
        ++calls;
        if (std::strcmp(line, "SSV 2") == 0) {
            speed.store(2);
            return "t";
        }
        if (std::strcmp(line, "SGV") == 0) {
            return speed.load() == 1 ? "t 1.000000" : "t 2.000000";
        }
        return "f";
    }
};

struct Client {
    SOCKET s = INVALID_SOCKET;
    ~Client() { if (s != INVALID_SOCKET) epicsSocketDestroy(s); }

    bool connectTo(unsigned short port) {
        s = epicsSocketCreate(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET) return false;
        timeval timeout{2, 0};
        if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
            return false;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        return ::connect(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    }

    bool send(const std::string& text) {
        size_t sent = 0;
        while (sent < text.size()) {
            const ssize_t n = ::send(s, text.data() + sent, text.size() - sent, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    std::string readLine() {
        std::string result;
        char ch;
        while (result.size() < 128) {
            const ssize_t n = ::recv(s, &ch, 1, 0);
            if (n < 0 && errno == EINTR) continue;
            if (n != 1) break;
            result += ch;
            if (ch == '\n') break;
        }
        return result;
    }

    bool closedWithoutReply() {
        char ch;
        ssize_t n;
        do {
            n = ::recv(s, &ch, 1, 0);
        } while (n < 0 && errno == EINTR);
        // A timeout is not a rejection; require EOF or a reset and no reply.
        return n == 0 || (n < 0 && errno == ECONNRESET);
    }

    bool noReplyYet() {
        // Give the server time to consume a partial command in a separate read.
        // recv's timeout distinguishes an open, quiet socket from EOF/reset.
        timeval timeout{0, 100000};
        if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) return false;
        char ch;
        ssize_t n;
        do {
            n = ::recv(s, &ch, 1, 0);
        } while (n < 0 && errno == EINTR);
        const bool quiet = n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
        timeout = timeval{2, 0};
        return setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 && quiet;
    }
};

void testServer() {
    testDiag("NUL cannot reach request filters or change backend settings");
    const char* env = std::getenv("EMC_TEST_PORT");
    const unsigned short port = static_cast<unsigned short>((env ? std::atoi(env) : 15731) + 60);

    StatefulDispatcher dispatcher;
    FilterChain filters;
    auto counting = std::make_unique<CountingFilter>();
    CountingFilter& filter = *counting;
    filters.add(std::move(counting));
    EmcServer server(dispatcher, filters, port);
    if (!testOk(server.start(), "start NUL test server on port %u", port)) return;

    // Keep one normal client open while malformed clients are disconnected.
    Client healthy;
    if (!testOk(healthy.connectTo(port), "connect normal client")) return;
    testOk(healthy.send("SGV\n") && healthy.readLine() == "t 1.000000\n",
           "normal getter works before malformed input");

    struct Case { const char* name; const char* prefix; std::string payload; };
    const Case cases[] = {
        {"beginning", "", bytes("\0SSV 2\n")},
        {"inside command", "", bytes("SS\0V 2\n")},
        {"inside argument", "", bytes("SSV 2\0.5\n")},
        {"after valid command", "", bytes("SSV 2\0 extra\n")},
        {"fragmented argument", "SSV 2", bytes("\0.5\n")},
        {"unterminated fragmented request", "SSV 2", bytes("\0")},
        {"NUL followed by another command", "", bytes("SSV 2\0 extra\nSSV 2\n")},
    };
    for (const Case& c : cases) {
        Client malformed;
        if (!testOk(malformed.connectTo(port), "%s: connect client", c.name)) continue;
        const unsigned beforeFilters = filter.requests.load();
        const unsigned beforeCalls = dispatcher.calls.load();
        if (*c.prefix) {
            testOk(malformed.send(c.prefix) && malformed.noReplyYet(),
                   "%s: first fragment stays open without a reply", c.name);
        }
        testOk(malformed.send(c.payload), "%s: send actual NUL bytes", c.name);
        testOk(malformed.closedWithoutReply(), "%s: connection closed without reply", c.name);
        testOk(filter.requests.load() == beforeFilters,
               "%s: no malformed request reaches the filter", c.name);
        testOk(dispatcher.calls.load() == beforeCalls,
               "%s: no malformed request reaches the dispatcher", c.name);
        testOk(dispatcher.speed.load() == 1, "%s: backend setting unchanged", c.name);
        testOk(healthy.send("SGV\n") && healthy.readLine() == "t 1.000000\n",
               "%s: another client still reads the unchanged setting", c.name);
    }

    // Prove the test backend can change state when a normal command is accepted.
    testOk(healthy.send("SSV 2\n") && healthy.readLine() == "t\n",
           "normal setting command succeeds after NUL rejections");
    testOk(dispatcher.speed.load() == 2, "normal setting command really changes state");
    server.stop();
}

} // namespace

MAIN(testRequestNul) {
    testPlan(0);
    osiSockAttach();
    testServer();
    osiSockRelease();
    return testDone();
}
