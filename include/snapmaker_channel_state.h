// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The Snapmaker U1 filament_feed channel_state vocabulary: one table that the
// AMS backend's status parse and the connect-time batch reconcile both read.
// Compiled on every target, so capability code that only asks "is a feed under
// way" does not depend on the gated backend class.

#include "ams_types.h"

#include <string>

namespace helix::snapmaker {

// Classification of a single filament_feed channel_state. The firmware exposes
// 39 distinct states (filament_feed.py:34-72, captured live from firmware
// 20260608); this maps each to everything the backend needs so the parse reads
// off ONE table instead of scattered string compares. See
// .claude/scratchpad/u1_channel_state_reference.md for the authoritative table.
//
// Fields:
//  - action:        the AmsAction the operation collapses to (drives the coarse
//                   LOAD/UNLOAD/ERROR/IDLE status). LOADING covers preload/load/
//                   manual feed; UNLOADING covers unload; ERROR covers *_fail;
//                   IDLE covers none/inited/wait_insert/test and every *_finish.
//  - phase:         step-bar step index into get_operation_step_model(op).
//                   Per-direction (a state is unambiguously load/unload/manual by
//                   prefix, so indices never collide across directions):
//                     LOAD/manual/preload model (5 steps):
//                       0=Home 1=Select 2=Heat 3=Feed 4=Purge
//                     UNLOAD model (4 steps):
//                       0=Home 1=Select 2=Heat 3=Retract
//                   -1 = "no active step" (idle / *_finish / *_fail).
//  - is_terminal:   a *_finish that ENDS the operation (resolves action -> IDLE).
//                   preload_finish is terminal-for-latch but does NOT end the op
//                   (the nozzle may still be heating on a re-unload); the parse
//                   special-cases it.
//  - is_fail:       a *_fail state, surfaced as ERROR.
//  - sets_loaded:   SET the "loaded at toolhead" latch true (load_finish only).
//  - clears_loaded: CLEAR the latch false (unload_finish/wait_insert/preload_finish).
//  - ignore:        the factory 'test' state; touch nothing.
struct ChannelStateInfo {
    AmsAction action = AmsAction::IDLE;
    int phase = -1;
    bool is_terminal = false;
    bool is_fail = false;
    bool sets_loaded = false;
    bool clears_loaded = false;
    bool ignore = false;
};

[[nodiscard]] ChannelStateInfo classify_channel_state(const std::string& state);

/// True when a channel_state names a load or unload under way.
[[nodiscard]] bool channel_state_in_progress(const std::string& state);

} // namespace helix::snapmaker
