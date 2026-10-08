// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// LINEAR/HUB route plan for the filament_path_canvas detail panel.
//
// The [hits] cases render the widget and pin the hub/buffer/bypass hit rects
// the click handler reads, so the restructured renderer records exactly the
// boxes it draws.

#include "ui_filament_path_canvas.h"

#include "../lvgl_test_fixture.h"
#include "ams_types.h"
#include "lvgl/lvgl.h"
#include "src/ui/ui_filament_path_internal.h"

#include "../catch_amalgamated.hpp"

using namespace helix::ui::fpath;

namespace {

constexpr int32_t W = 400;
constexpr int32_t H = 400;

// A 400x400 canvas with four slots at x = 50, 150, 250, 350 (computed slot
// positions: no slot grid, 100 px slots, no overlap).
lv_obj_t* make_canvas(lv_obj_t* screen, int topology) {
    lv_obj_t* w = ui_filament_path_canvas_create(screen);
    lv_obj_set_size(w, W, H);
    ui_filament_path_canvas_set_slot_count(w, 4);
    ui_filament_path_canvas_set_slot_width(w, 100);
    ui_filament_path_canvas_set_slot_overlap(w, 0);
    ui_filament_path_canvas_set_topology(w, topology);
    return w;
}

void render(LVGLTestFixture& fx, lv_obj_t* w) {
    lv_obj_update_layout(fx.test_screen());
    fx.process_lvgl(120);
    REQUIRE(get_data(w)->layers.render_count > 0);
}

bool area_eq(const lv_area_t& a, const lv_area_t& b) {
    return a.x1 == b.x1 && a.y1 == b.y1 && a.x2 == b.x2 && a.y2 == b.y2;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "FilamentPath: HUB hit rects match the drawn boxes",
                 "[filament-path][plan][hits]") {
    lv_obj_t* w = make_canvas(test_screen(), static_cast<int>(helix::PathTopology::HUB));
    ui_filament_path_canvas_set_buffer_info(w, true, 0);
    ui_filament_path_canvas_set_show_bypass(w, true);
    render(*this, w);

    const FilamentPathData* d = get_data(w);
    lv_area_t c;
    lv_obj_get_coords(w, &c);
    const int32_t r = d->theme.sensor_radius;
    const int32_t hw = d->theme.hub_width;
    const int32_t cx = c.x1 + 200;
    const int32_t hub_h = (int32_t)(H * HUB_HEIGHT_RATIO);
    const int32_t hub_y = c.y1 + (int32_t)(H * HUB_Y_RATIO);

    // Four lanes widen the hub to 3 * 22 + 2 * 8 px, within [hub_width, slot span].
    const int32_t hub_w = LV_CLAMP(3 * 22 + 2 * 8, hw, LV_MAX(hw, 300));
    REQUIRE(d->hits.hub_valid);
    CHECK(area_eq(d->hits.hub,
                  {cx - hub_w / 2, hub_y - hub_h / 2, cx + hub_w / 2, hub_y + hub_h / 2}));

    const int32_t buf_y = c.y1 + (int32_t)(H * BUFFER_Y_RATIO);
    const int32_t buf_w = LV_MAX(36, hw * 4 / 5);
    const int32_t buf_h = LV_MAX(16, hub_h);
    REQUIRE(d->hits.buffer_valid);
    CHECK(area_eq(d->hits.buffer,
                  {cx - buf_w / 2, buf_y - buf_h / 2, cx + buf_w / 2, buf_y + buf_h / 2}));

    const int32_t bx = c.x1 + (int32_t)(W * BYPASS_X_RATIO);
    const int32_t my = c.y1 + (int32_t)(H * BYPASS_MERGE_Y_RATIO);
    REQUIRE(d->hits.bypass_valid);
    CHECK(area_eq(d->hits.bypass, {bx - r * 3, my - r * 4, bx + r * 3, my + r * 4}));
    CHECK(d->hits.origin.x == c.x1);
    CHECK(d->hits.origin.y == c.y1);
}

TEST_CASE_METHOD(LVGLTestFixture, "FilamentPath: hub_only records the hub and nothing below it",
                 "[filament-path][plan][hits]") {
    lv_obj_t* w = make_canvas(test_screen(), static_cast<int>(helix::PathTopology::HUB));
    ui_filament_path_canvas_set_buffer_info(w, true, 0);
    ui_filament_path_canvas_set_show_bypass(w, true);
    ui_filament_path_canvas_set_hub_only(w, true);
    render(*this, w);

    const FilamentPathData* d = get_data(w);
    CHECK(d->hits.hub_valid);
    CHECK_FALSE(d->hits.buffer_valid);
    CHECK_FALSE(d->hits.bypass_valid);
}

TEST_CASE_METHOD(LVGLTestFixture, "FilamentPath: LINEAR selector hit rect spans the slot row",
                 "[filament-path][plan][hits]") {
    lv_obj_t* w = make_canvas(test_screen(), static_cast<int>(helix::PathTopology::LINEAR));
    render(*this, w);

    const FilamentPathData* d = get_data(w);
    lv_area_t c;
    lv_obj_get_coords(w, &c);
    const int32_t r = d->theme.sensor_radius;
    const int32_t cx = c.x1 + 200;
    const int32_t hub_h = (int32_t)(H * HUB_HEIGHT_RATIO);
    const int32_t sel_y = c.y1 + (int32_t)(H * PREP_Y_RATIO) + r + hub_h / 2;
    const int32_t sel_w = 300 + LV_MAX(100, r * 4);

    REQUIRE(d->hits.hub_valid);
    CHECK(area_eq(d->hits.hub,
                  {cx - sel_w / 2, sel_y - hub_h / 2, cx + sel_w / 2, sel_y + hub_h / 2}));
}
