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
#include "i_moonraker_sub_apis.h" // for JobQueueEntry
#include "json_fwd.h"

#include <map>
#include <optional>
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

/// @brief The job id present in @p after but not in @p before
///
/// post_job carries no echo of the created id, so it is recovered by diffing
/// the queue before the call against the queue the response reports.
/// nullopt when the answer is not exactly one id — an unchanged list, or two
/// new ids at once (another client queued concurrently): guessing there would
/// attach the saved options to someone else's job, so the caller skips the
/// save and the job stays queued without options.
std::optional<std::string> find_new_job_id(const std::vector<std::string>& before,
                                           const std::vector<JobQueueEntry>& after);

/// @brief Read-modify-write one job's options into the store
///
/// Best-effort like the prune: a missing key is the start-from-empty case
/// (the write still happens), and a read error that is not a missing key
/// skips the save rather than clobbering the stored map with one entry.
/// Marshalled through @p lifetime for the same owner-outlives-request
/// guarantee.
///
/// @param lifetime Guard of the object owning @p api's lifetime
/// @param api API to read/write through; null is a no-op
/// @param job_id The newly queued job the options belong to
/// @param options Filename and option row states gathered at queue time
void save_queued_job_options(AsyncLifetimeGuard& lifetime, IMoonrakerAPI* api,
                             const std::string& job_id, QueuedJobOptions options);

} // namespace helix::queue
