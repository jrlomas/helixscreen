// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_cfs.h"
#include "moonraker_client_mock.h"
#include "test_helpers/mock_personas.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;

namespace {

/// Captures notify_status_update frames and waits for one carrying `box` that
/// satisfies a predicate. Dispatch may run on the mock's own thread.
class BoxFrames {
  public:
    std::function<void(const json&)> callback() {
        return [this](const json& n) {
            std::lock_guard<std::mutex> lock(mutex_);
            frames_.push_back(n);
            cv_.notify_all();
        };
    }

    size_t mark() {
        std::lock_guard<std::mutex> lock(mutex_);
        return frames_.size();
    }

    /// The first frame carrying `box` captured after `after` that satisfies `pred`.
    std::optional<json> wait_for(size_t after, const std::function<bool(const json&)>& pred,
                                 int timeout_ms = 2000) {
        std::unique_lock<std::mutex> lock(mutex_);
        std::optional<json> hit;
        size_t next = after;
        cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
            for (; next < frames_.size(); ++next) {
                const auto& params = frames_[next]["params"];
                if (params.is_array() && !params.empty() && params[0].contains("box") &&
                    pred(params[0])) {
                    hit = params[0];
                    return true;
                }
            }
            return false;
        });
        return hit;
    }

  private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<json> frames_;
};

/// The unit-1 bay the frame's box reports loaded ("None" when none is).
std::string loaded_bay(const json& status) {
    return status["box"]["T1"].value("filament", "");
}

bool nozzle_sees_filament(const json& status) {
    const char* key = "filament_switch_sensor filament_sensor";
    return status.contains(key) && status[key].value("filament_detected", false);
}

} // namespace

TEST_CASE("The k2 persona's CR_BOX load and unload scripts move the box frame",
          "[mock][persona][k2][cfs]") {
    helix::test::PersonaEnv env("k2");
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::CREALITY_K2_PLUS);
    BoxFrames frames;
    mock.register_notify_update(frames.callback());

    using helix::printer::AmsBackendCfs;
    using helix::printer::CfsMacroVariant;

    size_t mark = frames.mark();
    mock.gcode_script(AmsBackendCfs::load_gcode(0, CfsMacroVariant::K2));
    CHECK(frames.wait_for(
        mark, [](const json& st) { return loaded_bay(st) == "A" && nozzle_sees_filament(st); }));

    mark = frames.mark();
    mock.gcode_script(AmsBackendCfs::unload_gcode(CfsMacroVariant::K2));
    CHECK(frames.wait_for(mark, [](const json& st) {
        return loaded_bay(st) == "None" && !nozzle_sees_filament(st);
    }));

    // A load of another bay reports that bay, not the first.
    mark = frames.mark();
    mock.gcode_script(AmsBackendCfs::load_gcode(2, CfsMacroVariant::K2));
    CHECK(frames.wait_for(mark, [](const json& st) { return loaded_bay(st) == "C"; }));
}

TEST_CASE("The k2 persona answers a box script only after its frames are out",
          "[mock][persona][k2][cfs]") {
    helix::test::PersonaEnv env("k2");
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::CREALITY_K2_PLUS);
    BoxFrames frames;
    mock.register_notify_update(frames.callback());
    mock.connect("ws://mock/websocket", [] {}, [] {});

    std::atomic<bool> acked{false};
    const size_t mark = frames.mark();
    mock.send_jsonrpc(
        "printer.gcode.script",
        {{"script",
          helix::printer::AmsBackendCfs::load_gcode(0, helix::printer::CfsMacroVariant::K2)}},
        [&acked](const json&) { acked = true; }, [](const MoonrakerError&) {});

    // The loaded bay is out before the answer, which a caller verifying the
    // outcome on completion depends on.
    CHECK_FALSE(acked.load());
    CHECK(frames.wait_for(mark, [](const json& st) { return loaded_bay(st) == "A"; }));
    for (int i = 0; i < 400 && !acked.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(acked.load());
    mock.disconnect();
}
