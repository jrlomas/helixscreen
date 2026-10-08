// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lvgl/lvgl.h"
#include "subject_managed_panel.h"
#include "wizard_step.h"

namespace helix::wizard {

/// Which preference row groups the page offers. Global rows are written once, so
/// a printer added after the first run does not repeat them; the two AMS rows
/// are offered only where the backend makes them do something.
struct PreferenceRows {
    bool global = false;
    bool bypass_spool = false;
    bool keep_spool_info = false;
};

PreferenceRows preference_rows(bool is_subsequent_printer, bool bypass_is_virtual,
                               bool reports_spool_ids, bool retains_spool_info);

/// Wizard page of taste-only toggles. Each row is bound to the same subject and
/// callback as its settings-overlay twin, so there is no second copy of the state.
class WizardPreferencesStep : public Step {
  public:
    StepId id() const override {
        return StepId::Preferences;
    }
    const char* component_name() const override {
        return "wizard_preferences";
    }
    const char* log_name() const override {
        return "WizardPreferences";
    }

    WizardPreferencesStep() = default;
    WizardPreferencesStep(const WizardPreferencesStep&) = delete;
    WizardPreferencesStep& operator=(const WizardPreferencesStep&) = delete;

    void init_subjects() override;
    void register_callbacks() override;
    lv_obj_t* create(lv_obj_t* parent) override;
    void cleanup() override;

  private:
    lv_obj_t* root_ = nullptr;
    SubjectManager subjects_;
    lv_subject_t show_global_{};
    lv_subject_t show_bypass_spool_{};
    lv_subject_t show_keep_spool_info_{};
};

WizardPreferencesStep* get_wizard_preferences_step();

} // namespace helix::wizard
