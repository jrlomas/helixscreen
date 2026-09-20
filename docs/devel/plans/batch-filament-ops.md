# Batch filament load/unload for parallel-toolhead printers

In-flight plan. Delete in the change that ships this.

## Why

A Snapmaker U1 has four independent toolheads, each with its own feeder and nozzle.
Loading or unloading all four means four trips through the per-slot context menu. The
stock screen lets the user tick several heads and runs them in turn; users go back to
the stock UI for this.

## The firmware already sequences

`AUTO_FEEDING EXTRUDER=<n> LOAD=1|UNLOAD=1` is per-extruder, and both directions block
until the feed state machine reaches its terminal state. Verified in the firmware source
(`/home/lava/klipper/klippy/extras/filament_feed.py`): `cmd_FEED_AUTO`'s LOAD and UNLOAD
branches both call `_do_feed()` and check `channel_error` only after it returns, and
`_do_feed` gates on a single process-wide `channel_active` scalar, released in a
`finally`. So the firmware serializes across channels by itself.

Moonraker holds the `printer.gcode.script` response until the whole script has run
(measured: a two-dwell script with `M400` barriers took 6.54s for 2x3s).

So a batch is ONE multi-line gcode script. No sequencer, no completion chaining.

Looping `unload_filament(slot)` does NOT work: `AmsSubscriptionBackend::run_filament_op`
refuses a second op while one is in flight, so ops 2..N bounce off the claim.

## Design

### Interface (`include/ams_backend.h`)

Two virtuals beside the existing per-slot ops, following the `not_supported` default
pattern already used for `eject_lane` / gate select:

```cpp
virtual bool supports_batch_filament_ops() const { return false; }
virtual AmsError load_filament_batch(const std::vector<int>& slots);
virtual AmsError unload_filament_batch(const std::vector<int>& slots);
```

Defaults return `AmsErrorHelper::not_supported(...)`.

### Gate (`ams_subscription_backend.{h,cpp}`)

`run_filament_op` already owns the claim + print refusal. EXTEND it rather than forking a
twin: add a trailing `const std::vector<int>* batch = nullptr`. When non-null the switch
dispatches to a new hook instead of the per-slot one:

```cpp
virtual AmsError do_filament_batch(const std::vector<int>& slots, bool load);
```

`load_filament_batch` / `unload_filament_batch` become `final` overrides that call
`run_filament_op(op, -1, &slots)`. One claim covers the whole batch, which is the point.

### Snapmaker (`ams_backend_snapmaker.{h,cpp}`)

- `supports_batch_filament_ops()` returns true.
- `do_filament_batch()` validates each index, builds the script, and sends it with
  `execute_gcode(chain, on_ok, on_err, slots.size() * 150000, /*silent=*/true)`.
  150s per op mirrors `prepare_for_resume`; `silent` keeps a late timeout from raising a
  toast.
- The script builder is a **static, pure** member so it unit-tests without a backend,
  exactly like the existing `AmsBackendSnapmaker::preprint_gcode`:

```cpp
static std::string batch_feed_gcode(const std::vector<int>& slots, bool load);
```

Joins `AUTO_FEEDING EXTRUDER={} LOAD=1` (or `UNLOAD=1`) with `\n`, no trailing newline.
Empty input yields an empty string and the caller refuses.

### UI

- New modal, `<ui_multiselect>` rows (one per slot, labelled with the lane noun) plus
  Load and Unload actions. `UiMultiselect` already exists, is unit-tested, and is
  registered as an XML component; it has no other consumer yet.
- Entry point: a button in `ui_xml/components/ams_sidebar.xml` next to the existing
  Unload, shown only when the backend answers `supports_batch_filament_ops()`.
- Prefill: for Unload tick the slots that report filament at the toolhead; for Load tick
  the ones that do not.
- Progress reuses the existing stepper. `ams_operation_phase` already tracks the firmware
  phase and will walk head by head.

## Known ceiling

Klipper aborts the rest of a script when one command raises, so a head that fails mid
batch strands the heads after it. The picker only offering plausible slots keeps this
rare. Not worth a per-op recovery path until someone hits it.

## Tests

- `batch_feed_gcode`: ordering, load vs unload verb, single slot, empty, join has no
  trailing newline.
- Batch is refused with `not_supported` on a backend that does not implement it.
- Batch is refused as busy while another filament op holds the claim.
- Batch is refused while printing.
