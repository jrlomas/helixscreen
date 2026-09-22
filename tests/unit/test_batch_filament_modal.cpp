// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_batch_filament_modal.cpp
 * @brief Pure-logic tests for the batch filament picker modal
 *
 * The widget plumbing is LVGL-driven and covered by driving the mock; these
 * cases pin the two decisions that must hold regardless of the screen.
 */

#include "ui_batch_filament_modal.h"

#include "ams_backend_snapmaker.h"
#include "lvgl/src/others/translation/lv_translation.h"

#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::BatchFilamentModal;

namespace {
/// Every lane holds filament; only heads 0 and 2 are loaded at the toolhead.
/// This is the live rig state: four channels reporting filament_detected with
/// two at load_finish and two at preload_finish.
class DisagreeingBackend : public helix::AmsBackendSnapmaker {
  public:
    DisagreeingBackend() : helix::AmsBackendSnapmaker(nullptr, nullptr) {}

    helix::AmsSystemInfo get_system_info() const override {
        helix::AmsSystemInfo info;
        info.total_slots = 4;
        return info;
    }
    helix::SlotInfo get_slot_info(int slot_index) const override {
        helix::SlotInfo slot;
        slot.slot_index = slot_index;
        slot.status = helix::SlotStatus::AVAILABLE; // lane has filament
        return slot;
    }
    bool can_unload_from_toolhead(int slot_index) const override {
        return slot_index == 0 || slot_index == 2;
    }
};
} // namespace

TEST_CASE("BatchFilamentModal prefill ticks by direction", "[ams][batch]") {
    using o = std::optional<bool>;
    const std::vector<std::optional<bool>> presence{o(true), o(false), o(), o(true)};

    SECTION("unload ticks only heads with filament at the toolhead") {
        REQUIRE(BatchFilamentModal::prefill_selection(presence, false) ==
                std::vector<bool>{true, false, false, true});
    }
    SECTION("load ticks the rest; an unpublished presence reads as loadable") {
        REQUIRE(BatchFilamentModal::prefill_selection(presence, true) ==
                std::vector<bool>{false, true, true, false});
    }
    SECTION("no slots yields no ticks") {
        REQUIRE(BatchFilamentModal::prefill_selection({}, true).empty());
        REQUIRE(BatchFilamentModal::prefill_selection({}, false).empty());
    }
}

TEST_CASE("BatchFilamentModal collects toolhead state separately from lane presence",
          "[ams][batch]") {
    DisagreeingBackend backend;

    const auto rows = BatchFilamentModal::collect_rows(backend);

    REQUIRE(rows.slots.size() == 4);
    CHECK(rows.lane_presence == std::vector<std::optional<bool>>{true, true, true, true});
    CHECK(rows.at_toolhead == std::vector<std::optional<bool>>{true, false, true, false});

    // The whole point: Unload pre-ticks the loaded heads, not every full lane.
    CHECK(BatchFilamentModal::prefill_selection(rows.at_toolhead, /*for_load=*/false) ==
          std::vector<bool>{true, false, true, false});
}

TEST_CASE("BatchFilamentModal selected keys convert to slot indices", "[ams][batch]") {
    SECTION("keys in row order become slots") {
        REQUIRE(BatchFilamentModal::selected_slots({"0", "2", "3"}) == std::vector<int>{0, 2, 3});
    }
    SECTION("single slot") {
        REQUIRE(BatchFilamentModal::selected_slots({"1"}) == std::vector<int>{1});
    }
    SECTION("empty selection stays empty") {
        REQUIRE(BatchFilamentModal::selected_slots({}).empty());
    }
}

TEST_CASE("BatchFilamentModal row label names the lane contents", "[ams][batch]") {
    using helix::SlotInfo;
    using helix::ui::LaneNoun;

    SECTION("a loaded lane names its material") {
        SlotInfo info;
        info.material = "PETG";
        REQUIRE(BatchFilamentModal::row_label(LaneNoun::Feeder, 0, info, true) ==
                "Feeder 1 (PETG)");
    }
    SECTION("a lane known to be empty says so") {
        REQUIRE(BatchFilamentModal::row_label(LaneNoun::Feeder, 1, SlotInfo{}, false) ==
                std::string("Feeder 2 (") + lv_tr("Empty") + ")");
    }
    SECTION("an unanswerable presence leaves the lane name bare") {
        REQUIRE(BatchFilamentModal::row_label(LaneNoun::Feeder, 2, SlotInfo{}, std::nullopt) ==
                "Feeder 3");
    }
    SECTION("material wins over presence, so a stale nullopt cannot blank a loaded lane") {
        SlotInfo info;
        info.material = "PLA";
        REQUIRE(BatchFilamentModal::row_label(LaneNoun::Slot, 0, info, std::nullopt) ==
                "Slot 1 (PLA)");
    }
}
