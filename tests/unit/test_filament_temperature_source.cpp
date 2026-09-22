// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_filament_temperature_source.cpp
 * @brief The firmware-published per-filament operation temperatures.
 *
 * Run with: ./build/bin/helix-tests "[snapmaker][filament-temps]"
 *
 * The fixture below is the U1's FILAMENT_PARA_GET_ALL_INFO response, captured
 * verbatim from the machine (firmware table version 0.0.10): one console line
 * on the response channel, a Python dict literal, temperatures on the leaf of
 * TYPE -> vendor_X -> sub_Y. The capture also drives the fallback-chain cases:
 * the machine's own live slot 3 reports vendor "Generic", type PETG, sub-type
 * "SnapSpeed", and the table has neither vendor "Generic" nor sub "SnapSpeed"
 * spelled that way — an exact-only lookup answers nothing for a spool that is
 * physically loaded in the machine.
 */
#include "active_material_provider.h"
#include "ams_types.h"
#include "filament_database.h"
#include "filament_op_slot_resolver.h"
#include "filament_temperature_source.h"
#include "material_settings_manager.h"
#include "printer_discovery.h"

#include <map>

#include "../catch_amalgamated.hpp"

using namespace helix;
using filament_temps::FilamentKey;
using filament_temps::FilamentTemperatures;

namespace {

/// One full leaf, exactly as the firmware prints it, so the parser has to walk
/// past the per-nozzle-diameter flow maps to reach the temperatures.
const char* FULL_LEAF_PLA_GENERIC =
    "{'load_temp': 250, 'unload_temp': 250, 'clean_nozzle_temp': 170, "
    "'is_soft': False, 'flow_temp': 220, "
    "'flow_k': {'02': 0.2, '04': 0.02, '06': 0.012, '08': 0.008}, "
    "'flow_slow_v': {'02': 0.17, '04': 0.63, '06': 1.386, '08': 2.44}, "
    "'flow_fast_v': {'02': 0.83, '04': 4.99, '06': 4.99, '08': 4.99}, "
    "'flow_accel': {'02': 40.5, '04': 153.6, '06': 339.6, '08': 598.3}, "
    "'flow_k_min': {'02': 0.03, '04': 0.005, '06': 0, '08': 0}, "
    "'flow_k_max': {'02': 0.3, '04': 0.04, '06': 0.03, '08': 0.02}}";

/// Remaining leaves keep the field layout but drop the flow maps the parser
/// must ignore anyway; every temperature below is the capture's own value.
std::string fixture_response() {
    auto leaf = [](int load, int unload, int clean, bool soft, int flow) {
        std::string s = "{'load_temp': " + std::to_string(load) +
                        ", 'unload_temp': " + std::to_string(unload) +
                        ", 'clean_nozzle_temp': " + std::to_string(clean) + ", 'is_soft': ";
        s += soft ? "True" : "False";
        s += ", 'flow_temp': " + std::to_string(flow) + "}";
        return s;
    };
    std::string t = "// {'version': '0.0.10', 'hard_filaments_max_flow_k': 0.4, "
                    "'soft_filaments_max_flow_k': 0.5, ";
    t += "'PLA': {'vendor_generic': {'sub_generic': " + std::string(FULL_LEAF_PLA_GENERIC);
    t += ", 'sub_SnapSpeed': " + leaf(250, 250, 170, false, 220) + "}, ";
    t += "'vendor_Snapmaker': {'sub_generic': " + leaf(250, 250, 170, false, 220);
    t += ", 'sub_Silk': " + leaf(250, 250, 180, false, 230);
    t += ", 'sub_SnapSpeed': " + leaf(250, 250, 170, false, 220) + "}, ";
    t += "'vendor_Polymaker': {'sub_generic': " + leaf(250, 250, 170, false, 220);
    t += ", 'sub_Silk': " + leaf(250, 250, 180, false, 230) + "}}, ";
    t += "'PETG': {'vendor_generic': {'sub_generic': " + leaf(270, 270, 205, false, 255);
    t += ", 'sub_HF': " + leaf(270, 270, 170, false, 220) + "}, ";
    t += "'vendor_Snapmaker': {'sub_generic': " + leaf(270, 270, 205, false, 255) + "}}, ";
    t +=
        "'PETG-HF': {'vendor_generic': {'sub_generic': " + leaf(270, 270, 170, false, 220) + "}}, ";
    t += "'TPU': {'vendor_generic': {'sub_generic': " + leaf(250, 250, 190, true, 240) + "}}}";
    return t;
}

/// Wipes both the published firmware table and any staged user override, so a
/// case never reads what a previous case stored. Same pattern as
/// test_active_material_provider.cpp's OverrideFixture.
struct FilamentTempsFixture {
    FilamentTempsFixture() {
        filament_temps::clear_filament_temperatures();
    }
    ~FilamentTempsFixture() {
        filament_temps::clear_filament_temperatures();
        auto& mgr = MaterialSettingsManager::instance();
        for (const char* name : {"PETG", "PLA"}) {
            if (mgr.has_override(name)) {
                mgr.clear_override(name);
            }
        }
    }

    void store_captured_table() const {
        auto table = filament_temps::parse_filament_temperatures(fixture_response());
        REQUIRE(!table.empty());
        filament_temps::store_filament_temperatures(table);
    }
};

SlotInfo slot(const char* brand, const char* material, const char* sub_type) {
    SlotInfo s;
    s.brand = brand;
    s.material = material;
    s.spool_name = sub_type; // carries SUB_TYPE on the firmwares in the table
    return s;
}

} // namespace

// ============================================================================
// parse_filament_temperatures()
// ============================================================================

TEST_CASE("filament-temps parse: the captured response yields the table",
          "[snapmaker][filament-temps]") {
    auto table = filament_temps::parse_filament_temperatures(fixture_response());

    // 12 leaves across 4 types; version and the two flow ceilings are not types.
    REQUIRE(table.size() == 12);
    REQUIRE(table.find(FilamentKey{"snapmaker", "pla", "snapspeed"}) != table.end());
    REQUIRE(table.find(FilamentKey{"generic", "petg", "generic"}) != table.end());
    REQUIRE(table.find(FilamentKey{"generic", "version", "generic"}) == table.end());
}

TEST_CASE("filament-temps parse: an exact leaf reads its three temperatures",
          "[snapmaker][filament-temps]") {
    auto table = filament_temps::parse_filament_temperatures(fixture_response());
    auto it = table.find(FilamentKey{"snapmaker", "pla", "snapspeed"});
    REQUIRE(it != table.end());
    CHECK(it->second.load_c.value() == 250);
    CHECK(it->second.unload_c.value() == 250);
    CHECK(it->second.clean_nozzle_c.value() == 170);
}

TEST_CASE("filament-temps parse: malformed input yields an empty map, never a throw",
          "[snapmaker][filament-temps]") {
    // Truncated mid-leaf, as a dropped websocket frame would leave it.
    CHECK(filament_temps::parse_filament_temperatures(
              "// {'version': '0.0.10', 'PLA': {'vendor_generic': {")
              .empty());
    // A JSON body: the normalisation only accepts the firmware's own quoting,
    // and says so by refusing rather than corrupting.
    CHECK(filament_temps::parse_filament_temperatures(R"(// {"version": "0.0.10"})").empty());
    CHECK(filament_temps::parse_filament_temperatures("// ok").empty());
    CHECK(filament_temps::parse_filament_temperatures("").empty());
}

// ============================================================================
// lookup_filament_temperatures() — the fallback chain
// ============================================================================

TEST_CASE_METHOD(FilamentTempsFixture,
                 "filament-temps lookup: exact vendor/type/sub resolves directly",
                 "[snapmaker][filament-temps]") {
    store_captured_table();
    auto t = filament_temps::lookup_filament_temperatures(slot("Snapmaker", "PLA", "SnapSpeed"));
    REQUIRE(t.has_value());
    CHECK(t->load_c.value() == 250);
    CHECK(t->unload_c.value() == 250);
    CHECK(t->clean_nozzle_c.value() == 170);
}

TEST_CASE_METHOD(FilamentTempsFixture,
                 "filament-temps lookup: the live Generic/PETG/SnapSpeed spool resolves",
                 "[snapmaker][filament-temps]") {
    // Slot 3 as the machine itself reports it: the table's PETG row has no
    // sub_SnapSpeed, so this resolves through sub_generic — exact-only would
    // answer nullopt for a spool loaded in the machine right now.
    store_captured_table();
    auto t = filament_temps::lookup_filament_temperatures(slot("Generic", "PETG", "SnapSpeed"));
    REQUIRE(t.has_value());
    CHECK(t->load_c.value() == 270);
    CHECK(t->unload_c.value() == 270);
    CHECK(t->clean_nozzle_c.value() == 205);
}

TEST_CASE_METHOD(FilamentTempsFixture,
                 "filament-temps lookup: an unknown vendor falls back to vendor_generic",
                 "[snapmaker][filament-temps]") {
    store_captured_table();
    auto t = filament_temps::lookup_filament_temperatures(slot("Polymaker", "PETG-HF", "Silk"));
    REQUIRE(t.has_value());
    CHECK(t->load_c.value() == 270);
    CHECK(t->clean_nozzle_c.value() == 170);
}

TEST_CASE_METHOD(FilamentTempsFixture,
                 "filament-temps lookup: a type the table does not carry is nullopt",
                 "[snapmaker][filament-temps]") {
    store_captured_table();
    CHECK_FALSE(
        filament_temps::lookup_filament_temperatures(slot("Generic", "NITINOL", "")).has_value());
}

TEST_CASE_METHOD(FilamentTempsFixture, "filament-temps lookup: no published table answers nullopt",
                 "[snapmaker][filament-temps]") {
    CHECK_FALSE(
        filament_temps::lookup_filament_temperatures(slot("Generic", "PETG", "")).has_value());
}

// ============================================================================
// Capability questions over PrinterDiscovery
// ============================================================================

TEST_CASE("filament-temps capability: keyed on the filament_parameters object",
          "[snapmaker][filament-temps]") {
    PrinterDiscovery hw;
    CHECK_FALSE(filament_temps::firmware_publishes_filament_temperatures(hw));
    CHECK(filament_temps::filament_temperature_query_gcode(hw).empty());

    hw.parse_objects(nlohmann::json::array({"extruder", "filament_parameters"}));
    CHECK(filament_temps::firmware_publishes_filament_temperatures(hw));
    CHECK(filament_temps::filament_temperature_query_gcode(hw) == "FILAMENT_PARA_GET_ALL_INFO");

    PrinterDiscovery other;
    other.parse_objects(nlohmann::json::array({"extruder", "heater_bed"}));
    CHECK_FALSE(filament_temps::firmware_publishes_filament_temperatures(other));
    CHECK(filament_temps::filament_temperature_query_gcode(other).empty());
}

// ============================================================================
// Wiring into build_active_material (three-tier precedence, #961)
// ============================================================================

TEST_CASE_METHOD(FilamentTempsFixture,
                 "active material: firmware temps ride along without touching the print range",
                 "[snapmaker][filament-temps][active_material]") {
    store_captured_table();
    auto m = build_active_material(slot("Generic", "PETG", "SnapSpeed"));

    REQUIRE(m.firmware_temps.has_value());
    CHECK(m.firmware_temps->load_c.value() == 270);
    CHECK(m.firmware_temps->unload_c.value() == 270);
    CHECK(m.firmware_temps->clean_nozzle_c.value() == 205);
    // Load/unload/clean are OPERATION temperatures; the DB's print range for
    // PETG stands untouched beside them.
    CHECK(m.material_info.nozzle_min == 230);
    CHECK(m.material_info.nozzle_max == 260);
}

TEST_CASE_METHOD(FilamentTempsFixture,
                 "active material: a user override still wins with the firmware table present",
                 "[snapmaker][filament-temps][active_material]") {
    filament::MaterialOverride ovr;
    ovr.nozzle_min = 215;
    MaterialSettingsManager::instance().set_override("PETG", ovr);

    store_captured_table();
    auto m = build_active_material(slot("Generic", "PETG", "SnapSpeed"));

    CHECK(m.material_info.nozzle_min == 215); // tier 1: the user's overlay
    CHECK(m.material_info.nozzle_max == 260); // tier 3: DB (no preset, no override)
    REQUIRE(m.firmware_temps.has_value());    // tier 2 data, its own fields
    CHECK(m.firmware_temps->load_c.value() == 270);
}

TEST_CASE_METHOD(FilamentTempsFixture,
                 "active material: no table published leaves firmware_temps unset",
                 "[snapmaker][filament-temps][active_material]") {
    auto m = build_active_material(slot("Generic", "PETG", "SnapSpeed"));
    CHECK_FALSE(m.firmware_temps.has_value());
}

// ============================================================================
// The one consumer: load preheat
// ============================================================================

TEST_CASE_METHOD(FilamentTempsFixture,
                 "load preheat: the firmware's load temp beats the DB midpoint",
                 "[snapmaker][filament-temps][filament][preheat]") {
    const auto db = filament::find_material("PLA");
    REQUIRE(db.has_value());

    // Without a published table the resolver answers the DB midpoint.
    SlotInfo pla_lane = slot("Snapmaker", "PLA", "SnapSpeed");
    auto t = helix::ui::resolve_load_preheat_material(0, &pla_lane, nullptr);
    REQUIRE(t.has_value());
    CHECK(t->temp_c == db->nozzle_recommended());

    store_captured_table();
    t = helix::ui::resolve_load_preheat_material(0, &pla_lane, nullptr);
    REQUIRE(t.has_value());
    CHECK(t->temp_c == 250); // the firmware's own load temperature for this spool
}
