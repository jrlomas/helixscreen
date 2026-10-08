#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Where this machine's build and test hosts are configured. Sourced, never run.
#
# One file outside the tree, so every worktree and clone shares it:
#
#   ${XDG_CONFIG_HOME:-$HOME/.config}/helixscreen/build-hosts.env
#
# KEY=VALUE lines, # comments. No quotes and no spaces around =, because make
# reads the same file with -include (mk/remote.mk). A variable already set in
# the environment wins over the file. HELIX_BUILD_HOSTS_FILE names another file.
#
# Keys: HELIX_TEST_HOST, HELIX_TEST_CONTAINER, HELIX_TEST_WORKDIR,
# HELIX_TEST_TREES_HOST, HELIX_TEST_TREES, HELIX_TEST_CCACHE,
# HELIX_TEST_LOCK_DIR (scripts/test-host-run.sh), REMOTE_HOST, REMOTE_USER,
# REMOTE_DIR (mk/remote.mk), REMOTE_BUILD_DIR (scripts/remote-build.sh).
# docs/devel/BUILD_SYSTEM.md "Using your own build/test host" has the details.

BUILD_HOSTS_FILE="${HELIX_BUILD_HOSTS_FILE:-${XDG_CONFIG_HOME:-$HOME/.config}/helixscreen/build-hosts.env}"

# Parsed, not sourced: nothing in the file runs, and the environment can win.
load_build_hosts() {
    local line key
    [ -r "$BUILD_HOSTS_FILE" ] || return 0
    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in ''|'#'*|*' '=*|*=' '*) continue ;; *=*) ;; *) continue ;; esac
        key=${line%%=*}
        [[ "$key" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] || continue
        [ -n "${!key+x}" ] || export "$key=${line#*=}"
    done < "$BUILD_HOSTS_FILE"
}

# Fails with one line naming the variable and the file when it is unset, so a
# command that needs a host never guesses one.
require_build_host() { # <variable>
    [ -n "${!1:-}" ] && return 0
    echo "✗ $1 is not set: add $1=<host> to $BUILD_HOSTS_FILE (or export it)" >&2
    return 2
}

load_build_hosts
