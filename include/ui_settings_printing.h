// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_printing.h
 * @brief Printing Settings overlay - toolhead, G-code, Z movement, limits, retraction, temps
 *
 * This overlay allows users to configure:
 * - Toolhead style (icon appearance)
 * - G-code preview render mode
 * - Z movement style (bed vs nozzle)
 * - Machine velocity/acceleration limits
 * - Firmware retraction settings (when available)
 * - Material temperature presets
 * - Timelapse recording (when available)
 *
 * @pattern Overlay (lazy init)
 * @threading Main thread only
 *
 * @see SettingsManager for persistence
 * @see DisplaySettingsManager for G-code render mode
 */

#pragma once

#include "lvgl/lvgl.h"
#include "overlay_base.h"

namespace helix::settings {

/**
 * @class PrintingSettingsOverlay
 * @brief Overlay for configuring printing-related settings
 *
 * ## Usage:
 *
 * @code
 * auto& overlay = helix::settings::get_printing_settings_overlay();
 * overlay.show(parent_screen);
 * @endcode
 */
class PrintingSettingsOverlay : public OverlayBase {
  public:
    PrintingSettingsOverlay();
    ~PrintingSettingsOverlay() override;

    //
    // === OverlayBase Interface ===
    //

    void init_subjects() override;
    void register_callbacks() override;

    const char* get_name() const override {
        return "Printing Settings";
    }

    void on_activate() override;

    //
    // === UI Creation ===
    //

    lv_obj_t* create(lv_obj_t* parent) override;

    /**
     * @brief Show the overlay (lazy-creates if needed)
     * @param parent_screen The parent screen for overlay creation
     */
    void show(lv_obj_t* parent_screen);

    bool is_created() const {
        return overlay_root_ != nullptr;
    }

    //
    // === Event Handlers (public for static callbacks) ===
    //

    void handle_toolhead_style_changed(int index);
    void handle_gcode_mode_changed(int index);
    void handle_z_movement_style_changed(int index);
    void handle_machine_limits_clicked();
    void handle_material_temps_clicked();

  private:
    //
    // === Dropdown Initialization ===
    //

    void init_toolhead_style_dropdown();
    void init_gcode_mode_dropdown();
    void init_z_movement_dropdown();

    //
    // === Static Callbacks ===
    //

    static void on_toolhead_style_changed(lv_event_t* e);
    static void on_gcode_mode_changed(lv_event_t* e);
    static void on_z_movement_style_changed(lv_event_t* e);
    static void on_machine_limits_clicked(lv_event_t* e);
    static void on_motion_settings_clicked(lv_event_t* e);
    static void on_retraction_row_clicked(lv_event_t* e);
    static void on_material_temps_clicked(lv_event_t* e);
    static void on_timelapse_settings_clicked(lv_event_t* e);
};

/**
 * @brief Global instance accessor
 *
 * Creates the overlay on first access and registers it for cleanup
 * with StaticPanelRegistry.
 *
 * @return Reference to singleton PrintingSettingsOverlay
 */
PrintingSettingsOverlay& get_printing_settings_overlay();

} // namespace helix::settings
