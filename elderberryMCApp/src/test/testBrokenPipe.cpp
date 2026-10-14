/**
 * @file testBrokenPipe.cpp
 * @brief exercise the real NetLoop send path with SIGPIPE default.
 *
 * Each scenario runs in a fresh child, before it creates any worker threads.
 * The parent survives a regression and reports it through EPICS unit tests.
 * A socketpair peer with its receive side shut down deterministically causes
 * EPIPE without making the server socket readable as EOF first. This avoids
 * TCP disconnect timing and does not mock send() or hard-code its flags.
 * The existing NetLoopTestAccess friend supplies the test connections.
 * Run with make runtests or directly; the auxiliary server uses port 0.
 */

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <epicsUnitTest.h>
#include <testMain.h>

#include "dispatcher.h"
#include "emc_server.h"

struct NetLoopTestAccess {
    static std::shared_ptr<ConnectionRequests> add(NetLoop& loop, int id,
                                                   int fd, const char* reply) {
        NetLoop::Conn c;
        c.id = id;
        c.s = fd;
        c.peer.conn_id = id;
        c.peer.addr = INADDR_LOOPBACK;
        c.lastActivity = std::chrono::steady_clock::now();
        c.outbuf = reply;
        auto state = c.requests;
        loop.pendingReplyBytes_ += c.outbuf.size();
        loop.conns_.emplace(id, std::move(c));
        loop.fd2id_.emplace(fd, id);
        return state;
    }
    static bool clean(const NetLoop& loop) {
        return loop.conns_.empty() && loop.fd2id_.empty()
            && loop.pendingReplyBytes_ == 0;
    }
};

namespace {

constexpr unsigned setupFailure = 128;
constexpr unsigned allChecks = 63;

struct DummyDispatcher : Dispatcher {
    std::string processLine(const char*) override { return "unused"; }
};

struct CloseObserver : ConnectionFilter {
    std::atomic<unsigned> brokenClosed{0};
    const char* name() const override { return "broken-pipe-test"; }
    void onClose(const PeerInfo& peer) override {
        if (peer.conn_id == 1) brokenClosed.fetch_add(1);
    }
};

bool defaultSignals() {
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGPIPE, &action, nullptr) != 0
        || sigaction(SIGALRM, &action, nullptr) != 0) return false;
    sigset_t unblock;
    sigemptyset(&unblock);
    sigaddset(&unblock, SIGPIPE);
    sigaddset(&unblock, SIGALRM);
    if (sigprocmask(SIG_UNBLOCK, &unblock, nullptr) != 0) return false;
    alarm(10); // Includes startup, socket operations, stop() and join().
    return true;
}

bool makePair(int (&pair)[2]) {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) return false;
    if (pair[0] >= FD_SETSIZE || pair[1] >= FD_SETSIZE) return false;
    const int flags = fcntl(pair[0], F_GETFL, 0);
    if (flags < 0 || fcntl(pair[0], F_SETFL, flags | O_NONBLOCK) != 0) return false;
    timeval timeout{2, 0};
    return setsockopt(pair[1], SOL_SOCKET, SO_RCVTIMEO,
                      &timeout, sizeof(timeout)) == 0;
}

unsigned serverScenario() {
    osiSockAttach();
    DummyDispatcher dispatcher;
    FilterChain auxiliaryFilters;
    EmcServer server(dispatcher, auxiliaryFilters, 0);
    server.setIdleTimeout(0);
    // Supplies the normal running lifecycle. Its own loop has no clients;
    // the tested loop below uses separate queues and controlled socket pairs.
    if (!server.start()) return setupFailure;

    // Establish the disposition after EPICS startup too, so host protection
    // cannot accidentally mask a missing MSG_NOSIGNAL in the production call.
    if (!defaultSignals()) return setupFailure;
    FilterChain filters;
    auto observer = new CloseObserver;
    filters.add(std::unique_ptr<ConnectionFilter>(observer));
    epicsMessageQueue requests(8, sizeof(void*)), responses(8, sizeof(void*));
    NetLoop loop(&server, INVALID_SOCKET, &requests, &responses, filters);

    int broken[2], healthy[2];
    if (!makePair(broken) || !makePair(healthy)
        || shutdown(broken[1], SHUT_RD) != 0) return setupFailure;
    PeerInfo peer;
    peer.addr = INADDR_LOOPBACK;
    peer.conn_id = 1;
    if (filters.onAccept(peer) != FilterVerdict::Allow) return setupFailure;
    const auto brokenState = NetLoopTestAccess::add(loop, 1, broken[0], "reply\n");
    peer.conn_id = 2;
    if (filters.onAccept(peer) != FilterVerdict::Allow) return setupFailure;
    NetLoopTestAccess::add(loop, 2, healthy[0], "");
    std::thread worker([&loop] { loop.run(); });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (observer->brokenClosed.load() == 0
           && std::chrono::steady_clock::now() < deadline) {
        epicsThreadSleep(0.001);
    }
    unsigned passed = 0;
    if (observer->brokenClosed.load() == 1) passed |= 1;
    if (!brokenState->open.load()) passed |= 2;

    // Submit a new reply only after the broken connection has been dropped.
    // This proves the loop continues serving another connection after EPIPE.
    auto response = new Response{2, "OK"};
    void* ptr = response;
    const bool queued = responses.trySend(&ptr, sizeof(ptr)) == 0;
    if (!queued) delete response;
    char reply[3]{};
    size_t received = 0;
    while (queued && received < sizeof(reply)) {
        const ssize_t n = recv(healthy[1], reply + received, sizeof(reply) - received, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        received += static_cast<size_t>(n);
    }
    if (received == sizeof(reply) && std::memcmp(reply, "OK\n", sizeof(reply)) == 0)
        passed |= 4;
    // Once the new reply arrives, dropConn() has finished on the worker thread.
    if ((passed & 4) && fcntl(broken[0], F_GETFD) == -1 && errno == EBADF)
        passed |= 8;

    server.stop();
    worker.join();
    if (NetLoopTestAccess::clean(loop)) passed |= 16;
    if (observer->brokenClosed.load() == 1) passed |= 32;
    close(broken[1]);
    close(healthy[1]);
    // Drain a reply left queued after an earlier failure.
    while (responses.tryReceive(&ptr, sizeof(ptr)) > 0)
        delete static_cast<Response*>(ptr);
    osiSockRelease();
    return passed;
}

bool runChild(bool control, int& status) {
    const pid_t child = fork();
    if (child < 0) return false;
    if (child == 0) {
        if (!defaultSignals()) _exit(setupFailure);
        if (control) {
            int pair[2];
            if (!makePair(pair) || shutdown(pair[1], SHUT_RD) != 0)
                _exit(setupFailure);
            // Negative control: prove this setup generates SIGPIPE with flags 0.
            send(pair[0], "x", 1, 0);
            _exit(setupFailure); // Reaching this means SIGPIPE did not terminate us.
        }
        _exit(serverScenario());
    }
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    return waited == child;
}

} // namespace

MAIN(testBrokenPipe) {
    testPlan(8);
    int status = 0;
    bool waited = runChild(true, status);
    testOk(waited && WIFSIGNALED(status) && WTERMSIG(status) == SIGPIPE,
           "control: unprotected broken-peer send terminates child with SIGPIPE");
    waited = runChild(false, status);
    const bool survived = waited && WIFEXITED(status)
        && static_cast<unsigned>(WEXITSTATUS(status)) <= allChecks;
    testOk(survived, "production send survives with SIGPIPE default and unblocked");
    if (!survived) {
        if (waited && WIFSIGNALED(status))
            testDiag("server child terminated by signal %d", WTERMSIG(status));
        else if (waited && WIFEXITED(status))
            testDiag("server child setup failed (exit %d)", WEXITSTATUS(status));
        else testDiag("fork/waitpid failed: errno=%d", errno);
        testSkip(6, "server child did not complete");
    } else {
        const unsigned checks = static_cast<unsigned>(WEXITSTATUS(status));
        testOk(checks & 1, "failed send invokes the connection close hook");
        testOk(checks & 2, "failed connection is marked closed for queued requests");
        testOk(checks & 4, "another connection receives a new reply after the failure");
        testOk(checks & 8, "failed connection socket is closed");
        testOk(checks & 16, "shutdown leaves no connections, descriptors or pending reply bytes");
        testOk(checks & 32, "failed connection close hook is called exactly once");
    }
    return testDone();
}
