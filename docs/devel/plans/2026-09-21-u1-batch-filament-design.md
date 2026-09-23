# Snapmaker U1 batch filament: firmware-native batching, honest eligibility, per-head verification

Design for reworking the U1 "Load / Unload" batch picker so it drives the
firmware's own batch entry point, refuses heads the firmware would refuse, and
notices when a head does not finish.

## Why

Three gaps, all confirmed against firmware source and the rig at
`192.168.30.103` (stock `1.5.2.13`).

**Eligibility is guessed.** `BatchFilamentModal#prefill_selection` pre-ticks from
`slot_presence()`, which is "filament in the buffer". Live rig state:

| Head | `channel_state` | `filament_detected` |
|------|-----------------|---------------------|
| 0    | `load_finish`   | true                |
| 1    | `preload_finish`| true                |
| 2    | `load_finish`   | true                |
| 3    | `preload_finish`| true                |

All four report filament; only 0 and 2 are loaded at the toolhead. Today the
picker pre-ticks all four for Unload. `filament_detected` cannot answer "is this
head loaded" and the backend already knows better - `loaded_at_toolhead_` in
`ams_backend_snapmaker.cpp#handle_status_update` holds the real answer and the
picker never asks.

**We bypass the firmware's batch mode.** `ams_backend_snapmaker.cpp#batch_feed_gcode`
emits bare `AUTO_FEEDING EXTRUDER={n} LOAD=1` per head. Stock firmware ships
`AUTO_FEEDING_BATCH`, which wraps the same calls with temperature bookkeeping and
next-head preheat.

**Nothing verifies.** `ams_backend_snapmaker.cpp#do_filament_batch` returns as soon
as the RPC is queued. A head that fails to reach `load_finish` is invisible to us.

## What the firmware actually provides

From `Snapmaker/u1-klipper`, `lava/fluidd.cfg`. Read this before changing the
gcode we emit - the three features below are one mechanism, not a menu.

`AUTO_FEEDING_BATCH ACTION=START`
: Snapshots all four extruder targets into macro variables, sets `doing=True`.

`AUTO_FEEDING_BATCH ACTION=DOING EXTRUDER=i LOAD=1 [NEXT_EXTRUDER=j]`
: Errors unless `doing`. Preheats head `i`, and head `j` when `NEXT_EXTRUDER` is
  given, via `AUTO_FEEDING_PREHEAT` (`INNER_HEAT_TO_LOADED_FILAMENT_TEMP DELTA=-90`,
  a standby soak 90 C below target). Then calls `AUTO_FEEDING ... RESTORE_TEMP={saved
  target for head i}`.

`AUTO_FEEDING_BATCH ACTION=END`
: Restores the snapshotted targets when printing or paused; zeroes all four when
  idle. Clears `doing`.

**Why the three are inseparable.** `FEED_AUTO` restores a head's temperature after
an op from `RESTORE_TEMP` when given, otherwise from a `last_temp` it captures
inside the op (`klippy/extras/filament_feed.py#cmd_FEED_AUTO`). Preheating happens
*before* that capture, so a batch that preheats without passing `RESTORE_TEMP`
would restore heads to their preheated value instead of their pre-batch value.
Preheat overlap requires `RESTORE_TEMP`, which requires `START`/`END` to compute
it. Adopting one means adopting all three.

**`doing` is a real interlock.** `PRINT_PRESTART_CHECK` refuses to start a print
while it is set (coded error `0003-0531-0000-0021`), and resume is blocked the
same way. Klipper aborts the remaining lines of a script when one raises, so a
failed head means our own `ACTION=END` line never runs and the flag survives.
Stock self-heals on `PRINT_END` and `CANCEL_PRINT`, but not on its own.

**Version boundary.** `AUTO_FEEDING_BATCH` and `AUTO_FEEDING_PREHEAT` are absent on
the rig's `1.5.2.13` and present in the vendor repo's current `main` (1.6.x-era).
PAXX `v1.6.0-paxx12-22` carries them unmodified - its only `fluidd.cfg` patch adds
`PRINT_START`/`PRINT_END`/`CANCEL_PRINT` hooks. So this is gated on firmware
version, not on stock-vs-custom.

## Decisions

| Question | Decision | Why |
|---|---|---|
| Cooldown ownership | Firmware | `ACTION=END` restores mid-print and zeroes all four when idle. `PostOpCooldownManager` only cools the active extruder; it becomes a natural no-op since it skips when the target is already 0. |
| Accept the `doing` wedge? | Yes | Recovery is cheaper for us than for a macro - we hold a live connection and an error callback. |
| Ineligible head | Grey the row, give a reason | A macro must refuse the whole batch because it has no UI. We have a picker, so the user never selects a doomed head. |
| `DRY_RUN` equivalent | None | The picker is the preview once its labels come from `channel_state`. |
| One RPC or N | One | Keeps the firmware's own batch framing. Verification watches `channel_state` instead of splitting the script. |
| Eligibility seam | General virtual, Snapmaker impl | Default keeps today's permissive behaviour; other backends can adopt later. |
| Verification depth | Verify plus progress | Per-head terminal-state check, specific error on mismatch, "Head 2 of 4" in the sidebar. |

## Design

### 1. Capability gate

`AUTO_FEEDING_BATCH` is a `gcode_macro` with variables, so it implements
`get_status()` and appears in `printer.objects.list` - the same capability check
`ams_backend_ad5x_ifs.cpp#required_status_objects` already relies on. Verified: the
rig lists `gcode_macro AUTO_FEEDING` today.

Add a dialect accessor to `PrinterDiscovery` alongside `screws_tilt_dialect()`,
resolved at discovery from the object list. `AmsBackendSnapmaker` branches on it in
`batch_feed_gcode`:

- **present** - `START` / `DOING`+`NEXT_EXTRUDER` / `END`
- **absent** - today's bare `AUTO_FEEDING` loop, unchanged

The fallback is the code we ship now, so it costs nothing to keep and needs no new
tests beyond pinning that the branch is taken. Recovery logic (below) arms only on
the batch path; without `AUTO_FEEDING_BATCH` there is no flag to wedge.

Subscribe to `gcode_macro AUTO_FEEDING_BATCH` when present so `doing` is readable.

### 2. Eligibility

New non-pure virtual on `AmsBackend`, next to `can_unload_from_toolhead`, returning
a classification and a reason rather than a rendering. Default implementation keeps
current behaviour so no other backend changes.

`AmsBackendSnapmaker` stores the per-channel fields
`ams_backend_snapmaker.cpp#handle_status_update` currently parses and discards
(`channel_state`, `channel_error`, `filament_detected`, `module_exist`,
`disable_auto`) and answers from them:

| `channel_state` | Load | Unload |
|---|---|---|
| `wait_insert`, no filament | ineligible, "empty" | ineligible, "empty" |
| `preload_finish` / `unload_finish` + filament | **eligible** | ineligible, "not loaded" |
| `load_finish` + filament + module | ineligible, "already loaded" | **eligible** |
| anything else, or `channel_error != ok` | ineligible, with the state or error named | same |

Additional refusals on an otherwise-eligible head: `module_exist` false or
`disable_auto` true ("feeder not in automatic mode"); for Load only, the head's
`filament_motion_sensor e{n}_filament.enabled` false ("filament sensor disabled"),
which we already subscribe to in
`moonraker_discovery_sequence.cpp#complete_discovery_subscription`.

**Eligibility is direction-dependent, so rows cannot be statically greyed.** A head
at `preload_finish` is eligible for Load and ineligible for Unload; one list serves
both buttons. So the row carries its *state*, not a verdict: "Feeder 2 (PLA) -
loaded", "Feeder 3 (PLA) - ready to load", "Feeder 1 - empty", "Feeder 4 - feeder
error". Pre-tick stays unload-oriented as today. On either button press the
selection is filtered to the eligible heads for that direction, and a toast names
what was dropped and why. Nothing doomed reaches the firmware, and the user sees
the reason without the modal having to guess a direction up front.

### 3. Execution

On the gated path, one script, unchanged single-RPC dispatch:

```
AUTO_FEEDING_BATCH ACTION=START
AUTO_FEEDING_BATCH ACTION=DOING EXTRUDER=0 LOAD=1 NEXT_EXTRUDER=2
AUTO_FEEDING_BATCH ACTION=DOING EXTRUDER=2 LOAD=1
AUTO_FEEDING_BATCH ACTION=END
```

`NEXT_EXTRUDER` is the next *selected* head, omitted on the last. Timeout stays
`slots.size() * BATCH_FEED_OP_TIMEOUT_MS`.

### 4. Verification, progress, recovery

Keep the plan (ordered heads, direction) in the backend for the batch's duration.
`handle_status_update` already classifies `channel_state` per channel; extend it to
advance a cursor over the plan.

- Head reaches its expected terminal state (`load_finish` / `unload_finish`) -
  advance, publish progress.
- Head reaches a `*_fail` state, or the RPC errors or times out - stop, surface a
  message naming the head and state, send `AUTO_FEEDING_BATCH ACTION=END` so `doing`
  cannot block printing.
- Progress surfaces through `AmsSystemInfo.operation_detail`. `ams_state.cpp` already
  caches it and gives it first priority in the detail string the AMS UI observes, and
  the mock already writes "Batch load: N slots" there, so "Head 2 of 4" needs no new
  subject.

**Connect-time reconciliation.** `auto_screws::reconcile_on_connect` is the existing
precedent for firmware state that refuses unrelated filament operations until
cleared. Add the same for `doing`: on connect, if it is set and the printer is idle
with no print active, send `ACTION=END`.

## Testing

`filament_feed` and `channel_state` exist on `1.5.2`, so eligibility and
verification get real hardware coverage now. The `AUTO_FEEDING_BATCH` path needs
1.6.x on the rig.

**Mock work is a prerequisite.** `ams_backend_mock.cpp#execute_load_operation`
simulates `AmsAction` phases on a timer and never touches `channel_state` or
`operation_phase`, so the verification state machine has nothing to observe under
`--test`. The mock must publish per-channel `channel_state` transitions, including
a fault injection path for `*_fail`, before the state machine is testable.

| Area | Coverage |
|---|---|
| Eligibility table | Unit, per state x direction, both eligible and each refusal reason |
| Script builder | Unit, both dialects, `NEXT_EXTRUDER` placement and omission on last |
| Capability gate | Unit, object present and absent picks the right branch |
| Verification | Unit against the mock: happy path, mid-batch fail, timeout |
| Recovery | Unit: failure sends `ACTION=END`; connect-time reconcile clears a set flag |
| Hardware | Eligibility and verification on the rig at 1.5.2; full batch path once at 1.6.x |

## Not doing

- `DRY_RUN` mode - the picker is the preview.
- A `delayed_gcode` watchdog on the printer - we install nothing on the printer;
  the client sends `ACTION=END` itself.
- A "clear stuck batch" UI affordance - automatic recovery plus connect-time
  reconciliation should cover it. Revisit if the flag is seen wedged in practice.
- Extending batch ops to other backends - the seam is general, the implementation
  stays Snapmaker.

## Open

- Rig is on stock `1.5.2.13`. `AUTO_FEEDING_BATCH` path is mock-only until it moves
  to 1.6.x; PAXX installs from USB at the printer, so that is a manual step.
- `channel_action_state` and `channel_error_state` exist on the wire and are unused
  here. Not needed for this design; noted so a later reader does not assume they
  were missed.
