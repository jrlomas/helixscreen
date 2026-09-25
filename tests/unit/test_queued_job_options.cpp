// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_queued_job_options.cpp
 * @brief Pure encode/decode/prune rules for the per-job queue option store,
 * plus the automatic_transition parse off Moonraker's server.config.
 *
 * Run with: ./build/bin/helix-tests "[job_queue][options]"
 *
 * No fixture: everything here is a free function over json. The wiring that
 * pushes these values through the Moonraker database and the queue refresh
 * is covered separately in test_queued_job_options_store.cpp.
 */

#include "job_queue_state.h"
#include "json_fwd.h"
#include "queued_job_options.h"

#include "../catch_amalgamated.hpp"

using helix::queue::QueuedJobOptions;
using helix::queue::QueuedJobOptionsMap;

namespace {

QueuedJobOptionsMap sample_map() {
    QueuedJobOptionsMap m;
    m["0001"] = QueuedJobOptions{"benchy_v2.gcode", {{"skip_beam", true}, {"soak", false}}};
    m["0002"] = QueuedJobOptions{"cube.gcode", {{"skip_beam", false}}};
    return m;
}

} // namespace

TEST_CASE("queued job options encode/decode round trip", "[job_queue][options]") {
    const auto original = sample_map();

    const auto decoded =
        helix::queue::decode_queued_job_options(helix::queue::encode_queued_job_options(original));

    REQUIRE(decoded.size() == original.size());
    CHECK(decoded.at("0001").filename == "benchy_v2.gcode");
    CHECK(decoded.at("0001").options.at("skip_beam") == true);
    CHECK(decoded.at("0001").options.at("soak") == false);
    CHECK(decoded.at("0002").options.at("skip_beam") == false);

    // A second round trip through the same codec must be stable.
    const auto re_encoded = helix::queue::encode_queued_job_options(decoded);
    CHECK(helix::queue::decode_queued_job_options(re_encoded) == decoded);
}

TEST_CASE("queued job options malformed value decodes to empty", "[job_queue][options]") {
    // Not an object at all
    CHECK(helix::queue::decode_queued_job_options(json::array({"0001"})).empty());
    CHECK(helix::queue::decode_queued_job_options(json("garbage")).empty());
    CHECK(helix::queue::decode_queued_job_options(json(nullptr)).empty());
    CHECK(helix::queue::decode_queued_job_options(json::object()).empty());

    // Object shape but wrong entry types: bad rows are skipped, good ones kept
    json mixed;
    mixed["good"] = {{"filename", "a.gcode"}, {"options", {{"x", true}}}};
    mixed["not_an_object"] = "oops";
    mixed["no_filename_key"] = {{"options", json::object()}};
    mixed["options_not_object"] = {{"filename", "b.gcode"}, {"options", 7}};
    mixed["option_not_bool"] = {{"filename", "c.gcode"}, {"options", {{"y", "yes"}}}};
    const auto decoded = helix::queue::decode_queued_job_options(mixed);
    REQUIRE(decoded.size() == 1);
    CHECK(decoded.count("good") == 1);
    CHECK(decoded.at("good").filename == "a.gcode");
    CHECK(decoded.at("good").options.at("x") == true);
}

TEST_CASE("queued job options prune drops unqueued ids and reports changed",
          "[job_queue][options]") {
    auto stored = sample_map();

    SECTION("nothing to drop: unchanged") {
        auto result = helix::queue::prune_queued_job_options(stored, {"0002", "0001"});
        CHECK_FALSE(result.changed);
        REQUIRE(result.entries.size() == 2);
        CHECK(result.entries.count("0001") == 1);
        CHECK(result.entries.count("0002") == 1);
    }

    SECTION("empty store: unchanged even with an empty queue") {
        auto result = helix::queue::prune_queued_job_options({}, {});
        CHECK_FALSE(result.changed);
        CHECK(result.entries.empty());
    }

    SECTION("dropped job disappears and changed is set") {
        auto result = helix::queue::prune_queued_job_options(stored, {"0002"});
        REQUIRE(result.changed);
        REQUIRE(result.entries.size() == 1);
        CHECK(result.entries.count("0001") == 0);
        CHECK(result.entries.count("0002") == 1);
    }

    SECTION("empty queue prunes everything") {
        auto result = helix::queue::prune_queued_job_options(stored, {});
        REQUIRE(result.changed);
        CHECK(result.entries.empty());
    }
}

namespace {

JobQueueEntry entry(const char* job_id, const char* filename) {
    return {job_id, filename, 0.0, 0.0};
}

} // namespace

TEST_CASE("find_new_job_id recovers the id added between two queue snapshots",
          "[job_queue][options]") {
    using helix::queue::find_new_job_id;

    const std::vector<std::string> before = {"0001", "0002"};
    const std::vector<JobQueueEntry> after = {entry("0001", "a.gcode"), entry("0002", "b.gcode"),
                                              entry("0003", "benchy.gcode")};

    SECTION("one new id") {
        const auto found = find_new_job_id(before, after);
        REQUIRE(found.has_value());
        CHECK(*found == "0003");
    }

    SECTION("unchanged queue: nothing new") {
        CHECK_FALSE(
            find_new_job_id({"0001", "0002"}, {entry("0001", "a.gcode"), entry("0002", "b.gcode")})
                .has_value());
    }

    SECTION("empty before list: the whole queue is new, but one entry is one id") {
        const auto found = find_new_job_id({}, {entry("0007", "only.gcode")});
        REQUIRE(found.has_value());
        CHECK(*found == "0007");
    }

    SECTION("two new ids at once: ambiguous, refuse to guess") {
        // Another client queued concurrently; either id could be ours, so the
        // saved options must not attach to the wrong job.
        CHECK_FALSE(find_new_job_id(before, {entry("0001", "a.gcode"), entry("0002", "b.gcode"),
                                             entry("0003", "x.gcode"), entry("0004", "y.gcode")})
                        .has_value());
    }

    SECTION("a job removed while ours was added still yields one new id") {
        const auto found =
            find_new_job_id(before, {entry("0002", "b.gcode"), entry("0003", "x.gcode")});
        REQUIRE(found.has_value());
        CHECK(*found == "0003");
    }

    SECTION("duplicate filename already queued: the id is still the new one") {
        const auto found = find_new_job_id(
            before, {entry("0001", "a.gcode"), entry("0002", "b.gcode"), entry("0009", "b.gcode")});
        REQUIRE(found.has_value());
        CHECK(*found == "0009");
    }

    SECTION("empty everything") {
        CHECK_FALSE(find_new_job_id({}, {}).has_value());
    }
}

TEST_CASE("automatic_transition parse off server.config", "[job_queue][options]") {
    json response;
    response["jsonrpc"] = "2.0";
    response["id"] = 42;

    SECTION("true") {
        response["result"]["config"]["job_queue"]["automatic_transition"] = true;
        CHECK(helix::parse_automatic_transition(response) == true);
    }

    SECTION("false") {
        response["result"]["config"]["job_queue"]["automatic_transition"] = false;
        CHECK(helix::parse_automatic_transition(response) == false);
    }

    SECTION("missing job_queue section") {
        response["result"]["config"]["file_manager"] = json::object();
        CHECK(helix::parse_automatic_transition(response) == false);
    }

    SECTION("missing automatic_transition key") {
        response["result"]["config"]["job_queue"]["job_transition_delay"] = 0.5;
        CHECK(helix::parse_automatic_transition(response) == false);
    }

    SECTION("missing config section entirely") {
        response["result"]["debug"] = json::object();
        CHECK(helix::parse_automatic_transition(response) == false);
    }

    SECTION("non-boolean value reads as false, not a crash") {
        response["result"]["config"]["job_queue"]["automatic_transition"] = "yes";
        CHECK(helix::parse_automatic_transition(response) == false);
    }

    SECTION("unreadable response (no result) reads as false") {
        CHECK(helix::parse_automatic_transition(json::object()) == false);
    }
}
