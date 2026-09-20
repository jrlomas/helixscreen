// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_modal.h"
#include "ui_multiselect.h"

#include <optional>
#include <string>
#include <vector>

namespace helix::ui {

/**
 * @brief Picker modal for batch filament load/unload
 *
 * Shown on backends whose toolheads feed independently
 * (AmsBackend::supports_batch_filament_ops()): one row per slot, and either
 * button dispatches the whole ticked set as one firmware script.
 */
class BatchFilamentModal : public Modal {
  public:
    /// One-shot owned show: create, populate from the active backend, and hand
    /// the instance to ModalStack, which frees it when its entry goes.
    static bool show_owned();

    const char* get_name() const override {
        return "Batch Filament";
    }
    const char* component_name() const override {
        return "batch_filament_modal";
    }

    // Pure: which rows start ticked. for_load ticks the slots WITHOUT filament
    // at the toolhead; !for_load ticks the ones with it. nullopt (backend
    // publishes no presence for the lane) counts as loadable, never as
    // unloadable — the backend refuses an empty lane, not the picker.
    static std::vector<bool> prefill_selection(const std::vector<std::optional<bool>>& at_toolhead,
                                               bool for_load);

    // Pure: multiselect keys are slot indices as decimal strings.
    static std::vector<int> selected_slots(const std::vector<std::string>& keys);

  protected:
    void on_show() override;

    /// btn_primary (Load). The ok/cancel hooks are just which row button they
    /// wire; both dispatch and hide.
    void on_ok() override;
    /// btn_secondary (Unload)
    void on_cancel() override;

  private:
    void dispatch(bool load);

    UiMultiselect multiselect_;
};

} // namespace helix::ui
