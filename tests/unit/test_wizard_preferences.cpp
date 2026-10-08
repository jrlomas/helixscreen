// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_wizard_preferences.h"

#include "../../src/ui/wizard_step_registry.h"
#include "../lvgl_ui_test_fixture.h"
#include "display_settings_manager.h"
#include "settings_manager.h"

#include <lvgl/lvgl.h>

#include "../catch_amalgamated.hpp"

using helix::wizard::preference_rows;
using helix::wizard::StepId;

TEST_CASE("Preference rows: global rows only on the first printer", "[wizard][preferences]") {
    CHECK(preference_rows(false, false, false, false).global);
    CHECK_FALSE(preference_rows(true, false, false, false).global);
}

TEST_CASE("Preference rows: AMS rows follow what the backend does", "[wizard][preferences]") {
    CHECK_FALSE(preference_rows(false, false, false, false).bypass_spool);
    CHECK(preference_rows(false, true, false, false).bypass_spool);

    CHECK_FALSE(preference_rows(false, false, false, false).keep_spool_info);
    CHECK(preference_rows(false, false, true, false).keep_spool_info);
    // Firmware that keeps the spool itself leaves the toggle with nothing to do.
    CHECK_FALSE(preference_rows(false, false, true, true).keep_spool_info);
}

TEST_CASE("Preferences step sits between InputShaper and Summary", "[wizard][preferences]") {
    CHECK(static_cast<int>(StepId::Preferences) == static_cast<int>(StepId::InputShaper) + 1);
    CHECK(static_cast<int>(StepId::Summary) == static_cast<int>(StepId::Preferences) + 1);

    helix::wizard::Step* step = helix::wizard::step_by_id(StepId::Preferences);
    REQUIRE(step != nullptr);
    CHECK(step->id() == StepId::Preferences);

    helix::wizard::StepContext first_run;
    first_run.preset = {true, true};
    CHECK_FALSE(step->should_skip(first_run));
}

TEST_CASE_METHOD(LVGLUITestFixture, "Preference rows write the same setting as their overlay twin",
                 "[wizard][preferences]") {
    auto& settings = SettingsManager::instance();
    settings.init_subjects();

    auto* step = helix::wizard::get_wizard_preferences_step();
    step->register_callbacks();
    lv_obj_t* root = step->create(test_screen());
    REQUIRE(root != nullptr);

    // Each row starts unchecked (setting off) and must turn its setting on.
    auto flip_on = [&](const char* row_name, auto set, auto get) {
        set(false);
        REQUIRE_FALSE(get());
        lv_obj_t* row = lv_obj_find_by_name(root, row_name);
        REQUIRE(row != nullptr);
        lv_obj_t* sw = lv_obj_find_by_name(row, "toggle");
        REQUIRE(sw != nullptr);
        lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, nullptr);
        CHECK(get());
    };

    flip_on(
        "row_filament_auto_open_editor", [&](bool v) { settings.set_filament_auto_open_editor(v); },
        [&] { return settings.get_filament_auto_open_editor(); });
    flip_on(
        "row_filament_auto_cooldown", [&](bool v) { settings.set_filament_auto_cooldown(v); },
        [&] { return settings.get_filament_auto_cooldown(); });
    flip_on(
        "row_ams_always_show_bypass_spool",
        [&](bool v) { settings.set_ams_always_show_bypass_spool(v); },
        [&] { return settings.get_ams_always_show_bypass_spool(); });
    flip_on(
        "row_ams_keep_spool_info_on_eject",
        [&](bool v) { settings.set_ams_keep_spool_info_on_eject(v); },
        [&] { return settings.get_ams_keep_spool_info_on_eject(); });
    flip_on(
        "row_widget_labels", [&](bool v) { settings.set_show_widget_labels(v); },
        [&] { return settings.get_show_widget_labels(); });
    auto& display = helix::DisplaySettingsManager::instance();
    flip_on(
        "row_sleep_while_printing", [&](bool v) { display.set_sleep_while_printing(v); },
        [&] { return display.get_sleep_while_printing(); });

    step->cleanup();
}
