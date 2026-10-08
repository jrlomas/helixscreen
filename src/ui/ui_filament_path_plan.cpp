// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// LINEAR/HUB route plan: the pure frame → plan step and the layered painter.
// See ui_filament_path_plan.h for the model.

#include "ui_filament_path_plan.h"

#include <algorithm>
#include <cmath>

namespace helix::ui::fpath {

// ============================================================================
// Frame
// ============================================================================

lv_color_t pulsed_error_color(const FilamentPathData& data) {
    const lv_color_t error = data.theme.color_error;
    if (!data.anim.error_pulse_active || data.anim.error_pulse_opa >= LV_OPA_COVER)
        return error;
    const float blend = (float)(LV_OPA_COVER - data.anim.error_pulse_opa) /
                        (float)(LV_OPA_COVER - ERROR_PULSE_OPA_MIN);
    return ph_blend(error, ph_darken(error, 80), blend);
}

int32_t tube_gauge_for_spacing(int32_t space_xs) {
    return LV_MAX(3, space_xs - 3) + 2;
}

TubePalette tube_palette(const FilamentPathData& data) {
    const ThemeCache& t = data.theme;
    return {t.color_idle, t.color_accent, pulsed_error_color(data), t.color_bg, t.tube_gauge};
}

// Layout mirrors the ratios at the top of ui_filament_path_internal.h; LINEAR
// butts the selector against the prep sensors and slides the output exit under
// the active slot.
LinearHubFrame compute_linear_hub_frame(const FilamentPathData& data, const BaseGeometry& g,
                                        int32_t glyph_top) {
    LinearHubFrame f;
    const ThemeCache& theme = data.theme;
    const bool linear = data.topology == static_cast<int>(PathTopology::LINEAR);

    f.entry_y = g.y_off + (int32_t)(g.height * ENTRY_Y_RATIO);
    f.prep_y = g.y_off + (int32_t)(g.height * PREP_Y_RATIO);
    f.hub_y = g.y_off + (int32_t)(g.height * HUB_Y_RATIO);
    f.hub_h = (int32_t)(g.height * HUB_HEIGHT_RATIO);
    f.toolhead_y = g.y_off + (int32_t)(g.height * TOOLHEAD_Y_RATIO);
    if (data.hub_on_toolhead) {
        // The selector is part of the unit, butted against the prep sensors;
        // the hub moves down to the print head with a short shared stub below.
        f.selector_y = f.prep_y + (int32_t)(g.height * (HUB_HEIGHT_RATIO / 2 + 0.02f));
        const int32_t stub = LV_MAX(10, (int32_t)(g.height * 0.03f));
        f.hub_y = f.toolhead_y - stub - f.hub_h / 2;
    }
    f.nozzle_y = g.y_off + (int32_t)(g.height * NOZZLE_Y_RATIO);
    f.inlet_y = f.nozzle_y - theme.extruder_scale * 2;
    f.bypass_merge_y = g.y_off + (int32_t)(g.height * BYPASS_MERGE_Y_RATIO);
    f.center_x = g.center_x;
    f.sensor_r = theme.sensor_radius;

    f.buffer_y = g.y_off + (int32_t)(g.height * BUFFER_Y_RATIO);
    const int32_t buf_box_h = LV_MAX(16, f.hub_h);
    f.has_buffer = data.buffer_present;

    // HUB with no bypass lane: the hub and buffer hang above the toolhead glyph,
    // giving the lane fan the height they would otherwise occupy. The stack
    // only ever moves them down; a canvas too short for it keeps the ratio
    // layout.
    if (!linear && !data.hub_on_toolhead && !data.show_bypass && !data.hub_only) {
        // Equal clearance above and below the buffer box (or above the glyph
        // when there is none): hub bottom, buffer and glyph top evenly spaced.
        const int32_t gap = f.hub_h / 2 + 2 * f.sensor_r;
        int32_t stacked_buffer_y = f.buffer_y;
        int32_t hub_bottom = glyph_top - 2 * gap;
        if (f.has_buffer) {
            hub_bottom = glyph_top - (buf_box_h + 2 * gap);
            stacked_buffer_y = (hub_bottom + glyph_top) / 2;
        }
        const int32_t stacked_hub_y = hub_bottom - f.hub_h / 2;
        if (stacked_hub_y > f.hub_y) {
            f.hub_stacked = true;
            f.hub_y = stacked_hub_y;
            if (f.has_buffer)
                f.buffer_y = stacked_buffer_y;
            // The toolhead sensor band sits in the clear gap above the glyph.
            const int32_t last_box_bottom = f.has_buffer ? f.buffer_y + buf_box_h / 2 : hub_bottom;
            f.toolhead_y = (last_box_bottom + glyph_top) / 2;
        }
    }
    f.buf_fil_top = f.buffer_y - buf_box_h;

    if (linear)
        f.hub_y = f.prep_y + f.sensor_r + f.hub_h / 2;
    // Output sensor butted against the hub bottom (mirrors the hub-top entries)
    f.output_y = f.hub_y + f.hub_h / 2;

    f.idle_color = theme.color_idle;
    f.active_color = lv_color_hex(data.filament_color);
    f.hub_bg = theme.color_hub_bg;
    f.hub_border = theme.color_hub_border;
    f.error_color = pulsed_error_color(data);

    f.output_x = f.center_x;
    if (linear && data.active_slot >= 0 && data.active_slot < g.slot_count) {
        f.output_x =
            data.anim.output_x_active ? data.anim.output_x_current : g.slot_x[data.active_slot];
    }

    f.has_error = data.error_segment > 0;
    f.error_seg = static_cast<PathSegment>(data.error_segment);
    f.fil_seg = static_cast<PathSegment>(data.filament_segment);
    f.states = compute_slot_render_states(&data);

    // HUB merge fan. Horizontal entry spacing alone does not keep shallow
    // diagonals apart: the hub is widened until neighbouring tubes clear the
    // tube, its halo and a gap, and drawn at exactly that width.
    f.hub_box_w = theme.hub_width;
    if (linear)
        return f;
    int32_t hub_top = f.hub_y - f.hub_h / 2;
    pg::MergeLaneIn fan_in[FilamentPathData::MAX_SLOTS];
    const int fan_n = LV_MIN(data.slot_count, FilamentPathData::MAX_SLOTS);
    for (int i = 0; i < fan_n; i++) {
        // Tubes leave the on-toolhead selector's bottom edge, else the prep sensor.
        int32_t start_y = data.slot_has_prep_sensor[i] ? (f.prep_y + f.sensor_r) : f.prep_y;
        if (data.hub_on_toolhead)
            start_y = f.selector_y + f.hub_h / 2;
        fan_in[i] = {(float)g.slot_x[i], (float)start_y};
    }
    constexpr int32_t TARGET_ENTRY_SPACING = 22;
    constexpr int32_t ENTRY_MARGIN = 8;
    const int32_t want_w =
        (fan_n > 1) ? (fan_n - 1) * TARGET_ENTRY_SPACING + 2 * ENTRY_MARGIN : theme.hub_width;
    const int32_t slot_span =
        (data.slot_count > 1) ? (g.slot_x[data.slot_count - 1] - g.slot_x[0]) : theme.hub_width;
    const int32_t max_width = LV_MAX(theme.hub_width, slot_span + 2 * ENTRY_MARGIN);
    const int32_t min_width = LV_CLAMP(theme.hub_width, want_w, max_width);
    // Outer tube + its halo + 2 px between halos, and never less than two bands.
    const int32_t separation = LV_MAX(theme.tube_gauge + HALO_WIDTH_EXTRA + 2, 2 * f.sensor_r + 2);
    if (fan_n > 2 && !data.hub_on_toolhead && !f.hub_stacked) {
        // Borrow unused output-run height before widening the hub. Keep the
        // buffer/bypass area clear and leave on-toolhead hubs in place.
        float deepest_start = fan_in[0].start_y;
        for (int i = 1; i < fan_n; ++i)
            deepest_start = std::max(deepest_start, fan_in[i].start_y);
        const int32_t next_y = f.has_buffer ? f.buf_fil_top : f.bypass_merge_y;
        const int32_t max_top = next_y - f.hub_h - 2 * f.sensor_r - 8;
        const int32_t wanted_top = (int32_t)deepest_start + 24 + 2 * separation + f.sensor_r;
        hub_top = LV_MAX(hub_top, LV_MIN(wanted_top, max_top));
        f.hub_y = hub_top + f.hub_h / 2;
        f.output_y = f.hub_y + f.hub_h / 2;
    }
    // The width is fitted to a fan ending one sensor radius above the hub, so
    // the final legs keep their clearance across the hub-entry bands.
    const int32_t tube_end_y = hub_top - f.sensor_r;
    f.hub_box_w = (int32_t)std::ceil(pg::merge_fan_width(
        fan_in, fan_n, (float)f.center_x, (float)tube_end_y, (float)min_width, (float)max_width,
        (float)ENTRY_MARGIN, /*fillet_r=*/8.0f, /*max_slope=*/1.2f, (float)separation));
    pg::build_merge_fan(fan_in, fan_n, (float)f.center_x, (float)hub_top, (float)f.hub_box_w,
                        (float)ENTRY_MARGIN, /*fillet_r=*/8.0f, /*max_slope=*/1.2f, f.hub_fan);
    return f;
}

// MIXED layout ratios: more vertical spread than PARALLEL to fit hub + nozzles.
MixedFrame compute_mixed_frame(const FilamentPathData& data, const BaseGeometry& g) {
    MixedFrame f;
    constexpr float SENSOR_Y = 0.15f;
    constexpr float HUB_Y = 0.32f;
    constexpr float HUB_H = 0.08f;
    constexpr float TOOLHEAD_Y = 0.62f;

    f.entry_y = g.y_off + (int32_t)(g.height * ENTRY_Y_RATIO);
    f.sensor_y = g.y_off + (int32_t)(g.height * SENSOR_Y);
    f.hub_h = LV_MAX(16, (int32_t)(g.height * HUB_H));
    f.toolhead_y = g.y_off + (int32_t)(g.height * TOOLHEAD_Y);
    f.tool_scale = LV_MAX(6, data.theme.extruder_scale * 2 / 3);
    if (data.hub_on_toolhead) {
        // Combiner on the print head: the box hugs the toolhead over a short
        // stub of shared tube, measured from the nozzle glyph's top so the two
        // never overlap.
        const int32_t nozzle_top = f.toolhead_y - f.tool_scale * 2;
        const int32_t stub = LV_MAX(10, (int32_t)(g.height * 0.03f));
        f.hub_bottom = nozzle_top - stub;
        f.hub_cy = f.hub_bottom - f.hub_h / 2;
    } else {
        f.hub_cy = g.y_off + (int32_t)(g.height * HUB_Y);
        f.hub_bottom = f.hub_cy + f.hub_h / 2;
    }

    f.states = compute_slot_render_states(&data);

    const int n = LV_MIN(data.slot_count, FilamentPathData::MAX_SLOTS);
    int32_t hub_x_sum = 0;
    for (int i = 0; i < n; i++) {
        if (data.slot_is_hub_routed[i]) {
            hub_x_sum += g.slot_x[i];
            f.hub_count++;
            if (f.first_hub_lane < 0)
                f.first_hub_lane = i;
        }
    }
    f.hub_cx = (f.hub_count > 0) ? (hub_x_sum / f.hub_count) : (g.x_off + 150);
    // ~60% of the full hub topology width, enough for the hub lanes
    f.hub_w = LV_MAX(40, data.theme.hub_width * 3 / 5);

    for (int i = 0; i < FilamentPathData::MAX_SLOTS; i++)
        f.slot_to_fan[i] = -1;
    const int32_t hub_top = f.hub_cy - f.hub_h / 2;
    pg::MergeLaneIn fan_in[FilamentPathData::MAX_SLOTS];
    int fan_n = 0;
    for (int i = 0; i < n; i++) {
        if (!data.slot_is_hub_routed[i])
            continue;
        fan_in[fan_n] = {(float)g.slot_x[i], (float)f.sensor_y};
        f.slot_to_fan[i] = fan_n++;
    }
    pg::build_merge_fan(fan_in, fan_n, (float)f.hub_cx, (float)hub_top, (float)f.hub_w,
                        /*entry_margin=*/8.0f, /*fillet_r=*/8.0f, /*max_slope=*/1.2f, f.hub_fan);
    return f;
}

// ============================================================================
// Routes, styles, bands
// ============================================================================

bool operator==(const SpanStyle& a, const SpanStyle& b) {
    return a.wall == b.wall && lv_color_eq(a.bore, b.bore) && a.filled == b.filled &&
           a.painted == b.painted && a.fade == b.fade;
}

bool route_append(Route& r, const pg::FilamentPath& piece, SpanStyle s) {
    for (int i = 0; i < piece.count; i++) {
        if (r.path.count == pg::FilamentPath::MAX_SEGS) {
            r.dropped += piece.count - i;
            return false;
        }
        r.style[r.path.count] = s;
        r.path.segs[r.path.count++] = piece.segs[i];
    }
    return true;
}

int coalesce(const Route& r, Stroke* out, int max_out) {
    int n = 0;
    for (int i = 0; i < r.path.count;) {
        if (!r.style[i].painted) {
            i++;
            continue;
        }
        int j = i + 1;
        while (j < r.path.count && r.style[j] == r.style[i])
            j++;
        if (n < max_out)
            out[n++] = {i, j, r.style[i]};
        i = j;
    }
    return n;
}

pg::FilamentPath filled_prefix(const Route& r) {
    pg::FilamentPath p;
    int last = -1;
    for (int i = 0; i < r.path.count; i++)
        if (r.style[i].filled)
            last = i;
    for (int i = 0; i <= last; i++)
        p.segs[p.count++] = r.path.segs[i];
    return p;
}

SpanStyle span_style(PathSegment span, PathSegment reached, bool on_active_route,
                     PathSegment error_seg, lv_color_t filament, lv_color_t bg) {
    const bool error = on_active_route && span == error_seg;
    if (!is_segment_active(span, reached))
        return {error ? TubeWall::Error : TubeWall::Plain, bg, false, true};
    const TubeWall wall =
        error ? TubeWall::Error : (on_active_route ? TubeWall::Active : TubeWall::Plain);
    return {wall, filament, true, true};
}

BandState band_state(PathSegment sensor, PathSegment reached, bool on_active_route,
                     PathSegment error_seg) {
    if (on_active_route && sensor == error_seg)
        return BandState::Error;
    if (!is_segment_active(sensor, reached))
        return BandState::Empty;
    return on_active_route ? BandState::Active : BandState::Loaded;
}

void band_segment(const SensorBand& band, int32_t gauge, pg::PathPoint& p0, pg::PathPoint& p1) {
    const float h = (gauge + BAND_EXTRA) / 2.0f - BAND_THICKNESS / 2.0f;
    const float nx = -band.tangent.y;
    const float ny = band.tangent.x;
    p0 = {band.at.x - nx * h, band.at.y - ny * h};
    p1 = {band.at.x + nx * h, band.at.y + ny * h};
}

void draw_sensor_band(lv_layer_t* layer, const SensorBand& band, int32_t gauge, lv_color_t color) {
    pg::PathPoint p0, p1;
    band_segment(band, gauge, p0, p1);
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = BAND_THICKNESS;
    dsc.opa = LV_OPA_COVER;
    dsc.p1.x = p0.x;
    dsc.p1.y = p0.y;
    dsc.p2.x = p1.x;
    dsc.p2.y = p1.y;
    dsc.round_start = true;
    dsc.round_end = true;
    lv_draw_line(layer, &dsc);
}

void append_line(Route& r, float x0, float y0, float x1, float y1, SpanStyle s) {
    pg::FilamentPath piece;
    piece.add_line(x0, y0, x1, y1);
    route_append(r, piece, s);
}

void add_band(PathPlan& plan, BandKind kind, pg::PathPoint at, pg::PathPoint tangent,
              BandState state, lv_color_t fill, bool on_box_edge) {
    const bool full = (kind == BandKind::Lane)
                          ? plan.band_count - plan.trunk_band_count >= MAX_BANDS - TRUNK_BANDS
                          : plan.band_count >= MAX_BANDS;
    if (full) {
        plan.dropped++;
        return;
    }
    plan.trunk_band_count += kind == BandKind::Trunk;
    plan.bands[plan.band_count++] = {at, tangent, state, fill, on_box_edge};
}

// A band where the route currently ends, across its last segment.
void add_band_at_end(PathPlan& plan, BandKind kind, const Route& r, BandState state,
                     lv_color_t fill, bool on_box_edge) {
    if (r.path.count == 0)
        return;
    pg::PathPoint tangent;
    const pg::PathPoint at = pg::path_point_at(r.path, pg::path_length(r.path), &tangent);
    add_band(plan, kind, at, tangent, state, fill, on_box_edge);
}

Route& new_route(PathPlan& plan) {
    Route& r = plan.routes[plan.route_count++];
    r.path.clear();
    r.dropped = 0;
    return r;
}

SpanStyle unpainted(SpanStyle s) {
    s.painted = false;
    return s;
}

namespace {

// Hub bottom → inlet, appended to the route that owns the trunk. Ends at the
// merge when an active bypass (@p bypass_owner) owns everything below it.
void append_trunk(PathPlan& plan, Route& r, const Lane& lane, const LinearHubFrame& f,
                  const FilamentPathData& data, bool bypass_on_trunk, const Lane* bypass_owner) {
    const float cx = (float)f.center_x;
    const float hub_bot = (float)f.output_y;
    if (!data.hub_on_toolhead) {
        add_band(plan, BandKind::Trunk, {(float)f.output_x, hub_bot}, {0, 1},
                 lane.band(PathSegment::OUTPUT), lane.color, /*on_box_edge=*/true);
    }

    const float output_end = (float)(bypass_on_trunk ? f.bypass_merge_y : f.toolhead_y);
    // LINEAR: the exit jogs from under the active slot to the center above the buffer.
    const float jog_end = f.has_buffer ? (float)f.buf_fil_top : output_end;
    pg::FilamentPath piece;
    pg::route_orthogonal(piece, (float)f.output_x, hub_bot, cx, jog_end, FILLET_RADIUS);
    route_append(r, piece, lane.style(PathSegment::OUTPUT));
    if (jog_end < output_end)
        append_line(r, cx, jog_end, cx, output_end, lane.style(PathSegment::OUTPUT));

    if (bypass_on_trunk) {
        if (bypass_owner) {
            add_band_at_end(plan, BandKind::Trunk, r, bypass_owner->band(PathSegment::OUTPUT),
                            bypass_owner->color);
            return;
        }
        add_band_at_end(plan, BandKind::Trunk, r, lane.band(PathSegment::OUTPUT), lane.color);
        append_line(r, cx, output_end, cx, (float)f.toolhead_y, lane.style(PathSegment::TOOLHEAD));
    }
    if (data.has_toolhead_sensor)
        add_band_at_end(plan, BandKind::Trunk, r, lane.band(PathSegment::TOOLHEAD), lane.color);
    append_line(r, cx, (float)f.toolhead_y, cx, (float)f.inlet_y, lane.style(PathSegment::NOZZLE));
}

// The hub_only output stub: a short tube leaving the hub (or selector) bottom
// that fades out over its last third, so it reads as continuing elsewhere. It
// carries filament once the lane is past the hub or the hub sensor reads it.
void append_output_stub(PathPlan& plan, Route& r, Lane lane, const LinearHubFrame& f,
                        const FilamentPathData& data, float max_y) {
    if (data.hub_sensor_triggered && lane.reached < PathSegment::OUTPUT)
        lane.reached = PathSegment::OUTPUT;
    const float x = (float)f.output_x;
    const float top = (float)f.output_y;
    if (data.has_hub_sensor) {
        add_band(plan, BandKind::Trunk, {x, top}, {0, 1}, lane.band(PathSegment::OUTPUT),
                 lane.color, /*on_box_edge=*/true);
    }
    const float len = LV_MIN((float)data.theme.stub_length, max_y - top);
    if (len <= 0)
        return;
    const SpanStyle s = lane.style(PathSegment::OUTPUT);
    const float solid = len * 2 / 3;
    append_line(r, x, top, x, top + solid, s);
    constexpr int FADE_STEPS = 4;
    const float step = (len - solid) / FADE_STEPS;
    for (int k = 0; k < FADE_STEPS; k++) {
        SpanStyle faded = s;
        faded.fade = (uint8_t)(255 * (k + 1) / (FADE_STEPS + 1));
        append_line(r, x, top + solid + step * k, x, top + solid + step * (k + 1), faded);
    }
}

} // namespace

// ============================================================================
// Plan
// ============================================================================

void reset_plan(PathPlan& out) {
    out.route_count = 0;
    out.band_count = 0;
    out.trunk_band_count = 0;
    out.dropped = 0;
    out.active_route = -1;
    out.trunk_route = -1;
    out.bypass_route = -1;
    out.buffer_has_filament = false;
}

void total_dropped(PathPlan& out) {
    for (int i = 0; i < out.route_count; i++)
        out.dropped += out.routes[i].dropped;
}

namespace {

// A lane's own state: how far its filament reached, mounted or not.
Lane lane_of(const SlotRenderState& s, PathSegment error, lv_color_t bg) {
    return {s.has_filament ? s.segment : PathSegment::NONE, s.is_mounted, error, s.color, bg};
}

// Spool entry down to the lane's sensor, with its band. The sensor reads
// filament once it has reached the toolhead segment.
Route& start_lane(PathPlan& out, const Lane& lane, float x, int32_t entry_y, int32_t sensor_y) {
    Route& r = new_route(out);
    append_line(r, x, (float)entry_y, x, (float)sensor_y, lane.style(PathSegment::SPOOL));
    add_band_at_end(out, BandKind::Lane, r, lane.band(PathSegment::TOOLHEAD), lane.color);
    return r;
}

} // namespace

void plan_parallel(const FilamentPathData& data, const BaseGeometry& g, PathPlan& out) {
    reset_plan(out);
    const int32_t entry_y = g.y_off + (int32_t)(g.height * ENTRY_Y_RATIO);
    const int32_t sensor_y = g.y_off + (int32_t)(g.height * PARALLEL_SENSOR_Y_RATIO);
    const int32_t toolhead_y = g.y_off + (int32_t)(g.height * PARALLEL_TOOLHEAD_Y_RATIO);
    const int32_t tool_scale = LV_MAX(6, data.theme.extruder_scale * 2 / 3);
    const float nozzle_top = (float)(toolhead_y - tool_scale * 2);
    const PathSegment error = static_cast<PathSegment>(data.error_segment);
    const SlotRenderStates states = compute_slot_render_states(&data);

    const int n = LV_MIN(data.slot_count, FilamentPathData::MAX_SLOTS);
    for (int i = 0; i < n; i++) {
        const Lane lane = lane_of(states[i], error, data.theme.color_bg);
        const float x = (float)g.slot_x[i];
        Route& r = start_lane(out, lane, x, entry_y, sensor_y);
        append_line(r, x, (float)sensor_y, x, nozzle_top, lane.style(PathSegment::NOZZLE));
        if (lane.on)
            out.active_route = i;
    }
    total_dropped(out);
}

void plan_mixed(const MixedFrame& f, const FilamentPathData& data, const BaseGeometry& g,
                PathPlan& out) {
    reset_plan(out);
    const lv_color_t bg = data.theme.color_bg;
    const float nozzle_top = (float)(f.toolhead_y - f.tool_scale * 2);
    const PathSegment error = static_cast<PathSegment>(data.error_segment);

    const int n = LV_MIN(data.slot_count, FilamentPathData::MAX_SLOTS);
    for (int i = 0; i < n; i++) {
        const Lane lane = lane_of(f.states[i], error, bg);
        const float x = (float)g.slot_x[i];
        Route& r = start_lane(out, lane, x, f.entry_y, f.sensor_y);
        const int fi = f.slot_to_fan[i];
        if (fi >= 0) {
            pg::FilamentPath fan;
            pg::route_polyline_filleted(fan, f.hub_fan[fi].pts, 4, 8.0f);
            route_append(r, fan, lane.style(PathSegment::SPOOL));
        } else {
            append_line(r, x, (float)f.sensor_y, x, nozzle_top, lane.style(PathSegment::NOZZLE));
        }
        if (lane.on)
            out.active_route = i;
    }

    if (f.hub_count > 0) {
        // The shared trunk shows the first hub lane that reached the nozzle.
        // With none there it is idle, and still shows an error unless a direct
        // lane is the mounted one.
        const bool direct_mounted = data.active_slot >= 0 && data.active_slot < n &&
                                    !data.slot_is_hub_routed[data.active_slot];
        Lane trunk{PathSegment::NONE, !direct_mounted, error, f.states[f.first_hub_lane].color, bg};
        for (int j = 0; j < n; j++) {
            if (data.slot_is_hub_routed[j] && f.states[j].segment >= PathSegment::NOZZLE) {
                trunk = lane_of(f.states[j], error, bg);
                break;
            }
        }
        out.trunk_route = out.route_count;
        Route& r = new_route(out);
        append_line(r, (float)f.hub_cx, (float)f.hub_bottom, (float)f.hub_cx, nozzle_top,
                    trunk.style(PathSegment::NOZZLE));
    }
    total_dropped(out);
}

void plan_linear_hub(const LinearHubFrame& f, const FilamentPathData& data, const BaseGeometry& g,
                     PathPlan& out) {
    reset_plan(out);

    const lv_color_t bg = data.theme.color_bg;
    const bool linear = data.topology == static_cast<int>(PathTopology::LINEAR);
    const bool on_head = data.hub_on_toolhead && !linear;
    const int n = LV_MIN(data.slot_count, FilamentPathData::MAX_SLOTS);
    const int active = (data.active_slot >= 0 && data.active_slot < n) ? data.active_slot : -1;
    const bool trunk = !data.hub_only;
    const bool bypass = trunk && data.show_bypass;
    // On-toolhead the merge point sits above the head hub, off the trunk.
    const bool bypass_on_trunk = bypass && !on_head;
    const bool bypass_owns = bypass_on_trunk && data.bypass_active;
    // The bypass filament reached the nozzle; on its own route it takes the
    // same error rules as an AMS lane.
    const Lane bypass_lane{data.bypass_active ? PathSegment::NOZZLE : PathSegment::NONE,
                           data.bypass_active, f.error_seg, lv_color_hex(data.bypass_color),
                           data.theme.color_bg};
    const Lane* bypass_owner = bypass_owns ? &bypass_lane : nullptr;
    // One unit of several: its hub's output leaves as a stub, the toolhead
    // belongs to the overview.
    const bool stub = data.hub_only && !on_head;
    const float stub_end = (float)(g.y_off + g.height - 2);
    // Box edges in whole pixels, as the boxes are drawn.
    const int32_t hub_top_px = f.hub_y - f.hub_h / 2;
    const float hub_top = (float)hub_top_px;
    const float hub_bot = (float)f.output_y;
    const float cx = (float)f.center_x;

    out.buffer_has_filament =
        active >= 0 && !data.bypass_active && is_segment_active(PathSegment::OUTPUT, f.fil_seg);
    out.buffer_fill = f.active_color;

    for (int i = 0; i < n; i++) {
        const SlotRenderState& s = f.states[i];
        const Lane lane{s.has_filament ? s.segment : PathSegment::NONE, i == active, f.error_seg,
                        s.color, bg};
        const float x = (float)g.slot_x[i];
        Route& r = new_route(out);

        append_line(r, x, (float)f.entry_y, x, (float)f.prep_y, lane.style(PathSegment::SPOOL));
        if (data.slot_has_prep_sensor[i])
            add_band_at_end(out, BandKind::Lane, r, lane.band(PathSegment::PREP), s.color);

        if (linear) {
            append_line(r, x, (float)f.prep_y, x, hub_top, lane.style(PathSegment::LANE));
            if (!lane.on)
                continue;
            // Painted: the selector is translucent, so the tube shows through it.
            append_line(r, x, hub_top, (float)f.output_x, hub_bot, lane.style(PathSegment::HUB));
        } else {
            pg::PathPoint pts[4] = {f.hub_fan[i].pts[0], f.hub_fan[i].pts[1], f.hub_fan[i].pts[2],
                                    f.hub_fan[i].pts[3]};
            if (on_head) {
                const int32_t sel_top_px = f.selector_y - f.hub_h / 2;
                const float sel_top = (float)sel_top_px;
                append_line(r, x, (float)f.prep_y, x, sel_top, lane.style(PathSegment::LANE));
                append_line(r, x, sel_top, x, pts[0].y, unpainted(lane.style(PathSegment::LANE)));
            } else {
                pts[0].y = (float)f.prep_y;
            }
            pg::FilamentPath fan;
            pg::route_polyline_filleted(fan, pts, 4, 8.0f);
            route_append(r, fan, lane.style(PathSegment::LANE));
            add_band_at_end(out, BandKind::Lane, r, lane.band(PathSegment::HUB), s.color,
                            /*on_box_edge=*/true);
            if (!lane.on)
                continue;
            append_line(r, pts[3].x, hub_top, cx, hub_bot, unpainted(lane.style(PathSegment::HUB)));
        }

        out.active_route = i;
        if (trunk)
            append_trunk(out, r, lane, f, data, bypass_on_trunk, bypass_owner);
        else if (stub)
            append_output_stub(out, r, lane, f, data, stub_end);
    }

    if (stub && active < 0) {
        // No lane of this unit is mounted: the stub is the unit's own, idle
        // unless its hub sensor reads filament.
        const Lane idle{PathSegment::NONE, true, f.error_seg, f.idle_color, bg};
        out.trunk_route = out.route_count;
        Route& r = new_route(out);
        append_output_stub(out, r, idle, f, data, stub_end);
    }

    if (trunk && active < 0) {
        // Nothing loaded owns the trunk. It still shows an error at its sensors.
        const Lane idle{PathSegment::NONE, true, f.error_seg, f.idle_color, bg};
        out.trunk_route = out.route_count;
        Route& r = new_route(out);
        append_trunk(out, r, idle, f, data, bypass_on_trunk, bypass_owner);
    }

    if (bypass) {
        const float merge_y = (float)f.bypass_merge_y;
        // The spool widget is centered at BYPASS_X_RATIO; the tube stops at its left edge.
        const float spool_edge = (float)(g.x_off + (int32_t)(g.width * (BYPASS_X_RATIO - 0.05f)));
        out.bypass_route = out.route_count;
        Route& b = new_route(out);
        // Spool → merge carries the SPOOL tag: no AMS error lands on it.
        append_line(b, spool_edge, merge_y, cx, merge_y, bypass_lane.style(PathSegment::SPOOL));
        if (bypass_owns) {
            append_line(b, cx, merge_y, cx, (float)f.toolhead_y,
                        bypass_lane.style(PathSegment::TOOLHEAD));
            if (data.has_toolhead_sensor)
                add_band_at_end(out, BandKind::Trunk, b, bypass_lane.band(PathSegment::TOOLHEAD),
                                bypass_lane.color);
            append_line(b, cx, (float)f.toolhead_y, cx, (float)f.inlet_y,
                        bypass_lane.style(PathSegment::NOZZLE));
        }
    }

    total_dropped(out);
}

// ============================================================================
// Paint
// ============================================================================

namespace {

// A faded span is pre-blended toward the background, so its strokes stay opaque
// and overlapping caps never double-blend.
LaneStyle lane_style_for(const SpanStyle& s, const TubePalette& pal, bool simple) {
    lv_color_t wall = s.wall == TubeWall::Active  ? pal.accent
                      : s.wall == TubeWall::Error ? pal.error
                                                  : pal.idle_wall;
    lv_color_t bore = s.bore;
    if (s.fade > 0 && !simple) {
        const float t = s.fade / 255.0f;
        wall = ph_blend(wall, pal.bg, t);
        bore = ph_blend(bore, pal.bg, t);
    }
    return {wall, bore, pal.bg, pal.gauge, s.wall == TubeWall::Active};
}

void stroke_layer(lv_layer_t* layer, const Route& r, const Stroke& st, const TubePalette& pal,
                  TubeLayer which, bool simple) {
    TubePass passes[2];
    const int n = build_passes(lane_style_for(st.style, pal, simple), which, passes, simple);
    if (n == 0)
        return;
    pg::FilamentPath sub;
    for (int i = st.first; i < st.end; i++)
        sub.segs[sub.count++] = r.path.segs[i];
    stroke_path(layer, sub, passes, n);
}

void paint_bands(lv_layer_t* layer, const PathPlan& plan, const TubePalette& pal,
                 bool on_box_edge) {
    for (int i = 0; i < plan.band_count; i++) {
        const SensorBand& b = plan.bands[i];
        if (b.on_box_edge != on_box_edge)
            continue;
        const lv_color_t color = b.state == BandState::Active   ? pal.accent
                                 : b.state == BandState::Error  ? pal.error
                                 : b.state == BandState::Loaded ? b.fill
                                                                : pal.idle_wall;
        draw_sensor_band(layer, b, pal.gauge, color);
    }
}

} // namespace

void paint_tubes(lv_layer_t* layer, const PathPlan& plan, const TubePalette& pal, bool simple) {
    auto each = [&](TubeLayer which, auto&& want) {
        Stroke strokes[pg::FilamentPath::MAX_SEGS];
        for (int ri = 0; ri < plan.route_count; ri++) {
            const int n = coalesce(plan.routes[ri], strokes, pg::FilamentPath::MAX_SEGS);
            for (int k = 0; k < n; k++)
                if (want(ri, strokes[k]))
                    stroke_layer(layer, plan.routes[ri], strokes[k], pal, which, simple);
        }
    };

    each(TubeLayer::Halo, [](int, const Stroke& s) { return s.style.wall == TubeWall::Active; });
    each(TubeLayer::Wall, [](int, const Stroke&) { return true; });
    // The active route's bore goes last so at a T its fill wins over a neighbour's cap.
    each(TubeLayer::Bore, [](int, const Stroke& s) { return !s.style.filled; });
    each(TubeLayer::Bore,
         [&](int ri, const Stroke& s) { return s.style.filled && ri != plan.active_route; });
    each(TubeLayer::Bore,
         [&](int ri, const Stroke& s) { return s.style.filled && ri == plan.active_route; });

    paint_bands(layer, plan, pal, /*on_box_edge=*/false);
}

void paint_box_bands(lv_layer_t* layer, const PathPlan& plan, const TubePalette& pal) {
    paint_bands(layer, plan, pal, /*on_box_edge=*/true);
}

} // namespace helix::ui::fpath
