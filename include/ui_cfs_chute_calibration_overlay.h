// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#if HELIX_HAS_CFS

#include "ui_observer_guard.h"

#include "lvgl/lvgl.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"

namespace helix::ui {

/**
 * @brief Guided purge-chute calibration overlay for the Creality CFS on K1
 * printers (K1 stock command dialect).
 *
 * Walks the stock screen's verified choreography (prestonbrown/helixscreen#1282):
 * full home (XYZ_ZERO), prepare (COORDINATES_ADJUST_PREPARE), Y-only jogging
 * to line the toolhead up with the chute, then save (COORDINATES_ADJUST_SAVE_POS
 * + Y_SAFE). All gcode goes through AmsBackendCfs; this overlay owns only the
 * step machine, the live-Y readout and the jog clamping.
 *
 * @pattern Overlay (lazy init, singleton via get_cfs_chute_calibration_overlay())
 * @threading Main thread only
 */
class CfsChuteCalibrationOverlay : public OverlayBase {
  public:
    CfsChuteCalibrationOverlay();
    ~CfsChuteCalibrationOverlay() override;

    // === OverlayBase Interface ===

    void init_subjects() override;
    void register_callbacks() override;

    const char* get_name() const override {
        return "CFS Purge Chute Calibration";
    }

    lv_obj_t* create(lv_obj_t* parent) override;
    void show(lv_obj_t* parent_screen);

    void on_activate() override;
    void on_deactivating(DeactivateReason reason) override;

  private:
    /// Step machine values published on s_cfs_chute_state. Order matters: the
    /// cancel path treats everything before ADJUSTING as "nothing to unwind".
    enum class State {
        IDLE = 0,
        HOMING = 1,
        ADJUSTING = 2,
        SAVING = 3,
        DONE = 4,
    };

    void set_state(State state);
    void start_calibration();
    void handle_jog(double delta_mm);
    void save_position();
    void update_y_display(int centimm);

    // Static XML event callbacks (user_data carries the jog delta as a string)
    static void on_start_clicked(lv_event_t* e);
    static void on_jog_clicked(lv_event_t* e);
    static void on_save_clicked(lv_event_t* e);
    static void on_cancel_clicked(lv_event_t* e);
    static void on_done_clicked(lv_event_t* e);

    /// Live toolhead Y in mm, from the position subject; drives the readout
    /// and the jog clamp.
    double current_y_mm_ = 0.0;
    bool y_known_ = false;
    bool edge_warned_y_ = false;

    lv_obj_t* overlay_root_ = nullptr;

    ObserverGuard y_observer_;
    SubjectManager subjects_;
    bool subjects_initialized_ = false;
};

/// Singleton accessor. The instance is destroyed with the panel registry.
CfsChuteCalibrationOverlay& get_cfs_chute_calibration_overlay();

} // namespace helix::ui

#endif // HELIX_HAS_CFS
