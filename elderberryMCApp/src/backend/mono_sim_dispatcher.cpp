// SPDX-FileCopyrightText: 2026 Helmholtz-Zentrum Berlin für Materialien und Energie GmbH
// SPDX-License-Identifier: MIT

#include "mono_sim_dispatcher.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <locale>
#include <sstream>
#include <utility>

#define Debug(L, ...) do { if ((L) <= MonoSimDispatcher::debugLevel()) { \
    std::fprintf(stderr, "[MonoSim] "); \
    std::fprintf(stderr, __VA_ARGS__); \
    std::fprintf(stderr, "\t- file: %s, line %d\n", __FILE__, __LINE__); \
} } while (false)

namespace {
std::atomic<int> simulation_debug(0);

double steadyNow() {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

const char* stateName(MonoSimModel::State state) {
    switch (state) {
    case MonoSimModel::State::Idle: return "idle";
    case MonoSimModel::State::MovingToTarget: return "moving-to-target";
    case MonoSimModel::State::MovingToStart: return "moving-to-start";
    case MonoSimModel::State::SweepReady: return "sweep-ready";
    case MonoSimModel::State::Sweeping: return "sweeping";
    }
    return "unknown";
}

std::string numericReply(double value) {
    std::ostringstream reply;
    reply.imbue(std::locale::classic());
    reply << "t " << std::fixed << std::setprecision(6) << value;
    return reply.str();
}
} // namespace

MonoSimDispatcher::MonoSimDispatcher(Clock clock)
    : clock_(clock ? std::move(clock) : Clock(steadyNow)) {}

void MonoSimDispatcher::setDebugLevel(int level) {
    simulation_debug.store(level < 0 ? 0 : level, std::memory_order_relaxed);
}

int MonoSimDispatcher::debugLevel() {
    return simulation_debug.load(std::memory_order_relaxed);
}

std::string MonoSimDispatcher::processLine(const char* line) {
    const MonoSimModel::State before = model_.state();
    model_.advance(clock_());
    std::string reply = execute(line);
    if (before != model_.state()) {
        Debug(1, "command='%s' state=%s -> %s energy=%.9g target=%.9g velocity=%.9g",
              line ? line : "<null>", stateName(before), stateName(model_.state()),
              model_.energy(), model_.target(), model_.velocity());
    }
    if (reply == "f") {
        Debug(1, "rejected command='%s' state=%s energy=%.9g",
              line ? line : "<null>", stateName(model_.state()), model_.energy());
    }
    Debug(2, "command='%s' reply='%s' energy=%.9g target=%.9g velocity=%.9g state=%s",
          line ? line : "<null>", reply.c_str(), model_.energy(), model_.target(),
          model_.velocity(), stateName(model_.state()));
    return reply;
}

std::string MonoSimDispatcher::execute(const char* line) {
    std::istringstream input(line ? line : "");
    input.imbue(std::locale::classic());
    std::string command, extra;
    if (!(input >> command)) return "f";
    for (char& c : command) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }

    const bool setter = command == "SPE" || command == "SPO" || command == "SSS" ||
                        command == "SSE" || command == "SSV";
    double value = 0.0;
    if (setter && (!(input >> value) || !std::isfinite(value) || value <= 0.0))
        return "f";
    if (input >> extra) return "f";

    if (setter) {
        bool accepted = false;
        if (command == "SPE") accepted = model_.setEnergy(value);
        else if (command == "SPO") accepted = model_.setWavelength(value);
        else if (command == "SSS") accepted = model_.setSweepStart(value);
        else if (command == "SSE") accepted = model_.setSweepEnd(value);
        else if (command == "SSV") accepted = model_.setVelocity(value);
        if (accepted) {
            Debug(1, "command=%s value=%.9g energy=%.9g target=%.9g velocity=%.9g",
                  command.c_str(), value, model_.energy(), model_.target(), model_.velocity());
        }
        return accepted ? "t" : "f";
    }
    if (command == "SI") return model_.sweepInit() ? "t" : "f";
    if (command == "SR") return model_.sweepRun() ? "t" : "f";
    if (command == "STO") {
        model_.stop();
        return "t";
    }
    if (command == "GST") return model_.status() ? "t 1" : "t 0";
    if (command == "GPE") return numericReply(model_.energy());
    if (command == "GPO") return numericReply(model_.wavelength());
    if (command == "SGS") return numericReply(model_.sweepStart());
    if (command == "SGE") return numericReply(model_.sweepEnd());
    if (command == "SGV") return numericReply(model_.velocity());
    return "f";
}
