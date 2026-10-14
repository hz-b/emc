/**
 * @file  testAccessControl.cpp
 * @brief Tests for the ACL parser, the four strategies, the filter chain and
 *        the EMC server's admission path over real loopback sockets.
 *
 * Run with "make runtests" or directly. The loopback part binds TCP port
 * EMC_TEST_PORT (default 15731) and connects from 127.0.0.1 and 127.0.0.2;
 * it is skipped if the port cannot be bound.
 */

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <epicsThread.h>
#include <epicsUnitTest.h>
#include <osiSock.h>
#include <testMain.h>

#include "access_control.h"
#include "acl_session.h"
#include "connection_filter.h"
#include "echo_dispatcher.h"
#include "emc_server.h"

namespace {

// ---------------------------------------------------------------- helpers
epicsUInt32 ip(const char* text) {
    Ipv4Network net;
    std::string error;
    if (!Ipv4Network::parse(text, net, error) || net.prefix != 32) {
        testAbort("bad test address %s", text);
    }
    return net.addr;
}

std::unique_ptr<AccessPolicy> build(const AclConfig& config) {
    std::string error;
    return makeAccessPolicy(config, error);
}

bool hasWarning(const AclConfig& config, const char* fragment) {
    for (const std::string& w : aclWarnings(config)) {
        if (w.find(fragment) != std::string::npos) return true;
    }
    return false;
}

// ---------------------------------------------------------------- parser
void testParser() {
    testDiag("Ipv4Network::parse");
    struct Good { const char* text; epicsUInt32 addr; unsigned prefix; };
    const Good good[] = {
        {"10.0.0.0/8",         0x0A000000u, 8},
        {"192.168.1.5",        0xC0A80105u, 32},
        {"192.168.1.5/32",     0xC0A80105u, 32},
        {"172.16.0.0/12",      0xAC100000u, 12},
        {"0.0.0.0/0",          0x00000000u, 0},
        {"255.255.255.255",    0xFFFFFFFFu, 32},
        {"0.0.0.0",            0x00000000u, 32},
    };
    for (const Good& g : good) {
        Ipv4Network net;
        std::string error;
        const bool ok = Ipv4Network::parse(g.text, net, error);
        testOk(ok && net.addr == g.addr && net.prefix == g.prefix,
               "accepts '%s' -> %s", g.text, ok ? net.str().c_str() : error.c_str());
    }

    const char* bad[] = {
        "", "10", "10.0.0", "10.0.0.0.0", "10..0.1", "256.0.0.1", "1111.0.0.1",
        "010.0.0.1",        // octal to inet_aton
        "0x0a.0.0.1",       // hex to inet_aton
        "+1.2.3.4", "-1.2.3.4", " 10.0.0.1", "10.0.0.1 ", "10.0.0.1\n",
        "10.0.0.1/", "10.0.0.1/33", "10.0.0.1/08", "10.0.0.1/-1", "10.0.0.1/100",
        "10.0.0.1/32x", "10.0.0.1x", "a.b.c.d",
        "10.0.0.5/8",       // host bits set
        "192.168.1.1/24",   // host bits set
    };
    for (const char* text : bad) {
        Ipv4Network net;
        net.addr = 0x01020304u;
        net.prefix = 7;
        std::string error;
        const bool ok = Ipv4Network::parse(text, net, error);
        testOk(!ok && !error.empty() && net.addr == 0x01020304u && net.prefix == 7,
               "rejects '%s' (%s), output unchanged", text, error.c_str());
    }
    {
        Ipv4Network net;
        std::string error;
        testOk(!Ipv4Network::parse(nullptr, net, error), "rejects a null string");
        Ipv4Network::parse("10.0.0.5/8", net, error);
        testOk(error.find("10.0.0.0/8") != std::string::npos,
               "host-bits error suggests the network: %s", error.c_str());
    }

    Ipv4Network all;  all.addr = 0; all.prefix = 0;
    Ipv4Network ten;  ten.addr = 0x0A000000u; ten.prefix = 8;
    Ipv4Network host; host.addr = 0x0A000001u; host.prefix = 32;
    testOk1(all.mask() == 0u && all.contains(ip("1.2.3.4")) && all.contains(ip("255.255.255.255")));
    testOk1(ten.mask() == 0xFF000000u && ten.contains(ip("10.255.0.1")) && !ten.contains(ip("11.0.0.0")));
    testOk1(host.mask() == 0xFFFFFFFFu && host.contains(ip("10.0.0.1")) && !host.contains(ip("10.0.0.2")));
    testOk1(formatIpv4(0xC0A80105u) == "192.168.1.5" && ten.str() == "10.0.0.0/8");
}

void testModeNames() {
    testDiag("parseAclMode");
    AclMode m;
    testOk1(parseAclMode("allowlist", m) && m == AclMode::Allowlist);
    testOk1(parseAclMode("DENYLIST", m) && m == AclMode::Denylist);
    testOk1(parseAclMode("default-deny", m) && m == AclMode::DefaultDeny);
    testOk1(parseAclMode("defaultdeny", m) && m == AclMode::DefaultDeny);
    testOk1(parseAclMode("Allow-All", m) && m == AclMode::AllowAll);
    testOk1(!parseAclMode("allow", m) && !parseAclMode("", m) && !parseAclMode(nullptr, m));
}

// ---------------------------------------------------------------- strategies
void testStrategies() {
    testDiag("AccessPolicy strategies");
    AllowAllPolicy all;
    testOk1(all.isAllowed(ip("1.2.3.4")) && all.mode() == AclMode::AllowAll);

    Ipv4Network ten;  ten.addr = ip("10.0.0.0"); ten.prefix = 8;
    Ipv4Network one;  one.addr = ip("192.168.1.5"); one.prefix = 32;

    AllowlistPolicy emptyAllow({});
    testOk(!emptyAllow.isAllowed(ip("127.0.0.1")), "empty allowlist refuses everyone");
    AllowlistPolicy allow({ten, one});
    testOk1(allow.isAllowed(ip("10.1.2.3")) && allow.isAllowed(ip("192.168.1.5")));
    testOk1(!allow.isAllowed(ip("192.168.1.6")) && !allow.isAllowed(ip("11.0.0.1")));

    DenylistPolicy emptyDeny({});
    testOk(emptyDeny.isAllowed(ip("8.8.8.8")), "empty denylist accepts everyone");
    DenylistPolicy deny({ten, one});
    testOk1(!deny.isAllowed(ip("10.1.2.3")) && !deny.isAllowed(ip("192.168.1.5")));
    testOk1(deny.isAllowed(ip("192.168.1.6")) && deny.isAllowed(ip("11.0.0.1")));

    // allow 10/8, except 10.0.5.0/24, except again 10.0.5.7
    Ipv4Network sub;  sub.addr = ip("10.0.5.0"); sub.prefix = 24;
    Ipv4Network hole; hole.addr = ip("10.0.5.7"); hole.prefix = 32;
    std::vector<AclRule> rules = {
        {AclAction::Allow, ten}, {AclAction::Deny, sub}, {AclAction::Allow, hole}};
    std::vector<AclRule> reversed(rules.rbegin(), rules.rend());
    DefaultDenyPolicy dd(rules), ddRev(reversed);
    const char* probes[] = {"10.1.1.1", "10.0.5.1", "10.0.5.7", "11.0.0.1", "127.0.0.1"};
    const bool expected[] = {true, false, true, false, false};
    for (size_t i = 0; i < 5; ++i) {
        testOk(dd.isAllowed(ip(probes[i])) == expected[i] &&
               ddRev.isAllowed(ip(probes[i])) == expected[i],
               "default-deny, most specific wins, order independent: %s -> %s",
               probes[i], expected[i] ? "accept" : "refuse");
    }
    DefaultDenyPolicy tie({{AclAction::Allow, ten}, {AclAction::Deny, ten}});
    DefaultDenyPolicy tieRev({{AclAction::Deny, ten}, {AclAction::Allow, ten}});
    testOk(!tie.isAllowed(ip("10.0.0.1")) && !tieRev.isAllowed(ip("10.0.0.1")),
           "equally specific allow and deny: deny wins");
    DefaultDenyPolicy none({});
    testOk(!none.isAllowed(ip("127.0.0.1")), "default-deny without rules refuses everyone");
}

// ---------------------------------------------------------------- config
void testConfig() {
    testDiag("AclConfig and makeAccessPolicy");
    std::string error;

    AclConfig fresh;
    testOk(fresh.mode() == AclMode::DefaultDeny, "unconfigured mode is default-deny");
    auto p = build(fresh);
    testOk(p && !p->isAllowed(ip("127.0.0.1")), "unconfigured ACL refuses localhost");
    testOk1(hasWarning(fresh, "every client will be rejected"));

    AclConfig c;
    testOk1(c.addRule(AclAction::Allow, "127.0.0.1", error));
    testOk1(c.addRule(AclAction::Allow, "127.0.0.1/32", error) && c.rules().size() == 1);
    const bool refused = !c.addRule(AclAction::Allow, "127.0.0.01", error);
    testOk(refused && c.errorCount() == 1, "bad rule is counted: %s", error.c_str());
    testOk(!build(c), "any recorded error blocks the policy");
    c.clear();
    testOk1(c.errorCount() == 0 && c.rules().empty() && c.mode() == AclMode::DefaultDeny);
    c.recordError();
    testOk(!build(c), "recordError (e.g. unknown mode) blocks the policy");
    c.clear();

    c.setMode(AclMode::AllowAll);
    p = build(c);
    testOk1(p && p->mode() == AclMode::AllowAll && p->isAllowed(ip("1.2.3.4")));
    testOk1(hasWarning(c, "access control is off"));
    c.addRule(AclAction::Allow, "10.0.0.0/8", error);
    p = makeAccessPolicy(c, error);
    testOk(!p, "allow-all with rules is rejected: %s", error.c_str());

    c.setMode(AclMode::Allowlist);
    p = build(c);
    testOk1(p && p->mode() == AclMode::Allowlist && p->isAllowed(ip("10.9.9.9")) &&
            !p->isAllowed(ip("11.0.0.1")));
    c.addRule(AclAction::Deny, "10.0.5.0/24", error);
    p = makeAccessPolicy(c, error);
    testOk(!p, "allowlist with a deny rule is rejected: %s", error.c_str());

    c.setMode(AclMode::Denylist);
    p = makeAccessPolicy(c, error);
    testOk(!p, "denylist with an allow rule is rejected: %s", error.c_str());

    c.setMode(AclMode::DefaultDeny);
    p = build(c);
    testOk1(p && p->isAllowed(ip("10.1.0.1")) && !p->isAllowed(ip("10.0.5.1")));

    c.clear();
    c.setMode(AclMode::Denylist);
    c.addRule(AclAction::Deny, "10.0.5.0/24", error);
    p = build(c);
    testOk1(p && p->mode() == AclMode::Denylist && !p->isAllowed(ip("10.0.5.1")) &&
            p->isAllowed(ip("10.0.6.1")));
    c.clear();
    testOk1(hasWarning(c, "denylist is empty"));

    c.setMode(AclMode::DefaultDeny);
    c.addRule(AclAction::Allow, "10.0.0.0/8", error);
    c.addRule(AclAction::Deny, "10.0.0.0/8", error);
    testOk1(hasWarning(c, "has no effect"));
    c.clear();
    c.addRule(AclAction::Allow, "0.0.0.0/0", error);
    testOk1(hasWarning(c, "matches every address"));

    AclConfig full;
    char text[32];
    bool allAdded = true;
    for (unsigned i = 0; i < AclConfig::kMaxRules; ++i) {
        std::snprintf(text, sizeof(text), "10.%u.%u.0/24", i / 256u, i % 256u);
        allAdded = full.addRule(AclAction::Allow, text, error) && allAdded;
    }
    testOk1(allAdded && full.rules().size() == AclConfig::kMaxRules);
    const bool overLimit = !full.addRule(AclAction::Allow, "11.0.0.0/8", error);
    testOk(overLimit && full.errorCount() == 1, "rule limit enforced: %s", error.c_str());
}

// ---------------------------------------------------------------- chain
struct Recorder : ConnectionFilter {
    std::string tag;
    std::vector<std::string>* log;
    FilterVerdict acceptVerdict = FilterVerdict::Allow;
    bool throwOnAccept = false, throwOnClose = false;
    Recorder(const char* t, std::vector<std::string>* l) : tag(t), log(l) {}
    const char* name() const override { return tag.c_str(); }
    FilterVerdict onAccept(const PeerInfo&) override {
        log->push_back(tag + ".accept");
        if (throwOnAccept) throw std::runtime_error("boom");
        return acceptVerdict;
    }
    void onClose(const PeerInfo&) override {
        log->push_back(tag + ".close");
        if (throwOnClose) throw 42;
    }
};

void testChain() {
    testDiag("FilterChain");
    PeerInfo peer;
    peer.conn_id = 7; peer.addr = ip("127.0.0.1"); peer.port = 4000;
    testOk1(peer.str() == "127.0.0.1:4000");

    FilterChain empty;
    testOk(empty.onAccept(peer) == FilterVerdict::Allow &&
           empty.onRequest(peer, "x") == FilterVerdict::Allow, "empty chain allows");

    std::vector<std::string> log;
    FilterChain chain;
    auto* a = new Recorder("a", &log);
    auto* b = new Recorder("b", &log);
    auto* c = new Recorder("c", &log);
    chain.add(std::unique_ptr<ConnectionFilter>(a));
    chain.add(std::unique_ptr<ConnectionFilter>(b));
    chain.add(std::unique_ptr<ConnectionFilter>(c));
    testOk1(chain.onAccept(peer) == FilterVerdict::Allow && log.size() == 3 &&
            log[0] == "a.accept" && log[2] == "c.accept");

    log.clear();
    b->acceptVerdict = FilterVerdict::Reject;
    testOk(chain.onAccept(peer) == FilterVerdict::Reject && log.size() == 4 &&
           log[2] == "b.close" && log[3] == "a.close",
           "first non-Allow verdict ends the chain and closes the filters that saw the peer");

    log.clear();
    b->acceptVerdict = FilterVerdict::Allow;
    a->throwOnAccept = true;
    testOk(chain.onAccept(peer) == FilterVerdict::Drop && log.size() == 2 && log[1] == "a.close",
           "an exception in a filter fails closed (Drop)");

    log.clear();
    a->throwOnClose = true;
    chain.onClose(peer);
    testOk(log.size() == 3 && log[0] == "c.close" && log[2] == "a.close",
           "onClose reaches every filter, in reverse order, even if one throws");

    bool threw = false;
    try { chain.add(std::unique_ptr<ConnectionFilter>()); }
    catch (const std::invalid_argument&) { threw = true; }
    testOk(threw && chain.size() == 3, "adding a null filter throws");

    threw = false;
    try { AclFilter bad{std::unique_ptr<const AccessPolicy>()}; }
    catch (const std::invalid_argument&) { threw = true; }
    testOk(threw, "AclFilter refuses a null policy");

    Ipv4Network lo; lo.addr = ip("127.0.0.1"); lo.prefix = 32;
    AclFilter acl(std::unique_ptr<const AccessPolicy>(new AllowlistPolicy({lo})));
    PeerInfo other = peer; other.addr = ip("127.0.0.2");
    testOk1(acl.onAccept(peer) == FilterVerdict::Allow);
    testOk1(acl.onAccept(other) == FilterVerdict::Drop);
    acl.onAccept(other);
    testOk1(acl.acceptedCount() == 1 && acl.deniedCount() == 2);
    testOk1(acl.onRequest(other, "anything") == FilterVerdict::Allow);
}

// ---------------------------------------------------------------- shared ACL
bool contains(const std::string& text, const char* fragment) {
    return text.find(fragment) != std::string::npos;
}

void testShared() {
    testDiag("SharedAcl and AclSession");
    std::string error, canonical;
    testOk1(SharedAcl::reset());

    AclSession mono("mono"), sim("sim");
    testOk(sim.chain().empty() && mono.chain().empty(),
           "a session that never called begin() has an empty chain");

    // Unconfigured: default-deny without rules.
    testOk1(mono.begin());
    PeerInfo peer; peer.conn_id = 1; peer.addr = ip("127.0.0.1"); peer.port = 1;
    testOk(mono.chain().onAccept(peer) == FilterVerdict::Drop,
           "unconfigured shared ACL refuses everyone");
    mono.end();

    // One configuration, applied by both services.
    testOk1(SharedAcl::setMode("default-deny", error) == AclChange::Ok);
    testOk1(SharedAcl::addRule(AclAction::Allow, "127.0.0.1", canonical, error) == AclChange::Ok);
    testOk(canonical == "127.0.0.1/32", "canonical form reported: %s", canonical.c_str());
    testOk1(SharedAcl::addRule(AclAction::Allow, "10.0.0.0/8", canonical, error) == AclChange::Ok);
    testOk1(mono.begin() && sim.begin());
    PeerInfo ten = peer; ten.addr = ip("10.1.2.3");
    PeerInfo other = peer; other.addr = ip("192.168.0.1");
    testOk(mono.chain().onAccept(peer) == FilterVerdict::Allow &&
           sim.chain().onAccept(peer) == FilterVerdict::Allow &&
           mono.chain().onAccept(ten) == FilterVerdict::Allow &&
           sim.chain().onAccept(ten) == FilterVerdict::Allow,
           "both services accept the allowed addresses");
    testOk(mono.chain().onAccept(other) == FilterVerdict::Drop &&
           sim.chain().onAccept(other) == FilterVerdict::Drop,
           "both services refuse everything else");
    testOk(&mono.chain() != &sim.chain(), "each service has its own chain");
    testOk(mono.accepted() == 2 && mono.denied() == 1 && sim.accepted() == 2 && sim.denied() == 1,
           "counters are per service");

    // Locked while any service runs, with the running services named.
    testOk1(SharedAcl::locked());
    testOk(SharedAcl::setMode("allow-all", error) == AclChange::Locked &&
           contains(error, "mono") && contains(error, "sim"),
           "mode change refused while running: %s", error.c_str());
    testOk1(SharedAcl::addRule(AclAction::Deny, "10.9.0.0/16", canonical, error) == AclChange::Locked);
    testOk1(SharedAcl::clear(error) == AclChange::Locked);
    testOk(!SharedAcl::reset(), "reset refused while locked");
    mono.end();
    testOk(SharedAcl::locked(), "still locked while the other service runs");
    testOk1(SharedAcl::setMode("allow-all", error) == AclChange::Locked && contains(error, "sim") &&
            !contains(error, "mono"));
    mono.end();   // repeated end() is harmless
    testOk1(SharedAcl::locked());
    sim.end();
    testOk(!SharedAcl::locked(), "unlocked once every service has stopped");

    // begin() rebuilds the chain: a restart picks up changes made while stopped.
    testOk1(SharedAcl::addRule(AclAction::Deny, "10.1.0.0/16", canonical, error) == AclChange::Ok);
    testOk1(mono.begin());
    testOk(mono.chain().onAccept(ten) == FilterVerdict::Drop &&
           mono.chain().onAccept(peer) == FilterVerdict::Allow &&
           mono.accepted() == 1 && mono.denied() == 1,
           "restart applies the new rule and resets the counters");
    testOk1(mono.begin() && SharedAcl::locked());   // idempotent while active
    mono.end();

    // Invalid input blocks every service until clear().
    testOk1(SharedAcl::addRule(AclAction::Allow, "10.0.0.1/8", canonical, error) == AclChange::Invalid);
    testOk1(SharedAcl::setMode("alowlist", error) == AclChange::Invalid);
    testOk(!mono.begin() && !sim.begin() && !SharedAcl::locked(),
           "a rejected configuration starts no service and takes no lock");
    testOk(mono.accepted() == 1, "failed begin() leaves the previous chain untouched");
    testOk1(SharedAcl::clear(error) == AclChange::Ok);
    testOk1(SharedAcl::setMode("allow-all", error) == AclChange::Ok && sim.begin());
    testOk(sim.chain().onAccept(other) == FilterVerdict::Allow, "allow-all accepts everyone");
    sim.end();

    // emcAclCheck backend.
    testOk1(SharedAcl::setMode("allowlist", error) == AclChange::Ok);
    testOk1(SharedAcl::addRule(AclAction::Allow, "10.0.0.0/8", canonical, error) == AclChange::Ok);
    bool accepted = false;
    testOk1(SharedAcl::check("10.2.3.4", accepted, error) && accepted);
    testOk1(SharedAcl::check("11.2.3.4", accepted, error) && !accepted);
    testOk(!SharedAcl::check("10.0.0.0/8", accepted, error), "check takes one address: %s", error.c_str());
    testOk1(!SharedAcl::check("nonsense", accepted, error));

    // A session destroyed while active releases its lock.
    {
        AclSession temp("temp");
        testOk1(temp.begin() && SharedAcl::locked());
    }
    testOk(!SharedAcl::locked(), "destroying an active session releases the lock");
    testOk1(SharedAcl::reset());
}

// ---------------------------------------------------------------- loopback
// Rejects lines starting with "bad", drops the connection on "kill", and
// counts accept/close calls to check the one-close-per-accept contract.
struct ProtocolFilter : ConnectionFilter {
    int accepts = 0, closes = 0;
    const char* name() const override { return "protocol"; }
    FilterVerdict onAccept(const PeerInfo&) override { ++accepts; return FilterVerdict::Allow; }
    FilterVerdict onRequest(const PeerInfo&, const std::string& line) override {
        if (line.compare(0, 3, "bad") == 0) return FilterVerdict::Reject;
        if (line == "kill") return FilterVerdict::Drop;
        return FilterVerdict::Allow;
    }
    void onClose(const PeerInfo&) override { ++closes; }
};

// Plain blocking client with a receive timeout.
struct Client {
    SOCKET s = INVALID_SOCKET;
    ~Client() { if (s != INVALID_SOCKET) epicsSocketDestroy(s); }

    bool connectFrom(const char* source, unsigned short port) {
        s = epicsSocketCreate(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET) return false;
        struct timeval tv;
        tv.tv_sec = 2; tv.tv_usec = 0;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv));
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(ip(source));
        if (bind(s, (sockaddr*)&local, sizeof(local)) != 0) return false;
        sockaddr_in srv{};
        srv.sin_family = AF_INET;
        srv.sin_addr.s_addr = htonl(ip("127.0.0.1"));
        srv.sin_port = htons(port);
        return connect(s, (sockaddr*)&srv, sizeof(srv)) == 0;
    }
    bool send(const std::string& text) {
        return ::send(s, text.data(), text.size(), 0) == (ssize_t)text.size();
    }
    // Reads until @p lines newlines arrive, EOF, error or timeout.
    // Returns the text; @p closed is set on EOF/reset.
    std::string read(int lines, bool& closed) {
        std::string got;
        closed = false;
        char buf[256];
        while (true) {
            int newlines = 0;
            for (char ch : got) newlines += (ch == '\n');
            if (newlines >= lines) break;
            const ssize_t n = recv(s, buf, sizeof(buf), 0);
            if (n > 0) { got.append(buf, (size_t)n); continue; }
            if (n == 0 || errno == ECONNRESET || errno == EPIPE) closed = true;
            break;   // EOF, error or timeout
        }
        return got;
    }
};

void testLoopback() {
    testDiag("EmcServer admission over loopback");
    const char* env = std::getenv("EMC_TEST_PORT");
    const unsigned short port = (unsigned short)(env ? std::atoi(env) : 15731);

    Ipv4Network lo; lo.addr = ip("127.0.0.1"); lo.prefix = 32;
    EchoDispatcher echo;
    FilterChain chain;
    auto* acl = new AclFilter(std::unique_ptr<const AccessPolicy>(new AllowlistPolicy({lo})));
    auto* proto = new ProtocolFilter;
    chain.add(std::unique_ptr<ConnectionFilter>(acl));
    chain.add(std::unique_ptr<ConnectionFilter>(proto));

    EmcServer server(echo, chain, port);
    if (!server.start()) {
        testSkip(9, "cannot bind the test port (set EMC_TEST_PORT)");
        return;
    }
    bool closed = false;
    {
        Client ok;
        testOk(ok.connectFrom("127.0.0.1", port), "client from 127.0.0.1 connects");
        ok.send("ping\n");
        testOk1(ok.read(1, closed) == "OK\n");
        ok.send("hello\nbad input\nworld\n");
        const std::string replies = ok.read(3, closed);
        testOk(replies == "hello\nERR rejected\nworld\n",
               "rejected line answered in order: '%s'", replies.c_str());
        ok.send("kill\n");
        const std::string after = ok.read(1, closed);
        testOk(after.empty() && closed, "Drop verdict closes the connection");
    }
    {
        Client denied;
        const bool connected = denied.connectFrom("127.0.0.2", port);
        // The kernel completes the handshake; the server then closes at once.
        denied.send("ping\n");
        const std::string reply = denied.read(1, closed);
        testOk(connected && reply.empty() && closed,
               "client from 127.0.0.2 is closed without any reply");
    }
    {
        Client again;
        testOk1(again.connectFrom("127.0.0.1", port));
        again.send("ping\n");
        testOk(again.read(1, closed) == "OK\n", "allowed clients still served after a refusal");
    }
    server.stop();
    testOk(acl->acceptedCount() == 2 && acl->deniedCount() == 1,
           "ACL counters: %lu accepted, %lu refused", acl->acceptedCount(), acl->deniedCount());
    // The protocol filter sits after the ACL: it never saw the refused peer,
    // and got exactly one close per accept.
    testOk(proto->accepts == 2 && proto->closes == 2,
           "one onClose per onAccept: %d accepts, %d closes", proto->accepts, proto->closes);
}

// Two services (like emcServerStart and emcSimStart) on two ports, one ACL.
void testTwoServers() {
    testDiag("two EmcServers sharing one ACL");
    const char* env = std::getenv("EMC_TEST_PORT");
    const unsigned short portA = (unsigned short)((env ? std::atoi(env) : 15731) + 10);
    const unsigned short portB = (unsigned short)(portA + 1);

    std::string error, canonical;
    testOk1(SharedAcl::reset());
    testOk1(SharedAcl::setMode("default-deny", error) == AclChange::Ok);
    testOk1(SharedAcl::addRule(AclAction::Allow, "127.0.0.0/8", canonical, error) == AclChange::Ok);
    testOk1(SharedAcl::addRule(AclAction::Deny, "127.0.0.2", canonical, error) == AclChange::Ok);

    EchoDispatcher echo;
    AclSession aclA("mono"), aclB("sim");
    EmcServer serverA(echo, aclA.chain(), portA);
    EmcServer serverB(echo, aclB.chain(), portB);
    bool startedA = aclA.begin() && serverA.start();
    bool startedB = aclB.begin() && serverB.start();
    if (!startedA || !startedB) {
        serverA.stop(); serverB.stop(); aclA.end(); aclB.end();
        testSkip(6, "cannot bind the test ports (set EMC_TEST_PORT)");
        SharedAcl::reset();
        return;
    }

    bool closed = false;
    const unsigned short ports[] = {portA, portB};
    const char* names[] = {"mono", "sim"};
    for (int i = 0; i < 2; ++i) {
        {
            Client ok;
            ok.connectFrom("127.0.0.1", ports[i]);
            ok.send("ping\n");
            testOk(ok.read(1, closed) == "OK\n", "%s: 127.0.0.1 is served", names[i]);
        }
        {
            Client denied;
            denied.connectFrom("127.0.0.2", ports[i]);
            denied.send("ping\n");
            const std::string reply = denied.read(1, closed);
            testOk(reply.empty() && closed, "%s: 127.0.0.2 (explicit deny) is refused", names[i]);
        }
        {
            Client third;   // allowed through the 127.0.0.0/8 rule
            third.connectFrom("127.0.0.3", ports[i]);
            third.send("ping\n");
            testOk(third.read(1, closed) == "OK\n", "%s: 127.0.0.3 is served", names[i]);
        }
    }
    testOk(SharedAcl::setMode("allow-all", error) == AclChange::Locked,
           "ACL is locked while the servers run");
    serverA.stop(); aclA.end();
    testOk(SharedAcl::setMode("allow-all", error) == AclChange::Locked,
           "...and while the other one still runs");
    serverB.stop(); aclB.end();
    testOk(aclA.accepted() == 2 && aclA.denied() == 1 && aclB.accepted() == 2 && aclB.denied() == 1,
           "per-service counters: mono %lu/%lu, sim %lu/%lu", aclA.accepted(), aclA.denied(),
           aclB.accepted(), aclB.denied());
    testOk1(SharedAcl::reset());
}

} // namespace

MAIN(testAccessControl) {
    testPlan(0);
    osiSockAttach();
    testParser();
    testModeNames();
    testStrategies();
    testConfig();
    testChain();
    testShared();
    testLoopback();
    testTwoServers();
    osiSockRelease();
    return testDone();
}
