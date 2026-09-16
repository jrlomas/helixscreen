# #1290 chamber-heater follow-ups

Closes the remaining task-list items on prestonbrown/helixscreen#1290. The backend
registry, auto-detection, M141-vs-raw routing, ceiling precedence and the
diagnostics card all landed already; this covers what they left open.

Rig: Snapmaker U1 at 192.168.30.103, Panda Breath at 192.168.30.104 running
DragonBreath `0b31c61`, API v2. Heating on the rig is authorised for this work.

## Live baseline

`heater_generic dragonbreath` (`max_temp: 75`, watermark), `dragonbreath` status
object (24 fields), `output_pin dragonbreath_filter`, plus Snapmaker's own
`purifier` and `temperature_sensor cavity`.

`/printer/gcode/help` lists `M141`, `M191` and `DRAGONBREATH_RESET`. There is no
`gcode_macro M141` object, which is why chamber routing correctly falls through to
`SET_HEATER_TEMPERATURE`. No drying command is registered, so #1299 stays blocked.

## Observed device vocabulary (U1 rig, 2026-09-16)

Captured by driving the heater and the filter pin and diffing the `dragonbreath`
status object every 3s.

`mode`: `off`, `power_on`.
`source`: `klipper` throughout; a non-klipper value was not reproduced from this side.
`fan_reason`: `off`, `heater`, `thermal_purge`, `requested`.
`chamber_status` / `ptc_status`: `ok` throughout; no fault vocabulary observed.
`connected`: true throughout. `device_moonraker_connected`: false throughout.

Heating to 40 C: `mode` off to power_on, `lease_owned` false to true, `lease_owner`
null to `klippy-3cf2d8ddbf89`, `heater.power` 0 to 1. So
`externally_controlled = (mode == "power_on") && !(lease_owned || source == "klipper")`
evaluates correctly here, and `mode == "power_on"` is confirmed rather than assumed.

Returning to 0: lease released the same poll, `fan_reason` goes `heater` to
`thermal_purge` and the fan keeps running at 100% until the element drops to ~40 C.

Chamber air is slow and the element is fast: 60 s of heating moved the chamber
26.9 to 29.2 C while `ptc_temp` went 25.9 to 73.7 C.

## Slice 0 — the filter-fan toggle contradicts the readout

Found on the rig, not in the issue. `output_pin dragonbreath_filter` drives the fan
only for the `requested` reason. When the device runs the fan itself (`heater`,
`thermal_purge`) the pin stays 0 while `fan_percent` reads 100.

The card derives `chamber_filter_fan_on`, `chamber_filter_fan_on_text` and
`chamber_filter_fan_icon` from the pin, and the percent readout from `fan_percent`.
So while the chamber is heating, the same row shows "Filter Fan 100%" beside a
button reading "Filter Fan: Off" with an off icon.

Measured:

| Action | `filter_pin` | `fan_percent` | `fan_reason` |
|---|---|---|---|
| idle | 0 | 0 | `off` |
| heater target 40 C | 0 | 100 | `heater` |
| target back to 0 | 0 | 100 | `thermal_purge` |
| `SET_PIN ... VALUE=1` | 1 | 100 | `requested` |
| `SET_PIN ... VALUE=0` | 0 | 0 | `off` |

The pin is a request, not the fan's state. The toggle's ON/OFF reading should come
from the fan's actual state, with the pin remaining what the button writes. The
device-driven cases also want distinguishing from a user request, since turning the
"toggle" off during a thermal purge cannot stop the fan.

Goes first: it is a visible defect in shipped code and it constrains what slice 2's
banner should say.

The data needed is already parsed and thrown away: `parse_diagnostics` fills
`ChamberHeaterDiagnostics::filter_fan_reason` from `fan_reason` and nothing reads
it. So this is plumbing an existing field to an existing card, not new parsing.

### Design

`fan_reason` is a raw vendor string, so the UI cannot switch on `thermal_purge`
without dragging a vendor vocabulary into generic code. Classify it at the backend
border exactly as `fault_reason` already becomes `FaultReason`:

```
enum class FilterFanDriver { Unknown, Off, Requested, Device };
```

DragonBreath maps `off` to Off, `requested` to Requested, and `heater` /
`thermal_purge` / anything else to Device. The raw string stays in
`filter_fan_reason` for logs, mirroring the fault pair.

Subject changes, keeping the button's action (write the pin) exactly as it is:

- `chamber_filter_fan_on` stops meaning "the pin is 1" and starts meaning "the fan
  is running" (`filter_fan_percent > 0`). This is what kills the contradiction: the
  label and icon then agree with the percent beside them.
- New `chamber_filter_fan_requested` carries the pin, so
  `on_chamber_filter_fan_clicked` keeps toggling our own request rather than
  inverting the device's state.
- New `chamber_filter_fan_device_driven` is 1 when the driver is Device. The card
  uses it to annotate the percent and to disable the button, since pressing it
  during a purge cannot stop the fan.

Disabling is an affordance, not a guard: `ctl click` fires on disabled widgets, so
the handler stays correct on its own when the driver is Device.

The generic backend returns Unknown, and Unknown must not render as Off.

Reachability caveat for the whole card: it is instantiated only in
`ui_xml/temp_graph_overlay.xml` behind
`printer_has_chamber_heater_diagnostics and temp_graph_mode eq 3`. The temp graph in
chamber mode is the only route to any of this, which is worth questioning
separately but is out of scope here.

## Slice 5 — CHAMBER_HEATER.md claims subjects that do not exist

The doc's subject table lists five that were never built:
`chamber_heater_fault_reason`, `chamber_heater_externally_controlled`,
`chamber_heater_element_temp` (int), `chamber_filter_fan_percent` (int),
`chamber_filter_fan_reason`. Only the `_text` formatter subjects exist.

Some of these become real as slices 0 to 3 land, so reconcile the doc against the
built surface at the end rather than now.

## Slice 1 — one chamber keyword rule, and one sensor pick

STATUS: the fold is DONE (`75fa521ea`). The rule now lives once, in
`chamber_heater_backend_generic.cpp#keyword_confidence`, declared beside `match()`;
sensor and cooling-fan slots score keywords only so an appliance heater never
claims them. Verified in both directions: pointing the sensor slot back at
`match()` fails `test_chamber_heater_discovery.cpp:117` on
`temperature_sensor dragonbreath`, and reverting returns 1312 assertions green.
The two copies turned out to agree on every input, so the fold removed a latent
divergence rather than an existing one.

The sensor-pick half is still open and is deliberately NOT what the fold changed.

The keyword tiers exist twice, verbatim: `chamber_heater_backend_generic.cpp#keyword_confidence`
and `printer_discovery.h#PrinterDiscovery::chamber_keyword_confidence`. Only the
heater path consults the registry; `try_set_chamber_sensor` and
`try_set_chamber_cooling_fan` still call discovery's private copy.

Fold onto one function. Expect the fold to surface places the two copies disagree.

Then the sensor pick. Chamber temperature already comes from the heater when one is
configured, so the main readout is right on this rig. But `chamber_sensor_name_`
resolves to `temperature_sensor cavity` and feeds `ui_overlay_temp_graph.cpp`,
`temperature_service.cpp` and `TemperatureSensorManager::apply_chamber_sensor_override`,
so the readout and the graph series can come from two different probes.

Decision to make: when an appliance backend wins the heater, does it also claim the
sensor slot? Proposed answer is yes, because the target the user sets refers to the
loop the appliance closes.

## Slice 2 — the appliance can be unreachable and nothing notices

`parse_diagnostics` reads 9 of the 24 fields. Unread: `connected`,
`device_moonraker_connected`, `protocol_error`, `chamber_status`, `ptc_status`,
`heater_demand`, `device_target`, `device_requested_target`, `smoothed_temp`,
`state_revision`, `boot_id`, `firmware_version`, `api_version`, `lease_owner`.

`connected` is the one that matters. When the Panda Breath drops off WiFi it goes
false, and HelixScreen presents a healthy chamber heater that will never reach its
target.

Add a link state to `ChamberHeaterDiagnostics` (tri-state: a generic backend cannot
answer the question, and Unknown must not read as Offline). Surface it in the
existing fault banner, and **hide Reset while offline** rather than offering a
button that cannot clear anything on a device we cannot reach.

`chamber_status` and `ptc_status` are string health fields reading `ok` at idle;
capture their non-ok vocabulary from the rig before deciding whether they earn
surface beyond logs.

## Slice 3 — arbitration: consume it or delete it

`ChamberHeaterDiagnostics::externally_controlled` is computed from
`mode`/`source`/`lease_owned` and its header comment calls it a display-only
annotation, but nothing displays it: grepping `src/` and `include/`, the only hits
outside the backends are the declarations. `device_autonomous_control()` is a pure
virtual every backend implements and no caller reads.

Proposal: consume `externally_controlled` as an info-level annotation in the card,
and leave `device_autonomous_control()` until slice 4 gives it its first real
caller. If slice 4 does not, it goes.

Needs the rig: drive the heater from the DragonBreath web UI while HelixScreen
watches, and record which `mode`/`source`/`lease_owned` combination actually
appears. `externally_controlled` currently keys off `mode == "power_on"`, and that
string is an assumption until observed.

## Slice 4 — stock Panda Breath firmware

`PandaBreathBackend` implements four methods: id, discovery, a 60 C ceiling, and
`device_autonomous_control() == true`. No diagnostics object, no filter fan, no
fault reset.

The device reports `inactive_slot: {"project": "panda_breath", "version": "1"}`, so
stock firmware is one A/B swap away. Flash it, capture the objects and payloads its
Klipper binding exposes, build the backend out against what is really there, then
flash back.

Last, because it takes DragonBreath off the rig that slices 1 to 3 verify against.

## Verification

Each slice: unit tests first, then `make mutate-diff` on the new hunks, then the
mock (`HELIX_MOCK_OBJECTS="heater_generic dragonbreath dragonbreath output_pin
dragonbreath_filter"`, `HELIX_MOCK_DRAGONBREATH_FAULT=1`), then the live rig.

Slice 2's offline path is verified by cutting power to the Panda Breath and
watching `connected` go false with HelixScreen attached, which is the only way to
prove the state is reachable rather than merely coded.

## Open: the one thing that needs a human

`connected: false` is slice 2's whole point and it cannot be induced from this
side. The appliance has to actually go away: pull the Panda Breath's power for
~30 s with HelixScreen watching, and capture what `connected`, `protocol_error`,
`chamber_status` and `ptc_status` do on the way out and on the way back.

Everything else in slice 2 is reproducible from the mock, so this gates the
hardware confirmation, not the implementation.

## Slice 6 — delta frames wipe diagnostics (found on hardware, PRE-EXISTING)

`DragonbreathBackend::parse_diagnostics` builds a fresh `ChamberHeaterDiagnostics`
from defaults on every frame. Moonraker sends DELTA frames carrying only the
fields that changed, so any field absent from a frame reverts to its unknown or
false default instead of keeping its last value.

Measured on the U1 with the app attached to the real printer. Toggling the filter
pin, then sampling `chamber_filter_fan_percent_text` every 0.5 s:

```
00.5s --      01.5s 100%    03.0s --      ... stays -- indefinitely
```

`100%` survives exactly one poll cycle: the delta that carried `fan_percent`. The
next delta carries only `ptc_temp`, which changes every poll, and the percent goes
back to unknown. `chamber_heater_element_temp_text` looks correct throughout
(37.1 C against the device's 37.1) purely because `ptc_temp` is in every frame.

The existing comment covers an absent OBJECT ("no news: subjects keep their last
value") but nothing covers absent FIELDS inside an object that is present.

**The serious case is not the fan.** `fault` defaults to false, so a delta frame
that does not mention `fault` clears a latched fault banner and the Reset button
with it. Same for `inhibited` and `fault_reason`. That reaches shipped code and is
not something slice 0 introduced.

Fix: make absence distinguishable from a value. Give `ChamberHeaterDiagnostics`
optional fields and have the state layer update only the ones a frame actually
carried, retaining the rest. All three backends and the subject-writing block
change together.

This sits underneath slice 0: slice 0's running-state rule is correct, and cannot
be observed on hardware until diagnostics survive a delta.

## Slice 2 design, revised after slice 6

Once the diagnostics fields are optional, comms health needs no tri-state enum of
its own: `std::optional<bool> device_connected` already carries all three
meanings. `nullopt` is "this frame did not say", `true` is online, `false` is
offline. A `LinkState` enum would duplicate what the optional provides.

Fields, from the live payload:

| Device field | Becomes | Note |
|---|---|---|
| `connected` | `std::optional<bool> device_connected` | the one that matters |
| `protocol_error` | `std::optional<std::string> link_error` | null engages as empty, logs only |
| `device_moonraker_connected` | not modelled | the appliance's own Bambu binding, reads false on a Klipper rig and means nothing to us |
| `chamber_status`, `ptc_status` | not modelled yet | only ever observed as `ok`; no fault vocabulary to map, so logs only until one appears |

Surface: a `chamber_heater_offline` subject, shown in the existing banner. **Reset
hides while offline** rather than disabling, because a latched-fault reset cannot
reach a device that is not answering, and a greyed button invites a press that
would silently do nothing.

Open until the hardware test: whether Klipper keeps reporting the heater's last
temperature when the appliance drops, or marks the sensor bad. That decides
whether the chamber readout also needs a stale marker, and it cannot be answered
from the mock.

## Slice 1b — which probe is "the chamber", measured

Measured on the U1 shortly after a heat cycle, while the element was still
shedding residual heat:

| Object | Reading |
|---|---|
| `temperature_sensor cavity` | 27.0 C |
| `dragonbreath` (appliance's own chamber probe) | 30.8 C |
| `heater_generic dragonbreath` | 31.0 C |
| `ptc_temp` (the element) | 32.4 C |

Four degrees apart. The appliance's probe sits near its own PTC element, so it
reads warm while that element is hot; the cavity sensor is elsewhere in the
enclosure.

Today the readout takes the heater's probe (temperature comes from the heater
whenever one is configured) while `chamber_sensor_name_` resolves to
`temperature_sensor cavity` and feeds the graph series,
`temperature_service.cpp` and `TemperatureSensorManager::apply_chamber_sensor_override`.
That is the worst of the two options: the same quantity shown from two probes
that disagree by 4 C.

The tradeoff is real in both directions:

- **Heater's probe wins both.** The display converges on the target the user set,
  because that is the loop the heater closes. It overstates enclosure air while
  the element is hot.
- **Cavity wins both.** The number better describes the air the print sits in,
  but a target of 40 C will sit at ~36 C on screen forever, because the heater is
  satisfying a different sensor. A readout that never reaches its setpoint reads
  as broken.

Recommendation: the heater's own probe wins both, on the grounds that a
temperature which cannot reach its own target is the worse failure, and the
cavity sensor remains available as its own graph series.

Not yet decided, and not a decision to take silently — it changes what the number
on the chamber tile means on every printer with both.

Followed the gap down as the element cooled (ptc 32.4 -> 29.1 over ~12 min):

    gap  +4.0   +2.5   +2.5   +2.2   +2.9

So it is mostly residual-element bias, but it does not close: a 2-3 C difference
persists with the element near chamber temperature. `temperature_sensor cavity`
also steps in whole degrees (27.0 <-> 28.0 and nothing between), so it carries
1 C resolution against the appliance's one decimal.

That makes the decision less dramatic than 4 C suggested but does not remove it:
the two probes still disagree by more than a degree at rest, and the readout and
the graph series currently take different ones.
