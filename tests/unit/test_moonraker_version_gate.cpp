// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "application.h"

#include <string>

#include "../catch_amalgamated.hpp"

// The predicate decides whether a printer gets a startup warning, so every
// answer it can give is worth pinning: an absent version must not read as old,
// and a git-describe suffix must not drag a supported release below the floor.
TEST_CASE("Moonraker version gate", "[version][moonraker]") {
    SECTION("releases below the floor warn") {
        CHECK(Application::moonraker_version_too_old("v0.8.0"));
        CHECK(Application::moonraker_version_too_old("0.8.9"));
        CHECK(Application::moonraker_version_too_old("v0.7.1"));
    }

    SECTION("the floor and above do not") {
        CHECK_FALSE(Application::moonraker_version_too_old("v0.9.0"));
        CHECK_FALSE(Application::moonraker_version_too_old("0.9.3"));
        CHECK_FALSE(Application::moonraker_version_too_old("v1.0.0"));
    }

    SECTION("a git-describe suffix is compared on the core triple") {
        // SemVer ranks a prerelease below its own release, so comparing the
        // full string would warn a printer running a build of the floor
        // release itself.
        CHECK_FALSE(Application::moonraker_version_too_old("v0.9.0-16-g0f1e2d3"));
        CHECK_FALSE(Application::moonraker_version_too_old("v0.9.4-2-gdeadbee"));
        CHECK(Application::moonraker_version_too_old("v0.8.0-16-g0f1e2d3"));
    }

    SECTION("a version we could not read is not a version we can judge") {
        // server.info's moonraker_version defaults to "unknown" when the field
        // is missing. Treating an unreadable string as too old would warn every
        // printer that never reported one.
        CHECK_FALSE(Application::moonraker_version_too_old("unknown"));
        CHECK_FALSE(Application::moonraker_version_too_old(""));
        CHECK_FALSE(Application::moonraker_version_too_old("not-a-version"));
    }
}
