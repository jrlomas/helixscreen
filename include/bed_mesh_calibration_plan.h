// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "standard_macros.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace helix {
namespace bed_mesh {

/// The profile Klipper stores a mesh in when BED_MESH_CALIBRATE names none.
/// Klipper refuses `BED_MESH_PROFILE SAVE=default`, so a mesh only gets there by
/// being probed there.
inline constexpr const char* DEFAULT_PROFILE = "default";

/// What to send for a calibration the user named before probing, and where the
/// finished mesh ends up.
struct CalibrationPlan {
    /// Gcode to send.
    std::string command;

    /// The command heats and homes the printer itself.
    bool self_prepares = false;

    /// The profile the command stores the finished mesh in.
    std::string writes_profile;

    /// Set when the command cannot store under the chosen name: after it completes,
    /// load writes_profile and save it under this name. Never `default`.
    std::string copy_to;

    /// The profile holding the mesh once the plan has run.
    [[nodiscard]] const std::string& final_profile() const {
        return copy_to.empty() ? writes_profile : copy_to;
    }
};

/// @p name as a single gcode parameter value. Klipper splits a command's
/// parameters the way a shell does, with `#` and `;` starting a comment, so a name
/// holding whitespace, quotes or either of those is quoted.
[[nodiscard]] inline std::string gcode_param_value(const std::string& name) {
    if (name.find_first_of(" \t\"'\\#;") == std::string::npos) {
        return name;
    }
    std::string quoted = "\"";
    for (char c : name) {
        if (c == '"' || c == '\\') {
            quoted += '\\';
        }
        quoted += c;
    }
    quoted += '"';
    return quoted;
}

/// ` PROFILE=<name>` for a named profile, empty for the default one, which is
/// what BED_MESH_CALIBRATE writes when given no profile.
[[nodiscard]] inline std::string profile_arg(const std::string& name) {
    return name == DEFAULT_PROFILE ? std::string() : " PROFILE=" + gcode_param_value(name);
}

/// Does Klipper refuse a `BED_MESH_PROFILE SAVE` under @p name?
[[nodiscard]] inline bool save_is_refused_for(const std::string& name) {
    return name == DEFAULT_PROFILE;
}

/// Is @p line Klipper declining a `BED_MESH_PROFILE SAVE`? It says so on the
/// console and still answers the request `ok`.
[[nodiscard]] inline bool is_profile_save_refusal(const std::string& line) {
    return line.find("Unable to save to profile") != std::string::npos ||
           line.find("has not been probed") != std::string::npos ||
           line.find("is reserved, please choose another profile name") != std::string::npos;
}

namespace detail {

/// The first word of a one-line @p script, uppercased; empty for several lines.
inline std::string single_command_word(const std::string& script) {
    if (script.find('\n') != std::string::npos) {
        return {};
    }
    const size_t start = script.find_first_not_of(" \t");
    if (start == std::string::npos) {
        return {};
    }
    const size_t end = script.find_first_of(" \t", start);
    std::string word = script.substr(start, end == std::string::npos ? end : end - start);
    std::transform(word.begin(), word.end(), word.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return word;
}

/// The value of a PROFILE= parameter already on @p command, or empty.
inline std::string named_profile(const std::string& command) {
    std::string upper = command;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    const size_t at = upper.find(" PROFILE=");
    if (at == std::string::npos) {
        return {};
    }
    const size_t start = at + 9;
    const size_t end = command.find_first_of(" \t", start);
    return command.substr(start, end == std::string::npos ? end : end - start);
}

} // namespace detail

/**
 * @brief Plan a mesh calibration whose result is to be stored as @p name.
 *
 * BED_MESH_CALIBRATE stores the finished mesh in the profile its PROFILE= names,
 * or in `default` when given none. A shipped sequence marks where that argument
 * goes with `{profile_arg}`; a bare BED_MESH_CALIBRATE, detected or configured,
 * takes it appended. Anything else (G29, a user's own macro, a sequence without
 * the placeholder) cannot be told a name, so it is left to store in `default`
 * and the plan copies the mesh from there.
 *
 * @param slot       The bed mesh slot to resolve.
 * @param name       Profile name the user chose, already trimmed and non-empty.
 * @param bed_temp_c Substituted for `{bed_temp}`.
 */
[[nodiscard]] inline CalibrationPlan plan_calibration(const StandardMacroInfo& slot,
                                                      const std::string& name, int bed_temp_c) {
    // accept_fallback=false: the HELIX_* fallback returns without probing when a
    // recent mesh exists, and a calibration the user asked for must probe.
    ResolvedMacroScript resolved =
        resolve_macro_script(slot, {.profile_arg = profile_arg(name), .bed_temp_c = bed_temp_c},
                             /*accept_fallback=*/false);
    if (resolved.script.empty()) {
        resolved.script = "BED_MESH_CALIBRATE";
    }

    CalibrationPlan plan;
    plan.self_prepares = resolved.self_prepares;
    plan.command = resolved.script;

    if (resolved.takes_profile_arg) {
        plan.writes_profile = name;
        return plan;
    }

    if (detail::single_command_word(resolved.script) == "BED_MESH_CALIBRATE") {
        const std::string already = detail::named_profile(resolved.script);
        if (already.empty()) {
            plan.command += profile_arg(name);
            plan.writes_profile = name;
            return plan;
        }
        plan.writes_profile = already;
    } else {
        plan.writes_profile = DEFAULT_PROFILE;
    }

    if (plan.writes_profile != name && !save_is_refused_for(name)) {
        plan.copy_to = name;
    }
    return plan;
}

} // namespace bed_mesh
} // namespace helix
