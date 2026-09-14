#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The K1 install sweep: on stock and Guilouz firmware, stop_competing_uis()
# stops the whole stock Creality stack that S99start_app runs, the backend
# (master-server, app-server, web-server) included. That is the kill list in
# hooks-k1.sh and what the K1 setup guide and TROUBLESHOOTING.md tell users.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    export INSTALL_DIR="$BATS_TEST_TMPDIR/usr/data/helixscreen"
    mkdir -p "$INSTALL_DIR/config"

    # Absolute system paths in the module resolve under MOCK_ROOT, so init
    # scripts, unit files and binaries on this host are never touched.
    export MOCK_ROOT="$BATS_TEST_TMPDIR/k1"
    mkdir -p "$MOCK_ROOT/etc/init.d"

    export START_APP_LOG="$BATS_TEST_TMPDIR/start_app.log"
    cat > "$MOCK_ROOT/etc/init.d/S99start_app" <<EOF
#!/bin/sh
echo "\$1" >> "$START_APP_LOG"
EOF
    chmod +x "$MOCK_ROOT/etc/init.d/S99start_app"

    # Every stock Creality process reads as running, under a pid string no
    # real process can have.
    mock_command_script pidof 'case "$1" in
    display-server|Monitor|master-server|audio-server|wifi-server|app-server|upgrade-server|web-server)
        echo "pid-of-$1" ;;
    *) exit 1 ;;
esac'

    export KILL_LOG="$BATS_TEST_TMPDIR/kill.log"
    : > "$KILL_LOG"

    INIT_SYSTEM="sysv"
    AD5M_FIRMWARE=""
    platform="k1"
    PREVIOUS_UI_SCRIPT=""
    unset HELIX_SELF_UPDATE

    local patched="$BATS_TEST_TMPDIR/competing_uis.sh"
    sed -E "s#(^|[^A-Za-z0-9_.~/-])/(etc|home|opt|lib|sys|usr)/#\\1$MOCK_ROOT/\\2/#g" \
        "$WORKTREE_ROOT/scripts/lib/installer/competing_uis.sh" > "$patched"
    unset _HELIX_COMMON_SOURCED _HELIX_COMPETING_UIS_SOURCED
    # shellcheck disable=SC1091
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh"
    # shellcheck disable=SC1090
    . "$patched"

    # ensure_k1_ssh starts host dropbear binaries it finds; nothing on this
    # host may be started.
    ensure_k1_ssh() { :; }
}

# Runs the sweep with signals and sleeps recorded instead of delivered. The
# overrides are defined inside `run`'s subshell, so bats' own process
# handling never sees them.
_sweep() {
    kill() { echo "$*" >> "$KILL_LOG"; }
    sleep() { :; }
    stop_competing_uis
}

# SIGTERM went to the pid pidof reported for $1.
_terminated() {
    grep -qx "pid-of-$1" "$KILL_LOG"
}

_assert_creality_stack_stopped() {
    grep -qx stop "$START_APP_LOG"
    _terminated display-server
    _terminated master-server
    _terminated app-server
    _terminated web-server
}

@test "k1 stock firmware: install sweep stops the Creality backend, web-server included" {
    K1_FIRMWARE="stock_klipper"
    run _sweep
    [ "$status" -eq 0 ]
    _assert_creality_stack_stopped
}

@test "k1 guilouz firmware: install sweep stops the Creality backend, web-server included" {
    K1_FIRMWARE="guilouz"
    run _sweep
    [ "$status" -eq 0 ]
    _assert_creality_stack_stopped
}
