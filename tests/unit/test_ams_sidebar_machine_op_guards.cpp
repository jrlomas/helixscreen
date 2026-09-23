// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_sidebar_machine_op_guards.cpp
 * @brief The AMS sidebar's Reset and Check-slots buttons must obey their guards.
 *
 * Neither button has a backend guard behind it. Every backend's reset() calls
 * check_preconditions() with the default requires_toolhead_motion = false, and
 * check_all_gates() checks only that the backend is running, so
 * refuse_if_printing() never runs for either. The disabled binding in
 * ams_sidebar.xml is the whole affordance-level guard
 * (prestonbrown/helixscreen#1523).
 *
 * compute_machine_op_gating() is unit-tested in
 * test_filament_op_slot_resolver.cpp; what this file pins is the wiring the rule
 * feeds — that the two subjects exist, and that the two buttons in the
 * production XML actually take LV_STATE_DISABLED from them. Deleting either
 * bind_state_if_eq leaves every other test in the tree green.
 */

#include "ui_ams_sidebar.h"
#include "ui_ams_slot.h"
#include "ui_endless_spool_arrows.h"
#include "ui_filament_path_canvas.h"
#include "ui_spool_canvas.h"

#include "../test_fixtures.h"
#include "ams_backend_mock.h"
#include "ams_backend_snapmaker.h"
#include "ams_state.h"
#include "helix-xml/src/xml/lv_xml.h"

#include <lvgl.h>
#include <memory>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

/// The sidebar's own components, registered the way production registers them.
void register_sidebar_xml_once() {
    static bool done = false;
    if (done) {
        return;
    }
    ui_spool_canvas_register();
    ui_ams_slot_register();
    ui_filament_path_canvas_register();
    ui_endless_spool_arrows_register();
    helix::ui::AmsOperationSidebar::register_callbacks_static();
    lv_xml_register_component_from_file("A:ui_xml/components/ams_loaded_card.xml");
    lv_xml_register_component_from_file("A:ui_xml/components/ams_sidebar.xml");
    done = true;
}

/// Build the real sidebar from production XML, with AmsState's subjects live so
/// the bindings resolve against something.
struct SidebarGuardFixture : public XMLTestFixture {
    SidebarGuardFixture() {
        auto mock = std::make_unique<helix::AmsBackendMock>(4);
        REQUIRE(mock->start().success());
        helix::AmsState::instance().set_backend(std::move(mock));
        helix::AmsState::instance().init_subjects(true);
        helix::AmsState::instance().sync_from_backend();

        register_sidebar_xml_once();
        root_ = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "ams_sidebar", nullptr));
        REQUIRE(root_ != nullptr);
    }

    ~SidebarGuardFixture() override {
        if (root_ && lv_obj_is_valid(root_)) {
            lv_obj_delete(root_);
        }
        root_ = nullptr;
        helix::AmsState::instance().set_backend(nullptr);
    }

    [[nodiscard]] lv_obj_t* button(const char* name) const {
        lv_obj_t* btn = lv_obj_find_by_name(root_, name);
        REQUIRE(btn != nullptr);
        return btn;
    }

    /// The gating subjects are file-static in ui_ams_sidebar.cpp and reachable
    /// only by the name the XML binds, which is the coupling under test.
    [[nodiscard]] static lv_subject_t* gate(const char* name) {
        lv_subject_t* subject = lv_xml_get_subject(nullptr, name);
        REQUIRE(subject != nullptr);
        return subject;
    }

    lv_obj_t* root_ = nullptr;
};

/// A batch-capable backend whose per-head toolhead state the test fixes: the
/// picker's rows and the sidebar's Load/Unload gating both read
/// can_unload_from_toolhead(), so this is the seam they share.
class FixedHeadsBackend : public helix::AmsBackendSnapmaker {
  public:
    explicit FixedHeadsBackend(std::vector<bool> unloadable)
        : helix::AmsBackendSnapmaker(nullptr, nullptr), unloadable_(std::move(unloadable)) {}

    helix::AmsSystemInfo get_system_info() const override {
        helix::AmsSystemInfo info;
        info.total_slots = static_cast<int>(unloadable_.size());
        return info;
    }
    bool can_unload_from_toolhead(int slot_index) const override {
        return slot_index >= 0 && slot_index < static_cast<int>(unloadable_.size()) &&
               unloadable_[slot_index];
    }

  private:
    std::vector<bool> unloadable_;
};

/// Builds the production sidebar over a chosen backend, so the gating subjects
/// carry the value production would show. setup() ends in
/// refresh_button_gating(), which is the path under test.
struct SidebarDirectionGateFixture : public XMLTestFixture {
    void build(std::unique_ptr<helix::AmsBackend> backend) {
        helix::AmsState::instance().set_backend(std::move(backend));
        helix::AmsState::instance().init_subjects(true);
        helix::AmsState::instance().sync_from_backend();

        register_sidebar_xml_once();
        // Built the way ams_panel.xml builds it: the sidebar sits inside a
        // panel, named, and setup() takes the panel. find_by_name matches
        // descendants, so passing the sidebar itself would not find it.
        panel_ = lv_obj_create(test_screen());
        const char* attrs[] = {"name", "ams_operation_sidebar", nullptr};
        root_ = static_cast<lv_obj_t*>(lv_xml_create(panel_, "ams_sidebar", attrs));
        REQUIRE(root_ != nullptr);
        sidebar_ = std::make_unique<helix::ui::AmsOperationSidebar>(state());
        REQUIRE(sidebar_->setup(panel_));
    }

    ~SidebarDirectionGateFixture() override {
        sidebar_.reset(); // cancels the stall watchdog before the tree goes
        if (panel_ && lv_obj_is_valid(panel_)) {
            lv_obj_delete(panel_); // takes the sidebar tree with it
        }
        panel_ = nullptr;
        root_ = nullptr;
        helix::AmsState::instance().set_backend(nullptr);
    }

    [[nodiscard]] static int gate_value(const char* name) {
        lv_subject_t* subject = lv_xml_get_subject(nullptr, name);
        REQUIRE(subject != nullptr);
        return lv_subject_get_int(subject);
    }

    [[nodiscard]] lv_obj_t* button(const char* name) const {
        lv_obj_t* btn = lv_obj_find_by_name(root_, name);
        REQUIRE(btn != nullptr);
        return btn;
    }

    std::unique_ptr<helix::ui::AmsOperationSidebar> sidebar_;
    lv_obj_t* panel_ = nullptr; // the panel root handed to setup()
    lv_obj_t* root_ = nullptr;  // the sidebar component itself
};

} // namespace

TEST_CASE_METHOD(SidebarGuardFixture, "AMS sidebar Reset takes its disabled state from the guard",
                 "[ams][sidebar][print_guard]") {
    lv_obj_t* btn = button("btn_reset");
    lv_subject_t* subject = gate("ams_sidebar_reset_disabled");

    // Both directions: a binding wired only one way would let the button latch
    // disabled after the first print and never come back.
    lv_subject_set_int(subject, 1);
    CHECK(lv_obj_has_state(btn, LV_STATE_DISABLED));

    lv_subject_set_int(subject, 0);
    CHECK_FALSE(lv_obj_has_state(btn, LV_STATE_DISABLED));

    lv_subject_set_int(subject, 1);
    CHECK(lv_obj_has_state(btn, LV_STATE_DISABLED));
}

TEST_CASE_METHOD(SidebarGuardFixture,
                 "AMS sidebar Check slots takes its disabled state from the guard",
                 "[ams][sidebar][print_guard]") {
    lv_obj_t* btn = button("btn_check_gates");
    lv_subject_t* subject = gate("ams_sidebar_check_gates_disabled");

    lv_subject_set_int(subject, 1);
    CHECK(lv_obj_has_state(btn, LV_STATE_DISABLED));

    lv_subject_set_int(subject, 0);
    CHECK_FALSE(lv_obj_has_state(btn, LV_STATE_DISABLED));

    lv_subject_set_int(subject, 1);
    CHECK(lv_obj_has_state(btn, LV_STATE_DISABLED));
}

TEST_CASE_METHOD(SidebarDirectionGateFixture,
                 "AMS sidebar Load and Unload gate per direction on a batch backend",
                 "[ams][sidebar][batch]") {
    SECTION("heads loaded with no tool on the carriage keep Unload live") {
        build(std::make_unique<FixedHeadsBackend>(std::vector<bool>{true, false, true, false}));
        lv_subject_t* loaded = helix::AmsState::instance().get_filament_loaded_subject();
        REQUIRE(loaded != nullptr);
        REQUIRE(lv_subject_get_int(loaded) == 0); // no tool on the carriage
        CHECK(gate_value("ams_sidebar_supports_batch") == 1);
        CHECK_FALSE(lv_obj_has_flag(button("btn_batch_load"), LV_OBJ_FLAG_HIDDEN));
        CHECK(gate_value("ams_sidebar_unload_disabled") == 0);
        CHECK(gate_value("ams_sidebar_load_disabled") == 0);
    }
    SECTION("no head can unload disables Unload, not Load") {
        build(std::make_unique<FixedHeadsBackend>(std::vector<bool>{false, false, false, false}));
        CHECK(gate_value("ams_sidebar_unload_disabled") == 1);
        CHECK(gate_value("ams_sidebar_load_disabled") == 0);
    }
    SECTION("every head already loaded disables Load, not Unload") {
        build(std::make_unique<FixedHeadsBackend>(std::vector<bool>{true, true, true, true}));
        CHECK(gate_value("ams_sidebar_unload_disabled") == 0);
        CHECK(gate_value("ams_sidebar_load_disabled") == 1);
    }
    SECTION("non-batch backends keep the aggregate rule and no Load button") {
        // The aggregate flag says loaded while no head answers can_unload: the
        // per-head rule leaking into non-batch backends would grey this out.
        class LoadedButStuckBackend : public helix::AmsBackendMock {
          public:
            LoadedButStuckBackend() : helix::AmsBackendMock(4) {}
            bool can_unload_from_toolhead(int) const override {
                return false;
            }
        };
        auto mock = std::make_unique<LoadedButStuckBackend>();
        REQUIRE(mock->start().success());
        build(std::move(mock));
        CHECK(gate_value("ams_sidebar_supports_batch") == 0);
        CHECK(lv_obj_has_flag(button("btn_batch_load"), LV_OBJ_FLAG_HIDDEN));
        lv_subject_t* loaded = helix::AmsState::instance().get_filament_loaded_subject();
        REQUIRE(loaded != nullptr);
        REQUIRE(lv_subject_get_int(loaded) == 1); // something IS loaded
        CHECK(gate_value("ams_sidebar_unload_disabled") == 0);
        CHECK(gate_value("ams_sidebar_load_disabled") == 1);
    }
}
