# AMS filament path: route plan, layered paint, PTFE-sleeve look, then #1753

Base: `main` @ 47c571171. Decisions are the maintainer's (restructure first; paint order;
look "B: PTFE sleeve + tight glow"; clamp-band sensors; bypass joins after the buffer;
slot highlight; re-apply #1753 with credit). This plan states interfaces, rules, tests and
gates. Header sketches below compile (see "Header check").

## What exists

| Piece | Today |
|---|---|
| `src/ui/ui_filament_path_topology.cpp#render_linear_hub` | LINEAR + HUB (+ `hub_on_toolhead`, `hub_only`). Phases `draw_entry_lanes` → `draw_bypass_section` → `draw_hub_section` → `draw_output_section` (+ `draw_buffer_element`) → `draw_toolhead_section` → `draw_nozzle_section`. Each section is its own capped stroke between sensor edges (`±sensor_r`), active/error/idle recomputed per section, and the active lane appends to `LinearHubFrame::active_path`, copied to `path_cache` in `draw_nozzle_section`. |
| `…#render_parallel`, `…#render_mixed` | Same shape: per-lane `draw_lane_vline` / fan polylines stopping at `draw_sensor_dot`. No `path_cache`. |
| `…#draw_animation_linear_hub` | Only `path_cache` consumer: flow dots (`draw_flow_dots_path`) and the segment tip (`pg::path_point_at` at a fraction of `pg::path_length`). The recorded path is **not** contiguous today: it jumps `2*sensor_r` at prep, toolhead and bypass merge. |
| `include/filament_tube_stroker.h` | `build_passes` → glow/body/core (solid) or outline/wall/bore (hollow); `draw_lane*` paint all passes per lane, so a later lane's glow/outline overpaints an earlier lane's body at junctions. |
| `src/ui/ui_system_path_canvas.cpp` (overview) | `sp_lane_style` (always `solid`, `bg = color`), `draw_lane`, `draw_lane_vline`, `draw_lane_route`, three `draw_merge_fan` calls (`#draw_routes`, `#draw_mini_hubs`, `#draw_unit_columns`). Own static `draw_sensor_dot`. No animation, no hit rects. |
| Hit rects | `data->hits.hub` (`#draw_hub_section`, widened by `draw_hub_box`'s gear overflow), `hits.buffer` (`#draw_buffer_element`), `hits.bypass` (`#draw_bypass_section`). Read by `src/ui/ui_filament_path_canvas.cpp` click dispatch. |
| Bug | `#draw_buffer_element`: `buffer_has_filament = … || data->bypass_active` and colors the buffer run + BUF box with the bypass color. The bypass merges at `BYPASS_MERGE_Y_RATIO` (0.58), below the buffer (0.46). |
| Tests | No test reads `path_cache` or `hits` today. `[canvas][hit_test]` (pure `hub_box_hit`), `[filament-path][geometry]`, `[filament-path][canvas][mixed]` (pixel count > 200), `[filament_path][canvas_buffer]`, `[filament_path][layers]`, `[filament_path][render_once]`, `[ams][buffer][path]`. |
| Goldens | None cover AMS. `ams` is deliberately outside `_SUBSET` in `tests/ui/test_screens.py` (bypass-spool render count is nondeterministic), so **no golden is re-accepted by any phase**; visual checks are screenshots. |
| ESP32 | `firmware/helixscreen-esp32/components/helixapp/app_srcs.txt` lists every file here; a new `.cpp` must be added there (`scripts/check_esp32_app_srcs.py`). |

## Design

### Model

A render is **frame → plan → paint tubes → boxes and glyphs**.

- **Frame** (exists): layout Ys/Xs, colors, slot states. `compute_linear_hub_frame` becomes
  `LinearHubFrame compute_linear_hub_frame(const FilamentPathData&, const BaseGeometry&)`:
  the `data->anim.output_x_*` write moves to `render_linear_hub`, so the frame is pure.
  `LinearHubFrame` and `MixedFrame` move into the new header.
- **Plan** (new, pure): routes (centerline + one `SpanStyle` per segment) and sensor bands.
- **Paint**: `paint_tubes` runs halo → walls → bores → bands over the whole plan; then the
  renderer draws boxes and glyphs exactly as today (`draw_hub_box`, `draw_buffer_coil`,
  `draw_toolhead`, `draw_tool_badge`), still recording `hits.*` at those draw sites.
- `render_linear_hub` copies `filled_prefix(plan.routes[plan.active_route])` into
  `path_cache.path`. `PathPlan` is ~14 KB, so the renderer uses one function-local
  `static PathPlan` (LVGL is single-threaded; render is not re-entrant) instead of the
  stack, which matters on the ESP32 LVGL task.

### Stroker (`include/filament_tube_stroker.h`, changed declarations)

```cpp
inline constexpr int32_t HALO_WIDTH_EXTRA = 6;   // outer band: gauge + 6
inline constexpr int32_t HALO_INNER_EXTRA = 3;   // inner band: gauge + 3
inline constexpr float HALO_OUTER_MIX = 0.25f;   // accent share, outer band
inline constexpr float HALO_INNER_MIX = 0.55f;   // accent share, inner band

struct LaneStyle {
    lv_color_t wall; // idle wall token, theme accent on the active route, error on error
    lv_color_t bore; // filament color when loaded, background when empty
    lv_color_t bg;   // background the halo bands pre-blend against
    int32_t width;   // outer gauge, walls included
    bool halo;       // active route only; halo color is the wall color
};
enum class TubeLayer : uint8_t { Halo, Wall, Bore };

int build_passes(const LaneStyle& style, TubeLayer layer, TubePass* out,
                 bool simple = reduced_effects());
LaneStyle lane_style(bool has_filament, bool active, lv_color_t fill, lv_color_t idle_wall,
                     lv_color_t accent, lv_color_t bg, int32_t gauge);
void draw_lane(...);  // signature unchanged; paints Halo, Wall, Bore in order
```

Removed: `get_glow_color`, `GLOW_OPA`, `GLOW_WIDTH_EXTRA`, the `solid` flag and the core
highlight. `stroke_path`, `draw_lane_vline/route/hline`, `MergeFanLane`, `draw_merge_fan`
keep their signatures.

Rules:
- Halo: two **opaque** bands, `tube_blend(bg, wall, HALO_OUTER_MIX)` at `gauge+6` and
  `tube_blend(bg, wall, HALO_INNER_MIX)` at `gauge+3`. Opaque, so `stroke_path` round-joins
  every chord and nothing double-blends at bends. `simple` → 0 passes. Constants are the
  starting point, tuned on screenshots.
- Wall: `{wall, gauge, COVER}`. Bore: `{bore, gauge-2, COVER}` (1 px walls).
- `lane_style`: `active && has_filament` → accent walls + halo; `has_filament` alone →
  idle walls, filament bore; else idle walls, `bg` bore. No neutral/black/white special case.
- Gauge: new `ThemeCache::tube_gauge = line_width_active + 2` (5 px at 800x480, the outer
  width the idle tube reads at today). New `ThemeCache::color_accent =
  theme_manager_get_color("primary")` (the slot highlight's token).

Who uses what:
- **Detail LINEAR/HUB, PARALLEL, MIXED**: adopt plan/coalesce/layered paint (Phases 2-3).
  All three draw sensors, and decision 2 (tubes never stop at a sensor) plus clamp bands
  need a continuous tube under every sensor; one painter serves all three.
- **Overview** (`ui_system_path_canvas.cpp`): new stroker look only. Its tubes are per-unit
  stems with per-lane widths (`line_idle` vs `line_active`), no animation and no
  `path_cache`; restructuring it buys nothing this work needs. `sp_lane_style(color, width,
  active)` → `{wall: active ? accent : color, bore: color, bg: card_bg, width, halo: active}`;
  `load_theme_colors` caches `card_bg` and `primary`. Its dots stay.

### Plan (`src/ui/ui_filament_path_plan.h` + `.cpp`, new)

```cpp
enum class TubeWall : uint8_t { Plain, Active, Error };
struct SpanStyle {
    TubeWall wall = TubeWall::Plain;
    lv_color_t bore;        // filament color, or the background when empty
    bool filled = false;    // filament is in this span
    bool painted = true;    // false inside an opaque box: recorded, never stroked
};
bool operator==(const SpanStyle&, const SpanStyle&);

struct Route { pg::FilamentPath path; SpanStyle style[pg::FilamentPath::MAX_SEGS]; };
void route_append(Route& r, const pg::FilamentPath& piece, SpanStyle s);

enum class BandState : uint8_t { Empty, Loaded, Active, Error };
struct SensorBand { pg::PathPoint at; pg::PathPoint tangent; BandState state; lv_color_t fill; };

inline constexpr int MAX_ROUTES = FilamentPathData::MAX_SLOTS + 2;  // lanes + trunk + bypass
inline constexpr int MAX_BANDS = 2 * FilamentPathData::MAX_SLOTS + 4;
struct PathPlan {
    Route routes[MAX_ROUTES]; int route_count = 0;
    int active_route = -1;
    SensorBand bands[MAX_BANDS]; int band_count = 0;
    bool buffer_has_filament = false; lv_color_t buffer_fill;
};

struct Stroke { int first = 0; int end = 0; SpanStyle style; };
int coalesce(const Route& r, Stroke* out, int max_out);
pg::FilamentPath filled_prefix(const Route& r);

SpanStyle span_style(PathSegment span, PathSegment reached, bool on_active_route,
                     PathSegment error_seg, lv_color_t filament, lv_color_t bg);
BandState band_state(PathSegment sensor, PathSegment reached, bool on_active_route,
                     PathSegment error_seg);

void plan_linear_hub(const LinearHubFrame& f, const FilamentPathData& data,
                     const BaseGeometry& g, PathPlan& out);
void plan_parallel(const FilamentPathData& data, const BaseGeometry& g, PathPlan& out);
void plan_mixed(const MixedFrame& f, const FilamentPathData& data, const BaseGeometry& g,
                PathPlan& out);

struct TubePalette { lv_color_t idle_wall, accent, error, bg; int32_t gauge; };
void paint_tubes(lv_layer_t* layer, const PathPlan& plan, const TubePalette& pal,
                 bool simple = reduced_effects());

inline constexpr int32_t BAND_EXTRA = 10;
inline constexpr int32_t BAND_THICKNESS = 4;
void band_segment(const SensorBand& band, int32_t gauge, pg::PathPoint& p0, pg::PathPoint& p1);
void draw_sensor_band(lv_layer_t* layer, const SensorBand& band, int32_t gauge, lv_color_t color);
```

**Span tags.** Each run is tagged with the `PathSegment` at which it fills (today's
`is_segment_active` thresholds):

| Run | Tag | Ends at band |
|---|---|---|
| spool entry → prep sensor | `SPOOL` | prep (`PREP`), if `slot_has_prep_sensor` |
| prep → hub-top entry (HUB fan) / selector top (LINEAR, on-toolhead) / merge (other) | `LANE` | hub entry (`HUB`), HUB only |
| inside the hub or selector box | `HUB` | output (`OUTPUT`), at hub bottom, not on-toolhead |
| hub bottom → bypass merge (or toolhead when bypass hidden); buffer lies inside this run | `OUTPUT` | bypass merge, if `show_bypass` |
| bypass merge → toolhead sensor | `TOOLHEAD` | toolhead (`TOOLHEAD`), if `show_bypass` |
| toolhead sensor → nozzle inlet (`nozzle_y - extruder_scale*2`) | `NOZZLE` | – |

The band set and visibility conditions equal today's `draw_sensor_dot` calls; only the glyph
changes. Runs are split at every band and box edge, so styles change only at segment
boundaries.

**span_style**: not `is_segment_active(span, reached)` → `{Plain, bg, filled=false}`, except
`on_active_route && span == error_seg` → `{Error, bg}`. Filled: `on_active_route && span ==
error_seg` → `Error`; `on_active_route` → `Active`; else `Plain`; bore = filament.

**band_state**: `on_active_route && sensor == error_seg` → `Error`; `on_active_route &&
is_segment_active(sensor, reached)` → `Active`; `is_segment_active(sensor, reached)` off the
active route → `Loaded`; else `Empty`. Band colors: Empty = idle wall, Loaded = that lane's
filament color (`SensorBand::fill`, so staged lanes stay visible), Active = accent,
Error = `f.error_color` (pulse-blended).

**Route ownership** (LINEAR/HUB). One route per slot from `(slot_x, entry_y)`. The trunk
(hub bottom → nozzle) belongs to exactly one route:
- `bypass_active`: the bypass route owns everything from the merge down: horizontal from
  the spool edge to `(center_x, bypass_merge_y)`, then down to the nozzle, all `Active` with
  `bypass_color`. The AMS trunk ends at the merge and is styled from AMS state only, so the
  buffer stays empty unless AMS filament is in `OUTPUT`: `buffer_has_filament =
  active_slot >= 0 && !bypass_active && is_segment_active(OUTPUT, fil_seg)` (decision 5).
- else `active_slot >= 0`: the active slot's route continues through the hub interior
  (`painted=false` under an opaque HUB box, `painted=true` through the LINEAR selector,
  which paints over it at `LV_OPA_60`, so the tube shows at about 40%: accepted, no
  special paint order) and the trunk, and becomes `plan.active_route`. Inactive bypass: a
  separate `Plain` horizontal route ending at `center_x`.
- else: a separate idle trunk route.
Other slots' routes end at their hub-top entry (HUB) or prep/selector top (LINEAR).
`hub_on_toolhead`: lane runs prep → selector top (`LANE`), selector interior unpainted,
selector bottom → fan → hub top (`LANE`), as today. `hub_only`: routes end at the hub.

**Segment budget**: longest route is on-toolhead (≈12 segments) under `MAX_SEGS = 16`;
`route_append` drops overflow like `add_line` does. A test pins the HUB worst case.

**coalesce**: adjacent painted segments with `==` styles form one stroke; an unpainted
segment ends a run and yields nothing. `stroke_path` round-joins opaque passes at every
joint, so a coalesced run has caps only at its two ends.

**filled_prefix**: `path.segs[0..k]`, `k` = last segment with `filled`, painted or not.

**paint_tubes** order:
1. Halo: per route, each maximal run of `Active` segments as one path (`TubeLayer::Halo`).
2. Walls: every stroke of every route.
3. Bores: empty strokes, then filled strokes off the active route, then the active route.
   The active route's bore is painted last so at a T (bypass, hub landings) its fill wins
   over a neighbor's cap.
4. Bands (`draw_sensor_band`), in plan order.
`simple` drops step 1 only.

**Bands**: `band_segment` returns `at ± n * ((gauge + BAND_EXTRA)/2 - BAND_THICKNESS/2)`,
`n` = unit normal of `tangent`; `draw_sensor_band` strokes it at `BAND_THICKNESS` with round
caps. Tangent comes from the route segment the band sits on (`pg::path_point_at(…,
&tangent)` or the line direction).

## Phases

Each phase lands on its own, gated by `make t F='<tags>'` while developing and one
`make full-test-run` at the end of the batch. Visual check after every pixel-changing phase:
`HELIX_HEADLESS=1 SDL_VIDEODRIVER=dummy HELIX_MOCK_AMS=<mode> scripts/screenshot.sh
helix-screen <name> ams -s 800x480` (and `-s small`), dark and `--light`, for: default
(Happy Hare LINEAR), `afc` (HUB), `afc` + `HELIX_MOCK_BUFFER_STATE=fault`, `ifs-module`
(hub on toolhead; confirm its trait in the log), `htlf` (MIXED), `toolchanger` (PARALLEL),
`multi` (overview), plus `HELIX_MOCK_AMS_STATE=bypass|error|loading` on `afc`; `openams`
(HUB, bypass hidden) once Phase 0 lands. Goldens: none cover these screens (see above).

### Phase 0: #1753 backend + mock (no canvas pixels)
- Cherry-pick 43cb3bccc (`AmsBackendOpenAms::infer_error_segment` guard + two `[openams]`
  cases) cleanly; credit is the original author.
- Port f365fc543 (`HELIX_MOCK_AMS=openams`): it conflicts with main in
  `docs/devel/MOCK_ENVIRONMENT_VARIABLES.md`, `include/moonraker_client_mock.h`,
  `src/api/moonraker_client_mock.cpp`; resolve by hand, keep authorship.
- Gate: `[openams]`, `[ams]`. Pixels: only the false OpenAMS nozzle error goes away.

### Phase 1: stroker look (pixels change on every AMS canvas)
- New `LaneStyle`/`TubeLayer`/`build_passes`/`lane_style` as above; `draw_lane*` paint
  Halo → Wall → Bore per lane. Detail renderers pass `tube_gauge` and `color_accent`;
  `active` = the mounted slot's filled runs (matches today's glow placement). Overview via
  `sp_lane_style` as above.
- Tests: new `tests/unit/test_filament_tube_stroker.cpp`, `[filament-path][stroker]`, all
  with `simple` passed explicitly; gauge 5, bg `0x000000`, wall `0x2196F3`, bore `0xFF0000`:
  - Halo, halo=true, simple=false → 2 passes, widths 11 and 8, both `LV_OPA_COVER`,
    colors `tube_blend(bg, wall, 0.25f)` and `tube_blend(bg, wall, 0.55f)`.
  - Halo, simple=true → 0; Halo, halo=false → 0.
  - Wall → 1 pass `{wall, 5, COVER}`; Bore → 1 pass `{bore, 3, COVER}`; gauge 3 → bore 1.
  - `lane_style(false,false,…)` → wall idle, bore bg, no halo; `(true,false,…)` → wall
    idle, bore fill; `(true,true,…)` → wall accent, bore fill, halo; `(false,true,…)` →
    same as `(false,false,…)`.
- Gate: `[filament-path]`, `[filament_path]`, `[ams]`.

### Phase 2: LINEAR/HUB onto the plan (pixels change: continuous tubes, bands, bypass fix)
- New `src/ui/ui_filament_path_plan.{h,cpp}` (+ ESP32 `app_srcs.txt`). `render_linear_hub`
  = frame → `plan_linear_hub` → `paint_tubes` → selector/hub boxes (+ `hits.hub`), buffer
  box (`draw_buffer_coil(…, plan.buffer_has_filament, plan.buffer_fill)` + `hits.buffer`),
  `hits.bypass`, toolhead glyph, `path_cache`. Every `draw_lane_*` and `draw_sensor_dot`
  call in the LINEAR/HUB path is deleted, as are `LaneState`, `derive_lane_state` and
  `draw_merge_to_hub_and_check_filament`'s line drawing (its hub-tint answer stays).
- Write the hit-rect characterization test **first, against main**, and keep it green.
- Tests, `tests/unit/test_filament_path_plan.cpp`, `[filament-path][plan]`. Frame fixture
  without LVGL: `BaseGeometry{x_off 0, y_off 0, width 400, height 400, slot_count 4,
  slot_x {50,150,250,350}, center_x 200}`, theme `line_width_active 3, tube_gauge 5,
  sensor_radius 4, hub_width 60, extruder_scale 10`, all `slot_has_prep_sensor`. Frame
  values: entry −48, prep 40, hub 120 (h 40, top 100), output 140, buffer 184, merge 232,
  toolhead 272, nozzle 328, inlet 308.
  - `coalesce`: styles `[A,A,B,A]` → 3 strokes `[0,2) A`, `[2,3) B`, `[3,4) A`;
    `[A, A(unpainted), A]` → `[0,1)`, `[2,3)`; 7 equal → 1 stroke `[0,7)`; empty → 0.
  - `span_style`: (LANE, HUB, on, NONE) → Active, filled, bore=filament;
    (OUTPUT, HUB, on, NONE) → Plain, empty, bore=bg; (LANE, LANE, off, NONE) → Plain,
    filled; (OUTPUT, NOZZLE, on, OUTPUT) → Error, filled; (OUTPUT, HUB, on, OUTPUT) →
    Error, empty; (OUTPUT, NOZZLE, off, OUTPUT) → Plain, filled.
  - `band_state`: (HUB, LANE, on) → Empty; (HUB, HUB, on) → Active; (PREP, LANE, off)
    → Loaded; (PREP, NONE, off) → Empty; (TOOLHEAD, HUB, on, error TOOLHEAD) → Error.
  - `band_segment`: at (200,140), tangent (0,1), gauge 5 → (194.5,140)–(205.5,140);
    tangent (1,0) → (200,134.5)–(200,145.5).
  - HUB, bypass shown, no buffer, slot 1 active at NOZZLE: active route starts at
    (150,−48), ends at (200,308), every `segs[i].p0 == segs[i-1].p1` (±0.01); it has
    segment boundaries at y = 40, 140, 232, 272; `coalesce` of it yields one painted
    `Active` stroke above the hub and one below (the unpainted interior splits them);
    `band_count == 11`; slot-1 prep/hub, output, merge, toolhead bands Active, the other
    six Empty. Fails on main's recorded path (gaps at prep, merge, toolhead).
  - Same, reached `HUB`: `filled_prefix` ends at (200,140); trunk segments Plain, empty.
  - Bypass bug: buffer present, bypass shown, `bypass_active`, `active_slot −1`:
    `buffer_has_filament == false`; every trunk segment with y < 232 is Plain, empty;
    the bypass route's segments are Active with bore `bypass_color` from x = spool edge to
    the nozzle inlet; `active_route == −1`.
  - Non-active slot 3 loaded to `LANE`: its route is Plain, filled to the hub top, its
    prep band `Loaded` with `fill` = slot 3's color, its hub band Empty.
  - Fill accuracy (accepted): active slot at `PREP` → entry run Active, fan run Plain and
    empty, hub band Empty; error at `PREP` with the slot at `LANE` → prep band Error, entry
    and fan runs Active (no Error run); error at `LANE` → only the fan run is Error.
  - LINEAR, active slot 2 at `OUTPUT`: the selector passage segment is `painted=true`.
  - On-toolhead, 4 slots, active at NOZZLE: route segment count ≤ 16 and contiguous.
  - Error at OUTPUT, loaded to NOZZLE, bypass hidden, buffer: the OUTPUT run from hub
    bottom to the toolhead is one `Error` stroke (no split at the buffer box edges).
- Widget-level `[filament-path][plan][hits]` (LVGLTestFixture, 400x400 canvas, written
  before the change): HUB with buffer + bypass: `hits.hub/buffer/bypass` valid and equal to
  the formulas in `#draw_hub_section`, `#draw_buffer_element`, `#draw_bypass_section`;
  `hub_only` → buffer and bypass invalid; LINEAR → hub rect spans the slot row.
- Gate: `[filament-path]`, `[filament_path]`, `[canvas]`, `[ams][buffer][path]`,
  `[ui_integration][ams]`.

### Phase 3: PARALLEL and MIXED onto the plan (pixels change: continuous lanes, bands)
- `plan_parallel` (one route per slot: entry → sensor `TOOLHEAD` band → nozzle top;
  mounted slot is the active route) and `plan_mixed` (hub lanes: entry → sensor → fan →
  hub top; one shared trunk route hub bottom → nozzle top; direct lanes entry → own nozzle
  top). Glyphs, badges and the MIXED hub box are drawn after `paint_tubes`, unchanged.
  `draw_sensor_dot` in `ui_filament_path_glyphs.cpp` then has no callers: delete it.
- Tests (`[filament-path][plan]`): PARALLEL 4 slots, none loaded → each route coalesces to
  **one** stroke (no break at the sensor), 4 bands at `PARALLEL_SENSOR_Y_RATIO`; slot 2
  mounted at NOZZLE → its route one Active stroke, its band Active; an unmounted slot loaded to its sensor has a
  Loaded band in its own color. MIXED (`htlf`
  layout: slots 0,1 direct, 2,3 hub): 3 routes reach a nozzle top, hub-lane routes end at
  the hub top, bands 4. Existing `[filament-path][canvas][mixed]` stay green.
- Gate: `[filament-path]`, `[filament_path]`.

### Phase 4: current-slot highlight (`src/ui/ui_ams_slot.cpp`)
- Extract `void ams_slot_apply_highlight(lv_obj_t* target, bool active, bool simple)`
  (declared in `include/ui_ams_slot.h`, `helix::ui::detail`), called with
  `reduced_effects()`. Active + capable: border 3 px primary, shadow 24 px, `LV_OPA_70`,
  spread 2 (from #1753). Active + simple: border 3 px primary, **no shadow**, outline 2 px
  primary at pad 2. Inactive and `ui_ams_slot_clear_highlight`: clear border, shadow and
  outline. File is an `lv_xml_register_widget` file, so `check_imperative_ui.py` exempts it.
- Tests in `tests/unit/test_ui_ams_slot.cpp`, `[ams_slot][highlight]`: simple=true →
  `shadow_width == 0`, `outline_width == 2`; simple=false → `shadow_width == 24`,
  `shadow_opa == LV_OPA_70`; inactive → border 0, shadow 0, outline 0.
- Gate: `[ams_slot]`.

### Phase 5: re-apply the rest of #1753 on the new structure (pixels change, HUB)
Commits carry `Co-authored-by: JR Lomas <lomas.jr@gmail.com>`.

Ported:
- `pathgeo::merge_fan_width()` verbatim: parallel diagonals, hub widened until the lines
  clear, self-adjusting to the lane count. The wide hub with the bypass shown is accepted.
  The only change at the caller (`build_linear_hub_merge_fan`) is the separation input,
  which comes from the new tube instead of the PR's `GLOW_WIDTH_EXTRA = 14`:
  `separation = max(tube_gauge + HALO_WIDTH_EXTRA + 2, 2*sensor_r + 2)`, i.e. outer tube
  (`line_width_active + 2`) plus the halo (+6) plus 2 px between halos: 13 px at 800x480 and
  at `micro` (both `tube_gauge` 5), 17 px at `xxlarge` (gauge 9). `min_width`/`max_width`,
  `TARGET_ENTRY_SPACING = 22`, `ENTRY_MARGIN = 8`, fillet 8 and slope 1.2 stay as the PR has them.
- Its `[filament-path][geometry][fan-clearance]` cases, **extended** rather than duplicated:
  the existing sampled-path test (`route_polyline_filleted` → 500 `path_point_at` samples
  per lane, nearest distance to the previous lane ≥ `separation − 0.1`) gains a second
  table built from the measured detail canvas (`ctl geom path_canvas`, `afc` mock):
  470x294 at 800x480 and 285x138 at `micro` (480x272, the smallest breakpoint). For each
  canvas and `count ∈ {2, 4, 8}`: slots evenly spaced across the canvas width,
  `start_y = 0.10*H + sensor_r`, `tube_end = 0.25*H - sensor_r`, `min_width` from
  `TARGET_ENTRY_SPACING`, `max_width = slot_span + 2*ENTRY_MARGIN`, `separation = 13`.
  Assert: clearance ≥ 12.9 along the sampled routes; `width < max_width` (the fit succeeded
  without hitting the clamp); hub-top entry step ≥ `tube_gauge + BAND_EXTRA + 2` = 17, so
  neighboring hub-entry bands never touch. The fan zone at `micro` is only about 20 px tall,
  so run the 8-lane `micro` case before porting: if it can only meet clearance at the
  clamp, that's a finding to report, not a test to loosen.
- The plan-level HUB test (`[filament-path][plan]`) gains `count ∈ {2, 4, 8}` at both
  canvas sizes: the hub box (`hits.hub`) stays inside the canvas width and every lane route
  ends inside it.
- The hub-top lowering ("borrow unused output-run height") in `build_linear_hub_merge_fan`.
- Hub + buffer stacking above the toolhead when HUB, bypass hidden, not on-toolhead, not
  `hub_only` (`hub_stacked` in the frame), and `toolhead_top_y()` in
  `ui_filament_path_glyphs.cpp`. The frame reads it through a `glyph_top` argument so the
  frame stays pure: `compute_linear_hub_frame(const FilamentPathData&, const BaseGeometry&,
  int32_t glyph_top)`.
- Hub gear color/placement in `draw_hub_box` (cfe6fae55, applies cleanly).
- `[filament-path][hub-stack]`: the stacking, centering (3 toolhead styles), on-toolhead
  and bypass-shown cases verbatim. Its two route cases are rewritten for the route model:
  "loaded to the nozzle with a buffer" → route contiguous, passes through hub bottom
  (`hits.hub.y2`) and ends at the inlet; "output run reaches the nozzle line unbroken" →
  with error at OUTPUT the OUTPUT run is one Error stroke spanning `hits.buffer` and ends
  at the toolhead boundary.

Dropped, superseded by Phases 1-2:
- `get_glow_color(color, bg)`, `needs_contrast_edge`, `contrast_tint`, the preblended
  `GLOW_OPA=150` / `GLOW_WIDTH_EXTRA=14` bands (including `GLOW_WIDTH_EXTRA` as the fan
  separation input), the contrast rim pass, `build_passes`'
  `!contrast_edge` core rule, and their `test_filament_tube_stroker.cpp` cases: decision 3
  puts contrast in the walls, with no neutral-filament special case. (Its `simple` parameter
  idea is kept, in Phase 1.)
- `continuous_output`, `single_output_stroke`, `nozzle_line_start_y`, the `draw_tube=false`
  parameters, the combined drop+bend stroke in `draw_entry_lanes`, `paint_buffer_box`, and
  the hub-interior endpoint tweaks: the route model strokes every equal run once by
  construction.
- `tests/unit/test_openams_path_preview.cpp` (`[.openams-preview]`) and the
  `XMLTestFixture(bool dark)` overload it needed: the openams mock + `screenshot.sh`
  `--light` gives the same picture without a hidden test.
- Gate: `[filament-path]`, `[openams]`; screenshot `openams` at 960x480 and 800x480, both
  themes, against the PR's before/after.

### Phase 6: dry-box unit: box, spools on the floor, glass lid (`src/ui/ui_ams_detail.cpp`; pixels change)

Independent of Phases 0-5. Reference math: `docs/devel/plans/2026-10-08-ams-path-redesign/unit_render.py` (approved in
direction); every number below is taken from it at 800x480 (spool size 61, front face
FL 10, FR 392, FT 101, FB 126).

What exists:

| Piece | Today |
|---|---|
| Tray | `ams_detail_update_tray` sizes `slot_tray` (`tray_height = grid_h / 4`, min 20) and fills the static `s_tray_box`; `depth = tray_height * DEPTH_PCT(40)/100`, `dx = depth`, `dy = depth * DY_PCT(45)/100`: an elevated oblique (back face +dx sideways, −dy up). `compute_face_coords` → `TrayFaceCoords`; `tray_back_draw_cb` (`DRAW_MAIN` on `slot_grid`, behind spools: back wall quad + dim edges); `tray_front_draw_cb` (`DRAW_POST` on `slot_tray`, over spools: left and right side quads, front rect, edges). Skipped when `!backend->has_physical_tray()`. |
| Spools | `src/ui/ui_spool_canvas.cpp`: local `ELLIPSE_RATIO = 0.45f` (flange `rx = ry * 0.45`): a level view, depth maps to 0.45 sideways and nothing vertical. |
| Readout | `ams_environment_indicator name="env_indicator"`, the right-hand flex child of `ui_xml/components/ams_unit_detail.xml`, beside `slot_container`; shown by `ams_detail_pre_show_env_indicator` from `has_environment_sensors()`, and by `ams_env_ind_detail_visible` = `unit.environment.has_value() || dryer.supported` (`src/printer/ams_state.cpp`, the per-unit env loop). |
| Labels | `material_label` at the top of each `ams_slot_view.xml`; for 5+ slots reparented to `labels_layer` (80 px tall) by `ams_detail_update_labels`. |
| Capability | `AmsUnit::environment` (`std::optional<EnvironmentData>`, per-unit temp/humidity); `SlotInfo::environment` per lane; `AmsBackend::get_environment_zones(unit)` groups both with dryers. |

Projection (one camera for spools, box and lid):
- `proj(x, y, z) = (x + K*z, y - RISE*z/DZ)`; x along the row, y screen-down at the front
  plane, z depth 0 (front wall) .. DZ (back wall).
- `K = DEPTH_SKEW = 0.45`, moved with `SPOOL_FLANGE_RADIUS = 0.42` into
  `include/ams_tray_projection.h`; `ui_spool_canvas.cpp` drops its local `ELLIPSE_RATIO`
  and `FLANGE_RADIUS` and uses them. The flange ellipses keep their pure 0.45 (no re-tilt).
- `DZ = 2 * SPOOL_FLANGE_RADIUS * spool_size * 1.1` (a spool diameter plus 10%),
  `RISE = round(DEPTH_RISE * DZ)` with `DEPTH_RISE = 0.09` (5 px at 800x480), `S = K*DZ`.
- `EB`, the back wall's extra height over the front wall, is `space_md` (10 px at 800x480),
  so it scales with the breakpoint and is not a raw pixel literal.
- `DEPTH_PCT`, `DY_PCT`, `MIN_DY_PX` and the elevated oblique go away.

Box (every unit with a physical tray, lid or not):
- Corners: front `fl_t (FL,FT)`, `fr_t (FR,FT)`, `fl_b (FL,FB)`, `fr_b (FR,FB)`; back
  `bl_t = proj(FL, FT-EB, DZ)`, `br_t = proj(FR, FT-EB, DZ)`, `bl_b = proj(FL, FB, DZ)`,
  `br_b = proj(FR, FB, DZ)`.
- Draw order: inside faces first, in `tray_back_draw_cb` (behind spools): back wall
  `[bl_t br_t br_b bl_b]`, floor `[fl_b fr_b br_b bl_b]`, left wall `[fl_t bl_t bl_b fl_b]`,
  back-top edge. Then the spools. Then in `tray_front_draw_cb` (over spools): the
  semi-transparent front wall `[fl_t fr_t fr_b fl_b]` (today's tray opacity), the opaque
  right side face `[fr_t br_t br_b fr_b]`, and the edges `fl_t-fr_t, fl_b-fr_b, fl_t-fl_b,
  fr_t-fr_b, fr_t-br_t, fr_b-br_b, br_t-br_b, fl_t-bl_t, fl_b-bl_b`.

Spools stand on the floor (layout change: today they sit above the tray):
- Front-plane `cy = FB - flange_ry - 2`, drawn at mid-depth: screen center
  `proj(slot_x, cy, DZ/2)`. The front wall covers their lower part, as on Bambu's panel.
  `ams_detail_update_tray` positions `slot_grid` so each spool canvas's center lands there:
  the row shifts right by `S/2` and down until the flange bottom is 2 px above `FB` (front
  plane). Badges and tool labels follow the spool center as they do today.

Lid (`lid_mode` != `None`):
- Half-elliptical cross-section on the sloped chord from the front-wall top to the
  back-wall top. `LID_H = FT - EB/2 - cy + flange_ry + 1`, so the crest at mid-depth sits
  1 px above the spool tops: tight, like Bambu.
- Cap profile at a row end `x_end`, θ from π to 0: `z = DZ/2*(1+cos θ)`,
  `h = EB*z/DZ + LID_H*sin θ`, screen `proj(x_end, FT-h, z)`. Caps at `x_end = FL` and `FR`;
  the silhouette is the convex hull of both caps.
- Back layer: the tinted interior shell (silhouette), then the back wall repainted over
  it, since the wall stands in front of the lid's lower interior.
- Front layer, after the front and side faces (look approved by the maintainer): clear
  glass over the silhouette (dark 6%, light 5%), the right cap denser (14% / 10%), the
  sheen below, then the silhouette edge (1.2 px, 60% / 55%) and cap rims (right at 0.7×,
  left at 0.3× the edge opacity).
- Sheen: ONE diffuse reflection, no crisp specular line. White, a 7 px band along the
  lid's upper front at profile angle θ = 112°, swept from the left cap to the right cap
  (`cap_point(…, FL, 112°)` to `cap_point(…, FR, 112°)`, a horizontal line since both caps
  share the profile), Gaussian blur σ 5 px, opacity 26% dark / 60% light. Along x it
  fades linearly in over 70 px starting 20 px right of the left point, and out over the
  110 px ending 30 px left of the right point (the reference's `fade`; these offsets are
  what `docs/devel/plans/2026-10-08-ams-path-redesign/unit_render.py`'s `streak` does). LVGL drawing: one `lv_draw_rect` per pixel row
  within ±12 px of the band center (≤ 25 draws), white, `bg_grad` horizontal with 4 opa
  stops (0, row opa, row opa, 0) at `x0, full0, full1, x1`; `LV_GRADIENT_MAX_STOPS` is 8
  and `LV_USE_DRAW_SW_COMPLEX_GRADIENTS` is on. No pre-rendered image or buffer to manage.
  Row math is the pure `sheen_rows`.
- `reduced_effects()`: outlines only (box edges, silhouette, caps); no shell, glass fill,
  sheen or edge blur.
- The glass is drawn by `slot_tray`'s `DRAW_POST`, which sits under `badge_layer`, so
  slot badges stay untinted (the reference paints the glass over them; this difference is
  intended).

Labels and readout:
- Material labels: centered at `proj(slot_x, 0, DZ/2).x`, baseline 10 px (`space_md`) above
  `unit_top_y`: the silhouette top with a lid, the back-wall top `FT - EB - RISE` without.
  For 5+ slots the same y applies on `labels_layer`.
- Readout (`env_indicator`): left edge at `br_t.x + space_md`, top at `unit_top_y - 4`,
  beside the drum to the right of the back-right corner. The slot row's available width
  shrinks so `br_t.x + space_md + readout width` fits the detail width.
- `slot_tray` grows to the full `slot_container` height so neither layer is clipped to
  the old tray strip; the face math anchors on `FB`.

Colors come from tokens (`tray_bg` plus new wall/floor/side/back/glass/glass-edge
consts in `ams_unit_detail.xml`, dark and light, values from the reference's palette).

Interfaces (`include/ams_tray_projection.h`, `src/ui/ams_tray_projection.cpp`, + ESP32
`app_srcs.txt`):

```cpp
namespace helix::ui::tray {
inline constexpr float DEPTH_SKEW = 0.45f, SPOOL_FLANGE_RADIUS = 0.42f,
                       BOX_DEPTH_MARGIN = 0.10f, DEPTH_RISE = 0.09f,
                       SPOOL_FLOOR_GAP = 2.0f, LID_GAP = 1.0f;
struct PointF { float x = 0, y = 0; };
struct TrayBox { float fl, fr, ft, fb, depth, rise, back_extra; };   // DZ, RISE, EB
float box_depth(float spool_size);
float box_rise(float depth);
PointF proj(const TrayBox& b, float x, float y, float z);
struct TrayFaces { PointF back_wall[4], floor[4], left_wall[4], front[4], right_side[4]; };
TrayFaces tray_faces(const TrayBox& b);
float spool_front_cy(const TrayBox& b, float flange_ry);
PointF spool_center(const TrayBox& b, float slot_x, float flange_ry);
float lid_height(const TrayBox& b, float flange_ry);
PointF cap_point(const TrayBox& b, float lid_h, float x_end, float theta);
int cap_polyline(const TrayBox& b, float lid_h, float x_end, PointF* out, int n);
float unit_top_y(const TrayBox& b, float lid_h, bool has_lid);
enum class LidMode : uint8_t { None, Unit, PerLane };
LidMode lid_mode(const AmsUnit& unit, bool has_physical_tray, bool dryer_supported);
// PerLane: one lid per slot over [slot_x - half, slot_x + half]
float lane_lid_half_width(float slot_spacing, const TrayBox& b);   // spacing/2 - K*DZ/4 - 1
}
```

`lid_mode` (maintainer's ruling: any climate data gets glass): no physical tray → `None`;
`unit.environment` or `dryer_supported` → `Unit` (one lid over the row); otherwise any slot
with `environment` (per-lane sensors, e.g. EMU) → `PerLane`; else `None`. `PerLane` draws
the same lid once per slot over `[slot_x - h, slot_x + h]`, `h = lane_lid_half_width` (each lid's
right end cap tucks halfway behind the next lid; lids are drawn left to right so the nearer one
covers it), and shows
each lane's humidity above its material label instead of the unit readout. Reference:
`unit_render.py` mode 2.

Tests, `tests/unit/test_ams_tray_projection.cpp`, `[ams][tray]` (pure, no LVGL). Box
`{FL 10, FR 392, FT 101, FB 126, DZ box_depth(61), RISE box_rise(DZ), EB 10}`, flange_ry
`0.42*61 = 25.62`:
- `box_depth(61) == 56.364` (±0.001); `box_rise(56.364) == 5`; `S = K*DZ ≈ 25.364`.
- `proj(b, 0, 0, 0) == (0, 0)`; `proj(b, 0, 0, DZ) == (25.364, -5)`.
- `tray_faces`: `bl_t (35.364, 86)`, `br_t (417.364, 86)`, `bl_b (35.364, 121)`,
  `br_b (417.364, 121)`; right side `[(392,101) (417.364,86) (417.364,121) (392,126)]`: left
  edge 25 tall, right edge 35; floor's back edge 5 px above its front edge.
- `spool_front_cy == 98.38`; `spool_center(b, 58, 25.62) == (70.682, 95.88)`; the spool's
  bottom (`95.88 + 25.62 = 121.5`) lies below `FT`, so the front wall covers it.
- `lid_height == 24.24`; `cap_point(…, 392, π) == (392, 101)`, `θ = 0 → (417.364, 86)`
  (= `br_t`), `θ = π/2 → (404.682, 69.26)`, exactly 1 px above the spool top at mid-depth
  (`95.88 - 25.62 = 70.26`).
- `unit_top_y(b, 24.24, true) ≈ 68.13` (hull top, sampled; above the θ=π/2 crest because
  the chord slopes); labels at `≈ 58.13`. `unit_top_y(b, 0, false) == 86`.
- `sheen_rows(b, 24.24, 0.26, …)`: band center y = 73.835 (`cap_point` at 112° is
  (17.931, 73.835) left and (399.931, 73.835) right); center row opa 34, rows at ±3 / ±6 /
  ±10 / ±12 have 29 / 19 / 6 / 3; `x0 = 37.931`, `full0 = 107.931`, `full1 = 259.931`,
  `x1 = 369.931`, `peak = 1`. Light (0.60): center 79, ±6 → 43. Row count ≤ 25, none under
  2. A short box (FR = 150) whose ramps overlap: `full0 == full1` at the crossing and
  `peak < 1`; FR so short that `x1 <= x0` → 0 rows.
- `cap_polyline(…, 17)`: first point `fr_t`, last `br_t`, x non-decreasing within
  `[392, 417.364]`, every point on or above the chord from `fr_t` to `br_t`.
- Readout x for this box is `br_t.x + 10 = 427.364` (checked in the widget case).
- `lid_mode`: environment + tray → Unit; environment, no tray → None; dryer only + tray →
  Unit; per-slot environment only + tray → PerLane; no climate data → None.
- `lane_lid_half_width(97, b)` == 97/2 - 25.364/4 - 1 = 41.159; a lid's right cap
  (`[x+h, x+h+S]`) overlaps the next lid's left end by `S/2` at most.
- Widget-level `[ams][tray][ui_integration]` (extends the fixture in
  `test_ams_detail_tray_draw_cb.cpp`, which stays green): for a lid unit at 800x480 the
  spool canvas centers are within 1 px of `spool_center`, every `material_label` sits
  `space_md` above `unit_top_y`, and `env_indicator.x1 == br_t.x + space_md` (±1); for a
  no-lid unit the labels sit above the back-wall top instead.
- Gate: `[ams][tray]`, `[ams][ui][user_flags]`, `[ams_slot]`.

Screenshot checklist (`HELIX_HEADLESS=1 SDL_VIDEODRIVER=dummy`, `scripts/screenshot.sh
helix-screen <name> ams`, at `-s 800x480` and `-s micro`, dark and `--light`), compared
with `docs/devel/plans/2026-10-08-ams-path-redesign/unit_render.py` output (lid and `LID=0`):
1. Default mock (passive env, 4 slots): box, spools on the floor behind the front wall,
   lid crest 1 px over the spools, labels 10 px above the lid, readout right of `br_t`.
2. `HELIX_MOCK_AMS_ENV=off`: same box and spools, no lid, labels above the back wall.
3. `HELIX_MOCK_DRYER=1 HELIX_MOCK_AMS_ENV=dryer`: lid + readout with drying state.
4. `HELIX_MOCK_AMS_ENV=slot` (per-lane sensors only): no lid.
5. `HELIX_AMS_GATES=8`: overlapping spools, labels on `labels_layer` above the lid.
6. `HELIX_MOCK_AMS=cfs`: real-backend per-unit environment → lid.
7. `HELIX_MOCK_AMS=toolchanger` (no physical tray): no box, no lid.
8. Overview detail mode (`HELIX_MOCK_AMS=multi`, open each unit): lid only on a unit with
   a reading.
9. The sheen: one soft band, no hard line, fading in on the left and out before the right
   cap; check it at `micro`, where the ramps may overlap.
10. Reduced effects: outline-only path (no sheen), on a constrained device or a local build forcing
   `reduced_effects()`.
11. The path canvas below still lines up with the spools after the row shifts by `S/2`
    (lane entry x = spool center x).

Docs (with Phase 2): `docs/devel/FILAMENT_MANAGEMENT.md` § path canvas gets the
frame → plan → paint model, span tags and band rules; the `ui_filament_path_internal.h`
file map gains `ui_filament_path_plan.cpp`. Phase 6 adds the tray/lid projection to the
AMS panel section of the same doc. This plan file is deleted in the change that lands the
last phase.

## Header check

Sketches of the changed stroker declarations and the full plan header were compiled with
`ui_filament_path_topology.cpp`'s flags from `compile_commands.json` (`clang++
-fsyntax-only`, sketch dirs first on `-I`, plus `src/ui`): clean.
`include/ams_tray_projection.h` (Phase 6) compiled clean the same way. `sizeof(PathPlan)` = 14,056 B, `sizeof(Route)` = 740 B (hence the static scratch).
`scripts/syntax_check.py` itself needs the file in a source directory, so run it on the
real headers in the worktree at Phase 1/2 start.

## Settled by the maintainer

- Phase order: the look (P1) before the restructure (P2).
- Off-route triggered bands take the lane's filament color (`BandState::Loaded`).
- The tube through the 60% LINEAR selector paints under it and shows at about 40%.
- Per-segment fill is accepted: an active slot at PREP no longer fills to the hub, and a
  PREP/LANE error colors only its own band or run.
- Hub width: PR #1753's parallel diagonals with `merge_fan_width()`, wide hub accepted.
- Phase 6 lid look approved: geometry from `docs/devel/plans/2026-10-08-ams-path-redesign/unit_render.py` (spools on the floor, crest
  1 px over the spool tops), one diffuse sheen, no specular line.

## Notes

- `EB` and the label/readout gaps use `space_md` (10 px at 800x480), scaling with the
  breakpoint (accepted).
- The glass draws under `badge_layer`, so slot badges stay untinted (accepted).
- The 8-lane fan at `micro` may only clear 13 px at the hub-width clamp: measure it before
  the Phase 5 port and report it rather than loosening the test.
