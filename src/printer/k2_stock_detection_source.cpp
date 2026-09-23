// SPDX-License-Identifier: GPL-3.0-or-later
// Ground truth, measured on a K2 Plus (.30.196) for #1378:
//   /usr/bin/detection <in.jpg> <out.jpg> 0   (mode 0 = the spaghetti model)
// runs the stock yolov5n on a file in ~4s. Positive output, one line per
// detection, each printed twice:
//   label: 1 prob: 0.903177 x:151.378 y:302.575 w:744.139 h:569.242
// Zero detections: no stdout, no output file, exit 0.
#include "k2_stock_detection_source.h"

#include "http_executor.h"
#include "hv/requests.h"
#include "i_moonraker_api.h"
#include "observer_factory.h"
#include "printer_detector.h"
#include "printer_state.h"
#include "settings_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <unistd.h>

#include "hv/json.hpp"

namespace helix::detection {

namespace {

// Stock paths on the K2 (Tina rootfs). Everything Creality-specific about this
// source lives in this file.
constexpr const char* DETECTION_BIN = "/usr/bin/detection";
constexpr const char* USER_PRINT_REFER =
    "/mnt/UDISK/creality/userdata/config/user_print_refer.json";
constexpr const char* SNAPSHOT_IN = "/tmp/helix_spaghetti_in.jpg";
constexpr const char* SNAPSHOT_OUT = "/tmp/helix_spaghetti_out.jpg";

bool fetch_snapshot_default(const std::string& url, const std::string& dest_path) {
    auto req = std::make_shared<HttpRequest>();
    req->method = HTTP_GET;
    req->url = url;
    req->timeout = 10;
    auto resp = requests::request(req);
    if (!resp || resp->status_code < 200 || resp->status_code >= 300 || resp->body.empty())
        return false;
    std::ofstream out(dest_path, std::ios::binary | std::ios::trunc);
    out.write(resp->body.data(), static_cast<std::streamsize>(resp->body.size()));
    return out.good();
}

int run_detection_default(const std::string& cmd, std::string& stdout_out) {
    // cmd is built entirely from fixed paths by this file; no shell metachars.
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe)
        return -1;
    char buf[512];
    while (fgets(buf, sizeof(buf), pipe.get()))
        stdout_out += buf;
    return pclose(pipe.release());
}

void poll_timer_trampoline(lv_timer_t* t) {
    static_cast<K2StockDetectionSource*>(lv_timer_get_user_data(t))->poll_tick();
}

} // namespace

std::optional<float> max_spaghetti_probability(const std::string& detection_stdout) {
    std::optional<float> best;
    std::istringstream lines(detection_stdout);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.find("label:") == std::string::npos)
            continue;
        std::istringstream tokens(line);
        std::string tok;
        while (tokens >> tok) {
            if (tok != "prob:")
                continue;
            double p = 0.0;
            if (tokens >> p) {
                if (!best || static_cast<float>(p) > *best)
                    best = static_cast<float>(p);
            }
            break;
        }
    }
    return best;
}

K2StockDetectionSource::K2StockDetectionSource(helix::PrinterState* state, IMoonrakerAPI* api)
    : state_(state), api_(api) {
    config_path_ = USER_PRINT_REFER;
    if (!fetcher_)
        fetcher_ = fetch_snapshot_default;
    if (!runner_)
        runner_ = run_detection_default;
    if (!submitter_)
        submitter_ = [](std::function<void()> work) {
            helix::http::HttpExecutor::fast().submit(std::move(work));
        };
}

void K2StockDetectionSource::start() {
    if (!state_)
        return;
    capable_ = PrinterDetector::is_creality_k2() && access(DETECTION_BIN, X_OK) == 0;

    // Thresholds and the pause choice: the ai_control block the stock stack
    // reads at startup. Parsed whether or not this machine is capable (the
    // file is absent everywhere but a Creality install, so the read is a
    // no-op elsewhere). Fall back to the factory values (25 s / 77.5 % /
    // pause) when the file is missing or malformed rather than refusing to
    // detect.
    std::ifstream f(config_path_);
    if (f) {
        try {
            const json j = json::parse(f);
            const auto& ai = j.at("ai_control");
            period_s_ = std::clamp(ai.value("pastaTime", period_s_), 5, 600);
            const double truth = ai.value("pastaTruth", 100.0 * threshold_);
            threshold_ = static_cast<float>(std::clamp(truth / 100.0, 0.05, 1.0));
            pause_on_detect_ = ai.value("pausePrint", 1) != 0;
        } catch (const std::exception& e) {
            spdlog::warn("[K2StockSource] unreadable ai_control ({}), using defaults", e.what());
        }
    }

    if (capable_) {
        spdlog::info("[K2StockSource] capable: poll every {} s at prob >= {:.3}, pause on "
                     "detect: {}",
                     period_s_, threshold_, pause_on_detect_ ? "yes" : "no");
    } else {
        spdlog::debug("[K2StockSource] not capable on this machine; poll ticks stay no-ops");
    }

    // Between jobs the detection context is new: a second print that starts
    // already failing must be able to fire even though the previous job ended
    // with last_positive_ latched.
    // RAW_PRINT_STATE_OK: the re-arm wants the job boundary itself — lifecycle's
    // Preparing/Idle distinction would leave the edge armed across pre-print.
    state_observer_ = helix::ui::observe_print_state<K2StockDetectionSource>(
        state_->get_print_state_enum_subject(), this,
        [](K2StockDetectionSource* self, PrintJobState value) { self->on_print_state(value); },
        state_->get_subjects_lifetime());

    poll_timer_.reset(lv_timer_create(poll_timer_trampoline, period_s_ * 1000, this));
}

void K2StockDetectionSource::on_print_state(PrintJobState state) {
    if (!printer_has_job(state))
        last_positive_ = false;
}

void K2StockDetectionSource::poll_tick() {
    if (!capable_ || busy_)
        return;
    if (!SettingsManager::instance().get_detection_enabled())
        return;
    // RAW_PRINT_STATE_OK: the stock daemon polls while the printer itself holds
    // a job; lifecycle would also poll through Preparing, before any plastic
    // has moved.
    if (!printer_has_job(state_->get_print_job_state()))
        return;

    busy_ = true;
    // The worker lambda holds by-value copies of everything it executes; the
    // raw `this` it captures is only ever stored into the deferred callback,
    // never dereferenced on the worker. The fetch/round can outlive this
    // object (HttpExecutor stop detaches stuck workers); the token re-checks
    // liveness on the main thread before handle_result runs.
    const auto fetch = fetcher_;
    const auto run = runner_;
    const auto url = snapshot_url_;
    const std::string cmd =
        std::string(DETECTION_BIN) + " " + SNAPSHOT_IN + " " + SNAPSHOT_OUT + " 0";
    const auto tok = lifetime_.token();
    const float threshold = threshold_;
    submitter_([this, fetch, run, url, cmd, tok, threshold] {
        PollResult r;
        if (fetch(url, SNAPSHOT_IN)) {
            std::string out;
            if (run(cmd, out) == 0) {
                if (const auto prob = max_spaghetti_probability(out)) {
                    r.ran = true;
                    r.prob = *prob;
                    r.positive = *prob >= threshold;
                }
            } else {
                spdlog::debug("[K2StockSource] detection exited nonzero");
            }
        } else {
            spdlog::debug("[K2StockSource] snapshot fetch failed");
        }
        spdlog::debug("[K2StockSource] poll done: ran={} positive={} prob={:.3}", r.ran, r.positive,
                      r.prob);
        // tok.defer marshals to the main thread and re-checks the generation
        // before the body runs, so `this` is valid inside handle_result.
        tok.defer("K2StockSource::handle_result", [this, r] { handle_result(r); });
    });
}

void K2StockDetectionSource::handle_result(const PollResult& r) {
    busy_ = false;
    if (r.positive && !last_positive_)
        fire(r);
    last_positive_ = r.positive;
}

void K2StockDetectionSource::fire(const PollResult& r) {
    const int pct = static_cast<int>(r.prob * 100.0f + 0.5f);
    spdlog::warn("[K2StockSource] spaghetti detected ({}%), {}", pct,
                 pause_on_detect_ ? "pausing print" : "pausePrint is off, notifying only");
    if (pause_on_detect_ && api_) {
        api_->job().pause_print([] { spdlog::info("[K2StockSource] print paused"); },
                                [](const MoonrakerError& err) {
                                    spdlog::warn("[K2StockSource] pause failed: {}", err.message);
                                });
    }

    DetectionEvent e;
    e.source_id = id();
    e.kind = DetectionKind::Spaghetti;
    e.attributable = true;
    e.confidence = r.prob;
    e.already_paused = false;
    e.message = "Spaghetti detected (" + std::to_string(pct) + "%)";
    if (cb_)
        cb_(e);
}

} // namespace helix::detection
