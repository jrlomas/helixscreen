// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_mock_persona_descriptor.cpp
 * @brief Every persona reports its descriptor's identity on every mock path
 *
 * Hostname, build volume and kinematics reach HelixScreen through printer.info,
 * configfile (query and subscribe are separate handlers), toolhead and the
 * discovery sequence. Each one must agree with helix::mock::descriptor(), or
 * --test detection scores a machine the persona does not describe.
 */

#include "../lvgl_test_fixture.h"
#include "mock_persona.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "test_helpers/mock_personas.h"
#include "test_helpers/moonraker_client_mock_test_access.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;

namespace {

using helix::test::PersonaEnv;

std::vector<std::string> discovered_objects(MoonrakerClientMock& mock) {
    mock.connect("ws://mock/websocket", [] {}, [] {});
    bool done = false;
    mock.discover_printer([&] { done = true; });
    REQUIRE(done);
    return mock.hardware().printer_objects();
}

bool lists(const std::vector<std::string>& objs, const char* name) {
    return std::find(objs.begin(), objs.end(), name) != objs.end();
}

/// The status frame the mock pushes when a client subscribes.
json initial_status(MoonrakerClientMock& mock) {
    json frame;
    const auto id =
        mock.register_notify_update([&frame](const json& n) { frame = n["params"][0]; });
    helix::MoonrakerClientMockTestAccess::dispatch_initial_state(mock);
    mock.unsubscribe_notify_update(id);
    return frame;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "Every persona reports its descriptor on every mock path",
                 "[mock][persona][descriptor]") {
    for (const auto& p : helix::mock::PERSONAS) {
        DYNAMIC_SECTION(p.id) {
            const std::string id(p.id);
            PersonaEnv env(id.c_str());
            const auto d = helix::mock::descriptor(p.type);
            MoonrakerClientMock mock(p.type);

            json info;
            mock.send_jsonrpc("printer.info", json(), [&](const json& r) { info = r; });
            CHECK(info["result"]["hostname"] == std::string(d.hostname));

            for (const char* method : {"printer.objects.query", "printer.objects.subscribe"}) {
                json r;
                mock.send_jsonrpc(method,
                                  {{"objects",
                                    {{"configfile", {"config", "settings"}},
                                     {"toolhead", {"axis_maximum"}},
                                     {"bed_mesh", nullptr}}}},
                                  [&](const json& resp) { r = resp; });
                INFO(method);
                json st = r["result"]["status"]; // non-const: a missing key reads null
                CHECK(st["configfile"]["settings"]["stepper_x"]["position_max"] == d.axis_max.x);
                CHECK(st["configfile"]["settings"]["stepper_y"]["position_max"] == d.axis_max.y);
                CHECK(st["configfile"]["settings"]["stepper_z"]["position_max"] == d.axis_max.z);
                CHECK(st["toolhead"]["axis_maximum"][0] == d.axis_max.x);
                CHECK(st["toolhead"]["axis_maximum"][1] == d.axis_max.y);
                CHECK(st["toolhead"]["axis_maximum"][2] == d.axis_max.z);
                CHECK(st["configfile"]["config"]["printer"]["kinematics"] ==
                      std::string(d.kinematics));
                // A probe reaches 15mm short of each far edge of the bed. Only
                // the query handler answers bed_mesh.
                if (std::string(method) == "printer.objects.query") {
                    CHECK(st["bed_mesh"]["mesh_max"][0] == d.axis_max.x - 15.0);
                    CHECK(st["bed_mesh"]["mesh_max"][1] == d.axis_max.y - 15.0);
                }

                const json& config = st["configfile"]["config"];
                if (d.omit & helix::mock::default_object::CARTOGRAPHER)
                    CHECK_FALSE(config.contains("cartographer"));
                if (d.omit & helix::mock::default_object::LED_EFFECTS)
                    CHECK_FALSE(config.contains("led_effect rainbow"));
            }

            // Moves reach the far corner of the persona's volume and no further.
            mock.gcode_script("G28");
            const auto move_error = [&mock](const std::string& gcode) {
                return mock.gcode_script(gcode) == 0 ? std::string{} : mock.get_last_gcode_error();
            };
            char in_range[96];
            std::snprintf(in_range, sizeof in_range, "G0 X%.2f Y%.2f Z%.2f", d.axis_max.x,
                          d.axis_max.y, d.axis_max.z);
            CHECK(move_error(in_range).empty());
            for (const char* axis : {"X", "Y", "Z"}) {
                const double max = *axis == 'X'   ? d.axis_max.x
                                   : *axis == 'Y' ? d.axis_max.y
                                                  : d.axis_max.z;
                const std::string past = "G0 " + std::string(axis) + std::to_string(max + 1.0);
                INFO(past);
                CHECK(move_error(past).find("out of range") != std::string::npos);
            }

            const json frame = initial_status(mock);

            const auto objs = discovered_objects(mock);
            const auto hw = mock.hardware();
            CHECK(hw.kinematics() == std::string(d.kinematics));
            CHECK(hw.build_volume().x_max == Catch::Approx(d.axis_max.x));
            CHECK(hw.build_volume().y_max == Catch::Approx(d.axis_max.y));
            CHECK(hw.build_volume().z_max == Catch::Approx(d.axis_max.z));
            CHECK(hw.hostname() == std::string(d.hostname));

            using namespace helix::mock::default_object;
            if (d.omit & HAPPY_HARE_MMU)
                CHECK_FALSE(lists(objs, "mmu"));
            if (d.omit & BME280_CHAMBER)
                CHECK_FALSE(lists(objs, "bme280 chamber"));
            if (d.omit & HTU21D_DRYER)
                CHECK_FALSE(lists(objs, "htu21d dryer"));
            if (d.omit & EBB_CAN_MCU)
                CHECK_FALSE(lists(objs, "mcu EBBCan"));
            if (d.omit & CHAMBER_SENSOR)
                CHECK_FALSE(lists(objs, "temperature_sensor chamber"));
            if (d.omit & WIDTH_SENSOR)
                CHECK_FALSE(lists(objs, "hall_filament_width_sensor"));
            if (d.omit & RUNOUT_SENSOR)
                CHECK_FALSE(lists(objs, "filament_switch_sensor runout_sensor"));
            if (d.omit & LED_EFFECTS)
                CHECK_FALSE(lists(objs, "led_effect rainbow"));
            if (d.omit & CARTOGRAPHER) {
                CHECK_FALSE(lists(objs, "cartographer"));
                CHECK_FALSE(frame.contains("cartographer"));
            }
            if (d.omit & CHAMBER_SENSOR)
                CHECK_FALSE(frame.contains("temperature_sensor chamber"));
        }
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "Omitted defaults drop only the persona's own objects",
                 "[mock][persona][descriptor]") {
    SECTION("creator5_zmod omits the Happy Hare mmu") {
        PersonaEnv env("creator5_zmod");
        MoonrakerClientMock mock(helix::mock::PrinterType::FLASHFORGE_CREATOR5_ZMOD);
        CHECK_FALSE(lists(discovered_objects(mock), "mmu"));
    }

    SECTION("voron_24 keeps every inherited default") {
        PersonaEnv env("voron_24");
        MoonrakerClientMock mock(helix::mock::PrinterType::VORON_24);
        const auto objs = discovered_objects(mock);
        CHECK(lists(objs, "mmu"));
        CHECK(lists(objs, "bme280 chamber"));
        CHECK(lists(objs, "htu21d dryer"));
        CHECK(lists(objs, "mcu EBBCan"));
        CHECK(lists(objs, "temperature_sensor chamber"));
        CHECK(lists(objs, "hall_filament_width_sensor"));
        CHECK(lists(objs, "filament_switch_sensor runout_sensor"));
        CHECK(lists(objs, "led_effect rainbow"));
        CHECK(lists(objs, "cartographer"));
    }
}
