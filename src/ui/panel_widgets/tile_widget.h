// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "panel_widget.h"
#include "src/ui/panel_widgets/tile_sizing.h"

#include <string>
#include <utility>

namespace helix {

/**
 * @brief A centred-icon tile whose only C++ behaviour is sizing itself
 *
 * Several home tiles are pure XML: the component draws everything and no C++
 * instance exists. Such a tile cannot size itself, because measuring needs
 * on_size_changed() and binding needs per-instance subject names to exist
 * before the component is parsed, and both of those live on an instance.
 *
 * Rather than six near-identical widget classes, one class covers every tile
 * whose only need is a TileSizing. A tile that grows real behaviour later stops
 * using this and implements PanelWidget directly.
 */
class TileWidget : public PanelWidget {
  public:
    TileWidget(std::string instance_id, TileSizing::Content content)
        : instance_id_(std::move(instance_id)), sizing_(instance_id_, std::move(content)) {}

    /// The component carries the whole appearance and the sizing subjects are
    /// live from construction, so the only thing to wire is the back-pointer
    /// edit mode reaches the live instance through: without it the resize
    /// clamp cannot ask this tile whether a size fits and accepts whatever the
    /// drag produced.
    void attach(lv_obj_t* widget_obj, lv_obj_t*) override {
        root_ = widget_obj;
        lv_obj_set_user_data(root_, this);
    }

    void detach() override {
        if (root_) {
            lv_obj_set_user_data(root_, nullptr);
            root_ = nullptr;
        }
    }

    const char* id() const override {
        return instance_id_.c_str();
    }

    /// The instance id carries a suffix for multi-instance tiles
    /// ("power_device:1"), but the component name is per TYPE, so the suffix is
    /// dropped here or lv_xml_create is handed a name no component answers to.
    std::string get_component_name() const override {
        const size_t colon = instance_id_.find(':');
        return "panel_widget_" +
               (colon == std::string::npos ? instance_id_ : instance_id_.substr(0, colon));
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

  private:
    lv_obj_t* root_ = nullptr;
    std::string instance_id_;
    TileSizing sizing_;
};

} // namespace helix
