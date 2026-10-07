# Mock Personas Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give every platform HelixScreen ships a `HELIX_MOCK_PRINTER` persona that `helix-screen --test` auto-detects as the real printer database entry and preset, so the panels match what a real owner of that printer sees.

**Architecture:** One header-only persona module (`include/mock_persona.h`) owns the `PrinterType` enum, a per-type descriptor (hostname, build volume, kinematics, inherited defaults the persona omits, default mock AMS mode, hardware-persona flag) and the single name table that maps `HELIX_MOCK_PRINTER` values to types. Every consumer (MoonrakerManager's selection and warning, the saved-type step, the CLI's `--real-ams` implication, the AMS backend factory, the mock client and `scripts/screenshot.sh --help`) reads that module instead of its own copy. A table-driven test runs each persona through the real discovery sequence and the real `PrinterDetector`, and asserts the database entry and preset it lands on.

**Tech Stack:** C++17, LVGL 9.5 app, Catch2 (`tests/unit/`), bats (`tests/shell/`), GNU make.

**Spec:** the scoping report `/home/pbrown/Code/Printing/helixscreen-mock-personas-scope.md` (outside the repo), plus the scope decisions recorded in this plan's Global Constraints. The scoping report's claims were checked against `main` @ `d19edc1ff`. Its corrections are under "Verified facts".

**Consumer:** HelixBot's screenshot renderer. It builds the **stable** line and picks the persona with `HELIX_MOCK_PRINTER`, so the personas only reach it through Task 12 (the backport). The bot-side platform→persona mapping is not part of this plan.

## Verified facts (checked 2026-10-07 against `main` @ `d19edc1ff`)

These were observed by running `build/bin/helix-screen --test` per persona with an empty `HELIX_CONFIG_DIR` and `SDL_VIDEODRIVER=dummy`, and by reading the code. The plan builds on them.

| Persona | Result today |
|---|---|
| `ad5m` | **FlashForge Adventurer 5M Pro**, 93%, preset `ad5m_pro`. This is wrong: the persona publishes `led chamber_led`, which plain-5M excludes and the Pro requires |
| `voron_24` | Voron 2.4, 100%, autosaved |
| `voron_trident` | Voron Trident, 73%, margin 3. **Not autosaved** (below `AUTOSAVE_MIN_CONFIDENCE = 85`) |
| `creator5` | Creator 5 Pro, 100%, autosaved. It then tries the `creator5_pro_zmod` variant because of `fan_generic fanM106` (out of scope here, see Open questions) |
| `creator5_zmod` | Creator 5 Pro, 98%, preset `creator5_pro` |
| `k1`, `k1max`, `snapmaker_u1` | Never detected: the saved-type step writes `Creality K1C` / `Creality K1 Max` / `Snapmaker U1` before detection runs |
| `generic_corexy` | AD5M Pro 55%, not autosaved |
| `generic_bedslinger`, `delta` | Kobra S1 Max 50%, not autosaved |
| `multi_extruder` | Voron 2.4 63%, not autosaved |

Three facts the scoping report missed:

- **Under `--test`, detection always runs with `kinematics: ''`** (`[PrinterDetector] printer_objects: N, steppers: 0, kinematics: ''`). The discovery sequence reads kinematics from `configfile.config.printer` (`src/printer/printer_discovery_parse.cpp#PrinterDiscovery::parse_config_keys`). The mock's `printer.objects.query` handler builds `configfile.config` without a `printer` section (`src/api/moonraker_client_mock_objects.cpp`, the `configfile` branch). So every `kinematics_match`/`kinematics_exclude` heuristic is dead under `--test`, and fixing only the AD5M kinematics value changes no detection result.
- **The build volume is reported three different ways.** Detection reads `configfile.settings.stepper_*.position_max`, which is always 250/250/300 (`MOCK_BED_*` in `src/api/moonraker_client_mock_internal.h`). The `toolhead.axis_maximum` in the query and subscribe handlers is hard-coded 235/235/250 (`moonraker_client_mock_objects.cpp`, two sites). Only the status pushes in `moonraker_client_mock.cpp` use `persona_axis_maximum()`. So the K1/K1 Max volumes never reach detection, which is probably why those personas "don't clear the detection bar".
- **`k1`/`k1max` need the saved-type step for a separate reason.** `AmsBackendCfs` latches its K1-vs-K2 macro dialect from the saved Config type in its constructor, which runs *before* `auto_detect_and_save` (`src/printer/ams_backend_cfs.cpp`, constructor comment, #968). With `HELIX_MOCK_AMS=cfs` on a fresh config, a K1 persona that only detects naturally would latch the K2 `CR_BOX_*` dialect. So `k1`/`k1max` keep a declared saved type even after they detect naturally. The table carries the declaration (`PersonaEntry::saved_type`) rather than an if-chain. `snapmaker_u1` loses its declaration in Task 7.

Real-machine captures live in `tests/fixtures/printers/` (`creality_k2_plus`, `elegoo_centauri_carbon`, `snapmaker_u1`, `qidi_q2`, `qidi_q2_stock`, …). The personas mirror these captures and do not invent identities. The real CC1 hostname is `cosmos`, the K2 Plus's is `K2Plus-50C1`, and the U1's is `snapmaker-u1`. A stock Q2 reports `linaro-alip` and names itself `QIDI@Q2` through `machine.system_info`.

**`release/1.0` is far behind `main` in every file this plan touches**: the merge-base is 2026-09-03 and main is 5,825 commits ahead. On 1.0, `moonraker_client_mock.cpp` differs by about 3,500 lines and `printer_detector.cpp` by about 1,500. 1.0 has 7 `PrinterType`s (no `k1max`, `creator5*` or `delta`), no `mock_persona.h`, no CFS mock mode (`is_mock_cfs`/`box`) and no `snapmaker_u1` persona. A plain cherry-pick will not apply, so Task 12 is a port.

## Global Constraints

- Work in `.worktrees/mock-personas` on `feature/mock-personas` (created by `scripts/setup-worktree.sh`). Run `scripts/helix-claim take worktree:mock-personas --pid $$` before your first edit and hold it until your last commit lands. Never edit the main tree or `.worktrees/1.0`.
- Mocks are compiled out of release builds: `HELIX_PACKAGING=1` sets `ENABLE_MOCKS=no` (`Makefile`, "Mock backends" block), and `src/api/*_mock*.cpp` is filtered out of `APP_SRCS`. `include/mock_persona.h` is also included by `src/system/cli_args.cpp`, which is always compiled, so the header must stay **header-only and std + `text_io.h` only**: no exceptions, no RTTI, no `std::regex`, no mock headers.
- `HELIX_MOCK_PRINTER` matching stays **exact and case-sensitive**. Unset and `""` both mean "default persona, no warning". An unrecognised value falls back to `voron_24` and logs a warning that lists exactly `persona_ids()`.
- An explicit non-empty `HELIX_MOCK_AMS` always beats a persona's default mock AMS. Every reader of `HELIX_MOCK_AMS` goes through `helix::mock::effective_mock_ams()`.
- An existing persona must keep detecting as it does today (Verified facts table) unless a test in this plan proves today's answer wrong. Today's answers are wrong for `ad5m` (Pro), and for `k1`/`k1max` (never detect).
- Personas mirror `tests/fixtures/printers/<slug>.json` where a capture exists. Never edit a fixture or `assets/config/printer_database.json` to make a persona pass. Fix the persona instead. A database change is a separate decision for Preston.
- Tests: Catch2 in `tests/unit/`, tag `[mock]` plus a specific tag. Every test that constructs `MoonrakerClientMock` with persona behaviour pins `HELIX_MOCK_PRINTER`, `HELIX_MOCK_AMS`, `HELIX_MOCK_OBJECTS`, `HELIX_MOCK_PROBE_TYPE`, `HELIX_MOCK_FILAMENT_SENSORS` and `HELIX_MOCK_KINEMATICS` with `helix::ScopedEnv` (`tests/test_helpers/scoped_env.h`). Shards share processes, and a leaked env var changes the objects list.
- The inner loop is `make t F='[tag]'`. Before every commit, run `make mutate-diff` and name one surviving-to-killed mutation in the commit body (`tests/CLAUDE.md` § "Proving a test can fail"). Commits use plain double-quoted `git commit -m "subject" -m "body"`, with a subject in `type(scope): …` form and a body of about 4 lines. Never `--no-verify`.
- No comment archaeology (`CLAUDE.md` § "Comments describe the code, not its past"). Doc citations use `path#symbol`, never line numbers.
- Push, merge to `main`, or touch `release/1.0` only with Preston's explicit go-ahead.

## Review Focus

1. **A persona-default AMS mode overridden by an explicit `HELIX_MOCK_AMS`.** For example, `HELIX_MOCK_PRINTER=k2 HELIX_MOCK_AMS=none` must show no AMS, and `ad5x` with `HELIX_MOCK_AMS=afc` must show AFC. Pinned in Task 1 (`effective_mock_ams` cases) and Task 6 (`k2` with `none` publishes no `box`).
2. **A config left over from another persona.** A `settings.json` saved with type "Voron 2.4" or "Creality K1C" must not win over the persona being launched. Pinned in Task 1 (`apply_mock_printer_identity` cases).
3. **An env override on top of an omitted default.** A persona that omits the cartographer probe or the `runout_sensor` still honours `HELIX_MOCK_PROBE_TYPE=cartographer` and `HELIX_MOCK_FILAMENT_SENSORS=…`. Pinned in Task 4 (CC1).
4. **`HELIX_MOCK_PRINTER=""` and wrongly cased values (`AD5M`).** Empty is silent default. Wrong case is an unrecognised value with the warning. Pinned in Task 1 (`resolve_persona` cases).
5. **Non-mock builds.** `cli_args.cpp` compiles `mock_persona.h` in every build, including packaging and firmware-adjacent ones. Pinned in Task 1 by a `scripts/syntax_check.py src/system/cli_args.cpp` step and an include-list check.

---

## File structure

| File | Responsibility |
|---|---|
| `include/mock_persona.h` (modify, grows from 20 lines) | `PrinterType`, `PersonaDescriptor` + `descriptor()`, `PERSONAS` table, `find_persona`/`resolve_persona`/`persona_ids`/`effective_mock_ams`/`is_hardware_persona` |
| `include/moonraker_client_mock.h` | `using PrinterType = helix::mock::PrinterType;` replaces the nested enum |
| `src/api/moonraker_client_mock.cpp`, `_objects.cpp`, `_server.cpp`, `_internal.h` | consume `descriptor()`: hostname, volume (configfile, toolhead, mesh, exclude-object grid), kinematics in `configfile.config`, omitted defaults; per-persona hardware/objects |
| `src/api/moonraker_client_mock_gcode.cpp` | `CR_BOX_*` handlers (Task 6) |
| `src/application/moonraker_manager.cpp` + `include/moonraker_manager.h` | table-driven selection; `helix::apply_mock_printer_identity()` |
| `src/system/cli_args.cpp`, `src/printer/ams_backend.cpp` | read `effective_mock_ams()` |
| `scripts/screenshot.sh` | `--printer` help lists ids extracted from `include/mock_persona.h` |
| `tests/test_helpers/printer_capture.h` (create) | capture-fixture loader moved out of `test_printer_detector.cpp` |
| `tests/unit/test_mock_persona.cpp` (create) | table/descriptor/env-resolution unit tests |
| `tests/unit/test_mock_persona_detection.cpp` (create) | per-persona discovery → `PrinterDetector` table test + fixture-fidelity checks |
| `tests/shell/test_screenshot_printer_ids.bats` (create) | help text lists every table id |
| `docs/devel/MOCK_ENVIRONMENT_VARIABLES.md` | `HELIX_MOCK_PRINTER` values + per-persona notes |

---

### Task 1: The persona module and its name consumers (effort: M)

**Files:**
- Modify: `include/mock_persona.h` (full rewrite, interface below)
- Modify: `include/moonraker_client_mock.h` (enum → alias; `mock_toolchanger_selected`, `mock_hardware_persona` keep their signatures)
- Modify: `src/api/moonraker_client_mock.cpp` (`mock_toolchanger_selected`, `mock_hardware_persona`, `mock_medusa_variant`, `is_mock_cfs`, `is_mock_ifs_module` read `effective_mock_ams(getenv("HELIX_MOCK_AMS"), getenv("HELIX_MOCK_PRINTER"))`)
- Modify: `src/application/moonraker_manager.cpp` (`create_client`'s if/else chain → `resolve_persona`; `init`'s saved-type block → `apply_mock_printer_identity`; the `ams_disabled` check → `effective_mock_ams`)
- Modify: `include/moonraker_manager.h` (declare `apply_mock_printer_identity`)
- Modify: `src/system/cli_args.cpp` (the `HELIX_MOCK_AMS` implication block reads `effective_mock_ams`)
- Modify: `src/printer/ams_backend.cpp` (`create_mock_with_client`/`try_create_mock` env reads → `effective_mock_ams`; delete the "Persona implies a tool changer" special case, because `creator5`'s descriptor now says `"toolchanger"`)
- Modify: `scripts/screenshot.sh` (help text)
- Create: `tests/unit/test_mock_persona.cpp`, `tests/shell/test_screenshot_printer_ids.bats`
- Modify: `tests/unit/application/test_moonraker_manager.cpp` (replace the `apply_mock_printer_type_clear` replay with calls to the real function)
- Modify: `docs/devel/MOCK_ENVIRONMENT_VARIABLES.md` (`HELIX_MOCK_PRINTER` row: "Values: see `include/mock_persona.h` `PERSONAS`" plus the current list; file pointer → `include/mock_persona.h`)

**Interfaces — Produces** (this exact header compiled clean through `scripts/syntax_check.py` while the plan was written; the descriptor values reproduce today's behaviour, and later tasks change only the rows they own):

```cpp
// include/mock_persona.h
#include "text_io.h"
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace helix::mock {

enum class PrinterType {
    VORON_24, VORON_TRIDENT, CREALITY_K1, CREALITY_K1_MAX, FLASHFORGE_AD5M,
    FLASHFORGE_CREATOR5, FLASHFORGE_CREATOR5_ZMOD, GENERIC_COREXY, GENERIC_BEDSLINGER,
    MULTI_EXTRUDER, DELTA,
};

using DefaultObjects = std::uint32_t;
namespace default_object {
inline constexpr DefaultObjects NONE = 0;
inline constexpr DefaultObjects HAPPY_HARE_MMU = 1u << 0;  // "mmu"
inline constexpr DefaultObjects CARTOGRAPHER   = 1u << 1;  // probe objects when HELIX_MOCK_PROBE_TYPE is unset
inline constexpr DefaultObjects BME280_CHAMBER = 1u << 2;  // "bme280 chamber"
inline constexpr DefaultObjects HTU21D_DRYER   = 1u << 3;  // "htu21d dryer"
inline constexpr DefaultObjects EBB_CAN_MCU    = 1u << 4;  // "mcu EBBCan"
inline constexpr DefaultObjects CHAMBER_SENSOR = 1u << 5;  // "temperature_sensor chamber" (the unconditional push)
inline constexpr DefaultObjects WIDTH_SENSOR   = 1u << 6;  // "hall_filament_width_sensor"
inline constexpr DefaultObjects RUNOUT_SENSOR  = 1u << 7;  // "filament_switch_sensor runout_sensor" default
inline constexpr DefaultObjects LED_EFFECTS    = 1u << 8;  // led_effect objects + LIGHTS_*/LED_* macros
}

struct AxisMax { double x; double y; double z; };

struct PersonaDescriptor {
    std::string_view hostname;          // printer.info hostname
    AxisMax axis_max;                   // stepper_* position_max == toolhead axis_maximum == mesh/grid bounds
    std::string_view kinematics;        // configfile [printer] kinematics
    DefaultObjects omit;                // inherited defaults this persona does not have
    std::string_view default_mock_ams;  // HELIX_MOCK_AMS when unset; "" = none chosen
    bool hardware_persona;              // production backend drives it (implies --real-ams)
};
[[nodiscard]] constexpr PersonaDescriptor descriptor(PrinterType type);  // switch, every case

struct PersonaEntry {
    std::string_view id;            // HELIX_MOCK_PRINTER value
    PrinterType type;
    std::string_view display_name;  // "[MoonrakerManager] Creating MOCK client (<display_name>, Nx speed)"
    std::string_view saved_type;    // printer type written before detection; "" = let detection decide
};
// clang-format off  -- one row per line: scripts/screenshot.sh extracts ids with sed
inline constexpr std::array<PersonaEntry, 12> PERSONAS = {{ /* rows below */ }};
// clang-format on

[[nodiscard]] inline const PersonaEntry* find_persona(std::string_view id);
[[nodiscard]] inline const PersonaEntry& resolve_persona(const char* env, bool* recognised = nullptr);
[[nodiscard]] inline std::string persona_ids();   // "voron_24, voron_trident, ..." table order
[[nodiscard]] inline std::string effective_mock_ams(const char* ams_env, const char* printer_env);
[[nodiscard]] inline bool is_hardware_persona(std::string_view persona);
}
```

Descriptor rows in Task 1 (all reproduce today):

| Type | hostname | axis_max | kinematics | omit | default_mock_ams | hw |
|---|---|---|---|---|---|---|
| CREALITY_K1 | `mock-printer` | 229/227/255 | corexy | NONE | "" | no |
| CREALITY_K1_MAX | `mock-printer` | 300/307.5/300 | corexy | NONE | "" | no |
| FLASHFORGE_AD5M | `ad5m-mock` | 250/250/300 | cartesian | NONE | "" | no |
| FLASHFORGE_CREATOR5 | `mock-printer` | 250/250/300 | corexy | NONE | `toolchanger` | no |
| FLASHFORGE_CREATOR5_ZMOD | `mock-printer` | 250/250/300 | corexy | HAPPY_HARE_MMU | "" | **yes** |
| VORON_24, VORON_TRIDENT, GENERIC_COREXY | `mock-printer` | 250/250/300 | corexy | NONE | "" | no |
| DELTA | `mock-printer` | 250/250/300 | delta | NONE | "" | no |
| GENERIC_BEDSLINGER, MULTI_EXTRUDER | `mock-printer` | 250/250/300 | cartesian | NONE | "" | no |

`PERSONAS` rows: `voron_24`→VORON_24 "Voron 2.4"; `voron_trident`; `k1`→CREALITY_K1 "Creality K1", saved `Creality K1C`; `k1max`→CREALITY_K1_MAX, saved `Creality K1 Max`; `ad5m` "Flashforge AD5M"; `creator5`; `creator5_zmod`; `generic_corexy`; `generic_bedslinger`; `multi_extruder`; `delta`; `snapmaker_u1`→MULTI_EXTRUDER "Snapmaker U1 (multi-extruder mock)", saved `Snapmaker U1`. `voron_24` must be row 0, because `resolve_persona` falls back to it.

`effective_mock_ams` returns the lowercased explicit value if non-empty, otherwise `descriptor(find_persona(printer_env)->type).default_mock_ams`, otherwise `""`. `is_hardware_persona(id)` returns `find_persona(id) && descriptor(type).hardware_persona`.

```cpp
// include/moonraker_manager.h, namespace helix
/// Settle the saved printer type for a launch with HELIX_MOCK_PRINTER set.
/// A persona that declares a type (PersonaEntry::saved_type) writes it; any
/// other value, recognised or not, clears a stale saved type so detection
/// re-resolves. nullptr or "" leaves config alone. Does not save: returns true
/// when it changed config, and the caller saves.
bool apply_mock_printer_identity(Config& config, const char* mock_printer_env);
```

- [ ] **Step 1: Write the failing unit tests** in `tests/unit/test_mock_persona.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mock_persona.h"

#include <set>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::mock::PrinterType;

namespace {
constexpr PrinterType ALL_TYPES[] = {
    PrinterType::VORON_24, PrinterType::VORON_TRIDENT, PrinterType::CREALITY_K1,
    PrinterType::CREALITY_K1_MAX, PrinterType::FLASHFORGE_AD5M, PrinterType::FLASHFORGE_CREATOR5,
    PrinterType::FLASHFORGE_CREATOR5_ZMOD, PrinterType::GENERIC_COREXY,
    PrinterType::GENERIC_BEDSLINGER, PrinterType::MULTI_EXTRUDER, PrinterType::DELTA,
};
} // namespace

TEST_CASE("Persona ids are unique and every type has a persona", "[mock][persona]") {
    std::set<std::string_view> ids;
    for (const auto& p : helix::mock::PERSONAS) {
        CHECK(ids.insert(p.id).second);
        CHECK_FALSE(p.display_name.empty());
    }
    for (PrinterType t : ALL_TYPES) {
        bool named = false;
        for (const auto& p : helix::mock::PERSONAS) named = named || p.type == t;
        INFO("PrinterType " << static_cast<int>(t));
        CHECK(named);
    }
    CHECK(helix::mock::PERSONAS[0].id == "voron_24");
}

TEST_CASE("resolve_persona: unset and empty are the silent default", "[mock][persona]") {
    bool recognised = false;
    CHECK(helix::mock::resolve_persona(nullptr, &recognised).id == "voron_24");
    CHECK(recognised);
    CHECK(helix::mock::resolve_persona("", &recognised).id == "voron_24");
    CHECK(recognised);
}

TEST_CASE("resolve_persona: exact, case-sensitive match", "[mock][persona]") {
    bool recognised = false;
    CHECK(helix::mock::resolve_persona("k1max", &recognised).type == PrinterType::CREALITY_K1_MAX);
    CHECK(recognised);
    CHECK(helix::mock::resolve_persona("AD5M", &recognised).id == "voron_24");
    CHECK_FALSE(recognised);
    CHECK(helix::mock::resolve_persona("ad5m ", &recognised).id == "voron_24");
    CHECK_FALSE(recognised);
}

TEST_CASE("persona_ids lists the table in order", "[mock][persona]") {
    const std::string ids = helix::mock::persona_ids();
    CHECK(ids.rfind("voron_24, voron_trident, ", 0) == 0);
    for (const auto& p : helix::mock::PERSONAS) {
        CHECK(ids.find(std::string(p.id)) != std::string::npos);
    }
    CHECK(ids.find(", ,") == std::string::npos);
}

TEST_CASE("effective_mock_ams: explicit wins, else the persona default", "[mock][persona][ams]") {
    using helix::mock::effective_mock_ams;
    CHECK(effective_mock_ams("AFC", "creator5") == "afc");
    CHECK(effective_mock_ams("none", "creator5") == "none");
    CHECK(effective_mock_ams(nullptr, "creator5") == "toolchanger");
    CHECK(effective_mock_ams("", "creator5") == "toolchanger");
    CHECK(effective_mock_ams(nullptr, "voron_24").empty());
    CHECK(effective_mock_ams(nullptr, "nonsense").empty());
    CHECK(effective_mock_ams(nullptr, nullptr).empty());
}

TEST_CASE("is_hardware_persona follows the descriptor", "[mock][persona]") {
    CHECK(helix::mock::is_hardware_persona("creator5_zmod"));
    CHECK_FALSE(helix::mock::is_hardware_persona("creator5"));
    CHECK_FALSE(helix::mock::is_hardware_persona("CREATOR5_ZMOD"));
    CHECK_FALSE(helix::mock::is_hardware_persona(""));
}
```

In `tests/unit/application/test_moonraker_manager.cpp`, delete `apply_mock_printer_type_clear` and `ScopedMockPrinterEnv`, and replace the two existing `[mock_printer]` cases with these (they call the production function directly, so the replay can no longer drift):

```cpp
TEST_CASE("apply_mock_printer_identity settles the saved type", "[application][mock_printer]") {
    Config cfg;
    const std::string type_path = cfg.df() + helix::wizard::PRINTER_TYPE;

    SECTION("a detecting persona clears a stale type") {
        cfg.set<std::string>(type_path, "Voron 2.4");
        CHECK(helix::apply_mock_printer_identity(cfg, "ad5m"));
        CHECK(cfg.get<std::string>(type_path, "x").empty());
    }
    SECTION("a declaring persona overwrites a stale type") {
        cfg.set<std::string>(type_path, "Voron 2.4");
        CHECK(helix::apply_mock_printer_identity(cfg, "k1max"));
        CHECK(cfg.get<std::string>(type_path, "") == "Creality K1 Max");
    }
    SECTION("a declaring persona writes into an empty config") {
        CHECK(helix::apply_mock_printer_identity(cfg, "k1"));
        CHECK(cfg.get<std::string>(type_path, "") == "Creality K1C");
    }
    SECTION("an unrecognised value still clears (it runs as voron_24)") {
        cfg.set<std::string>(type_path, "Creality K1C");
        CHECK(helix::apply_mock_printer_identity(cfg, "AD5M"));
        CHECK(cfg.get<std::string>(type_path, "x").empty());
    }
    SECTION("unset and empty leave config alone") {
        cfg.set<std::string>(type_path, "Voron 2.4");
        CHECK_FALSE(helix::apply_mock_printer_identity(cfg, nullptr));
        CHECK_FALSE(helix::apply_mock_printer_identity(cfg, ""));
        CHECK(cfg.get<std::string>(type_path, "") == "Voron 2.4");
    }
    SECTION("nothing saved and nothing declared is no change") {
        CHECK_FALSE(helix::apply_mock_printer_identity(cfg, "voron_24"));
    }
}
```

`tests/shell/test_screenshot_printer_ids.bats`:

```bash
#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# screenshot.sh --help lists the HELIX_MOCK_PRINTER ids from the persona table
# in include/mock_persona.h, so the help cannot name a persona the app rejects.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

table_ids() {
    sed -n 's/^ *{"\([a-z0-9_]*\)", PrinterType::.*/\1/p' "$WORKTREE_ROOT/include/mock_persona.h"
}

@test "the persona table yields at least twelve ids" {
    run table_ids
    [ "$status" -eq 0 ]
    [ "$(printf '%s\n' "$output" | wc -l)" -ge 12 ]
}

@test "screenshot.sh --help lists every persona id" {
    run "$WORKTREE_ROOT/scripts/screenshot.sh" --help
    for id in $(table_ids); do
        [[ "$output" == *"$id"* ]] || { echo "missing: $id"; false; }
    done
}
```

- [ ] **Step 2: Run them to verify they fail.** Run `make t F='[mock][persona]'`. Expected: compile failure (`PERSONAS`, `resolve_persona`, … undeclared), and `make t F='[mock_printer]'` fails the same way on `apply_mock_printer_identity`. Run `bats tests/shell/test_screenshot_printer_ids.bats`. Expected: the first test passes against nothing and fails on the line count. The second fails on `creator5`, which the help does not list today.

- [ ] **Step 3: Implement.**
  - Rewrite `include/mock_persona.h` per the interface. Keep the `PERSONAS` rows one per line inside `// clang-format off/on`.
  - In `MoonrakerClientMock`, replace the nested enum with `using PrinterType = helix::mock::PrinterType;`. All existing `MoonrakerClientMock::PrinterType::X` spellings keep compiling.
  - In `moonraker_manager.cpp`, `create_client` becomes `bool ok; const auto& p = helix::mock::resolve_persona(getenv("HELIX_MOCK_PRINTER"), &ok); if (!ok) spdlog::warn("[MoonrakerManager] HELIX_MOCK_PRINTER='{}' not recognised — falling back to Voron 2.4. Valid: {}.", env, helix::mock::persona_ids());`. It then constructs the mock with `p.type` and logs `p.display_name`. Delete the comment that lists persona names.
  - `init()` calls `if (config && helix::apply_mock_printer_identity(*config, getenv("HELIX_MOCK_PRINTER"))) config->save();` and logs what it did at info level.
  - Route the six `HELIX_MOCK_AMS` readers through `effective_mock_ams`. In `ams_backend.cpp`, `create_mock_with_client` lowercases nothing itself any more. Delete the `mock_toolchanger_selected()` persona fallback from `create_mock_with_client`, because the creator5 default now arrives through `effective_mock_ams`. `MoonrakerClientMock::mock_toolchanger_selected()` keeps its name and becomes `effective == "toolchanger" || "tool_changer" || "tc"`.
  - `scripts/screenshot.sh`: inside `show_help`, compute `ids=$(sed -n 's/^ *{"\([a-z0-9_]*\)", PrinterType::.*/\1/p' "$(dirname "${BASH_SOURCE[0]}")/../include/mock_persona.h" | paste -sd, - | sed 's/,/, /g')`. Print the `--printer` paragraph with `$ids` after the quoted heredoc, or split the heredoc around it. `SCRIPT_DIR` is defined after the help dispatch, so do not use it there.

- [ ] **Step 4: Verify.** Run `make t F='[mock][persona]'`, `make t F='[mock_printer]'`, `make t F='[mock]'` (creator5's tool-changer default must still hold: `test_mock_creator5_zmod.cpp` "The Reforge persona's mock AMS is a tool changer") and `make t F='[ams]'`. Run `bats tests/shell/test_screenshot_printer_ids.bats`. All must pass. Then run `scripts/syntax_check.py src/system/cli_args.cpp`, and `grep -n '^#include' include/mock_persona.h`, which must show only `text_io.h`, `<array>`, `<cstdint>`, `<string>` and `<string_view>` (Review Focus 5). Smoke-test with `SDL_VIDEODRIVER=dummy HELIX_MOCK_PRINTER=AD5M ./build/bin/helix-screen --test -v …` on a pinned socket and config dir (`CLAUDE.md` Quick Start box): the log shows the warning listing all 12 ids and `Creating MOCK client (Voron 2.4`.

- [ ] **Step 5: Commit.**

```bash
git add include/mock_persona.h include/moonraker_client_mock.h include/moonraker_manager.h \
  src/api/moonraker_client_mock.cpp src/application/moonraker_manager.cpp src/system/cli_args.cpp \
  src/printer/ams_backend.cpp scripts/screenshot.sh tests/unit/test_mock_persona.cpp \
  tests/unit/application/test_moonraker_manager.cpp tests/shell/test_screenshot_printer_ids.bats \
  docs/devel/MOCK_ENVIRONMENT_VARIABLES.md
git commit -m "refactor(mock): one persona table feeds selection, saved type, mock AMS default and help" -m "HELIX_MOCK_PRINTER values, their PrinterType, display name and declared saved type live in include/mock_persona.h; MoonrakerManager, cli_args, the AMS factory, the mock client and screenshot.sh --help read it. An empty HELIX_MOCK_PRINTER is now the silent default rather than an unrecognised value. Mutation: <name the hunk you reverted and the test that went red>."
```

---

### Task 2: The mock reports the descriptor consistently (effort: M)

**Files:**
- Modify: `src/api/moonraker_client_mock_internal.h`: delete `MOCK_BED_X_MAX`/`Y_MAX`/`Z_MAX` and `MOCK_MESH_*`, and add `MeshBounds mesh_bounds(PrinterType)` (axis_max minus `MOCK_PROBE_MARGIN`). Keep `MOCK_BED_X_MIN`/`Y_MIN` = 0.
- Modify: `src/api/moonraker_client_mock_objects.cpp`: the query **and** subscribe `configfile` branches use `descriptor(self->get_printer_type()).axis_max` for `stepper_*.position_max` and both `toolhead.axis_maximum` sites. The query branch's `config_section` gains `{"printer", {{"kinematics", self->kinematics()}}}`.
- Modify: `src/api/moonraker_client_mock.cpp`: delete `persona_axis_maximum` and `mock_internal::mock_kinematics` (callers use `descriptor()`). `populate_capabilities`' `mock_config` steppers, the bed-mesh `mesh_min/max` and the `mock_object_entry` grid read the descriptor (`mock_object_entry` becomes a member or takes the bounds). Each default push in `populate_capabilities` is gated on `!(descriptor(printer_type_).omit & default_object::X)`: the `mmu` gate keeps its MedusaHC/IFS-module conditions and replaces the `!= FLASHFORGE_CREATOR5_ZMOD` test with the omit bit. The cartographer bit applies only when `HELIX_MOCK_PROBE_TYPE` is unset, and the runout bit only when `HELIX_MOCK_FILAMENT_SENSORS` is unset.
- Modify: `src/api/moonraker_client_mock_server.cpp`: the `printer.info` hostname is `descriptor(self->get_printer_type()).hostname` (delete the switch).
- Modify: tests that pinned the old 235/235/250 toolhead numbers through the mock. Find them with `grep -rln "235" tests/unit`, then keep only the ones that construct `MoonrakerClientMock`. They now expect the descriptor value. Never weaken an assertion to `> 0`.
- Create: `tests/unit/test_mock_persona_descriptor.cpp`

**Interfaces:**
- Consumes: `helix::mock::descriptor`, `PERSONAS`, `default_object::*` (Task 1).
- Produces: `mock_internal::MeshBounds { double x_min, x_max, y_min, y_max; }` and `mock_internal::MeshBounds mock_internal::mesh_bounds(helix::mock::PrinterType)`. From here on, a persona's volume, hostname and kinematics come from its descriptor row alone.

- [ ] **Step 1: Write the failing test** (`tests/unit/test_mock_persona_descriptor.cpp`, tag `[mock][persona][descriptor]`). For **every row of `PERSONAS`** (`DYNAMIC_SECTION(p.id)`), with all six env vars pinned and `HELIX_MOCK_PRINTER=p.id`:

```cpp
const auto d = helix::mock::descriptor(p.type);
MoonrakerClientMock mock(p.type);

json info;
mock.send_jsonrpc("printer.info", json(), [&](const json& r) { info = r; });
CHECK(info["result"]["hostname"] == std::string(d.hostname));

for (const char* method : {"printer.objects.query", "printer.objects.subscribe"}) {
    json r;
    mock.send_jsonrpc(method,
        {{"objects", {{"configfile", {"config", "settings"}}, {"toolhead", {"axis_maximum"}}}}},
        [&](const json& resp) { r = resp; });
    INFO(method);
    const auto& st = r["result"]["status"];
    CHECK(st["configfile"]["settings"]["stepper_x"]["position_max"] == d.axis_max.x);
    CHECK(st["configfile"]["settings"]["stepper_y"]["position_max"] == d.axis_max.y);
    CHECK(st["configfile"]["settings"]["stepper_z"]["position_max"] == d.axis_max.z);
    CHECK(st["toolhead"]["axis_maximum"][0] == d.axis_max.x);
    CHECK(st["toolhead"]["axis_maximum"][1] == d.axis_max.y);
    CHECK(st["toolhead"]["axis_maximum"][2] == d.axis_max.z);
    CHECK(st["configfile"]["config"]["printer"]["kinematics"] == std::string(d.kinematics));
}

mock.connect("ws://mock/websocket", [] {}, [] {});
bool done = false;
mock.discover_printer([&] { done = true; });
REQUIRE(done);
const auto hw = mock.hardware();
CHECK(hw.kinematics() == std::string(d.kinematics));       // fails today: ''
CHECK(hw.build_volume().x_max == Catch::Approx(d.axis_max.x));
CHECK(hw.hostname() == std::string(d.hostname));
const auto& objs = hw.printer_objects();
auto has = [&](const char* o) { return std::find(objs.begin(), objs.end(), o) != objs.end(); };
if (d.omit & helix::mock::default_object::HAPPY_HARE_MMU) CHECK_FALSE(has("mmu"));
if (d.omit & helix::mock::default_object::BME280_CHAMBER) CHECK_FALSE(has("bme280 chamber"));
if (d.omit & helix::mock::default_object::HTU21D_DRYER) CHECK_FALSE(has("htu21d dryer"));
if (d.omit & helix::mock::default_object::EBB_CAN_MCU) CHECK_FALSE(has("mcu EBBCan"));
if (d.omit & helix::mock::default_object::WIDTH_SENSOR) CHECK_FALSE(has("hall_filament_width_sensor"));
if (d.omit & helix::mock::default_object::RUNOUT_SENSOR) CHECK_FALSE(has("filament_switch_sensor runout_sensor"));
if (d.omit & helix::mock::default_object::LED_EFFECTS) CHECK_FALSE(has("led_effect rainbow"));
```

If the subscribe handler's argument shape differs from the query's, mirror how `test_mock_probe_discovery.cpp` and the subscribe callers in `src/api/moonraker_discovery_sequence.cpp` pass it. The two methods must be checked separately because they are separate code paths.

Add one case that pins the omit semantics before any persona uses them. With `HELIX_MOCK_PRINTER=creator5_zmod`, `mmu` is absent. With `voron_24`, `mmu`, `bme280 chamber`, `htu21d dryer`, `mcu EBBCan` and `filament_switch_sensor runout_sensor` are present. Today's default set must survive the refactor.

- [ ] **Step 2: Run to verify it fails.** Run `make t F='[mock][persona][descriptor]'`. Expected failures: kinematics `''` after discovery, `toolhead.axis_maximum` 235 versus 250, and K1 `stepper_x.position_max` 250 versus 229.

- [ ] **Step 3: Implement** per the Files list.

- [ ] **Step 4: Verify.** Run `make t F='[mock][persona]'`, then `make t F='[mock]'`, `make t F='[bed_mesh]'`, `make t F='[exclude_object]'`, `make t F='[printer]'` and `make t F='[all_printers]'`. Then re-run the Verified-facts smoke for all 12 personas (loop from this plan's header: an empty config dir each, `timeout 15`, grep `Detection complete|Auto-detected printer`). Detection now sees real kinematics. **Expect some results to move**, and record each change in the commit body. Every non-generic persona must still name the same printer. A generic persona (`generic_*`, `multi_extruder`, `delta`) must still **not** autosave. If one now autosaves, the kinematics bonus pushed a coincidental match over 85: rename the generic persona's coincidental object (for example `neopixel chamber_led` on GENERIC_COREXY, which reads as an AD5M Pro chamber light) instead of touching the database. Task 3's table locks this in.

- [ ] **Step 5: Commit.** Use `git add` on the files above, then `git commit -m "fix(mock): hostname, build volume and kinematics come from the persona descriptor everywhere" -m "configfile settings, toolhead axis_maximum (query and subscribe), bed mesh bounds and the exclude-object grid now agree, and configfile.config carries [printer] kinematics, so --test detection sees kinematics at all. Inherited default objects are opt-out per persona. <detection changes observed>. Mutation: <…>."`

---

### Task 3: Detection table test; AD5M and K1 fixes; shared capture loader; stale lists (effort: M)

**Files:**
- Create: `tests/test_helpers/printer_capture.h`. Move `printers_fixture_path`, `load_printer_capture`, `hardware_from_json` and `printer_capture` out of `tests/unit/test_printer_detector.cpp`'s anonymous namespace into `namespace helix::test`, unchanged. `test_printer_detector.cpp` includes it and adds `using namespace helix::test;`. This is a pure move: `make t F='[printer_detector]'` must stay green with no test edits beyond the include.
- Create: `tests/unit/test_mock_persona_detection.cpp`
- Modify: `include/mock_persona.h` rows. FLASHFORGE_AD5M: hostname `ad5m-mock`, axis 220/220/220, **corexy**. CREALITY_K1: hostname `k1c-mock`. CREALITY_K1_MAX: hostname `k1max-mock`.
- Modify: `src/api/moonraker_client_mock.cpp#MoonrakerClientMock::populate_hardware`: in the AD5M case, drop `led chamber_led` and replace the fans with the `assets/config/presets/ad5m.json` `hardware/expected` set (`fan`, `heater_fan hotend_fan`, `controller_fan stepper_driver_fan`).
- Modify: `tests/unit/test_moonraker_api_domain.cpp` ("…work for all printer types") and `tests/unit/test_moonraker_full_stack.cpp` ("Full stack: All printer types work correctly"). Replace the hand-written 7-type vectors with every distinct `p.type` in `helix::mock::PERSONAS`, so a new persona is covered automatically.
- Modify: `tests/unit/test_preprint_adaptive.cpp` "AD5M mock identity…". It keeps its literal hostname by design and must stay green.

**Interfaces:**
- Consumes: Task 1's table and Task 2's descriptor plumbing.
- Produces: `helix::test::printer_capture(const std::string& slug) -> PrinterHardwareData` and `helix::test::load_printer_capture(const std::string& slug) -> nlohmann::json`. It also produces the `EXPECTED` table, to which every later persona task appends one row.

- [ ] **Step 1: Write the failing test** `tests/unit/test_mock_persona_detection.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mock_persona.h"
#include "moonraker_client_mock.h"
#include "printer_detector.h"
#include "printer_discovery.h"
#include "test_helpers/scoped_env.h"

#include <string>
#include <string_view>

#include "../catch_amalgamated.hpp"
#include "../helix_test_fixture.h"

namespace {

/// The persona's own objects, not what another test in this shard left in the
/// mock-topology env vars.
struct PersonaEnv {
    helix::ScopedEnv ams{"HELIX_MOCK_AMS", nullptr};
    helix::ScopedEnv objects{"HELIX_MOCK_OBJECTS", nullptr};
    helix::ScopedEnv probe{"HELIX_MOCK_PROBE_TYPE", nullptr};
    helix::ScopedEnv sensors{"HELIX_MOCK_FILAMENT_SENSORS", nullptr};
    helix::ScopedEnv kinematics{"HELIX_MOCK_KINEMATICS", nullptr};
    helix::ScopedEnv printer;
    explicit PersonaEnv(std::string_view id)
        : printer("HELIX_MOCK_PRINTER", std::string(id).c_str()) {}
};

/// What the app does under --test: the real discovery sequence over the mock.
helix::PrinterDiscovery discover(const helix::mock::PersonaEntry& p) {
    MoonrakerClientMock mock(p.type);
    mock.connect("ws://mock/websocket", [] {}, [] {});
    bool done = false;
    mock.discover_printer([&done] { done = true; });
    REQUIRE(done);
    return mock.hardware();
}

struct Expectation {
    std::string_view id;
    std::string_view type_name; ///< "" = no printer asserted (generic personas)
    std::string_view preset;
    bool autosaves;
};

// clang-format off
constexpr Expectation EXPECTED[] = {
    {"voron_24",           "Voron 2.4",                 "",             true},
    {"voron_trident",      "Voron Trident",             "",             false},
    {"k1",                 "Creality K1C",              "k1c",          true},
    {"k1max",              "Creality K1 Max",           "k1",           true},
    {"ad5m",               "FlashForge Adventurer 5M",  "ad5m",         true},
    {"creator5",           "FlashForge Creator 5 Pro",  "creator5_pro", true},
    {"creator5_zmod",      "FlashForge Creator 5 Pro",  "creator5_pro", true},
    {"generic_corexy",     "",                          "",             false},
    {"generic_bedslinger", "",                          "",             false},
    {"multi_extruder",     "",                          "",             false},
    {"delta",              "",                          "",             false},
    {"snapmaker_u1",       "",                          "",             false},
};
// clang-format on

} // namespace

TEST_CASE("Every persona has a detection expectation", "[mock][persona][detect]") {
    for (const auto& p : helix::mock::PERSONAS) {
        bool listed = false;
        for (const auto& e : EXPECTED) listed = listed || e.id == p.id;
        INFO(p.id);
        CHECK(listed);
    }
    for (const auto& e : EXPECTED) {
        INFO(e.id);
        CHECK(helix::mock::find_persona(e.id) != nullptr);
    }
}

TEST_CASE_METHOD(HelixTestFixture, "Each mock persona auto-detects as the printer it impersonates",
                 "[mock][persona][detect]") {
    for (const auto& e : EXPECTED) {
        DYNAMIC_SECTION(e.id) {
            const auto* p = helix::mock::find_persona(e.id);
            REQUIRE(p != nullptr);
            PersonaEnv env(e.id);
            const auto r = PrinterDetector::auto_detect(discover(*p));
            INFO(r.type_name << " " << r.confidence << "% margin " << r.margin() << ": " << r.reason);
            CHECK(PrinterDetector::meets_autosave_threshold(r) == e.autosaves);
            if (!e.type_name.empty()) {
                CHECK(r.type_name == e.type_name);
                CHECK(r.preset == e.preset);
            }
            // A persona that declares its type declares the one detection picks.
            if (!p->saved_type.empty() && e.autosaves) {
                CHECK(p->saved_type == r.type_name);
            }
        }
    }
}
```

- [ ] **Step 2: Run to verify it fails.** Run `make t F='[mock][persona][detect]'`. Expected failures: `ad5m` (AD5M Pro / `ad5m_pro`), and `k1`/`k1max` (`mock-printer` hostname, not autosaved). Every other row must pass at this step. If one does not, Task 2 changed it: stop and resolve that in Task 2's terms before continuing.

- [ ] **Step 3: Implement** the AD5M and K1 rows and the AD5M hardware per Files. If `k1` still does not autosave with `k1c-mock` plus its real volume, set that row to `autosaves = false` and keep `saved_type` (the declared type is what the CFS dialect needs anyway, per Verified facts). Record the detection result in the commit body. Do the same for `k1max`.

- [ ] **Step 4: Verify.** Run `make t F='[mock][persona]'`, `make t F='[printer_detector]'`, `make t F='[all_printers]'`, `make t F='[adaptive]'` and `make t F='[mock]'`. Then run the 12-persona smoke loop. `ad5m` logs `Auto-detected printer: 'FlashForge Adventurer 5M'` and `Applied preset 'ad5m'`.

- [ ] **Step 5: Commit**: `git commit -m "test(mock): each persona must auto-detect as its printer; ad5m detects as the plain 5M" -m "<detail; the K1 outcome; mutation line>"`.

---

### Task 4: CC1 persona (effort: S)

**Files:** `include/mock_persona.h` (enum `ELEGOO_CC1`, descriptor row, `PERSONAS` row `{"cc1", PrinterType::ELEGOO_CC1, "Elegoo Centauri Carbon", ""}`), `src/api/moonraker_client_mock.cpp` (`populate_hardware` and `populate_capabilities` cases), `tests/unit/test_mock_persona.cpp` (`ALL_TYPES`), `tests/unit/test_mock_persona_detection.cpp`, `docs/devel/MOCK_ENVIRONMENT_VARIABLES.md`.

**Interfaces:** Consumes Task 3's `EXPECTED` and `helix::test::load_printer_capture`. Produces `helix::mock::PrinterType::ELEGOO_CC1` and the persona id `cc1`.

Descriptor: hostname `cosmos` (the capture's own); axis 256/265/258 (capture `build_volume`); `corexy`; omit `HAPPY_HARE_MMU | CARTOGRAPHER | BME280_CHAMBER | HTU21D_DRYER | EBB_CAN_MCU | WIDTH_SENSOR | RUNOUT_SENSOR`; default mock AMS `""`; not hardware. The hardware and objects mirror `tests/fixtures/printers/elegoo_centauri_carbon.json` and `assets/config/presets/cc1.json` `hardware/expected`. That covers `load_cell_probe`, `probe`, `filament_switch_sensor filament_sensor`, `led case` and `led hotend`, `fan_generic aux_fan`/`case_fan`, `heater_fan extruder` and `temperature_sensor chamber`.

- [ ] **Step 1: Failing tests.** Append `{"cc1", "Elegoo Centauri Carbon", "cc1", true}` to `EXPECTED`, and add `PrinterType::ELEGOO_CC1` to `ALL_TYPES`. Add `#include "test_helpers/printer_capture.h"` and `<algorithm>` to `test_mock_persona_detection.cpp`. Then add the fidelity helper `check_mirrors_capture` to its anonymous namespace (every later persona task reuses it) and these cases at file scope:

```cpp
/// Every object, heater and fan the real machine reported is one the persona reports.
void check_mirrors_capture(const helix::PrinterDiscovery& hw, const std::string& slug) {
    const auto cap = helix::test::load_printer_capture(slug);
    auto contains = [](const auto& list, const std::string& v) {
        return std::find(list.begin(), list.end(), v) != list.end();
    };
    for (const auto& o : cap.value("printer_objects", nlohmann::json::array())) {
        INFO(slug << " object " << o);
        CHECK(contains(hw.printer_objects(), o.get<std::string>()));
    }
    for (const auto& h : cap.value("heaters", nlohmann::json::array())) {
        INFO(slug << " heater " << h);
        CHECK(contains(hw.heaters(), h.get<std::string>()));
    }
    for (const auto& f : cap.value("fans", nlohmann::json::array())) {
        INFO(slug << " fan " << f);
        CHECK(contains(hw.fans(), f.get<std::string>()));
    }
    CHECK(hw.hostname() == cap.value("hostname", std::string{}));
}

TEST_CASE_METHOD(HelixTestFixture, "The cc1 persona mirrors the real CC1 capture",
                 "[mock][persona][cc1]") {
    PersonaEnv env("cc1");
    check_mirrors_capture(discover(*helix::mock::find_persona("cc1")),
                          "elegoo_centauri_carbon");
}

TEST_CASE_METHOD(HelixTestFixture, "An env override restores an omitted default",
                 "[mock][persona][cc1]") {
    PersonaEnv env("cc1");
    const auto* p = helix::mock::find_persona("cc1");
    {
        const auto objs = discover(*p).printer_objects();
        CHECK(std::find(objs.begin(), objs.end(), "cartographer") == objs.end());
    }
    helix::ScopedEnv probe("HELIX_MOCK_PROBE_TYPE", "cartographer");
    helix::ScopedEnv sensors("HELIX_MOCK_FILAMENT_SENSORS", "switch:extra_runout");
    const auto objs = discover(*p).printer_objects();
    CHECK(std::find(objs.begin(), objs.end(), "filament_switch_sensor extra_runout") != objs.end());
    // the cartographer probe objects are helix::sim::mock_probe_status()'s keys
}
```

To finish the second test, assert that every key of `helix::sim::mock_probe_status()` under `cartographer` is present. Read how `populate_capabilities` pushes them, and use the same call.

- [ ] **Step 2: Run to verify it fails.** Run `make t F='[mock][persona]'`. It fails to compile (`ELEGOO_CC1`), and once it compiles, `cc1` is not found.
- [ ] **Step 3: Implement** the descriptor, rows and switch cases.
- [ ] **Step 4: Verify.** Run `make t F='[mock][persona]'` and `make t F='[mock]'`. Smoke-test with `HELIX_MOCK_PRINTER=cc1 … --test -s micro` (480x272, the CC1 screen). The log shows `Auto-detected printer: 'Elegoo Centauri Carbon'` and `Applied preset 'cc1'`. Run `helix-screen ctl -s "$HELIX_SOCK" screenshot` on home and on the bed-mesh panel, and check there are no `[error]` lines in the log. Ask Preston only whether the pixels look right.
- [ ] **Step 5: Commit**: `feat(mock): cc1 persona mirrors the real Centauri Carbon capture`. Include the docs row (value list + a "`cc1`: run with `-s micro`" note).

---

### Task 5: AD5X persona with the mock IFS default (effort: S)

**Files:** as Task 4, with enum `FLASHFORGE_AD5X`, persona `{"ad5x", PrinterType::FLASHFORGE_AD5X, "Flashforge AD5X (mock IFS)", ""}`.

Descriptor: hostname `ad5x-mock` (DB `hostname_match ad5x` 96, and it is excluded by every AD5M entry); axis 220/220/220; `corexy`; omit `HAPPY_HARE_MMU | CARTOGRAPHER | BME280_CHAMBER | HTU21D_DRYER | EBB_CAN_MCU | WIDTH_SENSOR | RUNOUT_SENSOR`; **default mock AMS `ifs`** (the `AmsBackendMock::set_ifs_mode` path); not hardware. Hardware mirrors `assets/config/presets/ad5x.json` `hardware/expected`: `fan_generic fanM106`, `heater_fan heat_fan`, `fan_generic chamber_fan`, `fan_generic pcb_fan`, `filament_switch_sensor head_switch_sensor`. Objects add `gcode_macro SET_EXTRUDER_SLOT` (DB 95, excluded by every AD5M entry). **Do not** publish `zmod_ifs`, `ifs`, `ifs_materials` or `_ifs_port_sensor_*`: those make discovery choose the production `AD5X_IFS` backend (`src/printer/printer_discovery_parse.cpp`, the IFS branches), which is Task 8's hardware persona. There is no capture fixture for the AD5X.

- [ ] **Step 1: Failing tests.** Append `{"ad5x", "FlashForge Adventurer 5X", "ad5x", true}` to `EXPECTED`, and add `FLASHFORGE_AD5X` to `ALL_TYPES`. In `test_mock_persona.cpp`, add `CHECK(effective_mock_ams(nullptr, "ad5x") == "ifs"); CHECK(effective_mock_ams("afc", "ad5x") == "afc");` (Review Focus 1). In `test_mock_persona_detection.cpp`, add a case: with `PersonaEnv env("ad5x")`, the discovered `hw.mmu_type()` is **not** `AmsType::AD5X_IFS`, and no object starting `ifs`/`zmod_ifs` is present. That proves the mock-IFS persona does not stand up the production backend. Use whatever accessor `PrinterDiscovery` exposes for the detected AMS type, found with `grep -n "mmu_type" include/printer_discovery.h`.
- [ ] **Step 2: Run to verify it fails.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Verify.** Run `make t F='[mock][persona]'` and `make t F='[ams]'`. Smoke-test with `HELIX_MOCK_PRINTER=ad5x … --test`. The log shows `Mock AD5X IFS mode enabled` and the AD5X auto-detected, and `ctl navigate` to the AMS panel shows 4 IFS slots. Then smoke-test with `HELIX_MOCK_AMS=none`, which shows no AMS panel entry.
- [ ] **Step 5: Commit**: `feat(mock): ad5x persona with the mock IFS by default`.

---

### Task 6: K2 Plus persona with CFS (effort: M)

**Files:** as Task 4, with enum `CREALITY_K2_PLUS`, persona `{"k2", PrinterType::CREALITY_K2_PLUS, "Creality K2 Plus", ""}`, plus `src/api/moonraker_client_mock_gcode.cpp` and `src/api/moonraker_client_mock.cpp` (CFS state).

Descriptor: hostname `K2Plus-50C1` (the capture's); axis = the capture's `configfile_settings` stepper travel (read it from `tests/fixtures/printers/creality_k2_plus.json`); `corexy`; omit everything Task 4 omits plus `LED_EFFECTS`; **default mock AMS `cfs`**; not a hardware persona. That flag means "implies `--real-ams`"; the CFS path already gets that through `cli_args`' `cfs` branch via `effective_mock_ams`. Objects and hardware mirror the capture (`box`, `motor_control`, `fan_feedback`, `load_ai`, `filament_rack`, `temperature_sensor chamber_temp`, `heater_generic chamber_heater`, fans `fan`, `heater_fan chamber_fan`) and `assets/config/presets/k2.json` `hardware/expected`. The persona reports the declared bed through `set_config_settings_section("gcode_macro product_param", {{"variable_bed_size_x","350"},{"variable_bed_size_y","350"}})`, called from the constructor for this persona. The K2 Plus entry's `build_volume_range` uses `measure: declared_bed`. Answer the `motor_control` and `fan_feedback` subscriptions (`src/api/moonraker_discovery_sequence.cpp` subscribes both when present) with minimal status that `src/printer/printer_fan_state.cpp` and `src/printer/cfs_status_parse.cpp` accept. Read those parsers for the field names.

Today `box` is pushed only when `is_mock_cfs()`. That now becomes true by default for this persona, through `effective_mock_ams`, so it needs no persona special case. The backend latches the **K2** `CR_BOX_*` dialect because no saved K1 type exists. `AmsBackendCfs` emits `CR_BOX_PRE_OPT`, `CR_BOX_EXTRUDE`, `CR_BOX_CUT`, `CR_BOX_RETRUDE`, `BOX_MODIFY_TN` and others (`grep -o '"CR_BOX_[A-Z_]*' src/printer/ams_backend_cfs.cpp`). The mock answers only `BOX_FIND_CUT_POS` and `BOX_CUSTOM_COMMAND` today (`moonraker_client_mock_gcode.cpp`, the CFS block).

- [ ] **Step 1: Failing tests.** Append `{"k2", "Creality K2 Plus", "k2", true}` to `EXPECTED`, and add `CREALITY_K2_PLUS` to `ALL_TYPES`. Add these cases:
  - Fidelity: `check_mirrors_capture(discover(k2), "creality_k2_plus")`.
  - `HELIX_MOCK_AMS=none` with `k2`: there is no `box` object, and `effective_mock_ams("none","k2") == "none"` (Review Focus 1).
  - Load and unload change the box frame. Build a K2 mock and capture `notify_status_update` frames, as `test_mock_creator5_zmod.cpp`'s `ZmodFrames` does. Send the gcode script `AmsBackendCfs` builds for loading slot `T1A`: take it from the backend's own builder function, never retype it, after finding the builder with `grep -n "CR_BOX_EXTRUDE" src/printer/ams_backend_cfs.cpp`. Assert that a later `box` frame reports that slot loaded. Then do the same for the unload script, and assert it unloaded.
- [ ] **Step 2: Run to verify it fails.** Expected: `k2` unknown. Once it compiles, the load assertion times out, because no `CR_BOX_*` handler exists.
- [ ] **Step 3: Implement** the descriptor and hardware. Add `CR_BOX_*` handlers beside the existing CFS block in `moonraker_client_mock_gcode.cpp`. They update the same state `cfs_box_status_json()` renders, and the existing status push then carries it. Reuse `cfs_box_status_json()` for the frame shape. Do not fork a K2 copy unless the K2 frame differs, and if it does, say how in a comment.
- [ ] **Step 4: Verify.** Run `make t F='[mock][persona]'`, `make t F='[cfs]'` and `make t F='[ams]'`. Smoke-test with `HELIX_MOCK_PRINTER=k2 … --test` at 800x480. The log shows `macro variant: K2 (CR_BOX_*)` and `Auto-detected printer: 'Creality K2 Plus'`. On the AMS panel, `ctl` a load of slot A, then an unload, and read the slot state back with `ctl text`.
- [ ] **Step 5: Commit**: `feat(mock): k2 persona (K2 Plus) with the CFS box and CR_BOX load/unload`.

---

### Task 7: Snapmaker U1 as a real 4-extruder persona, quick path (effort: M)

**Files:** as Task 4, with enum `SNAPMAKER_U1`. The `snapmaker_u1` row moves from MULTI_EXTRUDER to `{"snapmaker_u1", PrinterType::SNAPMAKER_U1, "Snapmaker U1", ""}`, so the **declared saved type goes away**.

Descriptor: hostname `snapmaker-u1`; axis from the capture (`tests/fixtures/printers/snapmaker_u1.json` `build_volume`: 270/270/400); kinematics = the capture's value; omit `HAPPY_HARE_MMU | CARTOGRAPHER | BME280_CHAMBER | HTU21D_DRYER | EBB_CAN_MCU | WIDTH_SENSOR | RUNOUT_SENSOR`; **default mock AMS `snapmaker`** (`AmsBackendMock::set_snapmaker_mode`; it refuses when `HELIX_HAS_SNAPMAKER` is 0, which is never the case on a desktop build); not hardware. Heaters `extruder`..`extruder3` + `heater_bed`. Fans, LEDs and filament sensors come from the capture and `assets/config/presets/snapmaker_u1.json` (`filament_motion_sensor e0_filament`..`e3_filament`, `led cavity_led`, `temperature_sensor cavity`). Objects are the capture's identifying, non-backend objects: `fm175xx_reader`, `tmc2240 stepper_x`, `purifier`, `camera`, and macros `FILAMENT_DT_UPDATE`, `FILAMENT_DT_QUERY` and `EXTRUDER_OFFSET_ACTION_PROBE_CALIBRATE_ALL`. **Not** `filament_detect` (it switches discovery to the production Snapmaker backend; that is Task 9), and not `defect_detection` or `machine_state_manager`, which have production consumers. Check each object you add with `grep -rn '"<object>' src include`: an object with a production consumer belongs to Task 9.

- [ ] **Step 1: Failing tests.** Change the `snapmaker_u1` row to `{"snapmaker_u1", "Snapmaker U1", "snapmaker_u1", true}`. Add `SNAPMAKER_U1` to `ALL_TYPES`. Add `check_mirrors_capture(discover(u1), "snapmaker_u1")`. Assert that `hw.heaters()` holds exactly four `extruder*` entries, and that `"filament_detect"` is absent. In `test_moonraker_manager.cpp`, `apply_mock_printer_identity(cfg, "snapmaker_u1")` on a config saved as "Voron 2.4" now **clears** it (the declaration is gone).
- [ ] **Step 2: Run to verify it fails.**
- [ ] **Step 3: Implement.** Then grep for other `snapmaker_u1` persona users (`grep -rn '"snapmaker_u1"' src tests docs scripts | grep -v PrintStartProfile`), and update the doc's "multi-extruder mock with the U1's four pre-print options" line.
- [ ] **Step 4: Verify.** Run `make t F='[mock][persona]'`, `make t F='[snapmaker]'` and `make t F='[ams]'`. Smoke-test with `HELIX_MOCK_PRINTER=snapmaker_u1 … --test -s tiny` (480x320). The log shows `Auto-detected printer: 'Snapmaker U1'`, `Applied preset 'snapmaker_u1'` and `Mock Snapmaker U1 mode enabled`, and the home screen shows 4 nozzle temperatures (`ctl text`).
- [ ] **Step 5: Commit**: `feat(mock): snapmaker_u1 is a four-extruder persona that auto-detects`.

---

### Task 8 (OPTIONAL, separate decision): AD5X on real ZMOD IFS hardware (effort: M)

This task is not needed for screenshots. Do it only if Preston asks.

It adds a second persona, `{"ad5x_zmod", PrinterType::FLASHFORGE_AD5X_ZMOD, "Flashforge AD5X (Z-Mod IFS)", ""}`, with `hardware_persona = true`, so `--real-ams` is implied and `try_create_mock` declines. It publishes the native ZMOD IFS objects and sensors that `AmsBackendAd5xIfs` and `src/api/moonraker_discovery_sequence.cpp` read (`zmod_ifs`, `filament_motion_sensor ifs_motion_sensor` or `_ifs_port_sensor_*`, `save_variables` with `_IFS_VARS`). It also simulates the `_IFS_` / `SET_EXTRUDER_SLOT` gcode the backend sends, modelled on the existing standalone-module mode (`is_mock_ifs_module`, `ifs_module_status_json`) and the Z-Mod Creator 5 persona's `gcode_zmod`. The tests follow `test_mock_creator5_zmod.cpp`: the persona's discovery yields `AmsType::AD5X_IFS`, a slot change round-trips through frames, and an `EXPECTED` row reads `{"ad5x_zmod", "FlashForge Adventurer 5X", "ad5x", true}`. Read `docs/devel/FILAMENT_BACKEND_*` for the IFS backend (`.claude/rules/filament-backends.md` names it) before writing anything.

### Task 9 (OPTIONAL, separate decision): U1 full objects path (effort: L)

This task is not needed for screenshots. It adds `filament_detect`, `machine_state_manager`, `defect_detection`, `filament_entangle_detect`, `extruder_offset_calibration` and `homing_precise_corexy` with their status payloads, and flips the U1 descriptor to `hardware_persona = true` with default mock AMS `""`. `AmsBackendSnapmaker`, power-loss recovery (`machine_state_manager`), `DetectionManager`'s U1 source and the flow calibrator then run against the mock. Each of those subsystems gets one Catch2 case that proves its production path engages under the persona. Read `docs/devel/FILAMENT_MANAGEMENT.md` (Snapmaker section) and `src/printer/ams_backend_snapmaker.cpp` first.

### Task 10 (OPTIONAL, LAST — not a release platform): QIDI Q2 with the stock QIDI Box (effort: L)

Do this task only if Preston asks.

The persona is `{"qidi_q2", PrinterType::QIDI_Q2, "Qidi Q2 (QIDI Box)", ""}`. The hostname is `linaro-alip` (the real rootfs default), and the descriptor gains `std::string_view machine_name`, which the mock reports through `machine.system_info` as `QIDI@Q2`. This is the identity path real Q2s take (`src/api/moonraker_discovery_sequence.cpp#MoonrakerDiscoverySequence::publish_identity_locked`). Objects mirror `tests/fixtures/printers/qidi_q2.json`, plus the QIDI Box: `box_stepper slot1..4`, `box_extras`, `save_variables`, per-box `heater_generic`, and `FORCE_MOVE` handling. The persona omits `HAPPY_HARE_MMU` and sets `hardware_persona = true`. Its `EXPECTED` row reads `{"qidi_q2", "Qidi Q2", "qidi_q2", true}`, with `check_mirrors_capture(…, "qidi_q2")`.

**Conflict to surface before starting:** `assets/config/presets/qidi_q2.json` expects the Happy Hare shape (`mmu`, `mmu_pre_gate_*` sensors). A stock-Box persona would raise "expected hardware missing" for `mmu`. Ask Preston whether the preset or the persona is wrong before writing code.

---

### Task 11: Finish the branch on main's side (effort: S)

- [ ] Update `docs/devel/MOCK_ENVIRONMENT_VARIABLES.md` `HELIX_MOCK_PRINTER`. The values list is the final table. Add one example line per new persona with its screen size (`cc1 -s micro`, `snapmaker_u1 -s tiny`, `k2` 800x480), and a sentence saying each persona is asserted to auto-detect by `tests/unit/test_mock_persona_detection.cpp`. Replace the "(for AD5M) the `pre_print_options` set" sentence, which is now true of every named persona.
- [ ] Run the 12+N-persona smoke loop one last time, then `make full-test-run` (the completion gate: unit sweep + bats). Optionally run `scripts/zeus-run.sh sweep` after pushing the branch, but only with Preston's OK to push.
- [ ] Delete this plan: `git rm docs/devel/plans/2026-10-07-mock-personas.md`. The durable knowledge is in `include/mock_persona.h`, the tests and the mock doc. Commit `chore(plans): mock personas shipped`.
- [ ] **STOP.** Report the branch head SHA and the gate results to Preston. Landing on `main` follows `CLAUDE.md` § "Sharing This Tree" (batch landing, `take worktree:main`, push in the same claim), and happens only on his go-ahead.

---

### Task 12: Backport to `release/1.0` (effort: L; a port, not a cherry-pick)

Run this task only after Task 11 lands on `main`, or on Preston's instruction against the branch.

- [ ] **Step 1: Worktree.** Run `scripts/helix-claim check worktree:mock-personas-1.0`, then `scripts/setup-worktree.sh --base release/1.0 backport/mock-personas-1.0` (this creates `.worktrees/mock-personas-1.0`), then `scripts/helix-claim take worktree:mock-personas-1.0 --pid $$`. Do not use or modify `.worktrees/1.0`, which belongs to another session.
- [ ] **Step 2: Cherry-pick in order** with `git cherry-pick -x <sha>` for each Task 1–7 commit (and any optional task that shipped). Skip the plan add/delete commits.
- [ ] **Step 3: Resolve conflicts as a port.** 1.0 has 7 `PrinterType`s, no `mock_persona.h`, no `k1max`/`creator5*`/`delta`/`snapmaker_u1` personas, no CFS mock mode, and an older detector and database (see Verified facts). These rules apply:
  - The persona table on 1.0 lists **only personas whose types exist on 1.0** plus the new ones. Do not backport `k1max`/`creator5*`/`delta` personas just to fill the table.
  - Task 6's K2 persona needs the CFS mock mode (`is_mock_cfs`, `cfs_box_status_json`, the `box` push), which 1.0 lacks. Port that mock-only code with it. It is under `HELIX_ENABLE_MOCKS`. If it pulls in production `AmsBackendCfs` changes, **stop and ask**.
  - **Never** change 1.0's `printer_database.json`, `printer_detector.cpp` or any production backend to make a persona detect. If 1.0's detector lands a persona elsewhere, adjust the persona (hostname, objects) within the rules above, or record the 1.0 result in 1.0's `EXPECTED` row and say so in the report.
  - Fixtures missing on 1.0 (`tests/fixtures/printers/*.json`) come over with the test that reads them, and only those.
- [ ] **Step 4: Prove production is untouched.** Run `git diff release/1.0...HEAD --stat` and confirm that every changed file is a mock file, a test, `scripts/screenshot.sh`, docs, or one of `moonraker_manager.cpp`, `cli_args.cpp` or `ams_backend.cpp` (env routing only). Read every hunk outside `#ifdef HELIX_ENABLE_MOCKS` in those three. Then do a packaging build-flag dry run: `make -n HELIX_PACKAGING=1 2>/dev/null | grep -c moonraker_client_mock` must print `0`.
- [ ] **Step 5: Gates on 1.0.** Run `make t F='[mock][persona]'`, `make t F='[all_printers]'`, `make t F='[mock_printer]'`, `bats tests/shell/test_screenshot_printer_ids.bats`, then `make full-test-run`. Smoke-loop every persona on the 1.0 build.
- [ ] **Step 6: STOP before any push or merge.** Report the branch, its head SHA, the per-commit conflict notes, each persona's detection result on 1.0, and the gate output. Preston decides whether and how it reaches `release/1.0`.

---

## Open questions for Preston

1. **The K2 model.** This plan's `k2` persona is a **K2 Plus**: it has the capture fixture, the chamber heater and the CFS. Is that the screen HelixBot should show K2 users, or does it need a plain K2 or a K2 Pro as well?
2. **Backport scope.** The K2 persona on 1.0 needs the CFS mock mode ported along with it (mock-only code). Is that acceptable, or should 1.0 get a K2 without CFS?
3. **QIDI preset conflict (Task 10, only if done).** `qidi_q2.json` expects Happy Hare `mmu`, but a stock-Box persona has none. Which one is right?
4. **Out of scope, noticed.** The stock-firmware `creator5` persona trips the `creator5_pro_zmod` preset variant because the Reforge/stock Creator 5 also names its part fan `fan_generic fanM106` (`src/printer/printer_detector.cpp`, the ZMOD-variant block). If real stock Creator 5 Pros report that fan, production picks the wrong variant. Should it get an issue?
