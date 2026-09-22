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
