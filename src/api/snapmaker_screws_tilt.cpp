// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "snapmaker_screws_tilt.h"

#include "i_moonraker_client.h"
#include "snapmaker_exceptions.h"
#include "spdlog/spdlog.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <optional>

namespace helix {

namespace {

// Klipper emits JSON null for keys a printer does not define; reading one as
// 0 would fake a screw sitting exactly at the target height.
[[nodiscard]] bool read_number(const nlohmann::json& status, const std::string& field, float& out) {
    if (!status.is_object()) {
        return false;
    }
    const auto it = status.find(field);
    if (it == status.end() || it->is_null() || !it->is_number()) {
        return false;
    }
    out = it->get<float>();
    return true;
}

} // namespace

AutoScrewsTiltResults parse_auto_screws_tilt(const nlohmann::json& status,
                                             const nlohmann::json& config_section, float pitch_mm) {
    AutoScrewsTiltResults results;

    float target_z = 0.0f;
    if (!read_number(status, "target_z", target_z)) {
        results.error = "auto_screws_tilt_adjust: missing or null target_z";
        return results;
    }

    std::vector<ScrewTiltResult> screws;
    screws.reserve(4);
    for (int i = 1; i <= 4; i++) {
        const std::string base_key = "base_point" + std::to_string(i);

        float base_z = 0.0f;
        if (!read_number(status, base_key, base_z)) {
            results.error = "auto_screws_tilt_adjust: missing or null " + base_key;
            return results;
        }

        ScrewTiltResult screw;
        screw.z_height = base_z;
        // Measured against target_z, not against a base screw: every screw
        // can carry a real adjustment.
        screw.is_reference = false;

        // Coordinates and display names come from the configfile section.
        if (config_section.is_object()) {
            const auto coords = config_section.find("screw" + std::to_string(i));
            if (coords != config_section.end() && coords->is_string()) {
                std::sscanf(coords->get_ref<const std::string&>().c_str(), "%f,%f", &screw.x_pos,
                            &screw.y_pos);
            }
            const auto name = config_section.find("screw" + std::to_string(i) + "_name");
            if (name != config_section.end() && name->is_string()) {
                screw.screw_name = name->get_ref<const std::string&>();
            }
        }

        // Signed minutes carry the direction (CW positive, CCW negative), the
        // same convention signed_adjustment_minutes() parses back out.
        const int signed_minutes = screw_minutes_for_mm(target_z - base_z, pitch_mm);
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%s %02d:%02d", signed_minutes >= 0 ? "CW" : "CCW",
                      std::abs(signed_minutes) / 60, std::abs(signed_minutes) % 60);
        screw.adjustment = buf;

        screws.push_back(std::move(screw));
    }

    results.screws = std::move(screws);
    return results;
}

namespace snapmaker {
namespace screws_tilt {

bool stale_calibration_state(int main_state, const std::string& probe_step) {
    if (main_state != MAIN_STATE_SCREWS_TILT_ADJUST) {
        return false;
    }
    static const std::array<const char*, 6> kNotRunning = {
        "adjust_idle",
        "adjust_complete",
        "adjust_failed",
        "adjust_homing_failed",
        "adjust_plate_detection_error",
        "adjust_aborted",
    };
    return std::find(kNotRunning.begin(), kNotRunning.end(), probe_step) != kNotRunning.end();
}

std::optional<int> main_state_from_status(const nlohmann::json& status) {
    if (!status.is_object()) {
        return std::nullopt;
    }
    const auto manager = status.find("machine_state_manager");
    if (manager == status.end() || !manager->is_object()) {
        return std::nullopt;
    }
    const auto state = manager->find("main_state");
    if (state == manager->end() || !state->is_number()) {
        return std::nullopt;
    }
    return state->get<int>();
}

namespace {

// result.status out of an objects.query response; the response can instead
// be an error object, which carries no status.
const nlohmann::json* query_status(const nlohmann::json& response) {
    if (!response.is_object()) {
        return nullptr;
    }
    const auto result = response.find("result");
    if (result == response.end() || !result->is_object()) {
        return nullptr;
    }
    const auto status = result->find("status");
    if (status == result->end() || !status->is_object()) {
        return nullptr;
    }
    return &*status;
}

} // namespace

bool plate_still_on_bed(const std::string& error_message) {
    const auto code = snapmaker::decode_exception_code(error_message);
    return code && code->id == 530 && code->index == 0 && code->code == 11;
}

AutoScrewsTiltResults results_from_query(const nlohmann::json& response) {
    const nlohmann::json* status = query_status(response);
    const nlohmann::json* module_status = nullptr;
    if (status) {
        const auto module = status->find(MODULE_NAME);
        if (module != status->end() && module->is_object()) {
            module_status = &*module;
        }
    }
    if (!module_status) {
        AutoScrewsTiltResults none;
        none.error = std::string(MODULE_NAME) + ": no status object in query response";
        return none;
    }

    nlohmann::json config_section = nlohmann::json::object();
    const auto configfile = status->find("configfile");
    if (configfile != status->end() && configfile->is_object()) {
        const auto settings = configfile->find("settings");
        if (settings != configfile->end() && settings->is_object()) {
            const auto section = settings->find(MODULE_NAME);
            if (section != settings->end() && section->is_object()) {
                config_section = *section;
            }
        }
    }
    return parse_auto_screws_tilt(*module_status, config_section);
}

void request_exit(IMoonrakerClient& client) {
    json params = {
        {"objects", json::object({{"machine_state_manager", json::array({"main_state"})}})}};
    client.send_jsonrpc("printer.objects.query", params, [&client](const json& response) {
        const json* status = query_status(response);
        const std::optional<int> state = status ? main_state_from_status(*status) : std::nullopt;
        if (!state || *state != MAIN_STATE_SCREWS_TILT_ADJUST) {
            return;
        }
        spdlog::info("[AutoScrewsTilt] Leaving the firmware screws-tilt state");
        client.gcode_script(CMD_EXIT);
    });
}

void reconcile_on_connect(IMoonrakerClient& client, const nlohmann::json& initial_status) {
    const std::optional<int> held = main_state_from_status(initial_status);
    if (!held || *held != MAIN_STATE_SCREWS_TILT_ADJUST) {
        // The discovery subscription carries machine_state_manager only under
        // mmu_type() == SNAPMAKER, which does not track the screws dialect:
        // a U1 without SnapSwap never subscribes, and this is where that
        // coupling shows up as a missing object.
        if (!initial_status.contains("machine_state_manager")) {
            spdlog::debug("[AutoScrewsTilt] No machine_state_manager in the discovery snapshot "
                          "- connect-time reconcile has nothing to read");
        }
        return;
    }
    // probe_step is not part of the discovery subscription, so read it (and a
    // fresh main_state) now, once, at connect time.
    json params = {{"objects", json::object({{"machine_state_manager", json::array({"main_state"})},
                                             {MODULE_NAME, json::array({"probe_step"})}})}};
    client.send_jsonrpc("printer.objects.query", params, [&client](const json& response) {
        const json* status = query_status(response);
        if (!status) {
            return;
        }
        const std::optional<int> state = main_state_from_status(*status);

        std::string probe_step;
        const auto module = status->find(MODULE_NAME);
        if (module != status->end() && module->is_object()) {
            const auto step = module->find("probe_step");
            if (step != module->end() && step->is_string()) {
                probe_step = step->get_ref<const std::string&>();
            }
        }

        if (!state || !stale_calibration_state(*state, probe_step)) {
            spdlog::info("[AutoScrewsTilt] Screws-tilt state held with work in flight "
                         "(probe_step='{}') - leaving it to its driver",
                         probe_step.empty() ? "<unknown>" : probe_step);
            return;
        }
        spdlog::info("[AutoScrewsTilt] Clearing a stale screws-tilt state (probe_step={})",
                     probe_step);
        // CMD_EXIT needs exactly reconcile's own precondition (main_state ==
        // SCREWS_TILT_ADJUST), just re-read above. If the state changed
        // between that read and this send, the macro throws - a benign race
        // the fire-and-forget RPC layer logs; there is nothing to retry
        // against a state we no longer hold.
        client.gcode_script(CMD_EXIT);
    });
}

} // namespace screws_tilt
} // namespace snapmaker

} // namespace helix
