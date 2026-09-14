// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Where each bed mesh calibration tier stores a mesh the user named before
// probing, and what has to be copied once it finishes.

#include "bed_mesh_calibration_plan.h"
#include "printer_detector.h"

#include "../catch_amalgamated.hpp"

using helix::bed_mesh::gcode_param_value;
using helix::bed_mesh::is_profile_save_refusal;
using helix::bed_mesh::plan_calibration;

TEST_CASE("calibration plan: the panel's own BED_MESH_CALIBRATE takes the name",
          "[bed_mesh][calibration_plan]") {
    StandardMacroInfo slot;

    SECTION("default names no profile, so Klipper writes default itself") {
        const auto plan = plan_calibration(slot, "default", 60);
        CHECK(plan.command == "BED_MESH_CALIBRATE");
        CHECK(plan.writes_profile == "default");
        CHECK(plan.copy_to.empty());
        CHECK_FALSE(plan.self_prepares);
    }

    SECTION("any other name is passed as PROFILE") {
        const auto plan = plan_calibration(slot, "cold", 60);
        CHECK(plan.command == "BED_MESH_CALIBRATE PROFILE=cold");
        CHECK(plan.writes_profile == "cold");
        CHECK(plan.copy_to.empty());
    }

    SECTION("a name with a space reaches Klipper as one parameter") {
        const auto plan = plan_calibration(slot, "PEI Sheet", 60);
        CHECK(plan.command == "BED_MESH_CALIBRATE PROFILE=\"PEI Sheet\"");
        CHECK(plan.writes_profile == "PEI Sheet");
    }

    SECTION("the conditional HELIX fallback is never what a calibration sends") {
        slot.fallback_macro = "HELIX_BED_MESH_IF_NEEDED";
        CHECK(plan_calibration(slot, "cold", 60).command == "BED_MESH_CALIBRATE PROFILE=cold");
    }
}

TEST_CASE("calibration plan: a detected or configured BED_MESH_CALIBRATE takes the name",
          "[bed_mesh][calibration_plan]") {
    StandardMacroInfo slot;

    SECTION("detected") {
        slot.detected_macro = "BED_MESH_CALIBRATE";
        const auto plan = plan_calibration(slot, "cold", 60);
        CHECK(plan.command == "BED_MESH_CALIBRATE PROFILE=cold");
        CHECK(plan.writes_profile == "cold");
    }

    SECTION("configured with arguments of its own, in any case") {
        slot.configured_macro = "bed_mesh_calibrate ADAPTIVE=1";
        const auto plan = plan_calibration(slot, "cold", 60);
        CHECK(plan.command == "bed_mesh_calibrate ADAPTIVE=1 PROFILE=cold");
        CHECK(plan.writes_profile == "cold");
        CHECK(plan.copy_to.empty());
    }

    SECTION("configured with a PROFILE of its own: stored there, then copied") {
        slot.configured_macro = "BED_MESH_CALIBRATE PROFILE=pei";
        const auto plan = plan_calibration(slot, "cold", 60);
        CHECK(plan.command == "BED_MESH_CALIBRATE PROFILE=pei");
        CHECK(plan.writes_profile == "pei");
        CHECK(plan.copy_to == "cold");
    }
}

TEST_CASE("calibration plan: a command that cannot be named stores default, then copies",
          "[bed_mesh][calibration_plan]") {
    StandardMacroInfo slot;

    SECTION("G29") {
        slot.detected_macro = "G29";
        const auto plan = plan_calibration(slot, "cold", 60);
        CHECK(plan.command == "G29");
        CHECK(plan.writes_profile == "default");
        CHECK(plan.copy_to == "cold");
        CHECK(plan.final_profile() == "cold");
    }

    SECTION("a shipped sequence without the placeholder") {
        slot.shipped_macro = "CLEAN_NOZZLE\nBED_MESH_CALIBRATE";
        const auto plan = plan_calibration(slot, "cold", 60);
        CHECK(plan.command == "CLEAN_NOZZLE\nBED_MESH_CALIBRATE");
        CHECK(plan.self_prepares);
        CHECK(plan.writes_profile == "default");
        CHECK(plan.copy_to == "cold");
    }

    SECTION("default needs no copy") {
        slot.detected_macro = "G29";
        const auto plan = plan_calibration(slot, "default", 60);
        CHECK(plan.copy_to.empty());
        CHECK(plan.final_profile() == "default");
    }

    SECTION("a mesh stored elsewhere is never copied into default") {
        // Klipper refuses BED_MESH_PROFILE SAVE=default.
        slot.configured_macro = "BED_MESH_CALIBRATE PROFILE=pei";
        const auto plan = plan_calibration(slot, "default", 60);
        CHECK(plan.copy_to.empty());
        CHECK(plan.final_profile() == "pei");
    }
}

TEST_CASE("calibration plan: the Centauri Carbon template, default and named",
          "[bed_mesh][calibration_plan][cc1]") {
    PrinterDetector::reload();
    StandardMacroInfo slot;
    slot.shipped_macro = PrinterDetector::get_bed_mesh_calibrate_gcode("Elegoo Centauri Carbon");
    REQUIRE_FALSE(slot.shipped_macro.empty());

    SECTION("default") {
        const auto plan = plan_calibration(slot, "default", 60);
        CHECK(plan.command == "BED_MESH_CALIBRATE BED_TEMP=60");
        CHECK(plan.writes_profile == "default");
        CHECK(plan.copy_to.empty());
        CHECK(plan.self_prepares);
    }

    SECTION("named") {
        const auto plan = plan_calibration(slot, "cold", 95);
        CHECK(plan.command == "BED_MESH_CALIBRATE PROFILE=cold BED_TEMP=95");
        CHECK(plan.writes_profile == "cold");
        CHECK(plan.copy_to.empty());
    }
}

TEST_CASE("gcode_param_value quotes only what Klipper would split",
          "[bed_mesh][calibration_plan]") {
    CHECK(gcode_param_value("cold") == "cold");
    CHECK(gcode_param_value("PEI Sheet") == "\"PEI Sheet\"");
    CHECK(gcode_param_value("a#b") == "\"a#b\"");
    CHECK(gcode_param_value("say \"hi\"") == "\"say \\\"hi\\\"\"");
}

TEST_CASE("profile save refusals are recognised in the firmware's own words",
          "[bed_mesh][calibration_plan]") {
    // Klipper's bed_mesh.py, as notify_gcode_response delivers it.
    CHECK(is_profile_save_refusal(
        "// Unable to save to profile [cold], the bed has not been probed"));
    CHECK(is_profile_save_refusal(
        "// Profile 'default' is reserved, please choose another profile name."));
    CHECK_FALSE(is_profile_save_refusal("// Bed Mesh state has been saved to profile [cold]"));
    CHECK_FALSE(is_profile_save_refusal("// Mesh Bed Leveling Complete"));
}
