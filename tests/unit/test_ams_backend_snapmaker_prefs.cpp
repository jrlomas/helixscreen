// tests/unit/test_ams_backend_snapmaker_prefs.cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../helix_test_fixture.h"
#include "../test_helpers/snapmaker_test_access.h"
#include "ams_backend_snapmaker.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;

namespace {
/// An unwrapped status object carrying only print_task_config — the shape the
/// initial query response sends and handle_status_update accepts directly.
nlohmann::json frame(const nlohmann::json& ptc_fields) {
    nlohmann::json params = nlohmann::json::object();
    params["print_task_config"] = ptc_fields;
    return params;
}
} // namespace

TEST_CASE("snapmaker backend keeps the preferences it is told about", "[ams][snapmaker][prefs]") {
    HelixTestFixture fixture;
    AmsBackendSnapmaker backend(nullptr, nullptr);
    SnapmakerTestAccess::handle_status(
        backend, frame({{"end_led_turn_off", true}, {"filament_entangle_sen", "low"}}));
    REQUIRE(backend.print_preferences().end_led_turn_off.value() == true);
    REQUIRE(backend.print_preferences().filament_entangle_sen.value() == "low");
}

TEST_CASE("a later frame that omits a preference does not clear it", "[ams][snapmaker][prefs]") {
    // The delta trap: a frame carrying only filament colours says nothing about
    // the preferences, and replacing rather than merging would switch them off.
    HelixTestFixture fixture;
    AmsBackendSnapmaker backend(nullptr, nullptr);
    SnapmakerTestAccess::handle_status(backend, frame({{"end_led_turn_off", true}}));
    SnapmakerTestAccess::handle_status(backend, frame({{"filament_type", {"PLA"}}}));
    REQUIRE(backend.print_preferences().end_led_turn_off.value() == true);
}

TEST_CASE("a later frame that changes a preference wins", "[ams][snapmaker][prefs]") {
    HelixTestFixture fixture;
    AmsBackendSnapmaker backend(nullptr, nullptr);
    SnapmakerTestAccess::handle_status(backend, frame({{"end_led_turn_off", true}}));
    SnapmakerTestAccess::handle_status(backend, frame({{"end_led_turn_off", false}}));
    REQUIRE(backend.print_preferences().end_led_turn_off.value() == false);
}

TEST_CASE("a backend that has seen no preference field reports an empty set",
          "[ams][snapmaker][prefs]") {
    HelixTestFixture fixture;
    AmsBackendSnapmaker backend(nullptr, nullptr);
    SnapmakerTestAccess::handle_status(backend, frame({{"filament_type", {"PLA"}}}));
    REQUIRE(backend.print_preferences().empty());
}

TEST_CASE("one preference field is enough to make the held set non-empty",
          "[ams][snapmaker][prefs]") {
    HelixTestFixture fixture;
    AmsBackendSnapmaker backend(nullptr, nullptr);
    SnapmakerTestAccess::handle_status(backend, frame({{"auto_replenish_filament", false}}));
    REQUIRE_FALSE(backend.print_preferences().empty());
}
