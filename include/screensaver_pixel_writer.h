// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "screensaver_frame.h"

#include <cstddef>
#include <cstdint>

namespace helix::ui {

/**
 * @brief Writes colours into a FrameTarget
 *
 * No LVGL: the frame's format is data, so the host test build exercises every format
 * whatever LV_COLOR_DEPTH the app is built with. Callers keep coordinates inside the frame,
 * which contains() answers.
 */
class PixelWriter {
  public:
    explicit PixelWriter(const FrameTarget& frame) : frame_(frame) {}

    bool contains(int32_t x, int32_t y) const {
        return x >= 0 && y >= 0 && x < static_cast<int32_t>(frame_.w) &&
               y < static_cast<int32_t>(frame_.h);
    }

    /// Writes `c` at (x, y), with the X byte at 0xFF.
    void put(int32_t x, int32_t y, Rgb c) {
        uint8_t* p = pixel(x, y);
        p[0] = c.b;
        p[1] = c.g;
        p[2] = c.r;
        p[3] = 0xFF;
    }

    /// The colour stored at (x, y).
    Rgb get(int32_t x, int32_t y) const {
        const uint8_t* p = pixel(x, y);
        return {p[2], p[1], p[0]};
    }

    /// Writes `c` over every pixel, leaving row padding alone.
    void fill(Rgb c) {
        for (int32_t y = 0; y < static_cast<int32_t>(frame_.h); y++) {
            for (int32_t x = 0; x < static_cast<int32_t>(frame_.w); x++) {
                put(x, y, c);
            }
        }
    }

  private:
    uint8_t* pixel(int32_t x, int32_t y) const {
        return frame_.data + static_cast<size_t>(y) * frame_.stride + static_cast<size_t>(x) * 4;
    }

    FrameTarget frame_;
};

} // namespace helix::ui
