// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_state_action_detail.cpp
 * @brief Tests for AmsState::ams_action_detail subject derivation
 *
 * Verifies the priority logic that derives the user-visible AMS status
 * label from the combined view of AmsState and PrinterState:
 *   1. backend operation_detail (non-empty)
 *   2. ams_action != IDLE → action string
 *   3. PrintJobState::PRINTING → "Printing"
 *   4. PrintJobState::PAUSED  → "Paused"
 *   5. otherwise              → "Idle"
 *
 * Also verifies the observer wired to the print state subject so the
 * label refreshes without waiting for the next sync_from_backend().
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_globals.h"
#include "data_root_resolver.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "printer_state.h"
#include "translation_loader.h"

#include <filesystem>
#include <lvgl.h>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::printer;

namespace {

// Helper: read the current ams_action_detail subject string.
std::string detail_text() {
    return std::string(lv_subject_get_string(AmsState::instance().get_ams_action_detail_subject()));
}

// Helper: set print state via Moonraker-side subject + drain queue.
void set_print_state(PrintJobState state) {
    lv_subject_set_int(get_printer_state().get_print_state_enum_subject(), static_cast<int>(state));
    helix::ui::UpdateQueue::instance().drain();
}

// LVGL has no pack-unregister API, so selecting a language nothing has loaded
// makes every subsequent lookup miss again — the same restore idiom
// test_translation_loader.cpp uses.
class ScopedLanguage {
  public:
    ScopedLanguage() = default;
    ~ScopedLanguage() {
        lv_translation_set_language(helix::ui::kIdentityLocale);
    }
    ScopedLanguage(const ScopedLanguage&) = delete;
    ScopedLanguage& operator=(const ScopedLanguage&) = delete;
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "AmsState::ams_action_detail priority",
                 "[ams][ams_state][action_detail]") {
    auto& ams = AmsState::instance();
    auto& printer = get_printer_state();

    // PrinterState MUST be initialized before AmsState: AmsState's
    // install_print_state_observer() binds to PrinterState's print_state_enum
    // subject, which must already be live. This mirrors the production init
    // order in subject_initializer.cpp (PrinterState first, then AmsState).
    printer.init_subjects(false);
    ams.init_subjects(false);

    // Start in a known clean baseline.
    set_print_state(PrintJobState::STANDBY);
    ams.set_action(AmsAction::IDLE);
    ams.set_action_detail("");

    SECTION("action != IDLE wins regardless of print state (printing)") {
        set_print_state(PrintJobState::PRINTING);
        ams.set_action(AmsAction::LOADING);
        // The observer recomputes when the action changes.
        helix::ui::UpdateQueue::instance().drain();

        CHECK(detail_text() == "Loading");
    }

    SECTION("backend operation_detail wins when set (printing)") {
        set_print_state(PrintJobState::PRINTING);
        ams.set_action(AmsAction::IDLE);
        ams.set_action_detail("Waiting for slot 2");
        helix::ui::UpdateQueue::instance().drain();

        CHECK(detail_text() == "Waiting for slot 2");
    }

    SECTION("IDLE + empty detail + PRINTING -> Printing") {
        ams.set_action(AmsAction::IDLE);
        ams.set_action_detail("");
        set_print_state(PrintJobState::PRINTING);

        CHECK(detail_text() == "Printing");
    }

    SECTION("IDLE + empty detail + PAUSED -> Paused") {
        ams.set_action(AmsAction::IDLE);
        ams.set_action_detail("");
        set_print_state(PrintJobState::PAUSED);

        CHECK(detail_text() == "Paused");
    }

    SECTION("IDLE + empty detail + STANDBY -> Idle") {
        ams.set_action(AmsAction::IDLE);
        ams.set_action_detail("");
        set_print_state(PrintJobState::STANDBY);

        CHECK(detail_text() == "Idle");
    }

    SECTION("Observer flips label when print state changes mid-session") {
        // Start idle + standby: label should read "Idle".
        ams.set_action(AmsAction::IDLE);
        ams.set_action_detail("");
        set_print_state(PrintJobState::STANDBY);
        REQUIRE(detail_text() == "Idle");

        // Transition to PRINTING — observer should rerun derivation
        // without anyone calling sync_from_backend() or set_action()/set_action_detail().
        set_print_state(PrintJobState::PRINTING);
        CHECK(detail_text() == "Printing");

        // Pause it.
        set_print_state(PrintJobState::PAUSED);
        CHECK(detail_text() == "Paused");

        // Back to standby.
        set_print_state(PrintJobState::STANDBY);
        CHECK(detail_text() == "Idle");
    }

    // Restore clean state for any subsequent tests in the suite.
    set_print_state(PrintJobState::STANDBY);
    ams.set_action(AmsAction::IDLE);
    ams.set_action_detail("");
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "ams_action_detail holds the longest translated composition whole",
                 "[ams][ams_state][i18n]") {
    ScopedLanguage restore_lang;

    get_printer_state().init_subjects(false);
    auto& ams = AmsState::instance();
    ams.init_subjects(false);

    // Read the actual worst composition off the shipped translation packs
    // rather than pinning one locale by hand: which pack is longest shifts as
    // translations are edited, so a hardcoded copy stops proving anything the
    // moment it drifts from the catalog. CFS's load-failure verdict
    // (AmsBackendCfs::phase_verdict_message) is the longest known producer.
    const std::string cfs_verdict_key =
        "The filament did not reach the nozzle. The CFS reported no error, but "
        "the toolhead sensor still sees no filament. Check that the spool is "
        "seated and the path is clear, then try again.";
    std::string worst_detail;
    for (const auto& entry :
         std::filesystem::directory_iterator(helix::asset_path("ui_xml/translations"))) {
        const std::string stem = entry.path().stem().string();
        if (entry.path().extension() != ".xml" || stem == "translations") {
            continue; // translations.xml is the merged catalog, not a per-locale pack
        }
        helix::ui::ensure_translation_loaded(stem);
        lv_translation_set_language(stem.c_str());
        const std::string detail(lv_tr(cfs_verdict_key.c_str()));
        if (detail.size() > worst_detail.size()) {
            worst_detail = detail;
        }
    }

    REQUIRE(worst_detail.size() > 64); // otherwise this test cannot distinguish old from new

    // A toolchange narration an earlier test left latched outranks the
    // operation detail in recompute_action_detail(), so clear it first
    // (the same reset idiom test_afc_console_corpus.cpp uses).
    ams.set_narration_phase(-1, "");
    ams.set_action_detail(worst_detail);

    const std::string round_tripped(lv_subject_get_string(ams.get_ams_action_detail_subject()));
    CHECK(round_tripped == worst_detail);

    // last_operation_detail_ persists in the singleton; clear it so later
    // tests derive the detail from their own state.
    ams.set_action_detail("");
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "ams_action_detail truncates an overrunning composition on a codepoint boundary",
                 "[ams][ams_state][i18n]") {
    get_printer_state().init_subjects(false);
    auto& ams = AmsState::instance();
    ams.init_subjects(false);
    ams.set_narration_phase(-1, "");

    // A raw firmware message (AFC's message.message) is passed through with no
    // length bound at all, so a composition can still overrun the buffer no
    // matter how generously it is sized for the translated producers. Build
    // one out of a repeated 3-byte CJK codepoint so a byte-oriented cut always
    // has a chance to land mid-character.
    const std::string glyph = "\xE6\xB1\x9A"; // U+6C61, 3 bytes
    std::string oversized;
    for (int i = 0; i < 300; ++i) {
        oversized += glyph;
    }
    ams.set_action_detail(oversized);

    const std::string stored(lv_subject_get_string(ams.get_ams_action_detail_subject()));
    REQUIRE(stored.size() < oversized.size()); // otherwise this input doesn't overrun the buffer
    // Every glyph is 3 identical bytes, so a byte count that isn't a multiple
    // of 3 is the signature of a cut landing inside one of them.
    CHECK(stored.size() % glyph.size() == 0);

    ams.set_action_detail("");
}
