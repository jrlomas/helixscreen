// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_motion.h
 * @brief Motion Settings overlay - jog feedrates and step distances
 *
 * Persisted settings owned by SettingsManager:
 * - Jog Speed XY / Z (stored mm/min, displayed mm/s)
 * - Six step distances (Fine/Coarse/Turbo x inner/outer, mm)
 *
 * @pattern Overlay (lazy init, machine_limits shape)
 * @threading Main thread only
 *
 * @see SettingsManager jog accessors
 */

#pragma once

#include "ui_panel_motion.h"

#include "lvgl/lvgl.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"

// Forward declarations
class IMoonrakerAPI; // NAMESPACE_OK: matches the global-namespace interface in i_moonraker_api.h

namespace helix::settings {

/**
 * @class MotionSettingsOverlay
 * @brief Overlay for jog speed and step distance settings
 *
 * Opened only through show_motion_settings_overlay(), so the settings row and
 * the motion panel header cog share one entry point.
 */
class MotionSettingsOverlay : public OverlayBase {
  public:
    MotionSettingsOverlay();
    ~MotionSettingsOverlay() override;

    void set_api(IMoonrakerAPI* api);

    //
    // === OverlayBase Interface ===
    //

    void init_subjects() override;
    void register_callbacks() override;
    lv_obj_t* create(lv_obj_t* parent) override;

    const char* get_name() const override {
        return "Motion Settings";
    }

    void on_activate() override;
    void on_deactivating(DeactivateReason reason) override;

    /**
     * @brief Show the overlay (lazy create + push)
     * @param parent_screen Screen to create the overlay on
     */
    void show(lv_obj_t* parent_screen);

    /**
     * @brief One row per control. The value doubles as the user_data on each
     * setting_value_field in motion_settings_overlay.xml, so the order here
     * and the numbers there must agree. FIELD_SPECS in the .cpp is indexed by
     * the same value.
     */
    enum class Field : int {
        JogSpeedXY = 0,
        JogSpeedZ,
        FineInner,
        FineOuter,
        CoarseInner,
        CoarseOuter,
        TurboInner,
        TurboOuter,
        Count
    };

    //
    // === Event Handlers (public for static callbacks) ===
    //

    /// Slider drag or typed speed, in mm/s. is_z picks the axis.
    void handle_jog_speed_changed(bool is_z, int mm_s);

    /// Keypad-confirmed value for one row (speed in mm/s or distance in mm).
    void handle_keypad_value(Field field, double value);

    /// Open the keypad for one row (speed or distance).
    void handle_field_clicked(Field field);

    /// Reset button - restore the six shipped step distances.
    void handle_reset_distances();

  private:
    /// Format one row's display buffer from SettingsManager.
    void format_display(size_t i);

    /// Re-read SettingsManager into the display subjects.
    void refresh_displays();

    /// Re-read SettingsManager into the sliders and their range labels.
    void refresh_sliders();

    /// Persist any pending slider value (called by the debounce timer).
    void persist_pending_speed();

    /// One of the overlay's speed sliders, or nullptr before create().
    lv_obj_t* speed_slider(bool is_z) const;

    /// Slider ceiling in mm/s, from the printer's reported feedrate limit.
    int max_jog_mm_s() const;

    /// Deinitialize subjects for clean shutdown.
    void deinit_subjects();

    //
    // === Dependencies ===
    //

    IMoonrakerAPI* api_{nullptr};

    //
    // === State Tracking ===
    //

    /// Set while our own numeric keypad is open: returning from it
    /// re-activates this overlay, and a refresh there would read back the
    /// pre-debounce value and clobber the one just typed.
    bool returning_from_keypad_{false};

    /// Row the open keypad is editing. The overlay stack guarantees one
    /// keypad at a time, so a single slot is enough.
    Field pending_keypad_field_{Field::JogSpeedXY};

    /// Debounced SettingsManager write for slider drags: a drag fires
    /// value_changed per pixel and each write persists to disk. Indexed
    /// [XY, Z]; 0 means nothing pending (the settings clamp floor is 60).
    lv_timer_t* persist_timer_{nullptr};
    int pending_mm_min_[2]{0, 0};

    //
    // === Subject Management ===
    //

    SubjectManager subjects_;

    /// One string display subject per Field, named in SUBJECT_NAMES.
    lv_subject_t display_subjects_[static_cast<size_t>(Field::Count)]{};
    char display_buffers_[static_cast<size_t>(Field::Count)][24]{};

    /// Slider ceiling text, bound to both speed sliders' max end labels.
    lv_subject_t jog_speed_max_subject_{};
    char jog_speed_max_buf_[16]{};

    //
    // === Static Callbacks ===
    //

    static void on_jog_speed_xy_changed(lv_event_t* e);
    static void on_jog_speed_z_changed(lv_event_t* e);
    static void on_reset_distances(lv_event_t* e);

    /// Tap on a setting_value_field. user_data carries the Field index as text.
    static void on_field_clicked(lv_event_t* e);

    /// ui_keypad_callback_t; user_data carries the overlay.
    static void on_keypad_value(float value, void* user_data);
};

/// Global instance accessor; creates on first use and registers cleanup.
MotionSettingsOverlay& get_motion_settings_overlay();

/**
 * @brief The single opener for the Motion settings overlay
 *
 * Called by the Printing settings row and the motion panel header cog, so
 * both share one creation/registration path.
 */
void show_motion_settings_overlay();

} // namespace helix::settings
