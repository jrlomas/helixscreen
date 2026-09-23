// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_observer_guard.h"
#include "ui_timer_guard.h"

#include "async_lifetime_guard.h"
#include "detection_source.h"
#include "i_moonraker_api.h"

#include <functional>
#include <optional>
#include <string>

namespace helix {
class PrinterState;
enum class PrintJobState;
} // namespace helix

namespace helix::detection {

/// Highest spaghetti probability in /usr/bin/detection stdout, or nullopt when
/// the output holds no parsable detection. Measured format (one line per
/// detection, printed twice):
///   label: 1 prob: 0.903177 x:151.378 y:302.575 w:744.139 h:569.242
/// With zero detections the tool prints nothing and exits 0.
std::optional<float> max_spaghetti_probability(const std::string& detection_stdout);

/**
 * @brief Detection source backed by the Creality K2 stock yolov5n model.
 *
 * The stock loop (master-server -> cam_app -> ai_engine) needs exclusive
 * /dev/video0 and cannot coexist with our ustreamer. Instead, while a print is
 * active this source fetches a JPEG from the local ustreamer snapshot endpoint
 * and runs /usr/bin/detection on it, off the main thread, every pastaTime
 * seconds. A probability at or above pastaTruth/100 (user_print_refer.json
 * ai_control block) is a spaghetti detection: the source pauses the print and
 * emits the event, edge-triggered so a persistent failure fires once.
 */
class K2StockDetectionSource : public DetectionSource {
  public:
    /// Fetch a snapshot JPEG from @p url into @p dest_path. Blocking.
    using SnapshotFetcher =
        std::function<bool(const std::string& url, const std::string& dest_path)>;
    /// Run @p cmd, write its stdout into @p stdout_out, return its exit code. Blocking.
    using DetectionRunner = std::function<int(const std::string& cmd, std::string& stdout_out)>;
    /// Schedule blocking work off the main thread.
    using WorkSubmitter = std::function<void(std::function<void()>)>;

    K2StockDetectionSource(helix::PrinterState* state, IMoonrakerAPI* api);

    /// Unique registration key. Exposed so DetectionManager can match on id()
    /// and static_cast instead of dynamic_cast — the firmware builds -fno-rtti.
    static constexpr const char* SOURCE_ID = "k2_stock";

    std::string id() const override {
        return SOURCE_ID;
    }
    bool available() const override {
        return capable_;
    }
    void set_callback(Callback cb) override {
        cb_ = std::move(cb);
    }

    /// Probe capability (Creality K2 + /usr/bin/detection present), read the
    /// ai_control thresholds and start the poll timer. Called once.
    void start();

    /// Test seams (the defaults do real HTTP / popen / HttpExecutor).
    void set_fetcher(SnapshotFetcher f) {
        fetcher_ = std::move(f);
    }
    void set_runner(DetectionRunner r) {
        runner_ = std::move(r);
    }
    void set_submitter(WorkSubmitter s) {
        submitter_ = std::move(s);
    }
    /// Test seam: force the capability start() would have probed.
    void set_capable_for_test(bool v) {
        capable_ = v;
    }

    /// One poll decision: gates, then one background fetch+infer round.
    /// Public because the test pump only runs one-shot timers, so tests drive
    /// ticks directly instead of waiting on the periodic timer.
    void poll_tick();

  private:
    struct PollResult {
        bool ran = false;      ///< a full fetch+infer round completed
        bool positive = false; ///< detection at or above threshold
        float prob = 0.0f;
    };

    void on_print_state(PrintJobState state);
    void handle_result(const PollResult& r);
    void fire(const PollResult& r);

    helix::PrinterState* state_ = nullptr;
    IMoonrakerAPI* api_ = nullptr;
    Callback cb_;

    bool capable_ = false;
    bool busy_ = false;          ///< a poll round is in flight (main thread only)
    bool last_positive_ = false; ///< edge-trigger: fire only on negative->positive
    float threshold_ = 0.775f;   ///< pastaTruth / 100
    int period_s_ = 25;          ///< pastaTime
    std::string snapshot_url_ = "http://127.0.0.1:8080/snapshot";

    SnapshotFetcher fetcher_;
    DetectionRunner runner_;
    WorkSubmitter submitter_;

    helix::ui::LvglTimerGuard poll_timer_;
    ObserverGuard state_observer_;
    AsyncLifetimeGuard lifetime_;
};

} // namespace helix::detection
