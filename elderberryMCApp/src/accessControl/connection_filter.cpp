// SPDX-FileCopyrightText: 2026 Helmholtz-Zentrum Berlin für Materialien und Energie GmbH
// SPDX-License-Identifier: MIT

/**
 * @file  connection_filter.cpp
 * @brief FilterChain: runs admission filters in order, failing closed.
 */

#include "connection_filter.h"

#include <cstdio>
#include <exception>
#include <stdexcept>

// Shared runtime debug switch, defined and exported by emc_server.cpp.
extern int emc_server_debug;

#define Debug(L, FMT, ...) do { \
    if ((L) <= emc_server_debug) \
        std::fprintf(stderr, FMT "\t- file : " __FILE__ ", line %d\n", \
                     __VA_ARGS__, __LINE__); \
} while (false)

std::string formatIpv4(epicsUInt32 addr) {
    char text[16];
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u",
                  (unsigned)((addr >> 24) & 0xFFu), (unsigned)((addr >> 16) & 0xFFu),
                  (unsigned)((addr >> 8) & 0xFFu), (unsigned)(addr & 0xFFu));
    return text;
}

std::string PeerInfo::str() const {
    char text[24];
    std::snprintf(text, sizeof(text), "%s:%u", formatIpv4(addr).c_str(), (unsigned)port);
    return text;
}

const char* filterVerdictName(FilterVerdict verdict) {
    switch (verdict) {
    case FilterVerdict::Allow:  return "allow";
    case FilterVerdict::Reject: return "reject";
    case FilterVerdict::Drop:   return "drop";
    }
    return "drop";
}

namespace {
// Must not allocate: it may run while handling std::bad_alloc.
void reportException(const ConnectionFilter& filter, const char* hook,
                     const PeerInfo& peer, const char* what) noexcept {
    const char* name = "<unknown>";
    try {
        const char* candidate = filter.name();
        if (candidate) name = candidate;
    } catch (...) {
        // Keep the fallback if retrieving the name throws.
    }

    const epicsUInt32 a = peer.addr;
    Debug(0, "[FilterChain] filter '%s' threw in %s for client id=%d (%u.%u.%u.%u:%u): %s",
          name, hook, peer.conn_id,
          (unsigned)((a >> 24) & 0xFFu), (unsigned)((a >> 16) & 0xFFu),
          (unsigned)((a >> 8) & 0xFFu), (unsigned)(a & 0xFFu),
          (unsigned)peer.port, what);
}
} // namespace

void FilterChain::add(std::unique_ptr<ConnectionFilter> filter) {
    if (!filter) throw std::invalid_argument("FilterChain::add: null filter");
    filters_.push_back(std::move(filter));
}

void FilterChain::clear() {
    filters_.clear();
}

FilterVerdict FilterChain::onAccept(const PeerInfo& peer) noexcept {
    for (size_t i = 0; i < filters_.size(); ++i) {
        ConnectionFilter& filter = *filters_[i];
        FilterVerdict verdict = FilterVerdict::Drop;
        try {
            verdict = filter.onAccept(peer);
        } catch (const std::exception& e) {
            reportException(filter, "onAccept", peer, e.what());
        } catch (...) {
            reportException(filter, "onAccept", peer, "unknown exception");
        }
        if (verdict != FilterVerdict::Allow) {
            closeFirst(i + 1, peer);   // every filter that saw onAccept
            return verdict;
        }
    }
    return FilterVerdict::Allow;
}

FilterVerdict FilterChain::onRequest(const PeerInfo& peer, const std::string& line) noexcept {
    for (auto& filter : filters_) {
        try {
            const FilterVerdict verdict = filter->onRequest(peer, line);
            if (verdict != FilterVerdict::Allow) return verdict;
        } catch (const std::exception& e) {
            reportException(*filter, "onRequest", peer, e.what());
            return FilterVerdict::Drop;
        } catch (...) {
            reportException(*filter, "onRequest", peer, "unknown exception");
            return FilterVerdict::Drop;
        }
    }
    return FilterVerdict::Allow;
}

void FilterChain::onClose(const PeerInfo& peer) noexcept {
    closeFirst(filters_.size(), peer);
}

void FilterChain::closeFirst(size_t count, const PeerInfo& peer) noexcept {
    for (size_t i = count; i-- > 0;) {
        ConnectionFilter& filter = *filters_[i];
        try {
            filter.onClose(peer);
        } catch (const std::exception& e) {
            reportException(filter, "onClose", peer, e.what());
        } catch (...) {
            reportException(filter, "onClose", peer, "unknown exception");
        }
    }
}
