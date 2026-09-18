// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../test_fixtures.h"
#include "filament_tube_stroker.h"
#include "screenshot.h"
#include "theme_manager.h"

#include <cmath>
#include <cstdlib>

#include "../catch_amalgamated.hpp"

using namespace helix::ui;

TEST_CASE_METHOD(XMLTestFixture, "joined tube retains its black center through a bend",
                 "[filament-path][tube-contrast][tube-junction]") {
    auto* canvas = lv_canvas_create(test_screen());
    auto* buffer = lv_draw_buf_create(64, 100, LV_COLOR_FORMAT_ARGB8888, 0);
    REQUIRE(buffer != nullptr);
    struct Cleanup {
        lv_obj_t* canvas;
        lv_draw_buf_t* buffer;
        ~Cleanup() {
            lv_obj_delete(canvas);
            lv_draw_buf_destroy(buffer);
        }
    } cleanup{canvas, buffer};
    lv_canvas_set_draw_buf(canvas, buffer);
    auto bg = lv_color_hex(0x202020);
    lv_canvas_fill_bg(canvas, bg, LV_OPA_COVER);
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);
    LaneStyle style{true, lv_color_black(), bg, 3, true};
    pg::FilamentPath path;
    path.add_line(30, 10, 30, 30);
    pg::PathPoint pts[] = {{30, 30}, {30, 50}, {10, 70}, {10, 90}};
    pg::route_polyline_filleted(path, pts, 4, 12);
    draw_lane(&layer, path, style);
    lv_canvas_finish_layer(canvas, &layer);
    lv_obj_update_layout(test_screen());
    helix::CapturedFrame frame;
    REQUIRE(helix::capture_frame(frame, canvas));
    REQUIRE(frame.width == 64);
    auto pixel = [&](int x, int y) {
        const auto* p = &frame.rgba[(y * frame.width + x) * 4];
        return lv_color_make(p[0], p[1], p[2]);
    };
    // No later halo may overpaint the filament at the split or first bend.
    for (float d = 2; d < pg::path_length(path) - 2; d += 1) {
        auto point = pg::path_point_at(path, d);
        // LVGL's diagonal rasterizer can place the darkest center pixel one
        // pixel off the float centerline. Require a true black core nearby,
        // not merely a dark background pixel in an unpainted gap.
        int darkest = 255;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                auto color = pixel(static_cast<int>(std::round(point.x)) + dx,
                                   static_cast<int>(std::round(point.y)) + dy);
                darkest = std::min(darkest, int(lv_color_luminance(color)));
            }
        CAPTURE(d, point.x, point.y);
        CHECK(darkest < 10);
    }
    CHECK(pixel(37, 25).blue > pixel(37, 25).red);
}

TEST_CASE_METHOD(XMLTestFixture, "neutral filament glow follows the selection accent",
                 "[filament-path][tube-contrast]") {
    for (auto filament : {0x000000, 0xffffff}) {
        for (auto background : {0x202020, 0xf0f0f0}) {
            auto glow = get_glow_color(lv_color_hex(filament), lv_color_hex(background));
            auto accent = theme_manager_get_color("primary");
            auto expected = background == 0x202020 ? tube_lighten(accent, 40) : accent;
            CHECK(lv_color_eq(glow, expected));
        }
    }
}

TEST_CASE_METHOD(XMLTestFixture, "low contrast filament retains its color inside a visible rim",
                 "[filament-path][tube-contrast]") {
    for (bool simple : {false, true}) {
        for (bool light : {false, true}) {
            CAPTURE(simple, light);
            auto color = lv_color_hex(light ? 0xffffff : 0x000000);
            auto bg = lv_color_hex(light ? 0xf0f0f0 : 0x202020);
            LaneStyle style{true, color, bg, 3, true};
            TubePass passes[4];
            int count = build_passes(style, passes, simple);
            REQUIRE(count == (simple ? 2 : 4));
            // The final pass is actual filament color, not a gray substitute.
            CHECK(lv_color_eq(passes[count - 1].color, color));
            CHECK(passes[count - 1].width == 3);
            CHECK(passes[count - 1].opa == LV_OPA_COVER);
            const auto& rim = passes[count - 2];
            CHECK(rim.width == 5);
            CHECK(rim.opa == LV_OPA_COVER);
            CHECK(std::abs(int(lv_color_luminance(rim.color)) - int(lv_color_luminance(bg))) >= 90);
        }
    }
}

TEST_CASE("loaded tube glow is stronger and wider without changing idle tubes",
          "[filament-path][tube-contrast]") {
    TubePass passes[4];
    auto green = lv_color_hex(0x55ee33);
    auto bg = lv_color_hex(0x202020);
    LaneStyle style{true, green, bg, 3, true};
    REQUIRE(build_passes(style, passes, false) == 4);
    CHECK(passes[0].width == 17);
    CHECK(passes[1].width == 10);
    CHECK(GLOW_OPA >= 140);
    // Preblended opaque bands allow round joins without double-blending seams.
    CHECK(passes[0].opa == LV_OPA_COVER);
    CHECK(passes[1].opa == LV_OPA_COVER);
    auto glow = get_glow_color(green, bg);
    CHECK(lv_color_eq(passes[0].color, tube_blend(bg, glow, (GLOW_OPA / 2.0f) / 255.0f)));
    CHECK(lv_color_eq(passes[1].color, tube_blend(bg, glow, GLOW_OPA / 255.0f)));
    style.glow = false;
    REQUIRE(build_passes(style, passes, false) == 2);
    CHECK(passes[0].width == 3);
    style.solid = false;
    REQUIRE(build_passes(style, passes, false) == 3);
    CHECK(passes[0].width == 5);
    CHECK(passes[1].width == 3);
    CHECK(passes[2].width == 1);
    CHECK(lv_color_eq(passes[2].color, bg));
}
