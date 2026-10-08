// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file filament_tube_stroker.h
 * @brief Shared concentric tube stroker for AMS filament-path rendering.
 *
 * A "tube" is rendered as a set of CONCENTRIC stroke passes (no perpendicular
 * offsets) over a pathgeo::FilamentPath. Straight runs are drawn with
 * lv_draw_line (float coords — we build with LV_USE_FLOAT) and corners as a fan
 * of short straight chords sampled from the arc's exact float parametrization
 * (also lv_draw_line, NOT lv_draw_arc, whose integer center / outer radius would
 * round the band ~0.5px out of alignment with the adjoining lines). Round caps
 * live only on the very first seg-start and last seg-end of the path; interior
 * joints (including chord-to-chord) use butt caps (segments join tangentially,
 * so butt joints are seamless and translucent passes don't double-blend).
 *
 * This layer is shared by both AMS path canvases:
 *   - ui_filament_path_canvas (single-unit detail panel)
 *   - ui_system_path_canvas   (multi-unit overview)
 * so both produce identical clean quarter-arc fillet curves. Callers supply
 * lane width / color choices via LaneStyle (the overview's lanes are
 * intentionally thinner/dimmer than the detail panel).
 */

#include "filament_path_geometry.h"
#include "lvgl/lvgl.h"

namespace helix {
namespace ui {

namespace pg = pathgeo;

// Centerline fillet radius for orthogonal lane routing (route_orthogonal clamps
// internally, so tight layouts degrade gracefully to jogs / straight runs).
inline constexpr float FILLET_RADIUS = 12.0f;

// Halo around the active route: two opaque bands pre-blended from the background
// toward the wall color, so overlapping chords and bends never double-blend.
inline constexpr int32_t HALO_WIDTH_EXTRA = 6; // outer band: gauge + 6
inline constexpr int32_t HALO_INNER_EXTRA = 3; // inner band: gauge + 3
inline constexpr float HALO_OUTER_MIX = 0.25f; // wall share, outer band
inline constexpr float HALO_INNER_MIX = 0.55f; // wall share, inner band

/// Detect low-performance platforms (K1/K2/MIPS or constrained-memory devices).
/// When true, the stroker drops the halo; walls and bore still draw.
bool reduced_effects();

/// Color helpers (delegate to ams_draw::* — kept here so callers and the
/// stroker share one definition).
lv_color_t tube_darken(lv_color_t c, uint8_t amt);
lv_color_t tube_lighten(lv_color_t c, uint8_t amt);
lv_color_t tube_blend(lv_color_t c1, lv_color_t c2, float factor);

// One concentric stroke pass.
struct TubePass {
    lv_color_t color;
    int32_t width;
    lv_opa_t opa;
};

// A PTFE sleeve: 1 px walls around a bore that shows the filament when loaded.
struct LaneStyle {
    lv_color_t wall; // idle wall token, theme accent on the active route, error on error
    lv_color_t bore; // filament color when loaded, background when empty
    lv_color_t bg;   // background the halo bands pre-blend against
    int32_t width;   // outer gauge, walls included
    bool halo;       // active route only; halo color is the wall color
};

// Paint layers of one tube, bottom to top.
enum class TubeLayer : uint8_t { Halo, Wall, Bore };

/// Stroke a path with N concentric passes.
void stroke_path(lv_layer_t* layer, const pg::FilamentPath& path, const TubePass* passes,
                 int n_passes);

/// Build one layer's concentric passes for a LaneStyle. Returns the number of
/// passes written into @p out (at most 2). @p simple drops the halo.
int build_passes(const LaneStyle& style, TubeLayer layer, TubePass* out,
                 bool simple = reduced_effects());

/// Build a LaneStyle from slot state in ONE place. Only a loaded lane on the
/// active route gets accent walls and the halo.
LaneStyle lane_style(bool has_filament, bool active, lv_color_t fill, lv_color_t idle_wall,
                     lv_color_t accent, lv_color_t bg, int32_t gauge);

/// Draw a path with a style, painting Halo, Wall, Bore in order; optionally record (append) the
/// path's segments into
/// @p record so a flow-dot / tip animation walks exactly what was drawn.
void draw_lane(lv_layer_t* layer, const pg::FilamentPath& path, const LaneStyle& style,
               pg::FilamentPath* record = nullptr);

/// Convenience: vertical tube run from (x,y0) to (x,y1). Only records forward
/// (downward) verticals into @p record (preserves the legacy guard).
void draw_lane_vline(lv_layer_t* layer, int32_t x, int32_t y0, int32_t y1, const LaneStyle& style,
                     pg::FilamentPath* record = nullptr);

/// Convenience: orthogonal route (vertical -> fillet -> horizontal -> fillet ->
/// vertical) from (x0,y0) down to (x1,y1).
void draw_lane_route(lv_layer_t* layer, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                     float fillet_r, const LaneStyle& style, pg::FilamentPath* record = nullptr);

/// Convenience: horizontal tube run from (x0,y) to (x1,y).
void draw_lane_hline(lv_layer_t* layer, int32_t x0, int32_t x1, int32_t y, const LaneStyle& style,
                     pg::FilamentPath* record = nullptr);

// One lane in a hub merge fan: its source column / diagonal-start height, draw
// style, and an optional active-path record sink. Used by draw_merge_fan.
struct MergeFanLane {
    int32_t slot_x;
    int32_t start_y;
    LaneStyle style;
    pg::FilamentPath* record = nullptr; // append the drawn path here (active lane)
};

// Shared hub-merge renderer: parallel diagonals per side, separation by
// construction (see pathgeo::build_merge_fan). Computes widened hub-top entries
// + one common slope per side so no two lanes ever overlap or pinch, then routes
// and draws each lane. Consumed by BOTH AMS path canvases (detail HUB renderer,
// detail mixed-topology, overview multi-tool routes, overview single-tool hub
// convergence). @p hub_top is the verticals' final destination; @p fillet_r is
// the corner-fillet radius (8 on the detail panel, 9 on the overview).
//
// If @p entry_x_out is non-null it receives each lane's computed hub-top entry x
// (size >= n) so the caller can place sensor dots exactly where the tubes land.
void draw_merge_fan(lv_layer_t* layer, const MergeFanLane* lanes, int n, int32_t hub_cx,
                    int32_t hub_top, int32_t hub_w, float fillet_r, int32_t* entry_x_out = nullptr);

} // namespace ui
} // namespace helix
