// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_chamber_panel_diagnostics.cpp
 * @brief Task 7 of the chamber-heater backend abstraction (issue #1290):
 *        the temp graph overlay renders a capability- and mode-gated
 *        diagnostics card under the chart (chamber mode only).
 *
 * The card is pure declarative XML (shared component
 * components/chamber_diagnostics_card.xml): outer visibility is a structural
 * <if cond="printer_has_chamber_heater_diagnostics and temp_graph_mode eq 3">,
 * inner visibility comes from bind_flag_if (fault OR inhibited banner) and
 * bind_flag_if_eq (filter-fan capability), readouts bind the *_text formatter
 * subjects, and the two buttons fire TemperatureService XML callbacks that
 * delegate to the globally-registered TemperatureController — never the api
 * directly.
 *
 * The unit-test display is landscape, so the portrait branch (scrollable
 * graph column, min-height floored chart) is verified out-of-band via the
 * live `ctl geom` gate documented in task-7-report.md.
 */

#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "app_globals.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "panel_widget_manager.h"
#include "printer_state.h"
#include "printer_temperature_state.h"
#include "temperature_controller.h"
#include "temperature_service.h"

#include <lvgl.h>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrinterTemperatureState;
using helix::TemperatureController;

namespace {

/// Set an int subject in the XML registry — the name the overlay binds, which
/// may be this fixture's registration or a prior test's; either way it is the
/// live subject the overlay's bindings observe.
lv_subject_t* set_xml_int(const char* name, int value) {
    lv_subject_t* subject = lv_xml_get_subject(nullptr, name);
    REQUIRE(subject != nullptr);
    lv_subject_set_int(subject, value);
    return subject;
}

lv_subject_t* set_xml_string(const char* name, const char* value) {
    lv_subject_t* subject = lv_xml_get_subject(nullptr, name);
    REQUIRE(subject != nullptr);
    lv_subject_copy_string(subject, value);
    return subject;
}

bool hidden(lv_obj_t* obj) {
    return obj == nullptr || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

/// Nominal faulted dragonbreath frame (same shape as
/// test_chamber_diagnostics_subjects.cpp): latched fault, PTC element at
/// 106.2°C, filter fan purging at 100%, pin on.
nlohmann::json faulted_dragonbreath_status() {
    return nlohmann::json::parse(R"({
      "heater_generic dragonbreath": {"temperature": 25.5, "target": 30.0},
      "dragonbreath": {"fault": true, "inhibited": false, "fault_reason": "ptc_overtemp",
        "ptc_temp": 106.2, "fan_percent": 100, "fan_reason": "purge",
        "mode": "off", "source": "device", "lease_owned": false},
      "output_pin dragonbreath_filter": {"value": 1.0}})");
}

/// Builds the real temp_graph_overlay.xml with its component dependencies,
/// in the same shape production's xml_registration.cpp uses. temp_graph_mode
/// is a TempGraphOverlay-owned subject in production; the fixture registers a
/// static stand-in (the XML subject registry is process-global and never
/// forgets an entry, so a fixture member would dangle after this test — same
/// reasoning as AdvancedPowerGroupFixture's host-power subject).
class ChamberOverlayFixture : public XMLTestFixture {
  public:
    ChamberOverlayFixture() : XMLTestFixture() {
        // Chamber mode BEFORE create: the card's outer gate is a structural
        // <if cond="... temp_graph_mode eq 3"> evaluated at view creation.
        mode_subject_ = lv_xml_get_subject(nullptr, "temp_graph_mode");
        if (!mode_subject_) {
            static lv_subject_t mode_subject;
            lv_subject_init_int(&mode_subject, 3); // TempGraphOverlay::Mode::Chamber
            lv_xml_register_subject(nullptr, "temp_graph_mode", &mode_subject);
            mode_subject_ = &mode_subject;
        }
        // A prior test case may have left another mode on the shared static
        // subject — chamber is the default for every section here.
        lv_subject_set_int(mode_subject_, 3);

        REQUIRE(register_component("components/nozzle_icon"));
        REQUIRE(register_component("components/heater_icon"));
        REQUIRE(register_component("components/chamber_diagnostics_card"));
        REQUIRE(register_component("header_bar"));
        REQUIRE(register_component("overlay_panel"));
        // The card's two diagnostics callbacks must exist before the overlay's
        // XML resolves them — same registration TemperatureService performs.
        lv_xml_register_event_cb(nullptr, "on_chamber_fault_reset_clicked",
                                 TemperatureService::on_chamber_fault_reset_clicked);
        lv_xml_register_event_cb(nullptr, "on_chamber_filter_fan_clicked",
                                 TemperatureService::on_chamber_filter_fan_clicked);
        // The overlay's own callbacks (no-ops here; production registers the
        // TempGraphOverlay handlers in xml_registration.cpp).
        lv_xml_register_event_cb(nullptr, "on_temp_graph_preset_clicked", xml_test_noop_event_cb);
        lv_xml_register_event_cb(nullptr, "on_temp_graph_custom_clicked", xml_test_noop_event_cb);
        REQUIRE(register_component("temp_graph_overlay"));

        // Capability gates default to 0 (card not built); raise them before
        // creation so the structural <if> builds the card — the hidden-when-off
        // cases re-set them explicitly (the reactive cond rebuilds).
        set_xml_int("printer_has_chamber_heater_diagnostics", 1);
        set_xml_int("printer_has_chamber_filter_fan", 1);
        set_xml_int("printer_has_chamber_element_temp", 1);

        overlay_ = create_component("temp_graph_overlay");
        REQUIRE(overlay_ != nullptr);
        helix::ui::UpdateQueue::instance().drain();
    }

    lv_obj_t* overlay_;
    lv_subject_t* mode_subject_ = nullptr;

  private:
    static void xml_test_noop_event_cb(lv_event_t* /*e*/) {}
};

} // namespace

// ============================================================================
// Card structure + visibility (all declarative)
// ============================================================================

TEST_CASE_METHOD(ChamberOverlayFixture,
                 "temp graph overlay diagnostics card visibility and content",
                 "[chamber][panel][xml]") {
    SECTION("chamber mode with diagnostics shows banner, reason, buttons") {
        set_xml_int("chamber_heater_fault", 1);
        set_xml_int("chamber_heater_inhibited", 0);
        // Raw vendor code stays log-only; the banner binds the translated kind.
        set_xml_string("chamber_heater_fault_reason_text", lv_tr("Heater over-temperature"));
        set_xml_string("chamber_heater_element_temp_text", "106°C");
        set_xml_string("chamber_filter_fan_percent_text", "100%");
        helix::ui::UpdateQueue::instance().drain();

        lv_obj_t* card = lv_obj_find_by_name(overlay_, "chamber_diagnostics_card");
        REQUIRE(card != nullptr);
        CHECK_FALSE(hidden(card));

        lv_obj_t* banner = lv_obj_find_by_name(overlay_, "fault_banner");
        REQUIRE(banner != nullptr);
        CHECK_FALSE(hidden(banner));

        lv_obj_t* reason = lv_obj_find_by_name(overlay_, "fault_reason_label");
        REQUIRE(reason != nullptr);
        std::string reason_text = lv_label_get_text(reason);
        CHECK(reason_text == std::string(lv_tr("Heater over-temperature")));
        CHECK(reason_text.find("ptc_overtemp") == std::string::npos);

        REQUIRE(lv_obj_find_by_name(overlay_, "reset_fault_button") != nullptr);

        lv_obj_t* fan_btn = lv_obj_find_by_name(overlay_, "filter_fan_button");
        REQUIRE(fan_btn != nullptr);
        CHECK_FALSE(hidden(fan_btn));

        // Readout labels bind the formatter subjects.
        lv_obj_t* element = lv_obj_find_by_name(overlay_, "element_temp_label");
        REQUIRE(element != nullptr);
        CHECK(std::string(lv_label_get_text(element)) == "106°C");
        lv_obj_t* percent = lv_obj_find_by_name(overlay_, "fan_percent_label");
        REQUIRE(percent != nullptr);
        CHECK(std::string(lv_label_get_text(percent)) == "100%");
    }

    SECTION("fault and inhibited both clear hides the banner, keeps the card") {
        set_xml_int("chamber_heater_fault", 1);
        set_xml_int("chamber_heater_inhibited", 1);
        helix::ui::UpdateQueue::instance().drain();

        set_xml_int("chamber_heater_fault", 0);
        set_xml_int("chamber_heater_inhibited", 0);
        helix::ui::UpdateQueue::instance().drain();

        CHECK(hidden(lv_obj_find_by_name(overlay_, "fault_banner")));
        CHECK_FALSE(hidden(lv_obj_find_by_name(overlay_, "chamber_diagnostics_card")));
    }

    SECTION("inhibited alone keeps the banner (OR, not fault-only)") {
        set_xml_int("chamber_heater_fault", 0);
        set_xml_int("chamber_heater_inhibited", 1);
        helix::ui::UpdateQueue::instance().drain();

        CHECK_FALSE(hidden(lv_obj_find_by_name(overlay_, "fault_banner")));
    }

    SECTION("offline banners without Reset; merely faulted keeps Reset") {
        // Merely faulted: banner + reason + Reset present, no offline message.
        set_xml_int("chamber_heater_fault", 1);
        set_xml_int("chamber_heater_inhibited", 0);
        set_xml_int("chamber_heater_offline", 0);
        helix::ui::UpdateQueue::instance().drain();

        CHECK_FALSE(hidden(lv_obj_find_by_name(overlay_, "fault_banner")));
        CHECK_FALSE(hidden(lv_obj_find_by_name(overlay_, "reset_fault_button")));
        CHECK(hidden(lv_obj_find_by_name(overlay_, "offline_label")));

        // Offline with no fault: the banner carries the offline message, and
        // the Reset button is HIDDEN — DRAGONBREATH_RESET clears a latched
        // fault ON the device and cannot reach one that is not answering.
        set_xml_int("chamber_heater_fault", 0);
        set_xml_int("chamber_heater_offline", 1);
        helix::ui::UpdateQueue::instance().drain();

        CHECK_FALSE(hidden(lv_obj_find_by_name(overlay_, "fault_banner")));
        CHECK(hidden(lv_obj_find_by_name(overlay_, "reset_fault_button")));
        lv_obj_t* offline_label = lv_obj_find_by_name(overlay_, "offline_label");
        REQUIRE(offline_label != nullptr);
        CHECK_FALSE(hidden(offline_label));
        CHECK(std::string(lv_label_get_text(offline_label)) ==
              std::string(lv_tr("Heater offline")));
        // The fault-reason text does not show while offline: an unreachable
        // device is not a device reporting a fault.
        CHECK(hidden(lv_obj_find_by_name(overlay_, "fault_reason_label")));

        // Back online with nothing faulted: banner drops, Reset returns.
        set_xml_int("chamber_heater_offline", 0);
        helix::ui::UpdateQueue::instance().drain();

        CHECK(hidden(lv_obj_find_by_name(overlay_, "fault_banner")));
        CHECK_FALSE(hidden(lv_obj_find_by_name(overlay_, "reset_fault_button")));
    }

    SECTION("no diagnostics capability unbuilds the whole card") {
        // Structural <if>: dropping the capability tears the card down — the
        // names are gone from the tree entirely, not merely hidden.
        set_xml_int("printer_has_chamber_heater_diagnostics", 0);
        helix::ui::UpdateQueue::instance().drain();

        CHECK(lv_obj_find_by_name(overlay_, "chamber_diagnostics_card") == nullptr);
        CHECK(lv_obj_find_by_name(overlay_, "fault_banner") == nullptr);
    }

    SECTION("non-chamber modes unbuild the card (structural mode gate)") {
        set_xml_int("temp_graph_mode", 1); // Nozzle
        helix::ui::UpdateQueue::instance().drain();
        CHECK(lv_obj_find_by_name(overlay_, "chamber_diagnostics_card") == nullptr);

        set_xml_int("temp_graph_mode", 0); // GraphOnly
        helix::ui::UpdateQueue::instance().drain();
        CHECK(lv_obj_find_by_name(overlay_, "chamber_diagnostics_card") == nullptr);

        // Back to chamber: the reactive cond rebuilds the card.
        set_xml_int("temp_graph_mode", 3);
        helix::ui::UpdateQueue::instance().drain();
        lv_obj_t* card = lv_obj_find_by_name(overlay_, "chamber_diagnostics_card");
        REQUIRE(card != nullptr);
        CHECK_FALSE(hidden(card));
        CHECK(lv_obj_find_by_name(overlay_, "reset_fault_button") != nullptr);
    }

    SECTION("no filter-fan capability hides the toggle and its readout") {
        set_xml_int("printer_has_chamber_filter_fan", 0);
        helix::ui::UpdateQueue::instance().drain();

        CHECK_FALSE(hidden(lv_obj_find_by_name(overlay_, "chamber_diagnostics_card")));
        CHECK(hidden(lv_obj_find_by_name(overlay_, "filter_fan_button")));
        CHECK(hidden(lv_obj_find_by_name(overlay_, "filter_fan_readout")));
    }

    // A backend with no element temperature (stock Panda Breath) would otherwise
    // render a row that can only ever read "--".
    SECTION("no element-temp capability hides that readout, keeps the rest") {
        set_xml_int("printer_has_chamber_element_temp", 0);
        helix::ui::UpdateQueue::instance().drain();

        CHECK(hidden(lv_obj_find_by_name(overlay_, "element_readout")));
        CHECK_FALSE(hidden(lv_obj_find_by_name(overlay_, "chamber_diagnostics_card")));
        CHECK_FALSE(hidden(lv_obj_find_by_name(overlay_, "filter_fan_readout")));
    }

    // The External badge annotates the heater, not the element, so it survives a
    // backend that reports no element temperature.
    SECTION("external control shows with no element readout beside it") {
        set_xml_int("printer_has_chamber_element_temp", 0);
        set_xml_int("chamber_heater_externally_controlled", 1);
        helix::ui::UpdateQueue::instance().drain();

        CHECK(hidden(lv_obj_find_by_name(overlay_, "element_readout")));
        CHECK_FALSE(hidden(lv_obj_find_by_name(overlay_, "external_control_readout")));
        CHECK_FALSE(hidden(lv_obj_find_by_name(overlay_, "external_control_badge")));

        set_xml_int("chamber_heater_externally_controlled", 0);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(hidden(lv_obj_find_by_name(overlay_, "external_control_readout")));
    }

    SECTION("device-driven fan badges the readout and disables the toggle") {
        lv_obj_t* fan_btn = lv_obj_find_by_name(overlay_, "filter_fan_button");
        REQUIRE(fan_btn != nullptr);
        lv_obj_t* badge = lv_obj_find_by_name(overlay_, "fan_device_badge");
        REQUIRE(badge != nullptr);

        // Our request driving the fan: no badge, toggle usable.
        set_xml_int("chamber_filter_fan_device_driven", 0);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(hidden(badge));
        CHECK_FALSE(lv_obj_has_state(fan_btn, LV_STATE_DISABLED));

        // Device's own initiative: badge appears beside the percent, toggle
        // disables (a click here cannot stop a fan the device is running).
        set_xml_int("chamber_filter_fan_device_driven", 1);
        helix::ui::UpdateQueue::instance().drain();
        CHECK_FALSE(hidden(badge));
        CHECK(lv_obj_has_state(fan_btn, LV_STATE_DISABLED));
    }
}

// ============================================================================
// Button actions delegate to the globally-registered TemperatureController
// ============================================================================

TEST_CASE_METHOD(ChamberOverlayFixture, "diagnostics card buttons drive the controller",
                 "[chamber][panel][xml][actions]") {
    // The XML callbacks reach the controller through app_globals, exactly as
    // production wires it — register the fixture's controller there.
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    MoonrakerAPI api(client, state());
    TemperatureController controller(state(), &api);
    controller.set_chamber_actions("DRAGONBREATH_RESET", "output_pin dragonbreath_filter", 60.0);
    // execute_gcode gates on klippy state; the subject defaults to SHUTDOWN.
    state().set_klippy_state_sync(helix::KlippyState::READY);
    helix::PanelWidgetManager::instance().register_shared_resource<helix::TemperatureController>(
        &controller);

    set_xml_int("chamber_heater_fault", 1);
    set_xml_int("chamber_heater_inhibited", 0);
    helix::ui::UpdateQueue::instance().drain();

    SECTION("reset button sends the backend reset gcode") {
        lv_obj_t* reset = lv_obj_find_by_name(overlay_, "reset_fault_button");
        REQUIRE(reset != nullptr);

        client.clear_gcode_script_history();
        lv_obj_send_event(reset, LV_EVENT_CLICKED, nullptr);
        helix::ui::UpdateQueue::instance().drain();

        REQUIRE(client.gcode_script_history().size() == 1);
        CHECK(client.gcode_script_history()[0] == "DRAGONBREATH_RESET");
    }

    SECTION("filter-fan button toggles our request, not the device's running state") {
        lv_obj_t* fan_btn = lv_obj_find_by_name(overlay_, "filter_fan_button");
        REQUIRE(fan_btn != nullptr);

        // Request off (pin 0) -> click turns our request on.
        set_xml_int("chamber_filter_fan_requested", 0);
        client.clear_gcode_script_history();
        lv_obj_send_event(fan_btn, LV_EVENT_CLICKED, nullptr);
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(client.gcode_script_history().size() == 1);
        CHECK(client.gcode_script_history()[0] == "SET_PIN PIN=dragonbreath_filter VALUE=1");

        // Request on -> click turns it off.
        set_xml_int("chamber_filter_fan_requested", 1);
        client.clear_gcode_script_history();
        lv_obj_send_event(fan_btn, LV_EVENT_CLICKED, nullptr);
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(client.gcode_script_history().size() == 1);
        CHECK(client.gcode_script_history()[0] == "SET_PIN PIN=dragonbreath_filter VALUE=0");

        // The running state is the DEVICE's business: a fan the device runs
        // at full speed while our request is still off must not flip the next
        // click to VALUE=0 — the click inverts the request.
        set_xml_int("chamber_filter_fan_on", 1);
        set_xml_int("chamber_filter_fan_requested", 0);
        client.clear_gcode_script_history();
        lv_obj_send_event(fan_btn, LV_EVENT_CLICKED, nullptr);
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(client.gcode_script_history().size() == 1);
        CHECK(client.gcode_script_history()[0] == "SET_PIN PIN=dragonbreath_filter VALUE=1");
    }

    // Drop the registration so later tests' get_temperature_controller()
    // sees no controller instead of this fixture's soon-destroyed one.
    helix::PanelWidgetManager::instance().register_shared_resource<helix::TemperatureController>(
        std::shared_ptr<TemperatureController>{});
}

// ============================================================================
// Formatter subjects: the parse block writes display-ready strings alongside
// the raw ints (raw ints render bare; XML has no deci/percent formatter).
// ============================================================================

TEST_CASE("diagnostics parse block writes display text subjects", "[chamber][subjects][text]") {
    LVGLTestFixture fixture;

    PrinterTemperatureState ts;
    ts.init_subjects(false);
    ts.set_chamber_diagnostics_source("dragonbreath", "dragonbreath",
                                      "output_pin dragonbreath_filter");

    ts.update_from_status(faulted_dragonbreath_status());

    CHECK(std::string(lv_subject_get_string(ts.get_chamber_heater_fault_reason_text_subject())) ==
          std::string(lv_tr("Heater over-temperature")));
    CHECK(std::string(lv_subject_get_string(ts.get_chamber_heater_element_temp_text_subject())) ==
          "106°C"); // canonical decimal-drop rule: whole degrees at/above 100
    CHECK(std::string(lv_subject_get_string(ts.get_chamber_filter_fan_percent_text_subject())) ==
          "100%");
    CHECK(std::string(lv_subject_get_string(ts.get_chamber_filter_fan_on_text_subject())) ==
          std::string(lv_tr("Filter Fan: On")));
    // Icon-name subject (compact portrait card's bind_icon toggle) flips with
    // the same pin.
    CHECK(std::string(lv_subject_get_string(ts.get_chamber_filter_fan_icon_subject())) == "fan");

    // The pin is a request, not the fan: a pin-only delta updates the
    // request, and the running pair follows the reported fan speed (100%
    // above) until a diagnostics frame says otherwise.
    ts.update_from_status({{"output_pin dragonbreath_filter", {{"value", 0.0}}}});
    CHECK(lv_subject_get_int(ts.get_chamber_filter_fan_requested_subject()) == 0);
    CHECK(lv_subject_get_int(ts.get_chamber_filter_fan_on_subject()) == 1);
    CHECK(std::string(lv_subject_get_string(ts.get_chamber_filter_fan_on_text_subject())) ==
          std::string(lv_tr("Filter Fan: On")));
    CHECK(std::string(lv_subject_get_string(ts.get_chamber_filter_fan_icon_subject())) == "fan");
    CHECK(std::string(lv_subject_get_string(ts.get_chamber_heater_element_temp_text_subject())) ==
          "106°C");

    // Sub-100 element temp keeps its one decimal; the fan stopping is what
    // flips the running pair.
    ts.update_from_status(nlohmann::json::parse(
        R"({"dragonbreath": {"fault": false, "fault_reason": null, "ptc_temp": 39.4,
                            "fan_percent": 0, "fan_reason": "off"}})"));
    CHECK(std::string(lv_subject_get_string(ts.get_chamber_heater_element_temp_text_subject())) ==
          "39.4°C");
    CHECK(lv_subject_get_int(ts.get_chamber_filter_fan_on_subject()) == 0);
    CHECK(std::string(lv_subject_get_string(ts.get_chamber_filter_fan_on_text_subject())) ==
          std::string(lv_tr("Filter Fan: Off")));
    CHECK(std::string(lv_subject_get_string(ts.get_chamber_filter_fan_icon_subject())) ==
          "fan_off");
}
