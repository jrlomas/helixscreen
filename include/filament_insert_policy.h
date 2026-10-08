// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>

namespace helix {

/// Everything the "open the slot editor for a freshly inserted spool" decision
/// reads. Presence is tri-state because the machine may state nothing: a slot
/// whose status is UNKNOWN, or a sensor that has not reported yet, has no
/// reading, and no reading is never an "empty" half of a transition.
struct FilamentInsertContext {
    std::optional<bool> was_present; ///< Last stated presence (nullopt = no reading yet)
    std::optional<bool> is_present;  ///< Presence now (nullopt = no reading)
    bool print_active = false;       ///< PRINTING or PAUSED
    bool operation_busy = false;     ///< A load/unload/tool change is running or just finished
    bool setting_enabled = false;    ///< The user opted in
    bool editor_open = false;        ///< The slot editor is already on screen
};

/// True only for a stated-empty to stated-present edge while the user opted in
/// and nothing else owns the filament: the first reading after startup or a
/// reconnect has no prior reading, so a snapshot of an occupied slot is never
/// an insertion.
[[nodiscard]] constexpr bool should_open_editor_on_insert(const FilamentInsertContext& c) {
    return c.setting_enabled && c.was_present == false && c.is_present == true && !c.print_active &&
           !c.operation_busy && !c.editor_open;
}

} // namespace helix
