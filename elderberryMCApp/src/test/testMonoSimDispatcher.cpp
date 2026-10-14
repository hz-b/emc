#include "mono_sim_dispatcher.h"

#include <string>
#include <epicsUnitTest.h>
#include <testMain.h>

namespace {
void reply(MonoSimDispatcher& dispatcher, const char* command, const char* expected) {
    const std::string actual = dispatcher.processLine(command);
    testOk(actual == expected, "%s -> '%s' (expected '%s')",
           command ? command : "<null>", actual.c_str(), expected);
}

void testParsing() {
    testDiag("strict command parsing and protocol formatting");
    double now = 0.0;
    MonoSimDispatcher dispatcher([&now] { return now; });
    reply(dispatcher, "GST", "t 0");
    reply(dispatcher, "GPE", "t 1000.000000");
    reply(dispatcher, "GPO", "t 1.239842");
    reply(dispatcher, "SGS", "t 1000.000000");
    reply(dispatcher, "SGE", "t 1000.000000");
    reply(dispatcher, "SGV", "t 1.000000");
    const char* invalid[] = {
        nullptr, "", " \t\r\n", "# comment", "unknown", "GST 1", "GPE junk", "GPO 2",
        "SGS 1", "SGE 1", "SGV 1", "STO 1", "SI 1", "SR 1", "SPE", "SPO", "SSS",
        "SSE", "SSV", "SPE 0", "SPO -1", "SSS -1", "SSE 0", "SSV 0", "SSV -2",
        "SPE nan", "SPE inf", "SPE 1e999", "SPO 1e-310", "SSS nan", "SSE inf",
        "SSV nan", "SSV 1e999", "SPE 1001junk", "SPO 1 2", "SSV 2 extra"
    };
    for (const char* command : invalid) reply(dispatcher, command, "f");
    reply(dispatcher, "GPE", "t 1000.000000");
    reply(dispatcher, "SGV", "t 1.000000");
    reply(dispatcher, "  ssv 2.5 \r\n", "t");
    reply(dispatcher, "sgv", "t 2.500000");
    reply(dispatcher, "STO", "t");
}

void testMotionAndStop() {
    testDiag("target commands, malformed commands and stop during motion");
    double now = 0.0;
    MonoSimDispatcher dispatcher([&now] { return now; });
    reply(dispatcher, "SPE 1004", "t");
    reply(dispatcher, "GST", "t 1");
    now = 1.5;
    reply(dispatcher, "GPE", "t 1001.500000");
    reply(dispatcher, "SPE 999 extra", "f");
    now = 2.0;
    reply(dispatcher, "GPE", "t 1002.000000");
    reply(dispatcher, "SSV 2", "f");
    reply(dispatcher, "STO", "t");
    reply(dispatcher, "GST", "t 0");
    now = 100.0;
    reply(dispatcher, "GPE", "t 1002.000000");
    reply(dispatcher, "SSV 2", "t");
    reply(dispatcher, "SPE 996", "t");
    now = 101.0;
    reply(dispatcher, "GPE", "t 1000.000000");
    reply(dispatcher, "SPE 1004", "t");
    now = 102.0;
    reply(dispatcher, "GPE", "t 1002.000000");
    now = 1000.0;
    reply(dispatcher, "GST", "t 0");
    reply(dispatcher, "GPE", "t 1004.000000");
    reply(dispatcher, "SPE 1004", "t");
    reply(dispatcher, "GST", "t 0");
}

void testSweepsAndRetarget() {
    testDiag("continuous-mode commands and replacing a sweep with SPE");
    double now = 0.0;
    MonoSimDispatcher dispatcher([&now] { return now; });
    reply(dispatcher, "SR", "f");
    reply(dispatcher, "SSS 1002", "t");
    reply(dispatcher, "SSE 1008", "t");
    reply(dispatcher, "SSV 2", "t");
    reply(dispatcher, "SGS", "t 1002.000000");
    reply(dispatcher, "SGE", "t 1008.000000");
    reply(dispatcher, "SGV", "t 2.000000");
    reply(dispatcher, "SI", "t");
    reply(dispatcher, "GST", "t 1");
    reply(dispatcher, "SR", "f");
    reply(dispatcher, "SI", "f");
    reply(dispatcher, "SSS 500", "f");
    reply(dispatcher, "SSE 1500", "f");
    now = 1.0;
    reply(dispatcher, "GST", "t 0");
    reply(dispatcher, "GPE", "t 1002.000000");
    reply(dispatcher, "SR", "t");
    reply(dispatcher, "GST", "t 1");
    reply(dispatcher, "SR", "f");
    now = 2.0;
    reply(dispatcher, "GPE", "t 1004.000000");
    now = 4.0;
    reply(dispatcher, "GST", "t 0");
    reply(dispatcher, "GPE", "t 1008.000000");
    reply(dispatcher, "SR", "f");
    reply(dispatcher, "SI", "t");
    now = 7.0;
    reply(dispatcher, "SR", "t");
    now = 8.0;
    reply(dispatcher, "SPE 1000", "t");
    now = 9.0;
    reply(dispatcher, "GPE", "t 1002.000000");
    now = 10.0;
    reply(dispatcher, "GST", "t 0");
    reply(dispatcher, "GPE", "t 1000.000000");
    reply(dispatcher, "SR", "f");
}

void testWavelengthAndIsolation() {
    testDiag("wavelength targets and independent dispatcher instances");
    double now = 0.0;
    MonoSimDispatcher first([&now] { return now; });
    MonoSimDispatcher second([&now] { return now; });
    reply(first, "SPO 0.6199209921660013", "t");
    reply(first, "GST", "t 1");
    now = 500.0;
    reply(first, "GPE", "t 1500.000000");
    reply(first, "GPO", "t 0.826561");
    reply(second, "GPE", "t 1000.000000");
    reply(second, "GST", "t 0");
    now = 1000.0;
    reply(first, "GPE", "t 2000.000000");
    reply(first, "GPO", "t 0.619921");
    reply(first, "GST", "t 0");
    MonoSimDispatcher::setDebugLevel(2);
    testOk1(MonoSimDispatcher::debugLevel() == 2);
    MonoSimDispatcher::setDebugLevel(0);
}
} // namespace

MAIN(testMonoSimDispatcher) {
    testPlan(0);
    testParsing();
    testMotionAndStop();
    testSweepsAndRetarget();
    testWavelengthAndIsolation();
    return testDone();
}
