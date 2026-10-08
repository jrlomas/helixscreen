#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# make full-test-run's split mode (scripts/full-test-run.sh).
#
# With ZEUS=1 the unit sweep runs on zeus while bats runs here, and the gate
# passes only when both do. make and zeus-run.sh are stubbed: each reports a
# verdict the test chooses and records that it ran.

GATE="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)/scripts/full-test-run.sh"

setup() {
    load helpers
    export TMPDIR="$BATS_TEST_TMPDIR"
    export CALLS="$BATS_TEST_TMPDIR/calls"
    # <target> exits with $MAKE_<target>_RC (unit_sweep, test_shell), default 0.
    mock_command_script fake-make '
for a in "$@"; do case "$a" in -*) ;; *) t=$a ;; esac; done
echo "make $t" >> "$CALLS"
case "$t" in
    unit-sweep) exit "${MAKE_unit_sweep_RC:-0}" ;;
    test-shell) exit "${MAKE_test_shell_RC:-0}" ;;
esac'
    mock_command_script fake-zeus-run '
echo "zeus-run $*" >> "$CALLS"
case "$1" in
    sweep) echo "zeus sweep output"; exit "${ZEUS_SWEEP_RC:-0}" ;;
    --probe) exit "${ZEUS_PROBE_RC:-1}" ;;
esac'
    export MAKE=fake-make ZEUS_RUN=fake-zeus-run
    unset ZEUS HELIX_ZEUS_AUTO
}

@test "ZEUS=1: the sweep goes to zeus, bats runs here, and both green passes" {
    ZEUS=1 run "$GATE"
    [ "$status" -eq 0 ]
    grep -qx "zeus-run sweep" "$CALLS"
    grep -qx "make test-shell" "$CALLS"
    refute_grep "make unit-sweep" "$CALLS"
    contains "zeus unit sweep: passed" "$output"
    contains "local shell suite: passed" "$output"
}

@test "ZEUS=1: a red zeus sweep with green bats fails the gate" {
    ZEUS=1 ZEUS_SWEEP_RC=1 run "$GATE"
    [ "$status" -ne 0 ]
    contains "zeus unit sweep: FAILED" "$output"
    contains "local shell suite: passed" "$output"
    contains "zeus sweep output" "$output"
}

@test "ZEUS=1: green zeus sweep with red bats fails the gate" {
    ZEUS=1 MAKE_test_shell_RC=2 run "$GATE"
    [ "$status" -ne 0 ]
    contains "zeus unit sweep: passed" "$output"
    contains "local shell suite: FAILED" "$output"
}

@test "ZEUS=0 runs both suites here and never asks zeus" {
    ZEUS=0 HELIX_ZEUS_AUTO=1 ZEUS_PROBE_RC=0 run "$GATE"
    [ "$status" -eq 0 ]
    grep -qx "make unit-sweep" "$CALLS"
    grep -qx "make test-shell" "$CALLS"
    refute_grep "zeus-run" "$CALLS"
}

@test "local mode: a red sweep stops the gate before bats" {
    ZEUS=0 MAKE_unit_sweep_RC=1 run "$GATE"
    [ "$status" -ne 0 ]
    refute_grep "make test-shell" "$CALLS"
}

@test "unset ZEUS with automatic offload off stays local without probing" {
    ZEUS_PROBE_RC=0 run "$GATE"
    [ "$status" -eq 0 ]
    grep -qx "make unit-sweep" "$CALLS"
    refute_grep "zeus-run" "$CALLS"
    contains "HELIX_ZEUS_AUTO=1" "$output"
}

@test "unset ZEUS with automatic offload on follows the probe" {
    HELIX_ZEUS_AUTO=1 ZEUS_PROBE_RC=0 run "$GATE"
    [ "$status" -eq 0 ]
    grep -qx "zeus-run --probe" "$CALLS"
    grep -qx "zeus-run sweep" "$CALLS"

    : > "$CALLS"
    HELIX_ZEUS_AUTO=1 ZEUS_PROBE_RC=1 run "$GATE"
    [ "$status" -eq 0 ]
    grep -qx "make unit-sweep" "$CALLS"
    refute_grep "zeus-run sweep" "$CALLS"
}

@test "a ZEUS value other than 1, 0 or empty is refused" {
    ZEUS=yes run "$GATE"
    [ "$status" -eq 2 ]
    [ ! -e "$CALLS" ]
}
