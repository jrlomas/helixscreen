// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mock_persona.h"

#include <algorithm>
#include <vector>

namespace helix::test {

/// Every distinct PrinterType some mock persona uses, in persona-table order,
/// so a loop over "all printer types" covers a new persona without an edit.
inline std::vector<helix::mock::PrinterType> persona_printer_types() {
    std::vector<helix::mock::PrinterType> types;
    for (const auto& p : helix::mock::PERSONAS) {
        if (std::find(types.begin(), types.end(), p.type) == types.end()) {
            types.push_back(p.type);
        }
    }
    return types;
}

} // namespace helix::test
