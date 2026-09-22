// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "error_event.h"

#include <optional>
#include <string>
#include <vector>

#include "hv/json.hpp"

namespace helix {
class PrinterDiscovery;
} // namespace helix

/**
 * @file firmware_fault_codes.h
 * @brief Firmwares that report faults as structured codes rather than prose.
 *
 * Some firmware raises faults with a numeric identity and embeds it in the
 * error text. Matching the sentence instead is brittle: the wording belongs to
 * the firmware, it changes between releases, and it is not translated.
 *
 * This is the one module that knows which firmwares do that and how to read
 * their codes. `GcodeErrorRouter` asks the question and never names a firmware.
 * Adding another is a row in the provider table.
 */
namespace helix::faultcodes {

/// True when this firmware reports structured fault codes.
[[nodiscard]] bool firmware_reports_fault_codes(const PrinterDiscovery& hw);

/// Status objects carrying standing faults, for the subscription builder.
[[nodiscard]] std::vector<std::string> required_status_objects(const PrinterDiscovery& hw);

/// Classify one error line. nullopt when the line carries no code this
/// firmware owns, so the generic classifier still gets its turn.
[[nodiscard]] std::optional<ErrorEvent> classify(const PrinterDiscovery& hw,
                                                 const std::string& line);

/// The faults currently standing, one console-equivalent coded line each
/// (`!! LLLL-IIII-XXXX-CCCC <firmware message>` in the same format classify
/// parses), so the consumer feeds them through the same path a console line
/// takes. nullopt when the frame says nothing about faults -- a Moonraker
/// delta frame omits unchanged objects, and that must NOT read as "all
/// cleared"; an empty vector is the firmware explicitly reporting none. Pure
/// and untranslated, so it is safe on any thread.
[[nodiscard]] std::optional<std::vector<std::string>>
read_standing_faults(const PrinterDiscovery& hw, const nlohmann::json& status);

/// The part of a status frame any fault-code firmware reads: only the status
/// objects some provider subscribes to, or an empty object when the frame
/// carries none. Needs no discovery, so the WS thread can use it to skip
/// the many frames that have nothing to do with faults.
[[nodiscard]] nlohmann::json fault_status_subset(const nlohmann::json& status);

} // namespace helix::faultcodes
