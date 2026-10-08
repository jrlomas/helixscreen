// SPDX-License-Identifier: GPL-3.0-or-later

// A modal's entrance and exit fade the full-screen backdrop and scale the
// dialog through a transform layer on every frame. The limited tiers show and
// remove the modal in its final state instead.

#include "ui_modal.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "display_settings_manager.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "platform_capabilities.h"

#include "../catch_amalgamated.hpp"

using helix::PlatformTier;

namespace {
constexpr const char* TEST_COMPONENT = "print_cancel_confirm_modal";

struct TierScope {
    lv_subject_t* tier = lv_xml_get_subject(nullptr, "platform_tier");
    int saved_tier = 0;
    bool saved_anims = helix::DisplaySettingsManager::instance().get_animations_enabled();

    explicit TierScope(PlatformTier t) {
        REQUIRE(tier != nullptr);
        saved_tier = lv_subject_get_int(tier);
        lv_subject_set_int(tier, static_cast<int>(t));
        helix::DisplaySettingsManager::instance().set_animations_enabled(true);
    }
    ~TierScope() {
        lv_anim_delete_all();
        ModalStack::instance().clear();
        lv_subject_set_int(tier, saved_tier);
        helix::DisplaySettingsManager::instance().set_animations_enabled(saved_anims);
        helix::ui::UpdateQueue::instance().drain();
    }
};
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "modal: limited tiers show and hide without animating",
                 "[modal][modal_tier][platform_tier]") {
    for (PlatformTier t : {PlatformTier::BASIC, PlatformTier::EMBEDDED}) {
        TierScope scope(t);
        lv_obj_t* dialog = Modal::show(TEST_COMPONENT);
        REQUIRE(dialog != nullptr);
        lv_obj_t* backdrop = ModalStack::instance().backdrop_for(dialog);
        REQUIRE(backdrop != nullptr);

        CHECK(lv_obj_get_style_transform_scale_x(dialog, LV_PART_MAIN) == LV_SCALE_NONE);
        CHECK(lv_obj_get_style_transform_scale_y(dialog, LV_PART_MAIN) == LV_SCALE_NONE);
        CHECK(lv_anim_get(dialog, nullptr) == nullptr);
        CHECK(lv_anim_get(backdrop, nullptr) == nullptr);
        CHECK(lv_obj_get_style_opa(backdrop, LV_PART_MAIN) == LV_OPA_COVER);
        CHECK(lv_obj_get_style_opa(dialog, LV_PART_MAIN) == LV_OPA_COVER);

        Modal::hide(dialog);
        CHECK(ModalStack::instance().stack_empty());
        CHECK_FALSE(ModalStack::instance().is_exiting(backdrop));
        CHECK(lv_anim_get(backdrop, nullptr) == nullptr);
        process_lvgl(50);
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "modal: the capable tier still animates entrance and exit",
                 "[modal][modal_tier][platform_tier]") {
    TierScope scope(PlatformTier::STANDARD);
    lv_obj_t* dialog = Modal::show(TEST_COMPONENT);
    REQUIRE(dialog != nullptr);
    lv_obj_t* backdrop = ModalStack::instance().backdrop_for(dialog);
    REQUIRE(backdrop != nullptr);

    CHECK(lv_obj_get_style_transform_scale_x(dialog, LV_PART_MAIN) < LV_SCALE_NONE);
    CHECK(lv_anim_get(dialog, nullptr) != nullptr);
    CHECK(lv_anim_get(backdrop, nullptr) != nullptr);

    Modal::hide(dialog);
    CHECK(ModalStack::instance().is_exiting(backdrop));
    CHECK(lv_anim_get(backdrop, nullptr) != nullptr);
}
