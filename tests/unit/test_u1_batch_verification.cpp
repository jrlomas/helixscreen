// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mock-side prerequisite for U1 batch verification: MoonrakerClientMock
// answers AUTO_FEEDING with the channel_state sequence the feeder firmware
// reports, so AmsBackendSnapmaker's status parse — and the batch cursor that
// will be built on top of it — has real transitions to observe in a unit
// test, through the same handle_status_update path the live WebSocket drives.

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "ams_backend_snapmaker.h"
#include "ams_types.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/snapmaker_test_access.h"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

namespace {

/// setenv with RAII restore, so one case's fail-slot cannot leak into the next.
struct ScopedEnvVar {
    std::string name;
    std::string saved;
    bool was_set = false;

    ScopedEnvVar(std::string n, const char* value) : name(std::move(n)) {
        if (const char* old = ::getenv(name.c_str())) {
            saved = old;
            was_set = true;
        }
        ::setenv(name.c_str(), value, /*overwrite=*/1);
    }
    ~ScopedEnvVar() {
        if (was_set) {
            ::setenv(name.c_str(), saved.c_str(), 1);
        } else {
            ::unsetenv(name.c_str());
        }
    }
};

/// A production AmsBackendSnapmaker over the mock API + client, so a batch
/// dispatch runs the real gcode path and the mock client's simulated feeder
/// frames flow back through the subscription the same way live frames do.
struct MockBatchFixture : public LVGLTestFixture {
    MoonrakerClientMock mock_client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> api;
    std::unique_ptr<helix::test::RegisteredBackend<helix::AmsBackendSnapmaker>> backend_;

    MockBatchFixture() {
        state.init_subjects(false);
        api = std::make_unique<MoonrakerAPIMock>(mock_client, state);
        backend_ = std::make_unique<helix::test::RegisteredBackend<helix::AmsBackendSnapmaker>>(
            api.get(), &mock_client);
        REQUIRE(backend().start().success());
    }

    helix::AmsBackendSnapmaker& backend() {
        return **backend_;
    }

    /// Run the queued feeder frames until the queue is empty and the backend
    /// is back at idle. Bounded so a wedged walk fails the test instead of
    /// hanging the suite.
    void pump_until_idle() {
        for (int i = 0; i < 100; ++i) {
            helix::ui::UpdateQueue::instance().drain();
            if (backend().get_current_action() == helix::AmsAction::IDLE) {
                return;
            }
        }
        FAIL("pump_until_idle: backend never returned to IDLE (action="
             << helix::ams_action_to_string(backend().get_current_action()) << ")");
    }
};

} // namespace

TEST_CASE_METHOD(MockBatchFixture, "Mock walks each head to its terminal channel state",
                 "[ams][batch][mock]") {
    REQUIRE(backend().load_filament_batch({0, 1}).success());
    pump_until_idle();

    CHECK(backend().channel_snapshot(0).state == "load_finish");
    CHECK(backend().channel_snapshot(1).state == "load_finish");
}

TEST_CASE_METHOD(MockBatchFixture, "Mock can fail a named head", "[ams][batch][mock]") {
    ScopedEnvVar fail_slot("HELIX_MOCK_BATCH_FAIL_SLOT", "1");
    REQUIRE(backend().load_filament_batch({0, 1}).success());
    helix::ui::UpdateQueue::instance().drain();

    CHECK(backend().channel_snapshot(0).state == "load_finish");
    CHECK(backend().channel_snapshot(1).state == "load_fail");
}

TEST_CASE_METHOD(MockBatchFixture, "Mock walks an unload through its heat step",
                 "[ams][batch][mock]") {
    REQUIRE(backend().unload_filament_batch({2}).success());
    pump_until_idle();

    CHECK(backend().channel_snapshot(2).state == "unload_finish");
    CHECK_FALSE(backend().channel_snapshot(2).filament_detected);
}

TEST_CASE_METHOD(MockBatchFixture, "A batch advances its cursor as heads finish", "[ams][batch]") {
    REQUIRE(backend().load_filament_batch({0, 1}).success());
    pump_until_idle();

    const auto plan = backend().batch_plan();
    CHECK(plan.cursor == 2);
    CHECK_FALSE(plan.active);
    CHECK(backend().get_system_info().operation_detail.find("2 of 2") != std::string::npos);
}

TEST_CASE_METHOD(MockBatchFixture, "A failed head stops the batch at its cursor", "[ams][batch]") {
    ScopedEnvVar fail_slot("HELIX_MOCK_BATCH_FAIL_SLOT", "1");
    REQUIRE(backend().load_filament_batch({0, 1}).success());
    // A *_fail state leaves the action at ERROR, which no later frame resolves
    // to IDLE in this walk, so pump_until_idle would spin to its bound. The
    // frames are all queued by dispatch; one drain observes the final state.
    helix::ui::UpdateQueue::instance().drain();

    const auto plan = backend().batch_plan();
    CHECK(plan.cursor == 1); // head 0 finished, head 1 did not
    CHECK_FALSE(plan.active);
    CHECK(backend().get_system_info().operation_detail.find("2") != std::string::npos);
}

TEST_CASE_METHOD(MockBatchFixture, "A failed head ends the firmware batch", "[ams][batch]") {
    helix::SnapmakerTestAccess::set_use_batch_macro(backend(), true);
    ScopedEnvVar fail_slot("HELIX_MOCK_BATCH_FAIL_SLOT", "1");
    REQUIRE(backend().load_filament_batch({0, 1}).success());
    // A *_fail leaves the action at ERROR, which no later frame resolves to
    // IDLE in this walk, so pump_until_idle would spin to its bound. One
    // drain observes the failure AND the recovery it triggers.
    helix::ui::UpdateQueue::instance().drain();

    const auto plan = backend().batch_plan();
    REQUIRE(plan.cursor == 1);  // head 0 verified; head 1 failed — the case the
    REQUIRE_FALSE(plan.active); // recovery answers
    // Prove the macro shape was actually dispatched, not the bare fallback.
    const auto& history = mock_client.gcode_script_history();
    REQUIRE(std::any_of(history.begin(), history.end(), [](const std::string& line) {
        return line.find("AUTO_FEEDING_BATCH ACTION=START") != std::string::npos;
    }));
    // The chain itself ends with an ACTION=END line and the mock delivers
    // every line even though real Klipper would abort the script at the
    // raise, so the recovery is proven by a SECOND exact-line END beyond the
    // chain's own.
    CHECK(std::count(history.begin(), history.end(), "AUTO_FEEDING_BATCH ACTION=END") >= 2);
}

TEST_CASE_METHOD(MockBatchFixture, "Without the macro nothing extra is sent", "[ams][batch]") {
    helix::SnapmakerTestAccess::set_use_batch_macro(backend(), false);
    ScopedEnvVar fail_slot("HELIX_MOCK_BATCH_FAIL_SLOT", "0");
    REQUIRE(backend().load_filament_batch({0}).success());
    // Fail case: single drain, same reason as above.
    helix::ui::UpdateQueue::instance().drain();

    // Firmware without AUTO_FEEDING_BATCH has no interlock to clear, so the
    // recovery must not invent the macro out of thin air.
    const auto& history = mock_client.gcode_script_history();
    CHECK(std::none_of(history.begin(), history.end(), [](const std::string& line) {
        return line.find("AUTO_FEEDING_BATCH") != std::string::npos;
    }));
}

TEST_CASE_METHOD(MockBatchFixture, "The AUTO_FEEDING_BATCH shape advances the cursor too",
                 "[ams][batch]") {
    // Discovery never runs in a unit test, so the capability cache starts
    // false; force the START/DOING/END script shape and prove the cursor still
    // verifies each head through it.
    helix::SnapmakerTestAccess::set_use_batch_macro(backend(), true);
    REQUIRE(backend().load_filament_batch({0, 1}).success());
    // The mock records each script LINE separately; prove the START sentinel
    // of the batch shape actually went out, not a bare-AUTO_FEEDING fallback.
    const auto& history = mock_client.gcode_script_history();
    REQUIRE(std::any_of(history.begin(), history.end(), [](const std::string& line) {
        return line.find("AUTO_FEEDING_BATCH ACTION=START") != std::string::npos;
    }));
    pump_until_idle();

    const auto plan = backend().batch_plan();
    CHECK(plan.cursor == 2);
    CHECK_FALSE(plan.active);
}
