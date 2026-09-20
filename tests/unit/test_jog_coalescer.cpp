// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
// tests/unit/test_jog_coalescer.cpp
#include "jog_coalescer.h"

#include "../catch_amalgamated.hpp"

using helix::AxisMove;
using helix::JogCoalescer;

TEST_CASE("JogCoalescer: first tap sends immediately", "[jog_coalescer]") {
    JogCoalescer c;
    auto send = c.on_tap({1.0, 0.0, 0.0});
    REQUIRE(send.has_value());
    CHECK(send->dx == 1.0);
    CHECK(c.in_flight());
    CHECK(c.uncommitted_x() == 1.0); // in-flight counts as uncommitted
}

TEST_CASE("JogCoalescer: taps while in flight accumulate, ack flushes once", "[jog_coalescer]") {
    JogCoalescer c;
    REQUIRE(c.on_tap({1.0, 0.0, 0.0}).has_value());
    CHECK_FALSE(c.on_tap({1.0, 0.0, 0.0}).has_value());
    CHECK_FALSE(c.on_tap({1.0, 0.0, 0.0}).has_value());
    CHECK(c.uncommitted_x() == 3.0); // 1 in flight + 2 pending

    auto flush = c.on_ack();
    REQUIRE(flush.has_value());
    CHECK(flush->dx == 2.0); // both pending taps in ONE move
    CHECK(c.in_flight());

    auto done = c.on_ack();
    CHECK_FALSE(done.has_value()); // nothing pending -> idle
    CHECK_FALSE(c.in_flight());
    CHECK(c.uncommitted_x() == 0.0);
}

TEST_CASE("JogCoalescer: reversal cancels pending algebraically", "[jog_coalescer]") {
    JogCoalescer c;
    REQUIRE(c.on_tap({1.0, 0.0, 0.0}).has_value());
    c.on_tap({1.0, 0.0, 0.0});
    c.on_tap({-1.0, 0.0, 0.0});
    auto flush = c.on_ack();
    CHECK_FALSE(flush.has_value()); // +1 -1 pending nets to zero -> nothing to send
    CHECK_FALSE(c.in_flight());
}

TEST_CASE("JogCoalescer: multi-axis pending flushes as one move", "[jog_coalescer]") {
    JogCoalescer c;
    REQUIRE(c.on_tap({1.0, 0.0, 0.0}).has_value());
    c.on_tap({0.0, -2.0, 0.0});
    c.on_tap({0.0, 0.0, 0.5});
    auto flush = c.on_ack();
    REQUIRE(flush.has_value());
    CHECK(flush->dx == 0.0);
    CHECK(flush->dy == -2.0);
    CHECK(flush->dz == 0.5);
}

TEST_CASE("JogCoalescer: error drops pending and goes idle", "[jog_coalescer]") {
    JogCoalescer c;
    REQUIRE(c.on_tap({1.0, 0.0, 0.0}).has_value());
    c.on_tap({5.0, 0.0, 0.0});
    c.on_error();
    CHECK_FALSE(c.in_flight());
    CHECK(c.uncommitted_x() == 0.0);
    // Next tap sends immediately again
    CHECK(c.on_tap({1.0, 0.0, 0.0}).has_value());
}

TEST_CASE("JogCoalescer: reset clears everything", "[jog_coalescer]") {
    JogCoalescer c;
    c.on_tap({1.0, 0.0, 0.0});
    c.on_tap({2.0, 0.0, 0.0});
    c.reset();
    CHECK_FALSE(c.in_flight());
    CHECK(c.uncommitted_x() == 0.0);
}

TEST_CASE("JogCoalescer: float-residue reversal nets to idle, no residual flush",
          "[jog_coalescer]") {
    // 0.1 + 1.0 - 1.0 - 0.1 leaves ~1e-17 residue in double arithmetic. An exact
    // != 0.0 check would flush a near-null move (serialized in scientific notation);
    // the epsilon threshold in AxisMove::any() treats it as zero and goes idle.
    JogCoalescer c;
    REQUIRE(c.on_tap({1.0, 0.0, 0.0}).has_value()); // establish in-flight
    c.on_tap({0.1, 0.0, 0.0});
    c.on_tap({1.0, 0.0, 0.0});
    c.on_tap({-1.0, 0.0, 0.0});
    c.on_tap({-0.1, 0.0, 0.0});
    auto flush = c.on_ack();
    CHECK_FALSE(flush.has_value()); // residue below epsilon -> nothing to send
    CHECK_FALSE(c.in_flight());
}

TEST_CASE("clamp_jog_delta: clamps target to envelope", "[jog_coalescer]") {
    // current=195, nothing uncommitted, +10 would hit 205 with max 200 -> +5
    CHECK_THAT(helix::clamp_jog_delta(195.0, 0.0, 10.0, 0.0, 200.0),
               Catch::Matchers::WithinAbs(5.0, 1e-9));
    // Accounts for uncommitted travel: current=190, 5 uncommitted, +10 -> +5
    CHECK_THAT(helix::clamp_jog_delta(190.0, 5.0, 10.0, 0.0, 200.0),
               Catch::Matchers::WithinAbs(5.0, 1e-9));
    // Fully at edge -> 0
    CHECK(helix::clamp_jog_delta(200.0, 0.0, 1.0, 0.0, 200.0) == 0.0);
    // Never reverses direction even if predicted overshoots the envelope
    CHECK(helix::clamp_jog_delta(205.0, 0.0, 1.0, 0.0, 200.0) == 0.0);
    // Moves away from the edge pass through untouched
    CHECK_THAT(helix::clamp_jog_delta(200.0, 0.0, -1.0, 0.0, 200.0),
               Catch::Matchers::WithinAbs(-1.0, 1e-9));
}

TEST_CASE("clamp_jog_delta: clamps target to the min edge", "[jog_coalescer]") {
    // Mirror of the max-edge case above. Every assertion there passes min=0.0,
    // which never binds, so the min branch of std::clamp went unguarded.
    // current=5, nothing uncommitted, -10 would hit -5 with min 0 -> -5
    CHECK_THAT(helix::clamp_jog_delta(5.0, 0.0, -10.0, 0.0, 200.0),
               Catch::Matchers::WithinAbs(-5.0, 1e-9));
    // Accounts for uncommitted travel: current=10, -5 uncommitted, -10 -> -5
    CHECK_THAT(helix::clamp_jog_delta(10.0, -5.0, -10.0, 0.0, 200.0),
               Catch::Matchers::WithinAbs(-5.0, 1e-9));
    // Fully at edge -> 0
    CHECK(helix::clamp_jog_delta(0.0, 0.0, -1.0, 0.0, 200.0) == 0.0);
    // Never reverses direction even if predicted undershoots the envelope
    CHECK(helix::clamp_jog_delta(-5.0, 0.0, -1.0, 0.0, 200.0) == 0.0);
    // Moves away from the edge pass through untouched
    CHECK_THAT(helix::clamp_jog_delta(0.0, 0.0, 1.0, 0.0, 200.0),
               Catch::Matchers::WithinAbs(1.0, 1e-9));
}

TEST_CASE("clamp_jog_delta: honours a negative min envelope", "[jog_coalescer]") {
    // A min of 0.0 is indistinguishable from "no lower bound" for most sign
    // errors. Y axes routinely carry a negative soft limit (position_min: -5),
    // so the lower bound must be respected as an arbitrary value, not as zero.
    // predicted=0, -10 would hit -10 with min -5 -> -5
    CHECK_THAT(helix::clamp_jog_delta(0.0, 0.0, -10.0, -5.0, 200.0),
               Catch::Matchers::WithinAbs(-5.0, 1e-9));
    // Uncommitted travel already ate part of the envelope: predicted=-4, -1 left
    CHECK_THAT(helix::clamp_jog_delta(-3.0, -1.0, -10.0, -5.0, 200.0),
               Catch::Matchers::WithinAbs(-1.0, 1e-9));
    // Sitting exactly on the negative limit -> 0
    CHECK(helix::clamp_jog_delta(-5.0, 0.0, -1.0, -5.0, 200.0) == 0.0);
    // Past the negative limit -> 0, never a direction-reversing correction
    CHECK(helix::clamp_jog_delta(-7.0, 0.0, -1.0, -5.0, 200.0) == 0.0);
    // Moving back inside from the negative limit passes through untouched
    CHECK_THAT(helix::clamp_jog_delta(-5.0, 0.0, 2.0, -5.0, 200.0),
               Catch::Matchers::WithinAbs(2.0, 1e-9));
}

TEST_CASE("clamp_jog_with_warn: clamps at the axis maximum without warning", "[jog_coalescer]") {
    // At Z=248 on a 250mm axis, a +10 request may only travel 2. Partial travel
    // is not a block, so nothing is warned and the latch clears.
    const auto r = helix::clamp_jog_with_warn(248.0, 0.0, 10.0, 0.0, 250.0, false);
    REQUIRE(r.allowed == Catch::Approx(2.0));
    REQUIRE_FALSE(r.warn);
    REQUIRE_FALSE(r.latch);
}

TEST_CASE("clamp_jog_with_warn: clamps at the axis minimum", "[jog_coalescer]") {
    const auto r = helix::clamp_jog_with_warn(1.0, 0.0, -10.0, 0.0, 250.0, false);
    REQUIRE(r.allowed == Catch::Approx(-1.0));
    REQUIRE_FALSE(r.warn);
}

TEST_CASE("clamp_jog_with_warn: a fully blocked jog warns exactly once", "[jog_coalescer]") {
    // First attempt at the limit warns and sets the latch.
    const auto first = helix::clamp_jog_with_warn(250.0, 0.0, 10.0, 0.0, 250.0, false);
    REQUIRE(first.allowed == 0.0);
    REQUIRE(first.warn);
    REQUIRE(first.latch);

    // Second attempt, latch already set: still blocked, but silent. This is the
    // case hold-to-repeat turns into a toast flood without the latch.
    const auto second = helix::clamp_jog_with_warn(250.0, 0.0, 10.0, 0.0, 250.0, true);
    REQUIRE(second.allowed == 0.0);
    REQUIRE_FALSE(second.warn);
    REQUIRE(second.latch);
}

TEST_CASE("clamp_jog_with_warn: retreating clears the latch so the next approach warns",
          "[jog_coalescer]") {
    const auto away = helix::clamp_jog_with_warn(100.0, 0.0, -10.0, 0.0, 250.0, true);
    REQUIRE(away.allowed == Catch::Approx(-10.0));
    REQUIRE_FALSE(away.latch);

    const auto again = helix::clamp_jog_with_warn(250.0, 0.0, 10.0, 0.0, 250.0, away.latch);
    REQUIRE(again.warn);
}

TEST_CASE("clamp_jog_with_warn: accounts for an uncommitted pending move", "[jog_coalescer]") {
    // 240 now, 8mm already queued, so only 2 of a further 10 may go.
    const auto r = helix::clamp_jog_with_warn(240.0, 8.0, 10.0, 0.0, 250.0, false);
    REQUIRE(r.allowed == Catch::Approx(2.0));
}

TEST_CASE("clamp_jog_with_warn: a bed-moves printer clamps the gcode-space delta",
          "[jog_coalescer]") {
    // On a bed-moves printer the UP button produces gcode Z-. At Z=1 that must
    // clamp to -1. Passing the pre-inversion +10 would wrongly allow +2, which
    // is why MotionPanel clamps AFTER applying bed_moves_.
    const double gcode_delta = -10.0;
    const auto r = helix::clamp_jog_with_warn(1.0, 0.0, gcode_delta, 0.0, 250.0, false);
    REQUIRE(r.allowed == Catch::Approx(-1.0));
}
