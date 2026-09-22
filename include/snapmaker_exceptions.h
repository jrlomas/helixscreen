// include/snapmaker_exceptions.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <string>
#include <string_view>

/**
 * @file snapmaker_exceptions.h
 * @brief The U1's structured fault codes, and what they mean to a user.
 *
 * The firmware raises faults with a module id, an index, a code and a level,
 * and embeds them in error text as `level-id-index-code`. Matching the English
 * sentence instead is brittle: the wording is the firmware's to change, and it
 * is not translated. The code is the stable identity.
 *
 * Levels mirror what the firmware will do about it: 1 nothing, 2 pause,
 * 3 cancel.
 */
namespace helix::snapmaker {

struct ExceptionCode {
    int level = 0;
    int id = 0;
    int index = 0;
    int code = 0;
    bool operator==(const ExceptionCode&) const = default;
};

enum class ExceptionSeverity { Informational, Pause, Cancel };

/// Find a four-part `level-id-index-code` anywhere in `text`.
/// nullopt when there is none - including for the firmware's three-part basic
/// form, which omits the level and would otherwise decode shifted by one field.
[[nodiscard]] std::optional<ExceptionCode> decode_exception_code(const std::string& text);

/// Our wording for a fault we recognise; empty for one we do not, so the caller
/// falls back to the firmware's own message rather than inventing one.
[[nodiscard]] std::string_view exception_message(const ExceptionCode& c);

/// What the firmware will do about a fault at this level. An unrecognised level
/// reads as Cancel: assuming the worst is the safe direction.
[[nodiscard]] ExceptionSeverity severity_of(int level);

} // namespace helix::snapmaker
