// SPDX-FileCopyrightText: 2026 Helmholtz-Zentrum Berlin für Materialien und Energie GmbH
// SPDX-License-Identifier: MIT

#ifndef MONO_SIM_DISPATCHER_H
#define MONO_SIM_DISPATCHER_H

#include "dispatcher.h"
#include "mono_sim_model.h"
#include <functional>

// The server's WorkerLoop serializes calls. All clients share this model.
class MonoSimDispatcher : public Dispatcher {
public:
    using Clock = std::function<double()>;

    // Default: steady_clock seconds. A supplied clock makes tests deterministic.
    explicit MonoSimDispatcher(Clock clock = Clock());
    std::string processLine(const char* line) override;

    // Shared, atomic debug level: IOC shell may change it while the server runs.
    static void setDebugLevel(int level);
    static int debugLevel();

private:
    std::string execute(const char* line);
    Clock clock_;
    MonoSimModel model_;
};

#endif // MONO_SIM_DISPATCHER_H
