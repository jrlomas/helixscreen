# U1 Batch Filament Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the U1 batch picker ask the firmware the right questions - which heads are actually loaded, which can be operated on, and whether each one finished - and drive the firmware's own `AUTO_FEEDING_BATCH` entry point where it exists.

**Architecture:** Three independent layers. Tasks 1-4 correct what the picker asks and shows, and ship on any firmware. Tasks 5-6 add the firmware-native batch path behind a capability flag, falling back to today's loop. Tasks 7-10 add per-head verification, progress, and recovery.

**Tech Stack:** C++17, LVGL 9.5, Catch2 (`make t F='[batch]'`), Klipper/Moonraker JSON-RPC.

**Spec:** `docs/devel/plans/2026-09-21-u1-batch-filament-design.md`

## Global Constraints

- **Test tags:** existing files use `[ams][batch]` and `[snapmaker][batch]`. Keep them; run with `make t F='[batch]'`.
- **Fixture:** `BatchFixture` in `tests/unit/test_snapmaker_batch_filament_ops.cpp`. Every fixture derives from `HelixTestFixture`.
- **spdlog only.** No `printf`, `cout`, or `LV_LOG_*`.
- **No RTTI.** No `dynamic_cast` / `typeid`.
- **Comments describe the code, not its past.** No commit SHAs, no "used to", no narrated issue history. Bare issue refs like `(prestonbrown/helixscreen#1234)` are fine.
- **Vendor abstraction:** `channel_state` vocabulary lives only in `ams_backend_snapmaker.*`. Generic UI asks capability questions.
- **Doc citations** use `path#symbol`, never line numbers.
- **`NUM_TOOLS`** is the U1 head count; slot indices are 0-based tool numbers.
- **Mutex:** `AmsBackendSnapmaker` guards state with `mutex_`. Every new accessor that reads member state takes `std::lock_guard<std::mutex> lock(mutex_);`.
- **Never touch LVGL off the main thread.** Backend→UI goes through `ui_queue_update()`.

---

## File Structure

| File | Responsibility | Tasks |
|---|---|---|
| `src/ui/ui_batch_filament_modal.cpp` / `include/ui_batch_filament_modal.h` | Picker: rows, labels, pre-tick, dispatch filtering | 1, 4 |
| `src/printer/ams_backend_snapmaker.cpp` / `include/ams_backend_snapmaker.h` | Per-channel state storage, eligibility rules, script builder, verification cursor | 2, 3, 6, 8, 9 |
| `include/ams_backend.h` | The generic eligibility seam | 3 |
| `include/printer_discovery.h` | `AUTO_FEEDING_BATCH` capability flag | 5 |
| `src/api/moonraker_discovery_sequence.cpp` | Subscribe the macro object; connect-time reconcile | 5, 10 |
| `src/printer/ams_backend_mock.cpp` | Publish simulated `channel_state` transitions | 7 |
| `tests/unit/test_batch_filament_modal.cpp` | Picker unit tests | 1, 4 |
| `tests/unit/test_snapmaker_batch_filament_ops.cpp` | Backend unit tests | 2, 3, 6, 8, 9 |
| `tests/unit/test_u1_batch_verification.cpp` (new) | Verification state machine against the mock | 8 |

---

### Task 1: Unload pre-tick asks whether the head is loaded

The picker's own header comment says the tick set means "filament at the toolhead", and the variable is named `at_toolhead`, but it is filled from `slot_presence()`, which is lane occupancy. On the rig all four lanes hold filament while only two are loaded, so Unload pre-ticks two heads that have nothing at the nozzle. `AmsBackend::can_unload_from_toolhead()` already answers the real question and `AmsBackendSnapmaker` already overrides it with the `loaded_at_toolhead_` latch.

`prefill_selection` is already correct as a pure function, so testing it proves nothing - the defect is entirely at the call site in `on_show`, which no pure test reaches. So extract the data gathering into a pure function first, then fix what it collects. The file already follows this shape: `prefill_selection`, `selected_slots` and `row_label` are pure statics for exactly this reason.

**Files:**
- Modify: `src/ui/ui_batch_filament_modal.cpp` (`on_show`, new `collect_rows`)
- Modify: `include/ui_batch_filament_modal.h` (declare `BatchRowSource` + `collect_rows`)
- Test: `tests/unit/test_batch_filament_modal.cpp`

**Interfaces:**
- Consumes: `AmsBackend::can_unload_from_toolhead(int) const`, `AmsBackend::get_slot_info(int) const`, `AmsBackend::get_system_info() const` (all existing, all const).
- Produces:

```cpp
    /// What each picker row needs from the backend. Lane presence answers the
    /// label ("what is in this lane"); toolhead state answers the Unload tick
    /// ("is this head loaded"). They disagree on a lane holding filament that
    /// has not been fed to the nozzle.
    struct BatchRowSource {
        std::vector<SlotInfo> slots;
        std::vector<std::optional<bool>> lane_presence;
        std::vector<std::optional<bool>> at_toolhead;
    };

    static BatchRowSource collect_rows(const AmsBackend& backend);
```

Task 4 consumes `collect_rows` and extends `BatchRowSource` with an eligibility vector.

- [ ] **Step 1: Write the failing test**

In `tests/unit/test_batch_filament_modal.cpp`:

```cpp
namespace {
/// Every lane holds filament; only heads 0 and 2 are loaded at the toolhead.
/// This is the live rig state: four channels reporting filament_detected with
/// two at load_finish and two at preload_finish.
class DisagreeingBackend : public helix::AmsBackendSnapmaker {
  public:
    DisagreeingBackend() : helix::AmsBackendSnapmaker(nullptr, nullptr) {}

    helix::AmsSystemInfo get_system_info() const override {
        helix::AmsSystemInfo info;
        info.total_slots = 4;
        return info;
    }
    helix::SlotInfo get_slot_info(int slot_index) const override {
        helix::SlotInfo slot;
        slot.slot_index = slot_index;
        slot.status = helix::SlotStatus::AVAILABLE; // lane has filament
        return slot;
    }
    bool can_unload_from_toolhead(int slot_index) const override {
        return slot_index == 0 || slot_index == 2;
    }
};
} // namespace

TEST_CASE("BatchFilamentModal collects toolhead state separately from lane presence",
          "[ams][batch]") {
    DisagreeingBackend backend;

    const auto rows = BatchFilamentModal::collect_rows(backend);

    REQUIRE(rows.slots.size() == 4);
    CHECK(rows.lane_presence ==
          std::vector<std::optional<bool>>{true, true, true, true});
    CHECK(rows.at_toolhead ==
          std::vector<std::optional<bool>>{true, false, true, false});

    // The whole point: Unload pre-ticks the loaded heads, not every full lane.
    CHECK(BatchFilamentModal::prefill_selection(rows.at_toolhead, /*for_load=*/false) ==
          std::vector<bool>{true, false, true, false});
}
```

Add the includes the fake needs (`ams_backend_snapmaker.h`).

- [ ] **Step 2: Run to verify it fails**

Run: `make t F='[ams][batch]'`
Expected: FAIL to compile - no member named `collect_rows` in `BatchFilamentModal`.

- [ ] **Step 3: Implement `collect_rows`**

In `src/ui/ui_batch_filament_modal.cpp`:

```cpp
BatchFilamentModal::BatchRowSource BatchFilamentModal::collect_rows(const AmsBackend& backend) {
    BatchRowSource rows;
    const int total = backend.get_system_info().total_slots;
    rows.slots.reserve(static_cast<size_t>(total));
    rows.lane_presence.reserve(static_cast<size_t>(total));
    rows.at_toolhead.reserve(static_cast<size_t>(total));
    for (int slot = 0; slot < total; ++slot) {
        rows.slots.push_back(backend.get_slot_info(slot));
        rows.lane_presence.push_back(slot_presence(rows.slots.back()));
        rows.at_toolhead.push_back(backend.can_unload_from_toolhead(slot));
    }
    return rows;
}
```

- [ ] **Step 4: Rewire `on_show` onto it**

Replace the inline gathering loop in `on_show` with:

```cpp
    const BatchRowSource rows = collect_rows(*backend);
    const std::vector<bool> ticked = prefill_selection(rows.at_toolhead, /*for_load=*/false);

    std::vector<MultiSelectItem> items;
    items.reserve(rows.slots.size());
    for (size_t slot = 0; slot < rows.slots.size(); ++slot) {
        items.push_back({std::to_string(slot),
                         row_label(backend->lane_noun(), static_cast<int>(slot), rows.slots[slot],
                                   rows.lane_presence[slot]),
                         ticked[slot]});
    }
```

`row_label` keeps taking lane presence, so `"(Empty)"` still means "this lane has no filament" and is unaffected.

- [ ] **Step 5: Run the suite**

Run: `make t F='[batch]'`
Expected: PASS, including the existing `prefill ticks by direction` and `row label names the lane contents` cases, which are unchanged.

- [ ] **Step 6: Prove the test can fail**

Revert `collect_rows` to push `slot_presence(...)` into `at_toolhead` instead of `can_unload_from_toolhead(slot)` and re-run. The new case must go red on the `at_toolhead` and `prefill_selection` assertions. Restore the fix. Name this in the commit body.

- [ ] **Step 7: Commit**

```bash
git add src/ui/ui_batch_filament_modal.cpp include/ui_batch_filament_modal.h tests/unit/test_batch_filament_modal.cpp
git commit -m "fix(ams): unload pre-tick asks whether the head is loaded, not whether the lane has filament"
```

---

### Task 2: Backend keeps the per-channel fields it parses

`handle_status_update` reads `channel_state`, `channel_error`, `filament_detected`, `module_exist` and `disable_auto` and throws all but a latch away. Eligibility needs them.

**Files:**
- Modify: `include/ams_backend_snapmaker.h` (new member + accessor)
- Modify: `src/printer/ams_backend_snapmaker.cpp` (`handle_status_update`)
- Test: `tests/unit/test_snapmaker_batch_filament_ops.cpp`

**Interfaces:**
- Produces: `struct ChannelSnapshot { std::string state; std::string error; bool filament_detected; bool module_exist; bool disable_auto; };` and `ChannelSnapshot channel_snapshot(int slot_index) const;` on `AmsBackendSnapmaker`. Task 3 consumes both.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE_METHOD(BatchFixture, "Snapmaker keeps the per-channel feeder fields", "[snapmaker][batch]") {
    feed_status(R"({"filament_feed left":{"extruder0":{
        "channel_state":"load_finish","channel_error":"ok","filament_detected":true,
        "module_exist":true,"disable_auto":false}}})");

    const auto snap = backend().channel_snapshot(0);

    CHECK(snap.state == "load_finish");
    CHECK(snap.error == "ok");
    CHECK(snap.filament_detected);
    CHECK(snap.module_exist);
    CHECK_FALSE(snap.disable_auto);
}
```

If `BatchFixture` has no `feed_status` helper, add one that forwards a JSON string to the backend's status handler, mirroring how the existing dispatch tests drive it.

- [ ] **Step 2: Run to verify it fails**

Run: `make t F='[snapmaker][batch]'`
Expected: FAIL - no member named `channel_snapshot`.

- [ ] **Step 3: Add the storage**

In `include/ams_backend_snapmaker.h`, inside the class:

```cpp
    /// Raw per-channel feeder fields as the firmware last reported them.
    /// Eligibility answers from these; presence alone cannot distinguish a
    /// lane holding filament from a head that is loaded.
    struct ChannelSnapshot {
        std::string state;            ///< channel_state, e.g. "load_finish"
        std::string error{"ok"};      ///< channel_error
        bool filament_detected{false};
        bool module_exist{false};
        bool disable_auto{false};
    };

    [[nodiscard]] ChannelSnapshot channel_snapshot(int slot_index) const;

  private:
    std::array<ChannelSnapshot, NUM_TOOLS> channel_snapshots_{};
```

- [ ] **Step 4: Populate it**

In `src/printer/ams_backend_snapmaker.cpp`, in the `filament_feed` branch of `handle_status_update` where `channel_state` and `channel_error` are already read, store the whole set before classifying:

```cpp
            ChannelSnapshot snap;
            snap.state = safe_string(ch, "channel_state", "");
            snap.error = safe_string(ch, "channel_error", "ok");
            snap.filament_detected = safe_bool(ch, "filament_detected", false);
            snap.module_exist = safe_bool(ch, "module_exist", false);
            snap.disable_auto = safe_bool(ch, "disable_auto", false);
            channel_snapshots_[static_cast<size_t>(global_index)] = snap;
```

Use whatever the surrounding code already calls the resolved slot index and the `safe_*` helpers; do not invent new ones.

Add the accessor:

```cpp
AmsBackendSnapmaker::ChannelSnapshot AmsBackendSnapmaker::channel_snapshot(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot_index < 0 || slot_index >= NUM_TOOLS) {
        return {};
    }
    return channel_snapshots_[static_cast<size_t>(slot_index)];
}
```

- [ ] **Step 5: Run to verify it passes**

Run: `make t F='[snapmaker][batch]'`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add include/ams_backend_snapmaker.h src/printer/ams_backend_snapmaker.cpp tests/unit/test_snapmaker_batch_filament_ops.cpp
git commit -m "refactor(snapmaker): keep the per-channel feeder fields the status parse already reads"
```

---

### Task 3: A generic eligibility question, answered from channel state

**Files:**
- Modify: `include/ams_backend.h` (new virtual + result type)
- Modify: `include/ams_backend_snapmaker.h`, `src/printer/ams_backend_snapmaker.cpp` (override)
- Test: `tests/unit/test_snapmaker_batch_filament_ops.cpp`

**Interfaces:**
- Produces, on `AmsBackend`:

```cpp
    /// Why a slot cannot take a filament operation right now. Backends return
    /// a classification; callers decide how to render it.
    enum class FilamentOpEligibility {
        Eligible,
        Empty,           ///< no filament in the lane
        AlreadyLoaded,   ///< load requested on a head that is already loaded
        NotLoaded,       ///< unload requested on a head with nothing at the nozzle
        FeederUnavailable, ///< module absent or not in automatic mode
        SensorDisabled,  ///< load needs the head's motion sensor enabled
        Busy,            ///< transient or unrecognised channel state
        Error,           ///< the feeder reports a fault
    };

    [[nodiscard]] virtual FilamentOpEligibility
    slot_op_eligibility(int slot_index, bool load) const {
        (void)load;
        return get_slot_info(slot_index).is_present() ? FilamentOpEligibility::Eligible
                                                      : FilamentOpEligibility::Empty;
    }
```

Task 4 consumes `slot_op_eligibility` and a free function `const char* filament_op_eligibility_reason(FilamentOpEligibility)` declared in the same header, defined in `src/printer/ams_backend.cpp` if one exists, otherwise inline in the header.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE_METHOD(BatchFixture, "Snapmaker eligibility follows channel state", "[snapmaker][batch]") {
    using E = AmsBackend::FilamentOpEligibility;

    SECTION("preload_finish with filament loads, does not unload") {
        set_channel(0, "preload_finish", "ok", /*detected=*/true, /*module=*/true, /*no_auto=*/false);
        CHECK(backend().slot_op_eligibility(0, /*load=*/true) == E::Eligible);
        CHECK(backend().slot_op_eligibility(0, /*load=*/false) == E::NotLoaded);
    }
    SECTION("load_finish unloads, does not load") {
        set_channel(0, "load_finish", "ok", true, true, false);
        CHECK(backend().slot_op_eligibility(0, /*load=*/false) == E::Eligible);
        CHECK(backend().slot_op_eligibility(0, /*load=*/true) == E::AlreadyLoaded);
    }
    SECTION("wait_insert with no filament is empty in both directions") {
        set_channel(0, "wait_insert", "ok", /*detected=*/false, true, false);
        CHECK(backend().slot_op_eligibility(0, true) == E::Empty);
        CHECK(backend().slot_op_eligibility(0, false) == E::Empty);
    }
    SECTION("a feeder fault beats everything") {
        set_channel(0, "load_finish", "jam", true, true, false);
        CHECK(backend().slot_op_eligibility(0, false) == E::Error);
    }
    SECTION("manual mode or absent module refuses an otherwise eligible head") {
        set_channel(0, "preload_finish", "ok", true, /*module=*/true, /*no_auto=*/true);
        CHECK(backend().slot_op_eligibility(0, true) == E::FeederUnavailable);
        set_channel(1, "preload_finish", "ok", true, /*module=*/false, /*no_auto=*/false);
        CHECK(backend().slot_op_eligibility(1, true) == E::FeederUnavailable);
    }
    SECTION("an unrecognised state is busy, never eligible") {
        set_channel(0, "loading", "ok", true, true, false);
        CHECK(backend().slot_op_eligibility(0, true) == E::Busy);
        CHECK(backend().slot_op_eligibility(0, false) == E::Busy);
    }
}

TEST_CASE_METHOD(BatchFixture, "A backend without the override stays permissive", "[ams][batch]") {
    // The default must not change behaviour for AFC, Happy Hare, ACE or AD5X.
    CHECK(plain_backend().slot_op_eligibility(0, /*load=*/true) ==
          AmsBackend::FilamentOpEligibility::Eligible);
}
```

Add a `set_channel(slot, state, error, detected, module, no_auto)` helper to `BatchFixture` that builds the `filament_feed` JSON for that slot and feeds it through the status handler.

- [ ] **Step 2: Run to verify it fails**

Run: `make t F='[batch]'`
Expected: FAIL - no member named `slot_op_eligibility`.

- [ ] **Step 3: Implement the Snapmaker override**

```cpp
AmsBackend::FilamentOpEligibility
AmsBackendSnapmaker::slot_op_eligibility(int slot_index, bool load) const {
    using E = FilamentOpEligibility;
    if (slot_index < 0 || slot_index >= NUM_TOOLS) {
        return E::Busy;
    }
    const ChannelSnapshot snap = channel_snapshot(slot_index);

    if (snap.error != "ok") {
        return E::Error;
    }
    // Only these four are settled states. Anything else is mid-operation or
    // unrecognised, and a batch must not act on a head it cannot describe.
    const bool settled = snap.state == "wait_insert" || snap.state == "preload_finish" ||
                         snap.state == "load_finish" || snap.state == "unload_finish";
    if (!settled) {
        return E::Busy;
    }
    if (!snap.filament_detected) {
        return E::Empty;
    }
    const bool loaded = snap.state == "load_finish";
    if (load && loaded) {
        return E::AlreadyLoaded;
    }
    if (!load && !loaded) {
        return E::NotLoaded;
    }
    // Eligible on state; now the feeder has to be able to act.
    if (!snap.module_exist || snap.disable_auto) {
        return E::FeederUnavailable;
    }
    if (load && !motion_sensor_enabled(slot_index)) {
        return E::SensorDisabled;
    }
    return E::Eligible;
}
```

`motion_sensor_enabled(int)` reads the `enabled` field of `filament_motion_sensor e{n}_filament`, which discovery already subscribes to in `moonraker_discovery_sequence.cpp#complete_discovery_subscription`. If the backend does not yet store it, add it to `ChannelSnapshot` in the same shape as Task 2 and parse it in the sensor branch of `handle_status_update`.

Add the reason strings:

```cpp
inline const char* filament_op_eligibility_reason(AmsBackend::FilamentOpEligibility e) {
    switch (e) {
    case AmsBackend::FilamentOpEligibility::Eligible:          return "";
    case AmsBackend::FilamentOpEligibility::Empty:             return "empty";
    case AmsBackend::FilamentOpEligibility::AlreadyLoaded:     return "already loaded";
    case AmsBackend::FilamentOpEligibility::NotLoaded:         return "not loaded";
    case AmsBackend::FilamentOpEligibility::FeederUnavailable: return "feeder not in automatic mode";
    case AmsBackend::FilamentOpEligibility::SensorDisabled:    return "filament sensor disabled";
    case AmsBackend::FilamentOpEligibility::Busy:              return "busy";
    case AmsBackend::FilamentOpEligibility::Error:             return "feeder error";
    }
    return "";
}
```

These reach the user, so wrap them at the call site with `lv_tr()` and run `make translation-sync && make translations` in Task 4.

- [ ] **Step 4: Run to verify it passes**

Run: `make t F='[batch]'`
Expected: PASS, all sections.

- [ ] **Step 5: Commit**

```bash
git add include/ams_backend.h include/ams_backend_snapmaker.h src/printer/ams_backend_snapmaker.cpp tests/unit/test_snapmaker_batch_filament_ops.cpp
git commit -m "feat(ams): a slot can say why it cannot take a filament operation"
```

---

### Task 4: The picker shows state and refuses nothing silently

Eligibility is direction-dependent, so rows cannot be statically greyed - a head at `preload_finish` is eligible for Load and not for Unload, and one list serves both buttons. Rows carry their state; the selection is filtered on button press with a toast naming what was dropped.

**Files:**
- Modify: `src/ui/ui_batch_filament_modal.cpp` (`row_label`, `dispatch`), `include/ui_batch_filament_modal.h`
- Test: `tests/unit/test_batch_filament_modal.cpp`

**Interfaces:**
- Consumes: `AmsBackend::slot_op_eligibility`, `filament_op_eligibility_reason` from Task 3.
- Produces: `static std::vector<int> eligible_only(const std::vector<int>& selected, const std::vector<FilamentOpEligibility>& per_slot);`

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("BatchFilamentModal drops ineligible slots before dispatch", "[ams][batch]") {
    using E = AmsBackend::FilamentOpEligibility;
    const std::vector<E> per_slot{E::Eligible, E::NotLoaded, E::Eligible, E::Error};

    CHECK(helix::ui::BatchFilamentModal::eligible_only({0, 1, 2, 3}, per_slot) ==
          std::vector<int>{0, 2});
    CHECK(helix::ui::BatchFilamentModal::eligible_only({1, 3}, per_slot).empty());
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `make t F='[ams][batch]'`
Expected: FAIL - no member named `eligible_only`.

- [ ] **Step 3: Implement**

```cpp
std::vector<int> BatchFilamentModal::eligible_only(
    const std::vector<int>& selected,
    const std::vector<AmsBackend::FilamentOpEligibility>& per_slot) {
    std::vector<int> keep;
    keep.reserve(selected.size());
    for (int slot : selected) {
        const auto idx = static_cast<size_t>(slot);
        if (idx < per_slot.size() &&
            per_slot[idx] == AmsBackend::FilamentOpEligibility::Eligible) {
            keep.push_back(slot);
        }
    }
    return keep;
}
```

In `dispatch`, between reading the selection and calling the backend:

```cpp
    std::vector<AmsBackend::FilamentOpEligibility> per_slot;
    const AmsSystemInfo sys = backend->get_system_info();
    per_slot.reserve(static_cast<size_t>(sys.total_slots));
    for (int slot = 0; slot < sys.total_slots; ++slot) {
        per_slot.push_back(backend->slot_op_eligibility(slot, load));
    }
    const std::vector<int> runnable = eligible_only(slots, per_slot);
    if (runnable.size() != slots.size()) {
        // Name the first head we are dropping and why; a batch that silently
        // shrinks is worse than one that explains itself.
        for (int slot : slots) {
            const auto e = per_slot[static_cast<size_t>(slot)];
            if (e != AmsBackend::FilamentOpEligibility::Eligible) {
                NOTIFY_WARNING("{} {}: {}", lv_tr("Skipped"),
                               lane_label(backend->lane_noun(), slot),
                               lv_tr(filament_op_eligibility_reason(e)));
                break;
            }
        }
    }
    if (runnable.empty()) {
        return; // keep the picker open - nothing was dispatched
    }
```

then dispatch `runnable` rather than `slots`.

In `row_label`, name the state rather than only "(Empty)":

```cpp
std::string BatchFilamentModal::row_label(LaneNoun noun, int slot, const SlotInfo& info,
                                          std::optional<bool> present, bool at_toolhead) {
    const std::string lane = lane_label(noun, slot);
    std::string what;
    if (!info.material.empty()) {
        what = info.material; // material: no i18n
    } else if (present && !*present) {
        what = lv_tr("Empty");
    }
    const char* where = at_toolhead ? lv_tr("loaded") : lv_tr("ready to load");
    if (what.empty()) {
        return present && !*present ? lane + " (" + lv_tr("Empty") + ")" : lane;
    }
    return lane + " (" + what + " - " + where + ")";
}
```

Update the existing `row label names the lane contents` test for the new parameter and the new strings.

- [ ] **Step 4: Sync translations**

Run: `make translation-sync && make translations`
Then stage the YAMLs **and** the generated `ui_xml/translations/*.xml`, which are tracked and not auto-staged.

- [ ] **Step 5: Run the suite**

Run: `make t F='[batch]'`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add src/ui/ui_batch_filament_modal.cpp include/ui_batch_filament_modal.h tests/unit/test_batch_filament_modal.cpp translations/ ui_xml/translations/
git commit -m "feat(ams): the batch picker names each head's state and says what it skipped"
```

---

### Task 5: Discovery records whether the firmware has `AUTO_FEEDING_BATCH`

`AUTO_FEEDING_BATCH` is absent on the rig's stock `1.5.2.13` and present from 1.6.x. It is a `gcode_macro` with variables, so it implements `get_status()` and appears in `printer.objects.list` - the same capability check `ams_backend_ad5x_ifs.cpp#required_status_objects` relies on, and confirmed live: the rig lists `gcode_macro AUTO_FEEDING`.

The spec called this a "dialect accessor". A dialect enum earns its place when two modules compete, which is why `screws_tilt_dialect()` is one. Here it is presence or absence, so a `has_*` bool matching `has_qgl()` / `has_tool_changer()` is the right shape.

**Files:**
- Modify: `include/printer_discovery.h` (member, accessor, `parse_objects`)
- Modify: `src/api/moonraker_discovery_sequence.cpp` (subscribe the macro when present)
- Test: `tests/unit/test_printer_discovery.cpp` (or the existing discovery test file; find it with `grep -rl "parse_objects" tests/unit/`)

**Interfaces:**
- Produces: `[[nodiscard]] bool has_auto_feeding_batch() const` on `PrinterDiscovery`. Task 6 consumes it.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("Discovery detects the U1 batch feeding macro", "[discovery]") {
    PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json::array(
        {"gcode_macro AUTO_FEEDING", "gcode_macro AUTO_FEEDING_BATCH", "filament_feed left"}));
    CHECK(hw.has_auto_feeding_batch());

    PrinterDiscovery older;
    older.parse_objects(nlohmann::json::array({"gcode_macro AUTO_FEEDING", "filament_feed left"}));
    CHECK_FALSE(older.has_auto_feeding_batch());
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `make t F='[discovery]'`
Expected: FAIL - no member named `has_auto_feeding_batch`.

- [ ] **Step 3: Implement**

In `include/printer_discovery.h`, add the member beside the other `has_*` flags, set it in `parse_objects` where object names are matched, and add:

```cpp
    /// Whether the firmware ships AUTO_FEEDING_BATCH, the macro that wraps a
    /// multi-head feed with target snapshot/restore and next-head preheat.
    /// Absent before firmware 1.6; the batch path falls back to bare
    /// AUTO_FEEDING per head when this is false.
    [[nodiscard]] bool has_auto_feeding_batch() const {
        return has_auto_feeding_batch_;
    }
```

In `src/api/moonraker_discovery_sequence.cpp`, in the SNAPMAKER subscription block that already adds `filament_feed` and the motion sensors, subscribe the macro so its `doing` variable is readable:

```cpp
    if (hw.has_auto_feeding_batch()) {
        subscription_objects["gcode_macro AUTO_FEEDING_BATCH"] = nullptr;
    }
```

- [ ] **Step 4: Run to verify it passes**

Run: `make t F='[discovery]'`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add include/printer_discovery.h src/api/moonraker_discovery_sequence.cpp tests/unit/test_printer_discovery.cpp
git commit -m "feat(discovery): detect the U1 AUTO_FEEDING_BATCH macro"
```

---

### Task 6: The script builder drives the firmware's batch mode

The three firmware features are one mechanism. `ACTION=START` snapshots all four extruder targets; `ACTION=DOING` preheats the current head and, given `NEXT_EXTRUDER`, the next one, then calls `AUTO_FEEDING ... RESTORE_TEMP={snapshot}`; `ACTION=END` restores those targets when printing and zeroes all four when idle. Preheating before the op is exactly why `RESTORE_TEMP` must be passed - without it `FEED_AUTO` restores from a `last_temp` it captures *after* the preheat.

**Files:**
- Modify: `include/ams_backend_snapmaker.h`, `src/printer/ams_backend_snapmaker.cpp` (`batch_feed_gcode`)
- Test: `tests/unit/test_snapmaker_batch_filament_ops.cpp`

**Interfaces:**
- Consumes: `PrinterDiscovery::has_auto_feeding_batch()` from Task 5, surfaced onto the backend as a `bool use_batch_macro_` set at construction or from `on_started()`, whichever the backend already uses to read discovery.
- Produces: `batch_feed_gcode` keeps its signature `std::string(const std::vector<int>&, bool)`.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("Snapmaker batch_feed_gcode drives AUTO_FEEDING_BATCH when the firmware has it",
          "[snapmaker][batch]") {
    // NEXT_EXTRUDER names the next SELECTED head so the firmware preheats it
    // while the current one runs, and is omitted on the last.
    const std::string chain = batch_feed_gcode_with_macro({0, 2, 3}, /*load=*/true);

    CHECK(chain ==
          "AUTO_FEEDING_BATCH ACTION=START\n"
          "AUTO_FEEDING_BATCH ACTION=DOING EXTRUDER=0 LOAD=1 NEXT_EXTRUDER=2\n"
          "AUTO_FEEDING_BATCH ACTION=DOING EXTRUDER=2 LOAD=1 NEXT_EXTRUDER=3\n"
          "AUTO_FEEDING_BATCH ACTION=DOING EXTRUDER=3 LOAD=1\n"
          "AUTO_FEEDING_BATCH ACTION=END");
}

TEST_CASE("Snapmaker batch_feed_gcode falls back to bare AUTO_FEEDING without the macro",
          "[snapmaker][batch]") {
    const std::string chain = batch_feed_gcode_without_macro({1, 3}, /*load=*/false);

    CHECK(chain == "AUTO_FEEDING EXTRUDER=1 UNLOAD=1\n"
                   "AUTO_FEEDING EXTRUDER=3 UNLOAD=1");
}
```

The two helpers construct a backend with `use_batch_macro_` true and false respectively; add them to the test file, not to production code.

- [ ] **Step 2: Run to verify it fails**

Run: `make t F='[snapmaker][batch]'`
Expected: FAIL on the first case - the builder emits bare `AUTO_FEEDING` unconditionally.

- [ ] **Step 3: Implement**

```cpp
std::string AmsBackendSnapmaker::batch_feed_gcode(const std::vector<int>& slots, bool load) const {
    const char* dir = load ? "LOAD=1" : "UNLOAD=1";
    if (!use_batch_macro_) {
        std::string chain;
        for (int slot : slots) {
            if (!chain.empty()) {
                chain += '\n';
            }
            chain += fmt::format("AUTO_FEEDING EXTRUDER={} {}", slot, dir);
        }
        return chain;
    }

    // START snapshots every hotend target and raises the `doing` interlock;
    // END restores those targets mid-print and zeroes them when idle. The
    // firmware refuses a print start while `doing` is set, so END must run.
    std::string chain = "AUTO_FEEDING_BATCH ACTION=START";
    for (size_t i = 0; i < slots.size(); ++i) {
        chain += fmt::format("\nAUTO_FEEDING_BATCH ACTION=DOING EXTRUDER={} {}", slots[i], dir);
        if (i + 1 < slots.size()) {
            chain += fmt::format(" NEXT_EXTRUDER={}", slots[i + 1]);
        }
    }
    chain += "\nAUTO_FEEDING_BATCH ACTION=END";
    return chain;
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `make t F='[snapmaker][batch]'`
Expected: PASS, both cases, plus the existing `joins one AUTO_FEEDING line per slot` case which now covers the fallback.

- [ ] **Step 5: Prove the test can fail**

Run: `make mutate-diff`
Confirm at least one mutation of the `NEXT_EXTRUDER` placement or the `i + 1 < slots.size()` bound turns the suite red, and name it in the commit body.

- [ ] **Step 6: Commit**

```bash
git add include/ams_backend_snapmaker.h src/printer/ams_backend_snapmaker.cpp tests/unit/test_snapmaker_batch_filament_ops.cpp
git commit -m "feat(snapmaker): batch loads drive AUTO_FEEDING_BATCH with next-head preheat"
```

---

### Task 7: The mock publishes channel-state transitions

`ams_backend_mock.cpp#execute_load_operation` walks `AmsAction` phases on a timer and never writes `channel_state` or `operation_phase`, so a verification state machine has nothing to observe under `--test`. This task is a prerequisite for Task 8, not a nicety.

**Files:**
- Modify: `src/printer/ams_backend_mock.cpp` (`run_filament_batch`, `execute_load_operation`, `execute_unload_operation`)
- Test: `tests/unit/test_u1_batch_verification.cpp` (create)

**Interfaces:**
- Produces: the mock emits, per head and in order, the same `channel_state` strings the firmware does - load: `preload_finish` → `load_finish`; unload: `load_finish` → `unload_finish` - through the same `handle_status_update` path the real backend uses, so the verification cursor is exercised identically.
- Produces: `HELIX_MOCK_BATCH_FAIL_SLOT` environment variable. When set to a slot index, that head emits `load_fail` / `unload_fail` instead of its terminal state.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE_METHOD(MockBatchFixture, "Mock walks each head to its terminal channel state",
                 "[ams][batch][mock]") {
    backend().load_filament_batch({0, 1});
    pump_until_idle();

    CHECK(backend().channel_snapshot(0).state == "load_finish");
    CHECK(backend().channel_snapshot(1).state == "load_finish");
}

TEST_CASE_METHOD(MockBatchFixture, "Mock can fail a named head", "[ams][batch][mock]") {
    set_env("HELIX_MOCK_BATCH_FAIL_SLOT", "1");
    backend().load_filament_batch({0, 1});
    pump_until_idle();

    CHECK(backend().channel_snapshot(0).state == "load_finish");
    CHECK(backend().channel_snapshot(1).state == "load_fail");
}
```

`pump_until_idle()` drives `UpdateQueue` and the mock's timers until the backend's action returns to IDLE, with a bounded iteration count so a hang fails rather than spins. Note that the test pump runs one-shot timers only, so the mock's phase walk must be driven by one-shot timers or by direct calls, not a periodic timer.

- [ ] **Step 2: Run to verify it fails**

Run: `make t F='[batch][mock]'`
Expected: FAIL - `channel_snapshot(0).state` is empty; the mock never writes it.

- [ ] **Step 3: Implement**

In the mock's per-slot operation walk, after each phase transition, build the same `filament_feed` JSON shape the real firmware sends for that channel and route it through the backend's status handler, so the snapshot and the latch both update. Read `HELIX_MOCK_BATCH_FAIL_SLOT` once at batch start and substitute the `*_fail` terminal for that head.

Document the variable in `docs/devel/MOCK_ENVIRONMENT_VARIABLES.md` in the same table format the neighbouring entries use.

- [ ] **Step 4: Run to verify it passes**

Run: `make t F='[batch][mock]'`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add src/printer/ams_backend_mock.cpp tests/unit/test_u1_batch_verification.cpp docs/devel/MOCK_ENVIRONMENT_VARIABLES.md
git commit -m "test(mock): the U1 mock publishes per-channel state transitions"
```

---

### Task 8: Verify each head and report progress

**Files:**
- Modify: `include/ams_backend_snapmaker.h`, `src/printer/ams_backend_snapmaker.cpp`
- Test: `tests/unit/test_u1_batch_verification.cpp`

**Interfaces:**
- Produces on `AmsBackendSnapmaker`:

```cpp
    struct BatchPlan {
        std::vector<int> heads;   ///< in dispatch order
        bool load{false};
        size_t cursor{0};         ///< how many heads have reached their terminal state
        bool active{false};
    };
```
  plus `[[nodiscard]] BatchPlan batch_plan() const;` for tests. Task 9 consumes `batch_plan()`.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE_METHOD(MockBatchFixture, "A batch advances its cursor as heads finish",
                 "[ams][batch]") {
    backend().load_filament_batch({0, 1});
    pump_until_idle();

    const auto plan = backend().batch_plan();
    CHECK(plan.cursor == 2);
    CHECK_FALSE(plan.active);
}

TEST_CASE_METHOD(MockBatchFixture, "A failed head stops the batch at its cursor",
                 "[ams][batch]") {
    set_env("HELIX_MOCK_BATCH_FAIL_SLOT", "1");
    backend().load_filament_batch({0, 1});
    pump_until_idle();

    const auto plan = backend().batch_plan();
    CHECK(plan.cursor == 1);          // head 0 finished, head 1 did not
    CHECK_FALSE(plan.active);
    CHECK(backend().get_system_info().operation_detail.find("2") != std::string::npos);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `make t F='[ams][batch]'`
Expected: FAIL - no member named `batch_plan`.

- [ ] **Step 3: Implement**

Record the plan in `do_filament_batch` before dispatching. In the `channel_state` branch of `handle_status_update`, when the channel at `plan.heads[plan.cursor]` reaches the expected terminal state for the direction, advance the cursor and set `system_info_.operation_detail` to the progress string:

```cpp
    // "Head 2 of 4". operation_detail is the string ams_state.cpp gives first
    // priority in the AMS detail line, so progress needs no new subject.
    system_info_.operation_detail =
        fmt::format("{} {} {} {}", batch_.load ? "Load" : "Unload", batch_.cursor + 1,
                    "of", batch_.heads.size());
```

Wrap the user-visible words with `lv_tr()` and re-run the translation sync as in Task 4. When the channel reaches a `*_fail` state instead, clear `active`, leave the cursor where it is, and hand off to Task 9's failure path.

- [ ] **Step 4: Run to verify it passes**

Run: `make t F='[ams][batch]'`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add include/ams_backend_snapmaker.h src/printer/ams_backend_snapmaker.cpp tests/unit/test_u1_batch_verification.cpp translations/ ui_xml/translations/
git commit -m "feat(snapmaker): a batch verifies each head and reports which one it is on"
```

---

### Task 9: A failed batch clears the firmware interlock

Klipper aborts the remaining lines of a script when one raises, so a failed head means our own `ACTION=END` never runs and `doing` stays set. `PRINT_PRESTART_CHECK` then refuses to start a print (`0003-0531-0000-0021`) and resume is blocked the same way. We hold a live connection, so we send the `END` ourselves rather than installing a watchdog on the printer.

**Files:**
- Modify: `src/printer/ams_backend_snapmaker.cpp` (`do_filament_batch` error callback, the `*_fail` path from Task 8)
- Test: `tests/unit/test_u1_batch_verification.cpp`

**Interfaces:**
- Consumes: `batch_plan()` from Task 8.
- Produces: `void end_firmware_batch();` - sends `AUTO_FEEDING_BATCH ACTION=END`, no-op when `use_batch_macro_` is false.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE_METHOD(MockBatchFixture, "A failed head ends the firmware batch", "[ams][batch]") {
    set_env("HELIX_MOCK_BATCH_FAIL_SLOT", "1");
    backend().load_filament_batch({0, 1});
    pump_until_idle();

    CHECK(sent_gcode_contains("AUTO_FEEDING_BATCH ACTION=END"));
}

TEST_CASE_METHOD(MockBatchFixture, "Without the macro nothing extra is sent", "[ams][batch]") {
    set_use_batch_macro(false);
    set_env("HELIX_MOCK_BATCH_FAIL_SLOT", "0");
    backend().load_filament_batch({0});
    pump_until_idle();

    CHECK_FALSE(sent_gcode_contains("AUTO_FEEDING_BATCH"));
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `make t F='[ams][batch]'`
Expected: FAIL - nothing sends a recovery `END`.

- [ ] **Step 3: Implement**

```cpp
void AmsBackendSnapmaker::end_firmware_batch() {
    if (!use_batch_macro_) {
        return; // no interlock exists on this firmware
    }
    execute_gcode("AUTO_FEEDING_BATCH ACTION=END");
}
```

Call it from the `*_fail` path and from the RPC error and timeout callbacks in `do_filament_batch`. The existing callbacks capture no `this`; give them a weak handle through the backend's existing async-lifetime mechanism rather than capturing `this` raw, and surface the failure with the head and state named.

- [ ] **Step 4: Run to verify it passes**

Run: `make t F='[ams][batch]'`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add src/printer/ams_backend_snapmaker.cpp include/ams_backend_snapmaker.h tests/unit/test_u1_batch_verification.cpp
git commit -m "fix(snapmaker): a failed batch clears the firmware's print interlock"
```

---

### Task 10: Clear a stranded interlock at connect

A batch interrupted by a crash, a restart or a lost connection leaves `doing` set with nothing to clear it. `auto_screws::reconcile_on_connect` is the existing precedent for firmware state that refuses unrelated filament operations until cleared; this is the same shape.

**Files:**
- Modify: `src/api/moonraker_discovery_sequence.cpp` (`complete_discovery_subscription`)
- Create: `src/printer/u1_batch_reconcile.cpp`, `include/u1_batch_reconcile.h`
- Test: `tests/unit/test_u1_batch_verification.cpp`

**Interfaces:**
- Produces: `namespace helix::u1_batch { void reconcile_on_connect(helix::IMoonrakerClient& client, const nlohmann::json& status); }`

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("A stranded batch interlock is cleared at connect", "[ams][batch]") {
    FakeClient client;
    const auto status = nlohmann::json::parse(
        R"({"gcode_macro AUTO_FEEDING_BATCH":{"doing":true},
            "print_stats":{"state":"standby"},"virtual_sdcard":{"is_active":false}})");

    helix::u1_batch::reconcile_on_connect(client, status);

    CHECK(client.sent_contains("AUTO_FEEDING_BATCH ACTION=END"));
}

TEST_CASE("A batch interlock during a print is left alone", "[ams][batch]") {
    FakeClient client;
    const auto status = nlohmann::json::parse(
        R"({"gcode_macro AUTO_FEEDING_BATCH":{"doing":true},
            "print_stats":{"state":"printing"},"virtual_sdcard":{"is_active":true}})");

    helix::u1_batch::reconcile_on_connect(client, status);

    CHECK_FALSE(client.sent_contains("AUTO_FEEDING_BATCH"));
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `make t F='[ams][batch]'`
Expected: FAIL - no such namespace.

- [ ] **Step 3: Implement**

```cpp
void helix::u1_batch::reconcile_on_connect(helix::IMoonrakerClient& client,
                                           const nlohmann::json& status) {
    const auto it = status.find("gcode_macro AUTO_FEEDING_BATCH");
    if (it == status.end() || !it->value("doing", false)) {
        return;
    }
    // A print in flight owns the interlock legitimately; ending the batch
    // underneath it would restore the wrong hotend targets.
    const std::string state = status.value("print_stats", nlohmann::json::object())
                                  .value("state", std::string{});
    if (state == "printing" || state == "paused" ||
        status.value("virtual_sdcard", nlohmann::json::object()).value("is_active", false)) {
        return;
    }
    spdlog::info("[U1Batch] Clearing a stranded AUTO_FEEDING_BATCH interlock");
    client.run_gcode("AUTO_FEEDING_BATCH ACTION=END");
}
```

Use whatever `IMoonrakerClient` actually calls its fire-and-forget gcode method; `run_gcode` is a placeholder for that real name - check `include/i_moonraker_client.h` and use the real one.

Call it from `complete_discovery_subscription` beside the screws-tilt reconcile, gated on `hw.has_auto_feeding_batch()`.

- [ ] **Step 4: Run to verify it passes**

Run: `make t F='[ams][batch]'`
Expected: PASS

- [ ] **Step 5: Full suite**

Run: `make full-test-run`
Expected: PASS. This is the completion gate and the only thing that runs the bats suite locally.

- [ ] **Step 6: Commit**

```bash
git add src/printer/u1_batch_reconcile.cpp include/u1_batch_reconcile.h src/api/moonraker_discovery_sequence.cpp tests/unit/test_u1_batch_verification.cpp
git commit -m "fix(snapmaker): clear a stranded batch interlock at connect"
```

---

## Hardware verification

Tasks 1-4 and 7-8 are verifiable on the rig at `192.168.30.103` on stock `1.5.2.13` - `filament_feed` and `channel_state` are present there. Tasks 5, 6, 9 and the `AUTO_FEEDING_BATCH` half of 10 need 1.6.x, which is a USB-stick firmware install at the printer.

Drive the picker with the pinned-socket recipe from `CLAUDE.md`:

```bash
export HELIX_SOCK=/tmp/helix-u1-batch-filament.sock
export HELIX_CONFIG_DIR=/tmp/helix-config-u1-batch-filament
mkdir -p "$HELIX_CONFIG_DIR"
./build/bin/helix-screen --moonraker ws://192.168.30.103:7125 -vv \
  --remote-socket "$HELIX_SOCK" > /tmp/helix-u1.log 2>&1 &
./build/bin/helix-screen ctl -s "$HELIX_SOCK" navigate ams
./build/bin/helix-screen ctl -s "$HELIX_SOCK" click btn_batch_filament
./build/bin/helix-screen ctl -s "$HELIX_SOCK" text batch_multiselect
```

The expected row text with the rig in its current state is heads 0 and 2 loaded, heads 1 and 3 ready to load, and only 0 and 2 pre-ticked. **Read-only until this point.** Anything that actually feeds filament moves the machine and needs Preston's confirmation each time.

## Self-review

**Spec coverage.** Capability gate → Task 5, 6. Eligibility table → Task 3, surfaced in Task 4. Execution sequence → Task 6. Verification → Task 8. Progress → Task 8. Recovery → Task 9. Connect-time reconcile → Task 10. Mock prerequisite → Task 7. Cooldown needs no task: `ACTION=END` owns it and `PostOpCooldownManager` skips when the target is already zero.

**One addition beyond the spec.** Task 1 was not in the design doc. The `can_unload_from_toolhead()` seam already existed and was already correct on Snapmaker, so the confirmed pre-tick bug is a call-site fix that ships independently of everything else. It is first because it is the smallest correct thing.

**One deliberate deviation.** The spec said "dialect accessor"; Task 5 uses a `has_*` bool. A dialect enum is right when two modules compete for one capability, which is the `screws_tilt_dialect()` case. Presence or absence is a bool.

**Known soft spots**, to resolve during execution rather than by guessing now:
- Task 3 assumes the motion sensor's `enabled` reaches the backend. If it does not, extend `ChannelSnapshot` and parse it in the sensor branch, as the task says.
- Task 9 says to give the RPC callbacks a weak handle through the backend's existing async-lifetime mechanism. Use whatever `AmsBackendSnapmaker` already uses for deferred work; do not introduce a new one.
- Task 10's `client.run_gcode` is a placeholder for the real `IMoonrakerClient` method name.
