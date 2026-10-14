// SPDX-FileCopyrightText: 2026 Helmholtz-Zentrum Berlin für Materialien und Energie GmbH
// SPDX-License-Identifier: MIT

#ifndef DISPATCHER_H
#define DISPATCHER_H
#include <string>

struct Dispatcher {
    virtual ~Dispatcher() = default;
    virtual std::string processLine(const char* line) = 0;
};
#endif // DISPATCHER_H
