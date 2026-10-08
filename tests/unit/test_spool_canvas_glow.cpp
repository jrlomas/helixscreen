// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The current-spool highlight: an accent glow grown from the spool's own
// silhouette. Pixels are read straight from the canvas draw buffer; a "glow
// pixel" is one the highlighted render paints where the plain render is
// transparent.

#include "ui_spool_canvas.h"

#include "../lvgl_ui_test_fixture.h"
#include "filament_tube_stroker.h"

#include <cstdlib>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

constexpr int32_t SPOOL = 60;

struct Pixels {
    int32_t w = 0;
    int32_t h = 0;
    std::vector<lv_color32_t> px;
    const lv_color32_t& at(int32_t x, int32_t y) const {
        return px[y * w + x];
    }
};

Pixels snapshot(lv_obj_t* canvas) {
    lv_draw_buf_t* buf = lv_canvas_get_draw_buf(canvas);
    REQUIRE(buf != nullptr);
    Pixels p;
    p.w = buf->header.w;
    p.h = buf->header.h;
    p.px.resize(static_cast<size_t>(p.w * p.h));
    for (int32_t y = 0; y < p.h; y++) {
        const auto* row = reinterpret_cast<const lv_color32_t*>(buf->data + y * buf->header.stride);
        for (int32_t x = 0; x < p.w; x++)
            p.px[y * p.w + x] = row[x];
    }
    return p;
}

bool same(const Pixels& a, const Pixels& b) {
    if (a.w != b.w || a.h != b.h)
        return false;
    for (size_t i = 0; i < a.px.size(); i++) {
        const auto &p = a.px[i], &q = b.px[i];
        if (p.red != q.red || p.green != q.green || p.blue != q.blue || p.alpha != q.alpha)
            return false;
    }
    return true;
}

/// Chebyshev distance from every glow pixel to the plain silhouette, with the
/// plain render sitting @p m px in from the highlighted buffer's corner.
struct GlowReach {
    int count = 0;
    int max_dist = 0;
    lv_color32_t farthest{};
};

GlowReach glow_reach(const Pixels& plain, const Pixels& lit, int32_t m) {
    std::vector<std::pair<int32_t, int32_t>> solid;
    for (int32_t y = 0; y < plain.h; y++)
        for (int32_t x = 0; x < plain.w; x++)
            if (plain.at(x, y).alpha > 0)
                solid.emplace_back(x + m, y + m);
    REQUIRE_FALSE(solid.empty());

    GlowReach r;
    for (int32_t y = 0; y < lit.h; y++) {
        for (int32_t x = 0; x < lit.w; x++) {
            const int32_t px = x - m, py = y - m;
            const bool inside = px >= 0 && py >= 0 && px < plain.w && py < plain.h;
            if ((inside && plain.at(px, py).alpha > 0) || lit.at(x, y).alpha == 0)
                continue;
            int best = 1 << 30;
            for (auto [sx, sy] : solid)
                best = std::min(best, std::max(std::abs(sx - x), std::abs(sy - y)));
            r.count++;
            if (best > r.max_dist) {
                r.max_dist = best;
                r.farthest = lit.at(x, y);
            }
        }
    }
    return r;
}

lv_obj_t* make_canvas(lv_obj_t* parent) {
    ui_spool_canvas_invalidate_cache();
    lv_obj_t* c = ui_spool_canvas_create(parent, SPOOL);
    REQUIRE(c != nullptr);
    ui_spool_canvas_set_color(c, lv_color_hex(0xDE5923));
    ui_spool_canvas_set_fill_level(c, 0.8f);
    return c;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "spool_canvas: simple highlight is a tight outline, no tail",
                 "[spool_canvas][highlight]") {
    lv_obj_t* c = make_canvas(test_screen());
    const Pixels plain = snapshot(c);

    helix::ui::spool_canvas_set_highlighted(c, true, /*simple=*/true);
    CHECK(helix::ui::spool_canvas_highlighted(c));
    const Pixels lit = snapshot(c);

    // Same buffer: the outline fits inside the spool's own canvas.
    REQUIRE(lit.w == SPOOL);
    REQUIRE(lit.h == SPOOL);
    const GlowReach r = glow_reach(plain, lit, 0);
    CHECK(r.count > 20);
    CHECK(r.max_dist <= 2);

    lv_obj_delete(c);
}

TEST_CASE_METHOD(LVGLUITestFixture, "spool_canvas: capable highlight glows past 3px, inside margin",
                 "[spool_canvas][highlight]") {
    lv_obj_t* c = make_canvas(test_screen());
    const Pixels plain = snapshot(c);

    helix::ui::spool_canvas_set_highlighted(c, true, /*simple=*/false);
    const Pixels lit = snapshot(c);

    // The buffer grows by the margin on every side; the object keeps the spool's size.
    REQUIRE(lit.w > SPOOL);
    REQUIRE(lit.w == lit.h);
    REQUIRE((lit.w - SPOOL) % 2 == 0);
    const int32_t m = (lit.w - SPOOL) / 2;
    lv_obj_update_layout(c);
    CHECK(lv_obj_get_width(c) == SPOOL);
    CHECK(lv_obj_get_height(c) == SPOOL);

    const GlowReach r = glow_reach(plain, lit, m);
    CHECK(r.max_dist > 3);
    CHECK(r.max_dist <= m);

    // The far halo is the accent itself.
    const lv_color_t accent = helix::ui::tube_accent();
    CHECK(std::abs(r.farthest.red - accent.red) <= 2);
    CHECK(std::abs(r.farthest.green - accent.green) <= 2);
    CHECK(std::abs(r.farthest.blue - accent.blue) <= 2);

    // Nothing clips: the outermost ring of the buffer is (near) transparent.
    int edge_max = 0;
    for (int32_t i = 0; i < lit.w; i++) {
        edge_max = std::max({edge_max, static_cast<int>(lit.at(i, 0).alpha),
                             static_cast<int>(lit.at(i, lit.h - 1).alpha),
                             static_cast<int>(lit.at(0, i).alpha),
                             static_cast<int>(lit.at(lit.w - 1, i).alpha)});
    }
    CHECK(edge_max <= 4);

    lv_obj_delete(c);
}

TEST_CASE_METHOD(LVGLUITestFixture, "spool_canvas: highlighted and plain renders cache separately",
                 "[spool_canvas][highlight]") {
    for (bool simple : {true, false}) {
        CAPTURE(simple);
        lv_obj_t* c = make_canvas(test_screen());
        const Pixels plain = snapshot(c);

        helix::ui::spool_canvas_set_highlighted(c, true, simple);
        const Pixels lit = snapshot(c);
        CHECK_FALSE(same(plain, lit));

        helix::ui::spool_canvas_set_highlighted(c, false, simple);
        CHECK_FALSE(helix::ui::spool_canvas_highlighted(c));
        CHECK(same(plain, snapshot(c)));

        // Back on: served from the cache, identical to the first highlighted render.
        helix::ui::spool_canvas_set_highlighted(c, true, simple);
        CHECK(same(lit, snapshot(c)));

        lv_obj_delete(c);
    }
}
