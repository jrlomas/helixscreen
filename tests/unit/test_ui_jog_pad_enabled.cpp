// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ui_jog_pad_enabled.cpp
 * @brief Tests for ui_jog_pad_set_enabled().
 *
 * The jog pad is a custom-drawn widget with no XML disabled binding. When the
 * printer is not ready, ui_jog_pad_set_enabled(pad, false) adds LV_STATE_DISABLED
 * so LVGL's input handling stops routing presses/clicks to the pad (verified in
 * lv_indev.c, which gates delivery on !lv_obj_has_state(obj, LV_STATE_DISABLED))
 * and the draw callback overlays a dimming scrim. This exercises that toggle.
 */

#include "../../include/ui_jog_pad.h"
#include "../../include/ui_panel_motion.h" // helix::JogMode
#include "../lvgl_test_fixture.h"

#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

struct LabelTaskInfo {
    const char* text;
    uint32_t text_local;
};

/// Records every label draw task the pad enqueues during a refresh.
void capture_label_tasks(lv_event_t* e) {
    lv_draw_task_t* task = lv_event_get_draw_task(e);
    if (!task || lv_draw_task_get_type(task) != LV_DRAW_TASK_TYPE_LABEL) {
        return;
    }
    auto* out = static_cast<std::vector<LabelTaskInfo>*>(lv_event_get_user_data(e));
    const lv_draw_label_dsc_t* dsc = lv_draw_task_get_label_dsc(task);
    out->push_back({dsc->text, dsc->text_local});
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "ui_jog_pad_set_enabled toggles LV_STATE_DISABLED",
                 "[jog_pad][ui]") {
    lv_obj_t* pad = ui_jog_pad_create(lv_screen_active());
    REQUIRE(pad != nullptr);

    // Created enabled: input flows to the pad normally.
    CHECK_FALSE(lv_obj_has_state(pad, LV_STATE_DISABLED));

    // Disable: LV_STATE_DISABLED set -> indev skips press/click, scrim drawn.
    ui_jog_pad_set_enabled(pad, false);
    CHECK(lv_obj_has_state(pad, LV_STATE_DISABLED));

    // Re-enable: state cleared.
    ui_jog_pad_set_enabled(pad, true);
    CHECK_FALSE(lv_obj_has_state(pad, LV_STATE_DISABLED));

    // Idempotent: re-enabling an already-enabled pad is a no-op, not a crash.
    ui_jog_pad_set_enabled(pad, true);
    CHECK_FALSE(lv_obj_has_state(pad, LV_STATE_DISABLED));

    // Null-safe.
    ui_jog_pad_set_enabled(nullptr, false);

    lv_obj_delete(pad);
}

TEST_CASE_METHOD(LVGLTestFixture, "Jog pad label draw tasks own their text", "[jog_pad][ui]") {
    lv_obj_t* pad = ui_jog_pad_create(lv_screen_active());
    REQUIRE(pad != nullptr);
    lv_obj_set_size(pad, 200, 200);
    ui_jog_pad_set_mode(pad, helix::JogMode::Coarse);

    std::vector<LabelTaskInfo> tasks;
    // The event fires only on objects carrying this flag (lv_draw.c gates on
    // it); a bare lv_obj_create does not set it, labels do.
    lv_obj_add_flag(pad, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_add_event_cb(pad, capture_label_tasks, LV_EVENT_DRAW_TASK_ADDED, &tasks);

    // Some displays in this suite are created without a flush callback, so a
    // refresh of one never completes. Give this one a callback that reports the
    // flush done, matching test_label_scroll_motion.cpp's helper.
    lv_display_t* display = lv_obj_get_display(pad);
    lv_display_set_flush_cb(
        display, [](lv_display_t* d, const lv_area_t*, uint8_t*) { lv_display_flush_ready(d); });
    lv_obj_invalidate(pad);
    lv_refr_now(display);

    lv_obj_remove_event_cb_with_user_data(pad, capture_label_tasks, &tasks);

    // The draw callback must actually have run. Without this the rest of the
    // assertions pass vacuously on an empty vector.
    REQUIRE_FALSE(tasks.empty());

    // The pad's ring labels are formatted into a by-value struct that dies with
    // the draw callback's frame, while on threaded builds the render thread reads
    // the text later. Every label task must therefore own its own copy.
    for (const auto& t : tasks) {
        CHECK(t.text_local == 1);
        CHECK(t.text != nullptr);
    }

    lv_obj_delete(pad);
}
