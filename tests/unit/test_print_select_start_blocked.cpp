// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_select_start_blocked.cpp
 * @brief Starting a print from print select stays blocked while one runs.
 *
 * print_select_can_print drives the detail view's Print button (disabled at 0)
 * and print_select_blocked_reason carries the one-line why shown beside it.
 * The state-transition observers are registered by setup(), so the fixture
 * builds the real XML tree and runs setup() the way the app does; without it
 * the panel still computes a correct initial state but never reacts to a
 * transition. The second case pins the init-time compute alone, which is the
 * mid-print entry path: the panel's subjects are created while the enum
 * already holds PRINTING, so no transition ever fires
 * (prestonbrown/helixscreen#1395).
 */

#include "ui_panel_print_select.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "printer_state.h"

#include <lvgl.h>
#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::PrintJobState;
using helix::ui::UpdateQueue;

namespace {

/// The real panel over the real XML: subjects, widget tree and the
/// state-transition observers that setup() registers.
struct StartBlockedFixture : public LVGLUITestFixture {
    StartBlockedFixture() {
        panel_ = std::make_unique<PrintSelectPanel>(state(), nullptr);
        panel_->init_subjects();
        root_ = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "print_select_panel", nullptr));
        REQUIRE(root_ != nullptr);
        panel_->setup(root_, test_screen());
    }

    ~StartBlockedFixture() override {
        // XML tree first: its bindings observe subjects the panel owns.
        UpdateQueue::instance().drain();
        if (root_ && lv_obj_is_valid(root_)) {
            lv_obj_delete(root_);
        }
        root_ = nullptr;
        UpdateQueue::instance().drain();
        panel_.reset();
        UpdateQueue::instance().drain();
    }

    void set_print_state(PrintJobState s) {
        lv_subject_set_int(state().get_print_state_enum_subject(), static_cast<int>(s));
        UpdateQueue::instance().drain();
    }

    int can_print() const {
        return lv_subject_get_int(lv_xml_get_subject(nullptr, "print_select_can_print"));
    }

    std::string blocked_reason() const {
        lv_subject_t* s = lv_xml_get_subject(nullptr, "print_select_blocked_reason");
        return s ? lv_subject_get_string(s) : std::string("<missing>");
    }

    std::unique_ptr<PrintSelectPanel> panel_;
    lv_obj_t* root_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(StartBlockedFixture, "print select blocks start and states why while printing",
                 "[print_select][1395]") {
    // Idle baseline: printable, no reason on screen.
    set_print_state(PrintJobState::STANDBY);
    REQUIRE(can_print() == 1);
    CHECK(blocked_reason().empty());

    // Every state that holds the machine blocks the start and says why.
    for (PrintJobState s : {PrintJobState::PRINTING, PrintJobState::PAUSED}) {
        set_print_state(s);
        CAPTURE(static_cast<int>(s));
        CHECK(can_print() == 0);
        CHECK_FALSE(blocked_reason().empty());
    }

    // Terminal states release the block and clear the reason.
    set_print_state(PrintJobState::COMPLETE);
    CHECK(can_print() == 1);
    CHECK(blocked_reason().empty());
}

TEST_CASE_METHOD(LVGLUITestFixture, "a panel initialized mid-print starts blocked",
                 "[print_select][1395]") {
    // The state holds BEFORE the panel's subjects exist, which is the mid-print
    // entry path: no transition ever fires after init, so only the init-time
    // update_print_button_state() call can have published the blocked state.
    lv_subject_set_int(state().get_print_state_enum_subject(),
                       static_cast<int>(PrintJobState::PRINTING));

    auto panel = std::make_unique<PrintSelectPanel>(state(), nullptr);
    panel->init_subjects();

    lv_subject_t* can_print = lv_xml_get_subject(nullptr, "print_select_can_print");
    REQUIRE(can_print != nullptr);
    CHECK(lv_subject_get_int(can_print) == 0);

    lv_subject_t* reason = lv_xml_get_subject(nullptr, "print_select_blocked_reason");
    REQUIRE(reason != nullptr);
    CHECK_FALSE(std::string(lv_subject_get_string(reason)).empty());

    UpdateQueue::instance().drain();
    panel.reset();
    UpdateQueue::instance().drain();
}
