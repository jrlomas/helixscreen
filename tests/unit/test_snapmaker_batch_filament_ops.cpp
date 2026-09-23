// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for batch filament load/unload on parallel-toolhead printers.
//
// AmsBackendSnapmaker::batch_feed_gcode is the pure script builder: one
// multi-line AUTO_FEEDING script covering every requested head, in the order
// given. The firmware's FEED_AUTO serializes channels itself (a single
// process-wide channel_active gate), and Moonraker holds the script response
// until the whole script has run, so one script IS the sequencer.
//
// The batch rides AmsSubscriptionBackend::run_filament_op like the per-slot
// ops, so it inherits the claim (exactly one filament op in flight, batch or
// not) and the print-active refusal. The gate tests here pin that inheritance.

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/snapmaker_test_access.h"
#include "ams_backend.h"
#include "ams_backend_happy_hare.h"
#include "ams_backend_snapmaker.h"
#include "ams_error.h"
#include "ams_types.h"
#include "app_globals.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "test_helpers/registered_backend.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::AmsError;
using helix::AmsErrorHelper;
using helix::AmsResult;

namespace {

using namespace std::chrono_literals;

/// A subscription backend that has not opted into batch ops, with the started
/// half of the gate satisfied so the refusal under test is the batch one.
class BatchlessBackend : public helix::AmsBackendHappyHare {
  public:
    BatchlessBackend() : helix::AmsBackendHappyHare(nullptr, nullptr) {
        running_.store(true);
    }
};

/// Parks its do_load_filament entrant until released, so a test can observe a
/// second op against a filament-op claim that is genuinely held. The 5s escape
/// hatch keeps a regression from wedging the suite.
class ClaimParkingBackend : public helix::AmsBackendSnapmaker {
  public:
    ClaimParkingBackend() : helix::AmsBackendSnapmaker(nullptr, nullptr) {
        running_.store(true);
    }

    void wait_for_parked_hook() {
        std::unique_lock<std::mutex> lock(m_);
        REQUIRE(cv_.wait_for(lock, 5s, [this] { return entered_; }));
    }

    void release_hook() {
        std::lock_guard<std::mutex> lock(m_);
        release_ = true;
        cv_.notify_all();
    }

  protected:
    AmsError do_load_filament(int /*slot_index*/) override {
        std::unique_lock<std::mutex> lock(m_);
        entered_ = true;
        cv_.notify_all();
        cv_.wait_for(lock, 5s, [this] { return release_; });
        return AmsErrorHelper::success();
    }

  private:
    std::mutex m_;
    std::condition_variable cv_;
    bool entered_ = false;
    bool release_ = false;
};

struct BatchFixture : public LVGLTestFixture {
    BatchFixture() : mock_client(MoonrakerClientMock::PrinterType::VORON_24) {
        state.init_subjects(false);
        api = std::make_unique<MoonrakerAPIMock>(mock_client, state);
    }

    void set_print_state(helix::PrintJobState s) {
        helix::test::set_wire_state(state, s);
    }

    /// A started, idle Snapmaker backend through the production factory, with
    /// the gcode history cleared — the state the batch is measured in.
    std::unique_ptr<helix::AmsBackend> snapmaker() {
        auto backend =
            helix::AmsBackend::create(helix::AmsType::SNAPMAKER, api.get(), &mock_client);
        REQUIRE(backend != nullptr);
        REQUIRE(backend->start().success());
        REQUIRE(backend->get_current_action() == helix::AmsAction::IDLE);
        mock_client.clear_gcode_script_history();
        return backend;
    }

    /// Drain UpdateQueue before reading history: several dispatch paths hop to
    /// the main thread before their send, and without draining "nothing was
    /// sent" could just mean "not sent yet".
    const std::vector<std::string>& sent_gcodes() {
        helix::ui::UpdateQueue::instance().drain();
        return mock_client.gcode_script_history();
    }

    /// A registered Snapmaker backend with the concrete type visible, for
    /// tests that reach what only AmsBackendSnapmaker exposes. Registered
    /// through AmsState so lane funnels accept its lane ids.
    helix::AmsBackendSnapmaker& backend() {
        if (!raw_backend_) {
            raw_backend_ =
                std::make_unique<helix::test::RegisteredBackend<helix::AmsBackendSnapmaker>>(
                    nullptr, nullptr);
        }
        return **raw_backend_;
    }

    /// Feed one raw JSON status frame to the backend's status handler — the
    /// same entry point the WebSocket drives.
    void feed_status(const std::string& json_text) {
        helix::SnapmakerTestAccess::handle_status(backend(), nlohmann::json::parse(json_text));
    }

    /// One frame carrying the feeder channel and its motion sensor — the two
    /// objects slot_op_eligibility answers from. The sensor reads enabled, as
    /// it does on the rig; the SensorDisabled section re-feeds it false.
    void set_channel(int slot, const char* state, const char* error, bool detected, bool module,
                     bool no_auto) {
        nlohmann::json frame = {
            {"filament_feed " + std::string(slot < 2 ? "left" : "right"),
             {{"extruder" + std::to_string(slot),
               {{"channel_state", state},
                {"channel_error", error},
                {"filament_detected", detected},
                {"module_exist", module},
                {"disable_auto", no_auto}}}}},
            {"filament_motion_sensor e" + std::to_string(slot) + "_filament", {{"enabled", true}}},
        };
        feed_status(frame.dump());
    }

    MoonrakerClientMock mock_client;
    helix::PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> api;
    std::unique_ptr<helix::test::RegisteredBackend<helix::AmsBackendSnapmaker>> raw_backend_;
};

} // namespace

// ============================================================================
// batch_feed_gcode — the pure script builder
// ============================================================================

TEST_CASE("Snapmaker batch_feed_gcode joins one AUTO_FEEDING line per slot", "[snapmaker][batch]") {
    SECTION("ordering is preserved, load verb") {
        REQUIRE(helix::AmsBackendSnapmaker::batch_feed_gcode({2, 0, 3}, true) ==
                "AUTO_FEEDING EXTRUDER=2 LOAD=1\n"
                "AUTO_FEEDING EXTRUDER=0 LOAD=1\n"
                "AUTO_FEEDING EXTRUDER=3 LOAD=1");
    }

    SECTION("unload verb") {
        REQUIRE(helix::AmsBackendSnapmaker::batch_feed_gcode({1, 3}, false) ==
                "AUTO_FEEDING EXTRUDER=1 UNLOAD=1\n"
                "AUTO_FEEDING EXTRUDER=3 UNLOAD=1");
    }

    SECTION("single slot has no join and no trailing newline") {
        const std::string gcode = helix::AmsBackendSnapmaker::batch_feed_gcode({0}, true);
        REQUIRE(gcode == "AUTO_FEEDING EXTRUDER=0 LOAD=1");
        REQUIRE(gcode.back() != '\n');
    }

    SECTION("empty input yields an empty string") {
        REQUIRE(helix::AmsBackendSnapmaker::batch_feed_gcode({}, true).empty());
        REQUIRE(helix::AmsBackendSnapmaker::batch_feed_gcode({}, false).empty());
    }
}

TEST_CASE("Snapmaker batch_feed_gcode drives AUTO_FEEDING_BATCH when the firmware has it",
          "[snapmaker][batch]") {
    SECTION("load with next-head preheat") {
        const std::string chain = helix::AmsBackendSnapmaker::batch_feed_gcode(
            {0, 2, 3}, /*load=*/true, /*use_batch_macro=*/true);

        CHECK(chain == "AUTO_FEEDING_BATCH ACTION=START\n"
                       "AUTO_FEEDING_BATCH ACTION=DOING EXTRUDER=0 LOAD=1 NEXT_EXTRUDER=2\n"
                       "AUTO_FEEDING_BATCH ACTION=DOING EXTRUDER=2 LOAD=1 NEXT_EXTRUDER=3\n"
                       "AUTO_FEEDING_BATCH ACTION=DOING EXTRUDER=3 LOAD=1\n"
                       "AUTO_FEEDING_BATCH ACTION=END");
    }

    SECTION("unload keeps the batch sentinels") {
        const std::string chain = helix::AmsBackendSnapmaker::batch_feed_gcode(
            {1, 3}, /*load=*/false, /*use_batch_macro=*/true);

        CHECK(chain == "AUTO_FEEDING_BATCH ACTION=START\n"
                       "AUTO_FEEDING_BATCH ACTION=DOING EXTRUDER=1 UNLOAD=1 NEXT_EXTRUDER=3\n"
                       "AUTO_FEEDING_BATCH ACTION=DOING EXTRUDER=3 UNLOAD=1\n"
                       "AUTO_FEEDING_BATCH ACTION=END");
    }

    SECTION("single slot has no NEXT_EXTRUDER") {
        const std::string chain = helix::AmsBackendSnapmaker::batch_feed_gcode(
            {2}, /*load=*/true, /*use_batch_macro=*/true);

        CHECK(chain == "AUTO_FEEDING_BATCH ACTION=START\n"
                       "AUTO_FEEDING_BATCH ACTION=DOING EXTRUDER=2 LOAD=1\n"
                       "AUTO_FEEDING_BATCH ACTION=END");
    }
}

// ============================================================================
// do_filament_batch — validation and the single-script dispatch
// ============================================================================

TEST_CASE_METHOD(BatchFixture, "Snapmaker batch dispatch sends ONE script for the whole batch",
                 "[snapmaker][batch]") {
    auto backend = snapmaker();

    SECTION("load batch reaches firmware as one multi-line script") {
        REQUIRE(backend->load_filament_batch({0, 2}).success());
        // The mock's gcode.script handler simulates a script line-by-line, so
        // the history records one entry per line. The RPC's own script param is
        // the wire truth: two separate RPCs would leave a single line in
        // last_send_script(), not the joined chain.
        REQUIRE(mock_client.last_send_script() == "AUTO_FEEDING EXTRUDER=0 LOAD=1\n"
                                                  "AUTO_FEEDING EXTRUDER=2 LOAD=1");
        REQUIRE(sent_gcodes().size() == 2);
    }

    SECTION("unload batch uses the unload verb") {
        REQUIRE(backend->unload_filament_batch({3}).success());
        const auto& sent = sent_gcodes();
        REQUIRE(sent.size() == 1);
        REQUIRE(sent[0] == "AUTO_FEEDING EXTRUDER=3 UNLOAD=1");
    }

    SECTION("an out-of-range slot refuses and sends nothing") {
        REQUIRE(backend->load_filament_batch({0, 9}).result == AmsResult::INVALID_SLOT);
        REQUIRE(sent_gcodes().empty());
    }

    SECTION("an empty batch refuses and sends nothing") {
        // AmsErrorHelper::invalid_parameter() reports as WRONG_STATE.
        REQUIRE(backend->load_filament_batch({}).result == AmsResult::WRONG_STATE);
        REQUIRE(sent_gcodes().empty());
    }
}

// ============================================================================
// set_discovery — the batch shape comes from the discovery handed to the
// backend, not the global PrinterState
// ============================================================================

TEST_CASE_METHOD(BatchFixture, "Batch shape follows the discovery the backend was handed",
                 "[snapmaker][batch]") {
    // Startup builds and starts backends from AmsState's hardware argument
    // BEFORE PrinterState publishes discovery globally, so the capability
    // must arrive through set_discovery() — never a global read.
    get_printer_state().set_hardware(helix::PrinterDiscovery{});
    REQUIRE_FALSE(get_printer_state().get_discovery().has_auto_feeding_batch());

    /// A backend built the way init_backends_from_hardware() builds one:
    /// discovery applied before start().
    auto make = [&](const helix::PrinterDiscovery& hw) {
        auto b = helix::AmsBackend::create(helix::AmsType::SNAPMAKER, api.get(), &mock_client);
        REQUIRE(b != nullptr);
        b->set_discovery(hw);
        REQUIRE(b->start().success());
        mock_client.clear_gcode_script_history();
        return b;
    };

    SECTION("a discovery carrying the macro dispatches the batch state machine") {
        helix::PrinterDiscovery hw;
        hw.parse_objects(nlohmann::json::array({"gcode_macro AUTO_FEEDING_BATCH"}));
        REQUIRE(hw.has_auto_feeding_batch());

        auto backend = make(hw);
        REQUIRE(backend->load_filament_batch({1, 3}).success());
        REQUIRE(mock_client.last_send_script() ==
                "AUTO_FEEDING_BATCH ACTION=START\n"
                "AUTO_FEEDING_BATCH ACTION=DOING EXTRUDER=1 LOAD=1 NEXT_EXTRUDER=3\n"
                "AUTO_FEEDING_BATCH ACTION=DOING EXTRUDER=3 LOAD=1\n"
                "AUTO_FEEDING_BATCH ACTION=END");
    }

    SECTION("a discovery without the macro keeps the per-head fallback") {
        helix::PrinterDiscovery bare;
        REQUIRE_FALSE(bare.has_auto_feeding_batch());

        auto backend = make(bare);
        REQUIRE(backend->load_filament_batch({1, 3}).success());
        REQUIRE(mock_client.last_send_script() == "AUTO_FEEDING EXTRUDER=1 LOAD=1\n"
                                                  "AUTO_FEEDING EXTRUDER=3 LOAD=1");
    }
}

// ============================================================================
// The shared gate: capability, claim, print refusal
// ============================================================================

TEST_CASE_METHOD(BatchFixture, "Batch filament ops are not_supported on a plain backend",
                 "[ams][batch]") {
    BatchlessBackend backend;
    REQUIRE_FALSE(backend.supports_batch_filament_ops());
    REQUIRE(backend.load_filament_batch({0}).result == AmsResult::NOT_SUPPORTED);
    REQUIRE(backend.unload_filament_batch({0}).result == AmsResult::NOT_SUPPORTED);
}

TEST_CASE_METHOD(BatchFixture, "Snapmaker advertises batch filament ops", "[ams][batch]") {
    auto backend = snapmaker();
    REQUIRE(backend->supports_batch_filament_ops());
}

TEST_CASE_METHOD(BatchFixture, "A batch is refused busy while another filament op holds the claim",
                 "[ams][batch][threading]") {
    ClaimParkingBackend backend;

    // Hold the claim from a second thread via a per-slot op parked inside its
    // hook. api_ is null so refuse_if_printing() short-circuits and the worker
    // never touches LVGL.
    std::thread holder([&] { (void)backend.load_filament(0); });
    backend.wait_for_parked_hook();

    const AmsError refused = backend.load_filament_batch({1});
    CHECK(refused.result == AmsResult::BUSY);

    backend.release_hook();
    holder.join();

    // The refusal must not have leaked a claim. A wedged one would answer BUSY
    // again; reaching do_filament_batch's null-api refusal instead proves the
    // batch was genuinely admitted through the gate the second time.
    REQUIRE(backend.load_filament_batch({1}).result == AmsResult::NOT_CONNECTED);
}

TEST_CASE_METHOD(BatchFixture, "A batch is refused while printing", "[ams][batch]") {
    auto backend = snapmaker();

    set_print_state(helix::PrintJobState::PRINTING);
    const AmsError refused = backend->load_filament_batch({0, 1});
    CHECK_FALSE(refused.success());
    CHECK(refused.result == AmsResult::WRONG_STATE);
    CHECK(sent_gcodes().empty());

    // A refused op releases its claim; the backend is not wedged busy.
    set_print_state(helix::PrintJobState::STANDBY);
    REQUIRE(backend->load_filament_batch({0, 1}).success());
    REQUIRE(mock_client.last_send_script() == "AUTO_FEEDING EXTRUDER=0 LOAD=1\n"
                                              "AUTO_FEEDING EXTRUDER=1 LOAD=1");
    REQUIRE(sent_gcodes().size() == 2);
}

// ============================================================================
// channel_snapshot — the per-channel feeder fields the status parse keeps
// ============================================================================

TEST_CASE_METHOD(BatchFixture, "Snapmaker keeps the per-channel feeder fields",
                 "[snapmaker][batch]") {
    feed_status(R"({"filament_feed left":{"extruder0":{
        "channel_state":"load_finish","channel_error":"ok","filament_detected":true,
        "module_exist":true,"disable_auto":false}}})");

    const auto snap = backend().channel_snapshot(0);

    CHECK(snap.state == "load_finish");
    CHECK(snap.error == "ok");
    CHECK(snap.filament_detected);
    CHECK(snap.module_exist);
    CHECK_FALSE(snap.disable_auto);
}

TEST_CASE_METHOD(BatchFixture, "A feeder delta keeps the fields it does not mention",
                 "[snapmaker][batch]") {
    feed_status(R"({"filament_feed left":{"extruder0":{
        "channel_state":"load_finish","channel_error":"ok","filament_detected":true,
        "module_exist":true,"disable_auto":false}}})");

    SECTION("a channel_state-only frame leaves the booleans standing") {
        feed_status(R"({"filament_feed left":{"extruder0":{
            "channel_state":"unload_finish"}}})");
        const auto snap = backend().channel_snapshot(0);
        CHECK(snap.state == "unload_finish");
        CHECK(snap.filament_detected); // not cleared by a frame silent on it
        CHECK(snap.module_exist);
        CHECK_FALSE(snap.disable_auto);
    }

    SECTION("a filament_detected-only frame leaves state standing") {
        feed_status(R"({"filament_feed left":{"extruder0":{"filament_detected":false}}})");
        const auto snap = backend().channel_snapshot(0);
        CHECK(snap.state == "load_finish"); // not blanked by a frame silent on it
        CHECK_FALSE(snap.filament_detected);
        CHECK(snap.module_exist);
    }

    SECTION("an error token persists until a frame carries a real value again") {
        feed_status(R"({"filament_feed left":{"extruder0":{"channel_error":"jam"}}})");
        CHECK(backend().channel_snapshot(0).error == "jam");
        // A frame omitting channel_error keeps the token...
        feed_status(R"({"filament_feed left":{"extruder0":{"channel_state":"load_finish"}}})");
        CHECK(backend().channel_snapshot(0).error == "jam");
        // ...and an explicit "ok" clears it.
        feed_status(R"({"filament_feed left":{"extruder0":{"channel_error":"ok"}}})");
        CHECK(backend().channel_snapshot(0).error == "ok");
    }
}

// ============================================================================
// batch cursor — the progress line the channel parse renders per head
// ============================================================================

TEST_CASE_METHOD(BatchFixture, "A finished batch leaves no progress line behind",
                 "[snapmaker][batch]") {
    helix::SnapmakerTestAccess::set_batch_plan(backend(), {0, 1}, /*load=*/true, "Load", "of");

    SECTION("mid-batch, the line names the head now in progress") {
        set_channel(0, "load_finish", "ok", /*detected=*/true, /*module=*/true, /*no_auto=*/false);
        CHECK(backend().batch_plan().cursor == 1);
        CHECK(backend().batch_plan().active);
        CHECK(backend().get_system_info().operation_detail == "Load 2 of 2");
    }

    SECTION("the final head verifies and clears the line") {
        set_channel(0, "load_finish", "ok", true, true, false);
        set_channel(1, "load_finish", "ok", true, true, false);
        CHECK(backend().batch_plan().cursor == 2);
        CHECK_FALSE(backend().batch_plan().active);
        CHECK(backend().get_system_info().operation_detail.empty());
    }
}

// ============================================================================
// slot_op_eligibility — the direction-dependent refusal, from channel state
// ============================================================================

TEST_CASE_METHOD(BatchFixture, "Snapmaker eligibility follows channel state",
                 "[snapmaker][batch]") {
    using E = helix::AmsBackend::FilamentOpEligibility;

    SECTION("preload_finish with filament loads, does not unload") {
        set_channel(0, "preload_finish", "ok", /*detected=*/true, /*module=*/true,
                    /*no_auto=*/false);
        CHECK(backend().slot_op_eligibility(0, /*load=*/true) == E::Eligible);
        CHECK(backend().slot_op_eligibility(0, /*load=*/false) == E::NotLoaded);
    }
    SECTION("load_finish unloads, does not load") {
        set_channel(0, "load_finish", "ok", true, true, false);
        CHECK(backend().slot_op_eligibility(0, /*load=*/false) == E::Eligible);
        CHECK(backend().slot_op_eligibility(0, /*load=*/true) == E::AlreadyLoaded);
    }
    SECTION("a manual feed that finished on the printer is settled") {
        // manual_sta_finish is a persistent terminal: a head that completed a
        // manual EXTRUDE sits in it until the next operation. It is not a
        // load (the classifier leaves the loaded latch clear), so an unload
        // still refuses with NotLoaded while a load may proceed.
        set_channel(0, "manual_sta_finish", "ok", /*detected=*/true, /*module=*/true,
                    /*no_auto=*/false);
        CHECK(backend().slot_op_eligibility(0, /*load=*/true) == E::Eligible);
        CHECK(backend().slot_op_eligibility(0, /*load=*/false) == E::NotLoaded);
    }
    SECTION("wait_insert with no filament is empty in both directions") {
        set_channel(0, "wait_insert", "ok", /*detected=*/false, true, false);
        CHECK(backend().slot_op_eligibility(0, true) == E::Empty);
        CHECK(backend().slot_op_eligibility(0, false) == E::Empty);
    }
    SECTION("a feeder fault beats everything") {
        set_channel(0, "load_finish", "jam", true, true, false);
        CHECK(backend().slot_op_eligibility(0, false) == E::Error);
    }
    SECTION("an empty lane's no_filament token is Empty, not a feeder error") {
        // The firmware reports no_filament for any lane without filament;
        // on a settled state it must not read as a fault.
        set_channel(0, "wait_insert", "no_filament", /*detected=*/false, true, false);
        CHECK(backend().slot_op_eligibility(0, true) == E::Empty);
        CHECK(backend().slot_op_eligibility(0, false) == E::Empty);
    }
    SECTION("an unsettled empty lane is still Empty") {
        // An idle empty lane reports an unsettled state; presence wins.
        set_channel(0, "none", "no_filament", false, true, false);
        CHECK(backend().slot_op_eligibility(0, true) == E::Empty);
    }
    SECTION("blank and none error tokens are not faults") {
        set_channel(0, "load_finish", "none", true, true, false);
        CHECK(backend().slot_op_eligibility(0, false) == E::Eligible);
        set_channel(0, "load_finish", "", true, true, false);
        CHECK(backend().slot_op_eligibility(0, false) == E::Eligible);
    }
    SECTION("manual mode or absent module refuses an otherwise eligible head") {
        set_channel(0, "preload_finish", "ok", true, /*module=*/true, /*no_auto=*/true);
        CHECK(backend().slot_op_eligibility(0, true) == E::FeederUnavailable);
        set_channel(1, "preload_finish", "ok", true, /*module=*/false, /*no_auto=*/false);
        CHECK(backend().slot_op_eligibility(1, true) == E::FeederUnavailable);
    }
    SECTION("an unrecognised state is busy, never eligible") {
        set_channel(0, "loading", "ok", true, true, false);
        CHECK(backend().slot_op_eligibility(0, true) == E::Busy);
        CHECK(backend().slot_op_eligibility(0, false) == E::Busy);
    }
    SECTION("a disabled motion sensor blocks an otherwise eligible load") {
        set_channel(0, "unload_finish", "ok", true, true, false);
        feed_status(R"({"filament_motion_sensor e0_filament":{"enabled":false}})");
        CHECK(backend().slot_op_eligibility(0, true) == E::SensorDisabled);
        feed_status(R"({"filament_motion_sensor e0_filament":{"enabled":true}})");
        CHECK(backend().slot_op_eligibility(0, true) == E::Eligible);
    }
    SECTION("an out-of-range slot is busy") {
        CHECK(backend().slot_op_eligibility(9, true) == E::Busy);
        CHECK(backend().slot_op_eligibility(-1, false) == E::Busy);
    }
}

TEST_CASE_METHOD(BatchFixture, "A backend without the override stays permissive", "[ams][batch]") {
    // The default must not change behaviour for AFC, Happy Hare, ACE or AD5X.
    BatchlessBackend backend;
    CHECK(backend.slot_op_eligibility(0, /*load=*/true) ==
          helix::AmsBackend::FilamentOpEligibility::Eligible);
    CHECK(backend.slot_op_eligibility(9, /*load=*/false) ==
          helix::AmsBackend::FilamentOpEligibility::Eligible);
}
