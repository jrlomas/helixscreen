// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_spaghetti_detection_modal.h"

// LVGLUITestFixture registers ALL XML components (via
// helix::register_xml_components()), including spaghetti_detection_modal.xml,
// so the modal can be created from XML inside the test.
#include "ui_toast_manager.h"

#include "../lvgl_ui_test_fixture.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "settings_manager.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

// NOTE: SpaghettiDetectionModal is a one-shot modal owned by its ModalStack
// entry (Modal::show_owned, #1382): the entry frees it one tick after any
// close. These tests keep a raw view of the instance across show_owned() and
// stop touching it once process_lvgl() has drained the deferred free. A stack-
// allocated modal would outlive its own entry's free, so do NOT switch these
// to stack.

TEST_CASE_METHOD(LVGLUITestFixture, "SpaghettiDetectionModal shows message + invokes callbacks",
                 "[detection][modal][.ui_integration]") {
    int resumed = 0, aborted = 0, tuned = 0, disabled = 0;

    // Resume path: the stack frees the modal after the close.
    {
        auto owned = std::make_unique<SpaghettiDetectionModal>();
        auto* modal = owned.get();
        modal->set_on_resume([&] { ++resumed; });
        modal->set_on_abort([&] { ++aborted; });
        modal->set_on_tune([&] { ++tuned; });
        modal->set_detection("detected noodle", nullptr);
        REQUIRE(Modal::show_owned(std::move(owned), test_screen()));
        REQUIRE(modal->is_visible());
        modal->invoke_resume_for_test(); // invokes callback then hide(); stack frees after
        REQUIRE(resumed == 1);
        REQUIRE(aborted == 0);
        process_lvgl(50); // drain the entry's deferred free; do not touch modal after
    }

    // Abort path.
    {
        auto owned = std::make_unique<SpaghettiDetectionModal>();
        auto* modal = owned.get();
        modal->set_on_abort([&] { ++aborted; });
        modal->set_detection("detected noodle", nullptr);
        REQUIRE(Modal::show_owned(std::move(owned), test_screen()));
        REQUIRE(modal->is_visible());
        modal->invoke_abort_for_test();
        REQUIRE(aborted == 1);
        REQUIRE(resumed == 1);
        process_lvgl(50);
    }

    // Tune does NOT hide; it only invokes the callback. The modal stays alive
    // until we hide() it explicitly.
    {
        auto owned = std::make_unique<SpaghettiDetectionModal>();
        auto* modal = owned.get();
        modal->set_on_tune([&] { ++tuned; });
        modal->set_detection("detected noodle", nullptr);
        REQUIRE(Modal::show_owned(std::move(owned), test_screen()));
        modal->invoke_tune_for_test();
        REQUIRE(tuned == 1);
        REQUIRE(modal->is_visible()); // still visible: Tune does not hide
        modal->hide();                // the entry frees the instance a tick later
        process_lvgl(50);
    }

    // Turn off detection: same shape as Tune — the print decision stays open,
    // so the action fires without hiding the modal.
    {
        auto owned = std::make_unique<SpaghettiDetectionModal>();
        auto* modal = owned.get();
        modal->set_on_disable([&] { ++disabled; });
        modal->set_detection("detected noodle", nullptr);
        REQUIRE(Modal::show_owned(std::move(owned), test_screen()));
        modal->invoke_disable_for_test();
        REQUIRE(disabled == 1);
        REQUIRE(modal->is_visible()); // still visible: Disable does not hide
        modal->hide();
        process_lvgl(50);
    }
}

// ---------------------------------------------------------------------------
// present_detection: the response ladder, end to end
// ---------------------------------------------------------------------------
// DetectionManager maps settings + policy to Suppressed / WarnOnly /
// PauseAndRespond; present_detection is what each response does to the user.
// A mock print makes the pause half real: SDCARD_PRINT_FILE reaches PREHEAT
// synchronously, PREHEAT is pausable, and printer.print.pause is a
// synchronous mock handler.

namespace {

using ToastRecord = std::pair<ToastSeverity, std::string>;

class PresenterFixture : public LVGLUITestFixture {
  public:
    PresenterFixture() : mock_client(MoonrakerClientMock::PrinterType::VORON_24) {
        SettingsManager::instance().init_subjects();
        helix::detection::DetectionManager::instance().reset_for_test();

        mock_client.connect("ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<MoonrakerAPI>(mock_client, state);
        previous_api_ = get_moonraker_api();
        set_moonraker_api(api.get());
        get_printer_state().init_subjects(false);

        helix::ui::set_test_toast_hook(
            [this](ToastSeverity s, const std::string& m) { toasts.emplace_back(s, m); });
    }

    ~PresenterFixture() override {
        helix::ui::set_test_toast_hook(nullptr);
        set_moonraker_api(previous_api_);
        mock_client.stop_temperature_simulation();
        mock_client.disconnect();
        api.reset();
    }

    helix::detection::DetectionEvent spaghetti_event(bool already_paused) const {
        helix::detection::DetectionEvent e;
        e.source_id = "u1_stock";
        e.kind = helix::detection::DetectionKind::Spaghetti;
        e.attributable = true;
        e.already_paused = already_paused;
        e.message = "noodle detected";
        return e;
    }

    MoonrakerClientMock mock_client;
    helix::PrinterState state;
    std::unique_ptr<MoonrakerAPI> api;
    std::vector<ToastRecord> toasts;

  private:
    IMoonrakerAPI* previous_api_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(PresenterFixture, "present_detection response ladder",
                 "[detection][modal][presenter]") {
    using helix::detection::DetectionPolicy;
    auto& sm = SettingsManager::instance();

    SECTION("Suppressed: detection off shows nothing and pauses nothing") {
        sm.set_detection_enabled(false);
        helix::detection::present_detection(spaghetti_event(false), DetectionPolicy::DeferToSource);

        CHECK(toasts.empty());
        CHECK(ModalStack::instance().top_component_name().empty());
        CHECK(mock_client.get_print_phase() == MoonrakerClientMock::MockPrintPhase::IDLE);
    }

    SECTION("WarnOnly: notify policy warns and leaves the print alone") {
        sm.set_detection_enabled(true);
        sm.set_detection_pause_on_detect(true); // policy, not the setting, keeps this warn-only
        helix::detection::present_detection(spaghetti_event(false), DetectionPolicy::NotifyOnly);

        REQUIRE(toasts.size() == 1);
        CHECK(toasts[0].first == ToastSeverity::WARNING);
        CHECK(toasts[0].second == "Spaghetti detected");
        CHECK(ModalStack::instance().top_component_name().empty());
        CHECK(mock_client.get_print_phase() == MoonrakerClientMock::MockPrintPhase::IDLE);
    }

    SECTION("PauseAndRespond: a print the source did not pause pauses here") {
        sm.set_detection_enabled(true);
        sm.set_detection_pause_on_detect(true);
        mock_client.gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");
        REQUIRE(mock_client.get_print_phase() == MoonrakerClientMock::MockPrintPhase::PREHEAT);

        helix::detection::present_detection(spaghetti_event(false), DetectionPolicy::DeferToSource);

        CHECK(mock_client.get_print_phase() == MoonrakerClientMock::MockPrintPhase::PAUSED);
        CHECK(ModalStack::instance().top_component_name() == "spaghetti_detection_modal");
        CHECK(toasts.empty());
        ModalStack::instance().clear();
    }

    SECTION("PauseAndRespond: an already-paused print is not paused twice") {
        sm.set_detection_enabled(true);
        sm.set_detection_pause_on_detect(true);
        mock_client.gcode_script("SDCARD_PRINT_FILE FILENAME=3DBenchy.gcode");
        REQUIRE(mock_client.get_print_phase() == MoonrakerClientMock::MockPrintPhase::PREHEAT);

        helix::detection::present_detection(spaghetti_event(true), DetectionPolicy::DeferToSource);

        // Still PREHEAT: the presenter skipped pause_print because the source
        // reported the print already paused.
        CHECK(mock_client.get_print_phase() == MoonrakerClientMock::MockPrintPhase::PREHEAT);
        CHECK(ModalStack::instance().top_component_name() == "spaghetti_detection_modal");
        ModalStack::instance().clear();
    }
}
