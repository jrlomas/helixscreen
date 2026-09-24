// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "queued_job_options.h"

#include "async_lifetime_guard.h"
#include "i_moonraker_api.h"

#include <spdlog/spdlog.h>

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

} // namespace helix::queue
