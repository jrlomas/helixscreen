// tests/unit/test_snapmaker_error_classify.cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/gcode_error_router_test_access.h"
#include "app_globals.h"
#include "fault_surface_correlation.h"
#include "firmware_fault_codes.h"
#include "gcode_error_router.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "recovery_modal_presenter.h"
#include "snapmaker_exceptions.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

PrinterDiscovery hardware_with_objects(const std::vector<std::string>& names) {
    PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array();
    for (const auto& n : names) {
        objects.push_back(n);
    }
    hw.parse_objects(objects);
    return hw;
}

PrinterDiscovery coded_firmware() {
    return hardware_with_objects({"exception_manager", "print_task_config"});
}

} // namespace

TEST_CASE("a known fault classifies from its code", "[snapmaker][classify]") {
    auto e = faultcodes::classify(coded_firmware(),
                                  "!! 0003-0530-0000-0011 The plate has not been removed");
    REQUIRE(e.has_value());
    REQUIRE(e->detail.find("Remove the PEI sheet") != std::string::npos);
    REQUIRE(e->source == ErrorSource::SNAPMAKER);
}

TEST_CASE("classification does not depend on the firmware's wording", "[snapmaker][classify]") {
    // The whole point: the code is the identity. Firmware reworded, or a
    // locale we do not read, must still classify.
    auto e = faultcodes::classify(coded_firmware(), "!! 0003-0530-0000-0011 platen nicht entfernt");
    REQUIRE(e.has_value());
    REQUIRE(e->detail.find("Remove the PEI sheet") != std::string::npos);
}

TEST_CASE("an unknown code keeps the firmware's own text", "[snapmaker][classify]") {
    auto e = faultcodes::classify(coded_firmware(),
                                  "!! 0002-0999-0000-0007 something we have no wording for");
    REQUIRE(e.has_value());
    REQUIRE(e->detail.find("something we have no wording for") != std::string::npos);
}

TEST_CASE("a line with no code is left to the generic path", "[snapmaker][classify]") {
    REQUIRE_FALSE(faultcodes::classify(coded_firmware(), "!! Must home Z axis first").has_value());
}

TEST_CASE("a firmware without the code channel classifies nothing", "[snapmaker][classify]") {
    // The capability question gates this, not the vendor name: a printer that
    // does not publish exception_manager must fall through untouched even if a
    // line happens to look code-shaped.
    PrinterDiscovery plain = hardware_with_objects({"bed_mesh", "quad_gantry_level"});
    REQUIRE_FALSE(faultcodes::firmware_reports_fault_codes(plain));
    REQUIRE_FALSE(
        faultcodes::classify(plain, "!! 0003-0530-0000-0011 The plate has not been removed")
            .has_value());
}

TEST_CASE("a power-loss shutdown's coded payload classifies", "[snapmaker][classify]") {
    // The firmware reports a mains loss through invoke_shutdown with the code
    // in a JSON "coded" field, not through exception_manager. The decoder
    // scans anywhere in the line, so an error-prefixed line carrying that
    // payload reads our wording rather than an unrecognised shutdown's.
    auto e = faultcodes::classify(coded_firmware(),
                                  R"(error: {"coded": "0003-0522-0000-0017", )"
                                  R"("msg":"mcu: Power loss triggered", "oneshot": 0})");
    REQUIRE(e.has_value());
    REQUIRE(e->code == "0003-0522-0000-0017");
    REQUIRE(e->detail.find("Power was lost") != std::string::npos);
    REQUIRE(e->severity == ErrorSeverity::CRITICAL);
}

TEST_CASE_METHOD(LVGLUITestFixture, "the router surfaces our wording for a coded fault",
                 "[snapmaker][classify]") {
    // Install the capability on the singleton the router reads, then drive the
    // REAL process_line. The surfaced record must carry OUR wording for the
    // fault: the generic classifier can only produce the firmware's own
    // sentence, so this is what pins the router's faultcodes pass.
    PrinterDiscovery hw = coded_firmware();
    get_printer_state().set_hardware(std::move(hw));

    helix::ui::RecoveryModalPresenter presenter(nullptr);
    helix::GcodeErrorRouter router(nullptr, nullptr, presenter);
    REQUIRE_NOTHROW(GcodeErrorRouterTestAccess::process_line(
        router, "!! 0003-0530-0000-0011 The plate has not been removed"));
    helix::ui::UpdateQueue::instance().drain();

    const auto wording =
        snapmaker::exception_message(*snapmaker::decode_exception_code("0003-0530-0000-0011"));
    REQUIRE_FALSE(wording.empty());
    REQUIRE(fault_surface_correlation::was_recently_surfaced(std::string(wording)));

    get_printer_state().set_hardware(PrinterDiscovery{});
}

// ============================================================================
// Translation: the fault table holds the English source; the wording becomes
// user-facing at the detail/message assignment, which is where lv_tr() lives.
// ============================================================================

#include "translation_loader.h"

TEST_CASE_METHOD(LVGLUITestFixture, "the known fault wording reaches the user translated",
                 "[snapmaker][classify][i18n]") {
    helix::ui::ensure_translation_loaded("de");
    lv_translation_set_language("de");

    // The code is the identity, so a German-locale user classifies from the
    // code whatever the firmware's sentence says; the surfaced detail must be
    // the catalog's German sentence, which only happens if the table's
    // wording goes through lv_tr.
    auto e =
        faultcodes::classify(coded_firmware(), "!! 0003-0530-0000-0011 platten nicht entfernt");
    REQUIRE(e.has_value());
    REQUIRE(e->detail.find("Entferne die PEI-Platte") != std::string::npos);

    lv_translation_set_language(helix::ui::kIdentityLocale);
}

TEST_CASE_METHOD(LVGLUITestFixture, "a standing fault's message is translated",
                 "[snapmaker][exceptions][i18n]") {
    helix::ui::ensure_translation_loaded("de");
    lv_translation_set_language("de");

    nlohmann::json s = nlohmann::json::object();
    s["exception_manager"] = {{"exceptions",
                               {{{"id", 530},
                                 {"index", 0},
                                 {"code", 11},
                                 {"level", 3},
                                 {"message", "The plate has not been removed"},
                                 {"is_persistent", 0}}}}};
    const auto v = snapmaker::read_active_exceptions(s);
    REQUIRE(v.size() == 1);
    REQUIRE(v[0].message.find("Entferne die PEI-Platte") != std::string::npos);

    lv_translation_set_language(helix::ui::kIdentityLocale);
}
