// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "u1_batch_reconcile.h"

#include "i_moonraker_client.h"
#include "spdlog/spdlog.h"

namespace helix::u1_batch {

namespace {

/// Clears `doing` and restores the hotend targets the batch snapshotted at
/// its START. The command alias is uppercased regardless of the config's
/// spelling; the status object key is not (see macro_config_name()).
constexpr const char* CMD_END = "AUTO_FEEDING_BATCH ACTION=END";

} // namespace

void reconcile_on_connect(IMoonrakerClient& client, const nlohmann::json& status,
                          const std::string& macro_object) {
    const auto macro = status.find(macro_object);
    if (macro == status.end() || !macro->is_object()) {
        return;
    }
    // `doing` is a save-variable whose JSON type is only as strict as the
    // firmware that wrote it: a non-bool value is "no reading", never a
    // crash — an exception here unwinds through the subscribe response
    // callback and discovery never completes on any connect.
    const auto doing = macro->find("doing");
    if (doing == macro->end() || !doing->is_boolean() || !doing->get<bool>()) {
        return;
    }
    // A print in flight owns the interlock: ACTION=END restores the targets
    // snapshotted at batch START, which mid-print are the wrong values to
    // restore. Only an idle printer gets the cleanup.
    const std::string print_state =
        status.value("print_stats", nlohmann::json::object()).value("state", std::string());
    if (print_state == "printing" || print_state == "paused" ||
        status.value("virtual_sdcard", nlohmann::json::object()).value("is_active", false)) {
        spdlog::info("[U1Batch] Batch interlock held with a print in flight - leaving it alone");
        return;
    }
    spdlog::info("[U1Batch] Clearing a stranded AUTO_FEEDING_BATCH interlock");
    client.gcode_script(CMD_END);
}

} // namespace helix::u1_batch
