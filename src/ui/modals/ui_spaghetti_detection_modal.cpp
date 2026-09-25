// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_spaghetti_detection_modal.h"

#include "ui_toast_manager.h"

#include "abort_manager.h"
#include "app_globals.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "settings_manager.h"

#include <spdlog/spdlog.h>

#include <memory>

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

namespace helix::detection {

void present_detection(const DetectionEvent& e, DetectionPolicy p) {
    using DetectionResponse = helix::detection::DetectionResponse;
    const auto response = DetectionManager::instance().response_for(p);
    if (response == DetectionResponse::Suppressed)
        return;
    if (response == DetectionResponse::WarnOnly) {
        ToastManager::instance().show(ToastSeverity::WARNING, lv_tr("Spaghetti detected"), 8000);
        return;
    }
    // PauseAndRespond: sources only report, so a print the source did not
    // already pause (it paused itself) pauses here. With pause-on-detect off
    // this branch is never reached.
    if (!e.already_paused) {
        get_moonraker_api()->job().pause_print([] { spdlog::info("[Detection] print paused"); },
                                               [](const MoonrakerError& err) {
                                                   spdlog::warn("[Detection] pause failed: {}",
                                                                err.message);
                                               });
    }
    // Stack-owned via Modal::show_owned() (#1382): ModalStack frees the
    // instance when its entry goes, on every teardown path.
    auto modal = std::make_unique<SpaghettiDetectionModal>();
    // TODO(#1506): no frame yet; the modal shows the detector's text only
    modal->set_detection(e.message, nullptr);
    modal->set_on_resume(
        [] { get_moonraker_api()->job().resume_print([] {}, [](const MoonrakerError&) {}); });
    modal->set_on_abort([] { helix::AbortManager::instance().start_abort(); });
    modal->set_on_disable([] {
        SettingsManager::instance().set_detection_enabled(false);
        ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Detection turned off"), 4000);
    });
    // VENDOR_OK: DEFECT_DETECTION_CONFIG is the tuning macro of the stock
    // firmware that exposes can_tune(); sources running their own model (the
    // K2 polls it directly) decline the button.
    if (DetectionManager::instance().source_can_tune(e.source_id))
        modal->set_on_tune([] {
            // Null callbacks, not empty lambdas: a non-null error_cb reads
            // as "this caller reports the failure itself", which would
            // suppress Klipper's `!!` broadcast for a rejected
            // DEFECT_DETECTION_CONFIG and leave the user with nothing.
            get_moonraker_client()->send_jsonrpc(
                "printer.gcode.script",
                nlohmann::json{{"script", "DEFECT_DETECTION_CONFIG NOODLE_SENSITIVITY=low"}},
                nullptr, nullptr);
        });
    Modal::show_owned(std::move(modal), lv_screen_active());
}

} // namespace helix::detection
