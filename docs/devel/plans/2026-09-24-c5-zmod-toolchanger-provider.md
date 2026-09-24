# Creator 5 Pro on Z-Mod: tool changer provider - Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A FlashForge Creator 5 Pro running Z-Mod works as an ordinary 4-tool changer in HelixScreen: mounted head (including none), Mount/Unmount, and per-head material and colour owned by the firmware.

**Architecture:** No new backend. A Z-Mod row joins the `toolchanger_addon` provider table (detection, status objects, tool reading, swap commands), and providers gain an optional firmware material source that `AmsBackendToolChanger` reads into `VendorCache` observations and writes back with `CHANGE_ZCOLOR`. `ToolState` learns that a tool changer's carriage can be empty. A Z-Mod mock persona exercises the real backend under `--test`.

**Tech Stack:** C++17, LVGL 9.5, Catch2 (`tests/catch_amalgamated.hpp`), nlohmann json via `hv/json.hpp`, spdlog, pure Makefile.

**Spec:** `docs/devel/plans/2026-09-24-c5-zmod-toolchanger-provider-design.md` (read it first; this plan argues from it).

## Global Constraints

- Repo rules: `CLAUDE.md`, `.claude/rules/vendor-abstraction.md`, `.claude/rules/threading.md`. Vendor names live only in `toolchanger_addon`, the new `zmod_color_status` module, the AD5X backend and the mock.
- spdlog only; SPDX header `// SPDX-License-Identifier: GPL-3.0-or-later` on new source files; `#include "hv/json.hpp"`, never `<nlohmann/json.hpp>`; no RTTI.
- Comments describe the code as it is now. No history, no commit SHAs, no "used to".
- Gcode slot numbers that are 1-based on the wire carry `// DISPLAY_NUMBERING_OK: gcode wire, not a label`.
- Build with `make -j"$(scripts/helix-claim jobs)"`; test inner loop `make t F='<tags>'`; never pipe a build through `head`/`tail`/`grep`.
- Commit with explicit pathspecs; never `git add -A`, never `--no-verify`, never `git stash`.
- Each task's commit body: a one-line `mutation:` naming the change that turned its test red (`make mutate-diff` or a manual revert).
- Z-Mod write command, exact: `CHANGE_ZCOLOR SLOT=<n+1> HEX=<RRGGBB upper> TYPE=<type> SILENT=1`.
- Z-Mod swap commands, exact: `_T_IN T=<n>` and `_T_OUT`.
- Z-Mod C5 detection, exact: Klipper objects `zmod_color` AND `gcode_button extruder_grab1`.

## Review Focus

1. **A delta frame carrying only `active_tool_id`** must not clear any head's material or colour (Moonraker sends only changed fields). Test: Task 5, "a frame without slots leaves the firmware reading standing".
2. **Z-Mod without ghzserg/z_c5pro#1** (no `slots`, no `palette`): edits are declared locally and no `CHANGE_ZCOLOR` is sent. Test: Task 6, "before the firmware publishes slots an edit stays local".
3. **A colour outside the 24-entry palette** must be snapped to the nearest entry, never sent as a hex the firmware stores as white. Test: Task 6, "a colour outside the palette is snapped".
4. **A firmware type that is unsafe on a gcode line** (a user-added `filament_*` type containing `;`) is refused with an error and nothing is sent. Test: Task 6, "an unsafe type is refused".
5. **Unmounting to an empty carriage** leaves `ToolState` with no active tool rather than T0, and does not disturb lane-based (single-extruder) AMS topologies. Tests: Task 1.

---

### Task 0: Branch preparation

**Precondition:** `feature/c5-platform` (DB entry detecting both C5 firmwares, preset, `creator5` persona) is merged to `main`. If it is not, STOP and report; nothing below is built on the old base.

- [ ] **Step 1: Claim and rebase**

```bash
cd /home/pbrown/Code/Printing/helixscreen/.worktrees/c5-zmod-provider
scripts/helix-claim check worktree:c5-zmod-provider   # must be yours or FREE
git fetch origin
git -c merge.autoStash=false rebase --onto main $(git merge-base HEAD origin/main)
git log --oneline main..HEAD   # expect only the docs(plans) commits
```

- [ ] **Step 2: Build baseline**

```bash
make -j"$(scripts/helix-claim jobs)" && make t F='[toolchanger]'
```
Expected: build succeeds; `[toolchanger]` all passed. Record the assertion count.

---

### Task 1: An empty carriage stays empty on a tool changer

**Files:**
- Modify: `include/tool_state.h` (`struct ToolTopology`)
- Modify: `src/printer/tool_state.cpp` (`ToolState::set_ams_topology`)
- Modify: `src/printer/ams_state.cpp` (`build_ams_topology`)
- Test: `tests/unit/test_tool_state_ams_topology.cpp`

**Interfaces:**
- Produces: `bool ToolTopology::allows_empty_carriage` (default `false`); `build_ams_topology()` sets it from `AmsBackend::load_mounts_tool()`.

- [ ] **Step 1: Write the failing tests** (append to `tests/unit/test_tool_state_ams_topology.cpp`; add `#include "ams_backend_toolchanger.h"` and `#include "ams_tool_topology.h"` to its includes)

```cpp
TEST_CASE_METHOD(ToolStateFixture, "An empty carriage stays empty on a tool changer topology",
                 "[tool-state][ams-topology][empty-carriage]") {
    ToolTopology topo;
    topo.tool_count = 4;
    topo.tool_to_slot = {0, 1, 2, 3};
    topo.allows_empty_carriage = true;

    topo.active_tool = 2;
    ToolState::instance().set_ams_topology(topo);
    UpdateQueue::instance().drain();
    REQUIRE(ToolState::instance().active_tool_index() == 2);

    // Unmounting parks the head: nothing is on the carriage.
    topo.active_tool = -1;
    ToolState::instance().set_ams_topology(topo);
    UpdateQueue::instance().drain();
    CHECK(ToolState::instance().active_tool_index() == -1);
    CHECK(ToolState::instance().active_tool() == nullptr);

    ToolState::instance().clear_ams_topology();
    UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(ToolStateFixture, "A lane topology with nothing loaded still reports T0",
                 "[tool-state][ams-topology][empty-carriage]") {
    // Lanes feeding one extruder: -1 means no lane loaded, and the one hotend
    // is still T0.
    ToolTopology topo;
    topo.tool_count = 4;
    topo.tool_to_slot = {0, 1, 2, 3};
    topo.active_tool = -1;
    ToolState::instance().set_ams_topology(topo);
    UpdateQueue::instance().drain();
    CHECK(ToolState::instance().active_tool_index() == 0);

    ToolState::instance().clear_ams_topology();
    UpdateQueue::instance().drain();
}

TEST_CASE("A tool changer's topology allows an empty carriage",
          "[tool-state][ams-topology][empty-carriage]") {
    helix::AmsBackendToolChanger tc(nullptr, nullptr);
    tc.set_discovered_tools({"T0", "T1"});
    auto topo = helix::build_ams_topology(&tc, 0);
    REQUIRE(topo.has_value());
    CHECK(topo->allows_empty_carriage);
    CHECK(topo->active_tool == -1);
}
```

Check the namespace of `build_ams_topology` in `include/ams_tool_topology.h` and the one `AmsBackendToolChanger` lives in (`include/ams_backend_toolchanger.h`); adjust the qualifiers to match rather than guessing.

- [ ] **Step 2: Run to verify failure**

Run: `make t F='[empty-carriage]'`
Expected: FAIL to compile (`allows_empty_carriage` is not a member of `ToolTopology`).

- [ ] **Step 3: Implement**

`include/tool_state.h`, inside `struct ToolTopology` after `active_tool`:

```cpp
    /// The carriage can hold no tool at all, so active_tool -1 is a real state
    /// rather than "nothing loaded". True where selecting a slot mounts a tool.
    bool allows_empty_carriage = false;
```

`src/printer/tool_state.cpp`, in `ToolState::set_ams_topology`, replace the active-tool clamp:

```cpp
    int new_active = topo.active_tool;
    const bool empty_carriage = topo.allows_empty_carriage && new_active == -1;
    if (!empty_carriage && (new_active < 0 || new_active >= static_cast<int>(tools_.size()))) {
        new_active = 0; // Out-of-range falls back to T0 (matches init_tools convention)
    }
```

`src/printer/ams_state.cpp`, in `build_ams_topology`, after `topo.active_tool = backend->get_current_tool();`:

```cpp
    topo.allows_empty_carriage = backend->load_mounts_tool();
```

- [ ] **Step 4: Run to verify pass, plus the neighbours**

Run: `make t F='[tool-state]'` then `./build/bin/helix-tests '[ams]'` and `./build/bin/helix-tests '[toolchanger]'`
Expected: all passed. If a consumer of `active_tool_index()` misbehaves on -1 (nozzle label, tool badge), fix it in this task: the non-topology `toolchanger.tool_number` path already produces -1, so consumers are expected to handle it.

- [ ] **Step 5: Prove the test can fail, then commit**

Revert the `empty_carriage` clause only; `[empty-carriage]` must go red; restore.

```bash
git commit -m "fix(tools): a tool changer with nothing mounted has no active tool" \
  -m "mutation: dropping the empty_carriage clause reports T0 for a parked carriage; [empty-carriage] went red." \
  -- include/tool_state.h src/printer/tool_state.cpp src/printer/ams_state.cpp tests/unit/test_tool_state_ams_topology.cpp
git show --stat HEAD
```

---

### Task 2: One nearest-palette-colour rule

**Files:**
- Modify: `include/color_utils.h`
- Modify: `src/printer/ams_backend_qidi.cpp` (`AmsBackendQidi::resolve_color_id`)
- Test: `tests/unit/test_color_utils.cpp`

**Interfaces:**
- Produces: `template <typename Entries, typename Key> Key helix::nearest_palette_key(const Entries& entries, uint32_t rgb, Key fallback)`. `entries` iterates `(key, 0xRRGGBB)` pairs (`std::map<int, uint32_t>` or `std::vector<std::pair<K, uint32_t>>`). Ties keep the first entry.

- [ ] **Step 1: Write the failing test** (append to `tests/unit/test_color_utils.cpp`; add `#include <map>`, `<utility>`, `<vector>` if missing)

```cpp
TEST_CASE("nearest_palette_key picks the closest entry by RGB distance", "[color_utils][palette]") {
    const std::vector<std::pair<int, uint32_t>> palette = {
        {0, 0xFFFFFF}, {1, 0xF72224}, {2, 0x161616}, {3, 0x0ACC38}};
    CHECK(helix::nearest_palette_key(palette, 0xFFFFFF, -1) == 0);
    CHECK(helix::nearest_palette_key(palette, 0xE01010, -1) == 1);
    CHECK(helix::nearest_palette_key(palette, 0x000000, -1) == 2);
    CHECK(helix::nearest_palette_key(palette, 0x10D040, -1) == 3);
}

TEST_CASE("nearest_palette_key returns the fallback for an empty palette",
          "[color_utils][palette]") {
    const std::vector<std::pair<int, uint32_t>> none;
    CHECK(helix::nearest_palette_key(none, 0x123456, -7) == -7);
}

TEST_CASE("nearest_palette_key keeps the first of two equally close entries",
          "[color_utils][palette]") {
    const std::map<int, uint32_t> palette = {{5, 0x000010}, {9, 0x000030}};
    CHECK(helix::nearest_palette_key(palette, 0x000020, 0) == 5);
}
```

- [ ] **Step 2: Run to verify failure**

Run: `make t F='[palette]'`
Expected: FAIL to compile (`nearest_palette_key` is not a member of `helix`).

- [ ] **Step 3: Implement** in `include/color_utils.h`, inside `namespace helix`, after `color_to_hex_string`:

```cpp
/**
 * @brief Key of the palette entry nearest to @p rgb by squared RGB distance.
 *
 * For firmware that stores a colour as an index into a fixed palette, where a
 * colour outside it cannot be stored at all.
 *
 * @param entries (key, 0xRRGGBB) pairs: a std::map<int, uint32_t>, or a vector
 *        of pairs. Ties keep the first entry.
 * @param fallback Returned when @p entries is empty.
 */
template <typename Entries, typename Key>
Key nearest_palette_key(const Entries& entries, uint32_t rgb, Key fallback) {
    Key best = fallback;
    long best_dist = -1;
    const long r = (rgb >> 16) & 0xFF;
    const long g = (rgb >> 8) & 0xFF;
    const long b = rgb & 0xFF;
    for (const auto& [key, packed] : entries) {
        const long pr = (packed >> 16) & 0xFF;
        const long pg = (packed >> 8) & 0xFF;
        const long pb = packed & 0xFF;
        const long dist = (r - pr) * (r - pr) + (g - pg) * (g - pg) + (b - pb) * (b - pb);
        if (best_dist < 0 || dist < best_dist) {
            best_dist = dist;
            best = key;
        }
    }
    return best;
}
```

Replace the body of `AmsBackendQidi::resolve_color_id` with:

```cpp
    return helix::nearest_palette_key(palette, rgb, 0);
```
(add `#include "color_utils.h"` to `src/printer/ams_backend_qidi.cpp` if absent).

- [ ] **Step 4: Run**

Run: `make t F='[palette]'` then `./build/bin/helix-tests '[qidi_box]'`
Expected: all passed (QIDI's own `resolve_color_id` test still guards its behaviour).

- [ ] **Step 5: Mutate, commit**

Change `dist < best_dist` to `dist <= best_dist`; the tie test must go red; restore.

```bash
git commit -m "refactor(color): one nearest-palette-colour rule, QIDI Box uses it" \
  -m "mutation: '<=' in nearest_palette_key keeps the last of two equal entries; the tie case went red." \
  -- include/color_utils.h src/printer/ams_backend_qidi.cpp tests/unit/test_color_utils.cpp
git show --stat HEAD
```

---

### Task 3: One reader for Z-Mod's `zmod_color` status

**Files:**
- Create: `include/zmod_color_status.h`, `src/printer/zmod_color_status.cpp`
- Modify: `src/printer/ams_backend_ad5x_ifs.cpp` (`AmsBackendAd5xIfs::read_zmod_color_object`)
- Test: `tests/unit/test_zmod_color_status.cpp` (new), existing `[zmod_status]` tag in `tests/unit/test_ams_ad5x_zmod_status.cpp`

**Interfaces:**
- Produces (namespace `helix::zmod_color`):
  - `struct Slot { std::string material; std::string hex; };`
  - `std::optional<std::vector<std::optional<Slot>>> parse_slots(const nlohmann::json& obj, int max_slots);`
  - `std::optional<std::vector<std::pair<int, std::uint32_t>>> parse_palette(const nlohmann::json& obj);`
  - `std::optional<std::vector<std::string>> parse_valid_types(const nlohmann::json& obj);`

- [ ] **Step 1: Write the failing tests** (`tests/unit/test_zmod_color_status.cpp`)

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "zmod_color_status.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
using namespace helix;

TEST_CASE("zmod_color slots are indexed by their 1-based ID", "[zmod_color]") {
    const json obj = {{"slots",
                       json::array({{{"ID", "2"}, {"Material", "PETG"}, {"HEX", "0ACC38"}},
                                    {{"ID", 1}, {"Material", "PLA"}, {"HEX", "FFFFFF"}}})}};
    auto slots = zmod_color::parse_slots(obj, 4);
    REQUIRE(slots.has_value());
    REQUIRE(slots->size() == 4);
    REQUIRE((*slots)[0].has_value());
    CHECK((*slots)[0]->material == "PLA");
    CHECK((*slots)[1]->material == "PETG");
    CHECK((*slots)[1]->hex == "0ACC38");
    CHECK_FALSE((*slots)[2].has_value());
}

TEST_CASE("zmod_color's '?' material reads as unset", "[zmod_color]") {
    const json obj = {{"slots", json::array({{{"ID", "1"}, {"Material", "?"}, {"HEX", ""}}})}};
    auto slots = zmod_color::parse_slots(obj, 4);
    REQUIRE(slots.has_value());
    REQUIRE((*slots)[0].has_value());
    CHECK((*slots)[0]->material.empty());
}

TEST_CASE("zmod_color drops out-of-range and malformed slot IDs", "[zmod_color]") {
    const json obj = {{"slots", json::array({{{"ID", "0"}, {"Material", "PLA"}},
                                             {{"ID", "5"}, {"Material", "PLA"}},
                                             {{"ID", "x"}, {"Material", "PLA"}},
                                             "not an object"})}};
    auto slots = zmod_color::parse_slots(obj, 4);
    REQUIRE(slots.has_value());
    for (const auto& s : *slots) {
        CHECK_FALSE(s.has_value());
    }
}

TEST_CASE("A zmod_color frame without slots is no news", "[zmod_color]") {
    CHECK_FALSE(zmod_color::parse_slots(json{{"active_tool_id", 2}}, 4).has_value());
    CHECK_FALSE(zmod_color::parse_palette(json{{"active_tool_id", 2}}).has_value());
    CHECK_FALSE(zmod_color::parse_valid_types(json{{"active_tool_id", 2}}).has_value());
}

TEST_CASE("zmod_color palette keeps firmware index order", "[zmod_color]") {
    const json obj = {{"palette", json::array({"FFFFFF", "fef043", "junk", "161616"})}};
    auto palette = zmod_color::parse_palette(obj);
    REQUIRE(palette.has_value());
    REQUIRE(palette->size() == 3);
    CHECK((*palette)[0] == std::pair<int, std::uint32_t>{0, 0xFFFFFF});
    CHECK((*palette)[1] == std::pair<int, std::uint32_t>{1, 0xFEF043});
    CHECK((*palette)[2] == std::pair<int, std::uint32_t>{3, 0x161616});
}

TEST_CASE("zmod_color valid_types drops the '?' sentinel", "[zmod_color]") {
    const json obj = {{"valid_types", json::array({"PLA", "PETG", "?"})}};
    auto types = zmod_color::parse_valid_types(obj);
    REQUIRE(types.has_value());
    CHECK(*types == std::vector<std::string>{"PLA", "PETG"});
}
```

- [ ] **Step 2: Run to verify failure**

Run: `make t F='[zmod_color]'`
Expected: FAIL to compile (`zmod_color_status.h` not found).

- [ ] **Step 3: Implement**

`include/zmod_color_status.h`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Z-Mod's `zmod_color` Klipper status object. The AD5X (IFS lanes) and the
// Creator 5 Pro (toolheads) publish the same schema, so both readers parse it
// here.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "hv/json.hpp"

namespace helix::zmod_color {

struct Slot {
    std::string material; ///< Empty when the firmware reports its "?" unset sentinel
    std::string hex;      ///< As published, no '#'; may be empty
};

/// `slots[]`, indexed by 1-based `ID` minus one and sized @p max_slots. Entries
/// the frame did not carry, or carried malformed, stay nullopt. nullopt overall
/// when the object has no `slots` array: a delta frame that did not change it.
std::optional<std::vector<std::optional<Slot>>> parse_slots(const nlohmann::json& obj,
                                                            int max_slots);

/// `palette[]` as (index, 0xRRGGBB) in firmware index order. An unparseable
/// entry is skipped and the rest keep their own index. nullopt when absent.
std::optional<std::vector<std::pair<int, std::uint32_t>>> parse_palette(const nlohmann::json& obj);

/// `valid_types[]` without the "?" sentinel. nullopt when absent.
std::optional<std::vector<std::string>> parse_valid_types(const nlohmann::json& obj);

} // namespace helix::zmod_color
```

`src/printer/zmod_color_status.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "zmod_color_status.h"

#include "color_utils.h"

namespace helix::zmod_color {

namespace {

/// The module emits ID as str(i); an int is accepted too, since this is
/// someone else's schema.
std::optional<int> slot_id(const nlohmann::json& entry) {
    auto it = entry.find("ID");
    if (it == entry.end()) {
        return std::nullopt;
    }
    if (it->is_number_integer()) {
        return it->get<int>();
    }
    if (it->is_string()) {
        try {
            return std::stoi(it->get<std::string>());
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

} // namespace

std::optional<std::vector<std::optional<Slot>>> parse_slots(const nlohmann::json& obj,
                                                            int max_slots) {
    auto it = obj.find("slots");
    if (it == obj.end() || !it->is_array() || max_slots <= 0) {
        return std::nullopt;
    }
    std::vector<std::optional<Slot>> out(static_cast<size_t>(max_slots));
    for (const auto& entry : *it) {
        if (!entry.is_object()) {
            continue;
        }
        auto id = slot_id(entry);
        if (!id || *id < 1 || *id > max_slots) {
            continue;
        }
        Slot slot;
        if (auto mat = entry.find("Material"); mat != entry.end() && mat->is_string()) {
            slot.material = mat->get<std::string>();
            if (slot.material == "?") {
                slot.material.clear();
            }
        }
        if (auto hex = entry.find("HEX"); hex != entry.end() && hex->is_string()) {
            slot.hex = hex->get<std::string>();
        }
        out[static_cast<size_t>(*id - 1)] = std::move(slot);
    }
    return out;
}

std::optional<std::vector<std::pair<int, std::uint32_t>>> parse_palette(const nlohmann::json& obj) {
    auto it = obj.find("palette");
    if (it == obj.end() || !it->is_array()) {
        return std::nullopt;
    }
    std::vector<std::pair<int, std::uint32_t>> out;
    int index = 0;
    for (const auto& entry : *it) {
        if (entry.is_string()) {
            if (auto rgb = parse_hex_color(entry.get<std::string>())) {
                out.emplace_back(index, *rgb);
            }
        }
        ++index;
    }
    return out;
}

std::optional<std::vector<std::string>> parse_valid_types(const nlohmann::json& obj) {
    auto it = obj.find("valid_types");
    if (it == obj.end() || !it->is_array()) {
        return std::nullopt;
    }
    std::vector<std::string> out;
    for (const auto& entry : *it) {
        if (entry.is_string() && entry.get<std::string>() != "?") {
            out.push_back(entry.get<std::string>());
        }
    }
    return out;
}

} // namespace helix::zmod_color
```

`src/printer/ams_backend_ad5x_ifs.cpp`: in `read_zmod_color_object`, replace everything from `auto slots_it = obj.find("slots");` up to (not including) `if (saw_slot) {` with the shared reader, keeping the `ifs`/`channel` reads above it and the `saw_slot` tail unchanged:

```cpp
    auto slots = helix::zmod_color::parse_slots(obj, NUM_PORTS);
    if (!slots) {
        return false;
    }
    bool saw_slot = false;
    for (size_t i = 0; i < slots->size(); ++i) {
        if (!(*slots)[i]) {
            continue;
        }
        ZColorSlot slot;
        slot.material = (*slots)[i]->material;
        slot.hex = (*slots)[i]->hex;
        result.slots[i] = std::move(slot);
        saw_slot = true;
    }
```
Add `#include "zmod_color_status.h"` there. The "?" sentinel comment moves with the logic into `parse_slots`; delete the AD5X copy.

- [ ] **Step 4: Run**

Run: `make t F='[zmod_color]'` then `./build/bin/helix-tests '[zmod_status]'` and `./build/bin/helix-tests '[ad5x_ifs]'`
Expected: all passed. The Makefile picks up `src/printer/*.cpp` by wildcard; if the commit hook reports an unclassified source file (ESP32 manifest), classify it the way the hook's message says.

- [ ] **Step 5: Mutate, commit**

Remove the `"?"` clear in `parse_slots`; both `[zmod_color]` and `[zmod_status]` should report a failure (the AD5X tests pin "?" too; if they do not, say so in the commit body). Restore.

```bash
git add include/zmod_color_status.h src/printer/zmod_color_status.cpp tests/unit/test_zmod_color_status.cpp
git commit -m "refactor(zmod): one reader for the zmod_color status object" \
  -m "mutation: keeping the '?' sentinel as a material turned [zmod_color] red." \
  -- include/zmod_color_status.h src/printer/zmod_color_status.cpp src/printer/ams_backend_ad5x_ifs.cpp tests/unit/test_zmod_color_status.cpp
git show --stat HEAD
```
(`git add` is fine here: this is your own worktree, not the shared main tree.)

---

### Task 4: The Z-Mod provider row (detection, tool reading, swap commands)

**Files:**
- Create: `tests/test_helpers/toolchanger_test_helper.h` (moved `ToolChangerHelper`)
- Modify: `tests/unit/test_toolchanger_medusahc.cpp` (use the shared helper)
- Modify: `include/toolchanger_addon.h` (header comment), `src/printer/toolchanger_addon.cpp`
- Test: `tests/unit/test_toolchanger_zmod.cpp` (new)

**Interfaces:**
- Consumes: `toolchanger_addon::ToolReading`, `ToolCommands`, `resolve_tool_commands`, `resolve_tool_sensor`, `read_tool`, `feeder_macro_candidates` (existing).
- Produces: provider named `"Creator 5 Pro"`; `read_tool()` reads `zmod_color.active_tool_id`; `feeder_macro_candidates()` is empty for a provider with no feeder.
- Produces (test helper): `helix::test::ToolChangerHelper(int tool_count)` with `feed(json)`, `feed_status(const char*, int)`, `sent()`.

- [ ] **Step 1: Move the test helper**

Cut `ToolChangerHelper` out of the anonymous namespace in `tests/unit/test_toolchanger_medusahc.cpp` into `tests/test_helpers/toolchanger_test_helper.h` (with `#pragma once`, the SPDX line, and the includes it needs), inside `namespace helix::test`. In `test_toolchanger_medusahc.cpp` include it and add `using helix::test::ToolChangerHelper;`. Run `make t F='[toolchanger]'`: same count as Task 0's baseline, all passed.

- [ ] **Step 2: Write the failing tests** (`tests/unit/test_toolchanger_zmod.cpp`)

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../test_helpers/toolchanger_test_helper.h"
#include "printer_discovery.h"
#include "toolchanger_addon.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
using namespace helix;
using helix::test::ToolChangerHelper;

namespace {

/// Objects a Creator 5 Pro on Z-Mod reports (ghzserg/z_c5pro 1.7: c5pro_generic.cfg,
/// zmod_color.py lookups, stock heaters).
PrinterDiscovery zmod_c5_discovery() {
    json objects = json::array({"zmod", "zmod_color", "save_variables", "extruder", "extruder1",
                                "extruder2", "extruder3", "heater_bed", "toolhead", "gcode_move",
                                "configfile", "gcode_macro OPEN_DOOR"});
    for (int i = 1; i <= 4; ++i) {
        objects.push_back("gcode_button extruder_pos" + std::to_string(i));
        objects.push_back("gcode_button extruder_grab" + std::to_string(i));
    }
    for (int i = 0; i < 4; ++i) {
        objects.push_back("filament_switch_sensor fd_ex" + std::to_string(i));
        objects.push_back("filament_motion_sensor fm_ex" + std::to_string(i));
    }
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    return hw;
}

/// An AD5X on Z-Mod: zmod_color is there too, the carriage grab buttons are not.
PrinterDiscovery zmod_ad5x_discovery() {
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"zmod", "zmod_color", "zmod_ifs", "save_variables", "extruder",
                                  "heater_bed", "toolhead", "gcode_move", "configfile"}));
    return hw;
}

/// The backend holds a mutex, so the helper is built in place and wired here.
void wire_zmod(ToolChangerHelper& tc) {
    auto hw = zmod_c5_discovery();
    tc.set_tool_commands(toolchanger_addon::resolve_tool_commands(hw));
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(hw));
}

} // namespace

TEST_CASE("A Creator 5 Pro on Z-Mod is claimed by the Z-Mod row", "[toolchanger][zmod]") {
    auto hw = zmod_c5_discovery();
    REQUIRE(toolchanger_addon::present(hw));
    CHECK(toolchanger_addon::machine_name(hw) == "Creator 5 Pro");
    CHECK(toolchanger_addon::required_status_objects(hw) ==
          std::vector<std::string>{"zmod_color"});
    CHECK_FALSE(hw.has_tool_changer());
}

TEST_CASE("An AD5X on Z-Mod is not a tool changer", "[toolchanger][zmod]") {
    CHECK_FALSE(toolchanger_addon::present(zmod_ad5x_discovery()));
}

TEST_CASE("Z-Mod swaps with its own commands", "[toolchanger][zmod][commands]") {
    auto cmds = toolchanger_addon::resolve_tool_commands(zmod_c5_discovery());
    REQUIRE(cmds.present);
    CHECK(cmds.select_prefix == "_T_IN T=");
    CHECK(cmds.unselect == "_T_OUT");
}

TEST_CASE("Mounting on Z-Mod sends _T_IN", "[toolchanger][zmod][commands]") {
    ToolChangerHelper tc(4);
    wire_zmod(tc);
    tc.feed(json{{"zmod_color", {{"active_tool_id", -1}}}});
    REQUIRE(tc.change_tool(2).success());
    CHECK(tc.sent().back() == "_T_IN T=2");
}

TEST_CASE("Parking on Z-Mod sends _T_OUT", "[toolchanger][zmod][commands]") {
    // Separate from mounting: the helper never acks gcode, so a mount stays in
    // flight and a following unmount would be refused as busy.
    ToolChangerHelper tc(4);
    wire_zmod(tc);
    tc.feed(json{{"zmod_color", {{"active_tool_id", 2}}}});
    REQUIRE(tc.unload_filament(2).success());
    CHECK(tc.sent().back() == "_T_OUT");
}

TEST_CASE("zmod_color.active_tool_id is the carriage reading", "[toolchanger][zmod]") {
    auto mounted = toolchanger_addon::read_tool(json{{"zmod_color", {{"active_tool_id", 2}}}});
    REQUIRE(mounted.has_value());
    CHECK(mounted->current_tool == 2);
    CHECK_FALSE(mounted->sensor_error);

    auto empty = toolchanger_addon::read_tool(json{{"zmod_color", {{"active_tool_id", -1}}}});
    REQUIRE(empty.has_value());
    CHECK(empty->current_tool == -1);
    CHECK_FALSE(empty->sensor_error);

    auto conflict = toolchanger_addon::read_tool(json{{"zmod_color", {{"active_tool_id", -2}}}});
    REQUIRE(conflict.has_value());
    CHECK(conflict->sensor_error);
}

TEST_CASE("A zmod_color frame without active_tool_id is no carriage news", "[toolchanger][zmod]") {
    CHECK_FALSE(toolchanger_addon::read_tool(json{{"zmod_color", {{"slots", json::array()}}}})
                    .has_value());
}

TEST_CASE("The backend follows the Z-Mod carriage", "[toolchanger][zmod]") {
    ToolChangerHelper tc(4);
    wire_zmod(tc);
    tc.feed(json{{"zmod_color", {{"active_tool_id", 1}}}});
    CHECK(tc.get_current_tool() == 1);
    tc.feed(json{{"zmod_color", {{"active_tool_id", -1}}}});
    CHECK(tc.get_current_tool() == -1);
    // At rest, disagreeing buttons are a fault the user has to see.
    tc.feed(json{{"zmod_color", {{"active_tool_id", -2}}}});
    CHECK(tc.get_system_info().action == AmsAction::ERROR);
}

TEST_CASE("A dock sensor fault clears when the sensors agree again", "[toolchanger][zmod]") {
    // Z-Mod publishes no phase word and no toolchanger status, so nothing else
    // would ever move the unit out of the error the fault raised.
    ToolChangerHelper tc(4);
    wire_zmod(tc);
    tc.feed(json{{"zmod_color", {{"active_tool_id", -2}}}});
    REQUIRE(tc.get_system_info().action == AmsAction::ERROR);
    tc.feed(json{{"zmod_color", {{"active_tool_id", 1}}}});
    CHECK(tc.get_system_info().action == AmsAction::IDLE);
    CHECK(tc.get_current_tool() == 1);
}

TEST_CASE("Z-Mod has no feeder and offers no feeder macros", "[toolchanger][zmod][feeder]") {
    auto hw = zmod_c5_discovery();
    CHECK_FALSE(toolchanger_addon::resolve_feeder(hw).present);
    CHECK(toolchanger_addon::feeder_macro_candidates(hw).empty());
}
```

Check `get_current_tool()` is the accessor name in `include/ams_backend.h`; use whatever it is.

- [ ] **Step 3: Run to verify failure**

Run: `make t F='[zmod]'`
Expected: FAIL (`present` false for the C5 objects; `read_tool` returns nullopt; feeder candidates contain `OPEN_DOOR`).

- [ ] **Step 4: Implement** in `src/printer/toolchanger_addon.cpp`

Add after the MedusaHC helpers, before `providers()`:

```cpp
// --- Z-Mod on the FlashForge Creator 5 Pro -----------------------------------
//
// Z-Mod runs FlashForge's own Klipper, which has no klipper-toolchanger. Its
// zmod_color extra mounts a head with `_T_IN T=<n>`, parks it with `_T_OUT`, and
// reads gcode_button extruder_pos1..4 (dock) and extruder_grab1..4 (carriage)
// into zmod_color.active_tool_id: 0..3 mounted, -1 nothing on the carriage, -2
// the buttons disagree. The AD5X Z-Mod publishes zmod_color too but has no
// carriage buttons, which is what keeps it out of this row.

constexpr const char* kZmodColorObject = "zmod_color";

bool has_object(const PrinterDiscovery& hw, const char* name) {
    const auto& objects = hw.printer_objects();
    return std::find(objects.begin(), objects.end(), name) != objects.end();
}

bool zmod_c5_detect(const PrinterDiscovery& hw) {
    return has_object(hw, kZmodColorObject) && has_object(hw, "gcode_button extruder_grab1");
}

std::vector<std::string> zmod_c5_status_objects(const PrinterDiscovery& /*hw*/) {
    return {kZmodColorObject};
}

/// The firmware's own -2 is reported as a sensor error, not derived here.
std::optional<ToolReading> read_zmod_color(const nlohmann::json& obj) {
    auto tool = int_field(obj, "active_tool_id");
    if (!tool) {
        return std::nullopt;
    }
    ToolReading r;
    r.current_tool = *tool;
    r.sensor_error = (*tool == -2);
    return r;
}
```

`int_field` is defined further down in the same anonymous namespace; move `read_zmod_color` below it, or move `int_field` up. Keep the anonymous namespace.

Add the table row after MedusaHC:

```cpp
        {"Creator 5 Pro", zmod_c5_detect, zmod_c5_status_objects, nullptr, nullptr, "_T_IN T=",
         "_T_OUT"},
```

In `feeder_macro_candidates`, replace `if (!match(hw)) { return out; }` with:

```cpp
    const Provider* p = match(hw);
    if (!p || !p->open_gcode) {
        return out;
    }
```

At the end of `read_tool`, before `return std::nullopt;`:

```cpp
    auto zmod = status.find(kZmodColorObject);
    if (zmod != status.end() && zmod->is_object()) {
        if (auto r = read_zmod_color(*zmod)) {
            return r;
        }
    }
```

In `src/printer/ams_backend_toolchanger.cpp`, `apply_tool_sensor_locked`: the fault branch sets
`system_info_.action = AmsAction::ERROR` with `operation_detail = "sensor error"`, and nothing but a
phase word or a `toolchanger` status ever leaves it. Replace the bare `sensor_error_ = false;` after
the fault branch with:

```cpp
    // The fault this function raised is withdrawn once the sensors agree again.
    // A changer that publishes no phase word (Z-Mod) has nothing else to end it.
    if (sensor_error_ && system_info_.action == AmsAction::ERROR &&
        system_info_.operation_detail == "sensor error") {
        system_info_.action = AmsAction::IDLE;
        system_info_.operation_detail.clear();
    }
    sensor_error_ = false;
```

In `include/toolchanger_addon.h`, rewrite the top comment's first paragraphs so the module is "the one place that knows each tool changer's dialect: the add-on hardware klipper-toolchanger does not model (MedusaHC), and changer firmware that has no klipper-toolchanger at all (Z-Mod on the Creator 5 Pro)". Keep the MedusaHC specifics and the "adding a machine means adding one Provider" rule. Update the `Provider` struct comment "One machine bolted onto klipper-toolchanger" to "One machine this module knows the dialect of".

- [ ] **Step 5: Run**

Run: `make t F='[zmod]'` then `./build/bin/helix-tests '[toolchanger]'` and `./build/bin/helix-tests '[medusahc]'`
Expected: all passed.

- [ ] **Step 6: Mutate, commit**

Two mutations, each restored: drop the `gcode_button extruder_grab1` half of `zmod_c5_detect` ("An AD5X on Z-Mod is not a tool changer" goes red); remove the fault-withdrawal block ("A dock sensor fault clears" goes red).

```bash
git add tests/test_helpers/toolchanger_test_helper.h tests/unit/test_toolchanger_zmod.cpp
git commit -m "feat(toolchanger): Z-Mod on the Creator 5 Pro mounts with _T_IN and reads its carriage from zmod_color (prestonbrown/helixscreen#1714)" \
  -m "mutation: detecting on zmod_color alone claimed the AD5X, and without the withdrawal a sensor fault never cleared; both cases went red." \
  -- include/toolchanger_addon.h src/printer/toolchanger_addon.cpp tests/test_helpers/toolchanger_test_helper.h tests/unit/test_toolchanger_medusahc.cpp tests/unit/test_toolchanger_zmod.cpp
git show --stat HEAD
```

---

### Task 5: Firmware material source, read side

**Files:**
- Modify: `include/toolchanger_addon.h`, `src/printer/toolchanger_addon.cpp`
- Modify: `include/ams_backend.h` (`set_material_source` virtual next to `set_tool_commands`)
- Modify: `include/ams_backend_toolchanger.h`, `src/printer/ams_backend_toolchanger.cpp`
- Modify: `src/printer/ams_state.cpp` (wiring beside `set_tool_commands`)
- Test: `tests/unit/test_toolchanger_zmod.cpp`

**Interfaces:**
- Consumes: `helix::zmod_color::parse_slots/parse_palette/parse_valid_types` (Task 3).
- Produces (namespace `helix::toolchanger_addon`):
  ```cpp
  struct SlotMaterial { std::string material; std::optional<std::uint32_t> rgb; };
  struct MaterialReading {
      std::optional<std::vector<std::optional<SlotMaterial>>> slots;
      std::optional<std::vector<std::string>> valid_types;
      std::optional<std::vector<std::pair<int, std::uint32_t>>> palette;
  };
  struct MaterialSource {
      bool present = false;
      std::string provider_name;
      std::string (*write_gcode)(int slot_index, const std::string& type, std::uint32_t rgb) = nullptr;
  };
  MaterialSource resolve_material_source(const PrinterDiscovery& hw);
  std::optional<MaterialReading> read_materials(const nlohmann::json& status, int max_slots);
  ```
- Produces (backend): `void AmsBackend::set_material_source(helix::toolchanger_addon::MaterialSource)` (default no-op); `AmsBackendToolChanger` members `material_source_`, `firmware_valid_types_`, `firmware_palette_`, `firmware_slots_seen_`; `get_supported_materials()` override.

- [ ] **Step 1: Write the failing tests** (append to `tests/unit/test_toolchanger_zmod.cpp`; add `#include "lane_source_store.h"` and whatever header declares `helix::ams::reset_lane_sources`)

```cpp
namespace {

json full_zmod_color_frame() {
    json palette = json::array();
    for (const char* hex : {"FFFFFF", "FEF043", "0ACC38", "F72224", "161616"}) {
        palette.push_back(hex);
    }
    return json{{"zmod_color",
                 {{"active_tool_id", -1},
                  {"valid_types", json::array({"PLA", "PETG", "ABS", "?"})},
                  {"palette", palette},
                  {"slots", json::array({{{"ID", "1"}, {"Material", "PLA"}, {"HEX", "FFFFFF"}},
                                         {{"ID", "2"}, {"Material", "PETG"}, {"HEX", "0ACC38"}},
                                         {{"ID", "3"}, {"Material", "?"}, {"HEX", ""}},
                                         {{"ID", "4"}, {"Material", "ABS"}, {"HEX", "161616"}}})}}}};
}

void wire_material_source(ToolChangerHelper& tc) {
    tc.set_material_source(toolchanger_addon::resolve_material_source(zmod_c5_discovery()));
}

} // namespace

TEST_CASE("Z-Mod publishes each head's material and colour", "[toolchanger][zmod][material]") {
    helix::ams::reset_lane_sources();
    ToolChangerHelper tc(4);
    wire_zmod(tc);
    wire_material_source(tc);
    tc.feed(full_zmod_color_frame());

    auto vc = helix::ams::lane_sources(tc.lane_id(1)).vendor_cache;
    REQUIRE(vc.has_value());
    CHECK(vc->material == std::optional<std::string>("PETG"));
    CHECK(vc->color_rgb == std::optional<uint32_t>(0x0ACC38));

    const auto info = tc.get_system_info();
    CHECK(info.units[0].slots[1].material == "PETG");
    CHECK(info.units[0].slots[1].color_rgb == 0x0ACC38);
    // "?" is the firmware saying nothing is set.
    auto unset = helix::ams::lane_sources(tc.lane_id(2)).vendor_cache;
    REQUIRE(unset.has_value());
    CHECK_FALSE(unset->material.has_value());
}

TEST_CASE("A frame without slots leaves the firmware reading standing",
          "[toolchanger][zmod][material]") {
    helix::ams::reset_lane_sources();
    ToolChangerHelper tc(4);
    wire_zmod(tc);
    wire_material_source(tc);
    tc.feed(full_zmod_color_frame());
    tc.feed(json{{"zmod_color", {{"active_tool_id", 1}}}});

    auto vc = helix::ams::lane_sources(tc.lane_id(1)).vendor_cache;
    REQUIRE(vc.has_value());
    CHECK(vc->material == std::optional<std::string>("PETG"));
}

TEST_CASE("Z-Mod's valid types are the supported materials", "[toolchanger][zmod][material]") {
    ToolChangerHelper tc(4);
    wire_zmod(tc);
    wire_material_source(tc);
    CHECK_FALSE(tc.get_supported_materials().has_value());
    tc.feed(full_zmod_color_frame());
    auto types = tc.get_supported_materials();
    REQUIRE(types.has_value());
    CHECK(*types == std::vector<std::string>{"PLA", "PETG", "ABS"});
}

TEST_CASE("A changer without a material source files no firmware reading",
          "[toolchanger][zmod][material]") {
    helix::ams::reset_lane_sources();
    ToolChangerHelper tc(4); // no wire_material_source
    wire_zmod(tc);
    tc.feed(full_zmod_color_frame());
    CHECK_FALSE(helix::ams::lane_sources(tc.lane_id(1)).vendor_cache.has_value());
    CHECK_FALSE(tc.get_supported_materials().has_value());
}
```

Confirm `lane_id()` is reachable from a subclass-derived test helper (it is `public` or `protected` in `AmsBackend`; if protected, add a `using AmsBackend::lane_id;` in the helper's public section).

- [ ] **Step 2: Run to verify failure**

Run: `make t F='[material]'`
Expected: FAIL to compile (`set_material_source`, `resolve_material_source` missing).

- [ ] **Step 3: Implement the addon side**

`include/toolchanger_addon.h`, after `struct ToolSensor`:

```cpp
/// Material and colour for one slot, as the firmware stores them.
struct SlotMaterial {
    std::string material;             ///< Empty when the firmware has none set
    std::optional<std::uint32_t> rgb; ///< nullopt when unset or unparseable
};

/// What a firmware material source said in one status frame. Each field is
/// nullopt when the frame did not carry it: Moonraker republishes only what
/// CHANGED, so absence is never "cleared".
struct MaterialReading {
    std::optional<std::vector<std::optional<SlotMaterial>>> slots;
    std::optional<std::vector<std::string>> valid_types;
    std::optional<std::vector<std::pair<int, std::uint32_t>>> palette; ///< (index, 0xRRGGBB)
};

/// A changer whose firmware stores each slot's material and colour itself.
/// Default-constructed means HelixScreen's own store is the only one.
struct MaterialSource {
    bool present = false;
    std::string provider_name;
    /// Gcode that stores @p type and @p rgb for @p slot_index (0-based). @p type
    /// must already be one of the firmware's valid types, safe for a gcode line,
    /// and @p rgb one of its palette colours.
    std::string (*write_gcode)(int slot_index, const std::string& type, std::uint32_t rgb) = nullptr;
};

/// The firmware material source this printer has, or an absent capability.
MaterialSource resolve_material_source(const PrinterDiscovery& hw);

/// Pull a material reading out of a status frame. nullopt means no news.
std::optional<MaterialReading> read_materials(const nlohmann::json& status, int max_slots);
```
Add `#include <cstdint>` and `#include <utility>` to the header.

`src/printer/toolchanger_addon.cpp`: add `#include "color_utils.h"`, `#include "zmod_color_status.h"`, `#include <spdlog/fmt/fmt.h>`. Extend `struct Provider` with a last member:

```cpp
    /// Stores a slot's material and colour in firmware, or nullptr when the
    /// firmware keeps no such record.
    std::string (*material_write_gcode)(int slot_index, const std::string& type, std::uint32_t rgb);
```
MedusaHC row gets `nullptr` as its last field. Z-Mod writer, next to the other Z-Mod helpers:

```cpp
/// HEX and TYPE together write without opening a prompt; SILENT=1 keeps the
/// GET_ZCOLOR that CHANGE_ZCOLOR runs afterwards from opening one in
/// Mainsail/Fluidd.
std::string zmod_c5_material_write(int slot_index, const std::string& type, std::uint32_t rgb) {
    return fmt::format("CHANGE_ZCOLOR SLOT={} HEX={:06X} TYPE={} SILENT=1",
                       slot_index + 1, // DISPLAY_NUMBERING_OK: gcode wire, not a label
                       rgb & 0xFFFFFFu, type);
}
```
Z-Mod row: append `zmod_c5_material_write` as the last field.

Free functions, after `required_status_objects`:

```cpp
MaterialSource resolve_material_source(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    if (!p || !p->material_write_gcode) {
        return {};
    }
    MaterialSource s;
    s.present = true;
    s.provider_name = p->name;
    s.write_gcode = p->material_write_gcode;
    return s;
}

std::optional<MaterialReading> read_materials(const nlohmann::json& status, int max_slots) {
    if (!status.is_object()) {
        return std::nullopt;
    }
    auto it = status.find(kZmodColorObject);
    if (it == status.end() || !it->is_object()) {
        return std::nullopt;
    }
    MaterialReading r;
    if (auto slots = zmod_color::parse_slots(*it, max_slots)) {
        std::vector<std::optional<SlotMaterial>> out(slots->size());
        for (size_t i = 0; i < slots->size(); ++i) {
            if (!(*slots)[i]) {
                continue;
            }
            SlotMaterial m;
            m.material = (*slots)[i]->material;
            m.rgb = parse_hex_color((*slots)[i]->hex);
            out[i] = std::move(m);
        }
        r.slots = std::move(out);
    }
    r.valid_types = zmod_color::parse_valid_types(*it);
    r.palette = zmod_color::parse_palette(*it);
    if (!r.slots && !r.valid_types && !r.palette) {
        return std::nullopt;
    }
    return r;
}
```
`parse_hex_color("")` returns nullopt, which is the "unset" answer we want.

- [ ] **Step 4: Implement the backend side**

`include/ams_backend.h`, beside `set_tool_commands`:

```cpp
    /// Firmware that stores each slot's material and colour. Only tool changers use it.
    virtual void set_material_source(helix::toolchanger_addon::MaterialSource source) {
        (void)source;
    }
```

`include/ams_backend_toolchanger.h`: override `set_material_source` the way `set_tool_commands` is overridden (store under `mutex_`), declare `std::optional<std::vector<std::string>> get_supported_materials() const override;`, `void apply_material_reading_locked(const helix::toolchanger_addon::MaterialReading& reading);`, and members next to `tool_commands_`:

```cpp
    helix::toolchanger_addon::MaterialSource material_source_;
    /// Latched from the material source. Each arrives only in frames that changed it.
    std::optional<std::vector<std::string>> firmware_valid_types_;
    std::optional<std::vector<std::pair<int, std::uint32_t>>> firmware_palette_;
    bool firmware_slots_seen_ = false;
```

`src/printer/ams_backend_toolchanger.cpp`, in `handle_status_update` right after the `tool_sensor_` block:

```cpp
        if (material_source_.present) {
            if (auto reading = helix::toolchanger_addon::read_materials(
                    params, static_cast<int>(tool_names_.size()))) {
                apply_material_reading_locked(*reading);
                state_changed = true;
            }
        }
```

New functions:

```cpp
void AmsBackendToolChanger::apply_material_reading_locked(
    const helix::toolchanger_addon::MaterialReading& reading) {
    if (reading.valid_types) {
        firmware_valid_types_ = *reading.valid_types;
    }
    if (reading.palette) {
        firmware_palette_ = *reading.palette;
    }
    if (!reading.slots) {
        return;
    }
    firmware_slots_seen_ = true;
    for (size_t i = 0; i < reading.slots->size(); ++i) {
        const auto& slot = (*reading.slots)[i];
        if (!slot) {
            continue;
        }
        // The firmware's statement of what this head holds, replaced whole each
        // time the frame carries it.
        helix::ams::Observation vendor(helix::ams::ObservationSource::VendorCache);
        if (!slot->material.empty()) {
            vendor.material = slot->material;
        }
        if (slot->rgb) {
            vendor.color_rgb = *slot->rgb;
        }
        helix::ams::ingest(lane_id(static_cast<int>(i)), vendor);
    }
}

std::optional<std::vector<std::string>> AmsBackendToolChanger::get_supported_materials() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!material_source_.present || !firmware_valid_types_ || firmware_valid_types_->empty()) {
        return std::nullopt;
    }
    return firmware_valid_types_;
}
```

`src/printer/ams_state.cpp`, after `backend->set_tool_commands(...)`:

```cpp
        backend->set_material_source(helix::toolchanger_addon::resolve_material_source(hardware));
```

- [ ] **Step 5: Run**

Run: `make t F='[zmod]'` then `./build/bin/helix-tests '[toolchanger]'` and `./build/bin/helix-tests '[ams]'`
Expected: all passed.

- [ ] **Step 6: Mutate, commit**

Make `apply_material_reading_locked` clear `firmware_slots_seen_`/skip ingest when `reading.slots` is absent AND drop the `if (!reading.slots) return;` guard so a slot-less frame files empty observations: "A frame without slots leaves the firmware reading standing" must go red. Restore.

```bash
git commit -m "feat(toolchanger): read each head's material and colour from Z-Mod (prestonbrown/helixscreen#1714)" \
  -m "mutation: filing observations from a slot-less delta frame erased the heads' materials; the delta-frame case went red." \
  -- include/toolchanger_addon.h src/printer/toolchanger_addon.cpp include/ams_backend.h include/ams_backend_toolchanger.h src/printer/ams_backend_toolchanger.cpp src/printer/ams_state.cpp tests/unit/test_toolchanger_zmod.cpp
git show --stat HEAD
```

---

### Task 6: Firmware material source, write side

**Files:**
- Modify: `include/ams_backend.h`, `src/printer/ams_backend.cpp` (`AmsBackend::commit_user_edit`)
- Modify: `include/ams_backend_toolchanger.h`, `src/printer/ams_backend_toolchanger.cpp` (`apply_user_edit`, new override)
- Test: `tests/unit/test_toolchanger_zmod.cpp`

**Interfaces:**
- Consumes: `material_source_`, `firmware_palette_`, `firmware_slots_seen_` (Task 5); `helix::nearest_palette_key` (Task 2); `AmsBackend::normalize_material()`, `IMoonrakerAPI::is_safe_material_param()`, `IMoonrakerAPI::gcode_param_value()` (existing, static).
- Produces: `virtual bool AmsBackend::firmware_stores_color_and_material(int slot_index) const` (default false).

- [ ] **Step 1: Write the failing tests** (append to `tests/unit/test_toolchanger_zmod.cpp`)

```cpp
namespace {

/// Slot as the editor hands it over: the current slot with the user's change.
SlotInfo edited(const ToolChangerHelper& tc, int slot, std::optional<uint32_t> rgb,
                std::optional<std::string> material) {
    SlotInfo info = tc.get_system_info().units[0].slots[static_cast<size_t>(slot)];
    if (rgb) {
        info.color_rgb = *rgb;
    }
    if (material) {
        info.material = *material;
    }
    return info;
}

size_t zcolor_sends(const ToolChangerHelper& tc) {
    size_t n = 0;
    for (const auto& g : tc.sent()) {
        n += g.rfind("CHANGE_ZCOLOR", 0) == 0 ? 1 : 0;
    }
    return n;
}

} // namespace

TEST_CASE("A colour outside the palette is snapped", "[toolchanger][zmod][material][write]") {
    helix::ams::reset_lane_sources();
    ToolChangerHelper tc(4);
    wire_zmod(tc);
    wire_material_source(tc);
    tc.feed(full_zmod_color_frame());

    const SlotInfo before = tc.get_system_info().units[0].slots[0];
    REQUIRE(tc.commit_user_edit(0, before, edited(tc, 0, 0xE01010, std::nullopt)).success());
    REQUIRE_FALSE(tc.sent().empty());
    CHECK(tc.sent().back() == "CHANGE_ZCOLOR SLOT=1 HEX=F72224 TYPE=PLA SILENT=1");
}

TEST_CASE("A material outside the firmware's types is mapped to one of them",
          "[toolchanger][zmod][material][write]") {
    helix::ams::reset_lane_sources();
    ToolChangerHelper tc(4);
    wire_zmod(tc);
    wire_material_source(tc);
    tc.feed(full_zmod_color_frame());

    const SlotInfo before = tc.get_system_info().units[0].slots[1];
    REQUIRE(tc.commit_user_edit(1, before, edited(tc, 1, std::nullopt, "PETG-CF")).success());
    CHECK(tc.sent().back() == "CHANGE_ZCOLOR SLOT=2 HEX=0ACC38 TYPE=PETG SILENT=1");
}

TEST_CASE("The firmware owns colour and material: an edit declares neither",
          "[toolchanger][zmod][material][write]") {
    helix::ams::reset_lane_sources();
    ToolChangerHelper tc(4);
    wire_zmod(tc);
    wire_material_source(tc);
    tc.feed(full_zmod_color_frame());
    REQUIRE(tc.firmware_stores_color_and_material(0));

    SlotInfo info = edited(tc, 0, 0xF72224, "ABS");
    info.brand = "Polymaker";
    const SlotInfo before = tc.get_system_info().units[0].slots[0];
    REQUIRE(tc.commit_user_edit(0, before, info).success());

    auto user = helix::ams::lane_sources(tc.lane_id(0)).local_user;
    REQUIRE(user.has_value());
    CHECK_FALSE(user->color_rgb.has_value());
    CHECK_FALSE(user->material.has_value());
    CHECK(user->brand == std::optional<std::string>("Polymaker"));
}

TEST_CASE("An edit that changes neither colour nor material sends nothing",
          "[toolchanger][zmod][material][write]") {
    helix::ams::reset_lane_sources();
    ToolChangerHelper tc(4);
    wire_zmod(tc);
    wire_material_source(tc);
    tc.feed(full_zmod_color_frame());

    SlotInfo info = tc.get_system_info().units[0].slots[0];
    const SlotInfo before = info;
    info.brand = "Polymaker";
    REQUIRE(tc.commit_user_edit(0, before, info).success());
    CHECK(zcolor_sends(tc) == 0);
}

TEST_CASE("Before the firmware publishes slots an edit stays local",
          "[toolchanger][zmod][material][write]") {
    helix::ams::reset_lane_sources();
    ToolChangerHelper tc(4);
    wire_zmod(tc);
    wire_material_source(tc);
    tc.feed(json{{"zmod_color", {{"active_tool_id", -1}}}}); // a Z-Mod without the export
    CHECK_FALSE(tc.firmware_stores_color_and_material(0));

    const SlotInfo before = tc.get_system_info().units[0].slots[0];
    REQUIRE(tc.commit_user_edit(0, before, edited(tc, 0, 0xF72224, "ABS")).success());
    CHECK(zcolor_sends(tc) == 0);
    auto user = helix::ams::lane_sources(tc.lane_id(0)).local_user;
    REQUIRE(user.has_value());
    CHECK(user->color_rgb == std::optional<uint32_t>(0xF72224));
}

TEST_CASE("An unsafe type is refused", "[toolchanger][zmod][material][write]") {
    helix::ams::reset_lane_sources();
    ToolChangerHelper tc(4);
    wire_zmod(tc);
    wire_material_source(tc);
    json frame = full_zmod_color_frame();
    frame["zmod_color"]["valid_types"] = json::array({"PLA;M112", "?"});
    tc.feed(frame);

    const SlotInfo before = tc.get_system_info().units[0].slots[0];
    auto err = tc.commit_user_edit(0, before, edited(tc, 0, 0xF72224, "PLA;M112"));
    CHECK_FALSE(err.success());
    CHECK(zcolor_sends(tc) == 0);
}
```

If `SlotInfo` names differ (`color_rgb`, `material`, `brand`), use the real ones from `include/ams_types.h`. If `commit_user_edit` needs an AmsState-owned index for `lane_id`, follow how `tests/unit/test_lane_backend_observations.cpp` drives edits on a tool changer.

- [ ] **Step 2: Run to verify failure**

Run: `make t F='[write]'`
Expected: FAIL to compile (`firmware_stores_color_and_material` missing).

- [ ] **Step 3: Implement the funnel hook**

`include/ams_backend.h`, near `commit_user_edit`:

```cpp
    /**
     * @brief Whether this slot's colour and material live in firmware the backend
     *        writes through to.
     *
     * A user edit then declares neither: the firmware's echo is the only record,
     * so a change made on the printer is never masked by an older edit here.
     */
    [[nodiscard]] virtual bool firmware_stores_color_and_material(int slot_index) const {
        (void)slot_index;
        return false;
    }
```

`src/printer/ams_backend.cpp`, in `AmsBackend::commit_user_edit`: make `declaration` non-const and, right after it is built:

```cpp
    if (firmware_stores_color_and_material(slot_index)) {
        declaration.color_rgb.reset();
        declaration.color_name.reset();
        declaration.material.reset();
    }
```

- [ ] **Step 4: Implement the write-through** in `AmsBackendToolChanger`

Header: `bool firmware_stores_color_and_material(int slot_index) const override;`

```cpp
bool AmsBackendToolChanger::firmware_stores_color_and_material(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return material_source_.present && material_source_.write_gcode && firmware_slots_seen_ &&
           firmware_palette_ && !firmware_palette_->empty() && slot_index >= 0 &&
           slot_index < static_cast<int>(tool_names_.size());
}
```

In `apply_user_edit`, inside the existing locked block and BEFORE `write_filament_fields(slot, info);`, capture:

```cpp
            firmware_edit = material_source_.present && material_source_.write_gcode &&
                            firmware_slots_seen_ && firmware_palette_ &&
                            !firmware_palette_->empty() &&
                            (slot.color_rgb != info.color_rgb || slot.material != info.material);
            if (firmware_edit) {
                palette = *firmware_palette_;
                write_gcode = material_source_.write_gcode;
            }
```
declaring `bool firmware_edit = false; std::vector<std::pair<int, std::uint32_t>> palette; std::string (*write_gcode)(int, const std::string&, std::uint32_t) = nullptr;` before the block.

After `emit_event(EVENT_SLOT_CHANGED, ...)` and before the remap `if (!physical_tool_name.empty())`:

```cpp
    if (firmware_edit) {
        // Outside mutex_: normalize_material() reads get_supported_materials(),
        // which locks. The firmware stores a type from its own list and a colour
        // from its own palette, so both are mapped before the send.
        const std::string type = normalize_material(info.material);
        if (!IMoonrakerAPI::is_safe_material_param(type)) {
            return AmsErrorHelper::invalid_parameter("Material '" + type +
                                                     "' cannot be stored on this printer");
        }
        const int index = helix::nearest_palette_key(palette, info.color_rgb, -1);
        std::uint32_t rgb = palette.front().second;
        for (const auto& [key, packed] : palette) {
            if (key == index) {
                rgb = packed;
                break;
            }
        }
        AmsError sent = execute_gcode(write_gcode(slot_index, IMoonrakerAPI::gcode_param_value(type), rgb));
        if (!sent.success()) {
            return sent;
        }
    }
```
Add `#include "color_utils.h"` and `#include "i_moonraker_api.h"` if absent. If the `IMoonrakerAPI` class lives in a namespace, qualify accordingly.

- [ ] **Step 5: Run**

Run: `make t F='[write]'` then `./build/bin/helix-tests '[zmod]'`, `'[toolchanger]'`, `'[ams]'`, `'[lane]'`
Expected: all passed. `test_code_lint.bats` guards the lane-store funnels; this change adds no new friend or writer, so it must stay green (checked in Task 8's full run).

- [ ] **Step 6: Mutate, commit**

Two mutations, each restored: (a) remove the `firmware_stores_color_and_material` reset block in `commit_user_edit`: "an edit declares neither" goes red; (b) send `info.color_rgb` instead of the snapped `rgb`: "A colour outside the palette is snapped" goes red.

```bash
git commit -m "feat(toolchanger): Z-Mod stores each head's colour and material; edits write through (prestonbrown/helixscreen#1714)" \
  -m "mutation: declaring colour in the edit funnel masked the firmware's value, and sending the unsnapped colour stored white; both cases went red." \
  -- include/ams_backend.h src/printer/ams_backend.cpp include/ams_backend_toolchanger.h src/printer/ams_backend_toolchanger.cpp tests/unit/test_toolchanger_zmod.cpp
git show --stat HEAD
```

---

### Task 7: Z-Mod mock persona (mock hardware, real backend)

**Files:**
- Modify: `include/moonraker_client_mock.h` (`PrinterType::FLASHFORGE_CREATOR5_ZMOD`, state, `mock_hardware_persona()`)
- Modify: `src/api/moonraker_client_mock.cpp` (`populate_capabilities`, `populate_hardware`, gcode handling)
- Modify: `src/api/moonraker_client_mock_objects.cpp` (`zmod_color` status)
- Modify: `src/application/moonraker_manager.cpp` (`HELIX_MOCK_PRINTER=creator5_zmod`)
- Modify: `src/printer/ams_backend.cpp` (`try_create_mock` defers to real discovery for this persona)
- Modify: `docs/devel/MOCK_ENVIRONMENT_VARIABLES.md`
- Test: `tests/unit/test_mock_creator5_zmod.cpp` (new)

**Interfaces:**
- Produces: `HELIX_MOCK_PRINTER=creator5_zmod`; `static bool MoonrakerClientMock::mock_hardware_persona()` (true for `creator5_zmod`); mock `zmod_color` status with `active_tool_id`, `total_tools`, `color_limit`, `display` (false), `valid_types` (PLA, PETG, PLA-CF, PETG-CF, ABS, ASA, SILK, PET-CF, S-PAHT, S-MULTI, PA-CF, HIPS, PVA, TPU-90A, TPU-95A, TPU-64D, "?"), `hidden_types` ([]), `palette` (the 24 FlashForge colours below), `slots` (4 heads).

Palette, in index order (from Z-Mod's `filament.json` default comment block): `FFFFFF FEF043 DCF478 0ACC38 067749 0C6283 0DE2A0 75D9F3 45A8F9 2750E0 46328E A03CF7 F330F9 D4B0DC F95D73 F72224 7C4B00 F98D33 FDEBD5 D3C4A3 AF7836 898989 BCBCBC 161616`.

- [ ] **Step 1: Write the failing test** (`tests/unit/test_mock_creator5_zmod.cpp`)

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "toolchanger_addon.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
using helix::MoonrakerClientMock;

namespace {

/// Captures notify_status_update frames and waits for a zmod_color one that
/// satisfies a predicate. Dispatch may run on the mock's own thread.
class ZmodFrames {
  public:
    std::function<void(const json&)> callback() {
        return [this](const json& n) {
            std::lock_guard<std::mutex> lock(mutex_);
            frames_.push_back(n);
            cv_.notify_all();
        };
    }

    std::optional<json> wait_for(const std::function<bool(const json&)>& pred,
                                 int timeout_ms = 2000) {
        std::unique_lock<std::mutex> lock(mutex_);
        std::optional<json> hit;
        cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
            for (auto it = frames_.rbegin(); it != frames_.rend(); ++it) {
                const auto& params = (*it)["params"];
                if (params.is_array() && !params.empty() && params[0].contains("zmod_color") &&
                    pred(params[0]["zmod_color"])) {
                    hit = params[0]["zmod_color"];
                    return true;
                }
            }
            return false;
        });
        return hit;
    }

  private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<json> frames_;
};

std::optional<json> slot_with_id(const json& zc, const char* id) {
    if (!zc.contains("slots")) {
        return std::nullopt;
    }
    for (const auto& s : zc["slots"]) {
        if (s.value("ID", "") == id) {
            return s;
        }
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("The Z-Mod C5 persona reports Z-Mod's objects", "[mock][creator5][zmod]") {
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::FLASHFORGE_CREATOR5_ZMOD);
    const helix::PrinterDiscovery hw = mock.hardware();
    REQUIRE(helix::toolchanger_addon::present(hw));
    CHECK(helix::toolchanger_addon::machine_name(hw) == "Creator 5 Pro");
    CHECK_FALSE(hw.has_tool_changer());
    CHECK(hw.tool_names().size() == 4);
    CHECK(helix::toolchanger_addon::resolve_material_source(hw).present);
}

TEST_CASE("The Z-Mod C5 persona mounts, parks and stores colours", "[mock][creator5][zmod]") {
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::FLASHFORGE_CREATOR5_ZMOD);
    ZmodFrames frames;
    mock.register_notify_update(frames.callback());

    mock.gcode_script("_T_IN T=2");
    CHECK(frames.wait_for([](const json& zc) { return zc.value("active_tool_id", -9) == 2; }));

    mock.gcode_script("_T_OUT");
    CHECK(frames.wait_for([](const json& zc) { return zc.value("active_tool_id", -9) == -1; }));

    mock.gcode_script("CHANGE_ZCOLOR SLOT=2 HEX=F72224 TYPE=PETG SILENT=1");
    auto stored = frames.wait_for([](const json& zc) {
        auto s = slot_with_id(zc, "2");
        return s && s->value("Material", "") == "PETG";
    });
    REQUIRE(stored.has_value());
    CHECK(slot_with_id(*stored, "2")->value("HEX", "") == "F72224");

    // An off-palette colour is stored as index 0, white, exactly as the firmware does.
    mock.gcode_script("CHANGE_ZCOLOR SLOT=1 HEX=123456 TYPE=PLA SILENT=1");
    auto white = frames.wait_for([](const json& zc) {
        auto s = slot_with_id(zc, "1");
        return s && s->value("HEX", "") == "FFFFFF";
    });
    CHECK(white.has_value());
}
```

Check `hardware()`, `register_notify_update()` and `gcode_script()` against `include/moonraker_client_mock.h` and `tests/unit/test_moonraker_mock_behavior.cpp`; if the namespace differs (`MoonrakerClientMock` may be at global scope), fix the `using`.

- [ ] **Step 2: Run to verify failure**

Run: `make t F='[mock][zmod]'`
Expected: FAIL to compile (`FLASHFORGE_CREATOR5_ZMOD` missing).

- [ ] **Step 3: Implement the persona**

- `include/moonraker_client_mock.h`: add `FLASHFORGE_CREATOR5_ZMOD, // FlashForge Creator 5 Pro on Z-Mod (no klipper-toolchanger)` beside `FLASHFORGE_CREATOR5`; state members `std::atomic<int> zmod_active_tool_{-1};`, `std::array<std::pair<std::string, std::string>, 4> zmod_slots_` (material, HEX) seeded PLA/FFFFFF, PETG/0ACC38, ABS/161616, PLA/F72224, guarded by a `std::mutex zmod_mutex_`; `json zmod_color_status() const;`; `static bool mock_hardware_persona();`.
- `src/application/moonraker_manager.cpp`: `else if (t == "creator5_zmod") type = ...FLASHFORGE_CREATOR5_ZMOD;` and add it to the help string.
- `populate_capabilities`: a `case PrinterType::FLASHFORGE_CREATOR5_ZMOD:` pushing `zmod`, `zmod_color`, `save_variables`, `gcode_button extruder_pos1..4`, `gcode_button extruder_grab1..4`. In the filament-sensor branch, the ZMOD persona pushes `filament_switch_sensor fd_ex0..3` and `filament_motion_sensor fm_ex0..3`. Kinematics: add the case label beside `FLASHFORGE_CREATOR5` (corexy).
- `populate_hardware`: add `case PrinterType::FLASHFORGE_CREATOR5_ZMOD:` as a fallthrough into the `FLASHFORGE_CREATOR5` case (same heaters, fans, LED).
- `mock_toolchanger_selected()` must stay false for this persona (the real `AmsBackendToolChanger` runs). `mock_hardware_persona()` returns true when `HELIX_MOCK_PRINTER` is `creator5_zmod`.
- `src/printer/ams_backend.cpp`, `try_create_mock`: after the MedusaHC block, and only when `HELIX_MOCK_AMS` is unset:

```cpp
    // Personas that model firmware HelixScreen talks to through a production
    // backend are mock hardware too: decline, and real discovery builds that
    // backend against the mock's objects.
    if (!mock_ams_env && MoonrakerClientMock::mock_hardware_persona()) {
        spdlog::info("[AMS Backend] Mock printer persona is mock hardware - deferring to real "
                     "discovery");
        return nullptr;
    }
```
- `moonraker_client_mock_objects.cpp`: where requested objects are answered, `if (objects.contains("zmod_color")) status_obj["zmod_color"] = self->zmod_color_status();`
- Gcode handling, beside the MedusaHC block and only for this persona: `_T_IN T=<n>` sets `zmod_active_tool_` and dispatches `{"zmod_color": {"active_tool_id": n}}`; `_T_OUT` sets -1 and dispatches; `CHANGE_ZCOLOR SLOT=<n> HEX=<hex> TYPE=<t>` checks `TYPE` against the valid list (refuse with the gcode error path otherwise), stores `HEX` upper-case if it is in the palette else `FFFFFF`, and dispatches `{"zmod_color": {"slots": [...]}}` with all four slots.

- [ ] **Step 4: Run**

Run: `make t F='[mock]'` then `./build/bin/helix-tests '[creator5]'`
Expected: all passed.

- [ ] **Step 5: Drive it live** (pinned socket, own PID only)

```bash
TREE=$(basename "$(git rev-parse --show-toplevel)")
export HELIX_SOCK="/tmp/helix-$TREE.sock" HELIX_CONFIG_DIR="/tmp/helix-config-$TREE"
mkdir -p "$HELIX_CONFIG_DIR"
HELIX_MOCK_PRINTER=creator5_zmod SDL_VIDEODRIVER=dummy ./build/bin/helix-screen --test -vv \
  --remote-socket "$HELIX_SOCK" > /tmp/helix-$TREE.log 2>&1 &
PID=$!
grep -m1 -E 'Creator 5 Pro|deferring to real discovery' /tmp/helix-$TREE.log
./build/bin/helix-screen ctl -s "$HELIX_SOCK" navigate ams
./build/bin/helix-screen ctl -s "$HELIX_SOCK" ls
kill $PID
```
Expected in the log: detection as the Creator 5 Pro, "deferring to real discovery", and an `[AMS ToolChanger]` backend with 4 slots. Record what `ctl` shows for slot materials.

- [ ] **Step 6: Commit**

```bash
git add tests/unit/test_mock_creator5_zmod.cpp
git commit -m "feat(mock): a Creator 5 Pro on Z-Mod persona that runs the real tool changer backend (prestonbrown/helixscreen#1714)" \
  -- include/moonraker_client_mock.h src/api/moonraker_client_mock.cpp src/api/moonraker_client_mock_objects.cpp src/application/moonraker_manager.cpp src/printer/ams_backend.cpp docs/devel/MOCK_ENVIRONMENT_VARIABLES.md tests/unit/test_mock_creator5_zmod.cpp
git show --stat HEAD
```

---

### Task 8: Documentation, full verification, plan removal

**Files:**
- Modify: `docs/devel/FILAMENT_BACKEND_TOOLCHANGER.md` (a "Z-Mod on the Creator 5 Pro" section: detection, `_T_IN`/`_T_OUT`, `active_tool_id`, firmware material source, palette snap, SILENT=1, the stale -2 before ghzserg/z_c5pro#1)
- Modify: `docs/devel/FILAMENT_MANAGEMENT.md` ("Material names as G-code parameter values" table: a Tool changer (Z-Mod) row: `CHANGE_ZCOLOR ... TYPE=`, `normalize_material()` + `is_safe_material_param()` + `gcode_param_value()`)
- Modify: `docs/devel/printers/FLASHFORGE_CREATOR5_PRO_SUPPORT.md` (Z-Mod support status, the `creator5_zmod` persona)
- Delete: `docs/devel/plans/2026-09-24-c5-zmod-toolchanger-provider-design.md`, `docs/devel/plans/2026-09-24-c5-zmod-toolchanger-provider.md` (this change ships the work; durable knowledge now lives in the docs above)

- [ ] **Step 1: Write the docs** (use `path#symbol` citations, not line numbers; `make check-doc-anchors` to confirm they resolve)

- [ ] **Step 2: Full gate**

```bash
make full-test-run > /tmp/c5-zmod-full.log 2>&1; echo "exit=$?"
```
Expected: exit 0; report unit shard count and bats `ok/total` from the log.

- [ ] **Step 3: Mutation sweep**

Run: `make mutate-diff` over the branch. Every hunk in `toolchanger_addon.cpp`, `ams_backend_toolchanger.cpp`, `ams_backend.cpp`, `tool_state.cpp`, `zmod_color_status.cpp` must produce a red test; list any that do not and add the test.

- [ ] **Step 4: Commit docs and plan removal**

```bash
git commit -m "docs(toolchanger): Z-Mod on the Creator 5 Pro (prestonbrown/helixscreen#1714)" \
  -- docs/devel/FILAMENT_BACKEND_TOOLCHANGER.md docs/devel/FILAMENT_MANAGEMENT.md docs/devel/printers/FLASHFORGE_CREATOR5_PRO_SUPPORT.md docs/devel/plans/2026-09-24-c5-zmod-toolchanger-provider-design.md docs/devel/plans/2026-09-24-c5-zmod-toolchanger-provider.md
git show --stat HEAD
```

- [ ] **Step 5: Whole-branch review**

Independent reviewer with only the branch name, prestonbrown/helixscreen#1714 and `docs/devel/REVIEW_RUBRIC.md`. If fresh subagents run on GLM, the reviewer is a fork of the orchestrating session.

**hw-verify (not claimable here):** Z-Mod behaviour on a real Creator 5 Pro once ghzserg/z_c5pro#1 lands (ghzserg); empty-carriage on Reforge (Klipper4FlashForge).
