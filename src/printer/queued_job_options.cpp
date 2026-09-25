// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "queued_job_options.h"

#include "async_lifetime_guard.h"
#include "i_moonraker_api.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix::queue {

json encode_queued_job_options(const QueuedJobOptionsMap& entries) {
    json out = json::object();
    for (const auto& [job_id, entry] : entries) {
        json options = json::object();
        for (const auto& [option_id, enabled] : entry.options) {
            options[option_id] = enabled;
        }
        out[job_id] = {{"filename", entry.filename}, {"options", std::move(options)}};
    }
    return out;
}

QueuedJobOptionsMap decode_queued_job_options(const json& value) {
    QueuedJobOptionsMap out;
    if (!value.is_object()) {
        return out;
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        const json& entry = it.value();
        if (!entry.is_object() || !entry.contains("filename") || !entry["filename"].is_string()) {
            continue;
        }
        QueuedJobOptions parsed;
        parsed.filename = entry["filename"].get<std::string>();
        // A row is all-or-nothing: one kept with a malformed options member
        // would be written back by the next prune without the choices it did
        // carry, looking intact.
        bool row_ok = true;
        if (entry.contains("options")) {
            const json& options = entry["options"];
            row_ok = options.is_object();
            for (auto opt = options.begin(); row_ok && opt != options.end(); ++opt) {
                row_ok = opt.value().is_boolean();
                if (row_ok) {
                    parsed.options[opt.key()] = opt.value().get<bool>();
                }
            }
        }
        if (row_ok) {
            out[it.key()] = std::move(parsed);
        }
    }
    return out;
}

PrunedOptions prune_queued_job_options(QueuedJobOptionsMap entries,
                                       const std::vector<std::string>& queued_job_ids) {
    PrunedOptions result;
    result.changed = false;
    for (const auto& id : queued_job_ids) {
        auto it = entries.find(id);
        if (it == entries.end()) {
            continue;
        }
        result.entries.emplace(id, std::move(it->second));
    }
    result.changed = result.entries.size() != entries.size();
    return result;
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
                auto pruned =
                    prune_queued_job_options(decode_queued_job_options(stored), queued_job_ids);
                if (!pruned.changed) {
                    return;
                }
                api->database_post_item(
                    kOptionsDbNamespace, kOptionsDbKey, encode_queued_job_options(pruned.entries),
                    []() { spdlog::debug("[queue] Pruned queued_job_options written"); },
                    [](const MoonrakerError& err) {
                        spdlog::warn("[queue] Pruning queued_job_options failed to write: {}",
                                     err.user_message());
                    });
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

void save_queued_job_options(AsyncLifetimeGuard& lifetime, IMoonrakerAPI* api,
                             const std::string& job_id, QueuedJobOptions options) {
    if (!api) {
        return;
    }

    auto write = [api, job_id,
                  options = std::move(options)](const QueuedJobOptionsMap& base) mutable {
        auto entries = base;
        entries[job_id] = std::move(options);
        api->database_post_item(
            kOptionsDbNamespace, kOptionsDbKey, encode_queued_job_options(entries),
            []() { spdlog::debug("[queue] Saved queued_job_options written"); },
            [](const MoonrakerError& err) {
                spdlog::warn("[queue] Saving queued_job_options failed to write: {}",
                             err.user_message());
            });
    };

    api->database_get_item(
        kOptionsDbNamespace, kOptionsDbKey,
        lifetime.bg_cb(
            "queue::save_options_read",
            [write](const json& stored) mutable { write(decode_queued_job_options(stored)); }),
        lifetime.bg_cb(
            "queue::save_options_read_error", [write](const MoonrakerError& err) mutable {
                // A missing key is a first save, not a failure: write over an
                // empty base. Anything else is a real read error — skip rather
                // than clobber the stored map down to this one entry.
                const bool missing_key =
                    err.code == 404 || err.message.find("not found") != std::string::npos;
                if (missing_key) {
                    write({});
                    return;
                }
                spdlog::warn("[queue] Reading queued_job_options to save failed: {}", err.message);
            }));
}

} // namespace helix::queue
