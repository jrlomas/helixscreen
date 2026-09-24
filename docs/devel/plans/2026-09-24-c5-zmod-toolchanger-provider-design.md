# Creator 5 Pro on Z-Mod: a tool changer provider, not a new backend

Issue: prestonbrown/helixscreen#1714. Status: design approved 2026-09-24, implementation plan next.

## Goal

A FlashForge Creator 5 Pro (4 docked toolheads) works in HelixScreen on both firmwares people
actually run on it, with the same UX every other tool changer gets:

- **Z-Mod** ([ghzserg/z_c5pro](https://github.com/ghzserg/z_c5pro)): stock FlashForge Klipper
  plus Z-Mod's `zmod_color` Klipper extra.
- **Reforge** ([Klipper4FlashForge/firmware](https://github.com/Klipper4FlashForge/firmware)):
  current Klipper with viesturz-style `toolchanger` / `tool Tn` objects plus `ff_*` extras.

Success: four heads show their real material and colour, the mounted head is shown (including
"none mounted"), Mount and Unmount work from the existing UI, and on Z-Mod a colour or material
edit reaches the firmware so Z-Mod's own flows and the screen agree.

## Non-goals

- **No new `AmsType` and no new backend class.** From the UI a C5 Pro is a 4-tool changer, the
  same as a Snapmaker U1 or a viesturz machine. Mount/Unmount wording already exists
  (`src/ui/ui_ams_context_menu.cpp`, keyed on `AmsBackend::load_mounts_tool()`).
- **No filament panel redesign.** The downstream fork's tool-centric panel is a preference, not a
  C5 requirement.
- **No filament feeding into a head** (Z-Mod `_T_CHANGE_FILAMENT`): whatever main does for tool
  changers today applies unchanged.
- **Runout sensors** (`fd_ex0..3`, `fm_ex0..3`) are configured by the preset through
  `FilamentSensorManager`'s existing per-tool roles, not by the provider.
- Reforge needs nothing from this design: it already runs on `AmsBackendToolChanger`. Its dock/park
  row (`TOOLCHANGE_PARK`) belongs to the fork-port track.

## What the two firmwares expose

| | Z-Mod | Reforge |
|---|---|---|
| Mounted head | `zmod_color.active_tool_id`: 0..3 mounted, -1 empty carriage, -2 dock/grab sensors disagree. Derived from `gcode_button extruder_pos1..4` (dock) and `extruder_grab1..4` (carriage) | `toolchanger.tool_number` |
| Mount / unmount | `_T_IN T=<n>` / `_T_OUT` | `SELECT_TOOL` / `UNSELECT_TOOL` |
| Material + colour | FlashForge `firmwareRes/config/filament.json` (`ex{i}_filament_type`, `ex{i}_filament_color`, both indexes), written by `CHANGE_ZCOLOR SLOT=<1..4> HEX=<RRGGBB> TYPE=<type>`; `TYPE` must be in `zmod_color.valid_types` | None on the printer. `ff_tool <n>` carries offsets only |
| Heaters | `extruder`, `extruder1`..`extruder3` | same |

Z-Mod does not publish material in status yet. We are contributing that upstream (below).

## Design

### 1. A Z-Mod row in the `toolchanger_addon` provider table

`src/printer/toolchanger_addon.cpp#providers` gains a row beside MedusaHC:

- **Detect:** Klipper objects `zmod_color` **and** `gcode_button extruder_grab1`. AD5X Z-Mod also
  publishes `zmod_color` but has no carriage grab buttons, so it cannot match.
- **Status objects:** `zmod_color`.
- **Tool reading:** `active_tool_id` maps one-to-one onto `ToolReading::current_tool`
  (`include/toolchanger_addon.h#ToolReading`), which already uses -2 / -1 / 0..N-1. A frame without
  the field is no news. Z-Mod only recomputes `active_tool_id` inside its own commands, so on a
  release without ghzserg/z_c5pro#1 it reads a stale -2 after every restart and the unit shows a
  dock sensor error until the first `_T_IN` / `_T_OUT` / `GET_ZCOLOR`. Accepted: the fix is
  upstream (PR computes it live from the dock and carriage buttons), not a second copy of Z-Mod's
  button rule here.
- **Commands:** `ToolCommands{select_prefix = "_T_IN T=", unselect = "_T_OUT"}`.
- **Feeder:** none.

The module header is rewritten from "hardware bolted onto klipper-toolchanger" to "the one place
that knows each changer's dialect", since Z-Mod has no klipper-toolchanger at all. The name stays.

### 2. Optional firmware material source on a provider

A provider may supply:

- a reader: status frame -> per-slot `{material, hex}` (Z-Mod: `zmod_color.slots`, schema
  `[{ID, Material, Color, HEX, hasFilament}]`, identical to AD5X Z-Mod's, so the existing
  `AmsBackendAd5xIfs::read_zmod_color_object` parsing rules apply and should be shared, not copied);
- a writer: slot + material + hex -> gcode (`CHANGE_ZCOLOR ...`).

`AmsBackendToolChanger` files each reading as `Observation(ObservationSource::VendorCache)` through
`helix::ams::ingest()`. Providers without a material source (MedusaHC, Reforge, plain viesturz)
behave exactly as today.

**Authority: the firmware is the only store for colour and material** once a frame has carried
`slots`. A user edit on such a slot sends the writer's gcode and does **not** file colour or material
as a `LocalUser` declaration; brand, spool name, Spoolman link and weights are declared as usual.
The screen updates from the firmware's echo one status frame later, so a rejected write never shows.
This needs one new hook in the shared edit funnel (`AmsBackend::commit_user_edit`), answering which
fields this backend's firmware stores for a slot. Chosen over AD5X's declare-then-unlock model
because that model needs baseline/echo detection with a history of lost updates (#981, #1065).

Before the first `slots` frame (a Z-Mod build without the export) the hook answers "none" and edits
are declared locally, as on any tool changer.

**Type mapping:** `zmod_color.valid_types` becomes the backend's `get_supported_materials()`, so
the edit dropdown offers only those, and `AmsBackend::normalize_material()` (the pipeline every
restricted-firmware backend uses) maps anything else by compat group before sending. A type the
firmware lists but that is unsafe on a gcode line (`IMoonrakerAPI::is_safe_material_param()`) is
refused with an error and never sent.

**Colour palette:** `filament.json` stores a colour as an index into Z-Mod's `COLOR_MAPPING` (24
entries). A hex outside it is saved as index 0, white. The writer therefore snaps the picked colour
to the nearest palette entry before sending, the same rule QIDI Box already applies
(`AmsBackendQidi::resolve_color_id`, squared RGB distance), extracted into one shared helper rather
than copied. The palette comes from `zmod_color.palette` (hex keys in index order), added to the
upstream PR. `slots` and `palette` arrive together from that PR; write-through, and the firmware
owning colour and material, start only once both have been seen. Before that, edits stay local.

**No dialog:** `CHANGE_ZCOLOR` with `HEX` and `TYPE` calls `GET_ZCOLOR` on the same command, which
opens a Mainsail/Fluidd prompt unless `SILENT=1` is set. The writer always sends
`CHANGE_ZCOLOR SLOT=<n> HEX=<RRGGBB> TYPE=<type> SILENT=1`. `CHANGE_ZCOLOR` needs both `HEX` and
`TYPE` to write without a prompt, so a colour-only edit sends the slot's current type and a
material-only edit sends its current (snapped) colour.

### 3. Empty carriage on every tool changer

`ToolState::set_ams_topology` (`src/printer/tool_state.cpp`) turns an active tool of -1 into T0.
`AmsBackendToolChanger::owns_tool_mapping_table()` is true, so a changer's -1 reaches that path: on
main, every tool changer with nothing mounted reports T0 active. Fix for all changers by letting the
topology say an empty carriage is a real state (the downstream fork's `allows_empty_carriage`
approach). Established by reading; the first test must fail on main to confirm it.

## Data flow

- **Tool state:** frame (`zmod_color.active_tool_id`) -> `toolchanger_addon::read_tool` ->
  `AmsBackendToolChanger::apply_tool_sensor_locked` -> `build_ams_topology` -> `ToolState`.
- **Mount / Unmount:** `_T_IN T=<n>` / `_T_OUT`; completion is the gcode ack plus the next
  `active_tool_id`, as for MedusaHC.
- **Material read:** frame (`zmod_color.slots`) -> provider reader -> `VendorCache` observation ->
  `resolve()` -> slot. A Spoolman link still outranks it.
- **Material write:** edit -> `AmsState::commit_slot_edit` -> backend accepts -> writer gcode; no
  colour/material declaration -> firmware echo -> `VendorCache`.

## Errors

- `active_tool_id == -2`: existing rule. Last tool held, unit shows a sensor error; a -2 mid-swap is
  transitional (`toolchanger_addon::sensor_error_is_fault`).
- `_T_IN` / `_T_OUT` refused (Z-Mod raises `gcmd.error`): surfaces like a failed `SELECT_TOOL`;
  tool state does not move because `active_tool_id` does not.
- `CHANGE_ZCOLOR` refused: the error surfaces; the screen keeps the firmware's value.
- Delta frames: an absent `active_tool_id` or `slots` is no news, never a clear.

## Testing

- **Z-Mod C5 mock persona**: `zmod_color` (`active_tool_id`, `slots`, `valid_types`),
  `gcode_button extruder_pos1..4` / `extruder_grab1..4`, `fd_ex0..3` / `fm_ex0..3`; handles `_T_IN`,
  `_T_OUT`, `CHANGE_ZCOLOR`. The existing `creator5` persona models Reforge only.
- **Unit tests, each red first:** provider detect (Z-Mod C5 yes, AD5X Z-Mod no, MedusaHC unchanged);
  `active_tool_id` mapping incl. absent field; command strings; `slots` -> `VendorCache`
  observations; writer gcode incl. `valid_types` mapping and refusal; the edit hook (no colour/
  material declaration after `slots` seen, normal declaration before); `ToolState` keeps -1 on a tool
  changer (expected to fail on main).
- **Mutation proof** for each new rule (`make mutate-diff`).
- **hw-verify:** no C5 here. Z-Mod verification by ghzserg after his export lands; the empty-carriage
  fix on Reforge by the Klipper4FlashForge authors.

## Dependencies and ordering

1. `feature/c5-platform` (fork port: DB entry detecting both firmwares, preset, mock persona) merges
   first; this work builds on its detection and persona.
2. Upstream PR ghzserg/z_c5pro#1: `slots` and `palette` in `zmod_color.get_status()`, and a live
   `active_tool_id`. Everything except the
   material source works without it, and the material source stays inert until a frame carries
   `slots`.
3. Empty-carriage fix is independent and can land first.
