// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// IdlePrebuildGate decides when a deferred panel may be built ahead of its first
// visit: only after the printer has stayed connected for a few ticks, and only
// while nobody is touching the screen.

#include "panel_factory.h"

#include "../catch_amalgamated.hpp"

using helix::IdlePrebuildGate;

namespace {
constexpr uint32_t kQuiet = IdlePrebuildGate::kQuietMs;
} // namespace

TEST_CASE("IdlePrebuildGate opens after the connection settles and input is quiet",
          "[panel_factory][idle_prebuild]") {
    IdlePrebuildGate gate;
    for (int i = 1; i < IdlePrebuildGate::kSettleTicks; ++i) {
        CAPTURE(i);
        REQUIRE_FALSE(gate.tick(true, kQuiet));
    }
    REQUIRE(gate.tick(true, kQuiet));
}

TEST_CASE("IdlePrebuildGate stays shut while disconnected", "[panel_factory][idle_prebuild]") {
    IdlePrebuildGate gate;
    for (int i = 0; i < 10; ++i) {
        REQUIRE_FALSE(gate.tick(false, kQuiet * 10));
    }
}

TEST_CASE("IdlePrebuildGate restarts the settle count after a disconnect",
          "[panel_factory][idle_prebuild]") {
    IdlePrebuildGate gate;
    for (int i = 1; i < IdlePrebuildGate::kSettleTicks; ++i) {
        REQUIRE_FALSE(gate.tick(true, kQuiet));
    }
    REQUIRE_FALSE(gate.tick(false, kQuiet));
    for (int i = 1; i < IdlePrebuildGate::kSettleTicks; ++i) {
        REQUIRE_FALSE(gate.tick(true, kQuiet));
    }
    REQUIRE(gate.tick(true, kQuiet));
}

TEST_CASE("IdlePrebuildGate waits out recent input", "[panel_factory][idle_prebuild]") {
    IdlePrebuildGate gate;
    for (int i = 0; i < IdlePrebuildGate::kSettleTicks + 5; ++i) {
        REQUIRE_FALSE(gate.tick(true, kQuiet - 1));
    }
    REQUIRE(gate.tick(true, kQuiet));
}
