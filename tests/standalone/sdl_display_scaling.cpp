// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// This process owns LVGL and SDL outright: no application, printer connection,
// shared Catch2 display, or UpdateQueue is involved. The shell test links the
// production drivers and existing LVGL objects, and runs only dummy devices.

#include "lvgl/lvgl.h"
#include "lvgl/src/drivers/sdl/lv_sdl_private.h"
#include "lvgl_assert_handler.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

extern "C" {
helix_assert_callback_t g_helix_assert_cpp_callback = nullptr;
void helix_notify_app_backgrounded(void) {}
void helix_notify_app_foregrounded(void) {}
void helix_lvgl_anomaly(const char* code, const char* context) {
    spdlog::error("Unexpected LVGL anomaly: {} ({})", code, context);
    std::abort();
}
}

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void pump() {
    for (int i = 0; i < 3; ++i) {
        SDL_Delay(6);
        lv_timer_handler();
    }
}

void send(SDL_Event event) {
    require(SDL_PushEvent(&event) == 1, "SDL event enqueue failed");
    pump();
}

void check_pointer(lv_indev_t* mouse, int x, int y, lv_indev_state_t state, int tolerance = 0) {
    lv_point_t point{};
    lv_indev_get_point(mouse, &point);
    require(std::abs(point.x - x) <= tolerance && std::abs(point.y - y) <= tolerance,
            "pointer coordinates do not match the logical canvas");
    require(lv_indev_get_state(mouse) == state, "pointer pressed/released state is incorrect");
}

void send_mouse(SDL_Window* window, Uint32 type, int x, int y) {
    SDL_Event event{};
    event.type = type;
    if (type == SDL_MOUSEMOTION) {
        event.motion.windowID = SDL_GetWindowID(window);
        event.motion.x = x;
        event.motion.y = y;
    } else {
        event.button.windowID = SDL_GetWindowID(window);
        event.button.button = SDL_BUTTON_LEFT;
        event.button.x = x;
        event.button.y = y;
    }
    send(event);
}

void send_touch(SDL_Window* window, Uint32 type, float x, float y) {
    SDL_Event event{};
    event.type = type;
    event.tfinger.windowID = SDL_GetWindowID(window);
    event.tfinger.touchId = 1;
    event.tfinger.fingerId = 1;
    event.tfinger.x = x;
    event.tfinger.y = y;
    send(event);
}

void resize(lv_display_t* display, SDL_Window* window, int width, int height, float zoom) {
    const int scaled_width = static_cast<int>(std::lround(width * zoom));
    const int scaled_height = static_cast<int>(std::lround(height * zoom));
    SDL_SetWindowSize(window, scaled_width, scaled_height);
    SDL_Event event{};
    event.type = SDL_WINDOWEVENT;
    event.window.windowID = SDL_GetWindowID(window);
    event.window.event = SDL_WINDOWEVENT_RESIZED;
    event.window.data1 = scaled_width;
    event.window.data2 = scaled_height;
    send(event);

    require(lv_display_get_horizontal_resolution(display) == width &&
                lv_display_get_vertical_resolution(display) == height,
            "resize changed the requested logical resolution");
    int actual_width = 0, actual_height = 0;
    SDL_GetWindowSize(window, &actual_width, &actual_height);
    require(actual_width == scaled_width && actual_height == scaled_height,
            "decoration compensation reverted the scaled window size");
}

void check_decoration_compensation(lv_display_t* display, SDL_Window* window, int width, int height,
                                   float zoom) {
    const int scaled_width = static_cast<int>(std::lround(width * zoom));
    const int scaled_height = static_cast<int>(std::lround(height * zoom));
    // The dummy driver has no decorations; explicitly deliver the compositor's
    // initial short-content-area event before the target size settles.
    SDL_SetWindowSize(window, scaled_width, scaled_height - 36);
    SDL_Event event{};
    event.type = SDL_WINDOWEVENT;
    event.window.windowID = SDL_GetWindowID(window);
    event.window.event = SDL_WINDOWEVENT_RESIZED;
    event.window.data1 = scaled_width;
    event.window.data2 = scaled_height - 36;
    send(event);
    require(lv_display_get_horizontal_resolution(display) == width &&
                lv_display_get_vertical_resolution(display) == height,
            "decoration compensation changed the logical canvas");
    int actual_width = 0, actual_height = 0;
    SDL_GetWindowSize(window, &actual_width, &actual_height);
    require(actual_width == scaled_width && actual_height == scaled_height,
            "decoration compensation did not restore the scaled target");
}

void check_input(lv_display_t* display, SDL_Window* window, lv_indev_t* mouse, float zoom) {
    const int width = lv_display_get_horizontal_resolution(display);
    const int height = lv_display_get_vertical_resolution(display);
    int window_width = 0, window_height = 0;
    SDL_GetWindowSize(window, &window_width, &window_height);
    for (const auto& p : std::array<lv_point_t, 5>{{{0, 0},
                                                    {width - 1, 0},
                                                    {0, height - 1},
                                                    {width - 1, height - 1},
                                                    {width / 2, height / 2}}}) {
        // Window pixels are discrete; fractional scaling can move the inverse
        // coordinate by less than one logical pixel.
        const int x = p.x == width - 1 ? window_width - 1 : static_cast<int>(p.x * zoom);
        const int y = p.y == height - 1 ? window_height - 1 : static_cast<int>(p.y * zoom);
        send_mouse(window, SDL_MOUSEMOTION, x, y);
        check_pointer(mouse, p.x, p.y, LV_INDEV_STATE_RELEASED, 1);
    }

    int clicked = 0;
    lv_obj_t* button = lv_button_create(lv_display_get_screen_active(display));
    lv_obj_set_pos(button, 100, 100);
    lv_obj_set_size(button, 100, 70);
    lv_obj_add_event_cb(
        button, [](lv_event_t* event) { ++*static_cast<int*>(lv_event_get_user_data(event)); },
        LV_EVENT_CLICKED, &clicked);
    lv_obj_update_layout(button);
    send_mouse(window, SDL_MOUSEBUTTONDOWN, static_cast<int>(120 * zoom),
               static_cast<int>(120 * zoom));
    check_pointer(mouse, 120, 120, LV_INDEV_STATE_PRESSED);
    send_mouse(window, SDL_MOUSEBUTTONUP, static_cast<int>(120 * zoom),
               static_cast<int>(120 * zoom));
    require(clicked == 1, "scaled pointer did not click the visible button");

    send_mouse(window, SDL_MOUSEBUTTONDOWN, static_cast<int>(240 * zoom),
               static_cast<int>(200 * zoom));
    check_pointer(mouse, 240, 200, LV_INDEV_STATE_PRESSED);
    send_mouse(window, SDL_MOUSEBUTTONUP, static_cast<int>(40 * zoom), static_cast<int>(60 * zoom));
    check_pointer(mouse, 40, 60, LV_INDEV_STATE_RELEASED);

    send_touch(window, SDL_FINGERDOWN, 0.5f, 0.5f);
    check_pointer(mouse, width / 2, height / 2, LV_INDEV_STATE_PRESSED);
    send_touch(window, SDL_FINGERMOTION, 0.75f, 0.25f);
    check_pointer(mouse, static_cast<int>(width * 0.75f), static_cast<int>(height * 0.25f),
                  LV_INDEV_STATE_PRESSED);
    send_touch(window, SDL_FINGERUP, 0.25f, 0.75f);
    check_pointer(mouse, static_cast<int>(width * 0.25f), static_cast<int>(height * 0.75f),
                  LV_INDEV_STATE_RELEASED);
    lv_obj_delete(button);
}
} // namespace

int main() {
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    SDL_SetHintWithPriority(SDL_HINT_RENDER_DRIVER, "software", SDL_HINT_OVERRIDE);
    lv_init();
    try {
        for (float zoom : {1.0f, 1.25f, 1.5f, 1.75f, 2.0f}) {
            for (const auto& size : std::array<lv_point_t, 2>{{{1024, 600}, {801, 481}}}) {
                spdlog::info("Checking SDL zoom {} at {}x{}", zoom, size.x, size.y);
                lv_display_t* display = lv_sdl_window_create(size.x, size.y);
                require(display != nullptr, "SDL display creation failed");
                lv_display_set_default(display);
                SDL_Window* window = lv_sdl_window_get_window(display);
                require((SDL_GetWindowFlags(window) & SDL_WINDOW_ALLOW_HIGHDPI) != 0,
                        "desktop window does not allow high-DPI drawables");
                lv_indev_t* mouse = lv_sdl_mouse_create();
                require(mouse != nullptr, "SDL pointer creation failed");
                lv_indev_set_display(mouse, display);
                lv_sdl_window_set_zoom(display, zoom);
                int actual_width = 0, actual_height = 0;
                SDL_GetWindowSize(window, &actual_width, &actual_height);
                require(actual_width == std::lround(size.x * zoom) &&
                            actual_height == std::lround(size.y * zoom),
                        "initial window zoom rounded incorrectly");
                check_decoration_compensation(display, window, size.x, size.y, zoom);
                resize(display, window, size.x, size.y, zoom);
                check_input(display, window, mouse, zoom);
                resize(display, window, size.x - 40, size.y - 40, zoom);
                check_input(display, window, mouse, zoom);
                resize(display, window, size.x + 20, size.y + 20, zoom);
                check_input(display, window, mouse, zoom);
                lv_indev_delete(mouse);
                lv_display_delete(display);
            }
        }
        lv_sdl_quit();
        lv_deinit();
        spdlog::info("SDL fractional scaling, resizing, clicks, and touch passed");
        return 0;
    } catch (const std::exception& e) {
        spdlog::error("SDL scaling regression: {}", e.what());
        return 1;
    }
}
