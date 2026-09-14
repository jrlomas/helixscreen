// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_panel_macro_prefill.cpp
 * @brief The Filament panel hands its Load / Unload / Purge macros the nozzle
 *        temperature it already knows, and the parameter modal carries a prefill
 *        as a value that is sent, not as a hint.
 *
 * Run with: ./build/bin/helix-tests "[filament][prefill]"
 *
 * The temperature is the live extruder target, else the material the panel would
 * preheat for. When the known values cover every parameter the macro takes, the
 * macro runs with no dialog; otherwise the dialog opens with them typed in.
 */

#include "ui_panel_filament.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/filament_panel_test_access.h"
#include "../test_helpers/load_filament_expression_default.h"
#include "ams_state.h"
#include "display_settings_manager.h"
#include "filament_database.h"
#include "filament_op_router.h"
#include "filament_op_slot_resolver.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "macro_param_cache.h"
#include "macro_param_modal.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "preset_materials.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "standard_macros.h"
#include "tool_state.h"

#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using TA = helix::ui::FilamentPanelTestAccess;
using ParamValues = std::map<std::string, std::string>;
using Scripts = std::vector<std::string>;

namespace {

/// A macro that takes a nozzle temperature and one parameter the panel cannot know.
constexpr const char* LOAD_WITH_LENGTH = "{% set t = params.EXTRUDER_TEMP|default(220)|int %}\n"
                                         "{% set l = params.LENGTH|default(100)|float %}\n"
                                         "M109 S{t}\nG1 E{l} F300";

/// A real FilamentPanel with no AMS backend over a mock printer, so every op
/// reaches the configured-macro tier and the gcode it sends is observable.
struct PrefillPanelHarness {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    MoonrakerAPI api{client, state};
    std::unique_ptr<FilamentPanel> panel;

    int prompt_count = 0;
    std::string prompted_macro;
    ParamValues prompted_prefill;

    PrefillPanelHarness() {
        ToolState::instance().init_subjects(true);
        AmsState::instance().init_subjects(true);
        AmsState::instance().clear_backends();
        AmsState::instance().clear_external_spool_info();
        state.init_subjects(false);
        state.init_extruders({"extruder"});
        state.set_klippy_state_sync(helix::KlippyState::READY);

        helix::PrinterDiscovery hardware;
        nlohmann::json objects = {"extruder", "gcode_macro LOAD_FILAMENT",
                                  "gcode_macro UNLOAD_FILAMENT", "gcode_macro PURGE"};
        hardware.parse_objects(objects);
        StandardMacros::instance().reset();
        StandardMacros::instance().init(hardware);

        helix::MacroParamCache::instance().clear();
        helix::ui::set_filament_param_prompter(
            [this](const std::string& macro, const helix::CachedMacroInfo&,
                   const ParamValues& prefill, helix::MacroExecuteCallback) {
                ++prompt_count;
                prompted_macro = macro;
                prompted_prefill = prefill;
            });

        panel = std::make_unique<FilamentPanel>(state, &api);
        panel->init_subjects();
        client.clear_gcode_script_history();
    }

    ~PrefillPanelHarness() {
        helix::ui::set_filament_param_prompter({});
        panel.reset();
        StandardMacros::instance().reset();
        helix::MacroParamCache::instance().clear();
        helix::ui::UpdateQueue::instance().drain();
        AmsState::instance().deinit_subjects();
        ToolState::instance().deinit_subjects();
    }

    /// populate_from_configfile() replaces the cache, so every macro goes in one call.
    static void cache_macros(std::initializer_list<std::pair<const char*, const char*>> macros) {
        nlohmann::json config;
        std::unordered_set<std::string> names;
        for (const auto& [name, gcode] : macros) {
            config[std::string("gcode_macro ") + name]["gcode"] = gcode;
            names.insert(name);
        }
        helix::MacroParamCache::instance().populate_from_configfile(config, names);
    }

    /// The extruder target Klipper reports, in degrees.
    void set_extruder_target(double degrees) {
        state.update_from_status({{"extruder", {{"target", degrees}}}});
    }

    /// Every script sent whose first word is @p macro.
    [[nodiscard]] Scripts sent_for(const std::string& macro) const {
        Scripts out;
        for (const auto& script : client.gcode_script_history()) {
            if (script == macro || script.rfind(macro + " ", 0) == 0) {
                out.push_back(script);
            }
        }
        return out;
    }
};

} // namespace

// =============================================================================
// Load
// =============================================================================

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Load runs a temperature-only macro with the live extruder target, no dialog",
                 "[filament][prefill]") {
    PrefillPanelHarness h;
    h.cache_macros({{"LOAD_FILAMENT", helix::test::LOAD_FILAMENT_EXPRESSION_DEFAULT}});
    h.set_extruder_target(260.0);

    TA::execute_load(*h.panel);

    CHECK(h.prompt_count == 0);
    // Degrees, and only the parameter the macro reads.
    CHECK(h.sent_for("LOAD_FILAMENT") == Scripts{"LOAD_FILAMENT EXTRUDER_TEMP=260"});
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Load with the heater off sends the selected preset's temperature",
                 "[filament][prefill]") {
    PrefillPanelHarness h;
    h.cache_macros({{"LOAD_FILAMENT", helix::test::LOAD_FILAMENT_EXPRESSION_DEFAULT}});
    h.set_extruder_target(0.0);
    TA::set_selected_material(*h.panel, 0);

    auto material = filament::find_material(helix::presets::name(0));
    REQUIRE(material);
    const int preset_temp = helix::ui::load_preheat_temp(*material);
    REQUIRE(preset_temp > 0);

    TA::execute_load(*h.panel);

    CHECK(h.prompt_count == 0);
    CHECK(h.sent_for("LOAD_FILAMENT") ==
          Scripts{"LOAD_FILAMENT EXTRUDER_TEMP=" + std::to_string(preset_temp)});
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Load with no temperature known opens the dialog with nothing filled in",
                 "[filament][prefill]") {
    PrefillPanelHarness h;
    h.cache_macros({{"LOAD_FILAMENT", helix::test::LOAD_FILAMENT_EXPRESSION_DEFAULT}});
    h.set_extruder_target(0.0);
    TA::set_selected_material(*h.panel, -1);

    TA::execute_load(*h.panel);

    CHECK(h.prompt_count == 1);
    CHECK(h.prompted_macro == "LOAD_FILAMENT");
    CHECK(h.prompted_prefill.empty());
    CHECK(h.sent_for("LOAD_FILAMENT").empty());
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Load of a macro with a parameter nothing fills opens the dialog, temperature in",
                 "[filament][prefill]") {
    PrefillPanelHarness h;
    h.cache_macros({{"LOAD_FILAMENT", LOAD_WITH_LENGTH}});
    h.set_extruder_target(260.0);

    TA::execute_load(*h.panel);

    CHECK(h.prompt_count == 1);
    CHECK(h.prompted_prefill == ParamValues{{"EXTRUDER_TEMP", "260"}});
    CHECK(h.sent_for("LOAD_FILAMENT").empty());
}

// =============================================================================
// Unload and Purge take the same rule
// =============================================================================

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Unload hands a temperature-only macro the live extruder target",
                 "[filament][prefill]") {
    PrefillPanelHarness h;
    h.cache_macros({{"UNLOAD_FILAMENT",
                     "{% set t = params.TEMP|default(200)|int %}\nM109 S{t}\nG1 E-60 F600"}});
    h.set_extruder_target(250.0);

    TA::execute_unload(*h.panel);

    CHECK(h.prompt_count == 0);
    CHECK(h.sent_for("UNLOAD_FILAMENT") == Scripts{"UNLOAD_FILAMENT TEMP=250"});
}

TEST_CASE_METHOD(LVGLUITestFixture, "Purge sends PURGE_TEMP as a value, with no dialog",
                 "[filament][prefill]") {
    PrefillPanelHarness h;
    h.cache_macros(
        {{"PURGE", "{% set t = params.PURGE_TEMP|default(240)|int %}\nM109 S{t}\nG1 E30 F300"}});
    h.set_extruder_target(255.0);

    TA::execute_purge(*h.panel);

    CHECK(h.prompt_count == 0);
    CHECK(h.sent_for("PURGE") == Scripts{"PURGE PURGE_TEMP=255"});
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Purge with a parameter nothing fills asks through the shared prompt",
                 "[filament][prefill]") {
    PrefillPanelHarness h;
    h.cache_macros({{"PURGE", "{% set t = params.PURGE_TEMP|default(240)|int %}\n"
                              "{% set l = params.LENGTH|default(30)|float %}\n"
                              "M109 S{t}\nG1 E{l} F300"}});
    h.set_extruder_target(255.0);

    TA::execute_purge(*h.panel);

    CHECK(h.prompt_count == 1);
    CHECK(h.prompted_macro == "PURGE");
    CHECK(h.prompted_prefill == ParamValues{{"PURGE_TEMP", "255"}});
    CHECK(h.sent_for("PURGE").empty());
}

// =============================================================================
// MacroParamModal: a prefill is the field's text
// =============================================================================

namespace {

class ParamModalFixture : public LVGLUITestFixture {
  public:
    ParamModalFixture() {
        // Synchronous hide, so a dismissed modal is gone by the next drain.
        prev_animations_ = helix::DisplaySettingsManager::instance().get_animations_enabled();
        helix::DisplaySettingsManager::instance().set_animations_enabled(false);
    }

    ~ParamModalFixture() override {
        // A failed assertion can leave the modal up with its static instance
        // pointer aimed at this soon-dead object.
        if (modal.dialog()) {
            helix::MacroParamModal::cancel_cb(nullptr);
        }
        process_lvgl(20);
        helix::DisplaySettingsManager::instance().set_animations_enabled(prev_animations_);
    }

    /// Show the modal for the expression-default LOAD_FILAMENT and return its one field.
    lv_obj_t* show_load_filament(const ParamValues& prefill) {
        auto params = helix::parse_macro_params(helix::test::LOAD_FILAMENT_EXPRESSION_DEFAULT);
        REQUIRE(params.size() == 1);
        modal.show_for_macro(
            lv_screen_active(), "LOAD_FILAMENT", params,
            [this](const helix::MacroParamResult& result) {
                ++runs;
                sent = result;
            },
            prefill);
        REQUIRE(modal.dialog() != nullptr);
        lv_obj_t* field = lv_obj_find_by_name(modal.dialog(), "field_input");
        REQUIRE(field != nullptr);
        return field;
    }

    helix::MacroParamModal modal;
    int runs = 0;
    helix::MacroParamResult sent;

  private:
    bool prev_animations_ = true;
};

} // namespace

TEST_CASE_METHOD(ParamModalFixture, "A prefilled parameter is typed into its field and sent on Run",
                 "[filament][prefill][macro_params]") {
    lv_obj_t* field = show_load_filament({{"EXTRUDER_TEMP", "260"}});

    CHECK(std::string(lv_textarea_get_text(field)) == "260");
    // What a cleared field says: where the value comes from, not template source.
    CHECK(std::string(lv_textarea_get_placeholder_text(field)) == lv_tr("Printer default"));

    helix::MacroParamModal::run_cb(nullptr);
    process_lvgl(20);

    CHECK(runs == 1);
    CHECK(sent.params == ParamValues{{"EXTRUDER_TEMP", "260"}});
}

TEST_CASE_METHOD(ParamModalFixture, "A cleared prefill leaves the parameter to the macro",
                 "[filament][prefill][macro_params]") {
    lv_obj_t* field = show_load_filament({{"EXTRUDER_TEMP", "260"}});
    lv_textarea_set_text(field, "");

    helix::MacroParamModal::run_cb(nullptr);
    process_lvgl(20);

    CHECK(runs == 1);
    CHECK(sent.params.empty());
}
