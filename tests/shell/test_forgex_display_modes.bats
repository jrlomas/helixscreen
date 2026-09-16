#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# ForgeX display-mode ownership across install and uninstall:
# - configure_forgex_display records the mode the printer arrived in (first
#   write wins) before moving the display to GUPPY
# - uninstall_forgex restores the recorded mode and consumes the record; with
#   no record it leaves variables.cfg alone (GUPPY is itself a working UI, so
#   an uninstall never has to guess a mode to leave the printer in)
#
# All paths run through the FORGEX_VAR_FILE / FORGEX_PREV_DISPLAY seams, which
# default to the on-device /opt paths.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers
}

display_fixture() {
    export FORGEX_VAR_FILE="$BATS_TEST_TMPDIR/variables.cfg"
    export FORGEX_PREV_DISPLAY="$BATS_TEST_TMPDIR/helixscreen_prev_display"
    rm -f "$FORGEX_VAR_FILE" "$FORGEX_PREV_DISPLAY"
    SUDO=""
    log_info() { :; }
    log_success() { :; }
    log_warn() { :; }
    . "$WORKTREE_ROOT/scripts/lib/installer/forgex.sh"
}

write_var_file() {
    printf "[Variables]\ndisplay = '%s'\nz_offset = 0.12\n" "$1" > "$FORGEX_VAR_FILE"
}

# A $SUDO that logs every invocation and still performs it, so tests can pin
# that writes into root-owned mod_data go through sudo.
sudo_logging_fixture() {
    display_fixture
    local wrapper="$BATS_TEST_TMPDIR/sudo"
    printf '#!/bin/sh\necho "sudo $*" >> "%s/sudo-calls"\nexec "$@"\n' \
        "$BATS_TEST_TMPDIR" > "$wrapper"
    chmod +x "$wrapper"
    SUDO="$wrapper"
}

# --- install: record the arrival mode, then take over ---

@test "install records a STOCK arrival and moves the display to GUPPY" {
    display_fixture
    write_var_file STOCK
    run configure_forgex_display
    [ "$status" -eq 0 ] || fail "expected success, got status $status: $output"
    contains "STOCK" "$(cat "$FORGEX_PREV_DISPLAY")"
    contains "display = 'GUPPY'" "$(cat "$FORGEX_VAR_FILE")"
    # Unrelated keys must survive the takeover.
    contains "z_offset = 0.12" "$(cat "$FORGEX_VAR_FILE")"
}

@test "install records a HEADLESS arrival and moves the display to GUPPY" {
    display_fixture
    write_var_file HEADLESS
    run configure_forgex_display
    [ "$status" -eq 0 ] || fail "expected success, got status $status: $output"
    contains "HEADLESS" "$(cat "$FORGEX_PREV_DISPLAY")"
    contains "display = 'GUPPY'" "$(cat "$FORGEX_VAR_FILE")"
}

@test "install takes over a FEATHER arrival like STOCK and HEADLESS" {
    # Forge-X 1.4.2 defaults to FEATHER. Feather is Klipper macros driving
    # screen.sh, not a process to stop, so taking the display means claiming
    # the mode variable itself.
    display_fixture
    write_var_file FEATHER
    run configure_forgex_display
    [ "$status" -eq 0 ] || fail "expected success, got status $status: $output"
    contains "FEATHER" "$(cat "$FORGEX_PREV_DISPLAY")"
    contains "display = 'GUPPY'" "$(cat "$FORGEX_VAR_FILE")"
}

@test "install records a GUPPY arrival without rewriting variables.cfg" {
    display_fixture
    write_var_file GUPPY
    run configure_forgex_display
    # GUPPY needs no takeover and the fixture tree has no GuppyScreen init
    # scripts to disable, so configure reports that it changed nothing.
    [ "$status" -eq 1 ] || fail "expected status 1 (nothing to change), got $status: $output"
    contains "GUPPY" "$(cat "$FORGEX_PREV_DISPLAY")"
    contains "display = 'GUPPY'" "$(cat "$FORGEX_VAR_FILE")"
}

@test "an upgrade re-run does not overwrite the recorded arrival mode" {
    display_fixture
    printf 'STOCK\n' > "$FORGEX_PREV_DISPLAY"
    write_var_file GUPPY
    run configure_forgex_display
    contains "STOCK" "$(cat "$FORGEX_PREV_DISPLAY")"
}

@test "an unrecognised display mode is neither recorded nor rewritten" {
    display_fixture
    write_var_file WEIRD
    run configure_forgex_display
    [ "$status" -eq 1 ] || fail "expected status 1 (nothing to change), got $status: $output"
    [ ! -f "$FORGEX_PREV_DISPLAY" ] || fail "unknown mode must not be recorded"
    contains "display = 'WEIRD'" "$(cat "$FORGEX_VAR_FILE")"
}

@test "a missing variables.cfg records nothing" {
    display_fixture
    run configure_forgex_display
    [ ! -f "$FORGEX_PREV_DISPLAY" ] || fail "no record may appear without a variables.cfg"
}

@test "the record write goes through \$SUDO (mod_data is root-owned)" {
    sudo_logging_fixture
    write_var_file STOCK
    run configure_forgex_display
    [ "$status" -eq 0 ] || fail "expected success, got status $status: $output"
    contains "sudo tee $FORGEX_PREV_DISPLAY" "$(cat "$BATS_TEST_TMPDIR/sudo-calls")"
}

@test "the variables.cfg and record paths default to the on-device /opt paths" {
    local body
    body=$(cat "$WORKTREE_ROOT/scripts/lib/installer/forgex.sh")
    contains '${FORGEX_VAR_FILE:-/opt/config/mod_data/variables.cfg}' "$body"
    contains '${FORGEX_PREV_DISPLAY:-/opt/config/mod_data/helixscreen_prev_display}' "$body"
}

# --- uninstall: restore the recorded mode, or leave well enough alone ---

@test "uninstall restores a recorded STOCK mode and consumes the record" {
    display_fixture
    write_var_file GUPPY
    printf 'STOCK\n' > "$FORGEX_PREV_DISPLAY"
    run uninstall_forgex
    [ "$status" -eq 0 ] || fail "expected success, got status $status: $output"
    contains "display = 'STOCK'" "$(cat "$FORGEX_VAR_FILE")"
    [ ! -f "$FORGEX_PREV_DISPLAY" ] || fail "a completed restore must consume the record"
}

@test "uninstall restores a recorded HEADLESS mode" {
    display_fixture
    write_var_file GUPPY
    printf 'HEADLESS\n' > "$FORGEX_PREV_DISPLAY"
    run uninstall_forgex
    [ "$status" -eq 0 ] || fail "expected success, got status $status: $output"
    contains "display = 'HEADLESS'" "$(cat "$FORGEX_VAR_FILE")"
    [ ! -f "$FORGEX_PREV_DISPLAY" ] || fail "a completed restore must consume the record"
}

@test "uninstall restores a recorded FEATHER mode" {
    display_fixture
    write_var_file GUPPY
    printf 'FEATHER\n' > "$FORGEX_PREV_DISPLAY"
    run uninstall_forgex
    [ "$status" -eq 0 ] || fail "expected success, got status $status: $output"
    contains "display = 'FEATHER'" "$(cat "$FORGEX_VAR_FILE")"
    [ ! -f "$FORGEX_PREV_DISPLAY" ] || fail "a completed restore must consume the record"
}

@test "uninstall without a record leaves a GUPPY variables.cfg alone" {
    display_fixture
    write_var_file GUPPY
    run uninstall_forgex
    [ "$status" -eq 0 ] || fail "expected success, got status $status: $output"
    contains "display = 'GUPPY'" "$(cat "$FORGEX_VAR_FILE")"
}

@test "uninstall without a record does not force GUPPY over the current mode" {
    display_fixture
    write_var_file STOCK
    run uninstall_forgex
    [ "$status" -eq 0 ] || fail "expected success, got status $status: $output"
    contains "display = 'STOCK'" "$(cat "$FORGEX_VAR_FILE")"
}

@test "uninstall keeps a record that names no known mode" {
    display_fixture
    write_var_file GUPPY
    printf 'garbage\n' > "$FORGEX_PREV_DISPLAY"
    run uninstall_forgex
    [ "$status" -eq 0 ] || fail "expected success, got status $status: $output"
    contains "display = 'GUPPY'" "$(cat "$FORGEX_VAR_FILE")"
    [ -f "$FORGEX_PREV_DISPLAY" ] || fail "a record that restored nothing must be kept"
}

@test "uninstall keeps the record when variables.cfg is gone" {
    display_fixture
    printf 'STOCK\n' > "$FORGEX_PREV_DISPLAY"
    run uninstall_forgex
    [ "$status" -eq 0 ] || fail "expected success, got status $status: $output"
    [ -f "$FORGEX_PREV_DISPLAY" ] || fail "a record with nothing to restore into must be kept"
}

@test "the restore sed and record removal go through \$SUDO" {
    sudo_logging_fixture
    write_var_file GUPPY
    printf 'STOCK\n' > "$FORGEX_PREV_DISPLAY"
    run uninstall_forgex
    [ "$status" -eq 0 ] || fail "expected success, got status $status: $output"
    local calls
    calls=$(cat "$BATS_TEST_TMPDIR/sudo-calls")
    contains "sed -i" "$calls"
    contains "rm -f $FORGEX_PREV_DISPLAY" "$calls"
}

# --- wiring: the modular installer and both bundles carry the logic ---

@test "main.sh and the bundled install.sh call configure_forgex_display" {
    contains "configure_forgex_display" "$(cat "$WORKTREE_ROOT/scripts/lib/installer/main.sh")"
    contains "configure_forgex_display" "$(cat "$WORKTREE_ROOT/scripts/install.sh")"
}

@test "the uninstaller calls uninstall_forgex" {
    contains "uninstall_forgex" "$(cat "$WORKTREE_ROOT/scripts/lib/installer/uninstall.sh")"
}

@test "bundled install.sh and uninstall.sh carry the display-mode record" {
    contains "helixscreen_prev_display" "$(cat "$WORKTREE_ROOT/scripts/install.sh")"
    contains "helixscreen_prev_display" "$(cat "$WORKTREE_ROOT/scripts/uninstall.sh")"
}
