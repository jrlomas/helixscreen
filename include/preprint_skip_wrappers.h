// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Per-print skips for the leveling steps a PRINT_START runs unconditionally.
//
// HelixScreen never edits the user's PRINT_START. Instead it installs
// helix_skips.cfg: a `_HELIX_PREP` macro holding one run_* flag per step, and a
// gcode_macro per wrapped command that renames the built-in
// (rename_existing: _HELIX_BASE_<COMMAND>) and runs it unless its flag is 0.
// A skip is one-shot: the wrapper restores the flag as it consumes it.
//
// Klipper merges same-named [gcode_macro] sections from different files, later
// file winning per option. helix_skips.cfg is included at the top of
// printer.cfg, so a user macro of the same name always wins, and
// ours_intact() reads that as "not ours" from configfile.settings.
//
// No I/O and no LVGL: callers fetch configfile.settings and do the uploads.

#include <string>
#include <vector>

#include "hv/json.hpp"

namespace helix::skip_wrappers {

inline constexpr const char* FILENAME = "helix_skips.cfg";
inline constexpr const char* PREP_MACRO = "_HELIX_PREP";

enum class Op { BedMesh, Qgl, ZTilt };

/// The command the Op's wrapper replaces: "BED_MESH_CALIBRATE", ...
const char* command_for(Op op);

/// The `_HELIX_PREP` variable gating the Op: "run_bed_mesh", "run_qgl", "run_z_tilt".
const char* flag_for(Op op);

/// Ops this printer can wrap, in enum order. An Op qualifies when its built-in
/// section exists ([bed_mesh]; [quad_gantry_level]; [z_tilt] or [z_tilt_ng])
/// and every gcode_macro section it would define is absent or ours_intact().
/// Empty for a non-object settings value.
std::vector<Op> wrappable(const nlohmann::json& configfile_settings);

/// True when every gcode_macro section generate() writes for this Op is
/// present in configfile.settings with exactly our rename_existing and gcode.
/// A same-named user macro merged over ours reads false.
bool ours_intact(const nlohmann::json& configfile_settings, Op op);

/// Ops whose skip works on this printer right now: helix_skips.cfg is loaded
/// (the `_HELIX_PREP` section exists) and the Op is ours_intact(). Enum order.
std::vector<Op> active(const nlohmann::json& configfile_settings);

/// The helix_skips.cfg text for these Ops; empty string for no Ops.
std::string generate(const std::vector<Op>& ops);

/// Follows Klipper across the restart that loads a newly staged helix_skips.cfg.
/// A config error there is most likely ours, and removing the file is the way
/// back that needs no SSH; the READY the restart starts from is not the answer.
class LoadWatch {
  public:
    enum class Verdict { Waiting, Loaded, Failed };

    /// Feed every klippy state change after the restart was accepted.
    Verdict feed(bool ready, bool error);

  private:
    bool left_ready_ = false;
};

} // namespace helix::skip_wrappers
