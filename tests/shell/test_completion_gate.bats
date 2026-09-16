#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The completion gate's contract: `make full-test-run` runs the bats shell suite
# as well as the C++ unit sweep. No other local gate invokes bats - not the
# commit hook, not test-xml - so if this chain is broken the shell suite reaches
# CI unrun and nothing local reports it.
#
# The [.] and [slow] sets stay outside the gate deliberately: quality-checks.sh
# runs [.] on any staged code change and nightly CI runs [slow], which keeps the
# gate cheap enough to be worth reaching for when work is finished.

load helpers

setup_file() {
    export GATE_DB="$BATS_FILE_TMPDIR/gate_db.txt"
    make -n -p 2>/dev/null > "$GATE_DB" || true
}

recipe_of() {
    awk -v t="^$1:" '$0 ~ t {f=1; next} /^[^\t#]/ {f=0} f' "$GATE_DB"
}

@test "full-test-run and unit-sweep are both defined" {
    grep -q '^full-test-run:' "$GATE_DB"
    grep -q '^unit-sweep:' "$GATE_DB"
}

@test "full-test-run and unit-sweep are both phony" {
    local phony
    phony=$(grep '^\.PHONY:' "$GATE_DB")
    contains "full-test-run" "$phony"
    contains "unit-sweep" "$phony"
}

@test "full-test-run invokes the bats shell suite" {
    contains "test-shell" "$(recipe_of full-test-run)"
}

@test "full-test-run chains unit-sweep rather than duplicating its filter" {
    grep -qE '^full-test-run:.*\bunit-sweep\b' "$GATE_DB"
}

@test "unit-sweep excludes the hidden and slow sets" {
    local r
    r=$(recipe_of unit-sweep)
    contains '~[.]' "$r"
    contains '~[slow]' "$r"
}

@test "unit-sweep answers the C++ question alone, without the shell suite" {
    lacks "test-shell" "$(recipe_of unit-sweep)"
}

@test "test-run refuses, exits non-zero, and names both gates" {
    run make test-run
    [ "$status" -eq 2 ]
    contains "unit-sweep" "$output"
    contains "full-test-run" "$output"
}
