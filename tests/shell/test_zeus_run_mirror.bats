#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# zeus-run.sh mirrors the working tree to zeus (scripts/zeus-run.sh).
#
# A plain run copies the tree as it is on disk - uncommitted edits and
# untracked files included, ignored files and build output excluded - into a
# per-tree mirror on zeus, and runs the job there. Each mirror has its own
# lock, so two trees run at once while two runs of one tree queue.
#
# ssh is stubbed so its command runs here: rsync's remote half, the lock
# holder and the job heredoc all execute against a sandbox directory that
# stands in for zeus's mirror root. docker is stubbed to record, so no build
# runs. git is real: each test builds a small repository to sync.

ZEUS_RUN="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)/scripts/zeus-run.sh"

setup() {
    load helpers
    export ZEUS_LOCK_DIR="$BATS_TEST_TMPDIR/locks"
    export ZEUS_TREES_HOST="$BATS_TEST_TMPDIR/zeus-trees"
    export TMPDIR="$BATS_TEST_TMPDIR"
    export MOCK_DOCKER_LOG="$BATS_TEST_TMPDIR/docker.log"
    export ZEUS_JOBPOOL="$BATS_TEST_TMPDIR/no-jobpool"
    export ZEUS_ARC_PARAM="$BATS_TEST_TMPDIR/no-zfs/zfs_arc_max"
    export ZEUS_ARC_SYS_FREE="$BATS_TEST_TMPDIR/no-zfs/zfs_arc_sys_free"
    export ZEUS_ARC_MARK="$BATS_TEST_TMPDIR/arc-mark"
    mkdir -p "$ZEUS_LOCK_DIR"

    # ssh [-o opt]... <host> <command...>: run the command here through a
    # shell, the way sshd hands it to the remote login shell.
    mock_command_script ssh '
while [ $# -gt 0 ]; do
    case "$1" in -o) shift 2 ;; -*) shift ;; *) break ;; esac
done
shift
exec bash -c "$*"'
    mock_command_script sudo 'shift; exec "$@"'
    mock_command_script docker '
case "$1" in
    ps) echo helix-tsan ;;
    start) exit 0 ;;
    inspect) exit 0 ;;
    exec)
        case "$*" in
            *pgrep*) exit 1 ;;
            *) echo "[docker-exec] $*" >> "$MOCK_DOCKER_LOG" ;;
        esac ;;
    *) echo "unhandled docker call: $*" >&2; exit 1 ;;
esac
exit 0'
}

# A repository named <name> under the sandbox, with one committed file, an
# ignore rule for build/ and *.ignored, and a commit identity.
make_tree() { # <name>
    local t="$BATS_TEST_TMPDIR/$1"
    mkdir -p "$t"
    git -C "$t" init -q -b main
    git -C "$t" config user.email t@example.com
    git -C "$t" config user.name tester
    printf 'build/\n*.ignored\n' > "$t/.gitignore"
    echo one > "$t/tracked.txt"
    git -C "$t" add .gitignore tracked.txt
    git -C "$t" commit -qm base --no-verify
    echo "$t"
}

mirror_of() { echo "$ZEUS_TREES_HOST/$1"; }

@test "an uncommitted edit and an untracked file reach the mirror; ignored files and build/ do not" {
    local t m
    t=$(make_tree tree-a)
    m=$(mirror_of tree-a)
    echo edited > "$t/tracked.txt"
    echo new > "$t/untracked.txt"
    echo secret > "$t/local.ignored"
    mkdir -p "$t/build"; echo obj > "$t/build/thing.o"

    cd "$t"
    run "$ZEUS_RUN" test '[x]'
    [ "$status" -eq 0 ]
    [ "$(cat "$m/tracked.txt")" = edited ]
    [ "$(cat "$m/untracked.txt")" = new ]
    [ ! -e "$m/local.ignored" ]
    [ ! -e "$m/build" ]
    # The job runs in the mirror, not in the shared commit checkout.
    grep -qF -- "-w /work/trees/tree-a " "$MOCK_DOCKER_LOG"
    refute_grep "git reset" "$MOCK_DOCKER_LOG"
}

@test "a file deleted locally is gone from the mirror after the next sync" {
    local t m
    t=$(make_tree tree-a)
    m=$(mirror_of tree-a)
    echo a > "$t/committed-delete.txt"
    echo b > "$t/worktree-delete.txt"
    echo c > "$t/untracked-delete.txt"
    git -C "$t" add committed-delete.txt worktree-delete.txt
    git -C "$t" commit -qm more --no-verify
    cd "$t"
    run "$ZEUS_RUN" test
    [ "$status" -eq 0 ]
    [ -e "$m/committed-delete.txt" ] && [ -e "$m/worktree-delete.txt" ] && [ -e "$m/untracked-delete.txt" ]
    # Build output the job wrote is the mirror's own, never in the file list.
    mkdir -p "$m/build"; echo obj > "$m/build/keep.o"

    git -C "$t" rm -q committed-delete.txt
    git -C "$t" commit -qm drop --no-verify
    rm "$t/worktree-delete.txt" "$t/untracked-delete.txt"
    run "$ZEUS_RUN" test
    [ "$status" -eq 0 ]
    [ ! -e "$m/committed-delete.txt" ]
    [ ! -e "$m/worktree-delete.txt" ]
    [ ! -e "$m/untracked-delete.txt" ]
    [ -e "$m/tracked.txt" ]
    [ -e "$m/build/keep.o" ]
}

# A submodule at lib/<name> whose checkout lives elsewhere, reached through a
# symlink the way setup-worktree.sh shares lib/ with the main tree.
add_symlinked_submodule() { # <tree> <name>
    local t="$1" name="$2" src="$BATS_TEST_TMPDIR/src-$2"
    git init -q -b main "$src"
    echo "$name source" > "$src/source.c"
    git -C "$src" add source.c
    git -C "$src" -c user.email=t@example.com -c user.name=t commit -qm sub --no-verify
    git -C "$t" -c protocol.file.allow=always submodule add -q "$src" "lib/$name"
    git -C "$t" commit -qm "add $name" --no-verify
    rm -rf "${t:?}/lib/$name"
    git clone -q "$src" "$BATS_TEST_TMPDIR/shared-$name"
    ln -s "$BATS_TEST_TMPDIR/shared-$name" "$t/lib/$name"
}

@test "a symlinked lib/ submodule syncs as the target's files, not a link" {
    local t m
    t=$(make_tree tree-a)
    m=$(mirror_of tree-a)
    add_symlinked_submodule "$t" lvgl
    add_symlinked_submodule "$t" wpa_supplicant
    # A patch-created file in a patched submodule is part of the source; build
    # output in a shared one belongs to the local toolchain.
    echo created > "$BATS_TEST_TMPDIR/shared-lvgl/patch-created.h"
    echo arm > "$BATS_TEST_TMPDIR/shared-wpa_supplicant/libwpa_client.a"
    cd "$t"
    run "$ZEUS_RUN" test
    [ "$status" -eq 0 ]
    [ -d "$m/lib/lvgl" ] && [ ! -L "$m/lib/lvgl" ]
    [ "$(cat "$m/lib/lvgl/source.c")" = "lvgl source" ]
    [ "$(cat "$m/lib/lvgl/patch-created.h")" = created ]
    [ -f "$m/lib/wpa_supplicant/source.c" ]
    [ ! -e "$m/lib/wpa_supplicant/libwpa_client.a" ]
}

wait_for_file() { # <path>
    local _
    for _ in $(seq 1 200); do [ -e "$1" ] && return 0; sleep 0.05; done
    return 1
}

wait_for_line() { # <substring> <file>
    local _
    for _ in $(seq 1 200); do grep -qF "$1" "$2" 2>/dev/null && return 0; sleep 0.05; done
    return 1
}

hold_lock() { # <lock file> <ready marker>: prints the holder pid
    (
        exec 9>>"$1"
        flock 9
        printf 'held by pid %s: sweep 0123456 since 2026-10-08 12:00:00\n' "$$" >&9
        touch "$2"
        sleep 30 9>&-
    ) >/dev/null 2>&1 &
    echo $!
}

@test "two trees hold different locks and both proceed; two runs of one tree queue" {
    local a b holder out runner
    a=$(make_tree tree-a)
    b=$(make_tree tree-b)
    holder=$(hold_lock "$ZEUS_LOCK_DIR/helix-zeus-run-tree-a.lock" "$BATS_TEST_TMPDIR/ready")
    wait_for_file "$BATS_TEST_TMPDIR/ready"

    # tree-a's lock is held: tree-b runs straight through.
    cd "$b"
    run "$ZEUS_RUN" test
    [ "$status" -eq 0 ]
    lacks "busy:" "$output"
    grep -q "held by pid [0-9]*: test " "$ZEUS_LOCK_DIR/helix-zeus-run-tree-b.lock"

    # A run of tree-a waits for it, names the holder, and has synced nothing.
    out="$BATS_TEST_TMPDIR/waiting.log"
    cd "$a"
    "$ZEUS_RUN" test >"$out" 2>&1 &
    runner=$!
    wait_for_line "busy:" "$out"
    grep -q "busy: held by pid.*: sweep 0123456 since " "$out"
    [ ! -e "$(mirror_of tree-a)/tracked.txt" ]

    kill "$holder"
    wait "$holder" 2>/dev/null || true
    wait "$runner"
    grep -q "free; continuing" "$out"
    [ -e "$(mirror_of tree-a)/tracked.txt" ]
}

@test "with no jobpool on zeus, runs of different trees take turns" {
    # -j is sized from MemAvailable at start, which only holds for one run.
    local b holder out runner
    b=$(make_tree tree-b)
    holder=$(hold_lock "$ZEUS_LOCK_DIR/helix-zeus-run-global.lock" "$BATS_TEST_TMPDIR/ready")
    wait_for_file "$BATS_TEST_TMPDIR/ready"
    out="$BATS_TEST_TMPDIR/waiting.log"
    cd "$b"
    "$ZEUS_RUN" test >"$out" 2>&1 &
    runner=$!
    wait_for_line "no jobpool on" "$out"
    refute_grep "MemAvailable" "$out"
    kill "$holder"
    wait "$holder" 2>/dev/null || true
    wait "$runner"
    grep -q "MemAvailable" "$out"
}

@test "the provenance line and log name carry -dirty and the diff hash only when dirty" {
    local t sha short h1 h2
    t=$(make_tree tree-a)
    cd "$t"
    sha=$(git rev-parse HEAD)
    short=$(git rev-parse --short HEAD)

    run "$ZEUS_RUN" test
    [ "$status" -eq 0 ]
    contains "HEAD $sha + clean" "$output"
    [ -f "$TMPDIR/zeus-test-tree-a-$short.log" ]
    lacks "dirty" "$output"

    echo edited > tracked.txt
    run "$ZEUS_RUN" test
    [ "$status" -eq 0 ]
    h1=$(printf '%s\n' "$output" | sed -n 's/.*HEAD [0-9a-f]* + dirty \([0-9a-f]*\), 0 untracked.*/\1/p' | head -1)
    [ -n "$h1" ]
    [ -f "$TMPDIR/zeus-test-tree-a-$short-dirty-$h1.log" ]

    # Another edit is another hash; an untracked file is counted and hashed.
    echo new > untracked.txt
    run "$ZEUS_RUN" test
    [ "$status" -eq 0 ]
    h2=$(printf '%s\n' "$output" | sed -n 's/.*HEAD [0-9a-f]* + dirty \([0-9a-f]*\), 1 untracked.*/\1/p' | head -1)
    [ -n "$h2" ]
    [ "$h1" != "$h2" ]
}

@test "--commit on an unpushed commit still refuses" {
    local t
    t=$(make_tree tree-a)
    cd "$t"
    run "$ZEUS_RUN" --commit test
    [ "$status" -eq 1 ]
    contains "is not on any remote branch" "$output"
    [ ! -e "$MOCK_DOCKER_LOG" ]
    [ ! -e "$(mirror_of tree-a)" ]
}

make_mirror() { # <name> <age in days>
    mkdir -p "$ZEUS_TREES_HOST/$1"
    touch -d "$2 days ago" "$ZEUS_TREES_HOST/$1/.zeus-mirror-files"
}

@test "--prune removes an old mirror, keeps a fresh one and one in use" {
    make_mirror old 20
    make_mirror fresh 1
    make_mirror busy 20
    local holder
    holder=$(hold_lock "$ZEUS_LOCK_DIR/helix-zeus-run-busy.lock" "$BATS_TEST_TMPDIR/ready")
    wait_for_file "$BATS_TEST_TMPDIR/ready"

    run "$ZEUS_RUN" --prune
    kill "$holder"; wait "$holder" 2>/dev/null || true
    [ "$status" -eq 0 ]
    [ ! -e "$ZEUS_TREES_HOST/old" ]
    [ -d "$ZEUS_TREES_HOST/fresh" ]
    [ -d "$ZEUS_TREES_HOST/busy" ]
    contains "removed old" "$output"

    # DAYS is honoured: a 1-day-old mirror goes at --prune 0.
    run "$ZEUS_RUN" --prune 0
    [ "$status" -eq 0 ]
    [ ! -e "$ZEUS_TREES_HOST/fresh" ]
}

@test "--drop removes one tree's mirror" {
    make_mirror gone 0
    make_mirror stays 0
    run "$ZEUS_RUN" --drop gone
    [ "$status" -eq 0 ]
    [ ! -e "$ZEUS_TREES_HOST/gone" ]
    [ -d "$ZEUS_TREES_HOST/stays" ]
}

# --- --probe: the automatic full-test-run default ---------------------------

@test "--probe: an unreachable zeus means local" {
    mock_command_fail ssh
    run "$ZEUS_RUN" --probe
    [ "$status" -eq 1 ]
    contains "unreachable" "$output"
}

@test "--probe: a LAN round trip means zeus without measuring the sync" {
    mock_command ping "64 bytes from zeus: icmp_seq=1 ttl=64 time=0.137 ms"
    local t
    t=$(make_tree tree-a)
    cd "$t"
    run "$ZEUS_RUN" --probe
    [ "$status" -eq 0 ]
    contains "rtt 0.137 ms" "$output"
    contains "zeus" "$output"
    lacks "estimated sync" "$output"
}

@test "--probe: a slow link with a cold mirror means local, a warm one means zeus" {
    mock_command ping "64 bytes from zeus: icmp_seq=1 ttl=64 time=40.5 ms"
    export ZEUS_PROBE_BYTES_PER_SEC=1000   # pins the measured throughput
    local t
    t=$(make_tree tree-a)
    head -c 50000 /dev/urandom > "$t/big.bin"
    cd "$t"
    run "$ZEUS_RUN" --probe
    [ "$status" -eq 1 ]
    contains "estimated sync" "$output"

    # Warm: the mirror already holds every byte, so almost nothing would move.
    run "$ZEUS_RUN" test
    [ "$status" -eq 0 ]
    run "$ZEUS_RUN" --probe
    [ "$status" -eq 0 ]
    contains "estimated sync" "$output"
}
