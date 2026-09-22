# U1 batch filament: review findings

Max-effort review of `main..feature/u1-batch-filament` (13 commits). Seven
dimensions; deduped and ranked. The count in brackets is how many dimensions
found it independently.

**The common thread: five of the six P0 items are invisible to every test on the
branch**, because the mock publishes full frames with `channel_error="ok"` and
every lane occupied. The one hardware state we could observe was the one that works.

## P0 — must fix

1. **`lv_tr()` runs on the WebSocket thread** [2] — `ams_backend_snapmaker.cpp`,
   cursor-advance block. `lv_translation_get` walks LVGL's global pack list, which
   the main thread mutates on a language change, and writes a file-static on a miss.
   `threading.md` invariant 1. Store a plain token and translate at render time, or
   precompute on main.

2. **`ChannelSnapshot` write clobbers omitted delta fields** [4] —
   `ams_backend_snapmaker.cpp`, feeder branch. Status frames are deltas; the parse
   immediately above treats an omitted field as "no change". The snapshot replaces
   the whole entry with `safe_bool` defaults, so a `channel_state`-only frame clears
   `filament_detected`/`module_exist` and the picker calls a loaded lane Empty; a
   `filament_detected`-only frame blanks `state` and the head reads Busy until the
   next full publish. Carry each field forward when its key is absent, as
   `sensor_enabled` already does.

3. **Empty lanes render as feeder errors** [1] — `slot_op_eligibility`. The firmware
   reports `channel_error="no_filament"` for any empty lane (recorded in this file's
   own comment near the error latch). The `error != "ok"` test runs before the Empty
   case, so an empty feeder shows "Skipped Feeder 2: feeder error". `""` and `"none"`
   are treated as hard faults too, though the parse path excludes them as false alarms.

4. **Hard-coded uppercase object key can kill the whole subscription** [1] —
   `moonraker_discovery_sequence.cpp`. `has_auto_feeding_batch` is set by an
   uppercased match, but Klipper preserves the config's case in status object keys —
   which is why `PrinterDiscovery` carries `macro_config_names_`. A printer with
   `[gcode_macro auto_feeding_batch]` sets the flag, we request a nonexistent object,
   and Moonraker rejects the entire subscription: blank printer state.

5. **`value("doing", false)` throws on a non-bool** [2] — `u1_batch_reconcile.cpp`.
   `nlohmann` `type_error.302` unwinds through the subscribe response callback before
   `discovery_completed_`, so discovery never completes on any connect. The sibling
   `auto_screws::reconcile_on_connect` guards with `find()` + a type check.

6. **Unguarded `per_slot[slot]`** [5] — `ui_batch_filament_modal.cpp` skip-warning
   loop, while `eligible_only` bounds-checks the identical index three lines up. Needs
   `total_slots` to shrink while the modal is open, so it is unreachable on a
   fixed-4-slot U1 and live on any backend whose slot count changes.

## P1 — real, same pass

7. **No busy gate spans a running batch** [1] — `filament_op_in_flight_` is released
   when `run_filament_op` returns and `action` returns to IDLE at each head's terminal,
   so the inter-head preheat gap lets a second batch dispatch. Its `ACTION=START` is
   refused by the `doing` interlock, that refusal fires the new error recovery, and the
   recovery sends `ACTION=END` into the still-running first batch. The recovery path
   makes a concurrent dispatch worse than it was before this branch.

8. **Stale `batch_.active` after an RPC failure** [3] — the recovery clears the
   interlock but not the plan, so a later single-lane load advances the zombie cursor
   and overwrites `operation_detail` with progress for an unrelated operation. Best fix
   is 9, which subsumes it.

9. **Nothing reads `doing` at runtime** [2] — the subscription's stated reason is
   false: progress is `channel_state`-driven, and the only reader is the connect-time
   reconcile. Parsing `doing` in `handle_status_update` to clear `batch_.active` makes
   the subscription earn its cost, fixes 8, and catches the one stranding case no
   detector covers: a feeder that wedges mid-load, reaching neither terminal nor fail.

10. **Recovery sends `END` unconditionally** [3] — including on TIMEOUT, whose own log
    line says the script may still be running, and without checking that `batch_` still
    names that dispatch. A timeout firing minutes after a batch completed zeroes all
    four hotends and kills a preheat the user started since. The reconcile path refuses
    in exactly these conditions; recovery does not.

11. **The interlock command and object name are duplicated** [1] — `"AUTO_FEEDING_BATCH
    ACTION=END"` in three places, `"gcode_macro AUTO_FEEDING_BATCH"` in two. Drift in
    any copy silently strands the interlock this change exists to clear, and a drift
    between the subscription key and the reconcile's lookup makes the reconcile a no-op.

12. **`manual_sta_finish` missing from the settled set** [1] — a persistent terminal
    state, so a head left in manual-feed terminal reads Busy forever.

## P2 — conventions and reuse

13. **Vendor naming** [1] — `u1_batch::` and `u1_batch_reconcile.{h,cpp}` are
    model-named where the adjacent precedent is capability-named (`auto_screws::`,
    `z_offset_persistence`). `vendor-abstraction.md` names this explicitly. The
    subscription builder naming the vendor macro is the rule's own WRONG example,
    mitigated only by sitting inside a pre-existing SNAPMAKER block.

14. **`filament_detected` parsed twice** in one loop body with different absent-field
    semantics [1]. Folds into 2.

15. **Forked `ScopedEnvVar`** [1] — `helix::ScopedEnv` (`tests/test_helpers/scoped_env.h`)
    already has the two-arg set-and-restore constructor, plus the deleted copy
    constructor the fork dropped.

## P3 — design debt, not fixed

Deliberately left for a decision rather than patched.

- **The eligibility seam has one consumer.** Every single-op Load/Unload surface routes
  through `filament_op_dispatch.h` and never asks, so tapping Load on an already-loaded
  head outside the picker still dispatches a doomed `AUTO_FEEDING`. Two refusal systems
  now coexist, neither aware of the other.
- **`slot_op_eligibility` re-derives what `classify_channel_state` encodes.** Two
  hand-kept readings of one vocabulary, already disagreeing on `none`/`inited`.
- **Three overlapping per-channel truth tables**: `loaded_at_toolhead_`,
  `port_sensor_filament_present_`, `channel_snapshots_`.
- **The reconcile cannot distinguish a stranded interlock from a live mid-batch
  reconnect.** Klipper's sequential queue keeps it to a redundant `END` rather than
  interleaving, so severity is low.

## Cleared

Explicitly checked and found sound: no re-entrant lock (`handle_status_update` writes
members directly under the held lock, never through the locking accessors);
`end_firmware_batch` does not block (fire-and-forget, the timeout is advisory);
`batch_.heads[batch_.cursor]` cannot index an empty vector (empty slots refused before
`batch_` is written); the lifetime token re-checks its generation inside the queued
lambda; the mock splits gcode per line; `ChannelSnapshot` cannot leak cross-model.
