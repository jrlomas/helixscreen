// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_cfs_chute_calibration_overlay.cpp
 * @brief Guided purge-chute calibration for the Creality CFS (K1 dialect)
 */

#if HELIX_HAS_CFS

#include "ui_cfs_chute_calibration_overlay.h"

#include "ui_callback_helpers.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_nav_manager.h"
#include "ui_position_utils.h"

#include "ams_backend_cfs.h"
#include "ams_state.h"
#include "app_globals.h"
#include "jog_coalescer.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "static_panel_registry.h"
#include "unit_conversions.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdlib>
#include <memory>

namespace helix::ui {

using helix::printer::AmsBackendCfs;

// ============================================================================
// SINGLETON ACCESSOR
// ============================================================================

static std::unique_ptr<CfsChuteCalibrationOverlay> g_chute_overlay;

CfsChuteCalibrationOverlay& get_cfs_chute_calibration_overlay() {
    if (!g_chute_overlay) {
        g_chute_overlay = std::make_unique<CfsChuteCalibrationOverlay>();
        StaticPanelRegistry::instance().register_destroy("CfsChuteCalibrationOverlay",
                                                         []() { g_chute_overlay.reset(); });
    }
    return *g_chute_overlay;
}

// The step subject is static (like the z-offset calibration panel's): XML
// bindings resolve it by name from the current scope, and SubjectManager
// deinit on the singleton keeps observers disconnected across rebuilds.
static lv_subject_t s_cfs_chute_state;
static lv_subject_t s_cfs_chute_y_text;
static char s_cfs_chute_y_buf[16];
static lv_subject_t s_cfs_chute_saved_text;
static char s_cfs_chute_saved_buf[96];
static bool s_callbacks_registered = false;

namespace {

/// The CFS backend, or nullptr when the active filament system is not a CFS.
AmsBackendCfs* chute_backend() {
    auto* backend = AmsState::instance().get_backend();
    if (!backend || backend->get_type() != AmsType::CFS) {
        return nullptr;
    }
    return static_cast<AmsBackendCfs*>(backend);
}

} // namespace

CfsChuteCalibrationOverlay::CfsChuteCalibrationOverlay() {
    spdlog::debug("[CfsChuteCal] Created");
}

CfsChuteCalibrationOverlay::~CfsChuteCalibrationOverlay() {
    if (subjects_initialized_) {
        subjects_.deinit_all();
        subjects_initialized_ = false;
    }
    overlay_root_ = nullptr;
}

// ============================================================================
// SUBJECTS / CALLBACKS
// ============================================================================

void CfsChuteCalibrationOverlay::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    UI_MANAGED_SUBJECT_INT(s_cfs_chute_state, 0, "cfs_chute_state", subjects_);
    UI_MANAGED_SUBJECT_STRING(s_cfs_chute_y_text, s_cfs_chute_y_buf, "0.00 mm",
                              "cfs_chute_y_display", subjects_);
    UI_MANAGED_SUBJECT_STRING(s_cfs_chute_saved_text, s_cfs_chute_saved_buf, "",
                              "cfs_chute_saved_pos", subjects_);
    subjects_initialized_ = true;

    if (!s_callbacks_registered) {
        register_xml_callbacks({
            {"on_chute_start_clicked", on_start_clicked},
            {"on_chute_jog_clicked", on_jog_clicked},
            {"on_chute_save_clicked", on_save_clicked},
            {"on_chute_cancel_clicked", on_cancel_clicked},
            {"on_chute_done_clicked", on_done_clicked},
        });
        s_callbacks_registered = true;
    }
}

void CfsChuteCalibrationOverlay::register_callbacks() {
    // Callbacks are registered alongside the subjects in init_subjects(), the
    // z-offset calibration panel's arrangement: one flag, one place.
}

// ============================================================================
// UI CREATION / LIFECYCLE
// ============================================================================

lv_obj_t* CfsChuteCalibrationOverlay::create(lv_obj_t* parent) {
    if (overlay_root_) {
        return overlay_root_;
    }

    overlay_root_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent, "cfs_chute_calibration_overlay", nullptr));
    if (!overlay_root_) {
        spdlog::error("[CfsChuteCal] Failed to create overlay from XML");
        return nullptr;
    }

    // Live toolhead Y: the save step writes whatever position the head is in,
    // so the readout is the value being calibrated.
    auto& ps = get_printer_state();
    y_observer_ = observe_int_sync<CfsChuteCalibrationOverlay>(
        ps.get_position_y_subject(), this,
        [](CfsChuteCalibrationOverlay* self, int centimm) { self->update_y_display(centimm); },
        ps.get_subjects_lifetime());

    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);
    return overlay_root_;
}

void CfsChuteCalibrationOverlay::show(lv_obj_t* parent_screen) {
    init_subjects();

    if (!overlay_root_ && parent_screen) {
        create(parent_screen);
    }
    if (!overlay_root_) {
        spdlog::error("[CfsChuteCal] Cannot show - overlay not created");
        return;
    }

    NavigationManager::instance().register_overlay_instance(overlay_root_, this);
    NavigationManager::instance().push_overlay(overlay_root_);
}

void CfsChuteCalibrationOverlay::on_activate() {
    OverlayBase::on_activate();
    set_state(State::IDLE);
}

void CfsChuteCalibrationOverlay::on_deactivating(DeactivateReason reason) {
    // Leaving while adjusting leaves the toolhead off-park; re-park it. Before
    // ADJUSTING there is nothing to unwind (the home cannot be aborted), and
    // during SAVING the save script parks Y itself.
    if (reason != DeactivateReason::Shutdown) {
        auto* backend = chute_backend();
        if (backend) {
            const int state = lv_subject_get_int(&s_cfs_chute_state);
            if (state == static_cast<int>(State::ADJUSTING)) {
                backend->exit_chute_calibration();
            }
        }
    }
}

// ============================================================================
// STEP MACHINE
// ============================================================================

void CfsChuteCalibrationOverlay::set_state(State state) {
    lv_subject_set_int(&s_cfs_chute_state, static_cast<int>(state));
    if (state == State::ADJUSTING) {
        edge_warned_y_ = false;
    }
}

void CfsChuteCalibrationOverlay::start_calibration() {
    auto* backend = chute_backend();
    if (!backend) {
        return;
    }
    set_state(State::HOMING);
    auto err = backend->start_chute_calibration([this]() { set_state(State::ADJUSTING); });
    if (!err.success()) {
        set_state(State::IDLE);
        notify_ams_error(err);
    }
}

void CfsChuteCalibrationOverlay::handle_jog(double delta_mm) {
    auto* backend = chute_backend();
    if (!backend) {
        return;
    }

    // Soft-stop against the live envelope. The jog script's M400 means each
    // RPC returns with the move finished, so there is no uncommitted travel
    // to fold in (unlike the motion panel's coalescer).
    double delta = delta_mm;
    const auto bounds = get_printer_state().get_axis_bounds();
    if (bounds.has_y && y_known_) {
        const auto result =
            helix::clamp_jog_with_warn(current_y_mm_, 0.0, delta, static_cast<double>(bounds.y_min),
                                       static_cast<double>(bounds.y_max), edge_warned_y_);
        edge_warned_y_ = result.latch;
        delta = result.allowed;
        if (result.warn) {
            NOTIFY_INFO(lv_tr("Y axis limit reached"));
        }
    }
    if (std::abs(delta) <= helix::AxisMove::EPSILON_MM) {
        return;
    }
    backend->jog_chute_y(static_cast<float>(delta));
}

void CfsChuteCalibrationOverlay::save_position() {
    auto* backend = chute_backend();
    if (!backend) {
        return;
    }
    set_state(State::SAVING);
    // The done screen names the pair the firmware wrote when the response
    // line was captured, and stays a plain confirmation otherwise.
    auto err = backend->save_chute_position([this, backend]() {
        double x = 0.0, y = 0.0;
        const std::string text =
            backend->last_chute_saved_position(x, y)
                ? fmt::format(lv_tr("Chute position: X {:.1f}, Y {:.1f}"), x, y)
                : std::string(lv_tr("Purge chute position saved."));
        lv_subject_copy_string(&s_cfs_chute_saved_text, text.c_str());
        set_state(State::DONE);
    });
    if (!err.success()) {
        set_state(State::ADJUSTING);
        notify_ams_error(err);
    }
}

void CfsChuteCalibrationOverlay::update_y_display(int centimm) {
    current_y_mm_ = helix::units::from_centimm(centimm);
    y_known_ = true;
    helix::ui::position::format_position(centimm, s_cfs_chute_y_buf, sizeof(s_cfs_chute_y_buf));
    lv_subject_copy_string(&s_cfs_chute_y_text, s_cfs_chute_y_buf);
}

// ============================================================================
// STATIC XML CALLBACKS
// ============================================================================

void CfsChuteCalibrationOverlay::on_start_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[CfsChuteCal] on_start_clicked");
    get_cfs_chute_calibration_overlay().start_calibration();
    LVGL_SAFE_EVENT_CB_END();
}

void CfsChuteCalibrationOverlay::on_jog_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[CfsChuteCal] on_jog_clicked");
    const char* delta_str = static_cast<const char*>(lv_event_get_user_data(e));
    if (delta_str) {
        get_cfs_chute_calibration_overlay().handle_jog(std::atof(delta_str));
    }
    LVGL_SAFE_EVENT_CB_END();
}

void CfsChuteCalibrationOverlay::on_save_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[CfsChuteCal] on_save_clicked");
    get_cfs_chute_calibration_overlay().save_position();
    LVGL_SAFE_EVENT_CB_END();
}

void CfsChuteCalibrationOverlay::on_cancel_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[CfsChuteCal] on_cancel_clicked");
    // The exit gcode (Y_SAFE after PREPARE) rides on_deactivating(), so the
    // button itself only pops the overlay.
    NavigationManager::instance().go_back();
    LVGL_SAFE_EVENT_CB_END();
}

void CfsChuteCalibrationOverlay::on_done_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[CfsChuteCal] on_done_clicked");
    NavigationManager::instance().go_back();
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix::ui

#endif // HELIX_HAS_CFS
