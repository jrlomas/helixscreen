// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "panel_widget.h"

#include <string>

namespace helix {

class PrinterImageWidget : public PanelWidget {
  public:
    PrinterImageWidget();
    ~PrinterImageWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    /// Factory-registration key. Exposed so callers scanning a heterogeneous
    /// widget list can match on id() and static_cast, instead of dynamic_cast —
    /// the firmware builds -fno-rtti.
    static constexpr const char* WIDGET_ID = "printer_image";

    const char* id() const override {
        return WIDGET_ID;
    }

    /// Called when panel activates — re-check if printer image changed in settings
    void on_activate();

    /// Reload printer image and printer info subjects from config
    void reload_from_config();

    /// Re-check printer image setting and update the displayed image
    void refresh_printer_image();

    /// XML event callback — opens printer manager overlay
    static void printer_manager_clicked_cb(lv_event_t* e);

  private:
    /// Points `img` at the pre-scaled copy for its current size, if one is on disk.
    /// Returns false when the widget has no resolved size yet, or nothing is cached
    /// at that size, leaving the caller to fall back to the tier image.
    bool try_set_exact_size_source(lv_obj_t* img);

  public:
  protected:
    void on_hooked_root_deleted() override;

  private:
    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;

    // Persistent disk cache for exact-size printer image
    lv_timer_t* cache_timer_ = nullptr;
    // Defers the image refresh out of the synchronous attach()/on_activate()
    // path: lv_image_set_inner_align forces lv_obj_update_layout, which cascades
    // into the parent grid's grid_update. Running that during a panel rebuild
    // (mid-rebuild grid) walked the freed descriptor off the heap end (#983/#1025).
    lv_timer_t* refresh_timer_ = nullptr;
    std::string current_source_path_; // Resolved source image (LVGL path)
    /// What lv_image_set_src was last given, so a repeat resolve to the same file
    /// does not invalidate the widget for an identical image.
    std::string current_displayed_path_;

    /// Guards the cache-generation continuation, which runs from a worker thread
    /// and touches this widget's LVGL tree.
    helix::AsyncLifetimeGuard lifetime_;

    /// One cache generation at a time. Every navigation back to the panel schedules
    /// another cache check, and without this a second job would redo work already
    /// running for the same source and size.
    bool cache_job_inflight_ = false;

    /// Re-resolves the image when the printer type settles mid-session:
    /// auto-detection finishes after the home panel is built on a fresh
    /// install, so attach()'s one-shot resolve still shows the generic
    /// silhouette unless a type change re-triggers it.
    ObserverGuard printer_type_observer_;

    void schedule_image_refresh();
    void schedule_cache_check();
    void check_or_generate_cache();

    void handle_printer_manager_clicked();
};

} // namespace helix
