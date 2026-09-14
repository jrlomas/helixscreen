// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_close_keeps_tree.cpp
 * @brief Closing print status decides, at that moment, whether its tree survives
 *
 * On a low-memory host the print status widget tree is destroyed when the
 * overlay closes, to give back its ~400-800KB. While a job holds the machine
 * that trade is wrong: the user comes back to a preview rebuilt from nothing.
 * These cases open the real overlay through PrintStatusPanel::push_overlay(),
 * close it through NavigationManager::go_back() (animations off, so the close
 * callback runs inside the drain), and read what the panel kept.
 */

#include "ui_nav_manager.h"
#include "ui_panel_print_status.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/navigation_manager_test_access.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/print_status_panel_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "app_globals.h"
#include "display_settings_manager.h"
#include "memory_utils.h"
#include "printer_state.h"

#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <regex>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

MemoryInfo low_memory_host() {
    MemoryInfo info;
    info.total_kb = 128 * 1024;
    info.available_kb = 18 * 1024;
    info.free_kb = 4 * 1024;
    return info;
}

MemoryInfo roomy_host() {
    MemoryInfo info;
    info.total_kb = 4 * 1024 * 1024;
    info.available_kb = 3 * 1024 * 1024;
    info.free_kb = 2 * 1024 * 1024;
    return info;
}

/// Swaps a private ring-buffer logger into spdlog's default slot for its
/// lifetime, so the lines a case produces can be read back.
class LogCapture {
  public:
    LogCapture() : sink_(std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(kCapacity)) {
        sink_->set_level(spdlog::level::trace);
        auto logger = std::make_shared<spdlog::logger>("print_status_capture", sink_);
        logger->set_level(spdlog::level::trace);
        original_ = spdlog::default_logger();
        spdlog::set_default_logger(logger);
    }

    ~LogCapture() {
        spdlog::set_default_logger(original_);
    }

    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;

    [[nodiscard]] std::vector<std::string> lines() const {
        return sink_->last_formatted(kCapacity);
    }

  private:
    static constexpr size_t kCapacity = 4096;
    std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> sink_;
    std::shared_ptr<spdlog::logger> original_;
};

/// Home as the active main panel, animations off, and a low-memory host unless
/// a case says otherwise.
class PrintStatusCloseFixture : public LVGLUITestFixture {
  public:
    PrintStatusCloseFixture() {
        animations_were_enabled_ = DisplaySettingsManager::instance().get_animations_enabled();
        DisplaySettingsManager::instance().set_animations_enabled(false);

        home_widget_ = lv_obj_create(test_screen());
        lv_obj_t* panels[UI_PANEL_COUNT] = {nullptr};
        panels[static_cast<int>(PanelId::Home)] = home_widget_;
        NavigationManager::instance().set_panels(panels);

        use_memory(low_memory_host);
        set_wire_state(PrintJobState::STANDBY);
    }

    ~PrintStatusCloseFixture() override {
        // The cached tree is process-wide: leave none behind for the next case.
        NavigationManagerTestAccess::set_panel_stack(NavigationManager::instance(), {home_widget_});
        PrintStatusPanel::destroy_cached_overlay("test teardown");
        drain();
        process_lvgl(20);
        set_wire_state(PrintJobState::STANDBY);
        use_memory(get_system_memory_info);
        DisplaySettingsManager::instance().set_animations_enabled(animations_were_enabled_);
    }

    static void drain() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    static void set_wire_state(PrintJobState wire) {
        helix::test::set_wire_state(get_printer_state(), wire);
        drain();
    }

    static void use_memory(MemoryInfo (*source)()) {
        PrintStatusPanelTestAccess::set_memory_info_source(source);
    }

    /// Open print status over Home and settle.
    lv_obj_t* open_print_status() {
        REQUIRE(PrintStatusPanel::push_overlay(test_screen()));
        drain();
        lv_obj_t* tree = PrintStatusPanel::get_cached_overlay();
        REQUIRE(tree != nullptr);
        REQUIRE(NavigationManager::instance().is_panel_in_stack(tree));
        return tree;
    }

    /// Close it the way the back button does, then let the close callback and
    /// any deferred deletion run.
    void close_print_status(lv_obj_t* tree) {
        NavigationManager::instance().go_back();
        drain();
        process_lvgl(20);
        REQUIRE_FALSE(NavigationManager::instance().is_panel_in_stack(tree));
    }

  private:
    lv_obj_t* home_widget_ = nullptr;
    bool animations_were_enabled_ = true;
};

} // namespace

TEST_CASE_METHOD(PrintStatusCloseFixture,
                 "Print status keeps its tree when closed during an active print",
                 "[print_status][destroy_on_close]") {
    set_wire_state(PrintJobState::PRINTING);
    REQUIRE(get_printer_state().get_print_lifecycle() == PrintState::Printing);

    lv_obj_t* tree = open_print_status();
    close_print_status(tree);

    CHECK(PrintStatusPanel::get_cached_overlay() == tree);
    CHECK(lv_obj_is_valid(tree));
}

TEST_CASE_METHOD(PrintStatusCloseFixture, "Print status decides from the print state at close",
                 "[print_status][destroy_on_close]") {
    // Opened with no job, so a decision made when the tree was created would
    // destroy it on this close.
    lv_obj_t* tree = open_print_status();
    set_wire_state(PrintJobState::PRINTING);
    close_print_status(tree);
    REQUIRE(PrintStatusPanel::get_cached_overlay() == tree);

    // The kept tree comes back without re-creation, and its next close still
    // reaches the decision.
    REQUIRE(open_print_status() == tree);
    set_wire_state(PrintJobState::COMPLETE);
    close_print_status(tree);

    CHECK(PrintStatusPanel::get_cached_overlay() == nullptr);
    CHECK_FALSE(lv_obj_is_valid(tree));
}

TEST_CASE_METHOD(PrintStatusCloseFixture, "Print status decides from available memory at close",
                 "[print_status][destroy_on_close]") {
    set_wire_state(PrintJobState::COMPLETE);

    SECTION("memory runs low after the tree was created") {
        use_memory(roomy_host);
        lv_obj_t* tree = open_print_status();
        use_memory(low_memory_host);
        close_print_status(tree);

        CHECK(PrintStatusPanel::get_cached_overlay() == nullptr);
    }

    SECTION("memory frees up after the tree was created") {
        lv_obj_t* tree = open_print_status();
        use_memory(roomy_host);
        close_print_status(tree);

        CHECK(PrintStatusPanel::get_cached_overlay() == tree);
    }
}

TEST_CASE_METHOD(PrintStatusCloseFixture,
                 "A repeat print status tree logs WARN naming how the last one died",
                 "[print_status][destroy_on_close][logging]") {
    set_wire_state(PrintJobState::COMPLETE);
    LogCapture capture;

    lv_obj_t* first = open_print_status();
    close_print_status(first);
    REQUIRE(PrintStatusPanel::get_cached_overlay() == nullptr);
    open_print_status();

    // Tree numbers are process-wide, so read the first one back rather than
    // assume it.
    const std::regex destroyed(R"(\[info\] \[PrintStatusPanel\] Print status tree #(\d+) )"
                               R"(destroyed: overlay close while Complete)");
    int first_number = 0;
    for (const auto& line : capture.lines()) {
        std::smatch m;
        if (std::regex_search(line, m, destroyed)) {
            first_number = std::stoi(m[1].str());
        }
    }
    REQUIRE(first_number > 0);

    const std::string expected =
        "[warning] [PrintStatusPanel] Print status tree #" + std::to_string(first_number + 1) +
        " created while Complete (18MB available); tree #" + std::to_string(first_number) +
        " was destroyed: overlay close while Complete";
    int matches = 0;
    for (const auto& line : capture.lines()) {
        if (line.find(expected) != std::string::npos) {
            ++matches;
        }
    }
    CAPTURE(expected);
    CHECK(matches == 1);
}
