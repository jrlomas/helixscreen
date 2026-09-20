// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_batch_filament_modal.cpp
 * @brief Pure-logic tests for the batch filament picker modal
 *
 * The widget plumbing is LVGL-driven and covered by driving the mock; these
 * cases pin the two decisions that must hold regardless of the screen.
 */

#include "ui_batch_filament_modal.h"

#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::BatchFilamentModal;

TEST_CASE("BatchFilamentModal prefill ticks by direction", "[ams][batch]") {
    using o = std::optional<bool>;
    const std::vector<std::optional<bool>> presence{o(true), o(false), o(), o(true)};

    SECTION("unload ticks only heads with filament at the toolhead") {
        REQUIRE(BatchFilamentModal::prefill_selection(presence, false) ==
                std::vector<bool>{true, false, false, true});
    }
    SECTION("load ticks the rest; an unpublished presence reads as loadable") {
        REQUIRE(BatchFilamentModal::prefill_selection(presence, true) ==
                std::vector<bool>{false, true, true, false});
    }
    SECTION("no slots yields no ticks") {
        REQUIRE(BatchFilamentModal::prefill_selection({}, true).empty());
        REQUIRE(BatchFilamentModal::prefill_selection({}, false).empty());
    }
}

TEST_CASE("BatchFilamentModal selected keys convert to slot indices", "[ams][batch]") {
    SECTION("keys in row order become slots") {
        REQUIRE(BatchFilamentModal::selected_slots({"0", "2", "3"}) == std::vector<int>{0, 2, 3});
    }
    SECTION("single slot") {
        REQUIRE(BatchFilamentModal::selected_slots({"1"}) == std::vector<int>{1});
    }
    SECTION("empty selection stays empty") {
        REQUIRE(BatchFilamentModal::selected_slots({}).empty());
    }
}
