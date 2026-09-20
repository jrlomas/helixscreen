// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/settings_manager.h"
#include "../helix_test_fixture.h"

#include "../catch_amalgamated.hpp"

TEST_CASE_METHOD(HelixTestFixture, "Jog speeds default to today's hardcoded values",
                 "[settings_motion]") {
    auto& s = helix::SettingsManager::instance();
    s.init_subjects();
    REQUIRE(s.get_jog_speed_xy() == 6000);
    REQUIRE(s.get_jog_speed_z() == 600);
}

TEST_CASE_METHOD(HelixTestFixture, "Jog speeds round-trip", "[settings_motion]") {
    auto& s = helix::SettingsManager::instance();
    s.init_subjects();
    s.set_jog_speed_z(1500);
    REQUIRE(s.get_jog_speed_z() == 1500);
}

TEST_CASE_METHOD(HelixTestFixture, "Jog speeds clamp to a sane range", "[settings_motion]") {
    auto& s = helix::SettingsManager::instance();
    s.init_subjects();
    s.set_jog_speed_xy(0);
    REQUIRE(s.get_jog_speed_xy() == 60);
    s.set_jog_speed_xy(999999);
    REQUIRE(s.get_jog_speed_xy() == 60000);
}

#include "../../include/ui_panel_motion.h"

TEST_CASE_METHOD(HelixTestFixture, "Jog distances default to today's table", "[settings_motion]") {
    auto& s = helix::SettingsManager::instance();
    s.init_subjects();
    REQUIRE(s.get_jog_distance(helix::JogMode::Fine, false) == Catch::Approx(0.1f));
    REQUIRE(s.get_jog_distance(helix::JogMode::Fine, true) == Catch::Approx(1.0f));
    REQUIRE(s.get_jog_distance(helix::JogMode::Coarse, false) == Catch::Approx(1.0f));
    REQUIRE(s.get_jog_distance(helix::JogMode::Coarse, true) == Catch::Approx(10.0f));
    REQUIRE(s.get_jog_distance(helix::JogMode::Turbo, false) == Catch::Approx(10.0f));
    REQUIRE(s.get_jog_distance(helix::JogMode::Turbo, true) == Catch::Approx(50.0f));
}

TEST_CASE_METHOD(HelixTestFixture, "Jog distances round-trip and clamp", "[settings_motion]") {
    auto& s = helix::SettingsManager::instance();
    s.init_subjects();
    s.set_jog_distance(helix::JogMode::Turbo, true, 25.0f);
    REQUIRE(s.get_jog_distance(helix::JogMode::Turbo, true) == Catch::Approx(25.0f));
    s.set_jog_distance(helix::JogMode::Turbo, true, 0.0f);
    REQUIRE(s.get_jog_distance(helix::JogMode::Turbo, true) == Catch::Approx(0.01f));
}

TEST_CASE_METHOD(HelixTestFixture, "Reset restores the shipped distances", "[settings_motion]") {
    auto& s = helix::SettingsManager::instance();
    s.init_subjects();
    s.set_jog_distance(helix::JogMode::Fine, false, 5.0f);
    s.reset_jog_distances();
    REQUIRE(s.get_jog_distance(helix::JogMode::Fine, false) == Catch::Approx(0.1f));
}
