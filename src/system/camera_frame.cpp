// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "camera_frame.h"

#include "http_executor.h"
#include "spdlog/spdlog.h"

#include <algorithm>
#include <cstring>

#if HELIX_HAS_CAMERA
#include "camera_stream.h"
#include "hv/requests.h"
#include "stb_image.h"
#endif

namespace helix {

CameraFrame downscale_bgr(const uint8_t* src, int w, int h, int stride, int max_w, int max_h) {
    CameraFrame out;
    if (!src || w <= 0 || h <= 0 || max_w <= 0 || max_h <= 0)
        return out;
    const double scale =
        std::min({1.0, static_cast<double>(max_w) / w, static_cast<double>(max_h) / h});
    out.w = std::max(1, static_cast<int>(w * scale));
    out.h = std::max(1, static_cast<int>(h * scale));
    out.bgr.resize(static_cast<size_t>(out.w) * out.h * 3);
    for (int y = 0; y < out.h; y++) {
        const uint8_t* row = src + static_cast<size_t>(y * h / out.h) * stride;
        uint8_t* dst = out.bgr.data() + static_cast<size_t>(y) * out.w * 3;
        for (int x = 0; x < out.w; x++)
            std::memcpy(dst + x * 3, row + static_cast<size_t>(x * w / out.w) * 3, 3);
    }
    return out;
}

lv_draw_buf_t* to_draw_buf(const CameraFrame& f) {
    if (f.empty())
        return nullptr;
    lv_draw_buf_t* buf = lv_draw_buf_create(f.w, f.h, LV_COLOR_FORMAT_RGB888, 0);
    if (!buf)
        return nullptr;
    const size_t row = static_cast<size_t>(f.w) * 3;
    for (int y = 0; y < f.h; y++)
        std::memcpy(static_cast<uint8_t*>(buf->data) + static_cast<size_t>(y) * buf->header.stride,
                    f.bgr.data() + y * row, row);
    return buf;
}

CameraFrame acquire_camera_frame(const CameraFrameSources& src, int max_w, int max_h,
                                 LifetimeToken token,
                                 std::function<void(CameraFrame)> on_late_frame) {
    if (src.stream_frame) {
        CameraFrame f = src.stream_frame(max_w, max_h);
        if (!f.empty())
            return f;
    }
    const std::string url = src.snapshot_url ? src.snapshot_url() : std::string();
    if (url.empty() || !src.fetch)
        return {};
    src.fetch(url, [token, max_w, max_h, cb = std::move(on_late_frame)](std::string body) mutable {
        if (body.empty())
            return;
        CameraFrame f = decode_jpeg_frame(body, max_w, max_h);
        if (f.empty())
            return;
        token.defer("CameraFrame::late_snapshot",
                    [cb = std::move(cb), f = std::move(f)]() mutable { cb(std::move(f)); });
    });
    return {};
}

#if HELIX_HAS_CAMERA

CameraFrame decode_jpeg_frame(const std::string& jpeg, int max_w, int max_h) {
    int w = 0, h = 0, channels = 0;
    uint8_t* rgb = stbi_load_from_memory(reinterpret_cast<const uint8_t*>(jpeg.data()),
                                         static_cast<int>(jpeg.size()), &w, &h, &channels, 3);
    if (!rgb) {
        spdlog::debug("[CameraFrame] snapshot decode failed: {}", stbi_failure_reason());
        return {};
    }
    CameraFrame f;
    if (w <= 4096 && h <= 4096) {
        // stb yields RGB; LVGL RGB888 is B,G,R in memory.
        for (size_t i = 0; i + 2 < static_cast<size_t>(w) * h * 3; i += 3)
            std::swap(rgb[i], rgb[i + 2]);
        f = downscale_bgr(rgb, w, h, w * 3, max_w, max_h);
    }
    stbi_image_free(rgb);
    return f;
}

CameraFrameSources live_camera_sources() {
    CameraFrameSources s;
    s.stream_frame = [](int max_w, int max_h) {
        return CameraStream::latest_running_frame(max_w, max_h);
    };
    s.snapshot_url = [] {
        auto feed = CameraStream::resolve_from_printer("");
        return feed ? feed->snapshot_url : std::string();
    };
    s.fetch = [](const std::string& url, std::function<void(std::string)> done) {
        helix::http::HttpExecutor::fast().submit([url, done = std::move(done)]() {
            auto resp = requests::get(url.c_str());
            if (resp && resp->status_code >= 200 && resp->status_code < 300)
                done(std::move(resp->body));
            else
                done({});
        });
    };
    return s;
}

#else

CameraFrame decode_jpeg_frame(const std::string&, int, int) {
    return {};
}

CameraFrameSources live_camera_sources() {
    return {};
}

#endif

} // namespace helix
