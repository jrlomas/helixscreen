// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every subscription backend answers "is this slot index valid" through
// AmsSubscriptionBackend::validate_slot_index (#1624). Driving each concrete
// backend through the base's name proves none of them shadows it with a copy of
// its own, and driving the edit entry points proves none of them skips it.

#include "../test_helpers/backend_user_edit.h"
#include "ams_backend_ace.h"
#include "ams_backend_ad5x_ifs.h"
#include "ams_backend_afc.h"
#include "ams_backend_cfs.h"
#include "ams_backend_happy_hare.h"
#include "ams_backend_qidi.h"
#include "ams_backend_snapmaker.h"
#include "ams_backend_toolchanger.h"
#include "ams_error.h"
#include "ams_types.h"

#include <mutex>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Reaches the base's protected validator and slot count through the concrete
/// backend's scope, so a backend-local validate_slot_index would be the one
/// called here.
template <class B> class SlotIndexProbe : public B {
  public:
    SlotIndexProbe() : B(nullptr, nullptr) {}

    AmsError check(int slot_index) const {
        return this->validate_slot_index(slot_index);
    }
    void set_total_slots(int n) {
        std::lock_guard<std::mutex> lock(this->mutex_);
        this->system_info_.total_slots = n;
    }
    void clear_units() {
        std::lock_guard<std::mutex> lock(this->mutex_);
        this->system_info_.units.clear();
        this->system_info_.total_slots = 0;
    }
};

} // namespace

TEMPLATE_TEST_CASE("Every subscription backend bounds slot indices through the base validator",
                   "[ams][slot_index]", AmsBackendAce, AmsBackendAd5xIfs, AmsBackendAfc,
                   AmsBackendHappyHare, AmsBackendQidi, AmsBackendSnapmaker, AmsBackendToolChanger,
                   printer::AmsBackendCfs) {
    SlotIndexProbe<TestType> backend;
    // 3 matches no backend's hardware constant, so a copy bounded by NUM_TOOLS
    // or NUM_PORTS answers differently from the base.
    backend.set_total_slots(3);

    CHECK(backend.check(0).success());
    CHECK(backend.check(2).success());
    CHECK(backend.check(-2).result == AmsResult::INVALID_SLOT);
    CHECK(backend.check(3).result == AmsResult::INVALID_SLOT);

    SECTION("an edit to an index outside the range is refused as a bad slot") {
        CHECK(test::apply_edit(backend, -2, SlotInfo{}).result == AmsResult::INVALID_SLOT);
        CHECK(test::apply_edit(backend, 3, SlotInfo{}).result == AmsResult::INVALID_SLOT);
        CHECK(backend.sync_external_identity(-2, SlotInfo{}).result == AmsResult::INVALID_SLOT);
        CHECK(backend.sync_external_identity(3, SlotInfo{}).result == AmsResult::INVALID_SLOT);
    }
}

TEMPLATE_TEST_CASE("A backend with no slots discovered refuses as not connected",
                   "[ams][slot_index]", AmsBackendAce, AmsBackendAd5xIfs, AmsBackendAfc,
                   AmsBackendHappyHare, AmsBackendQidi, AmsBackendSnapmaker,
                   AmsBackendToolChanger) {
    SlotIndexProbe<TestType> backend;
    backend.set_total_slots(0);

    CHECK(backend.check(0).result == AmsResult::NOT_CONNECTED);
}

TEST_CASE("CFS keeps the whole TNN range valid until the box size is known",
          "[ams][cfs][slot_index]") {
    SlotIndexProbe<printer::AmsBackendCfs> backend;
    backend.set_total_slots(0);

    CHECK(backend.check(15).success());
    CHECK(backend.check(16).result == AmsResult::INVALID_SLOT);
}

TEST_CASE("QIDI Box refuses a bad slot as a bad slot at every entry point",
          "[ams][qidi_box][slot_index]") {
    SlotIndexProbe<AmsBackendQidi> backend;
    backend.set_total_slots(3);

    CHECK(backend.load_filament(3).result == AmsResult::INVALID_SLOT);
    CHECK(backend.unload_filament(3).result == AmsResult::INVALID_SLOT);
    CHECK(backend.eject_lane(3).result == AmsResult::INVALID_SLOT);
    CHECK(backend.set_tool_mapping(0, 3).result == AmsResult::INVALID_SLOT);
}

TEST_CASE("QIDI Box with no unit configured refuses a slot as nothing discovered",
          "[ams][qidi_box][slot_index]") {
    SlotIndexProbe<AmsBackendQidi> backend;
    backend.clear_units();

    CHECK(backend.unload_filament(0).result == AmsResult::NOT_CONNECTED);
    CHECK(backend.load_filament(0).result == AmsResult::NOT_CONNECTED);
}
