#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The build-hosts file (scripts/lib/build_hosts.sh) as make and the remote build
# script read it: the file names the hosts, the environment beats the file, and
# a command whose host is unset refuses in one line instead of guessing one.

ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers
    cd "$ROOT"
    mock_command_script ssh 'touch "$BATS_TEST_TMPDIR/ssh-called"; exit 255'
}

@test "mk/remote.mk takes the remote host from the build-hosts file, and the environment beats it" {
    printf 'REMOTE_HOST=fromfile.invalid\nREMOTE_DIR=/srv/fromfile\n' > "$HELIX_BUILD_HOSTS_FILE"
    run make -s -n -f mk/remote.mk remote-ssh
    [ "$status" -eq 0 ]
    contains "ssh -t fromfile.invalid" "$output"
    contains "cd /srv/fromfile" "$output"
    REMOTE_HOST=fromenv.invalid run make -s -n -f mk/remote.mk remote-ssh
    [ "$status" -eq 0 ]
    contains "ssh -t fromenv.invalid" "$output"
    contains "cd /srv/fromfile" "$output"
}

@test "a remote make target with no host refuses in one line, naming the file" {
    run make -s -f mk/remote.mk remote-ssh
    [ "$status" -ne 0 ]
    [ "${#lines[@]}" -eq 1 ]
    contains "REMOTE_HOST is not set" "$output"
    contains "$HELIX_BUILD_HOSTS_FILE" "$output"
    # The rsync targets need the directory too: --delete into "host:/" is what
    # an empty one would mean.
    REMOTE_HOST=h.invalid run make -s -f mk/remote.mk remote-sync
    [ "$status" -ne 0 ]
    contains "REMOTE_DIR is not set" "$output"
    [ ! -e "$BATS_TEST_TMPDIR/ssh-called" ]
}

@test "remote-build.sh with no host refuses in one line and reaches nothing" {
    run scripts/remote-build.sh test
    [ "$status" -eq 2 ]
    [ "${#lines[@]}" -eq 1 ]
    contains "REMOTE_HOST is not set" "$output"
    contains "$HELIX_BUILD_HOSTS_FILE" "$output"
    [ ! -e "$BATS_TEST_TMPDIR/ssh-called" ]
}

@test "a malformed or quoted line in the build-hosts file is ignored, not run" {
    printf 'HELIX_TEST_HOST = spaced\nHELIX_TEST_CONTAINER=$(touch %s/ran)\n# HELIX_TEST_WORKDIR=commented\n' \
        "$BATS_TEST_TMPDIR" > "$HELIX_BUILD_HOSTS_FILE"
    run bash -c '. scripts/lib/build_hosts.sh; echo "host=${HELIX_TEST_HOST:-} c=${HELIX_TEST_CONTAINER:-} w=${HELIX_TEST_WORKDIR:-}"'
    [ "$status" -eq 0 ]
    contains "host= " "$output"
    contains "w=" "$output"
    lacks "commented" "$output"
    [ ! -e "$BATS_TEST_TMPDIR/ran" ]
}
