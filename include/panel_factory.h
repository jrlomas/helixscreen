// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_nav.h"
#include "ui_timer_guard.h"

#include <array>
#include <cstdint>

// Forward declarations
struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;

namespace helix {

/**
 * @brief Decides when an idle moment has come to build a deferred panel ahead of
 * its first visit.
 *
 * Ticked once a second. Opens once the printer has stayed connected for
 * kSettleTicks ticks, so connect and discovery finish their own UI work first,
 * and no input has arrived for kQuietMs, so the build does not land under a
 * finger.
 */
class IdlePrebuildGate {
  public:
    static constexpr int kSettleTicks = 3;
    static constexpr uint32_t kQuietMs = 3000;

    bool tick(bool connected, uint32_t inactive_ms) {
        connected_ticks_ = connected ? connected_ticks_ + 1 : 0;
        return connected_ticks_ >= kSettleTicks && inactive_ms >= kQuietMs;
    }

  private:
    int connected_ticks_ = 0;
};

/**
 * @brief Factory for creating and wiring UI panels
 *
 * PanelFactory handles:
 * - Finding panels by name in the panel container
 * - Setting up panel observers and event handlers
 * - Creating overlay panels from XML
 * - Wiring panels together (e.g., print_select → print_status)
 *
 * Usage:
 *   PanelFactory factory;
 *   if (!factory.find_panels(panel_container)) { return error; }
 *   factory.setup_panels(screen);
 *   factory.create_overlays(screen);
 */
class PanelFactory {
  public:
    /// Panel names for lookup
    static constexpr const char* PANEL_NAMES[UI_PANEL_COUNT] = {
        "home_panel",     "print_select_panel", "controls_panel",
        "filament_panel", "settings_panel",     "advanced_panel"};

    /**
     * @brief Find all panels by name in the container
     * @param panel_container Container with panel children
     * @return true if all panels found, false if any missing
     */
    bool find_panels(lv_obj_t* panel_container);

    /**
     * @brief Set up all panel observers and event handlers
     * @param screen Root screen for overlays
     */
    void setup_panels(lv_obj_t* screen);

    /**
     * @brief Create print status overlay panel
     * @param screen Parent screen
     * @return true if created successfully
     */
    bool create_print_status_overlay(lv_obj_t* screen);

    /**
     * @brief Initialize numeric keypad modal
     * @param screen Parent screen
     */
    void init_keypad(lv_obj_t* screen);

    /**
     * @brief Get panel array for navigation system
     */
    lv_obj_t** panels() {
        return m_panels.data();
    }

    /**
     * @brief Get print status overlay panel
     */
    lv_obj_t* print_status_panel() const {
        return m_print_status_panel;
    }

    /**
     * @brief Create an overlay panel from XML
     * @param screen Parent screen
     * @param component_name XML component name
     * @param display_name Human-readable name for logging
     * @return Created object, or nullptr if failed
     */
    static lv_obj_t* create_overlay(lv_obj_t* screen, const char* component_name,
                                    const char* display_name);

    /**
     * @brief Build a panel that was deferred at boot (ESP32 first-navigation).
     *
     * Instantiates PANEL_NAMES[panel_id] into the panel container, runs its
     * setup(), and registers the widget + instance with NavigationManager. Paints
     * a loading state before the (multi-second) create. Registered as
     * NavigationManager's deferred_panel_builder on ESP; a no-op on other
     * platforms (all panels are built eagerly by setup_panels).
     */
    void build_deferred_panel(int panel_id);

  private:
    // Wire one of the six main panels: get_global_*_panel().setup(widget, screen)
    // + register the instance with NavigationManager. Used by the ESP eager-home
    // path and by build_deferred_panel; the desktop setup_panels() body is
    // unchanged and does not call this.
    void setup_one_panel(int panel_id);

    // Builds Print Files at the first idle moment, so its first visit does not
    // pay the build (ESP32; every panel is resident elsewhere).
    static void on_idle_prebuild_tick(lv_timer_t* timer);
    helix::ui::LvglTimerGuard m_idle_prebuild_timer;
    IdlePrebuildGate m_idle_prebuild_gate;

    std::array<lv_obj_t*, UI_PANEL_COUNT> m_panels = {};
    lv_obj_t* m_print_status_panel = nullptr;
    lv_obj_t* m_panel_container = nullptr; // for deferred panel creation (ESP)
    lv_obj_t* m_screen = nullptr;          // setup() target for deferred panels (ESP)
};

} // namespace helix
