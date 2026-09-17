// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file streaming_policy.h
 * @brief Centralized policy for deciding when to use streaming operations
 *
 * This singleton provides a single source of truth for file size thresholds
 * that determine whether operations should use streaming (disk-based) or
 * buffered (in-memory) approaches.
 *
 * The policy is critical for preventing memory exhaustion on embedded devices
 * like the AD5M (~108MB RAM) when handling large G-code files (10-100MB+).
 *
 * Usage:
 *   if (StreamingPolicy::instance().should_stream(file_size)) {
 *       // Use streaming path (download to disk, modify file-to-file, etc.)
 *   } else {
 *       // Small file - in-memory operations are acceptable
 *   }
 *
 * Configuration (settings.json):
 *   {
 *     "streaming": {
 *       "threshold_bytes": 0,      // 0 = auto-detect from RAM
 *       "force_streaming": false   // true = always stream regardless of size
 *     }
 *   }
 */

#pragma once

#include <atomic>
#include <cstddef>

namespace helix {

/**
 * @brief Centralized streaming decision policy
 *
 * Singleton that determines when file operations should use streaming
 * (disk-based) instead of buffered (in-memory) approaches.
 *
 * Thread-safe for read operations. Configuration should be set at startup.
 */
class StreamingPolicy {
  public:
    /**
     * @brief Get the singleton instance
     */
    static StreamingPolicy& instance();

    /**
     * @brief Determine if an operation should use streaming for a given file size
     *
     * This is the main decision point. All code that handles potentially large
     * files should call this method to determine the appropriate approach.
     *
     * @param file_size_bytes Size of the file in bytes
     * @return true if streaming should be used, false if buffered is acceptable
     */
    bool should_stream(size_t file_size_bytes) const;

    /**
     * @brief Get the current threshold in bytes
     *
     * If threshold is 0, auto-detection is used based on available RAM.
     *
     * @return Current threshold in bytes (may be computed dynamically)
     */
    size_t get_threshold_bytes() const;

    /**
     * @brief Set threshold override from config
     *
     * @param bytes Threshold in bytes. 0 = auto-detect from RAM.
     */
    void set_threshold_bytes(size_t bytes);

    /**
     * @brief Force streaming for all operations regardless of size
     *
     * Useful for testing or memory-constrained deployments.
     *
     * @param force true to always use streaming
     */
    void set_force_streaming(bool force);

    /**
     * @brief Check if force streaming is enabled
     */
    bool is_force_streaming() const {
        return force_streaming_.load();
    }

    /**
     * @brief Calculate auto-detected threshold based on available RAM
     *
     * Uses a percentage of available RAM as the threshold, bounded by
     * MIN_THRESHOLD and MAX_THRESHOLD.
     *
     * @return Calculated threshold in bytes
     */
    size_t auto_detect_threshold() const;

    /**
     * @brief Log current policy settings (at DEBUG level)
     */
    void log_settings() const;

    /**
     * @brief Load settings from config file and environment variables
     *
     * Call this after Config::init() to apply user settings.
     * Priority: ENV var > config file > auto-detect
     *
     * Environment variables:
     *   - HELIX_FORCE_STREAMING=1 - Force streaming for all file operations
     *
     * Config file (settings.json):
     *   - /streaming/force_streaming: bool - Force streaming mode
     *   - /streaming/threshold_mb: int - Threshold in MB (0 = auto-detect)
     */
    void load_from_config();

    // Threshold constants
    static constexpr double RAM_THRESHOLD_PERCENT = 0.10;          // 10% of available RAM
    static constexpr size_t MIN_THRESHOLD = 5 * 1024 * 1024;       // 5MB floor
    static constexpr size_t MAX_THRESHOLD = 100 * 1024 * 1024;     // 100MB ceiling
    static constexpr size_t FALLBACK_THRESHOLD = 10 * 1024 * 1024; // 10MB if can't read RAM

  private:
    StreamingPolicy() = default;
    ~StreamingPolicy() = default;

    StreamingPolicy(const StreamingPolicy&) = delete;
    StreamingPolicy& operator=(const StreamingPolicy&) = delete;

    std::atomic<size_t> threshold_bytes_{0}; // 0 = auto-detect
    std::atomic<bool> force_streaming_{false};
};

} // namespace helix
