// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_settings_motion.h"

#include "ui_component_keypad.h"
#include "ui_event_safety.h"
#include "ui_nav_manager.h"
#include "ui_toast_manager.h"

#include "app_globals.h"
#include "i_moonraker_api.h"
#include "jog_coalescer.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "settings_manager.h"
#include "static_panel_registry.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace helix::settings {

// ============================================================================
// ROW DESCRIPTORS
// ============================================================================

namespace {

/// Settings store mm/min because that is what the motion API takes; the UI
/// speaks mm/s to match Extrude Speed and the web interfaces.
int mm_min_to_mm_s(int mm_per_min) {
    return mm_per_min / 60;
}

int mm_s_to_mm_min(int mm_per_sec) {
    return mm_per_sec * 60;
}

/// One row per control. `title` is both the keypad header and the row's
/// translation_tag in motion_settings_overlay.xml, so it resolves with no
/// extra keys. Indexed by MotionSettingsOverlay::Field.
struct FieldSpec {
    const char* title; ///< Keypad header and row label
    bool is_speed;     ///< mm/s slider row (true) or mm distance row (false)
    bool is_z;         ///< speeds only: Z axis instead of XY
    JogMode mode;      ///< distances only
    bool outer;        ///< distances only
};

constexpr FieldSpec FIELD_SPECS[] = {
    {"Jog Speed XY", true, false, {}, false},
    {"Jog Speed Z", true, true, {}, false},
    {"Fine Inner", false, false, JogMode::Fine, false},
    {"Fine Outer", false, false, JogMode::Fine, true},
    {"Coarse Inner", false, false, JogMode::Coarse, false},
    {"Coarse Outer", false, false, JogMode::Coarse, true},
    {"Turbo Inner", false, false, JogMode::Turbo, false},
    {"Turbo Outer", false, false, JogMode::Turbo, true},
};

constexpr size_t FIELD_COUNT = sizeof(FIELD_SPECS) / sizeof(FIELD_SPECS[0]);
static_assert(FIELD_COUNT == static_cast<size_t>(Field::Count),
              "FIELD_SPECS must have one entry per Field");

/// The coupled keypad bounds pair each distance row with FIELD_SPECS[i ^ 1];
/// that only works while the distance rows start at an even index and each
/// adjacent pair is the same mode with opposite rings. A misordered insert
/// must fail here, not resolve silently to the row's own value.
constexpr bool distance_pairs_alternate() {
    for (size_t i = static_cast<size_t>(Field::FineInner); i < FIELD_COUNT; ++i) {
        if (FIELD_SPECS[i].mode != FIELD_SPECS[i ^ 1].mode) {
            return false;
        }
        if (FIELD_SPECS[i].outer == FIELD_SPECS[i ^ 1].outer) {
            return false;
        }
    }
    return true;
}
static_assert(static_cast<int>(Field::FineInner) % 2 == 0,
              "distance rows must start at an even Field index");
static_assert(distance_pairs_alternate(),
              "distance rows must pair same-mode inner/outer at i, i^1");

/// XML value_subject names, in Field order.
constexpr const char* const SUBJECT_NAMES[FIELD_COUNT] = {
    "jog_speed_xy_display", "jog_speed_z_display",  "fine_inner_display",  "fine_outer_display",
    "coarse_inner_display", "coarse_outer_display", "turbo_inner_display", "turbo_outer_display",
};

bool field_in_range(int raw) {
    return raw >= 0 && raw < static_cast<int>(Field::Count);
}

} // namespace

// ============================================================================
// GLOBAL INSTANCE
// ============================================================================

static std::unique_ptr<MotionSettingsOverlay> g_motion_settings_overlay;

MotionSettingsOverlay& get_motion_settings_overlay() {
    if (!g_motion_settings_overlay) {
        g_motion_settings_overlay = std::make_unique<MotionSettingsOverlay>();
        StaticPanelRegistry::instance().register_destroy(
            "MotionSettingsOverlay", []() { g_motion_settings_overlay.reset(); });
    }
    return *g_motion_settings_overlay;
}

void show_motion_settings_overlay() {
    auto& overlay = get_motion_settings_overlay();
    overlay.set_api(get_moonraker_api());
    overlay.show(lv_screen_active());
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

MotionSettingsOverlay::MotionSettingsOverlay() {
    spdlog::trace("[{}] Constructor", get_name());
}

MotionSettingsOverlay::~MotionSettingsOverlay() {
    // persist_timer_ cancels itself as a member; a still-pending write dies
    // with the overlay, which on_deactivating already flushed on the way out.
    deinit_subjects();
}

void MotionSettingsOverlay::set_api(IMoonrakerAPI* api) {
    api_ = api;
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void MotionSettingsOverlay::init_subjects() {
    init_subjects_guarded([this]() {
        for (size_t i = 0; i < FIELD_COUNT; ++i) {
            format_display(i);
            UI_MANAGED_SUBJECT_STRING(display_subjects_[i], display_buffers_[i],
                                      display_buffers_[i], SUBJECT_NAMES[i], subjects_);
        }
        UI_MANAGED_SUBJECT_STRING(jog_speed_max_subject_, jog_speed_max_buf_, "500",
                                  "jog_speed_max_display", subjects_);
    });
}

void MotionSettingsOverlay::register_callbacks() {
    lv_xml_register_event_cb(nullptr, "on_jog_speed_xy_changed", on_jog_speed_xy_changed);
    lv_xml_register_event_cb(nullptr, "on_jog_speed_z_changed", on_jog_speed_z_changed);
    lv_xml_register_event_cb(nullptr, "on_motion_field_clicked", on_field_clicked);
    lv_xml_register_event_cb(nullptr, "on_reset_distances", on_reset_distances);

    spdlog::debug("[{}] Callbacks registered", get_name());
}

void MotionSettingsOverlay::deinit_subjects() {
    deinit_subjects_base(subjects_);
}

// ============================================================================
// UI CREATION
// ============================================================================

lv_obj_t* MotionSettingsOverlay::create(lv_obj_t* parent) {
    if (!parent) {
        spdlog::error("[{}] NULL parent", get_name());
        return nullptr;
    }

    overlay_root_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent, "motion_settings_overlay", nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);
    spdlog::info("[{}] Overlay created", get_name());

    return overlay_root_;
}

void MotionSettingsOverlay::show(lv_obj_t* parent_screen) {
    spdlog::debug("[{}] show() called", get_name());

    if (!subjects_initialized_) {
        init_subjects();
        register_callbacks();
    }

    if (!overlay_root_ && parent_screen) {
        create(parent_screen);
    }

    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay", get_name());
        ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Failed to load overlay"), 2000);
        return;
    }

    NavigationManager::instance().register_overlay_instance(overlay_root_, this);
    NavigationManager::instance().push_overlay(overlay_root_);
}

// ============================================================================
// LIFECYCLE HOOKS
// ============================================================================

void MotionSettingsOverlay::on_activate() {
    OverlayBase::on_activate();

    spdlog::debug("[{}] on_activate()", get_name());

    // Returning from our own keypad is not a fresh entry. The keypad confirm
    // updates the display and schedules the debounced persist, so re-reading
    // SettingsManager here would put the pre-persist value back on screen.
    if (returning_from_keypad_) {
        returning_from_keypad_ = false;
        spdlog::debug("[{}] Keeping typed value: returned from keypad", get_name());
        return;
    }

    refresh_displays();
    refresh_sliders();
}

void MotionSettingsOverlay::on_deactivating(DeactivateReason) {
    spdlog::debug("[{}] on_deactivating()", get_name());

    // Flush a pending debounced write before leaving. Cancelling alone would
    // drop the user's last drag: the timer cancels, it does not run.
    if (persist_timer_.pending()) {
        persist_timer_.cancel();
        persist_pending_speed();
    }
}

// ============================================================================
// DISPLAY REFRESH
// ============================================================================

void MotionSettingsOverlay::format_display(size_t i) {
    const FieldSpec& spec = FIELD_SPECS[i];
    auto& settings = SettingsManager::instance();

    if (spec.is_speed) {
        const int mm_min = spec.is_z ? settings.get_jog_speed_z() : settings.get_jog_speed_xy();
        std::snprintf(display_buffers_[i], sizeof(display_buffers_[i]), "%d mm/s",
                      mm_min_to_mm_s(effective_mm_min(mm_min)));
    } else {
        // %g trims trailing zeros, so the defaults read 0.1, 1, 10 and 50.
        std::snprintf(display_buffers_[i], sizeof(display_buffers_[i]), "%g mm",
                      static_cast<double>(settings.get_jog_distance(spec.mode, spec.outer)));
    }
}

void MotionSettingsOverlay::refresh_displays() {
    for (size_t i = 0; i < FIELD_COUNT; ++i) {
        format_display(i);
        lv_subject_copy_string(&display_subjects_[i], display_buffers_[i]);
    }
}

int MotionSettingsOverlay::effective_mm_min(int stored_mm_min) const {
    if (api_) {
        // The same bounds is_safe_feedrate() enforces at emission, so what the
        // field shows is what the move uses.
        const SafetyLimits& limits = api_->get_safety_limits();
        return helix::effective_jog_speed_mm_min(stored_mm_min, limits.min_feedrate_mm_min,
                                                 limits.max_feedrate_mm_min);
    }
    // Settings clamp range for callers with no API to ask: stored values
    // already sit inside it, so this is a passthrough in practice.
    return helix::effective_jog_speed_mm_min(stored_mm_min, 0.0, 60000.0);
}

int MotionSettingsOverlay::max_jog_mm_s() const {
    if (api_) {
        // IMoonrakerAPI::get_safety_limits() is the same source is_safe_feedrate()
        // checks against, so the slider cannot offer a speed the move would reject.
        const SafetyLimits& limits = api_->get_safety_limits();
        return std::max(1, static_cast<int>(limits.max_feedrate_mm_min / 60.0));
    }
    // Settings clamp ceiling, for callers with no API to ask.
    return mm_min_to_mm_s(60000);
}

lv_obj_t* MotionSettingsOverlay::speed_slider(bool is_z) const {
    if (!overlay_root_) {
        return nullptr;
    }
    return lv_obj_find_by_name(overlay_root_, is_z ? "jog_speed_z_slider" : "jog_speed_xy_slider");
}

void MotionSettingsOverlay::refresh_sliders() {
    if (!overlay_root_) {
        return;
    }

    auto& settings = SettingsManager::instance();
    const int max_mm_s = max_jog_mm_s();
    const struct {
        const char* slider;
        int mm_min;
    } rows[] = {
        {"jog_speed_xy_slider", settings.get_jog_speed_xy()},
        {"jog_speed_z_slider", settings.get_jog_speed_z()},
    };

    std::snprintf(jog_speed_max_buf_, sizeof(jog_speed_max_buf_), "%d", max_mm_s);
    lv_subject_copy_string(&jog_speed_max_subject_, jog_speed_max_buf_);

    for (const auto& row : rows) {
        lv_obj_t* slider = lv_obj_find_by_name(overlay_root_, row.slider);
        if (slider) {
            lv_slider_set_range(slider, 1, max_mm_s);
            // The effective value, not the stored one: LVGL clamps the slider
            // to this range anyway, and the field must agree with it.
            lv_slider_set_value(slider, mm_min_to_mm_s(effective_mm_min(row.mm_min)), LV_ANIM_OFF);
        }
    }
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================

void MotionSettingsOverlay::handle_jog_speed_changed(bool is_z, int mm_s) {
    const size_t i =
        is_z ? static_cast<size_t>(Field::JogSpeedZ) : static_cast<size_t>(Field::JogSpeedXY);

    pending_mm_min_[is_z ? 1 : 0] = mm_s_to_mm_min(mm_s);
    // Trailing-edge debounce: a drag re-requests per pixel, and the write
    // fires once, 250ms after the burst stops, with the latest value.
    persist_timer_.schedule([this]() { persist_pending_speed(); });

    std::snprintf(display_buffers_[i], sizeof(display_buffers_[i]), "%d mm/s", mm_s);
    lv_subject_copy_string(&display_subjects_[i], display_buffers_[i]);
}

void MotionSettingsOverlay::persist_pending_speed() {
    auto& settings = SettingsManager::instance();
    if (pending_mm_min_[0] != 0) {
        settings.set_jog_speed_xy(pending_mm_min_[0]);
        pending_mm_min_[0] = 0;
    }
    if (pending_mm_min_[1] != 0) {
        settings.set_jog_speed_z(pending_mm_min_[1]);
        pending_mm_min_[1] = 0;
    }
}

void MotionSettingsOverlay::handle_field_clicked(Field field) {
    const size_t i = static_cast<size_t>(field);
    const FieldSpec& spec = FIELD_SPECS[i];
    auto& settings = SettingsManager::instance();
    pending_keypad_field_ = field;

    ui_keypad_config_t config = {};
    if (spec.is_speed) {
        lv_obj_t* slider = speed_slider(spec.is_z);
        if (!slider) {
            spdlog::warn("[{}] No slider for field {}", get_name(), static_cast<int>(field));
            return;
        }
        // Bounds come from the row's slider, whose range was derived from the
        // printer's reported feedrate limit, so the two cannot disagree.
        const KeypadBounds bounds =
            keypad_bounds(field, static_cast<float>(lv_slider_get_min_value(slider)),
                          static_cast<float>(lv_slider_get_max_value(slider)));
        config.min_value = bounds.min;
        config.max_value = bounds.max;
        config.initial_value = static_cast<float>(mm_min_to_mm_s(effective_mm_min(
            spec.is_z ? settings.get_jog_speed_z() : settings.get_jog_speed_xy())));
        config.allow_decimal = false;
        config.unit_label = "mm/s";
    } else {
        const KeypadBounds bounds =
            keypad_bounds(field, settings.get_jog_distance(spec.mode, false),
                          settings.get_jog_distance(spec.mode, true));
        config.initial_value = settings.get_jog_distance(spec.mode, spec.outer);
        config.min_value = bounds.min;
        config.max_value = bounds.max;
        config.allow_decimal = true;
        config.unit_label = "mm";
    }
    config.title_label = lv_tr(spec.title);
    config.allow_negative = false;
    config.callback = on_keypad_value;
    config.user_data = this;

    spdlog::debug("[{}] Keypad for {} ({}-{})", get_name(), spec.title, config.min_value,
                  config.max_value);
    ui_keypad_show(&config);
}

void MotionSettingsOverlay::handle_keypad_value(Field field, double value) {
    // Set on confirm, not when the keypad opens: the keypad invokes this
    // callback before it hides, so the flag is always consumed by the
    // on_activate() that follows.
    returning_from_keypad_ = true;

    const FieldSpec& spec = FIELD_SPECS[static_cast<size_t>(field)];
    if (spec.is_speed) {
        lv_obj_t* slider = speed_slider(spec.is_z);
        if (!slider) {
            spdlog::warn("[{}] No slider for field {}; dropping typed value {}", get_name(),
                         static_cast<int>(field), value);
            return;
        }
        const int mm_s = static_cast<int>(value);
        lv_slider_set_value(slider, mm_s, LV_ANIM_OFF);
        // Same path a drag takes: relabel, then persist. One code path for both.
        handle_jog_speed_changed(spec.is_z, mm_s);
    } else {
        SettingsManager::instance().set_jog_distance(spec.mode, spec.outer,
                                                     static_cast<float>(value));
        const size_t i = static_cast<size_t>(field);
        format_display(i);
        lv_subject_copy_string(&display_subjects_[i], display_buffers_[i]);
    }
}

void MotionSettingsOverlay::handle_reset_distances() {
    spdlog::info("[{}] Resetting jog distances to defaults", get_name());
    SettingsManager::instance().reset_jog_distances();
    refresh_displays();
}

// ============================================================================
// STATIC CALLBACKS
// ============================================================================

void MotionSettingsOverlay::on_field_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MotionSettingsOverlay] on_field_clicked");
    const char* index_str = static_cast<const char*>(lv_event_get_user_data(e));
    if (index_str) {
        const int raw = static_cast<int>(std::strtol(index_str, nullptr, 10));
        if (field_in_range(raw)) {
            get_motion_settings_overlay().handle_field_clicked(static_cast<Field>(raw));
        } else {
            spdlog::warn("[MotionSettingsOverlay] Ignoring out-of-range field index {}", raw);
        }
    }
    LVGL_SAFE_EVENT_CB_END();
}

void MotionSettingsOverlay::on_keypad_value(float value, void* user_data) {
    auto* self = static_cast<MotionSettingsOverlay*>(user_data);
    if (!self) {
        return;
    }
    self->handle_keypad_value(self->pending_keypad_field_, static_cast<double>(value));
}

void MotionSettingsOverlay::on_jog_speed_xy_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MotionSettingsOverlay] on_jog_speed_xy_changed");
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    get_motion_settings_overlay().handle_jog_speed_changed(/*is_z=*/false,
                                                           lv_slider_get_value(slider));
    LVGL_SAFE_EVENT_CB_END();
}

void MotionSettingsOverlay::on_jog_speed_z_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MotionSettingsOverlay] on_jog_speed_z_changed");
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    get_motion_settings_overlay().handle_jog_speed_changed(/*is_z=*/true,
                                                           lv_slider_get_value(slider));
    LVGL_SAFE_EVENT_CB_END();
}

void MotionSettingsOverlay::on_reset_distances(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MotionSettingsOverlay] on_reset_distances");
    get_motion_settings_overlay().handle_reset_distances();
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix::settings
