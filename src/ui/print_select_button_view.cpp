// src/ui/print_select_button_view.cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_select_button_view.h"

namespace helix::ui {

PrintSelectButtonView compute_print_select_button_view(const PrintSelectButtonInputs& in) {
    if (in.machine_busy) {
        if (in.job_queue_available && !in.macro_analysis_running) {
            return {PrintSelectButtonMode::Queue, ""};
        }
        return {PrintSelectButtonMode::Print, "Printing: start after this job"};
    }

    if (in.print_start_committed) {
        return {PrintSelectButtonMode::Print, "Printing: start after this job"};
    }

    if (in.macro_analysis_running) {
        return {PrintSelectButtonMode::Print, "Analyzing data..."};
    }

    return {PrintSelectButtonMode::Print, ""};
}

} // namespace helix::ui
