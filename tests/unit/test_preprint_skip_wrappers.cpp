// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_preprint_skip_wrappers.cpp
 * @brief The helix_skips.cfg generator and the conflict check that reads it back.
 *
 * Settings are built by loading config text the way Klipper does (sections and
 * option names lowercased, continuation lines stripped, same-named sections
 * merged with the later file winning per option), so ours_intact() is checked
 * against what generate() actually produces once Klipper has parsed it.
 */

#include "preprint_skip_wrappers.h"
#include "text_io.h"

#include <sstream>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using nlohmann::json;
namespace sw = helix::skip_wrappers;
using sw::Op;

namespace {

/// Merge config text into a configfile.settings-shaped object.
void load(json& settings, const std::string& text) {
    std::istringstream in(text);
    std::string raw;
    std::string sect;
    std::string opt;
    while (std::getline(in, raw)) {
        std::string line(helix::text_io::trim(raw));
        if (line.empty() || line[0] == '#') {
            continue;
        }
        bool continuation = raw[0] == ' ' || raw[0] == '\t';
        if (continuation && !opt.empty()) {
            std::string v = settings[sect][opt].get<std::string>();
            settings[sect][opt] = v + "\n" + line;
            continue;
        }
        if (line.front() == '[') {
            sect = helix::text_io::to_lower(line.substr(1, line.size() - 2));
            if (!settings.contains(sect)) {
                settings[sect] = json::object();
            }
            opt.clear();
            continue;
        }
        auto colon = line.find(':');
        opt = helix::text_io::to_lower(std::string(helix::text_io::trim(line.substr(0, colon))));
        settings[sect][opt] = std::string(helix::text_io::trim(line.substr(colon + 1)));
    }
}

json printer(std::initializer_list<const char*> sections) {
    json s = json::object();
    for (const char* name : sections) {
        s[name] = json::object();
    }
    return s;
}

/// The Voron 2.4 shape: bed mesh and QGL, no Z-tilt, PRINT_START a user macro.
json voron() {
    json s = printer({"bed_mesh", "quad_gantry_level", "probe", "stepper_z"});
    s["gcode_macro print_start"] = {{"gcode", "\nG28\nQUAD_GANTRY_LEVEL\nBED_MESH_CALIBRATE"}};
    return s;
}

json installed(json s) {
    load(s, sw::generate(sw::wrappable(s)));
    return s;
}

const std::string KAMP_STYLE = "[gcode_macro BED_MESH_CALIBRATE]\n"
                               "rename_existing: _BED_MESH_CALIBRATE\n"
                               "gcode:\n"
                               "    _BED_MESH_CALIBRATE {rawparams}\n";

} // namespace

TEST_CASE("wrappable picks the leveling steps the printer has", "[skip_wrappers]") {
    SECTION("a Voron wraps bed mesh and QGL") {
        CHECK(sw::wrappable(voron()) == std::vector<Op>{Op::BedMesh, Op::Qgl});
    }
    SECTION("z_tilt and z_tilt_ng both wrap Z-tilt") {
        CHECK(sw::wrappable(printer({"z_tilt"})) == std::vector<Op>{Op::ZTilt});
        CHECK(sw::wrappable(printer({"z_tilt_ng"})) == std::vector<Op>{Op::ZTilt});
    }
    SECTION("no bed_mesh section, no bed mesh wrapper") {
        CHECK(sw::wrappable(printer({"quad_gantry_level"})) == std::vector<Op>{Op::Qgl});
    }
    SECTION("settings that are not an object wrap nothing") {
        CHECK(sw::wrappable(json()).empty());
        CHECK(sw::wrappable(json::array()).empty());
    }
}

TEST_CASE("wrappable leaves a user's own macro alone", "[skip_wrappers]") {
    json s = voron();
    SECTION("a user BED_MESH_CALIBRATE macro drops bed mesh, in any case") {
        load(s, "[gcode_macro Bed_Mesh_Calibrate]\ngcode:\n    G28\n");
        CHECK(sw::wrappable(s) == std::vector<Op>{Op::Qgl});
    }
    SECTION("a user BED_MESH_CLEAR macro drops bed mesh too") {
        load(s, "[gcode_macro BED_MESH_CLEAR]\nrename_existing: _BMC\ngcode:\n    _BMC\n");
        CHECK(sw::wrappable(s) == std::vector<Op>{Op::Qgl});
    }
    SECTION("our own loaded wrappers are not a conflict") {
        CHECK(sw::wrappable(installed(s)) == std::vector<Op>{Op::BedMesh, Op::Qgl});
    }
    SECTION("a user macro added after install, merged over ours, drops that step") {
        json merged = installed(s);
        load(merged, KAMP_STYLE);
        CHECK(sw::wrappable(merged) == std::vector<Op>{Op::Qgl});
    }
}

TEST_CASE("ours_intact recognises exactly what generate wrote", "[skip_wrappers]") {
    json s = installed(voron());
    REQUIRE(sw::prep_installed(s));
    CHECK(sw::ours_intact(s, Op::BedMesh));
    CHECK(sw::ours_intact(s, Op::Qgl));
    CHECK_FALSE(sw::ours_intact(s, Op::ZTilt));

    SECTION("a user macro that wins gcode and rename_existing is not ours") {
        load(s, KAMP_STYLE);
        CHECK_FALSE(sw::ours_intact(s, Op::BedMesh));
    }
    SECTION("a merge that replaces only the gcode is not ours") {
        load(s, "[gcode_macro QUAD_GANTRY_LEVEL]\ngcode:\n    G28\n");
        CHECK_FALSE(sw::ours_intact(s, Op::Qgl));
    }
    SECTION("a merge that replaces only rename_existing is not ours") {
        load(s, "[gcode_macro QUAD_GANTRY_LEVEL]\nrename_existing: _QGL\n");
        CHECK_FALSE(sw::ours_intact(s, Op::Qgl));
    }
    SECTION("a missing BED_MESH_CLEAR wrapper breaks the bed mesh op") {
        s.erase("gcode_macro bed_mesh_clear");
        CHECK_FALSE(sw::ours_intact(s, Op::BedMesh));
    }
    SECTION("a merge that only adds an option keeps it ours") {
        load(s, "[gcode_macro BED_MESH_CALIBRATE]\nvariable_note: 1\n");
        CHECK(sw::ours_intact(s, Op::BedMesh));
    }
}

TEST_CASE("generate writes one-shot wrappers Klipper can parse", "[skip_wrappers]") {
    CHECK(sw::generate({}).empty());

    const std::string text = sw::generate({Op::BedMesh, Op::Qgl});

    SECTION("each wrapped command renames its built-in and forwards the params") {
        CHECK(text.find("[gcode_macro BED_MESH_CALIBRATE]\n"
                        "rename_existing: _HELIX_BASE_BED_MESH_CALIBRATE\n") != std::string::npos);
        CHECK(text.find("_HELIX_BASE_BED_MESH_CALIBRATE {rawparams}") != std::string::npos);
        CHECK(text.find("_HELIX_BASE_QUAD_GANTRY_LEVEL {rawparams}") != std::string::npos);
        CHECK(text.find("_HELIX_BASE_BED_MESH_CLEAR {rawparams}") != std::string::npos);
        CHECK(text.find("Z_TILT_ADJUST") == std::string::npos);
    }
    SECTION("no command shares a line: ';' would comment out the rest") {
        CHECK(text.find(';') == std::string::npos);
    }
    SECTION("_HELIX_PREP holds one flag per wrapped step and resets each to 1") {
        json s = json::object();
        load(s, text);
        const json& prep = s["gcode_macro _helix_prep"];
        CHECK(prep["variable_run_bed_mesh"] == "1");
        CHECK(prep["variable_run_qgl"] == "1");
        CHECK_FALSE(prep.contains("variable_run_z_tilt"));
        const std::string reset = prep["gcode"].get<std::string>();
        CHECK(reset.find("VARIABLE=run_bed_mesh VALUE=1") != std::string::npos);
        CHECK(reset.find("VARIABLE=run_qgl VALUE=1") != std::string::npos);
    }
    SECTION("a skipped step restores its flag; BED_MESH_CLEAR only guards") {
        json s = json::object();
        load(s, text);
        const std::string cal = s["gcode_macro bed_mesh_calibrate"]["gcode"];
        const std::string clear = s["gcode_macro bed_mesh_clear"]["gcode"];
        CHECK(cal.find("run_bed_mesh|int == 0") != std::string::npos);
        CHECK(cal.find("VARIABLE=run_bed_mesh VALUE=1") != std::string::npos);
        CHECK(clear.find("run_bed_mesh|int == 1") != std::string::npos);
        CHECK(clear.find("VALUE=") == std::string::npos);
    }
}

TEST_CASE("command and flag names", "[skip_wrappers]") {
    CHECK(std::string(sw::command_for(Op::BedMesh)) == "BED_MESH_CALIBRATE");
    CHECK(std::string(sw::command_for(Op::Qgl)) == "QUAD_GANTRY_LEVEL");
    CHECK(std::string(sw::command_for(Op::ZTilt)) == "Z_TILT_ADJUST");
    CHECK(std::string(sw::flag_for(Op::BedMesh)) == "run_bed_mesh");
    CHECK(std::string(sw::flag_for(Op::Qgl)) == "run_qgl");
    CHECK(std::string(sw::flag_for(Op::ZTilt)) == "run_z_tilt");
}
