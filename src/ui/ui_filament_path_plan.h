// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file ui_filament_path_plan.h
 * @brief Route plan for the LINEAR/HUB detail canvas: frame → plan → paint.
 *
 * The frame is the per-draw layout. The plan turns it into routes (one
 * contiguous centerline per lane, the trunk owned by exactly one route) with a
 * SpanStyle per segment, plus the sensor bands. paint_tubes then strokes every
 * route in layers (halo, walls, bores, bands), so tubes run unbroken under
 * every sensor and a junction never shows a neighbour's cap over the active
 * fill. Bands on a box edge wait for paint_box_bands, after the boxes, so they
 * clamp the tube where it enters. The active route's filled prefix is what the
 * animation pass replays.
 */

#include "ui_filament_path_internal.h"

namespace helix::ui::fpath {

// Everything the LINEAR/HUB plan needs for one draw: layout Ys, resolved
// colors, filament/error state and the HUB merge-fan plan.
struct LinearHubFrame {
    // Vertical layout (absolute display coords)
    int32_t entry_y = 0;
    int32_t prep_y = 0;
    int32_t hub_y = 0;
    int32_t hub_h = 0;
    int32_t output_y = 0;
    int32_t toolhead_y = 0;
    int32_t nozzle_y = 0;
    int32_t inlet_y = 0; // the toolhead glyph's filament inlet
    int32_t bypass_merge_y = 0;
    int32_t center_x = 0;
    int32_t output_x = 0; // LINEAR: under the active slot (possibly animating)

    int32_t buffer_y = 0;
    int32_t buf_fil_top = 0;
    bool has_buffer = false;

    lv_color_t idle_color, active_color, hub_bg, hub_border;
    lv_color_t error_color; // error token blended with the pulse phase

    int32_t sensor_r = 0;

    bool has_error = false;
    PathSegment error_seg = PathSegment::NONE;
    PathSegment fil_seg = PathSegment::NONE;

    // HUB merge fan: parallel diagonals per side, one 4-point polyline per lane.
    pg::MergeLaneOut hub_fan[FilamentPathData::MAX_SLOTS];
    int32_t hub_box_w = 0; // widened entry-spread width (HUB box drawn at this)
    // HUB with the bypass hidden: hub and buffer stacked upward from the
    // toolhead glyph instead of at their ratio positions.
    bool hub_stacked = false;
    // On-toolhead mode: the passthrough selector keeps the unit's position
    // while the hub box moves down to hug the toolhead.
    int32_t selector_y = 0;

    SlotRenderStates states;
};

/// @p glyph_top is the toolhead glyph's topmost drawn Y (toolhead_top_y()):
/// a HUB with the bypass hidden stacks its hub and buffer above it.
LinearHubFrame compute_linear_hub_frame(const FilamentPathData& data, const BaseGeometry& g,
                                        int32_t glyph_top);

// MIXED (HTLF) layout: some lanes run direct to their own nozzle, the rest fan
// into a shared hub feeding one nozzle.
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

    // Merge fan for the hub lanes: parallel diagonals per side spread across
    // distinct hub-top entries.
    pg::MergeLaneOut hub_fan[FilamentPathData::MAX_SLOTS];
    int slot_to_fan[FilamentPathData::MAX_SLOTS]; // slot index -> hub-lane order (-1 none)

    SlotRenderStates states;
};

MixedFrame compute_mixed_frame(const FilamentPathData& data, const BaseGeometry& g);

enum class TubeWall : uint8_t { Plain, Active, Error };

struct SpanStyle {
    TubeWall wall = TubeWall::Plain;
    lv_color_t bore;     // filament color, or the background when empty
    bool filled = false; // filament is in this span
    bool painted = true; // false inside an opaque box: recorded, never stroked
};
bool operator==(const SpanStyle& a, const SpanStyle& b);

struct Route {
    pg::FilamentPath path;
    SpanStyle style[pg::FilamentPath::MAX_SEGS];
    int dropped = 0; // segments that did not fit in MAX_SEGS
};

/// Append @p piece's segments with style @p s. Segments past MAX_SEGS are
/// dropped and counted in Route::dropped; returns false when any were.
bool route_append(Route& r, const pg::FilamentPath& piece, SpanStyle s);

enum class BandState : uint8_t { Empty, Loaded, Active, Error };

struct SensorBand {
    pg::PathPoint at;
    pg::PathPoint tangent; // unit direction of the tube under the band
    BandState state;
    lv_color_t fill;          // the lane's filament color (Loaded)
    bool on_box_edge = false; // straddles a hub/selector edge: painted over the box
};

inline constexpr int MAX_ROUTES = FilamentPathData::MAX_SLOTS + 2; // lanes + trunk + bypass
inline constexpr int MAX_BANDS = 2 * FilamentPathData::MAX_SLOTS + 4;

struct PathPlan {
    Route routes[MAX_ROUTES];
    int route_count = 0;
    int active_route = -1;
    int trunk_route = -1;  // idle trunk, when no lane owns it
    int bypass_route = -1; // bypass horizontal (and the trunk below it when active)
    SensorBand bands[MAX_BANDS];
    int band_count = 0;
    int trunk_band_count = 0; // of band_count: output, merge and toolhead bands
    int dropped = 0; // segments and bands that did not fit; the plan is incomplete when > 0
    bool buffer_has_filament = false;
    lv_color_t buffer_fill;
};

struct Stroke {
    int first = 0;
    int end = 0;
    SpanStyle style;
};

/// Maximal runs of adjacent painted segments with equal styles; an unpainted
/// segment ends a run and yields nothing. Returns the stroke count.
int coalesce(const Route& r, Stroke* out, int max_out);

/// segs[0..k], k = the last segment with filament, painted or not.
pg::FilamentPath filled_prefix(const Route& r);

SpanStyle span_style(PathSegment span, PathSegment reached, bool on_active_route,
                     PathSegment error_seg, lv_color_t filament, lv_color_t bg);
BandState band_state(PathSegment sensor, PathSegment reached, bool on_active_route,
                     PathSegment error_seg);

void plan_linear_hub(const LinearHubFrame& f, const FilamentPathData& data, const BaseGeometry& g,
                     PathPlan& out);
/// One route per tool: entry → sensor band → nozzle top; the mounted tool is active.
void plan_parallel(const FilamentPathData& data, const BaseGeometry& g, PathPlan& out);
/// Hub lanes: entry → sensor band → fan → hub top. One shared trunk: hub bottom
/// → nozzle top. Direct lanes: entry → sensor band → their own nozzle top.
void plan_mixed(const MixedFrame& f, const FilamentPathData& data, const BaseGeometry& g,
                PathPlan& out);

struct TubePalette {
    lv_color_t idle_wall, accent, error, bg;
    int32_t gauge;
};
/// The error token, blended toward its darker shade with the pulse phase.
lv_color_t pulsed_error_color(const FilamentPathData& data);
/// Theme walls, accent, pulsed error, background and gauge.
TubePalette tube_palette(const FilamentPathData& data);
/// Halo, walls, bores, then every band not on a box edge.
void paint_tubes(lv_layer_t* layer, const PathPlan& plan, const TubePalette& pal,
                 bool simple = reduced_effects());
/// The bands on a hub/selector edge; called after the boxes are drawn.
void paint_box_bands(lv_layer_t* layer, const PathPlan& plan, const TubePalette& pal);

// Clamp band: a short rounded bar across the tube.
inline constexpr int32_t BAND_EXTRA = 10;    // band length = gauge + BAND_EXTRA
inline constexpr int32_t BAND_THICKNESS = 4; // stroke width, round caps

/// Centerline of the band bar: at ± normal * ((gauge + BAND_EXTRA)/2 - BAND_THICKNESS/2).
void band_segment(const SensorBand& band, int32_t gauge, pg::PathPoint& p0, pg::PathPoint& p1);
void draw_sensor_band(lv_layer_t* layer, const SensorBand& band, int32_t gauge, lv_color_t color);

} // namespace helix::ui::fpath
