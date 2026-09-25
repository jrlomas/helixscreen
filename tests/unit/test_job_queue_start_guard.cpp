// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_job_queue_start_guard.cpp
 * @brief A queued job starts through the detail view and dies only on success.
 *
 * Run with: ./build/bin/helix-tests "[job_queue][queue_start]"
 *
 * PrintSelectPanel::start_queued_job() is the queue's single entry point (the
 * job queue modal's row tap today, the completion screen's "Start next" in
 * Phase 3). It owns three contracts:
 *
 * 1. The busy guard: can_start_new_print() covers BOTH the printer's reported
 *    state AND the app's committed-but-unconfirmed host-side start, so a tap
 *    during that window cannot delete the entry and lose the job.
 * 2. The entry outlives the attempt: remove_jobs and the stored-options delete
 *    run from the print-start SUCCESS callback, never from the tap. A missing
 *    file, a back-out and a failed start all leave the job queued.
 * 3. The saved option states seed the detail view's rows for the queued file
 *    (the merge rules themselves are pinned at the renderer level in
 *    test_print_select_detail_subjects.cpp and test_pre_print_options_renderer.cpp).
 */

#include "ui_job_queue_modal.h"
#include "ui_update_queue.h"

#include "../test_helpers/job_queue_modal_test_access.h"
#include "../test_helpers/print_select_panel_fixture.h"
#include "../test_helpers/print_select_panel_test_access.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "moonraker_api.h"
#include "printer_state.h"
#include "queued_job_options.h"
#include "test_helpers/pre_print_option_sets.h"
#include "test_helpers/printer_state_test_access.h"

#include <map>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::JobQueueModal;
using helix::PlantedGcode;
using helix::PrintJobState;
using helix::queue::QueuedJobOptions;
using helix::queue::QueuedJobOptionsMap;
using helix::test::make_skip_and_addon_set;
using helix::test::set_wire_state;

namespace {

/// Collects the user-facing warning toasts: toasts are compiled out of the
/// test build, so the hook is the only observable.
struct WarningLog {
    std::vector<std::string> messages;

    WarningLog() {
        helix::ui::set_test_notification_warning_hook(
            [this](const std::string& message) { messages.push_back(message); });
    }
    ~WarningLog() {
        helix::ui::set_test_notification_warning_hook(nullptr);
    }
    WarningLog(const WarningLog&) = delete;
    WarningLog& operator=(const WarningLog&) = delete;

    bool contains(const std::string& needle) const {
        for (const auto& m : messages) {
            if (m.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
};

/// Resets the shard-global PrinterState BEFORE the panel fixture builds the
/// real panel over it (bases construct in declaration order). Without this, a
/// prior case's subjects or preparing job decide this one's answers.
struct GlobalStateReset {
    GlobalStateReset() {
        auto& ps = get_printer_state();
        PrinterStateTestAccess::reset(ps);
        ps.init_subjects(false);
        if (ps.has_preparing_job()) {
            ps.retire_preparing(helix::PreparingExit::Superseded);
        }
        set_wire_state(ps, PrintJobState::STANDBY);
    }
};

class QueuedStartFixture : private GlobalStateReset, public helix::PrintSelectPanelFixture {
  public:
    QueuedStartFixture()
        : helix::PrintSelectPanelFixture(helix::PrintSelectFilelistHandler::Unregistered) {
        PrinterStateTestAccess::set_option_set(get_printer_state(), make_skip_and_addon_set());
        previous_api_ = get_moonraker_api();
        set_moonraker_api(api_.get());
        drain();
    }

    ~QueuedStartFixture() override {
        // With the panel and its api still alive: settle the state, then hand
        // the global api pointer back before the bases tear anything down.
        auto& ps = get_printer_state();
        if (ps.has_preparing_job()) {
            ps.retire_preparing(helix::PreparingExit::Superseded);
        }
        set_wire_state(ps, PrintJobState::STANDBY);
        drain();
        set_moonraker_api(previous_api_);
    }

    /// Writes a stored-options entry the way add_to_queue does. The mock
    /// dispatches inline, so the post answers before it returns.
    void seed_store(const std::string& job_id, const std::string& filename,
                    std::map<std::string, bool> options) {
        QueuedJobOptionsMap seed = read_store();
        seed[job_id] = QueuedJobOptions{filename, std::move(options)};
        api_->database_post_item(
            helix::queue::kOptionsDbNamespace, helix::queue::kOptionsDbKey,
            helix::queue::encode_queued_job_options(seed), []() {},
            [](const MoonrakerError&) { FAIL("seed post failed"); });
    }

    /// Reads the stored option map straight from the (mock) database.
    QueuedJobOptionsMap read_store() {
        QueuedJobOptionsMap out;
        bool answered = false;
        api_->database_get_item(
            helix::queue::kOptionsDbNamespace, helix::queue::kOptionsDbKey,
            [&out, &answered](const json& value) {
                out = helix::queue::decode_queued_job_options(value);
                answered = true;
            },
            [&answered](const MoonrakerError&) { answered = true; });
        REQUIRE(answered);
        return out;
    }

    /// Whether the job is still in Moonraker's queue (the mock answers
    /// synchronously, so a capturing lambda is safe here).
    bool queue_has(const std::string& job_id) {
        bool found = false;
        api_->queue().get_queue_status(
            [&found, &job_id](const JobQueueStatus& status) {
                for (const auto& job : status.queued_jobs) {
                    if (job.job_id == job_id) {
                        found = true;
                    }
                }
            },
            [](const MoonrakerError& err) { FAIL(err.message); });
        return found;
    }

    static JobQueueEntry entry(const std::string& job_id, const std::string& filename) {
        return JobQueueEntry{job_id, filename, 0.0, 0.0};
    }

    const std::string* pending_job_id() const {
        return ::PrintSelectPanelTestAccess::pending_queued_job_id(*panel_);
    }

  private:
    IMoonrakerAPI* previous_api_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(QueuedStartFixture,
                 "a queued job opens the detail view seeded and is removed only on confirmed start",
                 "[job_queue][queue_start]") {
    PlantedGcode file("queue_start_happy.gcode");
    seed_store("0001", file.name(), {{"bed_mesh", false}, {"timelapse", true}});

    panel_->start_queued_job(entry("0001", file.name()));
    drain();

    REQUIRE(::PrintSelectPanelTestAccess::detail_view_visible(*panel_));
    const std::string* pending = pending_job_id();
    REQUIRE(pending != nullptr);
    CHECK(*pending == "0001");

    // Saved states, not default_enabled, decide the rows for this file.
    const auto states = ::PrintSelectPanelTestAccess::collect_option_states(*panel_);
    REQUIRE(states.count("bed_mesh") == 1);
    CHECK(states.at("bed_mesh") == false);
    REQUIRE(states.count("timelapse") == 1);
    CHECK(states.at("timelapse") == true);

    // Nothing is removed until Moonraker confirms the print actually started:
    // opening the file, however far the pipeline gets, is not a start.
    CHECK(queue_has("0001"));
    REQUIRE(read_store().count("0001") == 1);

    ::PrintSelectPanelTestAccess::fire_print_started(*panel_);
    drain();

    CHECK_FALSE(queue_has("0001"));
    CHECK(read_store().empty());
    CHECK(pending_job_id() == nullptr);
}

TEST_CASE_METHOD(QueuedStartFixture,
                 "a queued file missing from the listing warns and stays queued",
                 "[job_queue][queue_start]") {
    WarningLog warnings;

    panel_->start_queued_job(entry("0003", "queue_start_ghost.gcode"));
    drain();

    // The refusal is answered, and neither the queue entry nor a stale
    // pending start is left behind.
    CHECK(warnings.contains("File not found"));
    CHECK(warnings.contains("queue_start_ghost.gcode"));
    CHECK(pending_job_id() == nullptr);
    CHECK(queue_has("0003"));
}

TEST_CASE_METHOD(QueuedStartFixture, "a queued job refuses to start while the printer is busy",
                 "[job_queue][queue_start][print_state]") {
    auto& ps = get_printer_state();

    SECTION("printing") {
        set_wire_state(ps, PrintJobState::PRINTING);
    }
    SECTION("paused") {
        set_wire_state(ps, PrintJobState::PAUSED);
    }
    SECTION("host-side pre-print block") {
        // The wire still reads standby here: the guard must consult the app's
        // committed start too, or the entry is deleted into a failed start.
        ps.begin_preparing(helix::PrintJobRef{"other.gcode", "", ""});
        drain();
        REQUIRE(ps.get_print_job_state() == PrintJobState::STANDBY);
        REQUIRE(ps.is_print_in_progress());
    }
    drain();

    WarningLog warnings;
    panel_->start_queued_job(entry("0002", "calibration_cube.gcode"));
    drain();

    CHECK(warnings.contains("calibration_cube.gcode"));
    CHECK(pending_job_id() == nullptr);
    CHECK(queue_has("0002"));
    CHECK_FALSE(::PrintSelectPanelTestAccess::detail_view_visible(*panel_));
}

TEST_CASE_METHOD(
    QueuedStartFixture,
    "closing the queued file's detail view drops the pending start once no start is in flight",
    "[job_queue][queue_start]") {
    PlantedGcode file("queue_start_backout.gcode");
    seed_store("0001", file.name(), {{"bed_mesh", false}});

    panel_->start_queued_job(entry("0001", file.name()));
    drain();
    REQUIRE(::PrintSelectPanelTestAccess::detail_view_visible(*panel_));

    SECTION("a Print tap in flight survives the close") {
        // A failed start re-shows the view; the pipeline closing it here must
        // not cancel the bookkeeping that removes the job on success.
        ::PrintSelectPanelTestAccess::set_pending_start_attempted(*panel_, true);
        ::PrintSelectPanelTestAccess::hide_detail_view(*panel_);
        drain();

        const std::string* pending = pending_job_id();
        REQUIRE(pending != nullptr);
        CHECK(*pending == "0001");
        CHECK(queue_has("0001"));
    }

    SECTION("a plain back-out clears the pending start") {
        ::PrintSelectPanelTestAccess::hide_detail_view(*panel_);
        drain();

        CHECK(pending_job_id() == nullptr);
        CHECK(queue_has("0001"));

        // Starting the same job again re-opens seeded: the saved states are
        // keyed to the job, not to whichever show happened first.
        panel_->start_queued_job(entry("0001", file.name()));
        drain();
        REQUIRE(::PrintSelectPanelTestAccess::detail_view_visible(*panel_));
        const auto states = ::PrintSelectPanelTestAccess::collect_option_states(*panel_);
        REQUIRE(states.count("bed_mesh") == 1);
        CHECK(states.at("bed_mesh") == false);
    }
}

TEST_CASE_METHOD(QueuedStartFixture, "opening a different file discards the pending queued start",
                 "[job_queue][queue_start]") {
    PlantedGcode queued_file("queue_start_diff_queued.gcode");
    PlantedGcode other_file("queue_start_diff_other.gcode");
    seed_store("0001", queued_file.name(), {{"bed_mesh", false}});

    panel_->start_queued_job(entry("0001", queued_file.name()));
    drain();
    REQUIRE(::PrintSelectPanelTestAccess::detail_view_visible(*panel_));
    REQUIRE(pending_job_id() != nullptr);

    // A start is in flight (so the close itself keeps the pending start), and
    // the user then picks a different file: the queued job's saved states
    // must not leak onto that file's rows, and the job stays queued for its
    // own row tap.
    ::PrintSelectPanelTestAccess::set_pending_start_attempted(*panel_, true);
    ::PrintSelectPanelTestAccess::hide_detail_view(*panel_);
    drain();
    REQUIRE(pending_job_id() != nullptr);

    REQUIRE(panel_->select_file_by_name(other_file.name()));
    drain();

    CHECK(pending_job_id() == nullptr);
    CHECK(queue_has("0001"));
    const auto states = ::PrintSelectPanelTestAccess::collect_option_states(*panel_);
    REQUIRE(states.count("bed_mesh") == 1);
    CHECK(states.at("bed_mesh") == true);
}

TEST_CASE_METHOD(QueuedStartFixture,
                 "the job queue modal delegates its row tap to the panel entry point",
                 "[job_queue][queue_start]") {
    PlantedGcode file("queue_start_modal.gcode");

    JobQueueModal modal;
    JobQueueModalTestAccess::start_job(modal, "0002", file.name());
    drain();

    // The modal hides itself and routes the tap to the global print select
    // panel's start_queued_job(). That panel is created lazily and never
    // set up in tests (no XML, no file provider), so its file list is empty
    // and the pending start waits for a listing that never arrives - which
    // is exactly what makes this a clean delegation proof: the pending start
    // EXISTS on that panel, and the queue is untouched.
    const std::string* pending =
        ::PrintSelectPanelTestAccess::pending_queued_job_id(get_global_print_select_panel());
    REQUIRE(pending != nullptr);
    CHECK(*pending == "0002");
    CHECK(queue_has("0002"));
}
