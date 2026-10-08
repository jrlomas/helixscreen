// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_afc_unlink_stale_echo.cpp
 * @brief A lane unlinked from HelixScreen stays unlinked while AFC keeps
 *        restating the old spool_id (remember_spool lanes) (#1717).
 */

#include "../lvgl_test_fixture.h"
#include "ams_backend_afc.h"
#include "test_helpers/afc_test_access.h"
#include "test_helpers/backend_user_edit.h"
#include "test_helpers/registered_backend.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace helix {

class AfcUnlinkHelper : public AmsBackendAfc {
  public:
    AfcUnlinkHelper() : AmsBackendAfc(nullptr, nullptr) {
        std::vector<std::string> names{"lane1", "lane2"};
        AfcTestAccess::initialize_slots(*this, names);
    }

    void feed_stepper(const nlohmann::json& data) {
        nlohmann::json params;
        params["AFC_stepper lane1"] = data;
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params, 0.0});
        handle_status_update(notification);
    }

    AmsError execute_gcode(const std::string& gcode) override {
        captured_gcodes.push_back(gcode);
        return AmsErrorHelper::success();
    }

    void restart() {
        on_started();
    }

    [[nodiscard]] int spool_id() const {
        return get_slot_info(0).spoolman_id;
    }

    void unlink() {
        SlotInfo info = get_slot_info(0);
        info.spoolman_id = 0;
        helix::test::apply_edit(*this, 0, info);
    }

    std::vector<std::string> captured_gcodes;
};

} // namespace helix

namespace {
nlohmann::json loaded(const nlohmann::json& spool_id) {
    return {{"prep", true}, {"load", true}, {"spool_id", spool_id}};
}
} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "AFC unlink: restated old spool_id stays cleared (#1717)",
                 "[1717][ams][afc]") {
    helix::test::RegisteredBackend<AfcUnlinkHelper> reg;
    AfcUnlinkHelper& afc = *reg;
    afc.feed_stepper(loaded(42));
    REQUIRE(afc.spool_id() == 42);

    afc.unlink();
    REQUIRE(afc.spool_id() == 0);

    afc.feed_stepper(loaded(42));
    afc.feed_stepper(loaded(42));
    CHECK(afc.spool_id() == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC unlink: a different id afterwards is accepted (#1717)",
                 "[1717][ams][afc]") {
    helix::test::RegisteredBackend<AfcUnlinkHelper> reg;
    AfcUnlinkHelper& afc = *reg;
    afc.feed_stepper(loaded(42));
    afc.unlink();
    afc.feed_stepper(loaded(42));
    REQUIRE(afc.spool_id() == 0);

    afc.feed_stepper(loaded(77));
    CHECK(afc.spool_id() == 77);
    // The guard ended with the new spool: the old id is a real link again.
    afc.feed_stepper(loaded(42));
    CHECK(afc.spool_id() == 42);
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC unlink: no unlink means a restated id is accepted (#1717)",
                 "[1717][ams][afc]") {
    helix::test::RegisteredBackend<AfcUnlinkHelper> reg;
    AfcUnlinkHelper& afc = *reg;
    afc.feed_stepper(loaded(42));
    afc.feed_stepper(loaded(42));
    CHECK(afc.spool_id() == 42);
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC unlink: guard ends on lane unload and on restart (#1717)",
                 "[1717][ams][afc]") {
    helix::test::RegisteredBackend<AfcUnlinkHelper> reg;
    AfcUnlinkHelper& afc = *reg;

    SECTION("lane unload") {
        afc.feed_stepper(loaded(42));
        afc.unlink();
        afc.feed_stepper({{"prep", false}, {"load", false}});
        afc.feed_stepper(loaded(42));
        CHECK(afc.spool_id() == 42);
    }

    SECTION("restart") {
        afc.feed_stepper(loaded(42));
        afc.unlink();
        afc.restart();
        afc.feed_stepper(loaded(42));
        CHECK(afc.spool_id() == 42);
    }

    SECTION("null spool_id from AFC") {
        afc.feed_stepper(loaded(42));
        afc.unlink();
        afc.feed_stepper(loaded(nullptr));
        afc.feed_stepper(loaded(42));
        CHECK(afc.spool_id() == 42);
    }
}
