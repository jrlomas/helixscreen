// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_boot_crash_guard.cpp
 * @brief A run of crash resets stops boot from reconnecting to the printer behind them.
 */

#include "boot_crash_guard.h"

#include "../catch_amalgamated.hpp"

using helix::BOOT_CRASH_FALLBACK_THRESHOLD;
using helix::choose_boot_printer;

namespace {
const std::vector<std::string> kPrinters = {"voron", "ender"};
}

TEST_CASE("A deliberate restart or power-on leaves the crash streak alone", "[boot_crash_guard]") {
    const auto choice = choose_boot_printer(false, BOOT_CRASH_FALLBACK_THRESHOLD - 1, false,
                                            "ender", "voron", kPrinters);
    CHECK(choice.crash_streak == BOOT_CRASH_FALLBACK_THRESHOLD - 1);
    CHECK(choice.fallback_id.empty());
    CHECK(choice.auto_connect);
}

TEST_CASE("A crash below the threshold counts and still connects", "[boot_crash_guard]") {
    const auto choice = choose_boot_printer(true, 0, false, "ender", "voron", kPrinters);
    CHECK(choice.crash_streak == 1);
    CHECK(choice.fallback_id.empty());
    CHECK(choice.auto_connect);

    const auto second = choose_boot_printer(true, BOOT_CRASH_FALLBACK_THRESHOLD - 2, false, "ender",
                                            "voron", kPrinters);
    CHECK(second.crash_streak == BOOT_CRASH_FALLBACK_THRESHOLD - 1);
    CHECK(second.fallback_id.empty());
    CHECK(second.auto_connect);
}

TEST_CASE("The threshold crash falls back to the previous printer", "[boot_crash_guard]") {
    const auto choice = choose_boot_printer(true, BOOT_CRASH_FALLBACK_THRESHOLD - 1, false, "ender",
                                            "voron", kPrinters);
    CHECK(choice.crash_streak == 0);
    CHECK(choice.fallback_id == "voron");
    CHECK(choice.auto_connect);
}

TEST_CASE("With no printer to go back to, the threshold crash stops auto-connect",
          "[boot_crash_guard]") {
    const int at = BOOT_CRASH_FALLBACK_THRESHOLD - 1;

    SECTION("no previous printer") {
        const auto choice = choose_boot_printer(true, at, false, "ender", "", kPrinters);
        CHECK(choice.fallback_id.empty());
        CHECK_FALSE(choice.auto_connect);
        CHECK(choice.crash_streak == 0);
    }
    SECTION("previous printer was removed") {
        const auto choice = choose_boot_printer(true, at, false, "ender", "prusa", kPrinters);
        CHECK(choice.fallback_id.empty());
        CHECK_FALSE(choice.auto_connect);
    }
    SECTION("previous printer is the one crashing") {
        const auto choice = choose_boot_printer(true, at, false, "ender", "ender", kPrinters);
        CHECK(choice.fallback_id.empty());
        CHECK_FALSE(choice.auto_connect);
    }
}

TEST_CASE("A streak past the threshold still falls back", "[boot_crash_guard]") {
    const auto choice = choose_boot_printer(true, BOOT_CRASH_FALLBACK_THRESHOLD + 4, false, "ender",
                                            "voron", kPrinters);
    CHECK(choice.fallback_id == "voron");
    CHECK(choice.crash_streak == 0);
}

TEST_CASE("The threshold crash marks the boot as tripped", "[boot_crash_guard]") {
    CHECK_FALSE(choose_boot_printer(true, 0, false, "ender", "voron", kPrinters).tripped);
    CHECK(choose_boot_printer(true, BOOT_CRASH_FALLBACK_THRESHOLD - 1, false, "ender", "voron",
                              kPrinters)
              .tripped);
    CHECK(
        choose_boot_printer(true, BOOT_CRASH_FALLBACK_THRESHOLD - 1, false, "ender", "", kPrinters)
            .tripped);
}

TEST_CASE("A held connection survives a non-crash reboot", "[boot_crash_guard]") {
    const auto tripped =
        choose_boot_printer(true, BOOT_CRASH_FALLBACK_THRESHOLD - 1, false, "ender", "", kPrinters);
    REQUIRE_FALSE(tripped.auto_connect);

    // The next boot is a power-cycle, with the hold read back from config.
    const auto power_cycle = choose_boot_printer(false, tripped.crash_streak, !tripped.auto_connect,
                                                 "ender", "", kPrinters);
    CHECK_FALSE(power_cycle.auto_connect);
    CHECK_FALSE(power_cycle.tripped);
    CHECK(power_cycle.crash_streak == 0);
}

TEST_CASE("A crash below the threshold keeps a held connection held", "[boot_crash_guard]") {
    const auto choice = choose_boot_printer(true, 0, true, "ender", "", kPrinters);
    CHECK_FALSE(choice.auto_connect);
    CHECK(choice.crash_streak == 1);
}

TEST_CASE("Falling back to the previous printer releases a held connection", "[boot_crash_guard]") {
    const auto choice = choose_boot_printer(true, BOOT_CRASH_FALLBACK_THRESHOLD - 1, true, "ender",
                                            "voron", kPrinters);
    CHECK(choice.fallback_id == "voron");
    CHECK(choice.auto_connect);
}
