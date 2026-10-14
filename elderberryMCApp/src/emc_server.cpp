// SPDX-FileCopyrightText: 2026 Helmholtz-Zentrum Berlin für Materialien und Energie GmbH
// SPDX-License-Identifier: MIT

/**
 * @file emc_server.cpp
 * @brief EMC Protocol Server -  Linux (TODO: Fully EPICS/OSI) TCP front-end to a supplied dispatcher.
 *
 * - **Decoupled I/O and Logic**: Socket I/O runs on its own thread (NetLoop)
 *   and uses `epicsMessageQueue` to hand off work to the WorkerLoop, which
 *   calls the dispatcher.
 * - **Non-blocking OSI sockets**: All networking uses EPICS OSI (`osiSock.h`)
 *   for the goal of portability across Linux and VxWorks (there are still some POSIX glitches).
 * - **Back-pressure aware**: If response queues are full, messages are dropped
 *   safely with a warning instead of blocking critical threads.
 * - **Admission filters**: an injected FilterChain (e.g. the ACL) decides on
 *   every new connection and every request line inside the NetLoop, before
 *   anything reaches a queue or the dispatcher. See connection_filter.h.
 * - **Reply terminator**: dispatchers return bare reply text; the NetLoop
 *   appends the server's terminator (LF by default; CRLF, CR or nothing via
 *   EmcServer::setReplyTerminator()).
 * - **Request terminators**: a request line ends at LF, CR or CRLF (one
 *   terminator, also when split across reads), so clients of the old
 *   VxWorks server (CR) should work unchanged. Blank lines get no reply.
 * - **Request bytes**: NUL is forbidden. Close the client before buffering
 *   a read containing NUL, so filters and C-string dispatchers agree.
 *
 * ## Threads
 * - NetLoop: accepts clients, reads and writes all client sockets,
 *   applies the filters and sends queued replies (one select() loop)
 * - WorkerLoop: takes queued requests and calls the dispatcher, one command
 *   at a time for all clients
 *
 * ## Typical Flow
 *  Client -> NetLoop -> FilterChain -> WorkerLoop -> Dispatcher -> WorkerLoop -> NetLoop -> Client
 *
 * ## Manual testing
 *   $ nc localhost 5001
 *   $ telnet localhost 5001
 *
 * ## Production environment testing
 *   HZB kiwi-scan plugin (https://github.com/hz-b/kiwi-scan)
 *
 */

#include "emc_server.h"
#include "dispatcher.h"
#include "access_control.h"

#include <cctype>
#include <cmath>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <algorithm>
#include <fcntl.h>
#include <iterator>
#include <limits>

#include <epicsTypes.h>
#include <epicsTime.h>
#include <epicsMutex.h>
#include <epicsExport.h>
#include <iocsh.h>

#define Debug(L, FMT, ARGS...) { if(L <= emc_server_debug) \
    fprintf(stderr, FMT "\t- file : " __FILE__ ", line %d\n", ## ARGS, __LINE__); }

int emc_server_debug= 0;
epicsExportAddress(int, emc_server_debug);

std::atomic<std::int64_t> EmcServer::defaultBindAddress_{0};
std::atomic<double> EmcServer::defaultIdleTimeoutSeconds_{0.0};
std::atomic<double> EmcServer::defaultRequestRate_{EMC_DEFAULT_REQUEST_RATE};
std::atomic<size_t> EmcServer::defaultRequestBurst_{EMC_DEFAULT_REQUEST_BURST};
std::atomic<size_t> EmcServer::defaultMaxOutstandingRequests_{EMC_DEFAULT_MAX_OUTSTANDING_REQUESTS};

namespace {
void bindAddressCall(const iocshArgBuf* args) {
    const char* text = args ? args[0].sval : nullptr;
    if (text && *text && !EmcServer::setDefaultBindAddress(text)) return;
    const std::string address = EmcServer::defaultBindAddress();
    std::printf("emc: common bind address %s%s; applied on each server's next start\n",
                address.c_str(), address == "0.0.0.0" ? " (all IPv4 interfaces)" :
                address == "invalid" ? " (startup blocked until corrected)" : "");
}
const iocshArg bindArg = {"local IPv4 (0.0.0.0: all, empty: show)", iocshArgString};
const iocshArg* const bindArgs[] = {&bindArg};
const iocshFuncDef bindDef = {"emcBindAddress", 1, bindArgs};

void idleTimeoutCall(const iocshArgBuf* args) {
    // Strings distinguish an omitted value (show) from an explicit zero.
    const char* text = args ? args[0].sval : nullptr;
    if (text && *text) {
        char* end = nullptr;
        errno = 0;
        const double seconds = std::strtod(text, &end);
        if (end == text || *end || errno == ERANGE || !std::isfinite(seconds) || seconds < 0.0) {
            Debug(0, "[Server] idle timeout '%s' rejected: use finite seconds >= 0 (unchanged: %g)", text, EmcServer::defaultIdleTimeout());
            return;
        }
        if (!EmcServer::setDefaultIdleTimeout(seconds)) return;
    }
    const double seconds = EmcServer::defaultIdleTimeout();
    std::printf("emc: common idle timeout %g seconds%s\n", seconds,
                seconds == 0.0 ? " (disabled)" : "");
}
const iocshArg idleArg = {"seconds (0: disabled, empty: show)", iocshArgString};
const iocshArg* const idleArgs[] = {&idleArg};
const iocshFuncDef idleDef = {"emcIdleTimeout", 1, idleArgs};

void requestRateCall(const iocshArgBuf* args) {
    const char* text = args ? args[0].sval : nullptr;
    if (text && *text) {
        char* end = nullptr;
        errno = 0;
        const double rate = std::strtod(text, &end);
        if (end == text || *end || errno == ERANGE || !std::isfinite(rate) || rate < 0.0) {
            Debug(0, "[Server] request rate '%s' rejected: use finite commands/second >= 0 (unchanged: %g)", text, EmcServer::defaultRequestRate());
            return;
        }
        if (!EmcServer::setDefaultRequestRate(rate)) return;
    }
    const double rate = EmcServer::defaultRequestRate();
    std::printf("emc: common request rate %g commands/second per connection%s\n", rate, rate == 0.0 ? " (disabled)" : "");
}

// Unsigned integer parsing also rejects signs, fractions and trailing junk.
bool parseRequestCount(const char* text, size_t& out) {
    size_t count = 0;
    for (const char* p = text; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        const size_t digit = static_cast<size_t>(*p - '0');
        if (count > (std::numeric_limits<size_t>::max() - digit) / 10) return false;
        count = count * 10 + digit;
    }
    out = count;
    return true;
}

void requestCountCall(const iocshArgBuf* args, bool burst) {
    const char* text = args ? args[0].sval : nullptr;
    if (text && *text) {
        size_t count = 0;
        if (!parseRequestCount(text, count) || (burst && count == 0)) {
            Debug(0, "[Server] %s '%s' rejected: use integer >= %d (unchanged)", burst ? "request burst" : "outstanding request limit", text, burst ? 1 : 0);
            return;
        }
        if (burst) EmcServer::setDefaultRequestBurst(count);
        else EmcServer::setDefaultMaxOutstandingRequests(count);
    }
    if (burst) std::printf("emc: common request burst %zu commands per connection\n", EmcServer::defaultRequestBurst());
    else {
        const size_t count = EmcServer::defaultMaxOutstandingRequests();
        std::printf("emc: common outstanding request limit %zu per connection%s\n", count, count == 0 ? " (disabled)" : "");
    }
}
void requestBurstCall(const iocshArgBuf* args) { requestCountCall(args, true); }
void outstandingCall(const iocshArgBuf* args) { requestCountCall(args, false); }

const iocshArg rateArg = {"commands/second (0: disabled, empty: show)", iocshArgString};
const iocshArg* const rateArgs[] = {&rateArg};
const iocshFuncDef rateDef = {"emcRequestRate", 1, rateArgs};
const iocshArg burstArg = {"burst commands (>= 1, empty: show)", iocshArgString};
const iocshArg* const burstArgs[] = {&burstArg};
const iocshFuncDef burstDef = {"emcRequestBurst", 1, burstArgs};
const iocshArg outstandingArg = {"outstanding commands (0: disabled, empty: show)", iocshArgString};
const iocshArg* const outstandingArgs[] = {&outstandingArg};
const iocshFuncDef outstandingDef = {"emcMaxOutstandingRequests", 1, outstandingArgs};
} // namespace

extern "C" void emcServerLimitsRegister(void) {
    // Like the other registrars, called serially during IOC shell setup.
    static bool registered = false;
    if (registered) return;
    iocshRegister(&bindDef, bindAddressCall);
    iocshRegister(&idleDef, idleTimeoutCall);
    iocshRegister(&rateDef, requestRateCall);
    iocshRegister(&burstDef, requestBurstCall);
    iocshRegister(&outstandingDef, outstandingCall);
    registered = true;
}
epicsExportRegistrar(emcServerLimitsRegister);

// Request admission
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool RequestTokenBucket::consume(double rate, size_t burst,
                                 std::chrono::steady_clock::time_point now) {
    if (burst_ == 0 || (rate_ == 0.0 && rate > 0.0)) {
        credits_ = static_cast<double>(burst); // new connection or re-enable
    } else {
        const double elapsed = std::max(0.0, std::chrono::duration<double>(now - updated_).count());
        credits_ = std::min(static_cast<double>(burst_), credits_ + elapsed * rate_);
        credits_ = std::min(static_cast<double>(burst), credits_); // clamp a reduced burst
    }
    updated_ = now;
    rate_ = rate;
    burst_ = burst;
    if (rate == 0.0) return true;
    if (credits_ < 1.0) return false;
    credits_ -= 1.0;
    return true;
}

bool EmcServer::setDefaultRequestRate(double rate) {
    if (!std::isfinite(rate) || rate < 0.0) {
        Debug(0, "[Server] request rate rejected: rate=%g; use finite commands/second >= 0 (unchanged)", rate);
        return false;
    }
    defaultRequestRate_.store(rate);
    Debug(1, "[Server] common request rate=%g commands/second per connection%s", rate, rate == 0.0 ? " (disabled)" : "");
    return true;
}

bool EmcServer::setDefaultRequestBurst(size_t burst) {
    if (burst == 0) {
        Debug(0, "[Server] request burst rejected: use integer >= 1 (unchanged)");
        return false;
    }
    defaultRequestBurst_.store(burst);
    Debug(1, "[Server] common request burst=%zu commands per connection", burst);
    return true;
}

bool EmcServer::setDefaultMaxOutstandingRequests(size_t count) {
    defaultMaxOutstandingRequests_.store(count);
    Debug(1, "[Server] common outstanding request limit=%zu per connection%s", count, count == 0 ? " (disabled)" : "");
    return true;
}

const char* replyTerminatorName(ReplyTerminator t) {
    switch (t) {
    case ReplyTerminator::LF:   return "lf";
    case ReplyTerminator::CRLF: return "crlf";
    case ReplyTerminator::CR:   return "cr";
    case ReplyTerminator::None: return "none";
    }
    return "lf";
}

const char* replyTerminatorBytes(ReplyTerminator t) {
    switch (t) {
    case ReplyTerminator::LF:   return "\n";
    case ReplyTerminator::CRLF: return "\r\n";
    case ReplyTerminator::CR:   return "\r";
    case ReplyTerminator::None: return "";
    }
    return "\n";
}

bool parseReplyTerminator(const char* text, ReplyTerminator& out) {
    if (!text) return false;
    std::string key;
    for (const char* p = text; *p; ++p) {
        key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*p))));
    }
    // Keywords or the control characters themselves
    if (key == "lf" || key == "nl" || key == "newline" || key == "\\n" || key == "\n") {
        out = ReplyTerminator::LF;   return true;
    }
    if (key == "crlf" || key == "\\r\\n" || key == "\r\n") {
        out = ReplyTerminator::CRLF; return true;
    }
    if (key == "cr" || key == "\\r" || key == "\r") {
        out = ReplyTerminator::CR;   return true;
    }
    if (key == "none" || key == "empty" || key == "off") {
        out = ReplyTerminator::None; return true;
    }
    return false;
}

bool RequestLineSplitter::append(const char* data, size_t n) {
    // Check the incoming bytes before allocating or copying them.
    // The limit is per line, not per TCP read.
    // One read can contain several valid commands.
    size_t length = assembling_;
    for (size_t i = 0; i < n; ++i) {
        if (data[i] == '\r' || data[i] == '\n') {
            length = 0;
        } else {
            if (length == EMC_MAX_REQUEST_BYTES) return false;
            ++length;
        }
    }
    buf_.append(data, n);
    assembling_ = length;
    return true;
}

bool RequestLineSplitter::next(std::string& line) {
    // CR/LF handling:
    //   Supported line terminators:
    //   '\r'    CR alone ends a command.
    //   '\n'    LF alone ends a command.
    //   "\r\n"  CR followed by LF ends one command, not two.
    if (afterCR_ && pos_ < buf_.size()) {
        afterCR_ = false;
        if (buf_[pos_] == '\n') ++pos_;
    }
    scan_ = std::max(scan_, pos_);
    const size_t end = buf_.find_first_of("\r\n", scan_);
    if (end == std::string::npos) {
        buf_.erase(0, pos_);   // keep only the incomplete line
        pos_ = 0;
        scan_ = buf_.size();  // these bytes were searched already
        return false;
    }
    line.assign(buf_, pos_, end - pos_);
    afterCR_ = (buf_[end] == '\r');
    pos_ = end + 1;
    scan_ = pos_;
    return true;
}

namespace {
// Prints all received bytes, including hidden characters
std::string debugProtocolBytes(const char* data, size_t size) {
    static const char hex[] = "0123456789ABCDEF";
    std::string text;
    for (size_t i = 0; i < size; ++i) {
        const unsigned char ch = static_cast<unsigned char>(data[i]);
        switch (ch) {
        case '\r': text += "\\r"; break;
        case '\n': text += "\\n"; break;
        case '\t': text += "\\t"; break;
        case '\\': text += "\\\\"; break;
        case '"': text += "\\\""; break;
        default:
            if (ch >= 0x20 && ch <= 0x7e) {
                text += static_cast<char>(ch);
            } else {
                text += "\\x";
                text += hex[ch >> 4];
                text += hex[ch & 0x0f];
            }
        }
    }
    return text;
}

// Empty or only blanks: the old VxWorks server ignored such a request and sent no reply.
bool isBlankRequest(const std::string& line) {
    return line.find_first_not_of(" \t") == std::string::npos;
}
}

/**
 * @brief Construct an EMC server bound to a TCP port.
 * @param dispatcher Command handler owned by the caller.
 * @param filters Admission filters (e.g. the ACL), owned by the caller.
 * @param port TCP port to listen on (e.g., 5001).
 *
 * Internally we allocate two EPICS message queues:
 *  - qReq_:  carries pointers to Request objects produced by socket readers.
 *  - qResp_: carries pointers to Response objects produced by the backend.
 * TODO: The I/O thread poll should be event based 
 *
 */
EmcServer::EmcServer(
        Dispatcher& dispatcher,
        FilterChain& filters,
        unsigned short port)
: port_(port), dispatcher_(dispatcher), filters_(&filters)
{
    // Pointers fit in message queues; we choose generous queue depths
    qReq_  = new epicsMessageQueue(1024, sizeof(void*)); // Request*
    qResp_ = new epicsMessageQueue(1024, sizeof(void*)); // Response*
}

EmcServer::~EmcServer() {
    stop();
    delete qReq_;
    delete qResp_;
}

bool EmcServer::setDefaultBindAddress(const char* text) {
    Ipv4Network parsed;
    std::string error;
    if (!Ipv4Network::parse(text, parsed, error)) {
        // Fail closed, including a typo before the first start (default: all).
        defaultBindAddress_.store(-1);
        Debug(0, "[Server] bind address '%s' rejected: %s; subsequent starts blocked until corrected",
              text ? text : "(null)", error.c_str());
        return false;
    }
    if (std::strchr(text, '/') != nullptr || parsed.addr == 0xffffffffu ||
        (parsed.addr & 0xf0000000u) == 0xe0000000u) {
        // INADDR_BROADCAST can act like INADDR_ANY on Linux: reject it explicitly.
        defaultBindAddress_.store(-1);
        Debug(0, "[Server] bind address '%s' rejected: use numeric IPv4 without CIDR, broadcast or multicast; subsequent starts blocked until corrected", text);
        return false;
    }
    defaultBindAddress_.store(static_cast<std::int64_t>(parsed.addr));
    Debug(1, "[Server] common bind address=%s; applies on next start; running listeners unchanged", text);
    return true;
}

std::string EmcServer::defaultBindAddress() {
    const std::int64_t address = defaultBindAddress_.load();
    return address < 0 ? "invalid" : formatIpv4(static_cast<epicsUInt32>(address));
}

bool EmcServer::setIdleTimeout(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) {
        Debug(0, "[Server] idle timeout rejected: port=%u seconds=%g; use a finite value >= 0 (unchanged)", port_, seconds);
        return false;
    }
    idleTimeoutSeconds_.store(seconds);
    Debug(1, "[Server] idle timeout: port=%u seconds=%g%s", port_, seconds, seconds == 0.0 ? " (disabled)" : "");
    return true;
}

bool EmcServer::setDefaultIdleTimeout(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) {
        Debug(0, "[Server] common idle timeout rejected: seconds=%g; use a finite value >= 0 (unchanged)", seconds);
        return false;
    }
    defaultIdleTimeoutSeconds_.store(seconds);
    Debug(1, "[Server] common idle timeout: seconds=%g%s; applies to all servers without an override", seconds, seconds == 0.0 ? " (disabled)" : "");
    return true;
}

bool EmcServer::openListenSocket() {
    // Snapshot the shared setting for this start; later changes require restart.
    const std::int64_t configured = defaultBindAddress_.load();
    if (configured < 0) {
        Debug(0, "[Server] startup refused: port=%u invalid common bind address; correct emcBindAddress", port_);
        return false;
    }
    const epicsUInt32 localAddress = static_cast<epicsUInt32>(configured);
    const std::string address = formatIpv4(localAddress);
    listenfd_ = epicsSocketCreate(AF_INET, SOCK_STREAM, 0);
    if (listenfd_ == INVALID_SOCKET) {
        Debug(0, "[Server] socket() failed: port=%u errno=%d (%s)", port_, errno, std::strerror(errno));
        return false;
    }
    // select() descriptor sets only support numbers below FD_SETSIZE.
    // select() is used instead of poll because it seems to be sufficient and for platform compatibility.
    // At the same time it avoids managing varaiable array with poll()
    if (listenfd_ < 0 || listenfd_ >= FD_SETSIZE) {
        Debug(0, "[Server] listener descriptor out of range: fd=%d FD_SETSIZE=%d", (int)listenfd_, FD_SETSIZE);
        epicsSocketDestroy(listenfd_);
        listenfd_ = INVALID_SOCKET;
        return false;
    }
    // Reuse addr
    int on = 1;
    setsockopt(listenfd_, SOL_SOCKET, SO_REUSEADDR, (char*)&on, sizeof(on));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(localAddress);
    addr.sin_port = htons(port_);
    if (bind(listenfd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        Debug(0, "[Server] bind() failed: address=%s port=%u errno=%d (%s); no fallback to all interfaces", address.c_str(), port_, errno, std::strerror(errno));
        epicsSocketDestroy(listenfd_);
        listenfd_ = INVALID_SOCKET;
        return false;
    }
    if (listen(listenfd_, 16) < 0) {
        Debug(0, "[Server] listen() failed: address=%s port=%u backlog=%d errno=%d (%s)", address.c_str(), port_, 16, errno, std::strerror(errno));
        epicsSocketDestroy(listenfd_);
        listenfd_ = INVALID_SOCKET;
        return false;
    }
    bindAddress_.store(localAddress);
    return true;
}

void EmcServer::closeListenSocket() {
    if (listenfd_ != INVALID_SOCKET) {
        epicsSocketDestroy(listenfd_);
        listenfd_ = INVALID_SOCKET;
    }
}

/**
 * @brief Start the EMC server: sockets, threads, and queues.
 * @return true on success, false on any initialization failure.
 */
bool EmcServer::start() {
    if (running_) return true;
    if (!openListenSocket()) return false;

    try {
        netLoop_ = std::make_unique<NetLoop>(this, listenfd_, qReq_, qResp_, *filters_);
        workerLoop_ = std::make_unique<WorkerLoop>(qReq_, qResp_, &running_, dispatcher_);
    } catch (const std::exception& e) {
        Debug(0, "[Server] failed to create workers: port=%u error='%s'", port_, e.what());
        netLoop_.reset();
        workerLoop_.reset();
        closeListenSocket();
        return false;
    }

    running_ = true;

    netLoopTid_ = epicsThreadCreate(
        "EMC-NetLoop",
        epicsThreadPriorityHigh,
        epicsThreadGetStackSize(epicsThreadStackMedium),
        &EmcServer::netLoopEntry,
        this);

    workerLoopTid_ = epicsThreadCreate(
        "EMC-WorkerLoop",
        epicsThreadPriorityMedium,
        epicsThreadGetStackSize(epicsThreadStackMedium),
        &EmcServer::workerLoopEntry,
        this);

    if (!netLoopTid_ || !workerLoopTid_) {
        Debug(0, "[Server] failed to start threads: netLoop=%s workerLoop=%s port=%u", netLoopTid_ ? "started" : "failed", workerLoopTid_ ? "started" : "failed", port_);
        stop();
        return false;
    }
    Debug(0, "[Server] listening on %s:%u%s", bindAddress().c_str(), port_,
          bindAddress_.load() == 0 ? " (all IPv4 interfaces)" : "");
    Debug(1, "[Server] request limits: maxBytes=%zu (excluding CR/LF) lineTimeout=%.1fs", EMC_MAX_REQUEST_BYTES, EMC_REQUEST_TIMEOUT_SECONDS);
    Debug(1, "[Server] reply limits: clientBytes=%zu totalBytes=%zu (including terminators)", EMC_MAX_CLIENT_REPLY_BYTES, EMC_MAX_TOTAL_REPLY_BYTES);
    Debug(1, "[Server] connection limits: port=%u total=%zu perAddress=%zu idleTimeout=%gs (0 disables expiry)", port_, EMC_MAX_CONNECTIONS, EMC_MAX_CONNECTIONS_PER_ADDRESS, idleTimeout());
    Debug(1, "[Server] admission limits: port=%u rate=%g/s burst=%zu outstanding=%zu (rate/outstanding 0 disables)", port_, defaultRequestRate(), defaultRequestBurst(), defaultMaxOutstandingRequests());
    return true;
}

/**
 * @brief Stop the EMC server and wait until both workers release their objects.
 *
 */
void EmcServer::stop() {
    if (!running_ && !netLoopTid_ && !workerLoopTid_) return;

    running_ = false;

    // Nudge queues so blocking gets released
    void* nil = nullptr;
    qReq_->trySend(&nil, sizeof(void*));
    qResp_->trySend(&nil, sizeof(void*));

    // Wait for workers
    if (netLoopTid_) {
        netLoopExited_.wait();
        netLoopTid_ = nullptr;
    }
    if (workerLoopTid_) {
        if (!workerLoopExited_.wait(1.0)) {
            Debug(0, "[Server] waiting for in-flight dispatcher call: port=%u timeout=%.1fs", port_, 1.0);
            workerLoopExited_.wait();
        }
        workerLoopTid_ = nullptr;
    }

    closeListenSocket();
    netLoop_.reset();
    workerLoop_.reset();
    discardPendingMessages();
    Debug(1, "[Server] Workers stopped; pending requests and replies discarded");
}

void EmcServer::discardPendingMessages() {
    // Queues store pointers; clear them to free memory and avoid old messages after restart.
    void* ptr = nullptr;
    while (qReq_->tryReceive(&ptr, sizeof(ptr)) > 0) {
        delete static_cast<Request*>(ptr);
    }
    while (qResp_->tryReceive(&ptr, sizeof(ptr)) > 0) {
        delete static_cast<Response*>(ptr);
    }
}

void EmcServer::netLoopEntry(void* arg) {
    EmcServer* srv = static_cast<EmcServer*>(arg);
    srv->netLoop_->run();
    srv->netLoopExited_.signal();
}
void EmcServer::workerLoopEntry(void* arg) {
    EmcServer* srv = static_cast<EmcServer*>(arg);
    srv->workerLoop_->run();
    srv->workerLoopExited_.signal();
}

// NetLoop
// Queue direction is fixed by the server wiring.
// TODO: This is the current API, change the API so that swapping queues does not compile (NOLINT-fix)
NetLoop::NetLoop(
        EmcServer* srv,
        SOCKET listenfd,
        epicsMessageQueue* qReq, // NOLINT(bugprone-easily-swappable-parameters)
        epicsMessageQueue* qResp,
        FilterChain& filters)
: srv_(srv), listenfd_(listenfd), qReq_(qReq), qResp_(qResp), filters_(filters)
{}

size_t NetLoop::connectionsFrom(epicsUInt32 addr) const {
    // Admission only: at most 512 entries, no extra counters/locks to maintain.
    return static_cast<size_t>(std::count_if(conns_.cbegin(), conns_.cend(),
        [addr](const auto& connection) { return connection.second.peer.addr == addr; }));
}

/**
 * @brief Accept one client and run it through the filter chain.
 *
 */
void NetLoop::acceptOne() {
    sockaddr_in cli{};
    osiSocklen_t len = sizeof(cli);
    SOCKET s = epicsSocketAccept(listenfd_, (sockaddr*)&cli, &len);
    if (s == INVALID_SOCKET) return;

    // Reject unsupported descriptors before adding the client to conns_.
    if (s < 0 || s >= FD_SETSIZE) {
        Debug(0, "[NetLoop] client descriptor out of range: fd=%d FD_SETSIZE=%d", (int)s, FD_SETSIZE);
        epicsSocketDestroy(s);
        return;
    }

    if (len < (osiSocklen_t)sizeof(cli) || cli.sin_family != AF_INET) {
        epicsSocketDestroy(s);
        Debug(0, "[NetLoop] refused client: addressFamily=%d addressLength=%u", (int)cli.sin_family, (unsigned int)len);
        return;
    }

    PeerInfo peer;
    // TODO: nextId_ wraps after INT_MAX connections (negative ids, -1 means "unset",
    // a reused id could overwrite a live client in conns_). Allocate positive ids that are not in conns_, e.g. allocateId() wrapping to 1.
    peer.conn_id = nextId_++;
    peer.addr = ntohl(cli.sin_addr.s_addr);
    peer.port = ntohs(cli.sin_port);

    const size_t addressCount = connectionsFrom(peer.addr);
    if (conns_.size() >= EMC_MAX_CONNECTIONS || addressCount >= EMC_MAX_CONNECTIONS_PER_ADDRESS) {
        Debug(0, "EMC CONNECTION LIMIT: port=%u peer=%s total=%zu/%zu address=%zu/%zu limit=%s; refusing client", 
                srv_->port(), peer.str().c_str(), conns_.size(), EMC_MAX_CONNECTIONS, addressCount, EMC_MAX_CONNECTIONS_PER_ADDRESS, conns_.size() >= EMC_MAX_CONNECTIONS ? "server" : "address");
        epicsSocketDestroy(s);
        return; // filters saw no onAccept, so they need no onClose
    }

    const FilterVerdict verdict = filters_.onAccept(peer);
    if (verdict != FilterVerdict::Allow) {
        // The chain already closed the filters that saw this peer.
        epicsSocketDestroy(s);
        // Filters log refusals themselves (throttled); keep this at level 1.
        Debug(1, "[NetLoop] Refused client id=%d from %s (%s)", peer.conn_id, peer.str().c_str(), filterVerdictName(verdict));
        return;
    }

    int id = peer.conn_id;
    Conn c;
    c.s = s;
    c.id = id;
    c.peer = peer;
    c.lastActivity = std::chrono::steady_clock::now();
    c.open = true;

    conns_[id] = c;
    fd2id_[s] = id;
    // Make non-blocking
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);

    Debug(1, "[NetLoop] Accepted client id=%d from %s total=%zu/%zu address=%zu/%zu", id, peer.str().c_str(), conns_.size(), EMC_MAX_CONNECTIONS, addressCount + 1, EMC_MAX_CONNECTIONS_PER_ADDRESS);
}

void NetLoop::dropConn(int id) {
    auto it = conns_.find(id);
    if (it == conns_.end()) return;
    // Pending requests are skipped; requests already running finish normally.
    it->second.requests->open.store(false);
    pendingReplyBytes_ -= it->second.outbuf.size();
    filters_.onClose(it->second.peer);
    SOCKET s = it->second.s;
    if (s != INVALID_SOCKET) {
        epicsSocketDestroy(s);
        fd2id_.erase(s);
    }
    Debug(1, "[NetLoop] Closed client id=%d outstanding=%zu pendingReleased=%zu totalPending=%zu connectionsRemaining=%zu", id, it->second.requests->outstanding.load(), it->second.outbuf.size(), pendingReplyBytes_, conns_.size() - 1);
    conns_.erase(it);
}

bool NetLoop::admitRequest(Conn& c, std::chrono::steady_clock::time_point now) {
    const double rate = EmcServer::defaultRequestRate();
    const size_t burst = EmcServer::defaultRequestBurst();
    if (!c.rate.consume(rate, burst, now)) {
        Debug(0, "EMC REQUEST RATE LIMIT: port=%u id=%d peer=%s rate=%g/s burst=%zu credits=%.3f; closing client", srv_->port(), c.id, c.peer.str().c_str(), rate, burst, c.rate.credits());
        dropConn(c.id);
        return false;
    }
    const size_t limit = EmcServer::defaultMaxOutstandingRequests();
    const size_t outstanding = c.requests->outstanding.load();
    if (limit != 0 && outstanding >= limit) {
        Debug(0, "EMC OUTSTANDING REQUEST LIMIT: port=%u id=%d peer=%s outstanding=%zu limit=%zu; closing client", srv_->port(), c.id, c.peer.str().c_str(), outstanding, limit);
        dropConn(c.id);
        return false;
    }
    return true;
}

bool NetLoop::dropExpiredRequest(const Conn& c, std::chrono::steady_clock::time_point now) {
    if (c.in.pending() == 0) return false;
    const double age = std::chrono::duration<double>(now - c.lineStarted).count();
    if (age < EMC_REQUEST_TIMEOUT_SECONDS) return false;
    Debug(0, "EMC REQUEST TIMEOUT: id=%d peer=%s pending=%zu age=%.3fs limit=%.1fs; closing client", c.id, c.peer.str().c_str(), c.in.pending(), age, EMC_REQUEST_TIMEOUT_SECONDS);
    dropConn(c.id);   // invalidates c; do not access it again
    return true;
}

bool NetLoop::dropIdleConnection(const Conn& c, std::chrono::steady_clock::time_point now,
                                 double timeoutSeconds) {
    if (timeoutSeconds == 0.0) return false;
    const double idle = std::chrono::duration<double>(now - c.lastActivity).count();
    if (idle < timeoutSeconds) return false;
    Debug(0, "EMC IDLE TIMEOUT: port=%u id=%d peer=%s idle=%.3fs limit=%gs; closing client", srv_->port(), c.id, c.peer.str().c_str(), idle, timeoutSeconds);
    dropConn(c.id); // invalidates c; do not access it again
    return true;
}

void NetLoop::handleReadable(SOCKET fd) {
    auto itId = fd2id_.find(fd);
    if (itId == fd2id_.end()) return;
    int id = itId->second;
    auto itConn = conns_.find(id);
    if (itConn == conns_.end()) return;
    Conn& c = itConn->second;

    const auto now = std::chrono::steady_clock::now();
    // A late terminator must not rescue an already expired incomplete request.
    if (dropExpiredRequest(c, now)) return;
    const bool hadPending = c.in.pending() != 0;

    char buf[1024];
    const ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) {
        // Capture the socket error before logging can change errno.
        const int socketError = n < 0 ? errno : 0;
        Debug(1, "EMC RX END: id=%d result=%zd errno=%d (%s)", id, n, socketError, n == 0 ? "peer closed" : "recv failed");
        dropConn(id);
        return;
    }
    c.lastActivity = now; // only successful reads count as socket activity
    // One TCP read may contain a partial command or several complete commands.
    Debug(2, "EMC RX: id=%d bytes=%zd raw=\"%s\"", id, n, debugProtocolBytes(buf, (size_t)n).c_str());
    // Reject NUL bytes immediately to prevent misinterpretation.
    if (std::memchr(buf, '\0', static_cast<size_t>(n)) != nullptr) {
        Debug(0, "EMC INVALID REQUEST: id=%d peer=%s NUL byte; closing client", id, c.peer.str().c_str());
        dropConn(id);
        return; // dropConn invalidates c
    }
    if (!c.in.append(buf, (size_t)n)) {
        Debug(0, "EMC REQUEST TOO LONG: id=%d peer=%s pending=%zu received=%zd maxBytes=%zu; closing client", id, c.peer.str().c_str(), c.in.pending(), n, EMC_MAX_REQUEST_BYTES);
        dropConn(id);   // no oversized line reaches filters or the request queue
        return;
    }

    // Extract complete lines (LF, CR or CRLF); a partial line stays buffered.
    std::string line;
    bool completedLine = false;
    while (c.in.next(line)) {
        completedLine = true;
        if (isBlankRequest(line)) {
            Debug(1, "[NetLoop] ignored blank line from id=%d", id);
            continue;
        }

        // Count each command, including filter rejections, not each TCP read.
        if (!admitRequest(c, std::chrono::steady_clock::now())) return;

        const FilterVerdict verdict = filters_.onRequest(c.peer, line);
        // Framing is complete here; command/argument parsing belongs to the dispatcher.
        Debug(2, "EMC PARSE: id=%d bytes=%zu line=\"%s\" filter=%s", id, line.size(), debugProtocolBytes(line.data(), line.size()).c_str(), filterVerdictName(verdict));
        if (verdict == FilterVerdict::Drop) {
            Debug(0, "[NetLoop] Filter dropped client id=%d from %s", id, c.peer.str().c_str());
            dropConn(id);   // invalidates c
            return;
        }

        // Queue all requests, including rejected ones, replies stay in the correct order.
        auto* req = new Request{ id, line, verdict == FilterVerdict::Reject, c.requests };
        void* ptr = req;
        if (qReq_->trySend(&ptr, sizeof(void*)) != 0) {
            Debug(0, "EMC REQUEST QUEUE FULL: port=%u id=%d peer=%s; closing client", srv_->port(), id, c.peer.str().c_str());
            delete req;
            dropConn(id); // avoid silent command loss and discard queued work
            return;
        } else {
            Debug(1, "[NetLoop] queued req id=%d outstanding=%zu: '%s'", id, c.requests->outstanding.load(), line.c_str());
        }
    }
    // Start a new timeout for the next command, never extend an existing timeout.
    if (c.in.pending() != 0 && (!hadPending || completedLine)) {
        c.lineStarted = now;
        Debug(2, "EMC REQUEST START: id=%d pending=%zu lineTimeout=%.1fs", id, c.in.pending(), EMC_REQUEST_TIMEOUT_SECONDS);
    }
}

void NetLoop::pumpResponses() {
    for (;;) {
        void* ptr = nullptr;
        int rc = qResp_->tryReceive(&ptr, sizeof(void*));
        if (rc <= 0) break;
        if (!ptr) continue;

        auto* resp = static_cast<Response*>(ptr);
        // Read per reply, so an iocsh change applies to the next reply.
        const ReplyTerminator term = srv_->replyTerminator();
        auto it = conns_.find(resp->conn_id);
        if (it != conns_.end()) {
            const char* terminator = replyTerminatorBytes(term);
            const size_t termBytes = std::strlen(terminator);
            const size_t clientRoom = EMC_MAX_CLIENT_REPLY_BYTES - it->second.outbuf.size();
            const size_t totalRoom = EMC_MAX_TOTAL_REPLY_BYTES - pendingReplyBytes_;
            // Subtract first: even an unexpectedly huge backend reply cannot
            // overflow the size calculation. Include CR/LF in both budgets.
            const bool clientFull = termBytes > clientRoom
                || resp->line.size() > clientRoom - termBytes;
            const bool totalFull = termBytes > totalRoom
                || resp->line.size() > totalRoom - termBytes;
            if (clientFull || totalFull) {
                Debug(0, "EMC REPLY LIMIT: id=%d pending=%zu reply=%zu terminator=%zu totalPending=%zu limit=%s; closing client", resp->conn_id, it->second.outbuf.size(), resp->line.size(), termBytes, pendingReplyBytes_, clientFull ? "client" : "total");
                dropConn(resp->conn_id); // discard this reply and any later ones
            } else {
                it->second.outbuf.append(resp->line);
                it->second.outbuf.append(terminator);
                pendingReplyBytes_ += resp->line.size() + termBytes;
                Debug(1, "[NetLoop] queued resp id=%d bytes=%zu pending=%zu totalPending=%zu", resp->conn_id, resp->line.size() + termBytes, it->second.outbuf.size(), pendingReplyBytes_);
            }
        } else {
            Debug(1, "EMC REPLY DROP: id=%d bytes=%zu client already closed", resp->conn_id, resp->line.size() + std::strlen(replyTerminatorBytes(term)));
        }
        delete resp;
    }
}

void NetLoop::run() {
    // Make listen socket non-blocking
    int flags = fcntl(listenfd_, F_GETFL, 0);
    fcntl(listenfd_, F_SETFL, flags | O_NONBLOCK);

    while (srv_->running_) {
        const auto now = std::chrono::steady_clock::now();
        const double idleTimeout = srv_->idleTimeout(); // one atomic load per iteration
        // Check timeouts even without new data; move forward first because expired connections may be removed.
        for (auto it = conns_.begin(); it != conns_.end();) {
            const Conn& c = (it++)->second;
            if (!dropExpiredRequest(c, now) && idleTimeout > 0.0) {
                dropIdleConnection(c, now, idleTimeout);
            }
        }
        // Handle replies and close overloaded clients before checking which sockets are ready.
        pumpResponses();
        fd_set rfds, wfds;
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        int maxfd = -1;

        if (listenfd_ != INVALID_SOCKET) {
            FD_SET(listenfd_, &rfds);
            maxfd = std::max(maxfd, (int)listenfd_);
        }
        for (auto& kv : conns_) {
            SOCKET s = kv.second.s;
            FD_SET(s, &rfds);
            if (!kv.second.outbuf.empty()) {
                FD_SET(s, &wfds); // want to write
            }
            maxfd = std::max(maxfd, (int)s);
        }

        // TODO: Replace by self wakeup and epics signals for no delay
        timeval tv{0, 5000}; // 5 ms
        int rc = select(maxfd + 1, &rfds, &wfds, nullptr, &tv);
        const int selectError = rc < 0 ? errno : 0;
        if (!srv_->running_) break;
        if (rc < 0) {
            Debug(1, "EMC SELECT ERROR: errno=%d", selectError);
            epicsThreadSleep(0.01);
            continue;
        }

        if (listenfd_ != INVALID_SOCKET && FD_ISSET(listenfd_, &rfds)) {
            acceptOne();
        }

        // Handle readable sockets
        std::vector<SOCKET> rdyRead;
        rdyRead.reserve(conns_.size());
        for (auto& kv : conns_) {
            if (FD_ISSET(kv.second.s, &rfds)) rdyRead.push_back(kv.second.s);
        }
        for (SOCKET s : rdyRead) handleReadable(s);

        // Handle writable sockets
        std::vector<SOCKET> rdyWrite;
        rdyWrite.reserve(conns_.size());
        for (auto& kv : conns_) {
            if (!kv.second.outbuf.empty() && FD_ISSET(kv.second.s, &wfds)) {
                rdyWrite.push_back(kv.second.s);
            }
        }
        for (SOCKET s : rdyWrite) {
            auto itId = fd2id_.find(s);
            if (itId == fd2id_.end()) continue;
            auto it = conns_.find(itId->second);
            if (it == conns_.end()) continue;
            Conn& c = it->second;

            if (!c.outbuf.empty()) {
                // Counts all bytes being sent; if only partial sent, the rest will be send for the next attempt
                Debug(2, "EMC TX: id=%d bytes=%zu raw=\"%s\" (send attempt)", c.id, c.outbuf.size(), debugProtocolBytes(c.outbuf.data(), c.outbuf.size()).c_str());
                const size_t requested = c.outbuf.size();
                // A broken client connection must not terminate the IOC with SIGPIPE.
                const ssize_t n = send(s, c.outbuf.data(), requested, MSG_NOSIGNAL);
                const int socketError = n < 0 ? errno : 0;
                Debug(2, "EMC TX RESULT: id=%d requested=%zu sent=%zd errno=%d", c.id, requested, n, socketError);
                if (n > 0) {
                    c.lastActivity = std::chrono::steady_clock::now();
                    c.outbuf.erase(0, (size_t)n);
                    pendingReplyBytes_ -= (size_t)n;
                } else if (n < 0) {
                    // EWOULDBLOCK -> try later; any other error -> drop
                    if (socketError != EAGAIN && socketError != EWOULDBLOCK) {
                        dropConn(c.id);
                    }
                }
            }
        }
    }

    // Cleanup: collect IDs first; dropConn() removes entries from conns_.
    std::vector<int> ids;
    ids.reserve(conns_.size());

    std::transform(
        conns_.cbegin(),  // start
        conns_.cend(),    // end
        std::back_inserter(ids),  // append
        [](const auto& connection) {
            return connection.first;
        });

    for (int id : ids)
        dropConn(id);
}

// WorkerLoop:
// Request and response queues roles ->
WorkerLoop::WorkerLoop(
        epicsMessageQueue* qReq, // NOLINT(bugprone-easily-swappable-parameters)
        epicsMessageQueue* qResp,
        std::atomic<bool>* running,
        Dispatcher& dispatcher)
: qReq_(qReq), qResp_(qResp), running_(running), dispatcher_(dispatcher)
{}

std::string WorkerLoop::processLine(const std::string& line) {
    if (line == "health" || line == "ping" || line == "test") return "OK";

    Debug(1, "[WorkerLoop] processLine: line='%s' dispatcher=%p", line.c_str(), (void*)&dispatcher_);

    try {
        auto reply = dispatcher_.processLine(line.c_str());
        Debug(1, "[WorkerLoop] reply='%s'", reply.c_str());
        return reply;
    } catch (const std::exception& e) {
        Debug(0, "[WorkerLoop] dispatcher exception: line='%s' error='%s'",
              line.c_str(), e.what());
        return "ERR exception";
    } catch (...) {
        Debug(0, "[WorkerLoop] unknown dispatcher exception: line='%s'", line.c_str());
        return "ERR exception";
    }
}

/**
 * @brief Main loop of the WorkerLoop.
 *
 * Receives raw client requests from the qReq_ message queue, calls
 * `processLine()` to obtain a reply string, and enqueues a Response*
 * onto qResp_ for the NetLoop to send back to the client.
 *
 * Behavior:
 *  - timed receive -> continue on timeout (keeps thread responsive)
 *  - null checks on dequeued pointers
 *  - if response queue is full: drop response and log (non-blocking)
 */
void WorkerLoop::run() {
    // TODO: move into loop and initialize:
    epicsTimeStamp now, starttime;
    double elapsedtime=0.0;

    while (*running_) {
        void* ptr = nullptr;
        // RETURNS: number of bytes received on success, <=0 on timeout/error
        int rc = qReq_->receive(&ptr, sizeof(void*), 0.2); // timed
        if (rc <= 0) continue;
        if (!ptr) continue;

        // Owning scope releases outstanding even on cancellation/error paths.
        std::unique_ptr<Request> req(static_cast<Request*>(ptr));
        if (!*running_) {
            break;
        }
        if (req->connection && !req->connection->open.load()) {
            Debug(1, "EMC REQUEST SKIP: id=%d client closed; discarding queued command", req->conn_id);
            continue;
        }
        Debug(1, "[WorkerLoop] got req id=%d: '%s'", req->conn_id, req->line.c_str());
        if (epicsTimeGetCurrent(&starttime) != epicsTimeOK) {
            Debug(0, "[WorkerLoop] failed to get dispatch start time: id=%d", req->conn_id);
        }
        if (!req->rejected) {
            Debug(1, "EMC DISPATCH BEGIN: id=%d line=\"%s\"", req->conn_id, debugProtocolBytes(req->line.data(), req->line.size()).c_str());
        }
        auto reply = req->rejected ? std::string("ERR rejected") : processLine(req->line);
        if (epicsTimeGetCurrent(&now) != epicsTimeOK) {
            Debug(0, "[WorkerLoop] failed to get dispatch end time: id=%d", req->conn_id);
        } else {
            elapsedtime = epicsTimeDiffInSeconds(&now, &starttime);
        }

        // Free the request slot before replying, so the client can send the next command immediately.
        Debug(2, "EMC REQUEST COMPLETE: id=%d; releasing outstanding slot", req->conn_id);
        req->complete();
        auto* resp = new Response{ req->conn_id, reply };
        void* rptr = resp;
        int send_rc = qResp_->trySend(&rptr, sizeof(void*));

        if (send_rc != 0) {
            Debug(0, "[WorkerLoop] qResp full, dropping response: id=%d queueResult=%d elapsed=%.6fs",
                  req->conn_id, send_rc, elapsedtime);
            delete resp;
        } else {
            Debug(1, "[WorkerLoop] queued resp id=%d: '%s', elapsed time=%f", req->conn_id, reply.c_str(), elapsedtime);
        }

    }
}
