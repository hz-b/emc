// SPDX-FileCopyrightText: 2026 Helmholtz-Zentrum Berlin für Materialien und Energie GmbH
// SPDX-License-Identifier: MIT

#ifndef EMC_SERVER_H
#define EMC_SERVER_H

#include <string>
#include <map>
#include <vector>
#include <atomic>
#include <memory>
#include <chrono>
#include <cstdint>
#include <utility>

// EPICS OSI
#include <osiSock.h>
#include <epicsThread.h>
#include <epicsMessageQueue.h>
#include <epicsEvent.h>

#include "connection_filter.h"

/// The socket thread accepts work; the worker finishes and cleans it up
struct ConnectionRequests {
    std::atomic<bool> open{true};
    std::atomic<size_t> outstanding{0};
};

struct Request {
    int conn_id;
    std::string line;
    // A rejected command is not executed, but it still receives an error reply in the correct order
    bool rejected = false;
    std::shared_ptr<ConnectionRequests> connection;

    Request(int id, const std::string& text, bool reject = false,
            std::shared_ptr<ConnectionRequests> state = {})
        : conn_id(id), line(text), rejected(reject), connection(std::move(state)) {
        if (connection) connection->outstanding.fetch_add(1);
    }
    // Each Request* must be deleted exactly once
    ~Request() { complete(); }
    void complete() {
        if (connection) {
            connection->outstanding.fetch_sub(1);
            connection.reset();
        }
    }
    Request(const Request&) = delete;
    Request& operator=(const Request&) = delete;
};

struct Response {
    int conn_id;
    std::string line;   // reply text without terminator; the NetLoop appends it
};

/// Request limit
//  Count all command bytes, exclude only CR/LF terminators
constexpr size_t EMC_MAX_REQUEST_BYTES = 4096;
/// Time from the first command byte to its terminator, never reset by fragments.
constexpr double EMC_REQUEST_TIMEOUT_SECONDS = 5.0;

/// Pending reply bytes, including terminators, in application send buffers.
constexpr size_t EMC_MAX_CLIENT_REPLY_BYTES = size_t{64} * 1024;
constexpr size_t EMC_MAX_TOTAL_REPLY_BYTES = size_t{4} * 1024 * 1024;

/// Established connections, per server and per source IPv4 address.
constexpr size_t EMC_MAX_CONNECTIONS = 512;
/// Clients behind a jumphost/NAT share the same per-address allowance.
constexpr size_t EMC_MAX_CONNECTIONS_PER_ADDRESS = 64;

/// Common runtime defaults; every connection has its own allowance.
constexpr double EMC_DEFAULT_REQUEST_RATE = 500.0;
constexpr size_t EMC_DEFAULT_REQUEST_BURST = 20;
constexpr size_t EMC_DEFAULT_MAX_OUTSTANDING_REQUESTS = 8;

/// The "token bucket rate limiter":
///     The server limits how quickly clients can send requests. Instead of running a timer,
///     it calculates how much quota has replenished whenever a complete, nonempty request arrives.
class RequestTokenBucket {
public:
    bool consume(double rate, size_t burst, std::chrono::steady_clock::time_point now);
    double credits() const { return credits_; }
private:
    double credits_ = 0.0;
    double rate_ = 0.0;
    size_t burst_ = 0; // zero means not initialized; configured burst is >= 1
    std::chrono::steady_clock::time_point updated_{};
};

/// Splits a client's byte stream into request lines.
///
/// A line ends at LF, CR or CRLF; a CRLF counts as one terminator, also when
/// the CR and the LF arrive in different reads. Terminators are removed.
/// Empty lines are returned as such; deciding what to do with them is up to
/// the caller (the NetLoop ignores blank lines, like the old VxWorks server).
/// This accepts both the old VxWorks clients (CR) and newer ones (LF, CRLF).
class RequestLineSplitter {
public:
    /// Validate each line before storing bytes.
    /// Return: False means an oversized request;
    bool append(const char* data, size_t n);
    /// Takes the next complete line. Returns false when no complete line is
    /// buffered; the incomplete rest stays for the next append().
    bool next(std::string& line);
    /// Bytes of the incomplete line still buffered.
    size_t pending() const { return buf_.size() - pos_; }
private:
    std::string buf_;
    size_t pos_ = 0;          // start of the next unread line in buf_
    size_t scan_ = 0;         // next byte to search: never rescan an incomplete line
    size_t assembling_ = 0;   // length of the current line, across append() calls
    bool afterCR_ = false;    // last terminator was CR: skip one LF that follows
};

enum class ReplyTerminator {
    LF,     ///< "\n"   default; what this server always sent before
    CRLF,   ///< "\r\n"
    CR,     ///< "\r"   what the old VxWorks server (vxEMC2.c) sent
    None    ///< nothing
};

/// Keyword for @p t: "lf", "crlf", "cr" or "none".
const char* replyTerminatorName(ReplyTerminator t);
/// Bytes appended for @p t ("" for None).
const char* replyTerminatorBytes(ReplyTerminator t);
/// Case-insensitive: "lf", "crlf", "cr", "none". Also "\n", "\r\n", "\r",
/// "nl", "newline", "empty" and "off". Returns false and leaves @p out
/// unchanged for anything else (including null).
bool parseReplyTerminator(const char* text, ReplyTerminator& out);

class WorkerLoop;
class NetLoop;
struct Dispatcher;

extern "C" void emcServerLimitsRegister(void);

class EmcServer {
public:
    // Every connection and command is checked by a shared filter that must stay alive and unchanged.
    EmcServer(Dispatcher& dispatcher, FilterChain& filters, unsigned short port = 5001);
    ~EmcServer();

    // Server must not be shared.
    EmcServer(const EmcServer&) = delete;
    EmcServer& operator=(const EmcServer&) = delete;

    // Lifecycle calls must be serialized by the caller, outside worker threads.
    bool start();
    // Waits for any in-flight dispatcher call; does not cancel it.
    void stop();
    bool isRunning() const { return running_; }
    unsigned short port() const { return port_; }

    /// Local address of the last successfully opened listener (initially 0.0.0.0).
    std::string bindAddress() const { return formatIpv4(bindAddress_.load()); }

    /// All servers share a configurable IPv4 address (default: all interfaces);
    /// changes take effect on restart, and invalid addresses prevent startup.
    static bool setDefaultBindAddress(const char* text);
    /// Returns "invalid" while subsequent starts are blocked.
    static std::string defaultBindAddress();

    /// Each reply ends with a configurable character (default: newline) and can be changed online
    void setReplyTerminator(ReplyTerminator t) { terminator_.store(t); }
    ReplyTerminator replyTerminator() const { return terminator_.load(); }

    /// Closes inactive connections after a configurable timeout (default: disabled),
    /// changes apply immediately
    bool setIdleTimeout(double seconds);
    double idleTimeout() const {
        const double local = idleTimeoutSeconds_.load();
        return local < 0.0 ? defaultIdleTimeout() : local;
    }
    /// Remove a per-server override and follow the common setting again.
    void useDefaultIdleTimeout() { idleTimeoutSeconds_.store(-1.0); }

    /// Sets the default idle timeout
    static bool setDefaultIdleTimeout(double seconds);
    static double defaultIdleTimeout() { return defaultIdleTimeoutSeconds_.load(); }

    /// Shared request limits apply to all servers; zero disables checks
    static bool setDefaultRequestRate(double rate);
    static double defaultRequestRate() { return defaultRequestRate_.load(); }
    static bool setDefaultRequestBurst(size_t burst);
    static size_t defaultRequestBurst() { return defaultRequestBurst_.load(); }
    static bool setDefaultMaxOutstandingRequests(size_t count);
    static size_t defaultMaxOutstandingRequests() { return defaultMaxOutstandingRequests_.load(); }

private:
    friend class NetLoop;
    friend class WorkerLoop;

    unsigned short port_;
    Dispatcher& dispatcher_;
    FilterChain* filters_;                      // never null
    SOCKET listenfd_ = INVALID_SOCKET;
    std::atomic<epicsUInt32> bindAddress_{0}; // last listener address, host byte order
    // One atomic snapshot includes validity: -1 is invalid, otherwise host-order IPv4.
    static std::atomic<std::int64_t> defaultBindAddress_;
    std::atomic<bool> running_{false};
    std::atomic<ReplyTerminator> terminator_{ReplyTerminator::LF};
    std::atomic<double> idleTimeoutSeconds_{-1.0}; // negative: inherit common default
    static std::atomic<double> defaultIdleTimeoutSeconds_;
    static std::atomic<double> defaultRequestRate_;
    static std::atomic<size_t> defaultRequestBurst_;
    static std::atomic<size_t> defaultMaxOutstandingRequests_;

    // Cross-thread queues old style
    epicsMessageQueue* qReq_;   // NetLoop    -> WorkerLoop (Request*)
    epicsMessageQueue* qResp_;  // WorkerLoop -> NetLoop    (Response*)

    // Threads
    epicsThreadId netLoopTid_ = nullptr;
    epicsThreadId workerLoopTid_ = nullptr;

    // Shutdown
    epicsEvent netLoopExited_;
    epicsEvent workerLoopExited_;

    // Loops
    std::unique_ptr<NetLoop> netLoop_;
    std::unique_ptr<WorkerLoop> workerLoop_;

    bool openListenSocket();
    void closeListenSocket();
    void discardPendingMessages();

    static void netLoopEntry(void* arg);
    static void workerLoopEntry(void* arg);
};

/// Network thread: accepts clients, reads and writes all client sockets,
/// applies the limits and filters, queues requests and sends the replies.
/// TODO: select() wakes up every 5 ms at epicsThreadPriorityHigh, even when
/// idle, and a queued reply can wait up to 5 ms. Make it event based: block until
/// a socket is ready, a reply is queued (self-pipe or eventfd) or a timeout is due.
class NetLoop {
public:
    NetLoop(EmcServer* srv, SOCKET listenfd, epicsMessageQueue* qReq,
            epicsMessageQueue* qResp, FilterChain& filters);
    void run();

private:
    friend struct NetLoopTestAccess; // deterministic resource-limit tests
    struct Conn {
        SOCKET s = INVALID_SOCKET;
        int    id = -1;
        PeerInfo peer;      // identity passed to the filter chain
        RequestLineSplitter in; // request line assembly (LF, CR, CRLF)
        std::chrono::steady_clock::time_point lineStarted{};
        std::chrono::steady_clock::time_point lastActivity{};
        std::string outbuf; // pending bytes to send
        bool   open = true;
        RequestTokenBucket rate;
        std::shared_ptr<ConnectionRequests> requests = std::make_shared<ConnectionRequests>();
    };

    EmcServer* srv_;
    SOCKET listenfd_;
    epicsMessageQueue* qReq_;
    epicsMessageQueue* qResp_;
    FilterChain& filters_;
    std::atomic<int> nextId_{1};
    std::map<int, Conn> conns_;          // by id
    std::map<SOCKET, int> fd2id_;        // reverse lookup
    size_t pendingReplyBytes_ = 0;      // sum of outbuf sizes; NetLoop thread only

    void acceptOne();
    size_t connectionsFrom(epicsUInt32 addr) const;
    void dropConn(int id);
    void handleReadable(SOCKET fd);
    // Checks both quotas before filters/queueing; false closes and invalidates c.
    bool admitRequest(Conn& c, std::chrono::steady_clock::time_point now);
    // Drops only this client if its incomplete line has exceeded the deadline.
    bool dropExpiredRequest(const Conn& c, std::chrono::steady_clock::time_point now);
    bool dropIdleConnection(const Conn& c, std::chrono::steady_clock::time_point now,
                            double timeoutSeconds);
    void pumpResponses(); // send queued backend responses to clients
};

/// Worker thread: runs the commands of all clients (up to EMC_MAX_CONNECTIONS)
/// one at a time, so one slow command delays the replies to every client.
/// TODO: One worker is enough for today's 1-3 clients. If that grows, adapt it,
/// e.g. one worker per connection. Replies per client must stay in order, and
/// dispatchers (e.g. MonoSimDispatcher) expect serialized calls.
class WorkerLoop {
public:
    // The caller owns dispatcher and must keep it alive until run() returns.
    WorkerLoop(epicsMessageQueue* qReq,
               epicsMessageQueue* qResp,
               std::atomic<bool>* running,
               Dispatcher& dispatcher);
    void run();

private:
    epicsMessageQueue* qReq_;
    epicsMessageQueue* qResp_;
    std::atomic<bool>* running_;
    Dispatcher& dispatcher_;

    // Forward a command to the supplied handler; no backend construction here.
    std::string processLine(const std::string& line);
};
#endif /* EMC_SERVER_H */
