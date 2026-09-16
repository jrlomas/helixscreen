// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_starfield.h"

#include <spdlog/spdlog.h>

using helix::ui::DirtyRect;
using helix::ui::FrameTarget;
using helix::ui::StarfieldSim;

static_assert(LV_COLOR_DEPTH == 32, "the starfield canvas is XRGB8888 on a 32 bpp display");

bool StarfieldScreensaver::on_start() {
    spdlog::info("[Screensaver] Starting starfield");
    sim_.init(static_cast<uint32_t>(screen_w()), static_cast<uint32_t>(screen_h()), rng());
    spdlog::debug("[Screensaver] Starfield started ({}x{}, {} stars)", screen_w(), screen_h(),
                  StarfieldSim::NUM_STARS);
    return true;
}

void StarfieldScreensaver::on_frame(uint32_t dt_ms, std::vector<DirtyRect>& dirty) {
    FrameTarget target = canvas().frame();
    dirty.push_back(sim_.step(dt_ms, target, rng()));
}

void StarfieldScreensaver::on_stop() {
    sim_.stars().clear();
}

#endif // HELIX_ENABLE_SCREENSAVER
