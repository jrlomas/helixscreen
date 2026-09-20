// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_batch_filament_modal.h"

#include "ui_error_reporting.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "ams_types.h"
#include "display_numbering.h"
#include "filament_op_slot_resolver.h"
#include "lvgl/src/others/translation/lv_translation.h"

#include <spdlog/spdlog.h>

#include <memory>

namespace helix::ui {

bool BatchFilamentModal::show_owned() {
    auto modal = std::make_unique<BatchFilamentModal>();
    if (!modal->show(lv_screen_active())) {
        return false; // the unique_ptr frees the never-shown instance
    }
    ModalStack::instance().assume_ownership(modal->backdrop(), std::move(modal));
    return true;
}

std::vector<bool>
BatchFilamentModal::prefill_selection(const std::vector<std::optional<bool>>& at_toolhead,
                                      bool for_load) {
    std::vector<bool> ticked;
    ticked.reserve(at_toolhead.size());
    for (const auto& present : at_toolhead) {
        ticked.push_back(for_load ? !(present && *present) : (present && *present));
    }
    return ticked;
}

std::vector<int> BatchFilamentModal::selected_slots(const std::vector<std::string>& keys) {
    std::vector<int> slots;
    slots.reserve(keys.size());
    for (const auto& key : keys) {
        slots.push_back(std::stoi(key));
    }
    return slots;
}

void BatchFilamentModal::on_show() {
    wire_ok_button("btn_primary");
    wire_cancel_button("btn_secondary");
    // Both action buttons dispatch, so Cancel is the only way out that does
    // nothing. Modal::on_tertiary() already just hides.
    wire_tertiary_button("btn_tertiary");

    AmsBackend* backend = AmsState::instance().get_backend();
    lv_obj_t* container = find_widget("batch_multiselect");
    if (!backend || !container) {
        spdlog::warn("[BatchFilamentModal] {} — picker left empty",
                     backend ? "batch_multiselect not found" : "no backend");
        return;
    }

    const AmsSystemInfo info = backend->get_system_info();
    std::vector<std::optional<bool>> at_toolhead;
    std::vector<MultiSelectItem> items;
    at_toolhead.reserve(static_cast<size_t>(info.total_slots));
    items.reserve(static_cast<size_t>(info.total_slots));
    for (int slot = 0; slot < info.total_slots; ++slot) {
        at_toolhead.push_back(slot_presence(backend->get_slot_info(slot)));
    }
    // One tick set serves both buttons, so it favors the direction with
    // a physical precondition: Unload on the heads that have filament at the
    // toolhead.
    const std::vector<bool> ticked = prefill_selection(at_toolhead, /*for_load=*/false);
    for (int slot = 0; slot < info.total_slots; ++slot) {
        items.push_back({std::to_string(slot), lane_label(backend->lane_noun(), slot),
                         ticked[static_cast<size_t>(slot)]});
    }
    multiselect_.attach(container);
    multiselect_.set_items(items);
    spdlog::debug("[BatchFilamentModal] {} row(s) populated", items.size());
}

void BatchFilamentModal::on_ok() {
    dispatch(/*load=*/true);
}

void BatchFilamentModal::on_cancel() {
    dispatch(/*load=*/false);
}

void BatchFilamentModal::dispatch(bool load) {
    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        NOTIFY_WARNING("{}", lv_tr("Multi-Filament System not available"));
        hide();
        return;
    }

    const std::vector<int> slots = selected_slots(multiselect_.get_selected_keys());
    if (slots.empty()) {
        NOTIFY_WARNING("{}", lv_tr("Select at least one"));
        return; // keep the picker open — nothing was dispatched
    }

    spdlog::info("[BatchFilamentModal] {} batch on {} slot(s)", load ? "Load" : "Unload",
                 slots.size());
    AmsError error =
        load ? backend->load_filament_batch(slots) : backend->unload_filament_batch(slots);
    if (!error.success()) {
        helix::ui::notify_ams_error(error, load ? lv_tr("Batch load failed")
                                                : lv_tr("Batch unload failed"));
    }
    hide();
}

} // namespace helix::ui
