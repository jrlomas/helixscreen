// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "panel_widget.h"
#include "src/ui/panel_widgets/tile_sizing.h"

namespace helix {

class LedControlsWidget : public PanelWidget {
  public:
    // The widget only opens the LED overlay, which reaches LedController and
    // NavigationManager through their own singletons — it needs no printer state
    // or API handle of its own.
    LedControlsWidget() = default;
    ~LedControlsWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    const char* id() const override {
        return "led_controls";
    }

    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override {
        (void)colspan;
        (void)rowspan;
        sizing_.measure_and_publish(width_px, height_px);
    }

    bool fits_at(int width_px, int height_px) const override {
        return sizing_.fits(width_px, height_px);
    }

    const char** xml_attrs() const override {
        return sizing_.subject_attrs();
    }

    TileSizing* tile_sizing() override {
        return &sizing_;
    }

    static void on_led_controls_clicked(lv_event_t* e);

  private:
    void handle_clicked();

    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;
    lv_obj_t* led_control_panel_ = nullptr;

    /// Built with the widget so its subjects exist before the manager parses
    /// this tile's component; a binding whose subject is missing at parse time
    /// is dropped permanently.
    TileSizing sizing_{"led_controls", TileSizing::Content{"", "", "LEDs", false}};
};

} // namespace helix
