// SPDX-FileCopyrightText: 2026 Helmholtz-Zentrum Berlin für Materialien und Energie GmbH
// SPDX-License-Identifier: MIT

#ifndef ECHO_DISPATCHER_H
#define ECHO_DISPATCHER_H

#include <string>
#include "dispatcher.h"

// Test support stub dispatcher 
class EchoDispatcher : public Dispatcher {
public:
    std::string processLine(const char* line) override;
};

#endif // ECHO_DISPATCHER_H
