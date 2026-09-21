#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for the qc_xml_tools gate in scripts/quality-checks.sh.
#
# The constraint: qc_xml_const and qc_xml_attr guard on `[ -x build/bin/... ]`
# and skip when the binary is missing, and no ordinary build produces those
# binaries — `make` builds only the app. A gate whose tool nothing builds is a
# pass verdict that examines nothing, so the wiring that produces the tools is
# pinned here: one serial step, ahead of the parallel gate batch (two makes in
# one tree race on the object dir), failing red when the build fails.

setup() {
    load helpers
    cd "$BATS_TEST_DIRNAME/../.." || return 1
}

@test "tool-build gate is registered in QC_ALL and QC_SERIAL" {
    run grep -q 'QC_ALL=.*qc_xml_tools' scripts/quality-checks.sh
    [ "$status" -eq 0 ]
    run bash -c "grep -q '^QC_SERIAL=\"qc_xml_tools ' scripts/quality-checks.sh"
    [ "$status" -eq 0 ]
}

@test "tool-build gate builds both validator binaries" {
    run bash -c "sed -n '/^qc_xml_tools() {/,/^}/p' scripts/quality-checks.sh"
    [ "$status" -eq 0 ] || fail "qc_xml_tools not extractable"
    contains "validate-xml-constants" "$output"
    contains "validate-xml-attrs" "$output"
}

@test "tool-build gate wakes on the files the validators inspect" {
    run bash -c "sed -n '/qc_xml_tools)/,/;;/p' scripts/quality-checks.sh"
    [ "$status" -eq 0 ] || fail "trigger case not extractable"
    contains '\.xml$' "$output"
    contains '^tools/validate_xml' "$output"
}
