# Snapmaker U1 per-print preferences

Status: design, not yet implemented.
Scope: expose the U1's `SET_PRINT_PREFERENCES` settings through surfaces we already have.

## The firmware model, and why it makes this cheap

`SET_PRINT_PREFERENCES` (`klippy/extras/print_task_config.py#cmd_SET_PRINT_PREFERENCES`)
is a plain setter. It parses named parameters, leaves anything absent untouched, writes
an in-memory dict, and persists it to JSON on the printer. No dialog, no wizard. Values
are readable back from the `print_task_config` status object.

**The preferences are consulted by the module that does the work, not by the caller.**
The slicer emits each *action* unconditionally, and each module checks the stored
preference and no-ops when disabled:

| action in sliced gcode | module that gates it on the preference |
|---|---|
| `BED_MESH_CALIBRATE` | `klippy/extras/bed_mesh.py` reads `auto_bed_leveling` |
| `TIMELAPSE_START` | `klippy/extras/timelapse.py` reads `time_lapse_camera` |
| `SM_PRINT_FLOW_CALIBRATE` | `klippy/extras/print_stats.py#cmd_SM_PRINT_FLOW_CALIBRATE` reads `flow_calibrate` and `flow_calib_extruders` |

This is what makes the feature cheap: we do not modify the gcode file, do not need the
HelixPrint plugin, and do not race the slicer. We write the preference before the print;
the already-baked commands obey it.

A mid-print guard refuses `BED_LEVEL`, `FLOW_CALIBRATE`, `SHAPER_CALIBRATE`,
`TIME_LAPSE_CAMERA` and `END_UNLOAD_FILAMENT` while printing or paused unless `FORCE=1`
is passed. We do not pass `FORCE`.

### Two commands the slicer emits that do not exist

Upstream OrcaSlicer's U1 profile still emits `SET_PRINT_AUTO_BED_LEVELING ENABLE=1` and
`SET_TIME_LAPSE_CAMERA ENABLE=1`. Neither command exists on current firmware; both were
folded into `SET_PRINT_PREFERENCES`. Klipper reports an unknown command as a non-fatal
`respond_info`, so they are silently discarded on every print. Snapmaker Orca's own
profile has already removed them. Both profiles carry the identical date stamp
`20260128`, so the stamp does not distinguish them.

Consequence: on current firmware nothing in a sliced file sets bed levelling or
timelapse. The stored preference is the only control, whichever slicer produced the file.

## Where each field lands

Eleven fields. Only four belong in the pre-print options list; the rest are machine
behaviour with existing homes.

| field | home | notes |
|---|---|---|
| `BED_LEVEL` | pre-print options | |
| `SHAPER_CALIBRATE` | pre-print options | |
| `TIME_LAPSE_CAMERA` | pre-print options | |
| `FLOW_CALIBRATE` | pre-print options | pressure advance, not extrusion flow |
| `FLOW_CALIBRATE_EXTRUDERS` | pre-print options | per-toolhead; cannot be one switch |
| `AUTO_REPLENISH_FILAMENT` | endless-spool abstraction | maps to `EndlessSpoolCapabilities::enabled` |
| `REPLENISH_IGNORE_COLOR` | endless-spool abstraction | needs a new match-policy field |
| `FILAMENT_ENTANGLE_DETECT` | AMS device-operations | |
| `FILAMENT_ENTANGLE_SEN` | AMS device-operations | three-way enum, not a toggle |
| `END_UNLOAD_FILAMENT` | AMS device-operations | per-toolhead array |
| `END_LED_TURN_OFF` | LED settings | |

`FLOW_CALIBRATE` is named misleadingly: the firmware help string is "start calibrate the
factor for pressure advance", and `flow_calibrator.py` fits K values and calls
`_set_pressure_advance()`. The result persists per extruder, and
`filament_parameters.py#is_allow_to_flow_calibrate` keys it on vendor, material, sub-type
and nozzle diameter. So it is a per-filament calibration whose *call* is per-print:
`cmd_SM_PRINT_FLOW_CALIBRATE` skips when the extruder is unused in this print, when the
preference is off, when that extruder is deselected, or when it has already been
calibrated.

## Mechanism: no new strategy kind

The handoff claimed this needs "a second strategy kind". It does not. Four already exist
(`MacroParam`, `PreStartGcode`, `QueueAheadJob`, `RuntimeCommand`, parsed in
`src/printer/pre_print_option.cpp#parse_strategy_kind`), and `PreStartGcode` already does
exactly what is needed. Creality K2 uses the same shape today.

```json
{"id": "u1_bed_level", "category": "mechanical", "order": 10,
 "strategy": "pre_start_gcode",
 "gcode_template": "SET_PRINT_PREFERENCES BED_LEVEL={value}",
 "emit_when_disabled": true}
```

Options are concatenated by
`src/ui/ui_print_preparation_manager.cpp#build_pre_start_gcode_block` into a single
`execute_gcode` call before the job. `PreStartGcode` does not depend on the HelixPrint
plugin, unlike `MacroParam`, which silently drops its parameters when the plugin is
absent.

The U1 has no `pre_print_options` block in `assets/config/printer_database.json` today,
so this is purely additive.

## Seeding toggles from live state

These preferences persist across prints and reboots. A per-job list whose defaults come
from a static `default_enabled` would drift from what the machine actually holds, and
from what Fluidd and the phone app display.

`src/printer/printer_state.cpp#PrinterState::apply_dynamic_options` already mutates the
option set at runtime - it is how the timelapse option is synthesised when the
moonraker-timelapse plugin is present. The same hook reads `print_task_config` and
overwrites each option's `default_enabled` from the machine. That is the only C++ needed.

## Out of scope

- `FILAMENT_ENTANGLE_SEN`: the renderer builds one switch per row
  (`src/ui/ui_pre_print_options_renderer.cpp#PrePrintOptionsRenderer::populate`), so a
  three-way choice has no home there. Defaults to `medium`.
- `FLOW_CALIBRATE_EXTRUDERS` as a per-toolhead selector: same reason. Ships as
  all-or-nothing under the parent toggle until a non-boolean row type exists.

## Verification

Mock coverage cannot prove any of this, because the whole feature is agreement with
firmware behaviour. Required on hardware:

1. Set each preference, read it back from `print_task_config`, confirm it persists.
2. Confirm a print with the preference off does not perform the action.
3. Confirm the mid-print guard refuses without `FORCE` and that we surface the refusal.

## Adjacent, foldable or separable

Two small items found while investigating. Neither blocks the above.

**Duplicate purifier heuristic.** `assets/config/printer_database.json` declares
`object_exists printer_objects "purifier"` twice for the U1, at confidence 80 and 75.
The same object scoring twice inflates identification confidence. The lower-confidence
copy is removed here; the `confidence: 80` "U1 air purifier module" entry stays.

**Purifier presence is not detected.** `[purifier]` is in `printer.cfg` on every U1
whether or not the top-hat accessory is attached, so the heuristic only identifies the
model. The accessory's real presence signal is `purifier.power_detected`, which we never
read. Any chamber-cooling UI should gate on that rather than on the printer model, so a
fanless or home-built enclosure is not offered controls that do nothing. The object also
exposes `mode`, `desired_chamber_temp`, `critical_chamber_temp`, and separate
`inner_fan` (with tachometer) and `exhaust_fan` blocks.

Note this is distinct from the U1's built-in chamber hardware - `temperature_sensor
cavity`, `fan_generic cavity_fan` and the `dragonbreath` heater are present and drivable
without the accessory.

## Decisions

**`REPLENISH_IGNORE_COLOR` does not extend `EndlessSpoolCapabilities`.** That struct is
shared by five backends and models backup edges and why they cannot be edited, not match
policy. One caller does not justify widening a shared type. `AUTO_REPLENISH_FILAMENT`
maps to the existing `enabled`; `IGNORE_COLOR` ships as a backend virtual bridged to its
own subject and rendered beside it, so both halves of one firmware feature stay on one
surface. Promote it into the struct when a second backend wants a configurable match
policy and can shape the interface.

**Ship four pre-print rows, then measure.** Fifteen printers declare a block today and
the largest (K1/K2 family) declares three, so four makes this the biggest such card by
one row. Rather than trim on a hunch, render it in the mock at the
smallest breakpoint and read `ctl geom` - exact, where a screenshot only proves what a
scroll position happened to expose. Cut to two and defer the rest only if the measurement
says to.

## Verified during implementation

Measured 2026-09-21 against the real machine (`.30.103`) with the desktop build.

**Option ids are a contract, not labels.** `MacroModificationManager` suppresses the
macro-modification wizard by looking up `options.find(category_to_capability_key(cat))`,
whose keys come from `category_key()` in `include/operation_patterns.h`. The bed-levelling
row must therefore be `id: "bed_mesh"` - the name this design first proposed,
`u1_bed_level`, would render identically and silently fail to suppress the wizard.

**`timelapse` is a reserved id.** `PrinterState::apply_dynamic_options` unconditionally
erases every option whose id is `timelapse` before re-adding one for the
moonraker-timelapse plugin. A database-declared `timelapse` row is deleted on every call,
so the U1's row ships as `u1_timelapse` with an explicit `label_key`.

**`requires_macro` must NOT be set.** `SET_PRINT_PREFERENCES` is registered in Python, not
a `[gcode_macro]`, so it never appears in the macro list and gating on it would hide all
four rows.

**`description_key` is parsed and never rendered.** `pre_print_option.cpp` stores it and
no consumer reads it; the K2's `ai_detect` description has never been displayed. The U1
options therefore declare labels only.

**Card measurement (the open question from Decision 2).** Four rows fit everywhere:

| tier | screen h | card bottom, 4 rows | card bottom, 5 rows |
|---|---|---|---|
| MICRO 480x272 | 272 | ~245 | **271** |
| MEDIUM 800x480 | 480 | ~421 | 474 |

Four rows is correct and has headroom. The fifth row measured above is not one of ours -
see below.

**The U1 shows a second, dead timelapse row.** `printer_has_timelapse` fires because
Moonraker reports a `timelapse` component, so `apply_dynamic_options` synthesises its
"Timelapse" option. On the U1 that component is a 47-line Snapmaker compatibility stub
(`# required by Mainsail API`) whose `_handle_settings` ignores its `web_request` and
returns a hardcoded `{"enabled": false}` for GET **and** POST. The row therefore always
reads off and discards every toggle. This is pre-existing on main and independent of this
work; the U1's real timelapse is the firmware's `klippy/extras/timelapse.py`, which gates
on `time_lapse_camera` (this design's preference) and ships frames over MQTT. Left
unresolved here - it needs a decision, not a patch.

**Purifier accessory detection confirmed.** `purifier.power_detected` is ADC-driven,
debounced and hot-pluggable, with a live path to true, and firmware drops fan writes when
it is false. Unlike `bed_plate_check` this field genuinely varies. The duplicate
`purifier` heuristic is removed; a sweep found `EXTRUDER_OFFSET_ACTION_PROBE_CALIBRATE_ALL`
duplicated too (90 and 85, kept 90). Neither removal changes detection: scoring is
base + min((identifying-1)*3, 12), the U1 has 13 identifying matches against the real
machine, and the bonus saturates at 5, so the score is 100 either way.

## Order of work

1. `pre_print_options` block for the U1 in `assets/config/printer_database.json`, four
   options, `pre_start_gcode` strategy. Data only, no C++.
2. Translation keys for the four labels and descriptions.
3. Measure the card at the smallest breakpoint; revisit decision 2.
4. Live seeding in `PrinterState::apply_dynamic_options`.
5. Hardware verification, the three checks above.

Steps 1-3 are independent of everything else and need no hardware.
