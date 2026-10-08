// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Topology renderers for the filament_path_canvas widget. Three layouts:
//
//   LINEAR/HUB (render_linear_hub) — the classic AMS detail view: entry lanes
//   → prep sensors → merge (fan into HUB, or butt into the LINEAR selector) →
//   hub/selector box → output sensor → buffer → bypass merge → toolhead
//   sensor → nozzle. Frame → route plan → layered tubes and sensor bands
//   (ui_filament_path_plan.cpp), then the boxes and glyphs on top.
//
//   PARALLEL (render_parallel) — tool changers: every slot is an independent
//   column (entry → sensor → own toolhead + badge).
//
//   MIXED (render_mixed) — HTLF-style: some lanes run direct to their own
//   nozzle, others fan into a shared hub feeding one nozzle.
//
// Each renderer derives a per-draw "frame" (layout Ys, resolved colors,
// per-slot states) once. LINEAR/HUB stores the active route's filled prefix in
// FilamentPathData::path_cache, and the hub/buffer/bypass boxes record their
// hit rects — see ui_filament_path_internal.h ("render → record").
//
// The DRAW_POST animation overlays (flow dots, heat glow, moving tip) live at
// the bottom of this file; they replay the recorded path each frame without
// re-running the heavyweight render.

#include "ui_filament_path_internal.h"
#include "ui_filament_path_plan.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace helix::ui::fpath {

// ============================================================================
// Shared per-draw derivations
// ============================================================================

BaseGeometry compute_base_geometry(lv_obj_t* obj, const FilamentPathData* data) {
    BaseGeometry g;
    lv_area_t obj_coords;
    lv_obj_get_coords(obj, &obj_coords);
    g.x_off = obj_coords.x1;
    g.y_off = obj_coords.y1;
    g.width = lv_area_get_width(&obj_coords);
    g.height = lv_area_get_height(&obj_coords);
    g.slot_count = data->slot_count;

    int count = LV_MIN(g.slot_count, FilamentPathData::MAX_SLOTS);
    for (int i = 0; i < count; i++) {
        g.slot_x[i] = g.x_off + get_slot_x(data, i, g.x_off);
    }

    // Center X: prefer midpoint of slot bounds so hub/selector/nozzle stay
    // aligned with the spool grid even when the grid is narrower than the
    // canvas (e.g. environment indicator present).
    if (g.slot_count >= 2) {
        g.center_x = (g.slot_x[0] + g.slot_x[g.slot_count - 1]) / 2;
    } else if (g.slot_count == 1) {
        g.center_x = g.slot_x[0];
    } else {
        g.center_x = g.x_off + g.width / 2;
    }
    return g;
}

SlotRenderStates compute_slot_render_states(const FilamentPathData* data) {
    SlotRenderStates states{};
    int count = LV_MIN(data->slot_count, FilamentPathData::MAX_SLOTS);
    for (int i = 0; i < count; i++) {
        SlotRenderState& s = states[i];

        // Per-slot installed filament (default).
        if (data->slot_filament_states[i].segment != PathSegment::NONE) {
            s.has_filament = true;
            s.color = lv_color_hex(data->slot_filament_states[i].color);
            s.segment = data->slot_filament_states[i].segment;
        }

        // Active slot overrides with current load/unload state when present.
        s.is_mounted = (i == data->active_slot);
        if (s.is_mounted && data->filament_segment > 0) {
            s.has_filament = true;
            s.color = lv_color_hex(data->filament_color);
            s.segment = static_cast<PathSegment>(data->filament_segment);
        }

        s.at_sensor = s.has_filament && (s.segment >= PathSegment::TOOLHEAD);
        s.at_nozzle = s.has_filament && (s.segment >= PathSegment::NOZZLE);
    }
    return states;
}

// Check if a segment should be drawn as "active" (filament present at or past it)
bool is_segment_active(PathSegment segment, PathSegment filament_segment) {
    return static_cast<int>(segment) <= static_cast<int>(filament_segment) &&
           filament_segment != PathSegment::NONE;
}

namespace {

// Detail-canvas tube style. Downstream runs carry only the routed filament, so
// they pass active=true and let has_filament decide. An errored run is filled
// with the error color and takes it for its walls too.
LaneStyle tube_style(const ThemeCache& theme, bool has_filament, bool active, lv_color_t fill,
                     bool is_error) {
    lv_color_t walls = is_error ? fill : theme.color_accent;
    return lane_style(has_filament, active, fill, theme.color_idle, walls, theme.color_bg,
                      theme.tube_gauge);
}

// ============================================================================
// PARALLEL topology (tool changers)
// ============================================================================
// Tool changers have independent toolheads — each slot is a complete tool with
// its own extruder. Unlike hub/linear topologies where filaments converge to a
// single toolhead, parallel topology shows separate per-slot paths.

// One independent tool column: entry line → sensor dot → line → toolhead glyph
// → tool badge.
void draw_parallel_slot(const RenderCtx& ctx, const SlotRenderStates& states, int i,
                        int32_t entry_y, int32_t sensor_y, int32_t toolhead_y) {
    const FilamentPathData* data = ctx.data;
    const ThemeCache& theme = data->theme;
    lv_color_t idle_color = theme.color_idle;
    int32_t sensor_r = theme.sensor_radius;

    int32_t slot_x = ctx.geo.slot_x[i];
    const SlotRenderState& s = states[i];
    lv_color_t tool_color = s.has_filament ? s.color : idle_color;

    int32_t tool_scale = LV_MAX(6, theme.extruder_scale * 2 / 3);
    int32_t nozzle_top = toolhead_y - tool_scale * 2; // Top of heater block

    // Entry → sensor line: colored if filament present, hollow if idle
    {
        LaneStyle st = tube_style(theme, s.has_filament, s.is_mounted, tool_color, false);
        draw_lane_vline(ctx.layer, slot_x, entry_y, sensor_y - sensor_r, st);
    }

    // Toolhead entry sensor dot
    lv_color_t sensor_color = s.at_sensor ? tool_color : idle_color;
    draw_sensor_dot(ctx.layer, slot_x, sensor_y, sensor_color, s.at_sensor, sensor_r);

    // Sensor → nozzle line: colored if filament reaches nozzle, hollow if idle
    {
        LaneStyle st = tube_style(theme, s.at_nozzle, s.is_mounted, tool_color, false);
        draw_lane_vline(ctx.layer, slot_x, sensor_y + sensor_r, nozzle_top, st);
    }

    // Nozzle color — only show filament color when actually at nozzle
    std::optional<lv_color_t> noz_color;
    if (s.at_nozzle) {
        noz_color = tool_color;
    }

    // Docked toolheads rendered at reduced opacity to visually distinguish from active
    lv_opa_t toolhead_opa = s.is_mounted ? LV_OPA_COVER : LV_OPA_40;

    // Flow particles for the active slot are painted separately in
    // draw_animation_parallel (DRAW_POST) so per-frame ticks don't bust the
    // overlay canvas cache.
    draw_toolhead(ctx.layer, slot_x, toolhead_y, noz_color, tool_scale, toolhead_opa);

    // Tool badge (E0/T0, …) below nozzle — matches system_path_canvas style
    if (theme.label_font) {
        char tool_label[16];
        int tool = (data->mapped_tool[i] >= 0) ? data->mapped_tool[i] : i;
        format_tool_badge_label(data, i, tool, tool_label, sizeof(tool_label));
        lv_color_t text = s.is_mounted ? theme.color_success : theme.color_text;
        draw_tool_badge(ctx, slot_x, toolhead_y + tool_scale * 4 + 6, tool_label, text,
                        toolhead_opa);
    }
}

// Static + state-tied content for PARALLEL — painted into the overlay canvas.
// Animation (flow dots) lives separately in draw_animation_parallel (DRAW_POST).
void render_parallel(lv_obj_t* obj, lv_layer_t* layer, FilamentPathData* data) {
    RenderCtx ctx{layer, data, compute_base_geometry(obj, data)};
    int32_t height = ctx.geo.height;
    int32_t y_off = ctx.geo.y_off;

    // Layout ratios for parallel topology (adjusted for per-slot toolheads).
    // SENSOR_Y/TOOLHEAD_Y are file-scope (PARALLEL_*_Y_RATIO) so the click
    // hit-test reads the identical values — no drift between draw and hit.
    constexpr float ENTRY_Y = -0.12f; // Top entry (connects to spool)

    int32_t entry_y = y_off + (int32_t)(height * ENTRY_Y);
    int32_t sensor_y = y_off + (int32_t)(height * PARALLEL_SENSOR_Y_RATIO);
    int32_t toolhead_y = y_off + (int32_t)(height * PARALLEL_TOOLHEAD_Y_RATIO);

    SlotRenderStates states = compute_slot_render_states(data);

    // Draw each tool as an independent column
    for (int i = 0; i < data->slot_count; i++) {
        draw_parallel_slot(ctx, states, i, entry_y, sensor_y, toolhead_y);
    }
}

// ============================================================================
// MIXED topology (HTLF: direct + hub lanes)
// ============================================================================
// Some lanes go directly to their own nozzle (like PARALLEL), while others
// converge through a hub box to a shared nozzle. Visual layout:
//   [spool0] [spool1] [spool2] [spool3]
//      |        |        |        |       entry lines
//      o        o        o        o       sensor dots
//      |        |         \      /        direct vs angled paths
//      |        |        [HUB]            hub box (hub lanes converge)
//      |        |          |              hub output line
//     (T0)    (T2)       (T1)             nozzles + tool labels

// Per-draw layout + merge-fan plan for MIXED.
struct MixedFrame {
    int32_t entry_y = 0;
    int32_t sensor_y = 0;
    int32_t hub_cy = 0;
    int32_t hub_h = 0;
    int32_t hub_bottom = 0;
    int32_t toolhead_y = 0;
    int32_t tool_scale = 0;

    int hub_count = 0;       // lanes routed through the hub
    int first_hub_lane = -1; // first hub-routed slot index
    int32_t hub_cx = 0;      // hub box center X (mean of hub lane Xs)
    int32_t hub_w = 0;

    // Per-hub-lane merge fan via the shared builder: parallel diagonals per
    // side spread across distinct hub-top entries (no coincident or pinching
    // runs). Built once; the path phase draws each lane's tube from its
    // precomputed waypoints.
    pg::MergeLaneOut hub_fan[FilamentPathData::MAX_SLOTS];
    int slot_to_fan[FilamentPathData::MAX_SLOTS]; // slot index -> hub-lane order (-1 none)

    SlotRenderStates states;
};

// Layout + hub-lane identification + merge-fan construction.
MixedFrame compute_mixed_frame(const RenderCtx& ctx) {
    const FilamentPathData* data = ctx.data;
    const BaseGeometry& g = ctx.geo;
    MixedFrame f;

    // Layout ratios — more vertical spread than parallel to fit hub + nozzles
    constexpr float ENTRY_Y = -0.12f;   // Top entry (connects to spool)
    constexpr float SENSOR_Y = 0.15f;   // Sensor dot position
    constexpr float HUB_Y = 0.32f;      // Hub box center Y
    constexpr float HUB_H = 0.08f;      // Hub box height ratio
    constexpr float TOOLHEAD_Y = 0.62f; // Nozzle/toolhead position

    f.entry_y = g.y_off + (int32_t)(g.height * ENTRY_Y);
    f.sensor_y = g.y_off + (int32_t)(g.height * SENSOR_Y);
    f.hub_h = LV_MAX(16, (int32_t)(g.height * HUB_H));
    f.toolhead_y = g.y_off + (int32_t)(g.height * TOOLHEAD_Y);
    f.tool_scale = LV_MAX(6, data->theme.extruder_scale * 2 / 3);
    if (data->hub_on_toolhead) {
        // Combiner mounted on the print head: the box hugs the toolhead with
        // only a stub of shared tube below it (a few percent of canvas), and
        // the merge fan runs the full height to reach it. The stub is measured
        // from the nozzle glyph's top so the two can never overlap.
        const int32_t nozzle_top = f.toolhead_y - f.tool_scale * 2;
        const int32_t stub = LV_MAX(10, (int32_t)(g.height * 0.03f));
        f.hub_bottom = nozzle_top - stub;
        f.hub_cy = f.hub_bottom - f.hub_h / 2;
    } else {
        f.hub_cy = g.y_off + (int32_t)(g.height * HUB_Y);
        f.hub_bottom = f.hub_cy + f.hub_h / 2;
    }

    f.states = compute_slot_render_states(data);

    // Identify hub lanes and compute hub center X
    int32_t hub_x_sum = 0;
    for (int i = 0; i < data->slot_count; i++) {
        if (data->slot_is_hub_routed[i]) {
            hub_x_sum += g.slot_x[i];
            f.hub_count++;
            if (f.first_hub_lane < 0)
                f.first_hub_lane = i;
        }
    }
    f.hub_cx = (f.hub_count > 0) ? (hub_x_sum / f.hub_count) : (g.x_off + 150);
    // Hub width: ~60% of full hub topology width, enough for the hub lanes
    f.hub_w = LV_MAX(40, data->theme.hub_width * 3 / 5);

    // Build the merge fan for the hub lanes.
    for (int i = 0; i < FilamentPathData::MAX_SLOTS; i++)
        f.slot_to_fan[i] = -1;
    {
        int32_t hub_top_e = f.hub_cy - f.hub_h / 2;
        int32_t sensor_r = data->theme.sensor_radius;
        pg::MergeLaneIn fan_in[FilamentPathData::MAX_SLOTS];
        int fan_n = 0;
        for (int i = 0; i < data->slot_count && i < FilamentPathData::MAX_SLOTS; i++) {
            if (!data->slot_is_hub_routed[i])
                continue;
            fan_in[fan_n] = {(float)g.slot_x[i], (float)(f.sensor_y + sensor_r)};
            f.slot_to_fan[i] = fan_n;
            fan_n++;
        }
        pg::build_merge_fan(fan_in, fan_n, (float)f.hub_cx, (float)hub_top_e, (float)f.hub_w,
                            /*entry_margin=*/8.0f, /*fillet_r=*/8.0f, /*max_slope=*/1.2f,
                            f.hub_fan);
    }
    return f;
}

// Entry lines and sensor dots for ALL lanes (direct and hub-routed alike).
void draw_mixed_entry_lanes(const RenderCtx& ctx, const MixedFrame& f) {
    const FilamentPathData* data = ctx.data;
    const ThemeCache& theme = data->theme;
    int32_t sensor_r = theme.sensor_radius;

    for (int i = 0; i < data->slot_count; i++) {
        int32_t slot_x = ctx.geo.slot_x[i];
        const SlotRenderState& s = f.states[i];
        lv_color_t tool_color = s.has_filament ? s.color : theme.color_idle;

        // Entry → sensor line
        {
            LaneStyle st = tube_style(theme, s.has_filament, s.is_mounted, tool_color, false);
            draw_lane_vline(ctx.layer, slot_x, f.entry_y, f.sensor_y - sensor_r, st);
        }

        // Sensor dot
        lv_color_t sensor_color = s.at_sensor ? tool_color : theme.color_idle;
        draw_sensor_dot(ctx.layer, slot_x, f.sensor_y, sensor_color, s.at_sensor, sensor_r);
    }
}

// Shared hub output: hub-bottom → nozzle line, the single shared toolhead, and
// its tool badge. Drawn once, right after the FIRST hub lane's tube (so later
// hub-lane tubes still paint on top in their original z-order).
void draw_mixed_shared_toolhead(const RenderCtx& ctx, const MixedFrame& f) {
    const FilamentPathData* data = ctx.data;
    const ThemeCache& theme = data->theme;

    // Check if any hub lane has filament at nozzle
    bool any_hub_at_nozzle = false;
    lv_color_t hub_nozzle_color = theme.color_idle;
    int hub_tool = (f.first_hub_lane >= 0 && data->mapped_tool[f.first_hub_lane] >= 0)
                       ? data->mapped_tool[f.first_hub_lane]
                       : (f.first_hub_lane >= 0 ? f.first_hub_lane : 0);
    // Lane whose extruder identity names this shared toolhead. Every hub-routed
    // lane feeds the same extruder, so any of them answers; the loaded one is
    // preferred only because the legacy T-label follows it.
    int hub_badge_lane = f.first_hub_lane;
    bool hub_mounted = false;

    for (int j = 0; j < data->slot_count; j++) {
        if (!data->slot_is_hub_routed[j])
            continue;
        const SlotRenderState& sj = f.states[j];
        if (sj.segment >= PathSegment::NOZZLE) {
            any_hub_at_nozzle = true;
            hub_nozzle_color = sj.color;
            hub_tool = (data->mapped_tool[j] >= 0) ? data->mapped_tool[j] : j;
            hub_badge_lane = j;
            hub_mounted = sj.is_mounted;
            break;
        }
    }

    int32_t nozzle_top = f.toolhead_y - f.tool_scale * 2;

    // Hub bottom → nozzle top line
    {
        LaneStyle st = tube_style(theme, any_hub_at_nozzle, hub_mounted, hub_nozzle_color, false);
        draw_lane_vline(ctx.layer, f.hub_cx, f.hub_bottom, nozzle_top, st);
    }

    // Shared hub nozzle — always "mounted" visually (it's a shared output)
    std::optional<lv_color_t> noz_color;
    if (any_hub_at_nozzle)
        noz_color = hub_nozzle_color;
    lv_opa_t hub_noz_opa = LV_OPA_COVER;
    draw_toolhead(ctx.layer, f.hub_cx, f.toolhead_y, noz_color, f.tool_scale, hub_noz_opa);

    // Tool label below shared hub nozzle
    if (theme.label_font) {
        char tool_label[16];
        format_tool_badge_label(data, hub_badge_lane, hub_tool, tool_label, sizeof(tool_label));
        draw_tool_badge(ctx, f.hub_cx, f.toolhead_y + f.tool_scale * 4 + 6, tool_label,
                        theme.color_text, hub_noz_opa);
    }
}

// Direct lane: straight vertical from sensor to its own nozzle + badge.
void draw_mixed_direct_lane(const RenderCtx& ctx, const MixedFrame& f, int i) {
    const FilamentPathData* data = ctx.data;
    const ThemeCache& theme = data->theme;
    const SlotRenderState& s = f.states[i];
    int32_t slot_x = ctx.geo.slot_x[i];
    lv_color_t tool_color = s.has_filament ? s.color : theme.color_idle;
    int32_t nozzle_top = f.toolhead_y - f.tool_scale * 2;

    {
        LaneStyle st =
            tube_style(theme, s.has_filament && s.at_nozzle, s.is_mounted, tool_color, false);
        draw_lane_vline(ctx.layer, slot_x, f.sensor_y + theme.sensor_radius, nozzle_top, st);
    }

    // Direct nozzle
    std::optional<lv_color_t> noz_color;
    if (s.at_nozzle) {
        noz_color = tool_color;
    }
    lv_opa_t toolhead_opa = s.is_mounted ? LV_OPA_COVER : LV_OPA_40;
    draw_toolhead(ctx.layer, slot_x, f.toolhead_y, noz_color, f.tool_scale, toolhead_opa);

    // Tool label below direct nozzle
    if (theme.label_font) {
        char tool_label[16];
        int tool = (data->mapped_tool[i] >= 0) ? data->mapped_tool[i] : i;
        format_tool_badge_label(data, i, tool, tool_label, sizeof(tool_label));
        lv_color_t text = s.is_mounted ? theme.color_success : theme.color_text;
        draw_tool_badge(ctx, slot_x, f.toolhead_y + f.tool_scale * 3 + 4, tool_label, text,
                        toolhead_opa);
    }
}

// Paths from sensor to nozzle (direct or hub-routed), preserving z-order:
// each lane in slot order; the shared hub toolhead immediately after the
// first hub lane's tube.
void draw_mixed_paths(const RenderCtx& ctx, const MixedFrame& f) {
    const FilamentPathData* data = ctx.data;
    const ThemeCache& theme = data->theme;
    bool hub_nozzle_drawn = false;

    for (int i = 0; i < data->slot_count; i++) {
        if (data->slot_is_hub_routed[i]) {
            // Hub-routed lane: parallel-diagonal merge run from the sensor down
            // to a distinct hub-top entry, drawn from this lane's precomputed
            // fan waypoints (separation by construction — see build_merge_fan).
            const SlotRenderState& s = f.states[i];
            lv_color_t tool_color = s.has_filament ? s.color : theme.color_idle;
            int fi = (i < FilamentPathData::MAX_SLOTS) ? f.slot_to_fan[i] : -1;
            if (fi >= 0) {
                LaneStyle st = tube_style(theme, s.has_filament, s.is_mounted, tool_color, false);
                pg::FilamentPath path;
                pg::route_polyline_filleted(path, f.hub_fan[fi].pts, 4, 8.0f);
                draw_lane(ctx.layer, path, st, nullptr);
            }

            // Hub output line + shared nozzle (draw only once)
            if (!hub_nozzle_drawn) {
                hub_nozzle_drawn = true;
                draw_mixed_shared_toolhead(ctx, f);
            }
        } else {
            draw_mixed_direct_lane(ctx, f, i);
        }
    }
}

void render_mixed(lv_obj_t* obj, lv_layer_t* layer, FilamentPathData* data) {
    RenderCtx ctx{layer, data, compute_base_geometry(obj, data)};
    MixedFrame f = compute_mixed_frame(ctx);

    // Phase 1: entry lines and sensor dots for ALL lanes
    draw_mixed_entry_lanes(ctx, f);

    // Phase 2: hub box (behind paths, so paths draw on top)
    if (f.hub_count > 0) {
        draw_hub_box(ctx, f.hub_cx, f.hub_cy, f.hub_w, f.hub_h, data->theme.color_hub_bg,
                     data->theme.color_hub_border, "HUB");
    }

    // Phase 3: paths from sensor to nozzle (direct or hub-routed)
    draw_mixed_paths(ctx, f);
}

// ============================================================================
// LINEAR / HUB topology
// ============================================================================

// Debug override: HELIX_FLOW_SEGMENT=PREP|LANE|HUB|OUTPUT|TOOLHEAD|NOZZLE
//                 HELIX_FLOW_DIR=LOAD|UNLOAD
// Forces filament to the specified segment with flow animation for isolated
// testing, and keeps a 30ms repaint timer alive while active.
void apply_debug_flow_override(lv_obj_t* obj, FilamentPathData* data) {
    static const char* dbg_seg_env = getenv("HELIX_FLOW_SEGMENT");
    if (!dbg_seg_env)
        return;

    // Force active slot 0 if none set (local override only for drawing)
    if (data->active_slot < 0)
        data->active_slot = 0;

    // Map name to PathSegment value
    static const struct {
        const char* name;
        int val;
    } seg_map[] = {
        {"PREP", (int)PathSegment::PREP},         {"LANE", (int)PathSegment::LANE},
        {"HUB", (int)PathSegment::HUB},           {"OUTPUT", (int)PathSegment::OUTPUT},
        {"TOOLHEAD", (int)PathSegment::TOOLHEAD}, {"NOZZLE", (int)PathSegment::NOZZLE},
    };
    for (auto& m : seg_map) {
        if (strcasecmp(dbg_seg_env, m.name) == 0) {
            data->filament_segment = m.val;
            break;
        }
    }

    // Use a persistent lv_timer to keep triggering redraws
    static lv_timer_t* dbg_timer = nullptr;
    if (!dbg_timer) {
        dbg_timer = lv_timer_create(
            [](lv_timer_t* t) {
                auto* o = static_cast<lv_obj_t*>(lv_timer_get_user_data(t));
                lv_obj_invalidate(o);
            },
            30, obj);
    }
}

// Whether filament has reached the hub (drives the box tint).
bool hub_has_filament(const FilamentPathData* data, const LinearHubFrame& f) {
    if (data->active_slot >= 0 && is_segment_active(PathSegment::HUB, f.fil_seg))
        return true;
    if (data->topology == static_cast<int>(PathTopology::LINEAR))
        return false;
    for (int i = 0; i < data->slot_count; i++) {
        if (f.states[i].segment >= PathSegment::HUB)
            return true;
    }
    return false;
}

// Hub box tint priority: error at hub > buffer fault > buffer warning >
// loaded-filament tint > plain theme colors.
void resolve_hub_tint(const RenderCtx& ctx, const LinearHubFrame& f, bool has_filament,
                      lv_color_t* bg_out, lv_color_t* border_out) {
    FilamentPathData* data = ctx.data;
    lv_color_t hub_bg_tinted = f.hub_bg;
    lv_color_t hub_border_final = f.hub_border;
    if (f.has_error && f.error_seg == PathSegment::HUB) {
        // Error at hub — red tint with pulsing error color
        hub_bg_tinted = ph_blend(f.hub_bg, f.error_color, 0.40f);
        hub_border_final = f.error_color;
    } else if (data->buffer_fault_state == 2) {
        // Fault detected — red tint
        hub_bg_tinted = ph_blend(f.hub_bg, data->theme.color_error, 0.50f);
        hub_border_final = data->theme.color_error;
    } else if (data->buffer_fault_state == 1) {
        // Approaching fault — yellow/warning tint
        lv_color_t warning = lv_color_hex(0xFFA500);
        hub_bg_tinted = ph_blend(f.hub_bg, warning, 0.40f);
        hub_border_final = warning;
    } else if (has_filament) {
        // Healthy — subtle filament color tint (use first loaded slot's color)
        lv_color_t tint_color = f.active_color;
        if (data->active_slot < 0) {
            // No active slot — find first slot loaded to hub for tint
            for (int i = 0; i < data->slot_count; i++) {
                if (f.states[i].segment >= PathSegment::HUB) {
                    tint_color = f.states[i].color;
                    break;
                }
            }
        }
        hub_bg_tinted = ph_blend(f.hub_bg, tint_color, 0.33f);
    }
    *bg_out = hub_bg_tinted;
    *border_out = hub_border_final;
}

// Hub/selector box: state-tinted fill, label, optional gear affordance and
// the recorded hub hit rect.
void draw_hub_section(const RenderCtx& ctx, const LinearHubFrame& f) {
    FilamentPathData* data = ctx.data;
    const BaseGeometry& g = ctx.geo;

    // Hub box - tint based on error state, buffer fault state, or filament color
    lv_color_t hub_bg_tinted, hub_border_final;
    resolve_hub_tint(ctx, f, hub_has_filament(data, f), &hub_bg_tinted, &hub_border_final);

    const char* hub_label = (data->topology == 0) ? "SELECTOR" : "HUB";

    // For LINEAR topology, hub box spans the full slot area width.
    // slot_x values are slot centers, so we add half a slot width on each
    // side to cover the full visual extent of the outermost slots.
    // For HUB topology, use the widened entry-spread width from the frame so
    // the merge tubes land cleanly on the box.
    int32_t hub_w = (data->topology == 1) ? f.hub_box_w : data->theme.hub_width;
    if (data->topology == 0 && data->slot_count > 1) {
        int32_t first_slot_x = g.slot_x[0];
        int32_t last_slot_x = g.slot_x[data->slot_count - 1];
        int32_t slot_pad = LV_MAX(data->slot_width, f.sensor_r * 4);
        hub_w = (last_slot_x - first_slot_x) + slot_pad;
    }

    lv_opa_t hub_opa = (data->topology == 0) ? LV_OPA_60 : LV_OPA_COVER;

    // On-toolhead mode draws the passthrough selector first, in its classic
    // spot under the lanes, full slot width like LINEAR's selector.
    int32_t selector_gear_overflow = 0;
    if (data->hub_on_toolhead) {
        int32_t sel_w = data->theme.hub_width;
        if (data->slot_count > 1) {
            int32_t first_slot_x = g.slot_x[0];
            int32_t last_slot_x = g.slot_x[data->slot_count - 1];
            int32_t slot_pad = LV_MAX(data->slot_width, f.sensor_r * 4);
            sel_w = (last_slot_x - first_slot_x) + slot_pad;
        }
        selector_gear_overflow =
            draw_hub_box(ctx, f.center_x, f.selector_y, sel_w, f.hub_h, hub_bg_tinted,
                         hub_border_final, "SELECTOR", LV_OPA_60,
                         /*interactive=*/data->hub_callback != nullptr);
        // The unit's own box is the management target; the hub on the head is
        // not a separate control.
        data->hits.hub = {f.center_x - sel_w / 2, f.selector_y - f.hub_h / 2,
                          f.center_x + sel_w / 2 + selector_gear_overflow,
                          f.selector_y + f.hub_h / 2};
        data->hits.hub_valid = true;
    }

    int32_t gear_overflow =
        draw_hub_box(ctx, f.center_x, f.hub_y, hub_w, f.hub_h, hub_bg_tinted, hub_border_final,
                     hub_label, hub_opa, /*interactive=*/data->hub_callback != nullptr);

    // Single source of truth for the selector/hub hit-test: record the exact
    // box we just drew (absolute display coords). When the gear is drawn
    // OUTSIDE the box's right edge (label too wide to fit it inside), extend
    // the hit rect rightward so the gear stays tappable. On-toolhead mode
    // recorded the SELECTOR box above; the head hub is not a control.
    if (!data->hub_on_toolhead) {
        data->hits.hub = {f.center_x - hub_w / 2, f.hub_y - f.hub_h / 2,
                          f.center_x + hub_w / 2 + gear_overflow, f.hub_y + f.hub_h / 2};
        data->hits.hub_valid = true;
    }
}

// Buffer (TurtleNeck / eSpooler) "BUF" box over the trunk, plus its recorded
// hit rect. Mirrors draw_buffer_coil()'s internal clamping so the click
// handler never re-derives the geometry.
void draw_buffer_section(const RenderCtx& ctx, const LinearHubFrame& f, const PathPlan& plan) {
    FilamentPathData* data = ctx.data;
    draw_buffer_coil(ctx, f.center_x, f.buffer_y, f.hub_h, plan.buffer_has_filament,
                     plan.buffer_fill);

    int32_t buf_hit_w = LV_MAX(36, data->theme.hub_width * 4 / 5);
    int32_t buf_hit_h = LV_MAX(16, f.hub_h);
    data->hits.buffer = {f.center_x - buf_hit_w / 2, f.buffer_y - buf_hit_h / 2,
                         f.center_x + buf_hit_w / 2, f.buffer_y + buf_hit_h / 2};
    data->hits.buffer_valid = true;
}

// Bypass spool hit region (absolute coords). The spool is a sibling widget,
// so the rect is anchored to the bypass merge geometry: the click handler's
// full-extent test is abs(dx) < sensor_r*3, abs(dy) < sensor_r*4, read with
// margin 0 via hub_box_hit.
void record_bypass_hit(const RenderCtx& ctx, const LinearHubFrame& f) {
    const BaseGeometry& g = ctx.geo;
    int32_t bypass_x = g.x_off + (int32_t)(g.width * BYPASS_X_RATIO);
    ctx.data->hits.bypass = {bypass_x - f.sensor_r * 3, f.bypass_merge_y - f.sensor_r * 4,
                             bypass_x + f.sensor_r * 3, f.bypass_merge_y + f.sensor_r * 4};
    ctx.data->hits.bypass_valid = true;
}

// Extruder glyph, in the color of whatever filament reached the nozzle.
void draw_nozzle_glyph(const RenderCtx& ctx, const LinearHubFrame& f) {
    FilamentPathData* data = ctx.data;
    std::optional<lv_color_t> noz_color;
    if (data->bypass_active) {
        noz_color = lv_color_hex(data->bypass_color);
    } else if (data->active_slot >= 0 && is_segment_active(PathSegment::NOZZLE, f.fil_seg)) {
        noz_color =
            (f.has_error && f.error_seg == PathSegment::NOZZLE) ? f.error_color : f.active_color;
    }
    draw_toolhead(ctx.layer, f.center_x, f.nozzle_y, noz_color, data->theme.extruder_scale);
}

// LINEAR/HUB renderer: frame → plan → tubes → boxes and glyphs. Hit-rect
// valid flags are reset here and re-recorded where the boxes are drawn; the
// active route's filled prefix becomes the animation pass's path.
void render_linear_hub(lv_obj_t* obj, lv_layer_t* layer, FilamentPathData* data) {
    data->hits.hub_valid = false;
    data->hits.buffer_valid = false;
    data->hits.bypass_valid = false;

    apply_debug_flow_override(obj, data);
    RenderCtx ctx{layer, data, compute_base_geometry(obj, data)};

    // LINEAR: the output exit snaps under the active slot unless it is sliding there.
    const int active = data->active_slot;
    if (data->topology == 0 && active >= 0 && active < ctx.geo.slot_count &&
        !data->anim.output_x_active) {
        data->anim.output_x_current = ctx.geo.slot_x[active];
        data->anim.output_x_target = ctx.geo.slot_x[active];
    }

    const LinearHubFrame f = compute_linear_hub_frame(*data, ctx.geo);
    // About 14 KB: kept off the stack, which on the ESP32 is the LVGL task's.
    // Rendering is single-threaded and not re-entrant.
    static PathPlan plan;
    plan_linear_hub(f, *data, ctx.geo, plan);

    const ThemeCache& theme = data->theme;
    paint_tubes(
        layer, plan,
        {theme.color_idle, theme.color_accent, f.error_color, theme.color_bg, theme.tube_gauge});

    draw_hub_section(ctx, f);
    if (data->hub_only)
        return;
    if (f.has_buffer)
        draw_buffer_section(ctx, f, plan);
    if (data->show_bypass)
        record_bypass_hit(ctx, f);
    draw_nozzle_glyph(ctx, f);

    data->path_cache.path = (plan.active_route >= 0) ? filled_prefix(plan.routes[plan.active_route])
                                                     : pg::FilamentPath{};
    data->path_cache.center_x = f.center_x;
    data->path_cache.nozzle_y = f.nozzle_y;
    data->path_cache.sensor_r = f.sensor_r;
    data->path_cache.valid = true;
}

// ============================================================================
// DRAW_POST animation overlays
// ============================================================================

// Animation overlay for LINEAR/HUB — flow particles, heat glow, segment
// transition filament tip. Reads the active path cached by the state-tied
// renderer (populated in render_linear_hub).
void draw_animation_linear_hub(lv_layer_t* layer, FilamentPathData* data) {
    if (!data->path_cache.valid)
        return;

    lv_color_t active_color = lv_color_hex(data->filament_color);
    int32_t sensor_r = data->path_cache.sensor_r;
    int32_t center_x = data->path_cache.center_x;
    int32_t nozzle_y = data->path_cache.nozzle_y;
    auto& path = data->path_cache.path;

    // Flow particles along the active filament path.
    if (data->anim.flow_active && data->active_slot >= 0 && !data->hub_only) {
        bool reverse = (data->anim.direction == AnimDirection::UNLOADING);
        draw_flow_dots_path(layer, path, active_color, data->anim.flow_offset, reverse);
    }

    // Heat glow halo around the nozzle tip.
    if (data->anim.heat_active) {
        int32_t tip_y = toolhead_tip_y(nozzle_y, data->theme.extruder_scale);
        draw_heat_glow(layer, center_x, tip_y, sensor_r, data->anim.heat_pulse_opa);
    }

    // Segment transition tip — interpolated along the path.
    if (data->anim.segment_active && data->active_slot >= 0 && !data->hub_only && path.count > 0) {
        PathSegment prev_seg = static_cast<PathSegment>(data->anim.prev_segment);
        PathSegment fil_seg = static_cast<PathSegment>(data->filament_segment);
        float progress_factor = data->anim.progress / 100.0f;
        const float NUM_INTERVALS = static_cast<float>(static_cast<int>(PathSegment::NOZZLE) -
                                                       static_cast<int>(PathSegment::SPOOL));
        float base = static_cast<float>(static_cast<int>(prev_seg) - 1);
        float target = static_cast<float>(static_cast<int>(fil_seg) - 1);
        float tip_fraction = (base + (target - base) * progress_factor) / NUM_INTERVALS;
        tip_fraction = LV_CLAMP(tip_fraction, 0.0f, 1.0f);
        float tip_distance = tip_fraction * pg::path_length(path);
        pg::PathPoint tip = pg::path_point_at(path, tip_distance);
        int32_t tip_x = (int32_t)lroundf(tip.x);
        int32_t tip_y = (int32_t)lroundf(tip.y);
        bool in_nozzle_body =
            (prev_seg == PathSegment::TOOLHEAD && fil_seg == PathSegment::NOZZLE) ||
            (prev_seg == PathSegment::NOZZLE && fil_seg == PathSegment::TOOLHEAD);
        if (!in_nozzle_body) {
            draw_filament_tip(layer, tip_x, tip_y, active_color, sensor_r);
        }
    }
}

// Animation overlay for PARALLEL — flow particles on the mounted slot's entry
// run. Painted via DRAW_POST on top of the overlay canvas.
void draw_animation_parallel(lv_layer_t* layer, const BaseGeometry& g,
                             const SlotRenderStates& states, const FilamentPathData* data) {
    if (!data->anim.flow_active)
        return;
    int32_t entry_y = g.y_off + static_cast<int32_t>(g.height * -0.12f);
    int32_t sensor_y = g.y_off + static_cast<int32_t>(g.height * PARALLEL_SENSOR_Y_RATIO);
    int32_t sensor_r = data->theme.sensor_radius;
    bool reverse = (data->anim.direction == AnimDirection::UNLOADING);
    int count = LV_MIN(data->slot_count, FilamentPathData::MAX_SLOTS);
    for (int i = 0; i < count; i++) {
        const SlotRenderState& s = states[i];
        if (!s.is_mounted || !s.has_filament)
            continue;
        draw_flow_dots_line(layer, g.slot_x[i], entry_y, g.slot_x[i], sensor_y - sensor_r, s.color,
                            data->anim.flow_offset, reverse);
    }
}

} // namespace

// ============================================================================
// Entry points (called by the layers module and the widget's DRAW_POST event)
// ============================================================================

void render_overlay_content(lv_obj_t* obj, lv_layer_t* layer, FilamentPathData* data) {
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    data->hits.origin = {coords.x1, coords.y1};
    if (data->topology == static_cast<int>(PathTopology::MIXED)) {
        render_mixed(obj, layer, data);
    } else if (data->topology == static_cast<int>(PathTopology::PARALLEL)) {
        render_parallel(obj, layer, data);
    } else {
        render_linear_hub(obj, layer, data);
    }
}

void render_animation_overlay(lv_obj_t* obj, lv_layer_t* layer, FilamentPathData* data) {
    int topo = data->topology;
    if (topo == static_cast<int>(PathTopology::PARALLEL)) {
        BaseGeometry g = compute_base_geometry(obj, data);
        SlotRenderStates states = compute_slot_render_states(data);
        draw_animation_parallel(layer, g, states, data);
    } else if (topo == static_cast<int>(PathTopology::LINEAR) ||
               topo == static_cast<int>(PathTopology::HUB)) {
        draw_animation_linear_hub(layer, data);
    }
    // MIXED has no per-frame animation — nothing to paint here.
}

} // namespace helix::ui::fpath
