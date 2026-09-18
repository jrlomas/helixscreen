#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/suite-verdict.sh decides whether a suite run passed, from anchored
# patterns and the run's own exit code.
#
# Two failure modes it exists to close. A pipeline reports the exit status of its
# LAST stage, so `make ... | tail` exits 0 over a failed build; and this suite
# names 199 passing tests after failure semantics, so any grep for FAILED, Error
# or fail matches test NAMES and invents or masks results. TAP has a grammar -
# `^ok` and `^not ok` - and only that grammar decides anything here.
#
# Silence is never a pass: a log with no recognisable suite output exits 2, so a
# run that died before producing any cannot be read as green.

load helpers

VERDICT="$BATS_TEST_DIRNAME/../../scripts/suite-verdict.sh"

@test "a clean TAP log passes" {
    printf 'ok 1 alpha\nok 2 beta\n' > "$BATS_TEST_TMPDIR/l"
    run "$VERDICT" "$BATS_TEST_TMPDIR/l"
    [ "$status" -eq 0 ]
    contains "PASS" "$output"
}

@test "a TAP failure fails" {
    printf 'ok 1 alpha\nnot ok 2 beta\nok 3 gamma\n' > "$BATS_TEST_TMPDIR/l"
    run "$VERDICT" "$BATS_TEST_TMPDIR/l"
    [ "$status" -eq 1 ]
    contains "FAIL" "$output"
}

@test "a passing test whose NAME contains FAILED still passes" {
    printf 'ok 1 a failure summary with no FAILED line is evidence enough\nok 2 Error paths return early\nok 3 fails closed on bad input\n' \
        > "$BATS_TEST_TMPDIR/l"
    run "$VERDICT" "$BATS_TEST_TMPDIR/l"
    [ "$status" -eq 0 ]
    contains "PASS" "$output"
}

@test "a make error fails even with no TAP lines" {
    printf 'building\nmake[1]: *** [mk/tests.mk:617: test-shell] Error 1\n' > "$BATS_TEST_TMPDIR/l"
    run "$VERDICT" "$BATS_TEST_TMPDIR/l"
    [ "$status" -eq 1 ]
    contains "FAIL" "$output"
}

@test "a Catch2 failure fails" {
    printf 'FAILED:\n  REQUIRE( x == 1 )\nassertions: 12 | 11 passed | 1 failed\n' > "$BATS_TEST_TMPDIR/l"
    run "$VERDICT" "$BATS_TEST_TMPDIR/l"
    [ "$status" -eq 1 ]
}

@test "a Catch2 pass summary passes" {
    printf 'All tests passed (431 assertions in 52 test cases)\n' > "$BATS_TEST_TMPDIR/l"
    run "$VERDICT" "$BATS_TEST_TMPDIR/l"
    [ "$status" -eq 0 ]
}

@test "an empty log is indeterminate, never a pass" {
    : > "$BATS_TEST_TMPDIR/l"
    run "$VERDICT" "$BATS_TEST_TMPDIR/l"
    [ "$status" -eq 2 ]
    contains "INDETERMINATE" "$output"
}

@test "a log with no suite output at all is indeterminate" {
    printf 'cloning\nfetching\ndone\n' > "$BATS_TEST_TMPDIR/l"
    run "$VERDICT" "$BATS_TEST_TMPDIR/l"
    [ "$status" -eq 2 ]
}

@test "a nonzero run exit fails a log that otherwise looks clean" {
    printf 'ok 1 alpha\n' > "$BATS_TEST_TMPDIR/l"
    run "$VERDICT" --exit 1 "$BATS_TEST_TMPDIR/l"
    [ "$status" -eq 1 ]
    contains "exit" "$output"
}

@test "a zero run exit does not rescue a log containing failures" {
    printf 'not ok 1 alpha\n' > "$BATS_TEST_TMPDIR/l"
    run "$VERDICT" --exit 0 "$BATS_TEST_TMPDIR/l"
    [ "$status" -eq 1 ]
}

@test "a missing log is indeterminate, not a pass" {
    run "$VERDICT" "$BATS_TEST_TMPDIR/nope.log"
    [ "$status" -eq 2 ]
}

@test "a sharded Catch2 pass is recognised despite its line prefix" {
    printf '[shard 53] All tests passed (978 assertions in 160 test cases)\n' > "$BATS_TEST_TMPDIR/l"
    run "$VERDICT" "$BATS_TEST_TMPDIR/l"
    [ "$status" -eq 0 ]
    contains "PASS" "$output"
}

@test "a sharded failure is still a failure" {
    printf '[shard 7] FAILED:\n[shard 7] assertions: 3 | 2 passed | 1 failed\n' > "$BATS_TEST_TMPDIR/l"
    run "$VERDICT" "$BATS_TEST_TMPDIR/l"
    [ "$status" -eq 1 ]
}
