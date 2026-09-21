// src/printer/snapmaker_print_preferences.cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "snapmaker_print_preferences.h"

namespace helix::snapmaker {
namespace {

/// A firmware storing a flag as a Python bool serialises true/false; one
/// storing it as an int serialises 0/1. Anything else is not a setting we can
/// read, and is left unset rather than guessed at.
std::optional<bool> read_flag(const nlohmann::json& obj, const char* key) {
    auto it = obj.find(key);
    if (it == obj.end()) {
        return std::nullopt;
    }
    if (it->is_boolean()) {
        return it->get<bool>();
    }
    if (it->is_number_integer()) {
        return it->get<int>() != 0;
    }
    return std::nullopt;
}

void append(std::string& out, const char* name, const std::string& value) {
    out += (out.empty() ? "SET_PRINT_PREFERENCES " : " ");
    out += name;
    out += '=';
    out += value;
}

} // namespace

bool PrintPreferences::empty() const {
    return !auto_replenish && !replenish_ignore_color && !filament_entangle_detect &&
           !end_led_turn_off && !filament_entangle_sen && end_unload_filament.empty();
}

PrintPreferences read_print_preferences(const nlohmann::json& status) {
    PrintPreferences p;
    if (!status.is_object()) {
        return p;
    }
    auto ptc = status.find("print_task_config");
    if (ptc == status.end() || !ptc->is_object()) {
        return p;
    }
    p.auto_replenish = read_flag(*ptc, "auto_replenish_filament");
    p.replenish_ignore_color = read_flag(*ptc, "replenish_ignore_color");
    p.filament_entangle_detect = read_flag(*ptc, "filament_entangle_detect");
    p.end_led_turn_off = read_flag(*ptc, "end_led_turn_off");

    if (auto it = ptc->find("filament_entangle_sen"); it != ptc->end() && it->is_string()) {
        p.filament_entangle_sen = it->get<std::string>();
    }
    if (auto it = ptc->find("end_unload_filament"); it != ptc->end() && it->is_array()) {
        for (const auto& v : *it) {
            if (v.is_boolean()) {
                p.end_unload_filament.push_back(v.get<bool>());
            } else if (v.is_number_integer()) {
                p.end_unload_filament.push_back(v.get<int>() != 0);
            }
        }
    }
    return p;
}

std::string write_print_preferences_gcode(const PrintPreferences& changes) {
    std::string out;
    if (changes.auto_replenish) {
        append(out, "AUTO_REPLENISH_FILAMENT", *changes.auto_replenish ? "1" : "0");
    }
    if (changes.replenish_ignore_color) {
        append(out, "REPLENISH_IGNORE_COLOR", *changes.replenish_ignore_color ? "1" : "0");
    }
    if (changes.filament_entangle_detect) {
        append(out, "FILAMENT_ENTANGLE_DETECT", *changes.filament_entangle_detect ? "1" : "0");
    }
    if (changes.end_led_turn_off) {
        append(out, "END_LED_TURN_OFF", *changes.end_led_turn_off ? "1" : "0");
    }
    if (changes.filament_entangle_sen) {
        append(out, "FILAMENT_ENTANGLE_SEN", *changes.filament_entangle_sen);
    }
    if (!changes.end_unload_filament.empty()) {
        std::string csv;
        for (size_t i = 0; i < changes.end_unload_filament.size(); ++i) {
            csv += (i ? "," : "");
            csv += changes.end_unload_filament[i] ? "1" : "0";
        }
        append(out, "END_UNLOAD_FILAMENT", csv);
    }
    return out;
}

} // namespace helix::snapmaker
