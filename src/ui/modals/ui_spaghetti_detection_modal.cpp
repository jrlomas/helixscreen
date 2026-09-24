// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_spaghetti_detection_modal.h"

#include <spdlog/spdlog.h>

void SpaghettiDetectionModal::on_show() {
    // Wire the four action buttons programmatically (mirrors the runout-guidance
    // modal). No XML callbacks on these buttons, so there's no double-wiring.
    //   btn_primary    → on_ok()        → Resume
    //   btn_secondary  → on_cancel()    → Abort
    //   btn_tertiary   → on_tertiary()  → Tune (hidden when the source can't tune)
    //   btn_quaternary → on_quaternary()→ Turn off detection
    wire_ok_button("btn_primary");
    wire_cancel_button("btn_secondary");
    wire_tertiary_button("btn_tertiary");
    wire_quaternary_button("btn_quaternary");

    // A dead Tune button is worse than none: hide it, with its divider, when
    // this source exposes no tuning. (Mirrors the preview hiding below — this
    // modal wires itself.)
    const bool show_tune = static_cast<bool>(on_tune_);
    if (lv_obj_t* tune_btn = find_widget("btn_tertiary")) {
        if (show_tune)
            lv_obj_remove_flag(tune_btn, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(tune_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (lv_obj_t* tune_div = find_widget("divider_tune")) {
        if (show_tune)
            lv_obj_remove_flag(tune_div, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(tune_div, LV_OBJ_FLAG_HIDDEN);
    }

    // Message text.
    lv_obj_t* text = find_widget("detection_text");
    if (text) {
        lv_label_set_text(text, message_.c_str());
    } else {
        spdlog::warn("[SpaghettiDetectionModal] detection_text widget not found");
    }

    // Optional camera frame preview. Hide the image entirely when no frame is
    // available so it doesn't reserve empty space.
    lv_obj_t* preview = find_widget("detection_preview");
    if (preview) {
        if (frame_) {
            lv_image_set_src(preview, frame_);
            lv_obj_remove_flag(preview, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(preview, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        spdlog::warn("[SpaghettiDetectionModal] detection_preview widget not found");
    }
}
