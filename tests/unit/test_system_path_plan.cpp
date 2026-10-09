// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The multi-unit overview's route plan. Single-tool cases run on a 400x400
// frame with unit stems at x = 100 and 300 and the default glyph at extruder
// scale 10: entry 20, hub sensor 78, merge 117, hub 189 (h 48, bottom 213),
// nozzle 346 (top 326, glyph bottom 372).

#include "ui_system_path_canvas.h"

#include "../lvgl_test_fixture.h"
#include "filament_path_test_helpers.h"
#include "src/ui/ui_system_path_plan.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix::ui::syspath;
using namespace helix::ui::fpath;
using namespace fpath_test;
using helix::PathSegment;

namespace {

const lv_color_t IDLE = lv_color_hex(0x555555);
const lv_color_t BG = lv_color_hex(0x202020);
const lv_color_t ACCENT = lv_color_hex(0x3399FF);
constexpr uint32_t ACTIVE = 0xE53935;
constexpr uint32_t OWL = 0x1E88E5;
constexpr lv_area_t AREA = {0, 0, 399, 399};

// Two HUB units (a Box Turtle and a Night Owl) on one toolhead, both with hub
// sensors; unit 0 is active and loaded to the nozzle.
std::unique_ptr<SystemPathData> multi() {
    auto d = std::make_unique<SystemPathData>();
    d->unit_count = 2;
    d->unit_x_positions[0] = 100;
    d->unit_x_positions[1] = 300;
    for (int u = 0; u < 2; u++) {
        d->unit_topology[u] = 1;
        d->unit_tool_count[u] = 1;
        d->unit_has_hub_sensor[u] = true;
    }
    d->total_tools = 1;
    d->active_unit = 0;
    d->active_color = ACTIVE;
    d->filament_loaded = true;
    d->filament_segment = PathSegment::NOZZLE;
    d->unit_hub_triggered[0] = true;
    d->color_idle = IDLE;
    d->color_bg = BG;
    d->color_accent = ACCENT;
    d->color_error = lv_color_hex(0xFF0000);
    d->color_hub_bg = lv_color_hex(0x333333);
    return d;
}

PathPlan& plan_of(const SystemPathData& d) {
    static PathPlan plan;
    OverviewBoxes boxes;
    plan_overview(d, compute_sys_layout(d, AREA), plan, boxes);
    return plan;
}

// The band whose center sits on the stem at @p x.
const SensorBand* stem_band(const PathPlan& plan, float x) {
    for (int i = 0; i < plan.band_count; i++)
        if (std::fabs(plan.bands[i].at.x - x) < 0.5f && plan.bands[i].at.y < 100)
            return &plan.bands[i];
    return nullptr;
}

// A segment of the route ends exactly at @p p.
bool on_route(const Route& r, pg::PathPoint p) {
    for (int i = 0; i < r.path.count; i++)
        if (near(seg_end(r.path.segs[i]), p.x, p.y, 0.5f))
            return true;
    return false;
}

} // namespace

TEST_CASE("Overview plan: each unit's route is continuous with a clamp band on its stem",
          "[system_path][filament_path]") {
    auto d = multi();
    const PathPlan& plan = plan_of(*d);
    REQUIRE(plan.dropped == 0);
    REQUIRE(plan.route_count == 2); // the active unit owns the trunk
    CHECK(plan.active_route == 0);

    for (int u = 0; u < 2; u++) {
        CAPTURE(u);
        const Route& r = plan.routes[u];
        CHECK(contiguous(r.path));
        const SensorBand* band = stem_band(plan, (float)d->unit_x_positions[u]);
        REQUIRE(band != nullptr);
        CHECK(on_route(r, band->at));
        CHECK(near(seg_start(r.path.segs[0]), (float)d->unit_x_positions[u], 20, 0.5f));
    }
    CHECK(plan.band_count == 2);

    // The active route runs on, unbroken, to the nozzle top.
    const Route& active = plan.routes[0];
    CHECK(near(seg_end(active.path.segs[active.path.count - 1]), 200, 326, 0.5f));
    for (int i = 0; i < active.path.count; i++) {
        CAPTURE(i);
        if (!active.style[i].painted)
            continue;
        CHECK(active.style[i].wall == TubeWall::Active);
        CHECK(active.style[i].filled);
        CHECK(lv_color_eq(active.style[i].bore, lv_color_hex(ACTIVE)));
    }
    // The inactive unit is an empty tube.
    for (int i = 0; i < plan.routes[1].path.count; i++)
        CHECK_FALSE(plan.routes[1].style[i].filled);
}

TEST_CASE("Overview layout: the toolheads stand on their glyph-bottom line",
          "[system_path][filament_path]") {
    auto d = multi();
    const SysLayout single = compute_sys_layout(*d, AREA);
    CHECK(single.nozzle_y == 346);
    CHECK(single.hub_y + single.hub_h / 2 == 213);
    CHECK(single.merge_y == 117);

    d->total_tools = 3;
    const SysLayout row = compute_sys_layout(*d, AREA);
    REQUIRE(row.multi_tool);
    // The tool row's glyphs (scale 7, bottom 19 below center) stand on 320.
    CHECK(small_tool_scale(*d) == 7);
    CHECK(row.tools_y == 320 - 19);
}

TEST_CASE("Overview plan: hub bands take the detail view's four states",
          "[system_path][filament_path]") {
    auto d = multi();

    SECTION("Active: triggered on the unit carrying the route") {
        CHECK(stem_band(plan_of(*d), 100)->state == BandState::Active);
    }
    SECTION("Empty: not triggered") {
        CHECK(stem_band(plan_of(*d), 300)->state == BandState::Empty);
    }
    SECTION("Loaded: triggered on an inactive unit, in its loaded lane's color") {
        d->unit_hub_triggered[1] = true;
        d->unit_lane_segment[1] = PathSegment::HUB;
        d->unit_lane_color[1] = OWL;
        const PathPlan& plan = plan_of(*d);
        const SensorBand* band = stem_band(plan, 300);
        REQUIRE(band != nullptr);
        CHECK(band->state == BandState::Loaded);
        CHECK(lv_color_eq(band->fill, lv_color_hex(OWL)));
        // Its tube is filled down to the combiner, in the lane color, plain walls.
        const Route& r = plan.routes[1];
        CHECK(r.style[0].filled);
        CHECK(r.style[0].wall == TubeWall::Plain);
        CHECK(lv_color_eq(r.style[r.path.count - 1].bore, lv_color_hex(OWL)));
    }
    SECTION("Loaded with no lane reporting a color takes the wall color") {
        d->unit_hub_triggered[1] = true;
        const SensorBand* band = stem_band(plan_of(*d), 300);
        REQUIRE(band != nullptr);
        CHECK(band->state == BandState::Loaded);
        CHECK(lv_color_eq(band->fill, IDLE));
    }
    SECTION("Error: the system error is at the active unit's hub") {
        d->error_segment = PathSegment::HUB;
        const PathPlan& plan = plan_of(*d);
        CHECK(stem_band(plan, 100)->state == BandState::Error);
        // An error belongs to the active route only.
        CHECK(stem_band(plan, 300)->state == BandState::Empty);
    }
}

TEST_CASE("Overview plan: a dumb hub is a continuous tube with no band",
          "[system_path][filament_path]") {
    auto d = multi();
    d->unit_has_hub_sensor[1] = false;
    const PathPlan& plan = plan_of(*d);
    CHECK(plan.band_count == 1);
    CHECK(stem_band(plan, 300) == nullptr);
    CHECK(contiguous(plan.routes[1].path));
}

TEST_CASE("Overview plan: an idle system keeps an idle trunk with its toolhead band",
          "[system_path][filament_path]") {
    auto d = multi();
    d->active_unit = -1;
    d->filament_loaded = false;
    d->filament_segment = PathSegment::NONE;
    d->unit_hub_triggered[0] = false;
    d->has_toolhead_sensor = true;
    const PathPlan& plan = plan_of(*d);
    REQUIRE(plan.trunk_route == 2);
    const Route& trunk = plan.routes[2];
    CHECK(contiguous(trunk.path));
    CHECK(near(seg_start(trunk.path.segs[0]), 200, 213, 0.5f));
    CHECK(near(seg_end(trunk.path.segs[trunk.path.count - 1]), 200, 326, 0.5f));
    REQUIRE(plan.band_count == 3);
    CHECK(plan.bands[2].state == BandState::Empty);
    CHECK(on_route(trunk, plan.bands[2].at));
}

TEST_CASE("Overview plan: an active bypass joins the trunk at the merge",
          "[system_path][filament_path]") {
    auto d = multi();
    d->has_bypass = true;
    d->bypass_active = true;
    d->bypass_color = OWL;
    d->active_unit = -1;
    d->unit_hub_triggered[0] = false;
    const PathPlan& plan = plan_of(*d);
    const SysLayout L = compute_sys_layout(*d, AREA);
    REQUIRE(plan.bypass_route >= 0);
    REQUIRE(plan.trunk_route >= 0);
    const Route& bypass = plan.routes[plan.bypass_route];
    const Route& trunk = plan.routes[plan.trunk_route];
    CHECK(contiguous(bypass.path));
    // The idle trunk stops at the merge, where the bypass turns down to the nozzle.
    const pg::PathPoint merge = seg_end(trunk.path.segs[trunk.path.count - 1]);
    CHECK(on_route(bypass, merge));
    CHECK(near(merge, (float)L.center_x, merge.y));
    CHECK(near(seg_end(bypass.path.segs[bypass.path.count - 1]), (float)L.center_x, 326, 0.5f));
    CHECK(bypass.style[bypass.path.count - 1].filled);
    CHECK(lv_color_eq(bypass.style[bypass.path.count - 1].bore, lv_color_hex(OWL)));
}

TEST_CASE("Overview plan: toolchanger tools each get a continuous route to their nozzle",
          "[system_path][filament_path]") {
    auto d = multi();
    d->unit_count = 1;
    d->unit_x_positions[0] = 200;
    d->unit_topology[0] = 2;
    d->unit_tool_count[0] = 3;
    d->total_tools = 3;
    d->active_tool = 1;
    const PathPlan& plan = plan_of(*d);
    const SysLayout L = compute_sys_layout(*d, AREA);
    REQUIRE(plan.route_count == 3);
    const float nozzle_top = (float)(L.tools_y - small_tool_scale(*d) * 2);
    for (int i = 0; i < 3; i++) {
        CAPTURE(i);
        const Route& r = plan.routes[i];
        CHECK(contiguous(r.path));
        const pg::PathPoint end = seg_end(r.path.segs[r.path.count - 1]);
        CHECK(std::fabs(end.y - nozzle_top) < 0.5f);
        CHECK(r.style[0].filled == (i == plan.active_route));
    }
    REQUIRE(plan.active_route >= 0);
    const pg::PathPoint end = seg_end(
        plan.routes[plan.active_route].path.segs[plan.routes[plan.active_route].path.count - 1]);
    CHECK(std::fabs(end.x - (float)calc_tool_x(1, 3, L.x_off, L.width)) < 0.5f);
}

TEST_CASE_METHOD(LVGLTestFixture, "Overview canvas: the error setter reaches the hub band",
                 "[system_path][filament_path]") {
    lv_obj_t* canvas = ui_system_path_canvas_create(test_screen());
    REQUIRE(canvas != nullptr);
    ui_system_path_canvas_set_unit_count(canvas, 2);
    ui_system_path_canvas_set_unit_x(canvas, 0, 100);
    ui_system_path_canvas_set_unit_x(canvas, 1, 300);
    for (int u = 0; u < 2; u++) {
        ui_system_path_canvas_set_unit_topology(canvas, u, 1);
        ui_system_path_canvas_set_unit_tools(canvas, u, 1, 0);
        ui_system_path_canvas_set_unit_hub_sensor(canvas, u, true, u == 0);
    }
    ui_system_path_canvas_set_total_tools(canvas, 1);
    ui_system_path_canvas_set_active_unit(canvas, 0);
    ui_system_path_canvas_set_filament_segment(canvas, static_cast<int>(PathSegment::NOZZLE));

    const SystemPathData* data = system_path_data(canvas);
    REQUIRE(data != nullptr);
    CHECK(stem_band(plan_of(*data), 100)->state == BandState::Active);

    ui_system_path_canvas_set_error_segment(canvas, static_cast<int>(PathSegment::HUB));
    CHECK(stem_band(plan_of(*data), 100)->state == BandState::Error);

    ui_system_path_canvas_set_unit_lane(canvas, 1, static_cast<int>(PathSegment::HUB), OWL);
    ui_system_path_canvas_set_unit_hub_sensor(canvas, 1, true, true);
    const SensorBand* owl = stem_band(plan_of(*data), 300);
    REQUIRE(owl != nullptr);
    CHECK(owl->state == BandState::Loaded);
    CHECK(lv_color_eq(owl->fill, lv_color_hex(OWL)));

    lv_obj_delete(canvas);
}
