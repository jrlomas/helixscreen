# Snapmaker U1 Remaining Firmware Surface — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Expose the seven remaining stock-firmware capabilities on the Snapmaker U1 — four `SET_PRINT_PREFERENCES` settings, structured error reporting, per-filament temperatures, and the power-loss sensor — with no vendor names in generic code.

**Architecture:** Four of the settings share one mechanism: read from the `print_task_config` status object, write with `SET_PRINT_PREFERENCES`. Task 1 builds that read/write pair once; Tasks 2-5 each add a `DeviceAction` that calls it, which the existing `AmsDeviceSectionDetailOverlay` renders with no UI work. Tasks 6-7 move Snapmaker error classification off phrase matching onto the firmware's `exception_manager` codes, through the `AmsBackend::classify_error` hook that already exists and Snapmaker does not yet override. Tasks 8-9 are independent reads of two objects nothing currently consumes.

**Tech Stack:** C++20, LVGL 9.5, Catch2, `hv/json.hpp` (libhv's bundled nlohmann), Moonraker JSON-RPC over WebSocket.

**Spec:** `docs/devel/plans/2026-09-21-u1-per-print-preferences-design.md` (the preceding design; its firmware model, the `SET_PRINT_PREFERENCES` field table and the eleven-field placement table are the source for Tasks 1-5).

## Global Constraints

- **Vendor names live in ONE module per capability.** `.claude/rules/vendor-abstraction.md`. Generic code asks a capability question. `AmsBackendSnapmaker` is a legitimate vendor module; `PrinterState`, panels and the subscription builder are not.
- **Never touch LVGL from a background thread.** Status frames arrive on the WebSocket thread. Route through `ui_queue_update()` or `async_lifetime_.defer()`. `.claude/rules/threading.md`.
- **Moonraker sends DELTA frames.** A field absent from a frame is silent, never false. Merge, never replace. See `reference_moonraker_delta_frames_wipe_struct_state`.
- **`SET_PRINT_PREFERENCES` is a setter.** An omitted parameter leaves the stored value unchanged; only send parameters you intend to change.
- **A mid-print guard** refuses `BED_LEVEL`, `FLOW_CALIBRATE`, `SHAPER_CALIBRATE`, `TIME_LAPSE_CAMERA` and `END_UNLOAD_FILAMENT` while printing or paused unless `FORCE=1`. Do not pass `FORCE`. Expect and surface the refusal (exception id 531).
- **spdlog only**, SPDX headers, no RTTI, `#include "hv/json.hpp"`.
- **Every behaviour change needs a test that fails when the behaviour is removed**, and one line in the commit body naming the mutation that proved it.
- **Commit body:** subject plus ~4 lines. No Tests/Verification/Mutation essay.
- **Test tags** go on every new `TEST_CASE`; run with `make t F='[tag]'`.

---

## File Structure

| File | Responsibility |
|---|---|
| `include/snapmaker_print_preferences.h` *(new)* | Typed read of the U1's stored preferences out of a `print_task_config` frame, and construction of the `SET_PRINT_PREFERENCES` line that writes them. Pure; no I/O, no LVGL. |
| `src/printer/snapmaker_print_preferences.cpp` *(new)* | Implementation of the above. |
| `tests/unit/test_snapmaker_print_preferences.cpp` *(new)* | Parse/render coverage including delta-silence and the setter semantics. |
| `include/ams_backend_snapmaker.h` *(modify)* | Hold last-seen preferences; declare the `DeviceAction` overrides and `classify_error`. |
| `src/printer/ams_backend_snapmaker.cpp` *(modify)* | Parse preferences in `handle_status_update`; expose them as `DeviceAction`s; execute writes; classify errors from structured codes. |
| `include/snapmaker_exceptions.h` *(new)* | Decode `level-id-index-code` strings and map known `(id, index, code)` triples to user-facing text. |
| `src/printer/snapmaker_exceptions.cpp` *(new)* | The code table. |
| `tests/unit/test_snapmaker_exceptions.cpp` *(new)* | Decoding, table lookups, and unknown-code fallback. |
| `src/api/moonraker_discovery_sequence.cpp` *(modify)* | Subscribe `exception_manager` when the printer reports it. |
| `include/filament_temperature_source.h` *(new)* | Capability question: does this firmware publish per-filament temperatures, and what are they for a given spool. |
| `src/printer/filament_temperature_source.cpp` *(new)* | Provider table; Snapmaker row queries `FILAMENT_PARA_GET_ALL_INFO`. |
| `tests/unit/test_filament_temperature_source.cpp` *(new)* | Provider detection, lookup, and absence behaviour. |
| `include/power_loss_sensor.h` *(new)* | Capability question: does this firmware expose a live power-loss sensor, and is it currently asserting. |
| `src/printer/power_loss_sensor.cpp` *(new)* | Provider table; Snapmaker row reads `power_loss_check`. |
| `tests/unit/test_power_loss_sensor.cpp` *(new)* | Reads, unknown-objects guard, assertion threshold. |

---

## Task 1: The preference read/write pair

Four settings share one mechanism. Build it once, with no UI and no backend wiring, so Tasks 2-5 are each a handful of lines.

**Files:**
- Create: `include/snapmaker_print_preferences.h`
- Create: `src/printer/snapmaker_print_preferences.cpp`
- Create: `tests/unit/test_snapmaker_print_preferences.cpp`
- Modify: `firmware/helixscreen-esp32/components/helixapp/app_srcs.txt` (add the new .cpp, sorted, beside `src/printer/pre_print_preferences.cpp`)

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `struct helix::snapmaker::PrintPreferences { std::optional<bool> auto_replenish, replenish_ignore_color, filament_entangle_detect, end_led_turn_off; std::optional<std::string> filament_entangle_sen; std::vector<bool> end_unload_filament; }`
  - `PrintPreferences read_print_preferences(const nlohmann::json& status)`
  - `std::string write_print_preferences_gcode(const PrintPreferences& changes)`

- [ ] **Step 1: Write the failing test**

```cpp
// tests/unit/test_snapmaker_print_preferences.cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "snapmaker_print_preferences.h"
#include "../catch_amalgamated.hpp"

using namespace helix::snapmaker;

namespace {
nlohmann::json ptc(const nlohmann::json& fields) {
    nlohmann::json s = nlohmann::json::object();
    s["print_task_config"] = fields;
    return s;
}
} // namespace

TEST_CASE("snapmaker prefs: every field reads out of a full frame", "[snapmaker][prefs]") {
    auto p = read_print_preferences(ptc({
        {"auto_replenish_filament", true},
        {"replenish_ignore_color", false},
        {"filament_entangle_detect", true},
        {"filament_entangle_sen", "medium"},
        {"end_led_turn_off", true},
        {"end_unload_filament", {false, true, false, true}},
    }));
    REQUIRE(p.auto_replenish.value() == true);
    REQUIRE(p.replenish_ignore_color.value() == false);
    REQUIRE(p.filament_entangle_detect.value() == true);
    REQUIRE(p.filament_entangle_sen.value() == "medium");
    REQUIRE(p.end_led_turn_off.value() == true);
    REQUIRE(p.end_unload_filament == std::vector<bool>{false, true, false, true});
}

TEST_CASE("snapmaker prefs: a field the frame omits stays nullopt", "[snapmaker][prefs]") {
    // Moonraker sends deltas. Silence is not an off.
    auto p = read_print_preferences(ptc({{"end_led_turn_off", true}}));
    REQUIRE(p.end_led_turn_off.value() == true);
    REQUIRE_FALSE(p.auto_replenish.has_value());
    REQUIRE_FALSE(p.filament_entangle_sen.has_value());
    REQUIRE(p.end_unload_filament.empty());
}

TEST_CASE("snapmaker prefs: a frame without print_task_config yields nothing",
          "[snapmaker][prefs]") {
    nlohmann::json other = nlohmann::json::object();
    other["toolhead"] = {{"homed_axes", "xyz"}};
    auto p = read_print_preferences(other);
    REQUIRE_FALSE(p.auto_replenish.has_value());
}

TEST_CASE("snapmaker prefs: integer and boolean spellings both read", "[snapmaker][prefs]") {
    auto p = read_print_preferences(ptc({{"auto_replenish_filament", 1},
                                         {"filament_entangle_detect", 0}}));
    REQUIRE(p.auto_replenish.value() == true);
    REQUIRE(p.filament_entangle_detect.value() == false);
}

TEST_CASE("snapmaker prefs: the write sends ONLY what changed", "[snapmaker][prefs]") {
    // SET_PRINT_PREFERENCES is a setter: an omitted parameter keeps its stored
    // value, so sending untouched fields would rewrite settings the user did
    // not ask to change.
    PrintPreferences changes;
    changes.filament_entangle_detect = true;
    const std::string g = write_print_preferences_gcode(changes);
    REQUIRE(g == "SET_PRINT_PREFERENCES FILAMENT_ENTANGLE_DETECT=1");
}

TEST_CASE("snapmaker prefs: booleans render as 1/0 and the enum renders bare",
          "[snapmaker][prefs]") {
    PrintPreferences changes;
    changes.auto_replenish = false;
    changes.filament_entangle_sen = "high";
    const std::string g = write_print_preferences_gcode(changes);
    REQUIRE(g.find("AUTO_REPLENISH_FILAMENT=0") != std::string::npos);
    REQUIRE(g.find("FILAMENT_ENTANGLE_SEN=high") != std::string::npos);
}

TEST_CASE("snapmaker prefs: the per-tool array renders as a comma list",
          "[snapmaker][prefs]") {
    PrintPreferences changes;
    changes.end_unload_filament = {true, false, true, false};
    REQUIRE(write_print_preferences_gcode(changes) ==
            "SET_PRINT_PREFERENCES END_UNLOAD_FILAMENT=1,0,1,0");
}

TEST_CASE("snapmaker prefs: nothing to change renders an empty string",
          "[snapmaker][prefs]") {
    // The caller must be able to tell "no write needed" from "write this",
    // rather than sending a bare command that sets nothing.
    REQUIRE(write_print_preferences_gcode(PrintPreferences{}).empty());
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `make t F='[snapmaker][prefs]'`
Expected: compile failure — `snapmaker_print_preferences.h` does not exist.

- [ ] **Step 3: Write the header**

```cpp
// include/snapmaker_print_preferences.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "hv/json.hpp"

#include <optional>
#include <string>
#include <vector>

/**
 * @file snapmaker_print_preferences.h
 * @brief The U1's stored print preferences: how to read them, how to write them.
 *
 * `print_task_config` holds settings the firmware keeps across prints and
 * reboots, and the gcode the slicer emits is gated on them. `SET_PRINT_PREFERENCES`
 * is the setter, and it is a SETTER: a parameter it is not given keeps its
 * stored value. So a write must carry only the fields that changed, and a read
 * must distinguish "false" from "the frame did not mention it".
 *
 * Pure: no I/O, no LVGL, no Moonraker. The backend owns the plumbing.
 */
namespace helix::snapmaker {

/// Every field optional, because a delta frame is silent about what it omits.
/// `end_unload_filament` is empty rather than nullopt for the same reason.
struct PrintPreferences {
    std::optional<bool> auto_replenish;
    std::optional<bool> replenish_ignore_color;
    std::optional<bool> filament_entangle_detect;
    std::optional<bool> end_led_turn_off;
    std::optional<std::string> filament_entangle_sen; ///< "low" | "medium" | "high"
    std::vector<bool> end_unload_filament;            ///< one per toolhead

    [[nodiscard]] bool empty() const;
};

/// Pull whatever this status frame says about the stored preferences.
/// Fields the frame omits are left unset; nothing is inferred.
[[nodiscard]] PrintPreferences read_print_preferences(const nlohmann::json& status);

/// The one-line command that writes exactly the fields `changes` sets.
/// Empty string when `changes` sets none, so the caller can skip the send.
[[nodiscard]] std::string write_print_preferences_gcode(const PrintPreferences& changes);

} // namespace helix::snapmaker
```

- [ ] **Step 4: Write the implementation**

```cpp
// src/printer/snapmaker_print_preferences.cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "snapmaker_print_preferences.h"

namespace helix::snapmaker {
namespace {

/// A firmware storing a flag as a Python bool serialises true/false; one
/// storing it as an int serialises 0/1. Anything else is not a setting we can
/// read, and is left unset rather than guessed at.
std::optional<bool> read_flag(const nlohmann::json& obj, const char* key) {
    auto it = obj.find(key);
    if (it == obj.end()) {
        return std::nullopt;
    }
    if (it->is_boolean()) {
        return it->get<bool>();
    }
    if (it->is_number_integer()) {
        return it->get<int>() != 0;
    }
    return std::nullopt;
}

void append(std::string& out, const char* name, const std::string& value) {
    out += (out.empty() ? "SET_PRINT_PREFERENCES " : " ");
    out += name;
    out += '=';
    out += value;
}

} // namespace

bool PrintPreferences::empty() const {
    return !auto_replenish && !replenish_ignore_color && !filament_entangle_detect &&
           !end_led_turn_off && !filament_entangle_sen && end_unload_filament.empty();
}

PrintPreferences read_print_preferences(const nlohmann::json& status) {
    PrintPreferences p;
    if (!status.is_object()) {
        return p;
    }
    auto ptc = status.find("print_task_config");
    if (ptc == status.end() || !ptc->is_object()) {
        return p;
    }
    p.auto_replenish = read_flag(*ptc, "auto_replenish_filament");
    p.replenish_ignore_color = read_flag(*ptc, "replenish_ignore_color");
    p.filament_entangle_detect = read_flag(*ptc, "filament_entangle_detect");
    p.end_led_turn_off = read_flag(*ptc, "end_led_turn_off");

    if (auto it = ptc->find("filament_entangle_sen");
        it != ptc->end() && it->is_string()) {
        p.filament_entangle_sen = it->get<std::string>();
    }
    if (auto it = ptc->find("end_unload_filament"); it != ptc->end() && it->is_array()) {
        for (const auto& v : *it) {
            if (v.is_boolean()) {
                p.end_unload_filament.push_back(v.get<bool>());
            } else if (v.is_number_integer()) {
                p.end_unload_filament.push_back(v.get<int>() != 0);
            }
        }
    }
    return p;
}

std::string write_print_preferences_gcode(const PrintPreferences& changes) {
    std::string out;
    if (changes.auto_replenish) {
        append(out, "AUTO_REPLENISH_FILAMENT", *changes.auto_replenish ? "1" : "0");
    }
    if (changes.replenish_ignore_color) {
        append(out, "REPLENISH_IGNORE_COLOR", *changes.replenish_ignore_color ? "1" : "0");
    }
    if (changes.filament_entangle_detect) {
        append(out, "FILAMENT_ENTANGLE_DETECT", *changes.filament_entangle_detect ? "1" : "0");
    }
    if (changes.end_led_turn_off) {
        append(out, "END_LED_TURN_OFF", *changes.end_led_turn_off ? "1" : "0");
    }
    if (changes.filament_entangle_sen) {
        append(out, "FILAMENT_ENTANGLE_SEN", *changes.filament_entangle_sen);
    }
    if (!changes.end_unload_filament.empty()) {
        std::string csv;
        for (size_t i = 0; i < changes.end_unload_filament.size(); ++i) {
            csv += (i ? "," : "");
            csv += changes.end_unload_filament[i] ? "1" : "0";
        }
        append(out, "END_UNLOAD_FILAMENT", csv);
    }
    return out;
}

} // namespace helix::snapmaker
```

- [ ] **Step 5: Add the file to the ESP32 build decision**

The gate `FAIL: N src/ file(s) not decided for the ESP32 firmware build` fires on any new `src/` file. This one belongs in the v1 cut, beside its sibling.

```bash
# add the bare path, in sorted position, after src/printer/pre_print_preferences.cpp
$EDITOR firmware/helixscreen-esp32/components/helixapp/app_srcs.txt
```

- [ ] **Step 6: Run the tests and watch them pass**

Run: `make t F='[snapmaker][prefs]'`
Expected: PASS, 8 test cases.

- [ ] **Step 7: Prove a test can fail**

Change `append` to always emit every field regardless of whether it is set, run `make t F='[snapmaker][prefs]'`, confirm "the write sends ONLY what changed" goes red, then revert.

- [ ] **Step 8: Commit**

```bash
git commit -m "feat(snapmaker): read and write the U1's stored print preferences" \
  -m "SET_PRINT_PREFERENCES is a setter, so a write carries only changed fields and a read distinguishes false from a delta frame's silence. Pure helper; no backend wiring yet." \
  -m "Mutation: emitting every field unconditionally turns [snapmaker][prefs] red." \
  -- include/snapmaker_print_preferences.h src/printer/snapmaker_print_preferences.cpp \
     tests/unit/test_snapmaker_print_preferences.cpp \
     firmware/helixscreen-esp32/components/helixapp/app_srcs.txt
```

---

## Task 2: Hold the preferences in the Snapmaker backend

Parse them on every status frame and keep them, so Tasks 3-6 have something to render. Merging, not replacing, because the frames are deltas.

**Files:**
- Modify: `include/ams_backend_snapmaker.h`
- Modify: `src/printer/ams_backend_snapmaker.cpp#AmsBackendSnapmaker::handle_status_update`
- Test: `tests/unit/test_ams_backend_snapmaker_prefs.cpp` *(new)*

**Interfaces:**
- Consumes: `helix::snapmaker::read_print_preferences`, `PrintPreferences` (Task 1).
- Produces: `const helix::snapmaker::PrintPreferences& AmsBackendSnapmaker::print_preferences() const`.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/unit/test_ams_backend_snapmaker_prefs.cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ams_backend_snapmaker.h"
#include "../catch_amalgamated.hpp"

namespace {
nlohmann::json frame(const nlohmann::json& ptc_fields) {
    nlohmann::json params = nlohmann::json::object();
    params["print_task_config"] = ptc_fields;
    return params;
}
} // namespace

TEST_CASE("snapmaker backend keeps the preferences it is told about",
          "[ams][snapmaker][prefs]") {
    AmsBackendSnapmaker backend;
    backend.handle_status_update(frame({{"end_led_turn_off", true},
                                        {"filament_entangle_sen", "low"}}));
    REQUIRE(backend.print_preferences().end_led_turn_off.value() == true);
    REQUIRE(backend.print_preferences().filament_entangle_sen.value() == "low");
}

TEST_CASE("a later frame that omits a preference does not clear it",
          "[ams][snapmaker][prefs]") {
    // The delta trap: a frame carrying only filament colours says nothing about
    // the preferences, and replacing rather than merging would switch them off.
    AmsBackendSnapmaker backend;
    backend.handle_status_update(frame({{"end_led_turn_off", true}}));
    backend.handle_status_update(frame({{"filament_type", {"PLA"}}}));
    REQUIRE(backend.print_preferences().end_led_turn_off.value() == true);
}

TEST_CASE("a later frame that changes a preference wins", "[ams][snapmaker][prefs]") {
    AmsBackendSnapmaker backend;
    backend.handle_status_update(frame({{"end_led_turn_off", true}}));
    backend.handle_status_update(frame({{"end_led_turn_off", false}}));
    REQUIRE(backend.print_preferences().end_led_turn_off.value() == false);
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `make t F='[ams][snapmaker][prefs]'`
Expected: compile failure — no member `print_preferences`.

- [ ] **Step 3: Declare the member and accessor**

In `include/ams_backend_snapmaker.h`, add the include and, in the private section beside the other `print_task_config` mirrors (`extruder_map_table_`, `extruders_used_`):

```cpp
#include "snapmaker_print_preferences.h"
```

```cpp
    /// What the firmware last reported for its stored print preferences.
    /// Merged across frames: Moonraker sends deltas, so a frame that omits a
    /// setting is silent about it rather than reporting it off.
    helix::snapmaker::PrintPreferences print_preferences_;
```

and in the public section:

```cpp
    [[nodiscard]] const helix::snapmaker::PrintPreferences& print_preferences() const {
        return print_preferences_;
    }
```

- [ ] **Step 4: Merge on every frame**

In `src/printer/ams_backend_snapmaker.cpp#AmsBackendSnapmaker::handle_status_update`, beside the existing `print_task_config` parse:

```cpp
    // Firmware-stored preferences. Merged field by field, because a delta frame
    // that mentions one setting says nothing about the others.
    const auto incoming = helix::snapmaker::read_print_preferences(status);
    if (incoming.auto_replenish) {
        print_preferences_.auto_replenish = incoming.auto_replenish;
    }
    if (incoming.replenish_ignore_color) {
        print_preferences_.replenish_ignore_color = incoming.replenish_ignore_color;
    }
    if (incoming.filament_entangle_detect) {
        print_preferences_.filament_entangle_detect = incoming.filament_entangle_detect;
    }
    if (incoming.end_led_turn_off) {
        print_preferences_.end_led_turn_off = incoming.end_led_turn_off;
    }
    if (incoming.filament_entangle_sen) {
        print_preferences_.filament_entangle_sen = incoming.filament_entangle_sen;
    }
    if (!incoming.end_unload_filament.empty()) {
        print_preferences_.end_unload_filament = incoming.end_unload_filament;
    }
```

Note: `read_print_preferences` expects the object that CONTAINS `print_task_config`. `handle_status_update` unwraps `notify_status_update` params first — pass whatever local variable already holds the unwrapped status object at the existing parse site, not the raw argument.

- [ ] **Step 5: Run the tests and watch them pass**

Run: `make t F='[ams][snapmaker][prefs]'`
Expected: PASS, 3 test cases.

- [ ] **Step 6: Prove a test can fail**

Replace the merge block with `print_preferences_ = incoming;`, run the tag, confirm "a later frame that omits a preference does not clear it" goes red, then revert.

- [ ] **Step 7: Commit**

```bash
git commit -m "feat(snapmaker): the backend keeps the U1's stored print preferences" \
  -m "Parsed on every status frame and merged field by field. Moonraker sends deltas, so replacing would clear settings a frame simply did not mention." \
  -m "Mutation: replacing the merge with assignment turns [ams][snapmaker][prefs] red." \
  -- include/ams_backend_snapmaker.h src/printer/ams_backend_snapmaker.cpp \
     tests/unit/test_ams_backend_snapmaker_prefs.cpp
```

---

## Task 3: Surface all six settings as DeviceActions

`AmsDeviceSectionDetailOverlay` already renders `DeviceAction`s generically as BUTTON / TOGGLE / SLIDER / DROPDOWN / INFO, so every one of these is data, not UI code. The three-way sensitivity is a DROPDOWN; the per-toolhead array is one TOGGLE per tool with a distinct `action_id`.

**Files:**
- Modify: `include/ams_backend_snapmaker.h` (declare the three overrides)
- Modify: `src/printer/ams_backend_snapmaker.cpp`
- Test: `tests/unit/test_ams_backend_snapmaker_actions.cpp` *(new)*
- Modify: `translations/*.yml` (9 files) then regenerate

**Interfaces:**
- Consumes: `AmsBackendSnapmaker::print_preferences()` (Task 2); `write_print_preferences_gcode` (Task 1); `DeviceSection`, `DeviceAction`, `ActionType` from `include/ams_types.h`.
- Produces: action ids `snapmaker_auto_replenish`, `snapmaker_replenish_ignore_color`, `snapmaker_entangle_detect`, `snapmaker_entangle_sen`, `snapmaker_end_led_off`, `snapmaker_end_unload_t<N>`.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/unit/test_ams_backend_snapmaker_actions.cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ams_backend_snapmaker.h"
#include "../catch_amalgamated.hpp"

#include <algorithm>

namespace {
nlohmann::json frame(const nlohmann::json& ptc) {
    nlohmann::json p = nlohmann::json::object();
    p["print_task_config"] = ptc;
    return p;
}
const DeviceAction* find_action(const std::vector<DeviceAction>& as, const std::string& id) {
    auto it = std::find_if(as.begin(), as.end(),
                           [&](const DeviceAction& a) { return a.action_id == id; });
    return it == as.end() ? nullptr : &(*it);
}
} // namespace

TEST_CASE("snapmaker exposes its firmware settings as device actions",
          "[ams][snapmaker][actions]") {
    AmsBackendSnapmaker backend;
    backend.handle_status_update(frame({{"auto_replenish_filament", true},
                                        {"filament_entangle_detect", false},
                                        {"filament_entangle_sen", "medium"},
                                        {"end_led_turn_off", true}}));
    const auto actions = backend.get_device_actions();

    const DeviceAction* rep = find_action(actions, "snapmaker_auto_replenish");
    REQUIRE(rep != nullptr);
    REQUIRE(rep->type == ActionType::TOGGLE);
    REQUIRE(std::any_cast<bool>(rep->current_value) == true);

    const DeviceAction* sen = find_action(actions, "snapmaker_entangle_sen");
    REQUIRE(sen != nullptr);
    REQUIRE(sen->type == ActionType::DROPDOWN);
    REQUIRE(sen->options == std::vector<std::string>{"low", "medium", "high"});
    REQUIRE(std::any_cast<std::string>(sen->current_value) == "medium");
}

TEST_CASE("a setting the firmware has not reported is not offered",
          "[ams][snapmaker][actions]") {
    // Offering a toggle whose state we do not know would render it off and
    // invite the user to "change" it to the value it already has.
    AmsBackendSnapmaker backend;
    const auto actions = backend.get_device_actions();
    REQUIRE(find_action(actions, "snapmaker_auto_replenish") == nullptr);
}

TEST_CASE("end-unload offers one toggle per reported toolhead",
          "[ams][snapmaker][actions]") {
    AmsBackendSnapmaker backend;
    backend.handle_status_update(frame({{"end_unload_filament", {true, false, false, true}}}));
    const auto actions = backend.get_device_actions();
    REQUIRE(find_action(actions, "snapmaker_end_unload_t0") != nullptr);
    REQUIRE(find_action(actions, "snapmaker_end_unload_t3") != nullptr);
    REQUIRE(find_action(actions, "snapmaker_end_unload_t4") == nullptr);
    REQUIRE(std::any_cast<bool>(
                find_action(actions, "snapmaker_end_unload_t0")->current_value) == true);
    REQUIRE(std::any_cast<bool>(
                find_action(actions, "snapmaker_end_unload_t1")->current_value) == false);
}

TEST_CASE("executing a toggle emits only that field", "[ams][snapmaker][actions]") {
    AmsBackendSnapmaker backend;
    backend.handle_status_update(frame({{"end_led_turn_off", false}}));
    const std::string g = backend.build_preference_gcode("snapmaker_end_led_off",
                                                         std::any(true));
    REQUIRE(g == "SET_PRINT_PREFERENCES END_LED_TURN_OFF=1");
}

TEST_CASE("executing the per-tool toggle rewrites the whole array",
          "[ams][snapmaker][actions]") {
    // END_UNLOAD_FILAMENT takes the full list, so flipping one tool must send
    // the others at their current values or they are cleared.
    AmsBackendSnapmaker backend;
    backend.handle_status_update(frame({{"end_unload_filament", {true, false, true, false}}}));
    const std::string g = backend.build_preference_gcode("snapmaker_end_unload_t1",
                                                         std::any(true));
    REQUIRE(g == "SET_PRINT_PREFERENCES END_UNLOAD_FILAMENT=1,1,1,0");
}

TEST_CASE("an unknown action id produces no gcode", "[ams][snapmaker][actions]") {
    AmsBackendSnapmaker backend;
    REQUIRE(backend.build_preference_gcode("not_a_real_action", std::any(true)).empty());
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `make t F='[ams][snapmaker][actions]'`
Expected: compile failure — no `build_preference_gcode`.

- [ ] **Step 3: Declare the overrides**

In `include/ams_backend_snapmaker.h` public section:

```cpp
    std::vector<DeviceSection> get_device_sections() const override;
    std::vector<DeviceAction> get_device_actions() const override;
    bool execute_device_action(const std::string& action_id, const std::any& value) override;

    /// The command one action produces, or empty when the id is not ours.
    /// Separated from execute_device_action so the mapping is testable without
    /// a Moonraker client.
    [[nodiscard]] std::string build_preference_gcode(const std::string& action_id,
                                                     const std::any& value) const;
```

Check the base signatures in `include/ams_backend.h` and match them exactly, including constness.

- [ ] **Step 4: Implement**

```cpp
// src/printer/ams_backend_snapmaker.cpp

std::vector<DeviceSection> AmsBackendSnapmaker::get_device_sections() const {
    if (print_preferences_.empty()) {
        return {}; // nothing reported yet - an empty section is worse than none
    }
    DeviceSection s;
    s.section_id = "snapmaker_print_prefs";
    s.display_name = lv_tr("Print Behaviour");
    s.display_order = 50;
    return {s};
}

std::vector<DeviceAction> AmsBackendSnapmaker::get_device_actions() const {
    std::vector<DeviceAction> out;
    const auto& p = print_preferences_;

    auto add_toggle = [&](const char* id, const char* label, bool value, int order) {
        DeviceAction a;
        a.action_id = id;
        a.section_id = "snapmaker_print_prefs";
        a.display_name = lv_tr(label);
        a.type = ActionType::TOGGLE;
        a.current_value = value;
        a.display_order = order;
        out.push_back(std::move(a));
    };

    if (p.auto_replenish) {
        add_toggle("snapmaker_auto_replenish", "Auto-replenish filament",
                   *p.auto_replenish, 10);
    }
    if (p.replenish_ignore_color) {
        add_toggle("snapmaker_replenish_ignore_color", "Replenish ignores colour",
                   *p.replenish_ignore_color, 20);
    }
    if (p.filament_entangle_detect) {
        add_toggle("snapmaker_entangle_detect", "Detect filament tangles",
                   *p.filament_entangle_detect, 30);
    }
    if (p.filament_entangle_sen) {
        DeviceAction a;
        a.action_id = "snapmaker_entangle_sen";
        a.section_id = "snapmaker_print_prefs";
        a.display_name = lv_tr("Tangle sensitivity");
        a.type = ActionType::DROPDOWN;
        a.options = {"low", "medium", "high"};
        a.current_value = *p.filament_entangle_sen;
        a.display_order = 40;
        out.push_back(std::move(a));
    }
    if (p.end_led_turn_off) {
        add_toggle("snapmaker_end_led_off", "Turn LED off when the print ends",
                   *p.end_led_turn_off, 50);
    }
    for (size_t t = 0; t < p.end_unload_filament.size(); ++t) {
        DeviceAction a;
        a.action_id = "snapmaker_end_unload_t" + std::to_string(t);
        a.section_id = "snapmaker_print_prefs";
        a.display_name = std::string(lv_tr("Unload at end")) + " - T" + std::to_string(t);
        a.type = ActionType::TOGGLE;
        a.current_value = static_cast<bool>(p.end_unload_filament[t]);
        a.slot_index = static_cast<int>(t);
        a.display_order = 60 + static_cast<int>(t);
        out.push_back(std::move(a));
    }
    return out;
}

std::string AmsBackendSnapmaker::build_preference_gcode(const std::string& action_id,
                                                        const std::any& value) const {
    helix::snapmaker::PrintPreferences changes;

    if (action_id == "snapmaker_auto_replenish") {
        changes.auto_replenish = std::any_cast<bool>(value);
    } else if (action_id == "snapmaker_replenish_ignore_color") {
        changes.replenish_ignore_color = std::any_cast<bool>(value);
    } else if (action_id == "snapmaker_entangle_detect") {
        changes.filament_entangle_detect = std::any_cast<bool>(value);
    } else if (action_id == "snapmaker_end_led_off") {
        changes.end_led_turn_off = std::any_cast<bool>(value);
    } else if (action_id == "snapmaker_entangle_sen") {
        changes.filament_entangle_sen = std::any_cast<std::string>(value);
    } else if (action_id.rfind("snapmaker_end_unload_t", 0) == 0) {
        // The firmware takes the whole list, so the untouched tools travel at
        // their current values or they are cleared.
        const size_t tool = static_cast<size_t>(
            std::stoul(action_id.substr(std::string("snapmaker_end_unload_t").size())));
        auto list = print_preferences_.end_unload_filament;
        if (tool >= list.size()) {
            return {};
        }
        list[tool] = std::any_cast<bool>(value);
        changes.end_unload_filament = std::move(list);
    } else {
        return {};
    }
    return helix::snapmaker::write_print_preferences_gcode(changes);
}

bool AmsBackendSnapmaker::execute_device_action(const std::string& action_id,
                                                const std::any& value) {
    const std::string gcode = build_preference_gcode(action_id, value);
    if (gcode.empty()) {
        return false;
    }
    // The mid-print guard refuses END_UNLOAD_FILAMENT while printing or paused
    // unless FORCE=1, which we deliberately do not pass. The refusal arrives as
    // an exception and is classified in Task 5 rather than swallowed here.
    send_gcode(gcode);
    return true;
}
```

Use whatever gcode-send helper this backend already has; `send_gcode` above is a placeholder for the existing one — grep the file for how `SET_PRINT_EXTRUDER_MAP` is sent and match it.

- [ ] **Step 5: Add the translation keys**

Add these to all nine `translations/*.yml` in sorted position — English maps the key to itself, the other eight get `''`:
`Print Behaviour`, `Auto-replenish filament`, `Replenish ignores colour`, `Detect filament tangles`, `Tangle sensitivity`, `Turn LED off when the print ends`, `Unload at end`.

`make translation-sync` will NOT find these — it scans `ui_xml/` and `src/` for `lv_tr(...)` with literal arguments, and these are literals inside a lambda call, so verify by grepping `translations/en.yml` after running it. Then `make translations` to regenerate `ui_xml/translations/*.xml`, and stage both the YAMLs and the generated XML.

- [ ] **Step 6: Run the tests and watch them pass**

Run: `make t F='[ams][snapmaker][actions]'`
Expected: PASS, 6 test cases.

- [ ] **Step 7: Prove a test can fail**

In the `snapmaker_end_unload_t` branch, drop the copy of the current list and set only the one tool (`changes.end_unload_filament = {std::any_cast<bool>(value)};`). Run the tag, confirm "executing the per-tool toggle rewrites the whole array" goes red, then revert.

- [ ] **Step 8: Commit**

```bash
git commit -m "feat(snapmaker): expose the U1's print-behaviour settings on the AMS surface" \
  -m "Six settings as DeviceActions, which AmsDeviceSectionDetailOverlay already renders generically - a dropdown for tangle sensitivity, one toggle per toolhead for end-unload. Each write carries only its own field, except the per-tool array which the firmware takes whole." \
  -m "Mutation: sending one tool instead of the whole end-unload list turns [ams][snapmaker][actions] red." \
  -- include/ams_backend_snapmaker.h src/printer/ams_backend_snapmaker.cpp \
     tests/unit/test_ams_backend_snapmaker_actions.cpp translations/ ui_xml/translations/
```

- [ ] **Step 9: Verify on hardware**

Ask Preston before pointing anything at the printer. Then:

```bash
export HELIX_SOCK=/tmp/helix-u1.sock HELIX_CONFIG_DIR=/tmp/helix-config-u1
mkdir -p "$HELIX_CONFIG_DIR"
SDL_VIDEODRIVER=dummy HELIX_LOG_DEST=console \
  ./build/bin/helix-screen --moonraker ws://192.168.30.103:7125 -vv \
  --remote-socket "$HELIX_SOCK" > /tmp/u1.log 2>&1 &
```

Navigate to the AMS device-operations overlay, open the Print Behaviour section, and confirm with `ctl text` / `ctl state` that each row shows what `curl .../printer/objects/query?print_task_config` reports. Toggle one, re-read the firmware, and set it back. Record the before values first and restore them.

---

## Task 4: Decode the firmware's structured exception codes

`exception_manager` raises `{id, index, code, message, level, oneshot, is_persistent}`, and codes appear in error text as `level-id-index-code` (e.g. `0003-0530-0000-0011` = level 3, module 530, index 0, code 11 — the plate-removal refusal). Levels are `1=none, 2=pause, 3=cancel`. Decode it once, in one place.

**Files:**
- Create: `include/snapmaker_exceptions.h`
- Create: `src/printer/snapmaker_exceptions.cpp`
- Create: `tests/unit/test_snapmaker_exceptions.cpp`
- Modify: `firmware/helixscreen-esp32/components/helixapp/app_srcs.txt`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `struct helix::snapmaker::ExceptionCode { int level, id, index, code; }`
  - `std::optional<ExceptionCode> decode_exception_code(const std::string& text)`
  - `std::string_view exception_message(const ExceptionCode& c)` — empty when unknown
  - `enum class ExceptionSeverity { Informational, Pause, Cancel }` + `ExceptionSeverity severity_of(int level)`

- [ ] **Step 1: Write the failing test**

```cpp
// tests/unit/test_snapmaker_exceptions.cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "snapmaker_exceptions.h"
#include "../catch_amalgamated.hpp"

using namespace helix::snapmaker;

TEST_CASE("a four-part code decodes as level-id-index-code", "[snapmaker][exceptions]") {
    auto c = decode_exception_code("0003-0530-0000-0011");
    REQUIRE(c.has_value());
    REQUIRE(c->level == 3);
    REQUIRE(c->id == 530);
    REQUIRE(c->index == 0);
    REQUIRE(c->code == 11);
}

TEST_CASE("a code embedded in a sentence is found", "[snapmaker][exceptions]") {
    auto c = decode_exception_code("!! Error 0003-0530-0000-0011 The plate has not been removed");
    REQUIRE(c.has_value());
    REQUIRE(c->id == 530);
}

TEST_CASE("text carrying no code decodes to nothing", "[snapmaker][exceptions]") {
    REQUIRE_FALSE(decode_exception_code("!! Must home Z axis first").has_value());
    REQUIRE_FALSE(decode_exception_code("").has_value());
}

TEST_CASE("a three-part code is not mistaken for a four-part one",
          "[snapmaker][exceptions]") {
    // The firmware also uses a 3-part basic form (id-index-code) with no level.
    // Reading it as level-id-index would silently shift every field.
    REQUIRE_FALSE(decode_exception_code("0530-0000-0011").has_value());
}

TEST_CASE("the plate-removal code maps to its message", "[snapmaker][exceptions]") {
    auto c = decode_exception_code("0003-0530-0000-0011");
    REQUIRE(exception_message(*c) ==
            "Remove the PEI sheet from the bed, then start again: probing through the sheet "
            "gives wrong results");
}

TEST_CASE("an unknown code has no message rather than a wrong one",
          "[snapmaker][exceptions]") {
    // A wrong-but-confident message is worse than falling back to the
    // firmware's own text.
    ExceptionCode unknown{3, 999, 0, 0};
    REQUIRE(exception_message(unknown).empty());
}

TEST_CASE("levels map to what the firmware will do", "[snapmaker][exceptions]") {
    REQUIRE(severity_of(1) == ExceptionSeverity::Informational);
    REQUIRE(severity_of(2) == ExceptionSeverity::Pause);
    REQUIRE(severity_of(3) == ExceptionSeverity::Cancel);
    REQUIRE(severity_of(99) == ExceptionSeverity::Cancel); // unknown: assume the worst
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `make t F='[snapmaker][exceptions]'`
Expected: compile failure — header does not exist.

- [ ] **Step 3: Write the header and implementation**

```cpp
// include/snapmaker_exceptions.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <string>
#include <string_view>

/**
 * @file snapmaker_exceptions.h
 * @brief The U1's structured fault codes, and what they mean to a user.
 *
 * The firmware raises faults with a module id, an index, a code and a level,
 * and embeds them in error text as `level-id-index-code`. Matching the English
 * sentence instead is brittle: the wording is the firmware's to change, and it
 * is not translated. The code is the stable identity.
 *
 * Levels mirror what the firmware will do about it: 1 nothing, 2 pause,
 * 3 cancel.
 */
namespace helix::snapmaker {

struct ExceptionCode {
    int level = 0;
    int id = 0;
    int index = 0;
    int code = 0;
    bool operator==(const ExceptionCode&) const = default;
};

enum class ExceptionSeverity { Informational, Pause, Cancel };

/// Find a four-part `level-id-index-code` anywhere in `text`.
/// nullopt when there is none - including for the firmware's three-part basic
/// form, which omits the level and would otherwise decode shifted by one field.
[[nodiscard]] std::optional<ExceptionCode> decode_exception_code(const std::string& text);

/// Our wording for a fault we recognise; empty for one we do not, so the caller
/// falls back to the firmware's own message rather than inventing one.
[[nodiscard]] std::string_view exception_message(const ExceptionCode& c);

/// What the firmware will do about a fault at this level. An unrecognised level
/// reads as Cancel: assuming the worst is the safe direction.
[[nodiscard]] ExceptionSeverity severity_of(int level);

} // namespace helix::snapmaker
```

```cpp
// src/printer/snapmaker_exceptions.cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "snapmaker_exceptions.h"

#include <array>
#include <cctype>

namespace helix::snapmaker {
namespace {

/// One fault we have wording for. Matched on (id, index, code); level is not
/// part of the identity, because the firmware may raise the same fault at a
/// different level depending on what it decides to do about it.
struct KnownException {
    int id;
    int index;
    int code;
    const char* message;
};

constexpr std::array<KnownException, 2> kKnown{{
    {530, 0, 11,
     "Remove the PEI sheet from the bed, then start again: probing through the sheet "
     "gives wrong results"},
    {531, 0, 16, "That setting cannot be changed while a print is running"},
}};

/// Is `s[at..at+3]` four digits?
bool four_digits(const std::string& s, size_t at) {
    if (at + 4 > s.size()) {
        return false;
    }
    for (size_t i = at; i < at + 4; ++i) {
        if (std::isdigit(static_cast<unsigned char>(s[i])) == 0) {
            return false;
        }
    }
    return true;
}

} // namespace

std::optional<ExceptionCode> decode_exception_code(const std::string& text) {
    // Scan for NNNN-NNNN-NNNN-NNNN. Requiring all four groups is what keeps the
    // three-part basic form from decoding as a shifted four-part one.
    for (size_t i = 0; i + 19 <= text.size(); ++i) {
        if (!four_digits(text, i) || text[i + 4] != '-' || !four_digits(text, i + 5) ||
            text[i + 9] != '-' || !four_digits(text, i + 10) || text[i + 14] != '-' ||
            !four_digits(text, i + 15)) {
            continue;
        }
        // A fifth group means this is not the shape we think it is.
        if (i + 19 < text.size() && text[i + 19] == '-') {
            continue;
        }
        ExceptionCode c;
        c.level = std::stoi(text.substr(i, 4));
        c.id = std::stoi(text.substr(i + 5, 4));
        c.index = std::stoi(text.substr(i + 10, 4));
        c.code = std::stoi(text.substr(i + 15, 4));
        return c;
    }
    return std::nullopt;
}

std::string_view exception_message(const ExceptionCode& c) {
    for (const auto& k : kKnown) {
        if (k.id == c.id && k.index == c.index && k.code == c.code) {
            return k.message;
        }
    }
    return {};
}

ExceptionSeverity severity_of(int level) {
    switch (level) {
    case 1:
        return ExceptionSeverity::Informational;
    case 2:
        return ExceptionSeverity::Pause;
    default:
        return ExceptionSeverity::Cancel;
    }
}

} // namespace helix::snapmaker
```

- [ ] **Step 4: Add the translation keys and the ESP32 entry**

The two `kKnown` messages are user-facing. Add both to all nine `translations/*.yml`, then `make translations`. Add `src/printer/snapmaker_exceptions.cpp` to `app_srcs.txt`.

- [ ] **Step 5: Run the tests and watch them pass**

Run: `make t F='[snapmaker][exceptions]'`
Expected: PASS, 7 test cases.

- [ ] **Step 6: Prove a test can fail**

Relax the parser to accept three groups, run the tag, confirm "a three-part code is not mistaken for a four-part one" goes red, then revert.

- [ ] **Step 7: Commit**

```bash
git commit -m "feat(snapmaker): decode the U1's structured fault codes" \
  -m "level-id-index-code, with a small table mapping the faults we have wording for. An unrecognised code returns no message so the caller falls back to the firmware's own text rather than inventing one." \
  -m "Mutation: accepting three groups turns [snapmaker][exceptions] red." \
  -- include/snapmaker_exceptions.h src/printer/snapmaker_exceptions.cpp \
     tests/unit/test_snapmaker_exceptions.cpp translations/ ui_xml/translations/ \
     firmware/helixscreen-esp32/components/helixapp/app_srcs.txt
```

---

## Task 5: Classify Snapmaker errors from codes, not phrases

`GcodeErrorRouter::process_line` already asks `AmsState::instance().get_backend()->classify_error(line, ctx)` first, and `AmsBackendSnapmaker` does not override it — the base returns nullopt, so every U1 fault falls through to generic handling. Override it with Task 4's decoder, and retire the phrase matchers.

**Files:**
- Modify: `include/ams_backend_snapmaker.h`
- Modify: `src/printer/ams_backend_snapmaker.cpp`
- Modify: `include/auto_screws_tilt_adjust.h` (drop `PLATE_NOT_REMOVED_TEXT`)
- Modify: `src/api/auto_screws_tilt_adjust.cpp#plate_still_on_bed`
- Test: `tests/unit/test_snapmaker_error_classify.cpp` *(new)*
- Modify: `tests/unit/test_auto_screws_tilt.cpp` (the phrase case becomes a code case)

**Interfaces:**
- Consumes: `decode_exception_code`, `exception_message`, `severity_of` (Task 4); `AmsBackend::classify_error` and `ClassifyContext` from `include/ams_backend.h`.
- Produces: `std::optional<AmsError> AmsBackendSnapmaker::classify_error(const std::string&, const ClassifyContext&) const`.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/unit/test_snapmaker_error_classify.cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ams_backend_snapmaker.h"
#include "../catch_amalgamated.hpp"

TEST_CASE("a known fault classifies from its code", "[snapmaker][classify]") {
    AmsBackendSnapmaker backend;
    ClassifyContext ctx;
    auto e = backend.classify_error("!! 0003-0530-0000-0011 The plate has not been removed",
                                    ctx);
    REQUIRE(e.has_value());
    REQUIRE(e->message.find("Remove the PEI sheet") != std::string::npos);
}

TEST_CASE("classification does not depend on the firmware's wording",
          "[snapmaker][classify]") {
    // The whole point: the code is the identity. Firmware reworded, or a
    // locale we do not read, must still classify.
    AmsBackendSnapmaker backend;
    ClassifyContext ctx;
    auto e = backend.classify_error("!! 0003-0530-0000-0011 platen nicht entfernt", ctx);
    REQUIRE(e.has_value());
    REQUIRE(e->message.find("Remove the PEI sheet") != std::string::npos);
}

TEST_CASE("an unknown code keeps the firmware's own text", "[snapmaker][classify]") {
    AmsBackendSnapmaker backend;
    ClassifyContext ctx;
    auto e = backend.classify_error("!! 0002-0999-0000-0007 something we have no wording for",
                                    ctx);
    REQUIRE(e.has_value());
    REQUIRE(e->message.find("something we have no wording for") != std::string::npos);
}

TEST_CASE("a line with no code is left to the generic path", "[snapmaker][classify]") {
    AmsBackendSnapmaker backend;
    ClassifyContext ctx;
    REQUIRE_FALSE(backend.classify_error("!! Must home Z axis first", ctx).has_value());
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `make t F='[snapmaker][classify]'`
Expected: FAIL — base `classify_error` returns nullopt, so the first case fails.

- [ ] **Step 3: Implement the override**

Read `include/ams_backend.h#AmsBackend::classify_error` for the exact signature and the `AmsError` fields, then in `src/printer/ams_backend_snapmaker.cpp`:

```cpp
std::optional<AmsError> AmsBackendSnapmaker::classify_error(const std::string& line,
                                                            const ClassifyContext& ctx) const {
    const auto code = helix::snapmaker::decode_exception_code(line);
    if (!code) {
        return std::nullopt; // not a structured fault; let the generic path have it
    }
    AmsError err;
    const std::string_view ours = helix::snapmaker::exception_message(*code);
    // Our wording when we have it; the firmware's own text when we do not. A
    // confident wrong message is worse than an unpolished right one.
    err.message = ours.empty() ? line : std::string(ours);
    err.severity = helix::snapmaker::severity_of(code->level);
    return err;
}
```

Map `ExceptionSeverity` onto whatever severity type `AmsError` actually carries — check the struct rather than assuming it matches.

- [ ] **Step 4: Retire the phrase fallback in the plate gate**

In `src/api/auto_screws_tilt_adjust.cpp#plate_still_on_bed`, drop the `PLATE_NOT_REMOVED_TEXT` branch and decode the code instead:

```cpp
bool plate_still_on_bed(const std::string& error_message) {
    const auto code = helix::snapmaker::decode_exception_code(error_message);
    return code && code->id == 530 && code->index == 0 && code->code == 11;
}
```

Remove `PLATE_NOT_REMOVED_TEXT` from `include/auto_screws_tilt_adjust.h`. Keep `PLATE_NOT_REMOVED_CODE` only if something else still references it; otherwise remove it too and let the decoder own the identity.

Update the case in `tests/unit/test_auto_screws_tilt.cpp` that feeds the bare phrase: it must now feed a message carrying the code. If the existing test asserted the phrase alone classifies, that assertion is now wrong and should be replaced, not deleted — assert instead that a phrase WITHOUT a code no longer classifies.

- [ ] **Step 5: Run the tests and watch them pass**

Run: `make t F='[snapmaker][classify],[screws_tilt],[auto_screws]'`
Expected: PASS.

- [ ] **Step 6: Prove a test can fail**

Make `classify_error` return nullopt unconditionally, run `make t F='[snapmaker][classify]'`, confirm red, revert.

- [ ] **Step 7: Commit**

```bash
git commit -m "fix(snapmaker): classify U1 faults by code instead of English wording" \
  -m "GcodeErrorRouter already asks the backend first and Snapmaker never answered, so every U1 fault fell through to generic handling. The override decodes level-id-index-code, keeps the firmware's own text for faults we have no wording for, and retires the plate gate's phrase fallback." \
  -m "Mutation: returning nullopt from classify_error turns [snapmaker][classify] red." \
  -- include/ams_backend_snapmaker.h src/printer/ams_backend_snapmaker.cpp \
     include/auto_screws_tilt_adjust.h src/api/auto_screws_tilt_adjust.cpp \
     tests/unit/test_snapmaker_error_classify.cpp tests/unit/test_auto_screws_tilt.cpp
```

---

## Task 6: Subscribe exception_manager and surface active faults

Classification handles faults that arrive as gcode responses. `exception_manager.exceptions` is the other half: the list of faults currently standing, including persistent ones that survived a restart and were never printed to the console this session.

**Files:**
- Modify: `src/api/moonraker_discovery_sequence.cpp#build_subscription_objects`
- Modify: `include/snapmaker_exceptions.h` / `src/printer/snapmaker_exceptions.cpp` (add the array reader)
- Modify: `tests/unit/test_snapmaker_exceptions.cpp`

**Interfaces:**
- Consumes: `ExceptionCode`, `exception_message`, `severity_of` (Task 4).
- Produces: `std::vector<ActiveException> read_active_exceptions(const nlohmann::json& status)` with `struct ActiveException { ExceptionCode code; std::string message; bool persistent; }`.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("active exceptions read out of a status frame", "[snapmaker][exceptions]") {
    nlohmann::json s = nlohmann::json::object();
    s["exception_manager"] = {{"exceptions",
                               {{{"id", 530}, {"index", 0}, {"code", 11}, {"level", 3},
                                 {"message", "The plate has not been removed"},
                                 {"is_persistent", 0}}}}};
    auto v = helix::snapmaker::read_active_exceptions(s);
    REQUIRE(v.size() == 1);
    REQUIRE(v[0].code.id == 530);
    REQUIRE(v[0].message.find("Remove the PEI sheet") != std::string::npos);
    REQUIRE(v[0].persistent == false);
}

TEST_CASE("an empty exception list means no active faults", "[snapmaker][exceptions]") {
    nlohmann::json s = nlohmann::json::object();
    s["exception_manager"] = {{"exceptions", nlohmann::json::array()}};
    REQUIRE(helix::snapmaker::read_active_exceptions(s).empty());
}

TEST_CASE("a frame without exception_manager is silent, not empty",
          "[snapmaker][exceptions]") {
    // Distinguishing these matters: "no faults" clears a banner, "the frame did
    // not mention faults" must leave it alone.
    nlohmann::json s = nlohmann::json::object();
    s["toolhead"] = {{"homed_axes", "xyz"}};
    REQUIRE_FALSE(helix::snapmaker::status_carries_exceptions(s));
    REQUIRE(helix::snapmaker::status_carries_exceptions(
        nlohmann::json{{"exception_manager", {{"exceptions", nlohmann::json::array()}}}}));
}
```

- [ ] **Step 2: Run it, watch it fail, implement, run it again**

Add `ActiveException`, `read_active_exceptions` and `status_carries_exceptions` to the header and cpp, reusing `exception_message`/`severity_of`. The `status_carries_exceptions` predicate is what keeps a delta frame from reading as "all faults cleared".

- [ ] **Step 3: Subscribe the object**

In `src/api/moonraker_discovery_sequence.cpp#build_subscription_objects`, beside the existing U1-specific subscriptions, add `exception_manager` **only when the printer reports it**. Follow the shape already used for `zoffset::required_status_objects(hw)` — a capability question, not a vendor branch. If `exception_manager` appears in `hw.printer_objects()`, subscribe it.

- [ ] **Step 4: Commit**

```bash
git commit -m "feat(snapmaker): subscribe exception_manager and read standing faults" \
  -m "Classification covers faults that arrive as console output; the exceptions array covers the ones already standing, including persistent faults that survived a restart. A frame that omits the object is silent about faults rather than reporting none." \
  -m "Mutation: treating an absent exception_manager as an empty list turns [snapmaker][exceptions] red." \
  -- include/snapmaker_exceptions.h src/printer/snapmaker_exceptions.cpp \
     src/api/moonraker_discovery_sequence.cpp tests/unit/test_snapmaker_exceptions.cpp
```

---

## Task 7: Per-filament load and unload temperatures

`filament_parameters` publishes nothing through status — `get_status` returns `{}`. Its data comes from the `FILAMENT_PARA_GET_ALL_INFO` gcode command, keyed by vendor, main type, sub type and nozzle diameter, and includes load, unload, clean-nozzle and flow temperatures. Today we use a generic material table for every printer.

**Files:**
- Create: `include/filament_temperature_source.h`
- Create: `src/printer/filament_temperature_source.cpp`
- Create: `tests/unit/test_filament_temperature_source.cpp`
- Modify: `firmware/helixscreen-esp32/components/helixapp/app_srcs.txt`

**Interfaces:**
- Consumes: `PrinterDiscovery`.
- Produces:
  - `bool firmware_publishes_filament_temperatures(const PrinterDiscovery& hw)`
  - `std::string filament_temperature_query_gcode(const PrinterDiscovery& hw)` — empty when none
  - `struct FilamentTemperatures { std::optional<int> load_c, unload_c, clean_nozzle_c; }`
  - `std::map<FilamentKey, FilamentTemperatures> parse_filament_temperatures(const std::string& response)`
  - `struct FilamentKey { std::string vendor, main_type, sub_type; }` with `operator<`

- [ ] **Step 1: Capture a real response first**

This task cannot be written blind — the response format of `FILAMENT_PARA_GET_ALL_INFO` is not documented anywhere in our tree. Before writing the parser, ask Preston for printer access and capture it:

```bash
curl -s -X POST 'http://192.168.30.103:7125/printer/gcode/script' \
  -H 'Content-Type: application/json' \
  -d '{"script":"FILAMENT_PARA_GET_ALL_INFO"}'
curl -s 'http://192.168.30.103:7125/server/gcode_store?count=50' | python3 -m json.tool
```

Save the response verbatim into the test file as a fixture. If it turns out to be enormous, trim to three representative filaments but keep the exact shape — do NOT hand-write a plausible format.

- [ ] **Step 2: Write the failing test against the captured fixture**

Assert: a known vendor/type/sub-type resolves to its temperatures; an unknown one yields nullopt rather than a default; a malformed response yields an empty map rather than throwing.

- [ ] **Step 3: Implement the provider table**

Same shape as `include/z_offset_persistence.h` and `include/pre_print_preferences.h`: a provider row keyed on a detection predicate (`filament_parameters` present in `hw.printer_objects()`, guarded by `hw.objects_reported()`), with the capability questions as free functions. No vendor name escapes this module.

- [ ] **Step 4: Wire one consumer, not all of them**

Find the current load/unload temperature decision (grep the call shape, not a guessed name — start from where a filament load sets a target and work back). Change **one** call site to prefer the firmware's value when the capability answers, falling back to the existing table otherwise. Do not convert every site in this task; one proven consumer is the deliverable.

- [ ] **Step 5: Commit**

```bash
git commit -m "feat(printer): prefer the firmware's own per-filament temperatures where it has them" \
  -m "The U1 publishes load, unload and clean-nozzle temperatures per vendor, type, sub-type and nozzle diameter via FILAMENT_PARA_GET_ALL_INFO; our generic material table was overriding a better answer. Capability question, so a second firmware is one provider row." \
  -m "Mutation: returning the generic table when the firmware has a value turns [filament][temps] red." \
  -- include/filament_temperature_source.h src/printer/filament_temperature_source.cpp \
     tests/unit/test_filament_temperature_source.cpp \
     firmware/helixscreen-esp32/components/helixapp/app_srcs.txt
```

---

## Task 8: The power-loss sensor, and what our blind PLR can do with it

`power_loss_check` is a live voltage monitor, not a record of a past loss: `{initialized, high_level_tick, low_level_tick, voltage_type, power_loss_flag, duty_percent}`. Our PLR ships blind — it recovers without ever knowing the firmware saw the loss coming.

**Files:**
- Create: `include/power_loss_sensor.h`
- Create: `src/printer/power_loss_sensor.cpp`
- Create: `tests/unit/test_power_loss_sensor.cpp`
- Modify: `firmware/helixscreen-esp32/components/helixapp/app_srcs.txt`

**Interfaces:**
- Consumes: `PrinterDiscovery`.
- Produces:
  - `bool firmware_reports_power_loss(const PrinterDiscovery& hw)`
  - `std::vector<std::string> required_status_objects(const PrinterDiscovery& hw)`
  - `std::optional<bool> power_loss_asserted(const PrinterDiscovery& hw, const nlohmann::json& status)`

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("power loss sensor: detected by its object", "[power_loss][sensor]") {
    PrinterDiscovery hw = hardware_with_objects({"power_loss_check"});
    REQUIRE(helix::power_loss::firmware_reports_power_loss(hw));
}

TEST_CASE("power loss sensor: the flag reads through", "[power_loss][sensor]") {
    PrinterDiscovery hw = hardware_with_objects({"power_loss_check"});
    nlohmann::json s;
    s["power_loss_check"] = {{"initialized", 1}, {"power_loss_flag", 1}};
    REQUIRE(helix::power_loss::power_loss_asserted(hw, s).value() == true);
    s["power_loss_check"]["power_loss_flag"] = 0;
    REQUIRE(helix::power_loss::power_loss_asserted(hw, s).value() == false);
}

TEST_CASE("power loss sensor: an uninitialised sensor answers nothing",
          "[power_loss][sensor]") {
    // initialized:0 means the monitor has not taken a reading. Reading its flag
    // as a false would report "mains is fine" on no evidence.
    PrinterDiscovery hw = hardware_with_objects({"power_loss_check"});
    nlohmann::json s;
    s["power_loss_check"] = {{"initialized", 0}, {"power_loss_flag", 0}};
    REQUIRE_FALSE(helix::power_loss::power_loss_asserted(hw, s).has_value());
}

TEST_CASE("power loss sensor: a frame that omits the object answers nothing",
          "[power_loss][sensor]") {
    PrinterDiscovery hw = hardware_with_objects({"power_loss_check"});
    REQUIRE_FALSE(helix::power_loss::power_loss_asserted(hw, nlohmann::json::object())
                      .has_value());
}
```

- [ ] **Step 2: Run it, watch it fail, implement, run it again**

Provider table on the same shape as Tasks 7 and `z_offset_persistence`. Subscribe the object from the discovery sequence via `required_status_objects`, as Task 6 does for `exception_manager`.

- [ ] **Step 3: Decide what PLR does with it — and write that decision down**

This is a judgement call and must not be guessed at in code. Read the existing PLR path (`project_u1_power_loss_recovery`, `tests/unit/test_plr_state.cpp`) and pick ONE of:

- **(a) Diagnostic only.** Surface the sensor in the debug bundle and the printer-info surface. No behaviour change. Lowest risk; makes the next investigation possible.
- **(b) Corroborate the recovery prompt.** When PLR offers to resume, say whether the firmware confirms it saw a power loss. Changes wording, not behaviour.
- **(c) Gate the prompt.** Only offer recovery when the sensor agrees. **Do not choose this without Preston** — a sensor that reads unknown on some units would suppress a recovery the user wanted.

Default to (a) unless Preston says otherwise, and record the choice in the commit body. Whatever is chosen, the sensor must never make recovery LESS available than it is today without an explicit decision.

- [ ] **Step 4: Commit**

```bash
git commit -m "feat(printer): read the firmware's power-loss sensor where it has one" \
  -m "power_loss_check is a live voltage monitor, not a record of a past loss, so an uninitialised sensor answers nothing rather than 'mains is fine'. Surfaced diagnostically; recovery behaviour is unchanged." \
  -m "Mutation: reading the flag while initialized is 0 turns [power_loss][sensor] red." \
  -- include/power_loss_sensor.h src/printer/power_loss_sensor.cpp \
     tests/unit/test_power_loss_sensor.cpp \
     firmware/helixscreen-esp32/components/helixapp/app_srcs.txt
```

---

## Verification

Mock coverage cannot prove agreement with firmware. After Tasks 3, 5 and 7, on real hardware, with Preston's go-ahead:

1. Each setting: change it in our UI, read it back from `print_task_config`, confirm it persists and that no OTHER preference moved.
2. Trigger the mid-print guard (change end-unload during a print) and confirm the refusal is classified, not raw.
3. Trigger the plate refusal with the PEI sheet on and confirm the message still reads correctly with the phrase matcher gone.
4. Record every preference before testing and restore it afterwards.

## Out of scope

- `SET_PAUSE_AT_LAYER` / `SET_PAUSE_NEXT_LAYER` — filed as prestonbrown/helixscreen#1703. Our pause UI derives from a file scan and a runtime-scheduled pause has no representation in it; that needs a design conversation, not a settings row.
- `FLOW_CALIBRATE_EXTRUDERS` as a per-toolhead selector — the parent `flow_calibrate` toggle already ships as all-or-nothing. Revisit only if the per-tool distinction is asked for.
