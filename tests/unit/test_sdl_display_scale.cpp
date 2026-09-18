// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sdl_display_scale.h"

#include <limits>

#include "../catch_amalgamated.hpp"

using namespace helix::sdl;

TEST_CASE("Desktop scale preserves fractional values", "[display][sdl_scale]") {
    for (double scale : {1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 4.0}) {
        INFO("scale: " << scale);
        REQUIRE(parse_scale(std::to_string(scale)) == scale);
    }
    REQUIRE(parse_scale(" \t1.5\r\n") == 1.5);
}

TEST_CASE("Desktop scale rejects malformed and unsafe overrides", "[display][sdl_scale]") {
    for (const char* value : {"", " ", "auto", "0", "-1", "0.5", "0.99", "4.01", "150%", "1.5x",
                              "nan", "inf", "1e999", "1.5 2"}) {
        INFO("value: " << value);
        REQUIRE_FALSE(parse_scale(value));
    }
}

TEST_CASE("Xft desktop DPI converts from the 96 DPI reference", "[display][sdl_scale]") {
    REQUIRE(xft_scale("Xft.dpi: 96\n") == 1.0);
    REQUIRE(xft_scale("Xft.dpi: 120\n") == 1.25);
    REQUIRE(xft_scale("Xft.dpi: 144\n") == 1.5);
    REQUIRE(xft_scale("Xft.dpi: 168\n") == 1.75);
    REQUIRE(xft_scale("Xcursor.size:\t48\n Xft.dpi :\t192\r\nXft.antialias:\t1\n") == 2.0);
    REQUIRE(xft_scale("Xft.dpi: 144.0") == 1.5);
}

TEST_CASE("Missing or invalid Xft resources leave the desktop scale unspecified",
          "[display][sdl_scale]") {
    for (const char* resources : {"", "Xcursor.size: 48", "! Xft.dpi: 192\n", "Other.Xft.dpi: 192",
                                  "Xft.dpi: ", "Xft.dpi: nan", "Xft.dpi: 0", "Xft.dpi: 72",
                                  "Xft.dpi: 9999999", "Xft.dpi: 192junk", "Xft.dpi: 144 192"}) {
        INFO("resources: " << resources);
        REQUIRE_FALSE(xft_scale(resources));
    }
}

TEST_CASE("X11 follows logical desktop scale rather than monitor physical DPI",
          "[display][sdl_scale]") {
    const auto scale = resolve_desktop_scale("x11", "", "", xft_scale("Xft.dpi: 192"));
    REQUIRE(scale.zoom == 2.0);
    REQUIRE(std::string_view(scale.source) == "Xft.dpi");

    // One shared 5120-pixel desktop space per output; the compositor maps it
    // onto each monitor's native pixels. The app must not scale a second time
    // when it moves between these outputs.
    constexpr double logical_width = 800;
    const double window_width = logical_width * scale.zoom;
    REQUIRE(window_width * 2560 / 5120 == logical_width);
    REQUIRE(window_width * 3840 / 5120 == logical_width * 1.5);
}

TEST_CASE("Explicit desktop scale wins and invalid values fall back", "[display][sdl_scale]") {
    REQUIRE(resolve_desktop_scale("x11", "1.25", "2", 2.0).zoom == 1.25);
    REQUIRE(resolve_desktop_scale("x11", "invalid", "1.5", 2.0).zoom == 1.5);
    REQUIRE(resolve_desktop_scale("x11", "", "invalid", 1.75).zoom == 1.75);
    REQUIRE(resolve_desktop_scale("x11", "", "", std::nullopt).zoom == 1.0);
    REQUIRE(resolve_desktop_scale("x11", "", "", std::numeric_limits<double>::infinity()).zoom ==
            1.0);
    REQUIRE(resolve_desktop_scale("x11", "", "", -1.0).zoom == 1.0);
}

TEST_CASE("Native logical-coordinate drivers do not double-apply X11 or GTK scale",
          "[display][sdl_scale]") {
    for (const char* driver : {"wayland", "cocoa", "dummy", "offscreen", ""}) {
        INFO("driver: " << driver);
        REQUIRE(resolve_desktop_scale(driver, "", "2", 2.0).zoom == 1.0);
        REQUIRE(resolve_desktop_scale(driver, "1.5", "2", 2.0).zoom == 1.5);
    }
}
