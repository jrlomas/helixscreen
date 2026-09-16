// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_base.h"
#include "screensaver_starfield_sim.h"

#include <cstdint>
#include <lvgl.h>
#include <optional>
#include <vector>

/**
 * @brief Windows 95-style Starfield screensaver
 *
 * Stars fly outward from the center of the screen. Each star starts small
 * and dim near the center, growing larger and brighter as it approaches
 * the edges.
 *
 * helix::ui::StarfieldSim draws each frame with direct pixel writes, erasing only where
 * stars were, and moves the stars by the time since the previous frame; the frame returns
 * only the part of the canvas the step changed.
 */
class StarfieldScreensaver : public helix::ui::SaverBase {
  public:
    ScreensaverType type() const override {
        return ScreensaverType::STARFIELD;
    }

  protected:
    std::optional<lv_color_format_t> canvas_format() const override {
        return helix::ui::SAVER_BUILD_CANVAS_FORMAT;
    }
    bool on_start() override;
    void on_frame(uint32_t dt_ms, std::vector<helix::ui::DirtyRect>& dirty) override;
    void on_stop() override;
    size_t ladder_size() const override {
        return helix::ui::TWO_LEVEL_COUNT;
    }
    uint32_t ladder_period_ms(size_t level) const override {
        return helix::ui::two_level_period_ms(level);
    }

  private:
    // Test-only seam: reads and places the stars. See tests/test_helpers/screensaver_test_access.h.
    friend class StarfieldScreensaverTestAccess;

    helix::ui::StarfieldSim sim_;
};

#endif // HELIX_ENABLE_SCREENSAVER
