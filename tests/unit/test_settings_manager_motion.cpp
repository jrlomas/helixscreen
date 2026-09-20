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
