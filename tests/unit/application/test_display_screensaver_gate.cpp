// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "ui_update_queue.h"

#include "app_globals.h"
#include "display_manager.h"
#include "lvgl_test_fixture.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"
#include "screensaver.h"
#include "test_helpers/printer_state_test_access.h"
#include "test_helpers/screensaver_manager_test_access.h"

#include "../../catch_amalgamated.hpp"
#include "hv/json.hpp"

namespace {

/// Gives the shared manager a scripted clock for the test and the real one back after it.
struct ScriptedGateClock {
    uint64_t wall_ns = 5000ULL * 1000000000ULL;
    ScreensaverManager& savers = ScreensaverManager::instance();

    ScriptedGateClock() {
        savers.stop();
        helix::ScreensaverManagerTestAccess::set_cpu_clock(
            savers, [this] { return helix::ui::CpuSample{0, wall_ns}; });
        helix::ScreensaverManagerTestAccess::reset_baseline(savers);
    }
    ~ScriptedGateClock() {
        helix::ScreensaverManagerTestAccess::set_cpu_clock(savers,
                                                           helix::ui::read_process_cpu_clock);
        helix::ScreensaverManagerTestAccess::reset_baseline(savers);
    }
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "the display manager's idle-check tick samples the screensaver gate",
                 "[application][display][screensaver_gate]") {
    ScriptedGateClock clock;
    DisplayManager mgr;

    for (int i = 0; i < 4; i++) {
        lv_display_trigger_activity(nullptr);
        mgr.check_display_sleep();
        clock.wall_ns += 300000000ULL;
    }

    CHECK(helix::ScreensaverManagerTestAccess::baseline_samples(clock.savers) == 4);
}

TEST_CASE_METHOD(
    LVGLTestFixture,
    "the screensaver host reports a print holding the machine and names a missing backend",
    "[application][display][screensaver_gate]") {
    helix::PrinterState& printer_state = get_printer_state();
    helix::PrinterStateTestAccess::reset(printer_state);
    printer_state.init_subjects(false);
    const auto drive = [&](const char* wire_state) {
        printer_state.update_from_status(nlohmann::json{{"print_stats", {{"state", wire_state}}}});
        printer_state.set_print_start_state(helix::PrintStartPhase::IDLE, "", 0);
        process_lvgl(10);
        for (int i = 0; i < 8; ++i) {
            helix::ui::UpdateQueue::instance().drain();
        }
    };

    const helix::ui::SaverHost host = DisplayManager::screensaver_host(nullptr);
    CHECK(host.display_backend == "unknown");
    REQUIRE(host.is_printing);

    drive("printing");
    REQUIRE(printer_state.get_print_lifecycle() == PrintState::Printing);
    CHECK(host.is_printing());

    drive("standby");
    REQUIRE(printer_state.get_print_lifecycle() == PrintState::Idle);
    CHECK_FALSE(host.is_printing());

    helix::PrinterStateTestAccess::reset(printer_state);
    printer_state.init_subjects(false);
}

#endif // HELIX_ENABLE_SCREENSAVER
