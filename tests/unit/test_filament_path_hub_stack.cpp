// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_path_hub_stack.cpp
 * @brief HUB topology with the bypass hidden: the hub and buffer stack down
 *        onto the toolhead, and the output run reaches the nozzle as one
 *        contiguous route.
 */

#include "ui_filament_path_canvas.h"

#include "../lvgl_test_fixture.h"
#include "lvgl/lvgl.h"
#include "settings_manager.h"
#include "src/ui/ui_filament_path_internal.h"

#include <algorithm>
#include <cmath>

#include "../catch_amalgamated.hpp"

using namespace helix::ui::fpath;

namespace {

constexpr int32_t CANVAS_W = 420;
constexpr int32_t CANVAS_H = 420;

struct HubCanvas {
    lv_obj_t* obj = nullptr;
    FilamentPathData* data = nullptr;
};

class HubStackFixture : public LVGLTestFixture {
  public:
    HubCanvas make(bool buffer, bool bypass, int slots = 2) {
        HubCanvas c;
        c.obj = ui_filament_path_canvas_create(test_screen());
        REQUIRE(c.obj != nullptr);
        lv_obj_set_size(c.obj, CANVAS_W, CANVAS_H);
        ui_filament_path_canvas_set_topology(c.obj, 1);
        ui_filament_path_canvas_set_slot_count(c.obj, slots);
        ui_filament_path_canvas_set_slot_width(c.obj, 80);
        ui_filament_path_canvas_set_slot_overlap(c.obj, 0);
        ui_filament_path_canvas_set_show_bypass(c.obj, bypass);
        ui_filament_path_canvas_set_buffer_info(c.obj, buffer, 0);
        c.data = get_data(c.obj);
        REQUIRE(c.data != nullptr);
        return c;
    }

    void load(const HubCanvas& c, int segment, int error_segment = 0) {
        ui_filament_path_canvas_set_active_slot(c.obj, 0);
        ui_filament_path_canvas_set_filament_color(c.obj, 0x44AAFF);
        ui_filament_path_canvas_set_filament_segment(c.obj, segment);
        ui_filament_path_canvas_set_error_segment(c.obj, error_segment);
    }

    void render(const HubCanvas& c) {
        lv_obj_update_layout(test_screen());
        ui_filament_path_canvas_refresh(c.obj);
        process_lvgl(100);
    }
};

void check_contiguous(const helix::ui::fpath::pg::FilamentPath& route) {
    REQUIRE(route.count > 2);
    for (int i = 1; i < route.count; ++i) {
        CAPTURE(i);
        CHECK(route.segs[i].p0.x == Catch::Approx(route.segs[i - 1].p1.x).margin(0.01));
        CHECK(route.segs[i].p0.y == Catch::Approx(route.segs[i - 1].p1.y).margin(0.01));
    }
}

int32_t ratio_y(const HubCanvas& c, float ratio) {
    lv_area_t a;
    lv_obj_get_coords(c.obj, &a);
    return a.y1 + static_cast<int32_t>(lv_area_get_height(&a) * ratio);
}

int32_t center_y(const lv_area_t& a) {
    return (a.y1 + a.y2) / 2;
}

} // namespace

TEST_CASE_METHOD(HubStackFixture, "HUB loaded to the nozzle with a buffer is one contiguous route",
                 "[filament-path][hub-stack]") {
    auto c = make(/*buffer=*/true, /*bypass=*/false);
    load(c, static_cast<int>(helix::PathSegment::NOZZLE));
    render(c);
    const auto& route = c.data->path_cache.path;
    check_contiguous(route);
    // Fully loaded, the run from the hub's bottom edge to the nozzle is a
    // single tube: no sensor-sized break between hub and buffer.
    REQUIRE(c.data->hits.hub_valid);
    CHECK(route.segs[route.count - 1].p0.y == Catch::Approx(c.data->hits.hub.y2).margin(0.5));
}

TEST_CASE_METHOD(HubStackFixture, "HUB output run with a buffer reaches the nozzle line unbroken",
                 "[filament-path][hub-stack]") {
    auto c = make(true, false);
    // An error on the output keeps the run from collapsing into the loaded
    // tube, so the output stroke and the nozzle line are drawn separately.
    load(c, static_cast<int>(helix::PathSegment::NOZZLE), static_cast<int>(helix::PathSegment::OUTPUT));
    render(c);
    const auto& route = c.data->path_cache.path;
    REQUIRE(route.count >= 2);
    // The stroke spans the buffer (starts above the box, ends below it) and
    // ends exactly where the nozzle line ends: no gap at the toolhead.
    const auto& stroke = route.segs[route.count - 1];
    REQUIRE(c.data->hits.buffer_valid);
    REQUIRE(c.data->hits.hub_valid);
    CHECK(stroke.p0.y == Catch::Approx(c.data->hits.hub.y2 + c.data->theme.sensor_radius).margin(0.5));
    CHECK(stroke.p0.y < c.data->hits.buffer.y1);
    CHECK(stroke.p1.y > c.data->hits.buffer.y2);
    CHECK(stroke.p1.y ==
          Catch::Approx(c.data->path_cache.nozzle_y - c.data->theme.extruder_scale * 2).margin(0.01));
}

namespace {
/// Sets the toolhead style for one case and restores the previous one.
struct ScopedToolheadStyle {
    helix::ToolheadStyle previous;
    explicit ScopedToolheadStyle(helix::ToolheadStyle style)
        : previous(helix::SettingsManager::instance().get_toolhead_style()) {
        helix::SettingsManager::instance().set_toolhead_style(style);
    }
    ~ScopedToolheadStyle() {
        helix::SettingsManager::instance().set_toolhead_style(previous);
    }
};
} // namespace

TEST_CASE_METHOD(HubStackFixture, "HUB without a bypass stacks the hub and buffer onto the toolhead",
                 "[filament-path][hub-stack]") {
    const auto style = GENERATE(helix::ToolheadStyle::DEFAULT, helix::ToolheadStyle::STEALTHBURNER,
                                helix::ToolheadStyle::A4T);
    ScopedToolheadStyle scoped(style);
    CAPTURE(static_cast<int>(style));

    SECTION("with a buffer") {
        auto c = make(true, false);
        load(c, static_cast<int>(helix::PathSegment::NOZZLE));
        render(c);
        REQUIRE(c.data->hits.hub_valid);
        REQUIRE(c.data->hits.buffer_valid);
        CHECK(center_y(c.data->hits.hub) > ratio_y(c, HUB_Y_RATIO));
        CHECK(c.data->hits.hub.y2 < c.data->hits.buffer.y1);
        const int32_t gap = (c.data->hits.hub.y2 - c.data->hits.hub.y1) / 2;
        const int32_t glyph_top =
            toolhead_top_y(c.data->path_cache.nozzle_y, c.data->theme.extruder_scale);
        CHECK(c.data->hits.buffer.y2 + gap <= glyph_top);
        // The buffer box sits midway between the hub's bottom edge and the glyph.
        const int32_t above = c.data->hits.buffer.y1 - c.data->hits.hub.y2;
        const int32_t below = glyph_top - c.data->hits.buffer.y2;
        CAPTURE(above, below);
        CHECK(std::abs(above - below) <= 1);
    }
    SECTION("without a buffer") {
        auto c = make(false, false);
        load(c, static_cast<int>(helix::PathSegment::NOZZLE));
        render(c);
        REQUIRE(c.data->hits.hub_valid);
        CHECK(center_y(c.data->hits.hub) > ratio_y(c, HUB_Y_RATIO));
        const int32_t glyph_top =
            toolhead_top_y(c.data->path_cache.nozzle_y, c.data->theme.extruder_scale);
        CHECK(c.data->hits.hub.y2 < glyph_top);
    }
}

TEST_CASE_METHOD(HubStackFixture, "HUB on the toolhead draws its output run from the hub's bottom edge",
                 "[filament-path][hub-stack]") {
    auto c = make(false, false);
    ui_filament_path_canvas_set_hub_on_toolhead(c.obj, true);
    load(c, static_cast<int>(helix::PathSegment::NOZZLE));
    render(c);
    const auto& route = c.data->path_cache.path;
    // The on-toolhead hub's bottom edge (its output Y) sits one stub above the
    // toolhead sensor row.
    const int32_t stub = std::max<int32_t>(10, static_cast<int32_t>(CANVAS_H * 0.03f));
    const int32_t output_y = ratio_y(c, TOOLHEAD_Y_RATIO) - stub;
    int run = -1;
    for (int i = 0; i < route.count; ++i) {
        if (std::abs(route.segs[i].p0.y - output_y) < 0.5f) {
            run = i;
            break;
        }
    }
    // No output sensor dot sits under an on-toolhead hub: the tube leaves the
    // hub's bottom edge and runs into the glyph's inlet.
    REQUIRE(run >= 0);
    CHECK(route.segs[run].p0.x == Catch::Approx(route.segs[run].p1.x).margin(0.01));
    CHECK(route.segs[run].p1.y >=
          c.data->path_cache.nozzle_y - c.data->theme.sensor_radius);
}

TEST_CASE_METHOD(HubStackFixture, "HUB with the bypass shown keeps the ratio layout",
                 "[filament-path][hub-stack]") {
    auto c = make(true, true);
    load(c, static_cast<int>(helix::PathSegment::NOZZLE));
    render(c);
    REQUIRE(c.data->hits.hub_valid);
    REQUIRE(c.data->hits.buffer_valid);
    CHECK(center_y(c.data->hits.hub) == Catch::Approx(ratio_y(c, HUB_Y_RATIO)).margin(1));
    CHECK(center_y(c.data->hits.buffer) == Catch::Approx(ratio_y(c, BUFFER_Y_RATIO)).margin(1));
}
