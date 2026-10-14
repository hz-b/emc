/**
 * @file  testReplyTerminator.cpp
 * @brief Tests for line endings: the per-server reply terminator (lf, crlf,
 *        cr, none) and request lines ending in LF, CR or CRLF.
 *
 * Run with "make runtests" or directly. Binds TCP port EMC_TEST_PORT + 20
 * (default 15751) on loopback; the socket part is skipped if it cannot bind.
 */

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <epicsThread.h>
#include <epicsUnitTest.h>
#include <osiSock.h>
#include <testMain.h>

#include "connection_filter.h"
#include "echo_dispatcher.h"
#include "emc_server.h"

namespace {

void testParse() {
    testDiag("parseReplyTerminator / names / bytes");
    struct Case { const char* text; ReplyTerminator t; };
    const Case good[] = {
        {"lf", ReplyTerminator::LF},     {"LF", ReplyTerminator::LF},
        {"nl", ReplyTerminator::LF},     {"newline", ReplyTerminator::LF},
        {"\\n", ReplyTerminator::LF},    {"\n", ReplyTerminator::LF},
        {"crlf", ReplyTerminator::CRLF}, {"CrLf", ReplyTerminator::CRLF},
        {"\\r\\n", ReplyTerminator::CRLF}, {"\r\n", ReplyTerminator::CRLF},
        {"cr", ReplyTerminator::CR},     {"\\r", ReplyTerminator::CR},
        {"none", ReplyTerminator::None}, {"NONE", ReplyTerminator::None},
        {"off", ReplyTerminator::None},  {"empty", ReplyTerminator::None},
    };
    for (const Case& c : good) {
        ReplyTerminator t = c.t == ReplyTerminator::None ? ReplyTerminator::LF
                                                         : ReplyTerminator::None;
        testOk(parseReplyTerminator(c.text, t) && t == c.t, "parses '%s' as %s",
               c.text, replyTerminatorName(c.t));
    }
    const char* bad[] = {"", "l f", "lfcr", "\\t", "0", "crlf ", "yes"};
    for (const char* text : bad) {
        ReplyTerminator t = ReplyTerminator::CR;
        testOk(!parseReplyTerminator(text, t) && t == ReplyTerminator::CR,
               "rejects '%s', output unchanged", text);
    }
    ReplyTerminator t = ReplyTerminator::CR;
    testOk(!parseReplyTerminator(nullptr, t) && t == ReplyTerminator::CR, "rejects null");

    testOk1(std::string(replyTerminatorBytes(ReplyTerminator::LF)) == "\n");
    testOk1(std::string(replyTerminatorBytes(ReplyTerminator::CRLF)) == "\r\n");
    testOk1(std::string(replyTerminatorBytes(ReplyTerminator::CR)) == "\r");
    testOk1(std::string(replyTerminatorBytes(ReplyTerminator::None)).empty());
    // Every name parses back to itself.
    const ReplyTerminator all[] = {ReplyTerminator::LF, ReplyTerminator::CRLF,
                                   ReplyTerminator::CR, ReplyTerminator::None};
    bool roundTrip = true;
    for (ReplyTerminator x : all) {
        ReplyTerminator y = ReplyTerminator::LF;
        roundTrip = roundTrip && parseReplyTerminator(replyTerminatorName(x), y) && y == x;
    }
    testOk(roundTrip, "names round-trip");
}

// Blocking loopback client with a short receive timeout.
struct Client {
    SOCKET s = INVALID_SOCKET;
    ~Client() { if (s != INVALID_SOCKET) epicsSocketDestroy(s); }

    bool connectTo(unsigned short port) {
        s = epicsSocketCreate(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET) return false;
        struct timeval tv;
        tv.tv_sec = 0; tv.tv_usec = 400000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv));
        sockaddr_in srv{};
        srv.sin_family = AF_INET;
        srv.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        srv.sin_port = htons(port);
        return connect(s, (sockaddr*)&srv, sizeof(srv)) == 0;
    }
    bool send(const std::string& text) {
        return ::send(s, text.data(), text.size(), 0) == (ssize_t)text.size();
    }
    // Everything that arrives until the receive timeout: a reply without a
    // terminator has no end marker, so a timeout is the only way to know.
    std::string readAll() {
        std::string got;
        char buf[256];
        for (;;) {
            const ssize_t n = recv(s, buf, sizeof(buf), 0);
            if (n <= 0) break;
            got.append(buf, (size_t)n);
        }
        return got;
    }
};

std::string visible(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out += c;
    }
    return out;
}

// Feeds @p chunks one after the other and returns every complete line,
// each followed by '|', plus "+<rest>" for an incomplete line left over.
std::string split(const std::vector<std::string>& chunks) {
    RequestLineSplitter s;
    std::string out, line;
    for (const std::string& chunk : chunks) {
        s.append(chunk.data(), chunk.size());
        while (s.next(line)) out += line + "|";
    }
    if (s.pending() > 0) out += "+" + std::to_string(s.pending());
    return out;
}

void testSplitter() {
    testDiag("RequestLineSplitter: LF, CR and CRLF end a request");
    struct Case { std::vector<std::string> chunks; const char* expected; const char* what; };
    const Case cases[] = {
        {{"OPN\n"},                     "OPN|",          "LF"},
        {{"OPN\r"},                     "OPN|",          "CR (old VxWorks clients)"},
        {{"OPN\r\n"},                   "OPN|",          "CRLF is one terminator"},
        {{"A\nB\rC\r\nD\n"},            "A|B|C|D|",      "mixed endings in one read"},
        {{"OPN\r", "\nCLO\n"},          "OPN|CLO|",      "CRLF split across two reads"},
        {{"OPN\r", "", "\n", "CLO\r"},  "OPN|CLO|",      "CRLF split with an empty read between"},
        {{"OPN\r", "CLO\r"},            "OPN|CLO|",      "CR, then a new line in the next read"},
        {{"SPE 4", "00", "\r"},         "SPE 400|",      "request split over three reads"},
        {{"GPE"},                       "+3",            "no terminator yet: kept, not dispatched"},
        {{"GPE", "\r"},                 "GPE|",          "...completed by a later CR"},
        {{"\n\n"},                      "||",            "two LFs are two empty lines"},
        {{"\r\r\n"},                    "||",            "CR, CRLF: two empty lines"},
        {{"A\n\rB\n"},                  "A||B|",         "LF CR is two terminators"},
    };
    for (const Case& c : cases) {
        const std::string got = split(c.chunks);
        testOk(got == c.expected, "%s: '%s'", c.what, visible(got).c_str());
    }
    // One byte per read.
    const std::string stream = "OPN\r\nCLO\rGPE\nSPE 1\r\n";
    std::vector<std::string> bytes;
    for (char ch : stream) bytes.push_back(std::string(1, ch));
    const std::string got = split(bytes);
    testOk(got == "OPN|CLO|GPE|SPE 1|", "one byte per read: '%s'", visible(got).c_str());
}

void testServer() {
    testDiag("EmcServer reply terminator over loopback");
    const char* env = std::getenv("EMC_TEST_PORT");
    const unsigned short port = (unsigned short)((env ? std::atoi(env) : 15731) + 20);

    EchoDispatcher echo;
    FilterChain open;   // empty chain: accepts everyone (test only)
    EmcServer server(echo, open, port);
    testOk(server.replyTerminator() == ReplyTerminator::LF, "default terminator is lf");

    server.setReplyTerminator(ReplyTerminator::None);   // set before start
    if (!server.start()) {
        testSkip(15, "cannot bind the test port (set EMC_TEST_PORT)");
        return;
    }
    {
        Client c;
        testOk1(c.connectTo(port));
        c.send("OPN\n");
        std::string got = c.readAll();
        testOk(got == "OPN", "none: bare reply '%s'", visible(got).c_str());
        c.send("CLO\r\nping\n");   // CRLF requests still accepted
        got = c.readAll();
        testOk(got == "CLOOK", "none: two replies back to back '%s'", visible(got).c_str());

        // Change while running, same connection.
        server.setReplyTerminator(ReplyTerminator::CRLF);
        c.send("OPN\n");
        got = c.readAll();
        testOk(got == "OPN\r\n", "crlf applied while running: '%s'", visible(got).c_str());

        server.setReplyTerminator(ReplyTerminator::CR);
        c.send("OPN\n");
        got = c.readAll();
        testOk(got == "OPN\r", "cr: '%s'", visible(got).c_str());

        server.setReplyTerminator(ReplyTerminator::LF);
        c.send("OPN\nCLO\n");
        got = c.readAll();
        testOk(got == "OPN\nCLO\n", "lf: '%s'", visible(got).c_str());
    }
    testDiag("request line endings over loopback");
    {
        Client c;
        testOk1(c.connectTo(port));
        c.send("OPN\r");
        std::string got = c.readAll();
        testOk(got == "OPN\n", "CR-terminated request answered: '%s'", visible(got).c_str());

        c.send("A\rB\nC\r\nD\n");
        got = c.readAll();
        testOk(got == "A\nB\nC\nD\n", "CR, LF, CRLF mixed: one reply each: '%s'",
               visible(got).c_str());

        c.send("OPN\r");
        epicsThreadSleep(0.05);   // the LF arrives in a separate read
        c.send("\nCLO\n");
        got = c.readAll();
        testOk(got == "OPN\nCLO\n", "CRLF split across reads, one reply: '%s'",
               visible(got).c_str());

        c.send("\r\n   \r\n\r\rGPE\r");
        got = c.readAll();
        testOk(got == "GPE\n", "blank lines get no reply: '%s'", visible(got).c_str());

        // Old VxWorks client: CR out, CR back.
        server.setReplyTerminator(ReplyTerminator::CR);
        c.send("OPN\r");
        got = c.readAll();
        testOk(got == "OPN\r", "old client style, CR both ways: '%s'", visible(got).c_str());
        c.send("GPE\rCLO\r");
        got = c.readAll();
        testOk(got == "GPE\rCLO\r", "two CR requests in one packet: '%s'", visible(got).c_str());
    }
    // The setting survives stop()/start().
    server.setReplyTerminator(ReplyTerminator::None);
    server.stop();
    testOk(server.replyTerminator() == ReplyTerminator::None, "kept after stop()");
    if (!server.start()) {
        testSkip(1, "cannot rebind the test port");
        return;
    }
    {
        Client c;
        c.connectTo(port);
        c.send("GPE\n");
        const std::string got = c.readAll();
        testOk(got == "GPE", "none after restart: '%s'", visible(got).c_str());
    }
    server.stop();
}

} // namespace

MAIN(testReplyTerminator) {
    testPlan(0);
    osiSockAttach();
    testParse();
    testSplitter();
    testServer();
    osiSockRelease();
    return testDone();
}
