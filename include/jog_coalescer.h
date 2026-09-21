// SPDX-License-Identifier: GPL-3.0-or-later
// include/jog_coalescer.h
#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

namespace helix {

/** Relative multi-axis jog delta in mm. */
struct AxisMove {
    double dx = 0.0;
    double dy = 0.0;
    double dz = 0.0;
    // Mixed-magnitude float cancellation can leave ~1e-17 residue in an
    // accumulated delta; treat anything below this as zero so a near-null move
    // (which would serialize in scientific notation) never flushes.
    static constexpr double EPSILON_MM = 1e-6;
    bool any() const {
        return std::abs(dx) > EPSILON_MM || std::abs(dy) > EPSILON_MM || std::abs(dz) > EPSILON_MM;
    }
};

/**
 * Serializes jog moves: one RPC in flight, further taps accumulate
 * algebraically into pending deltas and flush as ONE combined move when the
 * in-flight move acks. Main-thread only — callers marshal acks/errors onto
 * the UI thread before touching this.
 */
class JogCoalescer {
  public:
    /** Tap arrived. Returns the move to send NOW if idle; nullopt if it was
     *  accumulated behind the in-flight move. */
    std::optional<AxisMove> on_tap(const AxisMove& delta) {
        if (in_flight_) {
            pending_.dx += delta.dx;
            pending_.dy += delta.dy;
            pending_.dz += delta.dz;
            return std::nullopt;
        }
        in_flight_ = true;
        inflight_ = delta;
        return delta;
    }

    /** In-flight move acked. Returns the pending flush to send (stays in
     *  flight) or nullopt (now idle). */
    std::optional<AxisMove> on_ack() {
        if (pending_.any()) {
            inflight_ = pending_;
            pending_ = {};
            return inflight_;
        }
        in_flight_ = false;
        inflight_ = {};
        return std::nullopt;
    }

    /** In-flight move failed: drop pending, go idle. */
    void on_error() {
        reset();
    }

    /** Drop all state (panel deactivate, print start, UI teardown). */
    void reset() {
        in_flight_ = false;
        inflight_ = {};
        pending_ = {};
    }

    bool in_flight() const {
        return in_flight_;
    }

    /** Travel not yet reflected in the position subjects: in-flight + pending.
     *  Used to predict position for envelope clamping. */
    double uncommitted_x() const {
        return inflight_.dx + pending_.dx;
    }
    double uncommitted_y() const {
        return inflight_.dy + pending_.dy;
    }
    double uncommitted_z() const {
        return inflight_.dz + pending_.dz;
    }

  private:
    bool in_flight_ = false;
    AxisMove inflight_{};
    AxisMove pending_{};
};

/**
 * Clamp a jog delta so predicted-position + delta stays inside [min, max].
 * Returns 0 rather than a direction-reversing correction when the predicted
 * position is already at/past the edge in the tap direction.
 */
inline double clamp_jog_delta(double current, double uncommitted, double delta, double min,
                              double max) {
    const double predicted = current + uncommitted;
    const double clamped = std::clamp(predicted + delta, min, max) - predicted;
    if (clamped * delta <= 0.0) {
        return 0.0;
    }
    return clamped;
}

/// Outcome of clamping one jog delta against an axis's limits.
struct JogClampResult {
    double allowed; ///< Permitted delta; 0.0 when the axis cannot move at all.
    bool warn;      ///< Raise the "blocked" message now.
    bool latch;     ///< The caller's new per-axis latch value; store it unconditionally.
};

/// Clamp a jog delta against an axis's limits and decide whether this attempt
/// is the first blocked one of an approach.
///
/// `already_warned` is the caller's latch for this axis. `latch` in the result
/// is its new value and is always meaningful, so a caller assigns it without
/// branching: a jog that moves clears the latch, so retreating from a limit and
/// returning to it warns again.
///
/// Partial travel is not a block. A request for 10mm that yields 2mm moves 2mm
/// and says nothing; only a request that yields nothing is worth a message.
inline JogClampResult clamp_jog_with_warn(double current, double uncommitted, double delta,
                                          double min, double max, bool already_warned) {
    const double allowed = clamp_jog_delta(current, uncommitted, delta, min, max);
    const bool blocked = std::abs(allowed) <= AxisMove::EPSILON_MM;
    if (!blocked) {
        return {allowed, false, false};
    }
    return {0.0, !already_warned, true};
}

/// The jog feedrate actually used, given what the user stored and what the
/// printer permits. Storage keeps the user's choice so a machine with a higher
/// ceiling gets it back; emission can never exceed the limit. Bounds are plain
/// doubles so this header stays dependency-free — pass a SafetyLimits' fields.
inline int effective_jog_speed_mm_min(int stored_mm_min, double min_mm_min, double max_mm_min) {
    // min-then-max rather than std::clamp: the bounds are caller-supplied, and
    // std::clamp is undefined when min > max while this form resolves any
    // ordering to the maximum.
    const double v = static_cast<double>(stored_mm_min);
    return static_cast<int>(std::min(std::max(v, min_mm_min), max_mm_min));
}

} // namespace helix
