// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// LINEAR/HUB route plan for the filament_path_canvas detail panel.
//
// The [hits] cases render the widget and pin the hub/buffer/bypass hit rects
// the click handler reads, so the renderer records exactly the boxes it draws.
// The rest run the pure frame → plan step on a fixed 400x400 frame: slots at
// x = 50, 150, 250, 350, center 200; entry -48, prep 40, hub 120 (h 40, top
// 100), output 140, buffer 184, merge 232, toolhead 272, nozzle 328, inlet 308.

#include "ui_filament_path_canvas.h"

#include "../lvgl_test_fixture.h"
#include "ams_types.h"
#include "lvgl/lvgl.h"
#include "src/ui/ui_filament_path_internal.h"
#include "src/ui/ui_filament_path_plan.h"

#include <cmath>
#include <memory>

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

// ============================================================================
// Pure plan
// ============================================================================

namespace {

namespace pg = helix::ui::pathgeo;

constexpr uint32_t SLOT_COLORS[4] = {0xE53935, 0x1E88E5, 0x43A047, 0xFDD835};
constexpr uint32_t BYPASS_COLOR = 0x8E24AA;
const lv_color_t BG = lv_color_hex(0x101010);

std::unique_ptr<FilamentPathData> make_data(helix::PathTopology topo) {
    auto d = std::make_unique<FilamentPathData>();
    d->topology = static_cast<int>(topo);
    d->slot_count = 4;
    d->theme.line_width_active = 3;
    d->theme.tube_gauge = 5;
    d->theme.sensor_radius = 4;
    d->theme.hub_width = 60;
    d->theme.extruder_scale = 10;
    d->theme.color_idle = lv_color_hex(0x606060);
    d->theme.color_error = lv_color_hex(0xFF0000);
    d->theme.color_bg = BG;
    d->theme.color_accent = lv_color_hex(0x2196F3);
    for (int i = 0; i < 4; i++)
        d->slot_has_prep_sensor[i] = true;
    d->show_bypass = true;
    d->has_toolhead_sensor = true;
    d->bypass_color = BYPASS_COLOR;
    return d;
}

void load_active(FilamentPathData& d, int slot, helix::PathSegment seg) {
    d.active_slot = slot;
    d.filament_segment = static_cast<int>(seg);
    d.filament_color = SLOT_COLORS[slot];
}

BaseGeometry geometry() {
    BaseGeometry g;
    g.width = 400;
    g.height = 400;
    g.slot_count = 4;
    for (int i = 0; i < 4; i++)
        g.slot_x[i] = 50 + 100 * i;
    g.center_x = 200;
    return g;
}

float hub_entry_x(const FilamentPathData& d, int slot) {
    return compute_linear_hub_frame(d, geometry()).hub_fan[slot].pts[3].x;
}

PathPlan& plan_for(const FilamentPathData& d) {
    static PathPlan plan;
    const BaseGeometry g = geometry();
    plan_linear_hub(compute_linear_hub_frame(d, g), d, g, plan);
    return plan;
}

pg::PathPoint seg_start(const pg::PathSeg& s) {
    if (s.type == pg::PathSeg::LINE)
        return s.p0;
    return {s.center.x + s.radius * std::cos(s.start_angle),
            s.center.y + s.radius * std::sin(s.start_angle)};
}

pg::PathPoint seg_end(const pg::PathSeg& s) {
    if (s.type == pg::PathSeg::LINE)
        return s.p1;
    const float a = s.start_angle + s.sweep;
    return {s.center.x + s.radius * std::cos(a), s.center.y + s.radius * std::sin(a)};
}

bool near(pg::PathPoint p, float x, float y, float eps = 0.01f) {
    return std::fabs(p.x - x) <= eps && std::fabs(p.y - y) <= eps;
}

bool contiguous(const pg::FilamentPath& p) {
    for (int i = 1; i < p.count; i++) {
        const pg::PathPoint a = seg_end(p.segs[i - 1]);
        if (!near(seg_start(p.segs[i]), a.x, a.y))
            return false;
    }
    return true;
}

// A segment of the route ends exactly at (x, y).
bool has_boundary(const pg::FilamentPath& p, float x, float y) {
    for (int i = 0; i < p.count - 1; i++)
        if (near(seg_end(p.segs[i]), x, y))
            return true;
    return false;
}

const SensorBand* band_at(const PathPlan& plan, float x, float y) {
    for (int i = 0; i < plan.band_count; i++)
        if (near(plan.bands[i].at, x, y, 0.5f))
            return &plan.bands[i];
    return nullptr;
}

SpanStyle style_a() {
    return {TubeWall::Active, lv_color_hex(0x112233), true, true};
}
SpanStyle style_b() {
    return {TubeWall::Plain, BG, false, true};
}

Route route_of(std::initializer_list<SpanStyle> styles) {
    Route r;
    float y = 0;
    for (SpanStyle s : styles) {
        pg::FilamentPath piece;
        piece.add_line(0, y, 0, y + 10);
        route_append(r, piece, s);
        y += 10;
    }
    return r;
}

using helix::PathSegment;

} // namespace

TEST_CASE("FilamentPath plan: the frame fixture lays out as the plan states",
          "[filament-path][plan]") {
    auto d = make_data(helix::PathTopology::HUB);
    const LinearHubFrame f = compute_linear_hub_frame(*d, geometry());
    CHECK(f.entry_y == -48);
    CHECK(f.prep_y == 40);
    CHECK(f.hub_y == 120);
    CHECK(f.hub_h == 40);
    CHECK(f.output_y == 140);
    CHECK(f.buffer_y == 184);
    CHECK(f.bypass_merge_y == 232);
    CHECK(f.toolhead_y == 272);
    CHECK(f.nozzle_y == 328);
    CHECK(f.inlet_y == 308);
}

TEST_CASE("FilamentPath plan: coalesce joins equal painted runs", "[filament-path][plan]") {
    Stroke out[16];
    SpanStyle hidden = style_a();
    hidden.painted = false;

    Route r = route_of({style_a(), style_a(), style_b(), style_a()});
    REQUIRE(coalesce(r, out, 16) == 3);
    CHECK((out[0].first == 0 && out[0].end == 2 && out[0].style == style_a()));
    CHECK((out[1].first == 2 && out[1].end == 3 && out[1].style == style_b()));
    CHECK((out[2].first == 3 && out[2].end == 4 && out[2].style == style_a()));

    r = route_of({style_a(), hidden, style_a()});
    REQUIRE(coalesce(r, out, 16) == 2);
    CHECK((out[0].first == 0 && out[0].end == 1));
    CHECK((out[1].first == 2 && out[1].end == 3));

    r = route_of({style_a(), style_a(), style_a(), style_a(), style_a(), style_a(), style_a()});
    REQUIRE(coalesce(r, out, 16) == 1);
    CHECK((out[0].first == 0 && out[0].end == 7));

    CHECK(coalesce(Route{}, out, 16) == 0);
}

TEST_CASE("FilamentPath plan: span_style", "[filament-path][plan]") {
    const lv_color_t fil = lv_color_hex(SLOT_COLORS[1]);
    auto ss = [&](PathSegment span, PathSegment reached, bool on, PathSegment err) {
        return span_style(span, reached, on, err, fil, BG);
    };

    SpanStyle s = ss(PathSegment::LANE, PathSegment::HUB, true, PathSegment::NONE);
    CHECK(s.wall == TubeWall::Active);
    CHECK(s.filled);
    CHECK(lv_color_eq(s.bore, fil));

    s = ss(PathSegment::OUTPUT, PathSegment::HUB, true, PathSegment::NONE);
    CHECK(s.wall == TubeWall::Plain);
    CHECK_FALSE(s.filled);
    CHECK(lv_color_eq(s.bore, BG));

    s = ss(PathSegment::LANE, PathSegment::LANE, false, PathSegment::NONE);
    CHECK(s.wall == TubeWall::Plain);
    CHECK(s.filled);

    s = ss(PathSegment::OUTPUT, PathSegment::NOZZLE, true, PathSegment::OUTPUT);
    CHECK(s.wall == TubeWall::Error);
    CHECK(s.filled);

    s = ss(PathSegment::OUTPUT, PathSegment::HUB, true, PathSegment::OUTPUT);
    CHECK(s.wall == TubeWall::Error);
    CHECK_FALSE(s.filled);

    s = ss(PathSegment::OUTPUT, PathSegment::NOZZLE, false, PathSegment::OUTPUT);
    CHECK(s.wall == TubeWall::Plain);
    CHECK(s.filled);
}

TEST_CASE("FilamentPath plan: band_state", "[filament-path][plan]") {
    CHECK(band_state(PathSegment::HUB, PathSegment::LANE, true, PathSegment::NONE) ==
          BandState::Empty);
    CHECK(band_state(PathSegment::HUB, PathSegment::HUB, true, PathSegment::NONE) ==
          BandState::Active);
    CHECK(band_state(PathSegment::PREP, PathSegment::LANE, false, PathSegment::NONE) ==
          BandState::Loaded);
    CHECK(band_state(PathSegment::PREP, PathSegment::NONE, false, PathSegment::NONE) ==
          BandState::Empty);
    CHECK(band_state(PathSegment::TOOLHEAD, PathSegment::HUB, true, PathSegment::TOOLHEAD) ==
          BandState::Error);
}

TEST_CASE("FilamentPath plan: band_segment crosses the tube", "[filament-path][plan]") {
    pg::PathPoint p0, p1;
    SensorBand b{{200, 140}, {0, 1}, BandState::Empty, BG};
    band_segment(b, 5, p0, p1);
    CHECK(((near(p0, 194.5f, 140) && near(p1, 205.5f, 140)) ||
           (near(p1, 194.5f, 140) && near(p0, 205.5f, 140))));

    b.tangent = {1, 0};
    band_segment(b, 5, p0, p1);
    CHECK(((near(p0, 200, 134.5f) && near(p1, 200, 145.5f)) ||
           (near(p1, 200, 134.5f) && near(p0, 200, 145.5f))));
}

TEST_CASE("FilamentPath plan: HUB active route runs unbroken from spool to inlet",
          "[filament-path][plan]") {
    auto d = make_data(helix::PathTopology::HUB);
    load_active(*d, 1, PathSegment::NOZZLE);
    const PathPlan& plan = plan_for(*d);

    REQUIRE(plan.active_route >= 0);
    const Route& r = plan.routes[plan.active_route];
    REQUIRE(r.path.count > 0);
    CHECK(near(seg_start(r.path.segs[0]), 150, -48));
    CHECK(near(seg_end(r.path.segs[r.path.count - 1]), 200, 308));
    CHECK(contiguous(r.path));
    CHECK(has_boundary(r.path, 150, 40));
    CHECK(has_boundary(r.path, 200, 140));
    CHECK(has_boundary(r.path, 200, 232));
    CHECK(has_boundary(r.path, 200, 272));

    // The unpainted hub interior splits it into one stroke above and one below.
    Stroke strokes[16];
    REQUIRE(coalesce(r, strokes, 16) == 2);
    for (int i = 0; i < 2; i++) {
        CHECK(strokes[i].style.wall == TubeWall::Active);
        CHECK(strokes[i].style.filled);
    }
    CHECK(seg_end(r.path.segs[strokes[0].end - 1]).y <= 100.01f);
    CHECK(near(seg_start(r.path.segs[strokes[1].first]), 200, 140));

    REQUIRE(plan.band_count == 11);
    const float entry_x = hub_entry_x(*d, 1);
    const SensorBand* active[] = {band_at(plan, 150, 40), band_at(plan, entry_x, 100),
                                  band_at(plan, 200, 140), band_at(plan, 200, 232),
                                  band_at(plan, 200, 272)};
    for (const SensorBand* b : active) {
        REQUIRE(b != nullptr);
        CHECK(b->state == BandState::Active);
    }
    int empty = 0;
    for (int i = 0; i < plan.band_count; i++)
        empty += plan.bands[i].state == BandState::Empty;
    CHECK(empty == 6);
}

TEST_CASE("FilamentPath plan: hub-edge bands are painted after the hub box",
          "[filament-path][plan]") {
    auto d = make_data(helix::PathTopology::HUB);
    load_active(*d, 1, PathSegment::NOZZLE);
    const PathPlan& plan = plan_for(*d);

    // paint_tubes paints the rest; render_linear_hub calls paint_box_bands
    // after draw_hub_section, so these clamp the tube over the box edge.
    for (int i = 0; i < 4; i++) {
        const SensorBand* hub = band_at(plan, hub_entry_x(*d, i), 100);
        REQUIRE(hub != nullptr);
        CHECK(hub->on_box_edge);
    }
    const SensorBand* output = band_at(plan, 200, 140);
    REQUIRE(output != nullptr);
    CHECK(output->on_box_edge);

    int on_edge = 0;
    for (int i = 0; i < plan.band_count; i++)
        on_edge += plan.bands[i].on_box_edge;
    CHECK(on_edge == 5);
    CHECK_FALSE(band_at(plan, 150, 40)->on_box_edge);
    CHECK_FALSE(band_at(plan, 200, 232)->on_box_edge);
    CHECK_FALSE(band_at(plan, 200, 272)->on_box_edge);
}

TEST_CASE("FilamentPath plan: a lane loaded to the hub fills to the hub bottom",
          "[filament-path][plan]") {
    auto d = make_data(helix::PathTopology::HUB);
    load_active(*d, 1, PathSegment::HUB);
    const PathPlan& plan = plan_for(*d);

    REQUIRE(plan.active_route >= 0);
    const Route& r = plan.routes[plan.active_route];
    const pg::FilamentPath filled = filled_prefix(r);
    REQUIRE(filled.count > 0);
    CHECK(near(seg_end(filled.segs[filled.count - 1]), 200, 140));
    for (int i = filled.count; i < r.path.count; i++) {
        CHECK(r.style[i].wall == TubeWall::Plain);
        CHECK_FALSE(r.style[i].filled);
    }
}

TEST_CASE("FilamentPath plan: an active bypass owns the trunk below the merge",
          "[filament-path][plan]") {
    auto d = make_data(helix::PathTopology::HUB);
    d->buffer_present = true;
    d->bypass_active = true;
    d->active_slot = -1;
    const PathPlan& plan = plan_for(*d);

    CHECK_FALSE(plan.buffer_has_filament);
    CHECK(plan.active_route == -1);

    REQUIRE(plan.trunk_route >= 0);
    const Route& t = plan.routes[plan.trunk_route];
    REQUIRE(t.path.count > 0);
    CHECK(near(seg_end(t.path.segs[t.path.count - 1]), 200, 232));
    for (int i = 0; i < t.path.count; i++) {
        CHECK(t.style[i].wall == TubeWall::Plain);
        CHECK_FALSE(t.style[i].filled);
    }

    REQUIRE(plan.bypass_route >= 0);
    const Route& b = plan.routes[plan.bypass_route];
    REQUIRE(b.path.count > 0);
    CHECK(near(seg_start(b.path.segs[0]), 400 * (BYPASS_X_RATIO - 0.05f), 232, 1.0f));
    CHECK(near(seg_end(b.path.segs[b.path.count - 1]), 200, 308));
    CHECK(contiguous(b.path));
    for (int i = 0; i < b.path.count; i++) {
        CHECK(b.style[i].wall == TubeWall::Active);
        CHECK(lv_color_eq(b.style[i].bore, lv_color_hex(BYPASS_COLOR)));
    }
}

TEST_CASE("FilamentPath plan: a staged lane stays visible in its own color",
          "[filament-path][plan]") {
    auto d = make_data(helix::PathTopology::HUB);
    load_active(*d, 1, PathSegment::NOZZLE);
    d->slot_filament_states[3] = {PathSegment::LANE, SLOT_COLORS[3]};
    const PathPlan& plan = plan_for(*d);

    const Route& r = plan.routes[3];
    REQUIRE(r.path.count > 0);
    for (int i = 0; i < r.path.count; i++) {
        CHECK(r.style[i].wall == TubeWall::Plain);
        CHECK(r.style[i].filled);
    }
    const pg::PathPoint end = seg_end(r.path.segs[r.path.count - 1]);
    CHECK(end.y == Catch::Approx(100));

    const SensorBand* prep = band_at(plan, 350, 40);
    REQUIRE(prep != nullptr);
    CHECK(prep->state == BandState::Loaded);
    CHECK(lv_color_eq(prep->fill, lv_color_hex(SLOT_COLORS[3])));
    const SensorBand* hub = band_at(plan, end.x, end.y);
    REQUIRE(hub != nullptr);
    CHECK(hub->state == BandState::Empty);
}

TEST_CASE("FilamentPath plan: fill and error follow the segment the filament reached",
          "[filament-path][plan]") {
    auto d = make_data(helix::PathTopology::HUB);

    // Style of the first segment of the entry run (above prep) and of the fan
    // run (below prep) of slot 1's route.
    auto entry_style = [](const Route& r) { return r.style[0]; };
    auto fan_style = [](const Route& r) {
        for (int i = 0; i < r.path.count; i++)
            if (seg_start(r.path.segs[i]).y >= 40 - 0.01f)
                return r.style[i];
        return SpanStyle{};
    };
    const float entry_x = hub_entry_x(*d, 1);

    SECTION("loaded to PREP: the entry run fills, the fan stays empty") {
        load_active(*d, 1, PathSegment::PREP);
        const PathPlan& plan = plan_for(*d);
        const Route& r = plan.routes[1];
        CHECK(entry_style(r).wall == TubeWall::Active);
        CHECK(fan_style(r).wall == TubeWall::Plain);
        CHECK_FALSE(fan_style(r).filled);
        const SensorBand* hub = band_at(plan, entry_x, 100);
        REQUIRE(hub != nullptr);
        CHECK(hub->state == BandState::Empty);
    }
    SECTION("error at PREP with the lane loaded: only the prep band is an error") {
        load_active(*d, 1, PathSegment::LANE);
        d->error_segment = static_cast<int>(PathSegment::PREP);
        const PathPlan& plan = plan_for(*d);
        const Route& r = plan.routes[1];
        const SensorBand* prep = band_at(plan, 150, 40);
        REQUIRE(prep != nullptr);
        CHECK(prep->state == BandState::Error);
        CHECK(entry_style(r).wall == TubeWall::Active);
        CHECK(fan_style(r).wall == TubeWall::Active);
        for (int i = 0; i < r.path.count; i++)
            CHECK(r.style[i].wall != TubeWall::Error);
    }
    SECTION("error at LANE: only the fan run is an error") {
        load_active(*d, 1, PathSegment::LANE);
        d->error_segment = static_cast<int>(PathSegment::LANE);
        const PathPlan& plan = plan_for(*d);
        const Route& r = plan.routes[1];
        CHECK(entry_style(r).wall == TubeWall::Active);
        CHECK(fan_style(r).wall == TubeWall::Error);
    }
}

TEST_CASE("FilamentPath plan: the LINEAR selector passage is painted under the box",
          "[filament-path][plan]") {
    auto d = make_data(helix::PathTopology::LINEAR);
    load_active(*d, 2, PathSegment::OUTPUT);
    const PathPlan& plan = plan_for(*d);

    REQUIRE(plan.active_route == 2);
    const Route& r = plan.routes[2];
    CHECK(contiguous(r.path));
    // Selector: top at prep + sensor_r = 44, bottom at 84, under slot 2.
    bool found = false;
    for (int i = 0; i < r.path.count; i++) {
        if (near(seg_start(r.path.segs[i]), 250, 44) && near(seg_end(r.path.segs[i]), 250, 84)) {
            found = true;
            CHECK(r.style[i].painted);
            CHECK(r.style[i].filled);
        }
    }
    CHECK(found);
}

TEST_CASE("FilamentPath plan: the on-toolhead route fits the segment budget",
          "[filament-path][plan]") {
    auto d = make_data(helix::PathTopology::HUB);
    d->hub_on_toolhead = true;
    d->show_bypass = false;
    load_active(*d, 0, PathSegment::NOZZLE);
    const PathPlan& plan = plan_for(*d);

    REQUIRE(plan.active_route == 0);
    const Route& r = plan.routes[0];
    CHECK(r.path.count <= pg::FilamentPath::MAX_SEGS);
    CHECK(contiguous(r.path));
    CHECK(near(seg_start(r.path.segs[0]), 50, -48));
    CHECK(near(seg_end(r.path.segs[r.path.count - 1]), 200, 308));
}

TEST_CASE("FilamentPath plan: an OUTPUT error is one stroke through the buffer",
          "[filament-path][plan]") {
    auto d = make_data(helix::PathTopology::HUB);
    d->show_bypass = false;
    d->buffer_present = true;
    load_active(*d, 1, PathSegment::NOZZLE);
    d->error_segment = static_cast<int>(PathSegment::OUTPUT);
    const PathPlan& plan = plan_for(*d);

    const Route& r = plan.routes[plan.active_route];
    Stroke strokes[16];
    const int n = coalesce(r, strokes, 16);
    int errors = 0;
    for (int i = 0; i < n; i++) {
        if (strokes[i].style.wall != TubeWall::Error)
            continue;
        errors++;
        CHECK(near(seg_start(r.path.segs[strokes[i].first]), 200, 140));
        CHECK(near(seg_end(r.path.segs[strokes[i].end - 1]), 200, 272));
    }
    CHECK(errors == 1);
}

TEST_CASE("FilamentPath plan: the toolhead band follows the unit's sensor, bypass or not",
          "[filament-path][plan]") {
    auto d = make_data(helix::PathTopology::HUB);
    d->show_bypass = false;
    load_active(*d, 1, PathSegment::NOZZLE);

    SECTION("a unit with a toolhead sensor gets a band on the trunk") {
        const PathPlan& plan = plan_for(*d);
        const SensorBand* b = band_at(plan, 200, 272);
        REQUIRE(b != nullptr);
        CHECK(b->state == BandState::Active);
        const Route& r = plan.routes[plan.active_route];
        CHECK(contiguous(r.path));
        CHECK(has_boundary(r.path, 200, 272));
        CHECK(near(seg_end(r.path.segs[r.path.count - 1]), 200, 308));
    }
    SECTION("a unit without one gets none") {
        d->has_toolhead_sensor = false;
        const PathPlan& plan = plan_for(*d);
        CHECK(band_at(plan, 200, 272) == nullptr);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "FilamentPath: the animation replays one unbroken active path",
                 "[filament-path][plan]") {
    lv_obj_t* w = make_canvas(test_screen(), static_cast<int>(helix::PathTopology::HUB));
    ui_filament_path_canvas_set_active_slot(w, 1);
    ui_filament_path_canvas_set_filament_segment(w, static_cast<int>(PathSegment::NOZZLE));
    render(*this, w);

    const FilamentPathData* d = get_data(w);
    REQUIRE(d->path_cache.valid);
    REQUIRE(d->path_cache.path.count > 0);
    CHECK(contiguous(d->path_cache.path));
}
