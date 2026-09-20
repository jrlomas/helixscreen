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
#include "ams_backend.h"
#include "ams_backend_happy_hare.h"
#include "ams_backend_snapmaker.h"
#include "ams_error.h"
#include "ams_types.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

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

    MoonrakerClientMock mock_client;
    helix::PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> api;
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
