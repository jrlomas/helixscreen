// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "helix/ui/shared_font_style.h"

#include <spdlog/spdlog.h>

#include <cstdlib>

namespace helix::ui {

namespace {

// Append-only: one entry per compiled face a caller has ever resolved to (at
// most one per tier per role), so a fixed table is enough and nothing here
// needs the heap.
constexpr size_t MAX_FACES = 32;
const lv_font_t* g_faces[MAX_FACES] = {};
lv_style_t g_styles[MAX_FACES];

} // namespace

lv_style_t* shared_font_style(const lv_font_t* font) {
    for (size_t i = 0; i < MAX_FACES; ++i) {
        if (g_faces[i] == font)
            return &g_styles[i];
        if (g_faces[i] == nullptr) {
            g_faces[i] = font;
            lv_style_init(&g_styles[i]);
            lv_style_set_text_font(&g_styles[i], font);
            return &g_styles[i];
        }
    }
    spdlog::critical("[ui] FATAL: shared font style table full ({} faces)", MAX_FACES);
    std::exit(EXIT_FAILURE);
}

void apply_font_style(lv_obj_t* obj, const lv_font_t* font) {
    if (!obj || !font)
        return;

    // Drop whichever face was applied before, then add the one wanted. A
    // widget that resolves its face more than once must REPLACE the style
    // rather than stack a second one, and the face it already carries cannot
    // be recomputed from its size rung: a breakpoint change re-points the
    // icon_font_* tokens, so the rung names a different face than the one the
    // object is carrying.
    // Removing a style the object does not carry is a no-op, and the loop
    // stops at the first unused slot.
    for (size_t i = 0; i < MAX_FACES && g_faces[i] != nullptr; ++i) {
        lv_obj_remove_style(obj, &g_styles[i], LV_PART_MAIN);
    }
    lv_obj_add_style(obj, shared_font_style(font), LV_PART_MAIN);
}

} // namespace helix::ui
