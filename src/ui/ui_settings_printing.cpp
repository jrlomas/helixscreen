// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_printing.cpp
 * @brief Implementation of PrintingSettingsOverlay
 */

#include "ui_settings_printing.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_nav_manager.h"
#include "ui_overlay_timelapse_settings.h"
#include "ui_settings_machine_limits.h"
#include "ui_settings_material_temps.h"
#include "ui_settings_motion.h"

#include "app_globals.h"
#include "display_settings_manager.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_manager.h"
#include "settings_manager.h"
#include "static_panel_registry.h"

#include <spdlog/spdlog.h>

#include <memory>

namespace helix::settings {

// ============================================================================
// SINGLETON ACCESSOR
// ============================================================================

static std::unique_ptr<PrintingSettingsOverlay> g_printing_settings_overlay;

PrintingSettingsOverlay& get_printing_settings_overlay() {
    if (!g_printing_settings_overlay) {
        g_printing_settings_overlay = std::make_unique<PrintingSettingsOverlay>();
        StaticPanelRegistry::instance().register_destroy(
            "PrintingSettingsOverlay", []() { g_printing_settings_overlay.reset(); });
    }
    return *g_printing_settings_overlay;
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

PrintingSettingsOverlay::PrintingSettingsOverlay() {
    spdlog::debug("[{}] Created", get_name());
}

PrintingSettingsOverlay::~PrintingSettingsOverlay() {
    spdlog::trace("[{}] Destroyed", get_name());
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void PrintingSettingsOverlay::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

void PrintingSettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_toolhead_style_changed", on_toolhead_style_changed},
        {"on_gcode_mode_changed", on_gcode_mode_changed},
        {"on_z_movement_style_changed", on_z_movement_style_changed},
        {"on_machine_limits_clicked", on_machine_limits_clicked},
        {"on_motion_settings_clicked", on_motion_settings_clicked},
        {"on_material_temps_clicked", on_material_temps_clicked},
        // on_retraction_row_clicked is registered by RetractionSettingsOverlay
        // on_timelapse_settings_clicked is registered by SettingsPanel
        {"on_timelapse_settings_clicked", on_timelapse_settings_clicked},
    });

    spdlog::debug("[{}] Callbacks registered", get_name());
}

// ============================================================================
// UI CREATION
// ============================================================================

lv_obj_t* PrintingSettingsOverlay::create(lv_obj_t* parent) {
    if (overlay_root_) {
        spdlog::warn("[{}] create() called but overlay already exists", get_name());
        return overlay_root_;
    }

    spdlog::debug("[{}] Creating overlay...", get_name());

    overlay_root_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent, "settings_printing_overlay", nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    spdlog::info("[{}] Overlay created", get_name());
    return overlay_root_;
}

void PrintingSettingsOverlay::show(lv_obj_t* parent_screen) {
    spdlog::debug("[{}] show() called", get_name());

    parent_screen_ = parent_screen;

    if (!subjects_initialized_) {
        init_subjects();
        register_callbacks();
    }

    if (!overlay_root_ && parent_screen_) {
        create(parent_screen_);
    }

    if (!overlay_root_) {
        spdlog::error("[{}] Cannot show - overlay not created", get_name());
        return;
    }

    NavigationManager::instance().register_overlay_instance(overlay_root_, this);
    NavigationManager::instance().push_overlay(overlay_root_);
}

// ============================================================================
// LIFECYCLE
// ============================================================================

void PrintingSettingsOverlay::on_activate() {
    OverlayBase::on_activate();

    init_toolhead_style_dropdown();
    init_gcode_mode_dropdown();
    init_z_movement_dropdown();
}

// ============================================================================
// DROPDOWN INITIALIZATION
// ============================================================================

void PrintingSettingsOverlay::init_toolhead_style_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* row = lv_obj_find_by_name(overlay_root_, "row_toolhead_style");
    if (!row)
        return;

    lv_obj_t* dropdown = lv_obj_find_by_name(row, "dropdown");
    if (dropdown) {
        lv_dropdown_set_options(dropdown, SettingsManager::get_toolhead_style_options());
        auto style = SettingsManager::instance().get_toolhead_style();
        lv_dropdown_set_selected(
            dropdown,
            static_cast<uint32_t>(SettingsManager::toolhead_style_to_dropdown_index(style)));
        spdlog::trace("[{}] Toolhead style dropdown initialized (style={}, dropdown_index={})",
                      get_name(), static_cast<int>(style),
                      SettingsManager::toolhead_style_to_dropdown_index(style));
    }
}

void PrintingSettingsOverlay::init_gcode_mode_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* row = lv_obj_find_by_name(overlay_root_, "row_gcode_mode");
    if (!row)
        return;

    lv_obj_t* dropdown = lv_obj_find_by_name(row, "dropdown");
    if (dropdown) {
        auto& display_settings = DisplaySettingsManager::instance();
#ifndef ENABLE_GLES_3D
        // Without GLES, remove "3D View" option
        lv_dropdown_set_options(dropdown, (std::string(lv_tr("Auto")) + "\n" + lv_tr("2D Layers") +
                                           "\n" + lv_tr("Thumbnail Only"))
                                              .c_str());
        int mode = display_settings.get_gcode_render_mode();
        int index = 0; // Auto
        if (mode == 2)
            index = 1; // 2D Layers
        else if (mode == 3)
            index = 2; // Thumbnail Only
        lv_dropdown_set_selected(dropdown, index);
#else
        int mode = display_settings.get_gcode_render_mode();
        lv_dropdown_set_selected(dropdown, mode);
#endif
        spdlog::trace("[{}] G-code mode dropdown initialized", get_name());
    }
}

void PrintingSettingsOverlay::init_z_movement_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* row = lv_obj_find_by_name(overlay_root_, "row_z_movement_style");
    if (!row)
        return;

    lv_obj_t* dropdown = lv_obj_find_by_name(row, "dropdown");
    if (dropdown) {
        auto style = SettingsManager::instance().get_z_movement_style();
        lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(style));
        spdlog::trace("[{}] Z movement style dropdown initialized (style={})", get_name(),
                      static_cast<int>(style));
    }
}

// ============================================================================
// EVENT HANDLER IMPLEMENTATIONS
// ============================================================================

void PrintingSettingsOverlay::handle_toolhead_style_changed(int index) {
    auto style = SettingsManager::dropdown_index_to_toolhead_style(index);
    spdlog::info("[{}] Toolhead style changed: {} (dropdown index {})", get_name(),
                 static_cast<int>(style), index);
    SettingsManager::instance().set_toolhead_style(style);
}

void PrintingSettingsOverlay::handle_gcode_mode_changed(int index) {
#ifndef ENABLE_GLES_3D
    static const int INDEX_TO_MODE[] = {0, 2, 3}; // Auto, 2D Layers, Thumbnail Only
    int mode = (index >= 0 && index <= 2) ? INDEX_TO_MODE[index] : 0;
#else
    int mode = index;
#endif

    static const char* MODE_NAMES[] = {"Auto", "3D", "2D Layers", "Thumbnail Only"};
    spdlog::info("[{}] G-code render mode changed: {} ({})", get_name(), mode,
                 (mode >= 0 && mode <= 3) ? MODE_NAMES[mode] : "Unknown");
    DisplaySettingsManager::instance().set_gcode_render_mode(mode);
}

void PrintingSettingsOverlay::handle_z_movement_style_changed(int index) {
    auto style = static_cast<ZMovementStyle>(index);
    spdlog::info("[{}] Z movement style changed: {} ({})", get_name(), index,
                 index == 0 ? "Auto" : (index == 1 ? "Bed Moves" : "Nozzle Moves"));
    SettingsManager::instance().set_z_movement_style(style);
}

void PrintingSettingsOverlay::handle_machine_limits_clicked() {
    spdlog::debug("[{}] Machine Limits clicked", get_name());

    auto& overlay = helix::settings::get_machine_limits_overlay();
    overlay.set_api(get_moonraker_api());
    overlay.show(parent_screen_);
}

void PrintingSettingsOverlay::handle_material_temps_clicked() {
    spdlog::debug("[{}] Material Temperatures clicked", get_name());

    auto& overlay = helix::settings::get_material_temps_overlay();
    overlay.show(parent_screen_);
}

// ============================================================================
// STATIC CALLBACKS
// ============================================================================

void PrintingSettingsOverlay::on_toolhead_style_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintingSettingsOverlay] on_toolhead_style_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    get_printing_settings_overlay().handle_toolhead_style_changed(index);
    LVGL_SAFE_EVENT_CB_END();
}

void PrintingSettingsOverlay::on_gcode_mode_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintingSettingsOverlay] on_gcode_mode_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    get_printing_settings_overlay().handle_gcode_mode_changed(index);
    LVGL_SAFE_EVENT_CB_END();
}

void PrintingSettingsOverlay::on_z_movement_style_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintingSettingsOverlay] on_z_movement_style_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    get_printing_settings_overlay().handle_z_movement_style_changed(index);
    LVGL_SAFE_EVENT_CB_END();
}

void PrintingSettingsOverlay::on_machine_limits_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintingSettingsOverlay] on_machine_limits_clicked");
    get_printing_settings_overlay().handle_machine_limits_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void PrintingSettingsOverlay::on_motion_settings_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintingSettingsOverlay] on_motion_settings_clicked");
    helix::settings::show_motion_settings_overlay();
    LVGL_SAFE_EVENT_CB_END();
}

void PrintingSettingsOverlay::on_retraction_row_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintingSettingsOverlay] on_retraction_row_clicked");
    // Delegates to the global retraction settings overlay (same as settings panel)
    // RetractionSettingsOverlay registers its own callback; this is a fallback
    spdlog::debug("[PrintingSettingsOverlay] Retraction row clicked (fallback)");
    LVGL_SAFE_EVENT_CB_END();
}

void PrintingSettingsOverlay::on_material_temps_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintingSettingsOverlay] on_material_temps_clicked");
    get_printing_settings_overlay().handle_material_temps_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void PrintingSettingsOverlay::on_timelapse_settings_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintingSettingsOverlay] on_timelapse_settings_clicked");
    open_timelapse_settings();
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix::settings
