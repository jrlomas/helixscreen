// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "screensaver_pixel_writer.h"

#include <cstdint>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::FrameTarget;
using helix::ui::PixelFormat;
using helix::ui::PixelWriter;
using helix::ui::Rgb;

namespace {

constexpr uint8_t UNTOUCHED = 0xAA;

/// A heap frame whose every byte starts as UNTOUCHED, so a write outside its pixel shows.
struct TestFrame {
    std::vector<uint8_t> bytes;
    FrameTarget target;

    TestFrame(uint32_t w, uint32_t h, uint32_t stride, PixelFormat format)
        : bytes(static_cast<size_t>(stride) * h, UNTOUCHED) {
        target = {bytes.data(), stride, w, h, format};
    }
    TestFrame(const TestFrame&) = delete;
    TestFrame& operator=(const TestFrame&) = delete;
};

} // namespace

TEST_CASE("PixelWriter writes XRGB8888 as B, G, R and an opaque X byte",
          "[screensaver][pixel_writer]") {
    TestFrame frame(4, 3, 4 * 4 + 8, PixelFormat::XRGB8888);
    PixelWriter writer(frame.target);

    writer.put(3, 2, Rgb{0x12, 0x34, 0x56});

    const size_t at = 2 * static_cast<size_t>(frame.target.stride) + 3 * 4;
    CHECK(frame.bytes[at] == 0x56);
    CHECK(frame.bytes[at + 1] == 0x34);
    CHECK(frame.bytes[at + 2] == 0x12);
    CHECK(frame.bytes[at + 3] == 0xFF);
    // Rows step by the stride, so the padding after the row's last pixel is left alone.
    CHECK(frame.bytes[at + 4] == UNTOUCHED);
    CHECK(frame.bytes[at - 1] == UNTOUCHED);
    CHECK(writer.get(3, 2) == Rgb{0x12, 0x34, 0x56});
}

TEST_CASE("PixelWriter fill covers every XRGB8888 pixel and no row padding",
          "[screensaver][pixel_writer]") {
    constexpr uint32_t W = 5;
    constexpr uint32_t H = 4;
    constexpr uint32_t STRIDE = W * 4 + 12;
    TestFrame frame(W, H, STRIDE, PixelFormat::XRGB8888);

    PixelWriter(frame.target).fill(Rgb{1, 2, 3});

    size_t wrong_pixels = 0;
    size_t touched_padding = 0;
    for (uint32_t y = 0; y < H; y++) {
        const uint8_t* row = frame.bytes.data() + static_cast<size_t>(y) * STRIDE;
        for (uint32_t x = 0; x < W; x++) {
            const uint8_t* px = row + x * 4;
            wrong_pixels += (px[0] != 3 || px[1] != 2 || px[2] != 1 || px[3] != 0xFF) ? 1 : 0;
        }
        for (uint32_t i = W * 4; i < STRIDE; i++) {
            touched_padding += row[i] != UNTOUCHED ? 1 : 0;
        }
    }
    CHECK(wrong_pixels == 0);
    CHECK(touched_padding == 0);
}

TEST_CASE("PixelWriter knows which points lie inside the frame", "[screensaver][pixel_writer]") {
    TestFrame frame(4, 3, 16, PixelFormat::XRGB8888);
    const PixelWriter writer(frame.target);
    CHECK(writer.contains(0, 0));
    CHECK(writer.contains(3, 2));
    CHECK_FALSE(writer.contains(4, 0));
    CHECK_FALSE(writer.contains(0, 3));
    CHECK_FALSE(writer.contains(-1, 0));
    CHECK_FALSE(writer.contains(0, -1));
}
