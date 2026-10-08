// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_camera_frame.cpp
 * @brief Where a still of the camera comes from: a running stream's frame, else
 *        one snapshot fetched off-thread, else nothing; and that a snapshot
 *        landing after its owner is gone is dropped.
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "async_lifetime_guard.h"
#include "camera_frame.h"
#include "camera_stream.h"

#include <fstream>
#include <iterator>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

CameraFrame solid(int w, int h) {
    CameraFrame f;
    f.w = w;
    f.h = h;
    f.bgr.assign(static_cast<size_t>(w) * h * 3, 0x7f);
    return f;
}

std::string benchy_jpeg() {
    std::ifstream in("assets/test_timelapse/benchy_timelapse_20260310.thumb.jpg", std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

struct FakeCamera {
    CameraFrame stream;
    std::string url;
    std::function<CameraFrame(CameraFrame)> adjust;
    int fetches = 0;
    std::function<void(std::string)> pending;

    CameraFrameSources sources() {
        CameraFrameSources s;
        s.stream_frame = [this](int, int) { return stream; };
        s.snapshot = [this] {
            SnapshotTarget t;
            t.url = url;
            t.adjust = adjust;
            return t;
        };
        s.fetch = [this](const std::string&, std::function<void(std::string)> done) {
            ++fetches;
            pending = std::move(done);
        };
        return s;
    }
};

} // namespace

TEST_CASE("downscale_bgr fits the box, keeps aspect, never enlarges", "[camera_frame]") {
    auto big = solid(640, 360);
    auto f = downscale_bgr(big.bgr.data(), 640, 360, 640 * 3, 320, 320);
    CHECK(f.w == 320);
    CHECK(f.h == 180);
    CHECK(f.bgr.size() == 320u * 180u * 3u);

    auto small = solid(100, 50);
    auto g = downscale_bgr(small.bgr.data(), 100, 50, 100 * 3, 320, 320);
    CHECK(g.w == 100);
    CHECK(g.h == 50);
}

TEST_CASE_METHOD(LVGLTestFixture, "acquire_camera_frame picks stream, snapshot, or nothing",
                 "[camera_frame]") {
    AsyncLifetimeGuard owner;
    int late = 0;
    auto on_late = [&](CameraFrame f) {
        CHECK_FALSE(f.empty());
        ++late;
    };

    SECTION("a running stream wins; no fetch is started") {
        FakeCamera cam;
        cam.stream = solid(40, 30);
        cam.url = "http://cam/snapshot";
        auto f = acquire_camera_frame(cam.sources(), 320, 144, owner.token(), on_late);
        CHECK(f.w == 40);
        CHECK(cam.fetches == 0);
    }

    SECTION("no stream: one snapshot is fetched and lands on the main thread") {
        auto jpeg = benchy_jpeg();
        REQUIRE_FALSE(jpeg.empty());
        FakeCamera cam;
        cam.url = "http://cam/snapshot";
        auto f = acquire_camera_frame(cam.sources(), 100, 100, owner.token(), on_late);
        CHECK(f.empty());
        REQUIRE(cam.fetches == 1);
        cam.pending(jpeg);
        CHECK(late == 0); // not until the main thread drains
        helix::ui::UpdateQueue::instance().drain();
        CHECK(late == 1);
    }

    SECTION("the snapshot's rotation/flip is applied before it is shown") {
        auto jpeg = benchy_jpeg();
        FakeCamera cam;
        cam.url = "http://cam/snapshot";
        cam.adjust = [](CameraFrame f) {
            return CameraStream::transform_frame(f, {CameraRotation::Rotate90, false, false});
        };
        int w = 0, h = 0;
        acquire_camera_frame(cam.sources(), 100, 100, owner.token(), [&](CameraFrame f) {
            w = f.w;
            h = f.h;
        });
        cam.pending(jpeg);
        helix::ui::UpdateQueue::instance().drain();
        auto plain = decode_jpeg_frame(jpeg, 100, 100);
        CHECK(w == plain.h);
        CHECK(h == plain.w);
    }

    SECTION("a failed fetch delivers nothing") {
        FakeCamera cam;
        cam.url = "http://cam/snapshot";
        acquire_camera_frame(cam.sources(), 100, 100, owner.token(), on_late);
        cam.pending({});
        helix::ui::UpdateQueue::instance().drain();
        CHECK(late == 0);
    }

    SECTION("a snapshot landing after its owner closed is dropped") {
        auto jpeg = benchy_jpeg();
        FakeCamera cam;
        cam.url = "http://cam/snapshot";
        acquire_camera_frame(cam.sources(), 100, 100, owner.token(), on_late);
        owner.invalidate();
        cam.pending(jpeg);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(late == 0);
    }

    SECTION("no camera: text only, nothing fetched") {
        FakeCamera cam;
        auto f = acquire_camera_frame(cam.sources(), 320, 144, owner.token(), on_late);
        CHECK(f.empty());
        CHECK(cam.fetches == 0);
    }
}

TEST_CASE("transform_from_config XORs the user's flip with Moonraker's", "[camera_frame]") {
    WebcamInfo feed;
    feed.flip_horizontal = true;
    nlohmann::json cfg = {{"rotation", 270}, {"flip_h", true}, {"flip_v", true}};
    auto t = CameraStream::transform_from_config(cfg, feed);
    CHECK(t.rotation == CameraRotation::Rotate270);
    CHECK_FALSE(t.flip_h); // Moonraker flips, user flips: net none
    CHECK(t.flip_v);
}

TEST_CASE("transform_frame matches the pixel rule: 180 flips both axes", "[camera_frame]") {
    CameraFrame f;
    f.w = 2;
    f.h = 1;
    f.bgr = {1, 1, 1, 2, 2, 2};
    auto g = CameraStream::transform_frame(f, {CameraRotation::Rotate180, false, false});
    REQUIRE(g.w == 2);
    CHECK(g.bgr[0] == 2);
    CHECK(g.bgr[3] == 1);
}
