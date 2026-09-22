// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "hv/json.hpp"

namespace helix {

class IMoonrakerClient;

namespace u1_batch {

/**
 * @brief Connect-time cleanup of a stranded AUTO_FEEDING_BATCH interlock
 *
 * A session that dies mid-batch leaves the macro's `doing` save-variable set,
 * and PRINT_PRESTART_CHECK refuses every print over it. Sends ACTION=END when
 * the discovery snapshot shows `doing` true with no print in flight; a
 * printing or paused print, or an active virtual_sdcard, owns the interlock
 * legitimately, because ACTION=END restores the hotend targets the batch
 * snapshotted at START - the right cleanup when idle, the wrong targets
 * mid-print. Runs on the WebSocket thread beside auto_screws' reconcile;
 * touches no LVGL.
 */
void reconcile_on_connect(IMoonrakerClient& client, const nlohmann::json& initial_status);

} // namespace u1_batch
} // namespace helix
