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

// filament_op_eligibility_reason() returns runtime-chosen strings, so the
// extractor cannot see them at the lv_tr() call site in dispatch(). This never
// runs; it lists every reason as a literal key.
// clang-format off
static void eligibility_reason_translation_hints_() {
    (void)lv_tr("empty"); (void)lv_tr("already loaded"); (void)lv_tr("not loaded");
    (void)lv_tr("feeder not in automatic mode"); (void)lv_tr("filament sensor disabled");
    (void)lv_tr("busy"); (void)lv_tr("feeder error");
}
// clang-format on

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

std::string BatchFilamentModal::row_label(LaneNoun noun, int slot, const SlotInfo& info,
                                          std::optional<bool> present, bool at_toolhead) {
    const std::string lane = lane_label(noun, slot);
    if (!info.material.empty()) {
        const char* where = at_toolhead ? lv_tr("loaded") : lv_tr("ready to load");
        return lane + " (" + info.material + " - " + where + ")"; // material: no i18n
    }
    if (present && !*present) {
        return lane + " (" + lv_tr("Empty") + ")";
    }
    return lane;
}

std::vector<int>
BatchFilamentModal::eligible_only(const std::vector<int>& selected,
                                  const std::vector<AmsBackend::FilamentOpEligibility>& per_slot) {
    std::vector<int> keep;
    keep.reserve(selected.size());
    for (int slot : selected) {
        const auto idx = static_cast<size_t>(slot);
        if (idx < per_slot.size() && per_slot[idx] == AmsBackend::FilamentOpEligibility::Eligible) {
            keep.push_back(slot);
        }
    }
    return keep;
}

BatchFilamentModal::BatchRowSource BatchFilamentModal::collect_rows(const AmsBackend& backend) {
    BatchRowSource rows;
    const int total = backend.get_system_info().total_slots;
    rows.slots.reserve(static_cast<size_t>(total));
    rows.lane_presence.reserve(static_cast<size_t>(total));
    rows.at_toolhead.reserve(static_cast<size_t>(total));
    for (int slot = 0; slot < total; ++slot) {
        rows.slots.push_back(backend.get_slot_info(slot));
        rows.lane_presence.push_back(slot_presence(rows.slots.back()));
        rows.at_toolhead.push_back(backend.can_unload_from_toolhead(slot));
    }
    return rows;
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

    const BatchRowSource rows = collect_rows(*backend);
    // One tick set serves both buttons, so it favors the direction with
    // a physical precondition: Unload on the heads that have filament at the
    // toolhead.
    const std::vector<bool> ticked = prefill_selection(rows.at_toolhead, /*for_load=*/false);

    std::vector<MultiSelectItem> items;
    items.reserve(rows.slots.size());
    for (size_t slot = 0; slot < rows.slots.size(); ++slot) {
        items.push_back(
            {std::to_string(slot),
             row_label(backend->lane_noun(), static_cast<int>(slot), rows.slots[slot],
                       rows.lane_presence[slot], rows.at_toolhead[slot].value_or(false)),
             ticked[slot]});
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

    std::vector<AmsBackend::FilamentOpEligibility> per_slot;
    const AmsSystemInfo sys = backend->get_system_info();
    per_slot.reserve(static_cast<size_t>(sys.total_slots));
    for (int slot = 0; slot < sys.total_slots; ++slot) {
        per_slot.push_back(backend->slot_op_eligibility(slot, load));
    }
    const std::vector<int> runnable = eligible_only(slots, per_slot);
    if (runnable.size() != slots.size()) {
        // Name the first head we are dropping and why; a batch that silently
        // shrinks is worse than one that explains itself.
        for (int slot : slots) {
            const auto e = per_slot[static_cast<size_t>(slot)];
            if (e != AmsBackend::FilamentOpEligibility::Eligible) {
                NOTIFY_WARNING("{} {}: {}", lv_tr("Skipped"),
                               lane_label(backend->lane_noun(), slot),
                               lv_tr(filament_op_eligibility_reason(e)));
                break;
            }
        }
    }
    if (runnable.empty()) {
        return; // keep the picker open — nothing was dispatched
    }

    spdlog::info("[BatchFilamentModal] {} batch on {} slot(s)", load ? "Load" : "Unload",
                 runnable.size());
    AmsError error =
        load ? backend->load_filament_batch(runnable) : backend->unload_filament_batch(runnable);
    if (!error.success()) {
        helix::ui::notify_ams_error(error, load ? lv_tr("Batch load failed")
                                                : lv_tr("Batch unload failed"));
    }
    hide();
}

} // namespace helix::ui
