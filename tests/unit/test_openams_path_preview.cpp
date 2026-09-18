// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Opt-in visual fixture: renders the production AMS panel with a native
// OpenAMS status snapshot. No connected client, printer, or AFC backend.
// HELIX_OPENAMS_PREVIEW_PNG=/absolute/path.png helix-tests '[.openams-preview]'
// Set HELIX_OPENAMS_PREVIEW_LIGHT=1 in a separate process for light mode.
#include "ui_panel_ams.h"
#include "ui_update_queue.h"

#include "../../src/ui/ui_filament_path_internal.h"
#include "../test_fixtures.h"
#include "../test_helpers/backend_user_edit.h"
#include "ams_backend_openams.h"
#include "ams_state.h"
#include "screenshot.h"
#include "theme_manager.h"
#include "tool_state.h"

#include <cstdlib>
#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;
using json = nlohmann::json;

namespace {
class PreviewOpenAms : public AmsBackendOpenAms {
  public:
    PreviewOpenAms() : AmsBackendOpenAms(nullptr, nullptr) {}
    using AmsBackendOpenAms::handle_status_update;
};

struct ScopedPreviewBackend {
    ~ScopedPreviewBackend() {
        AmsState::instance().set_backend(nullptr);
    }
};

class OpenAmsPreviewFixture : public XMLTestFixture {
  public:
    OpenAmsPreviewFixture()
        : XMLTestFixture(std::getenv("HELIX_OPENAMS_PREVIEW_LIGHT") == nullptr) {}
};

struct ScopedPreviewPanel {
    AmsPanel& panel;
    lv_obj_t* obj;
    ~ScopedPreviewPanel() {
        panel.on_deactivate(DeactivateReason::NavigateAway);
        panel.clear_panel_reference();
        lv_obj_delete(obj);
        helix::ui::UpdateQueue::instance().drain();
    }
};
} // namespace

TEST_CASE_METHOD(OpenAmsPreviewFixture, "OpenAMS four-spool dark filament preview",
                 "[.openams-preview]") {
    const char* output = std::getenv("HELIX_OPENAMS_PREVIEW_PNG");
    if (!output)
        SKIP("Set HELIX_OPENAMS_PREVIEW_PNG to an absolute PNG path");

    // Seed only fake UI state: the client stays disconnected and no commands
    // are sent. Otherwise the panel's safety binding dims the entire preview.
    state().set_printer_connection_state_internal(2, "Preview");
    state().set_klippy_state_sync(KlippyState::READY);

    auto backend = std::make_unique<PreviewOpenAms>();
    json slots = json::array();
    json groups = json::array();
    for (int i = 0; i < 4; ++i) {
        slots.push_back({{"id", i}, {"bay", i}, {"ready", i >= 2}, {"loaded", i == 3}});
        if (i >= 2)
            groups.push_back({{"name", i == 3 ? "T0" : "T2"}, {"lane", "fps"}, {"slots", {i}}});
    }
    backend->handle_status_update({{"oams_manager",
                                    {{"api_version", 1},
                                     {"schema", "openams.manager"},
                                     {"ready", true},
                                     {"commands",
                                      {{"load", "OPENAMS_LOAD"},
                                       {"unload", "OPENAMS_UNLOAD"},
                                       {"cancel", "OAMSM_LOAD_FILAMENT_CANCEL"},
                                       {"reset", "OAMSM_CLEAR_ERRORS"}}},
                                     {"lanes", json::array({{{"id", "fps"},
                                                             {"state", "loaded"},
                                                             {"current_group", "T0"},
                                                             {"current_slot", 3}}})},
                                     {"units", json::array({{{"id", "1"},
                                                             {"name", "OpenAMS"},
                                                             {"kind", "oams"},
                                                             {"topology", "hub"},
                                                             {"lane", "fps"},
                                                             {"connected", true},
                                                             {"slots", slots}}})},
                                     {"groups", groups}}}});
    REQUIRE(backend->get_system_info().total_slots == 4);
    for (int i : {2, 3}) {
        auto slot = backend->get_slot_info(i);
        slot.material = "ABS";
        slot.color_rgb = i == 2 ? 0x77ee22 : 0x000000;
        slot.color_name = i == 2 ? "Green" : "Black";
        REQUIRE(helix::test::apply_edit(*backend, i, slot).success());
    }
    ScopedPreviewBackend cleanup;
    ToolState::instance().init_subjects();
    AmsState::instance().set_backend(std::move(backend));
    AmsState::instance().init_subjects(true);
    AmsState::instance().sync_from_backend();
    ensure_ams_widgets_registered();
    AmsPanel panel(state(), &api());
    panel.init_subjects();
    auto* obj = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "ams_panel", nullptr));
    REQUIRE(obj != nullptr);
    ScopedPreviewPanel panel_cleanup{panel, obj};
    panel.setup(obj, test_screen());
    panel.on_activate();
    lv_obj_update_layout(test_screen());

    const bool dark_mode = std::getenv("HELIX_OPENAMS_PREVIEW_LIGHT") == nullptr;
    CHECK((lv_color_luminance(lv_obj_get_style_bg_color(obj, LV_PART_MAIN)) < 128) == dark_mode);
    auto* left = lv_obj_find_by_name(obj, "left_column");
    REQUIRE(left != nullptr);
    CHECK_FALSE(lv_obj_has_state(left, LV_STATE_DISABLED));
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(100);
    lv_obj_update_layout(test_screen());

    auto* canvas = lv_obj_find_by_name(obj, "path_canvas");
    REQUIRE(canvas != nullptr);
    auto* path_data = helix::ui::fpath::get_data(canvas);
    REQUIRE(path_data != nullptr);
    CAPTURE(path_data->topology, path_data->hub_only, path_data->hub_on_toolhead,
            path_data->show_bypass, path_data->buffer_present, path_data->error_segment,
            path_data->active_slot, path_data->filament_segment);
    const auto& route = path_data->path_cache.path;
    REQUIRE(route.count > 2);
    // The loaded route must not stop at a sensor and restart a few pixels
    // later: independently capped strokes create double outlines and gaps.
    for (int i = 1; i < route.count; ++i) {
        CAPTURE(i);
        CHECK(route.segs[i].p0.x == Catch::Approx(route.segs[i - 1].p1.x).margin(0.01));
        CHECK(route.segs[i].p0.y == Catch::Approx(route.segs[i - 1].p1.y).margin(0.01));
    }

    CapturedFrame frame;
    REQUIRE(capture_frame(frame));
    const int join_x = static_cast<int>(route.segs[0].p1.x);
    const int join_y = static_cast<int>(route.segs[0].p1.y) - 3;
    REQUIRE(join_x >= 0);
    REQUIRE(join_x < frame.width);
    REQUIRE(join_y >= 0);
    REQUIRE(join_y < frame.height);
    const auto* pixel = &frame.rgba[(join_y * frame.width + join_x) * 4];
    // Light-mode high-contrast filament retains the existing 44/255 core
    // highlight; neither mode should have the much brighter halo over its core.
    CHECK(lv_color_luminance(lv_color_make(pixel[0], pixel[1], pixel[2])) <= 44);
    CHECK_FALSE(write_frame(frame, output).empty());
}
