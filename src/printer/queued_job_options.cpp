// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "queued_job_options.h"

#include "async_lifetime_guard.h"
#include "i_moonraker_api.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix::queue {

std::string queued_job_option_key(const std::string& job_id) {
    return std::string(kOptionsDbKey) + "." + job_id;
}

json encode_queued_job_entry(const QueuedJobOptions& entry) {
    json options = json::object();
    for (const auto& [option_id, enabled] : entry.options) {
        options[option_id] = enabled;
    }
    return {{"filename", entry.filename}, {"options", std::move(options)}};
}

namespace {

/// Shared row rule: a well-formed entry is an object with a string filename
/// and, when present, an object of booleans. Anything else is not an entry.
bool entry_shape_ok(const json& entry) {
    if (!entry.is_object() || !entry.contains("filename") || !entry["filename"].is_string()) {
        return false;
    }
    if (!entry.contains("options")) {
        return true;
    }
    const json& options = entry["options"];
    if (!options.is_object()) {
        return false;
    }
    for (auto opt = options.begin(); opt != options.end(); ++opt) {
        if (!opt.value().is_boolean()) {
            return false;
        }
    }
    return true;
}

} // namespace

QueuedJobOptions decode_queued_job_entry(const json& value) {
    QueuedJobOptions parsed;
    if (!entry_shape_ok(value)) {
        return parsed;
    }
    // entry_shape_ok() proved the key holds a string; .value() keeps the
    // read fallible so a malformed row can never reach a hard assert.
    parsed.filename = value.value("filename", std::string{});
    if (value.contains("options")) {
        const json& options = value["options"];
        for (auto opt = options.begin(); opt != options.end(); ++opt) {
            parsed.options[opt.key()] = opt.value().get<bool>();
        }
    }
    return parsed;
}

QueuedJobOptionsMap decode_queued_job_options(const json& value) {
    QueuedJobOptionsMap out;
    if (!value.is_object()) {
        return out;
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        // A malformed row is skipped whole: one kept partially would be
        // rewritten without the choices it did carry, looking intact.
        if (entry_shape_ok(it.value())) {
            out[it.key()] = decode_queued_job_entry(it.value());
        }
    }
    return out;
}

std::vector<std::string>
stale_queued_job_option_ids(const QueuedJobOptionsMap& stored,
                            const std::vector<std::string>& queued_job_ids) {
    std::vector<std::string> stale;
    for (const auto& [job_id, entry] : stored) {
        if (std::find(queued_job_ids.begin(), queued_job_ids.end(), job_id) ==
            queued_job_ids.end()) {
            stale.push_back(job_id);
        }
    }
    return stale;
}

void prune_stored_queued_job_options(AsyncLifetimeGuard& lifetime, IMoonrakerAPI* api,
                                     const std::vector<std::string>& queued_job_ids) {
    if (!api) {
        return;
    }

    api->database_get_item(
        kOptionsDbNamespace, kOptionsDbKey,
        lifetime.bg_cb(
            "queue::prune_options_read",
            [api, queued_job_ids](const json& stored) {
                for (const auto& job_id : stale_queued_job_option_ids(
                         decode_queued_job_options(stored), queued_job_ids)) {
                    api->database_delete_item(
                        kOptionsDbNamespace, queued_job_option_key(job_id),
                        [job_id]() {
                            spdlog::debug("[queue] Pruned stored options for job {}", job_id);
                        },
                        [job_id](const MoonrakerError& err) {
                            spdlog::warn("[queue] Pruning stored options for job {} failed: {}",
                                         job_id, err.user_message());
                        });
                }
            }),
        lifetime.bg_cb("queue::prune_options_read_error", [](const MoonrakerError& err) {
            // A missing key is the first-run state; anything else is informational.
            spdlog::debug("[queue] queued_job_options read failed: {}", err.message);
        }));
}

std::optional<std::string> find_new_job_id(const std::vector<std::string>& before,
                                           const std::vector<JobQueueEntry>& after) {
    std::optional<std::string> found;
    for (const auto& job : after) {
        if (std::find(before.begin(), before.end(), job.job_id) != before.end()) {
            continue;
        }
        if (found) {
            return std::nullopt;
        }
        found = job.job_id;
    }
    return found;
}

void save_queued_job_options(IMoonrakerAPI* api, const std::string& job_id,
                             QueuedJobOptions options) {
    if (!api) {
        return;
    }

    api->database_post_item(
        kOptionsDbNamespace, queued_job_option_key(job_id), encode_queued_job_entry(options),
        [job_id]() { spdlog::debug("[queue] Stored options for job {} written", job_id); },
        [job_id](const MoonrakerError& err) {
            spdlog::warn("[queue] Storing options for job {} failed: {}", job_id,
                         err.user_message());
        });
}

void load_queued_job_options(AsyncLifetimeGuard& lifetime, IMoonrakerAPI* api,
                             const std::string& job_id,
                             std::function<void(QueuedJobOptions)> on_loaded) {
    if (!api) {
        on_loaded(QueuedJobOptions{});
        return;
    }

    api->database_get_item(
        kOptionsDbNamespace, queued_job_option_key(job_id),
        lifetime.bg_cb("queue::load_options_read",
                       [on_loaded](const json& stored) mutable {
                           on_loaded(decode_queued_job_entry(stored));
                       }),
        lifetime.bg_cb("queue::load_options_read_error", [on_loaded](const MoonrakerError& err) {
            // A missing key is the never-saved case; every other error also
            // degrades to defaults rather than refusing the start.
            spdlog::debug("[queue] Reading stored options for load failed: {}", err.message);
            on_loaded(QueuedJobOptions{});
        }));
}

void delete_queued_job_options(IMoonrakerAPI* api, const std::string& job_id) {
    if (!api) {
        return;
    }

    api->database_delete_item(
        kOptionsDbNamespace, queued_job_option_key(job_id),
        [job_id]() { spdlog::debug("[queue] Stored options for job {} deleted", job_id); },
        [job_id](const MoonrakerError& err) {
            spdlog::warn("[queue] Deleting stored options for job {} failed: {}", job_id,
                         err.user_message());
        });
}

} // namespace helix::queue
