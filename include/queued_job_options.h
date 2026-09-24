// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file queued_job_options.h
 * @brief Per-job pre-print option state stored in the Moonraker database
 *
 * Moonraker's job queue carries no per-job data, so the options a user picked
 * when queueing a print (option row states from the file detail view) live in
 * namespace `helix-screen`, key `queued_job_options`:
 *
 *   { "<job_id>": { "filename": "...", "options": { "<option_id>": bool } } }
 *
 * Encode/decode/prune are pure so they test without LVGL; the store function
 * wires them to IMoonrakerAPI's database get/post.
 */

#pragma once

#include "i_moonraker_api.h"
#include "json_fwd.h"

#include <map>
#include <string>
#include <vector>

namespace helix {
class AsyncLifetimeGuard;
}

namespace helix::queue {

/// Moonraker database namespace shared with tool_state.cpp's spool data
inline constexpr const char* kOptionsDbNamespace = "helix-screen";
/// Database key holding the whole queued-job option map
inline constexpr const char* kOptionsDbKey = "queued_job_options";

/// Options saved for one queued job
struct QueuedJobOptions {
    std::string filename;                ///< G-code filename as queued
    std::map<std::string, bool> options; ///< option row id -> saved state

    friend bool operator==(const QueuedJobOptions& a, const QueuedJobOptions& b) {
        return a.filename == b.filename && a.options == b.options;
    }
};

/// job_id -> saved state for that queued job
using QueuedJobOptionsMap = std::map<std::string, QueuedJobOptions>;

/// @brief Serialize the map to the database value shape
json encode_queued_job_options(const QueuedJobOptionsMap& entries);

/// @brief Parse a database value. Rows are all-or-nothing: an entry that is
/// not an object, has a non-string filename, a non-object options member or a
/// non-boolean option state is skipped whole rather than thrown on — the store
/// is advisory and a bad row must not cost the rest.
QueuedJobOptionsMap decode_queued_job_options(const json& value);

/// @brief Result of pruning: the surviving entries and whether any dropped
struct PrunedOptions {
    QueuedJobOptionsMap entries;
    bool changed = false;
};

/// @brief Drop entries whose job_id is no longer queued
///
/// @param entries Stored map (moved from when entries are dropped)
/// @param queued_job_ids Job ids currently in Moonraker's queue
PrunedOptions prune_queued_job_options(QueuedJobOptionsMap entries,
                                       const std::vector<std::string>& queued_job_ids);

/// @brief Read the stored map, prune it against the current queue, write back
///
/// Fire-and-forget and best-effort: a missing key (first run, fresh database)
/// and any read error leave the store untouched, and nothing is written unless
/// pruning dropped something. The read completion is marshalled through
/// @p lifetime so a store whose owner died mid-request never dereferences
/// @p api.
///
/// @param lifetime Guard of the object owning @p api's lifetime
/// @param api API to read/write through; null is a no-op
/// @param queued_job_ids Job ids currently in Moonraker's queue
void prune_stored_queued_job_options(AsyncLifetimeGuard& lifetime, IMoonrakerAPI* api,
                                     const std::vector<std::string>& queued_job_ids);

} // namespace helix::queue
