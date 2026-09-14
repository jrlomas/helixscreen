// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace helix {
namespace bed_mesh {

/// Bed temperature a mesh is probed at when nothing else chooses one.
inline constexpr int DEFAULT_PROBE_BED_TEMP_C = 60;

/**
 * @brief The bed temperature, in whole degrees C, to probe a mesh at.
 *
 * Firmware heat-and-wait commands can wait for a bed to cool as well as to heat,
 * so the only temperature below the bed's own that this ever asks for is a
 * target the user set:
 * - a set target wins;
 * - otherwise a bed still above the default is probed where it is, floored so the
 *   wait is satisfied at once;
 * - otherwise the default.
 *
 * @param target_deci  Bed target in decidegrees, as PrinterState holds it.
 * @param current_deci Bed temperature in decidegrees.
 */
[[nodiscard]] inline constexpr int probe_bed_temp_c(int target_deci, int current_deci) {
    const int target_c = target_deci / 10;
    if (target_c > 0) {
        return target_c;
    }
    const int current_c = current_deci / 10;
    return current_c > DEFAULT_PROBE_BED_TEMP_C ? current_c : DEFAULT_PROBE_BED_TEMP_C;
}

} // namespace bed_mesh
} // namespace helix
