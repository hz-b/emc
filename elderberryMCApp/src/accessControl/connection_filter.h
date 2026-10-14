// SPDX-FileCopyrightText: 2026 Helmholtz-Zentrum Berlin für Materialien und Energie GmbH
// SPDX-License-Identifier: MIT

#ifndef CONNECTION_FILTER_H
#define CONNECTION_FILTER_H
/**
 * @file  connection_filter.h
 * @brief Admission filters for the EMC server (Chain of Responsibility).
 *
 * The NetLoop asks the filters before it keeps a new connection and before it queues requests.
 * Each filter allows, rejects or drops; the first that does not allow ends the chain.
 *
 * Threads: filters run on one thread only, no locks.
 * Don't change the chain while the server runs.
 * If a filter throws exceptions, the connection is dropped.
 *
 */

#include <memory>
#include <string>
#include <vector>
#include <epicsTypes.h>

/// Identity of one accepted TCP peer, as seen by the filters.
struct PeerInfo {
    int            conn_id = -1;
    epicsUInt32    addr = 0;   ///< IPv4 address, host byte order
    unsigned short port = 0;   ///< TCP port, host byte order

    /// "a.b.c.d:port", for logging.
    std::string str() const;
};

/// Dotted-quad text for an IPv4 address in host byte order.
std::string formatIpv4(epicsUInt32 addr);

/// Decision of one filter hook.
enum class FilterVerdict {
    Allow,   ///< pass to the next filter; the event proceeds if all allow
    Reject,  ///< onAccept: close the connection.
             ///< onRequest: do not dispatch the line; the client gets
             ///< "ERR rejected" in its normal reply order.
    Drop     ///< close the connection without a reply
};

const char* filterVerdictName(FilterVerdict verdict);

/// One filter of the chain. Keep hooks fast: a slow hook stalls every client.
struct ConnectionFilter {
    virtual ~ConnectionFilter() = default;

    /// Short name used in log messages.
    virtual const char* name() const = 0;

    /// A new TCP connection, before the server gives it any buffers.
    virtual FilterVerdict onAccept(const PeerInfo& peer) {
        (void)peer;
        return FilterVerdict::Allow;
    }

    /// One complete request line (CR/LF removed), before it is queued.
    virtual FilterVerdict onRequest(const PeerInfo& peer, const std::string& line) {
        (void)peer; (void)line;
        return FilterVerdict::Allow;
    }

    /// Called once for every onAccept this filter saw, even if the peer was refused.
    virtual void onClose(const PeerInfo& peer) { (void)peer; }
};


/// Runs the filters in the order they were added. An empty chain allows all.
/// Caller: call onClose only for peers that onAccept allowed.
/// If onAccept refuses a peer, the chain cleans up itself.
class FilterChain {
public:
    FilterChain() = default;
    FilterChain(const FilterChain&) = delete;
    FilterChain& operator=(const FilterChain&) = delete;

    /// Appends a filter. Throws std::invalid_argument for a null filter, so a
    /// missing filter cannot silently leave the chain open.
    void add(std::unique_ptr<ConnectionFilter> filter);
    void clear();
    /// Exchanges filters without allocating. Neither chain may be used by a
    /// running server during this operation.
    void swap(FilterChain& other) noexcept {
        filters_.swap(other.filters_);
    }
    bool empty() const { return filters_.empty(); }
    size_t size() const { return filters_.size(); }

    FilterVerdict onAccept(const PeerInfo& peer) noexcept;
    FilterVerdict onRequest(const PeerInfo& peer, const std::string& line) noexcept;
    /// Notifies every filter, even if an earlier one throws.
    void onClose(const PeerInfo& peer) noexcept;

private:
    std::vector<std::unique_ptr<ConnectionFilter>> filters_;

    /// Closes filters [0, count) in reverse order.
    void closeFirst(size_t count, const PeerInfo& peer) noexcept;
};

#endif /* CONNECTION_FILTER_H */
