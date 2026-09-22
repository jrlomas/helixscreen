// tests/unit/test_snapmaker_exceptions.cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "snapmaker_exceptions.h"

#include "../catch_amalgamated.hpp"

using namespace helix::snapmaker;

TEST_CASE("a four-part code decodes as level-id-index-code", "[snapmaker][exceptions]") {
    auto c = decode_exception_code("0003-0530-0000-0011");
    REQUIRE(c.has_value());
    REQUIRE(c->level == 3);
    REQUIRE(c->id == 530);
    REQUIRE(c->index == 0);
    REQUIRE(c->code == 11);
}

TEST_CASE("a code embedded in a sentence is found", "[snapmaker][exceptions]") {
    auto c = decode_exception_code("!! Error 0003-0530-0000-0011 The plate has not been removed");
    REQUIRE(c.has_value());
    REQUIRE(c->id == 530);
}

TEST_CASE("text carrying no code decodes to nothing", "[snapmaker][exceptions]") {
    REQUIRE_FALSE(decode_exception_code("!! Must home Z axis first").has_value());
    REQUIRE_FALSE(decode_exception_code("").has_value());
}

TEST_CASE("a three-part code is not mistaken for a four-part one", "[snapmaker][exceptions]") {
    // The firmware also uses a 3-part basic form (id-index-code) with no level.
    // Reading it as level-id-index would silently shift every field.
    REQUIRE_FALSE(decode_exception_code("0530-0000-0011").has_value());
}

TEST_CASE("the plate-removal code maps to its message", "[snapmaker][exceptions]") {
    auto c = decode_exception_code("0003-0530-0000-0011");
    REQUIRE(exception_message(*c) ==
            "Remove the PEI sheet from the bed, then start again: probing through the sheet "
            "gives wrong results");
}

TEST_CASE("an unknown code has no message rather than a wrong one", "[snapmaker][exceptions]") {
    // A wrong-but-confident message is worse than falling back to the
    // firmware's own text.
    ExceptionCode unknown{3, 999, 0, 0};
    REQUIRE(exception_message(unknown).empty());
}

TEST_CASE("levels map to what the firmware will do", "[snapmaker][exceptions]") {
    REQUIRE(severity_of(1) == ExceptionSeverity::Informational);
    REQUIRE(severity_of(2) == ExceptionSeverity::Pause);
    REQUIRE(severity_of(3) == ExceptionSeverity::Cancel);
    REQUIRE(severity_of(99) == ExceptionSeverity::Cancel); // unknown: assume the worst
}
