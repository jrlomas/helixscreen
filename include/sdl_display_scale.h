// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace helix::sdl {

inline std::string_view trim_scale_value(std::string_view value) {
    constexpr std::string_view whitespace = " \t\r\n";
    const auto first = value.find_first_not_of(whitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    return value.substr(first, value.find_last_not_of(whitespace) - first + 1);
}

inline std::optional<double> parse_scale(std::string_view value) {
    const std::string text(trim_scale_value(value));
    if (text.empty()) {
        return std::nullopt;
    }
    char* end = nullptr;
    const double scale = std::strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size() || !std::isfinite(scale) || scale < 1.0 || scale > 4.0) {
        return std::nullopt;
    }
    return scale;
}

// Xft.dpi describes desktop logical pixels (96 at 100%), not the monitor's
// physical DPI. X11 fractional scaling can render at 2x and downsample each
// output differently, so dividing by a monitor's physical DPI double-scales it.
inline std::optional<double> xft_scale(std::string_view resources) {
    while (!resources.empty()) {
        const auto newline = resources.find('\n');
        const auto line = resources.substr(0, newline);
        const auto colon = line.find(':');
        if (colon != std::string_view::npos &&
            trim_scale_value(line.substr(0, colon)) == "Xft.dpi") {
            const std::string text(trim_scale_value(line.substr(colon + 1)));
            char* end = nullptr;
            const double dpi = std::strtod(text.c_str(), &end);
            if (!text.empty() && end == text.c_str() + text.size() && std::isfinite(dpi) &&
                dpi >= 96.0 && dpi <= 384.0) {
                return dpi / 96.0;
            }
            return std::nullopt;
        }
        if (newline == std::string_view::npos) {
            break;
        }
        resources.remove_prefix(newline + 1);
    }
    return std::nullopt;
}

struct DesktopScale {
    double zoom;
    const char* source;
};

inline DesktopScale resolve_desktop_scale(std::string_view driver, std::string_view override_scale,
                                          std::string_view gdk_scale,
                                          std::optional<double> x11_scale) {
    if (const auto scale = parse_scale(override_scale)) {
        return {*scale, "HELIX_SDL_SCALE"};
    }
    // Wayland and Cocoa already size windows in desktop logical coordinates.
    // Only X11 needs an application-side logical-to-window conversion.
    if (driver == "x11") {
        if (const auto scale = parse_scale(gdk_scale)) {
            return {*scale, "GDK_SCALE"};
        }
        if (x11_scale && std::isfinite(*x11_scale) && *x11_scale >= 1.0 && *x11_scale <= 4.0) {
            return {*x11_scale, "Xft.dpi"};
        }
    }
    return {1.0, "native window coordinates"};
}

} // namespace helix::sdl
