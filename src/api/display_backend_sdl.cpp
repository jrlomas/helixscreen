// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// HelixScreen - SDL Display Backend Implementation

#ifdef HELIX_DISPLAY_SDL

#include "display_backend_sdl.h"

#include "sdl_display_scale.h"

#include <spdlog/spdlog.h>

// LVGL SDL driver
#include "lvgl/src/drivers/sdl/lv_sdl_window.h"

#include <lvgl.h>

// SDL2 headers
#include <SDL.h>
#ifndef __ANDROID__
#include <SDL_syswm.h>
#endif
#include <string_view>

#ifndef __ANDROID__
namespace {

std::string_view scale_env(const char* name) {
    const char* value = SDL_getenv(name);
    return value ? value : "";
}

std::optional<double> x11_desktop_scale(SDL_Window* window) {
#if defined(SDL_VIDEO_DRIVER_X11)
    SDL_SysWMinfo info{};
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_X11) {
        return std::nullopt;
    }

    // SDL loads X11 dynamically too. Keep it optional so the same binary can
    // run under Wayland or the headless driver without a new link dependency.
    void* x11 = SDL_LoadObject("libX11.so.6");
    if (!x11) {
        return std::nullopt;
    }
    using ResourceString = char* (*)(Display*);
    auto resource_string =
        reinterpret_cast<ResourceString>(SDL_LoadFunction(x11, "XResourceManagerString"));
    std::optional<double> scale;
    if (resource_string) {
        if (const char* resources = resource_string(info.info.x11.display)) {
            scale = helix::sdl::xft_scale(resources);
        }
    }
    SDL_UnloadObject(x11);
    return scale;
#else
    (void)window;
    return std::nullopt;
#endif
}

} // namespace
#endif

bool DisplayBackendSDL::is_available() const {
    // SDL is always "available" on desktop - actual initialization
    // happens in create_display() which can fail more gracefully
    return true;
}

lv_display_t* DisplayBackendSDL::create_display(int width, int height) {
    spdlog::debug("[SDL Backend] Creating SDL display: {}x{}", width, height);

    // Enable VSync to prevent tearing
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");

    // Prevent compositor bypass on X11 (no-op on other platforms)
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
    // LVGL's SDL driver handles SDL_Init and window creation internally
    display_ = lv_sdl_window_create(width, height);

    if (display_ == nullptr) {
        // Two very different failures land here, and only one is worth a retry.
        //
        // No video subsystem at all (bad SDL_VIDEODRIVER, no display server and
        // no fallback driver): SDL_CreateWindow failed. A different render
        // driver cannot help, and LVGL latches its `inited` flag on the first
        // call so SDL_Init is not even retried. Report the real reason instead
        // of blaming the renderer, and let DisplayManager move on to fbdev.
        //
        // Video subsystem up but no accelerated renderer: LVGL asks for
        // SDL_RENDERER_ACCELERATED (LV_SDL_ACCELERATED in lv_conf.h), which the
        // dummy/offscreen drivers, GPU-less containers and CI machines cannot
        // provide. That one retries cleanly with the software renderer, so
        // headless works without the caller knowing to set SDL_RENDER_DRIVER.
        if (SDL_WasInit(SDL_INIT_VIDEO) == 0) {
            spdlog::error("[SDL Backend] No usable SDL video driver: {}", SDL_GetError());
        } else {
            // OVERRIDE priority so it also beats a user-set SDL_RENDER_DRIVER
            // that just failed.
            const char* current = SDL_GetHint(SDL_HINT_RENDER_DRIVER);
            if (current == nullptr || std::string_view(current) != "software") {
                spdlog::warn("[SDL Backend] Accelerated renderer unavailable ({}) - "
                             "retrying with the software renderer",
                             SDL_GetError());
                SDL_SetHintWithPriority(SDL_HINT_RENDER_DRIVER, "software", SDL_HINT_OVERRIDE);
                display_ = lv_sdl_window_create(width, height);
                if (display_ != nullptr) {
                    spdlog::info("[SDL Backend] Using software renderer (no GPU acceleration)");
                }
            }
        }
    }

    if (display_ == nullptr) {
        spdlog::error("[SDL Backend] Failed to create SDL display");
        return nullptr;
    }

    // Raise window to foreground (macOS SDL windows start behind other windows)
    SDL_Window* window = lv_sdl_window_get_window(display_);
    if (window) {
#ifndef __ANDROID__
        const char* driver = SDL_GetCurrentVideoDriver();
        const auto override_scale = scale_env("HELIX_SDL_SCALE");
        if (!override_scale.empty() && !helix::sdl::parse_scale(override_scale)) {
            spdlog::warn("[SDL Backend] Ignoring invalid HELIX_SDL_SCALE '{}'; expected 1.0–4.0",
                         override_scale);
        }
        const auto scale =
            helix::sdl::resolve_desktop_scale(driver ? driver : "", override_scale,
                                              scale_env("GDK_SCALE"), x11_desktop_scale(window));
        if (scale.zoom != 1.0) {
            lv_sdl_window_set_zoom(display_, static_cast<float>(scale.zoom));
        }
        int window_width = 0, window_height = 0;
        SDL_GetWindowSize(window, &window_width, &window_height);
        spdlog::info(
            "[SDL Backend] Desktop scale {:.2f}x from {}: logical {}x{}, window {}x{} ({})",
            scale.zoom, scale.source, width, height, window_width, window_height,
            driver ? driver : "unknown");
#endif
        SDL_RaiseWindow(window);
    }

    spdlog::info("[SDL Backend] SDL display created: {}x{}", width, height);
    return display_;
}

lv_indev_t* DisplayBackendSDL::create_input_pointer() {
    if (display_ == nullptr) {
        spdlog::error("[SDL Backend] Cannot create input device without display");
        return nullptr;
    }

    mouse_ = lv_sdl_mouse_create();

    if (mouse_ == nullptr) {
        spdlog::error("[SDL Backend] Failed to create SDL mouse input");
        return nullptr;
    }

    spdlog::debug("[SDL Backend] SDL mouse input created");
    return mouse_;
}

lv_indev_t* DisplayBackendSDL::create_input_keyboard() {
    if (display_ == nullptr) {
        spdlog::error("[SDL Backend] Cannot create keyboard without display");
        return nullptr;
    }

    keyboard_ = lv_sdl_keyboard_create();

    if (keyboard_ == nullptr) {
        spdlog::warn("[SDL Backend] Failed to create SDL keyboard input");
        return nullptr;
    }

    spdlog::debug("[SDL Backend] SDL keyboard input created");
    return keyboard_;
}

#endif // HELIX_DISPLAY_SDL
