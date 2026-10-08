// SPDX-License-Identifier: GPL-3.0-or-later
#include "../helix_test_fixture.h"
#include "mock_persona.h"
#include "moonraker_client_mock.h"
#include "printer_detector.h"
#include "printer_discovery.h"
#include "test_helpers/scoped_env.h"

#include <string>
#include <string_view>

#include "../catch_amalgamated.hpp"

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
        for (const auto& e : EXPECTED)
            listed = listed || e.id == p.id;
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
            INFO(r.type_name << " " << r.confidence << "% margin " << r.margin() << ": "
                             << r.reason);
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
