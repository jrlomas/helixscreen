// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Real backend with gcode captured. client_ is null, so ensure_homed_then()
// routes straight to execute_gcode().

#include "ui_update_queue.h"

#include "ams_backend_toolchanger.h"
#include "ams_types.h"

#include <functional>
#include <string>
#include <vector>

#include "hv/json.hpp"

namespace helix::test {

using json = nlohmann::json;

class ToolChangerHelper : public AmsBackendToolChanger {
  public:
    explicit ToolChangerHelper(int tool_count) : AmsBackendToolChanger(nullptr, nullptr) {
        std::vector<std::string> names;
        for (int i = 0; i < tool_count; ++i) {
            names.push_back("T" + std::to_string(i));
        }
        set_discovered_tools(std::move(names));
        running_ = true;
    }

    ~ToolChangerHelper() override {
        helix::ui::UpdateQueue::instance().drain();
    }

    AmsError execute_gcode(const std::string& gcode) override {
        sent_.push_back(gcode);
        return AmsErrorHelper::success();
    }

    AmsError execute_gcode(const std::string& gcode, std::function<void()> on_complete) override {
        sent_.push_back(gcode);
        (void)on_complete;
        return AmsErrorHelper::success();
    }

    void feed(const json& status) {
        handle_status_update(
            json{{"method", "notify_status_update"}, {"params", json::array({status, 0.0})}});
    }

    void feed_status(const char* status, int tool_number) {
        feed(json{{"toolchanger", {{"status", status}, {"tool_number", tool_number}}}});
    }

    [[nodiscard]] const std::vector<std::string>& sent() const {
        return sent_;
    }

  private:
    std::vector<std::string> sent_;
};

} // namespace helix::test
