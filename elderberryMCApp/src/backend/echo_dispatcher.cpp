// SPDX-FileCopyrightText: 2026 Helmholtz-Zentrum Berlin für Materialien und Energie GmbH
// SPDX-License-Identifier: MIT

#include "echo_dispatcher.h"

std::string EchoDispatcher::processLine(const char* line) {
    return line ? line : "";
}
