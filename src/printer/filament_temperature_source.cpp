// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_temperature_source.h"

#include "i_moonraker_client.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <utility>

#include "hv/json.hpp"

namespace helix::filament_temps {
namespace {

/// One firmware that publishes its own per-filament temperature table.
struct Provider {
    const char* name;          ///< For logs.
    const char* detect_object; ///< Klipper object whose presence identifies it.
    const char* query_gcode;   ///< One-line command that prints the table.
};

// The U1's `filament_parameters` module exists to answer this query; it
// publishes nothing through status, only through the console response.
constexpr Provider PROVIDERS[] = {
    {"Snapmaker U1", "filament_parameters", "FILAMENT_PARA_GET_ALL_INFO"},
};

const Provider* match(const PrinterDiscovery& hw) {
    if (!hw.objects_reported()) {
        return nullptr;
    }
    const auto& objects = hw.printer_objects();
    for (const Provider& p : PROVIDERS) {
        if (std::find(objects.begin(), objects.end(), p.detect_object) != objects.end()) {
            return &p;
        }
    }
    return nullptr;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// True when every True/False in the payload sits where the schema puts a
/// boolean literal: after a ':' or '[' (spaces aside) and before a ',', '}' or
/// ']'. A True spelled inside a quoted value fails this and the parse refuses.
bool booleans_are_literals(const std::string& s) {
    for (const char* word : {"True", "False"}) {
        const size_t len = std::strlen(word);
        size_t pos = 0;
        while ((pos = s.find(word, pos)) != std::string::npos) {
            size_t before = pos;
            while (before > 0 && s[before - 1] == ' ') {
                --before;
            }
            if (before == 0 || (s[before - 1] != ':' && s[before - 1] != '[')) {
                return false;
            }
            size_t after = pos + len;
            while (after < s.size() && s[after] == ' ') {
                ++after;
            }
            if (after >= s.size() || (s[after] != ',' && s[after] != '}' && s[after] != ']')) {
                return false;
            }
            pos = after;
        }
    }
    return true;
}

void replace_all(std::string& s, const std::string& from, const std::string& to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

/// Read one leaf temperature: an integer the firmware reports, bounded like a
/// temperature. Anything else is absence, never a guess.
std::optional<int> leaf_temp(const nlohmann::json& leaf, const char* field) {
    auto it = leaf.find(field);
    if (it == leaf.end() || !it->is_number_integer()) {
        return std::nullopt;
    }
    const int64_t v = it->get<int64_t>();
    if (v < 0 || v > 999) {
        return std::nullopt;
    }
    return static_cast<int>(v);
}

std::mutex g_table_mutex;
std::map<FilamentKey, FilamentTemperatures> g_table;

} // namespace

bool firmware_publishes_filament_temperatures(const PrinterDiscovery& hw) {
    return match(hw) != nullptr;
}

std::string filament_temperature_query_gcode(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    return p ? std::string(p->query_gcode) : std::string();
}

std::map<FilamentKey, FilamentTemperatures>
parse_filament_temperatures(const std::string& response) {
    std::string payload = response;
    const size_t first_char = payload.find_first_not_of(" \t\r\n");
    payload.erase(0, first_char == std::string::npos ? payload.size() : first_char);
    // One console line on the response channel, prefixed "// ".
    if (payload.rfind("// ", 0) == 0) {
        payload.erase(0, 3);
    } else if (payload.rfind("//", 0) == 0) {
        payload.erase(0, 2);
    }

    // The quote/boolean swap below is only sound for the firmware's own
    // spelling; anything richer must refuse rather than corrupt.
    if (payload.find('"') != std::string::npos || payload.find('\\') != std::string::npos ||
        !booleans_are_literals(payload)) {
        return {};
    }
    std::replace(payload.begin(), payload.end(), '\'', '"');
    replace_all(payload, "True", "true");
    replace_all(payload, "False", "false");

    nlohmann::json root;
    try {
        root = nlohmann::json::parse(payload);
    } catch (const nlohmann::json::exception&) {
        return {};
    }

    std::map<FilamentKey, FilamentTemperatures> table;
    if (!root.is_object()) {
        return table;
    }
    for (auto type_it = root.begin(); type_it != root.end(); ++type_it) {
        // version and the two flow ceilings are table metadata, not types.
        if (type_it.key() == "version" || type_it.key() == "hard_filaments_max_flow_k" ||
            type_it.key() == "soft_filaments_max_flow_k" || !type_it.value().is_object()) {
            continue;
        }
        for (auto vendor_it = type_it.value().begin(); vendor_it != type_it.value().end();
             ++vendor_it) {
            const std::string& vkey = vendor_it.key();
            if (vkey.rfind("vendor_", 0) != 0 || !vendor_it.value().is_object()) {
                continue;
            }
            for (auto sub_it = vendor_it.value().begin(); sub_it != vendor_it.value().end();
                 ++sub_it) {
                const std::string& skey = sub_it.key();
                if (skey.rfind("sub_", 0) != 0 || !sub_it.value().is_object()) {
                    continue;
                }
                FilamentTemperatures temps;
                temps.load_c = leaf_temp(sub_it.value(), "load_temp");
                temps.unload_c = leaf_temp(sub_it.value(), "unload_temp");
                temps.clean_nozzle_c = leaf_temp(sub_it.value(), "clean_nozzle_temp");
                if (temps.load_c || temps.unload_c || temps.clean_nozzle_c) {
                    table[FilamentKey{lower(vkey.substr(7)), lower(type_it.key()),
                                      lower(skey.substr(4))}] = temps;
                }
            }
        }
    }
    return table;
}

void store_filament_temperatures(const std::map<FilamentKey, FilamentTemperatures>& table) {
    std::lock_guard<std::mutex> lock(g_table_mutex);
    g_table = table;
}

void clear_filament_temperatures() {
    std::lock_guard<std::mutex> lock(g_table_mutex);
    g_table.clear();
}

std::optional<FilamentTemperatures> lookup_filament_temperatures(const SlotInfo& slot) {
    std::lock_guard<std::mutex> lock(g_table_mutex);
    const std::string main = lower(slot.material);
    const std::string vendor = lower(slot.brand);
    const std::string sub = lower(slot.spool_name);
    auto at = [&](const std::string& v,
                  const std::string& s) -> std::optional<FilamentTemperatures> {
        auto it = g_table.find(FilamentKey{v, main, s});
        if (it == g_table.end()) {
            return std::nullopt;
        }
        return it->second;
    };
    if (auto t = at(vendor, sub)) {
        return t;
    }
    if (auto t = at(vendor, "generic")) {
        return t;
    }
    return at("generic", "generic");
}

void capture_on_connect(IMoonrakerClient& client, const PrinterDiscovery& hw) {
    clear_filament_temperatures();
    const Provider* p = match(hw);
    if (!p) {
        return;
    }
    spdlog::info("[FilamentTemps] {} publishes per-filament temperatures; querying", p->name);
    const json params = {{"script", std::string(p->query_gcode)}};
    client.send_jsonrpc("printer.gcode.script", params, [&client](const json& response) {
        if (response.contains("error")) {
            return;
        }
        // Script completion means every line it printed is already in the
        // store, so read back and keep the newest line that parses.
        client.get_gcode_store(
            32,
            [](const std::vector<GcodeStoreEntry>& entries) {
                for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
                    if (it->type != "response") {
                        continue;
                    }
                    auto table = parse_filament_temperatures(it->message);
                    if (!table.empty()) {
                        store_filament_temperatures(table);
                        spdlog::info("[FilamentTemps] Published {} per-filament temperature leaves",
                                     table.size());
                        return;
                    }
                }
                spdlog::warn("[FilamentTemps] No temperature table in the gcode store after query");
            },
            [](const MoonrakerError&) {});
    });
}

} // namespace helix::filament_temps
