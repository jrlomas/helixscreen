#!/usr/bin/env bash
# Run the expensive, non-interactive gates on zeus instead of the box you are
# typing on.
#
#   scripts/zeus-run.sh mutate --tests '[1543]'     # the mutation gate
#   scripts/zeus-run.sh asan '[1543]'               # AddressSanitizer, one tag
#   scripts/zeus-run.sh asan                        # AddressSanitizer, full suite, sharded as CI runs it
#   scripts/zeus-run.sh tsan '[ams],[spoolman]'     # ThreadSanitizer, tags as ONE argument
#   scripts/zeus-run.sh tsan                        # ThreadSanitizer, full suite, sharded
#   scripts/zeus-run.sh test '[netd]'               # plain suite, one tag
#   scripts/zeus-run.sh sweep                       # make unit-sweep, sharded
#   scripts/zeus-run.sh asan-app help-qr --repeat 50  # the APP under ASAN
#   scripts/zeus-run.sh tsan-app help-qr --repeat 50  # the APP under TSan
#
# The app modes are here for the same reason asan is: the run is long and
# non-interactive, and the container's image (SDL, no ld.so.preload) is the
# only place an instrumented desktop app runs cleanly.
#
# A whole-suite C++ verdict comes from sweep, which shards the way CI and the
# local gate do. test with no tag runs the suite in one process, where
# cross-test contamination fails cases no branch touched, so it is not a gate.
# bats is not run here: the container is root with no shellcheck, so the shell
# suite fails on its environment; run `make test-shell` on thelio instead.
#
# Why these two in particular:
#
#   mutate  mutate_diff.py rebuilds and re-runs the suite once per changed hunk,
#           so it is the most expensive and least interactive thing in the loop.
#
#   asan    thelio's /etc/ld.so.preload holds libinput-config.so, so ASAN's
#           runtime loads second there and the binary produces NO test output and
#           exits 0 - a pass that ran nothing. The container has no
#           /etc/ld.so.preload, so ASAN works there with no workaround, and its
#           image matches CI's, which is why sanitizer findings reproduce.
#
# The commit under test has to be pushed: the container fetches it, it does not
# take your working tree. A verdict about an unpushed tree is one nobody else can
# reproduce.
set -euo pipefail

HOST="${ZEUS_HOST:-zeus.local}"   # bare `zeus` does not resolve from thelio
CONTAINER="${ZEUS_CONTAINER:-helix-tsan}"
WORKDIR="${ZEUS_WORKDIR:-/work/helixscreen}"

# zeus reports 72 cores and 251 GB, and TrueNAS hands most of that RAM to the
# ZFS ARC. The ARC gives memory back through gradual kernel reclaim, which a
# burst of compilers can outrun, and there is no swap, so the overshoot goes
# straight to the OOM killer and a compile dies with no error text.
#
# zeus keeps build headroom free with the zfs_arc_sys_free module parameter
# (64 GiB, a persistent TrueNAS ZFS tunable): ZFS shrinks the ARC ahead of time
# to hold that much RAM free, so a build starts into memory that is already
# free, and the ARC self-adjusts above that floor. The run checks the parameter
# and warns when it is missing or below SYS_FREE_FLOOR_GB.
#
# The job count comes from MemAvailable, or from zeus's jobpool, which sizes
# itself from MemAvailable too. A compile peaks near 400 MB here, ASAN
# included, so no single process is the problem - the total is.
SYS_FREE_FLOOR_GB=32
GB_PER_JOB="${ZEUS_GB_PER_JOB:-1}"      # asan overrides to 1.5 below

# When jobpool is installed on zeus, the container's make draws from zeus's
# machine pool instead of a -j of its own, so two runs (or a run and anything
# else pooled there) share the cores. The pool's state dir has to sit inside a
# directory the container already mounts (STATE_DIR in zeus's jobpool conf).
# Without it the run sizes -j from memory as below. Resolved on zeus.
JOBPOOL_BIN="${ZEUS_JOBPOOL:-\$HOME/.local/bin/jobpool}"   # $HOME is zeus's

WHAT="${1:-}"
[ -n "$WHAT" ] || { sed -n '2,37p' "$0" | sed 's/^# \?//'; exit 2; }
shift

SHA=$(git rev-parse HEAD)
SHORT=$(git rev-parse --short HEAD)
if ! git branch -r --contains "$SHA" 2>/dev/null | grep -q .; then
    echo "✗ $SHORT is not on any remote branch — push it first, or $HOST cannot fetch it" >&2
    exit 1
fi

# $HELIX_J is resolved on zeus from the memory that is free when the job starts.
# $HELIX_JFLAG is -j$HELIX_J, or empty when the container joins zeus's jobpool:
# make leaves an inherited jobserver when its command line has any -j.
# shellcheck disable=SC2016  # $HELIX_J must reach zeus unexpanded
case "$WHAT" in
    mutate) CMD='python3 scripts/mutate_diff.py --jobs $HELIX_J '"$*" ;;
    asan)   _tag="${1:-}"; [ $# -gt 0 ] && shift
            # Trailing args become make overrides, so ASAN_RUN_OPTIONS can be
            # tuned per run (quarantine_size_mb keeps freed blocks poisoned, which
            # turns a recycled-memory SEGV into a heap-use-after-free report).
            # No tag is the nightly's own sharded run, leak ratchet included.
            if [ -z "$_tag" ]; then
                CMD='make test-asan $HELIX_JFLAG '"$*"
            else
                CMD='make test-asan-one TEST="'"$_tag"'" $HELIX_JFLAG '"$*"
            fi
            GB_PER_JOB=1.5 ;;
    tsan)   _tag="${1:-}"; [ $# -gt 0 ] && shift
            # Same shape as asan: trailing args become make overrides, and no
            # tag is the sharded full-suite run.
            if [ -z "$_tag" ]; then
                CMD='make test-tsan $HELIX_JFLAG '"$*"
            else
                CMD='make test-tsan-one TEST="'"$_tag"'" $HELIX_JFLAG '"$*"
            fi
            TSAN_TAG="$_tag"
            GB_PER_JOB=1.5 ;;
    test)   CMD='make test $HELIX_JFLAG && ./build/bin/helix-tests "'"${1:-}"'"' ;;
    # Trailing args become make overrides, e.g. SHARD_CONCURRENCY=24.
    # NPROCS pins the shard count to thelio's 96. The count decides which tests
    # share a process, so zeus's own 216 would judge a grouping nobody runs
    # locally; a trailing NPROCS= still overrides it.
    sweep)  CMD='make unit-sweep NPROCS=96 $HELIX_JFLAG '"$*" ;;
    asan-app|tsan-app)
        # RECIPE is the positional argument; --repeat N (default 25 in the
        # make target) widens the drive. Both map onto the make target's
        # RECIPE/REPEAT variables.
        _recipe=""; _repeat=""
        while [ $# -gt 0 ]; do
            case "$1" in
                --repeat)
                    [ $# -ge 2 ] || { echo "✗ --repeat needs a value" >&2; exit 2; }
                    _repeat="$2"; shift 2 ;;
                --recipe)
                    [ $# -ge 2 ] || { echo "✗ --recipe needs a value" >&2; exit 2; }
                    _recipe="$2"; shift 2 ;;
                *)
                    if [ -n "$_recipe" ]; then
                        echo "✗ unexpected argument '$1' (usage: $WHAT [RECIPE] --repeat N)" >&2
                        exit 2
                    fi
                    _recipe="$1"; shift ;;
            esac
        done
        _vars=""
        if [ -n "$_recipe" ]; then _vars="RECIPE=$_recipe"; fi
        if [ -n "$_repeat" ]; then _vars="$_vars REPEAT=$_repeat"; fi
        CMD="make $WHAT $_vars"' $HELIX_JFLAG'
        EXPECTED_REPEAT="${_repeat:-25}"
        GB_PER_JOB=1.5 ;;
    *)      echo "✗ unknown job '$WHAT' (mutate | asan | tsan | test | sweep | asan-app | tsan-app)" >&2; exit 2 ;;
esac

LOG="${TMPDIR:-/tmp}/zeus-$WHAT-$SHORT.log"
echo "→ $HOST:$CONTAINER $WORKDIR @ $SHORT, log $LOG"

# The heredoc runs on zeus. docker needs sudo -n there (pbrown is deliberately
# not in the docker group), and git inside the container looks at a host-owned
# checkout, hence safe.directory.
# shellcheck disable=SC2087  # client-side expansion is the point: the sha, the
# container name and the caller's arguments are resolved HERE, and the one value
# that has to stay server-side ($1 in D) is escaped.
ssh "$HOST" bash -se <<REMOTE | tee "$LOG"
set -euo pipefail

# --- One job in the workdir at a time -----------------------------------------
# The job resets the checkout and rebuilds in $WORKDIR, so a second run in the
# same tree builds against files the first is replacing. Jobs queue rather
# than share: -j is sized from MemAvailable at start, which is only sound
# while this run is the only one allocating. The lock lives on the host
# because this script runs there; the workdir path only exists inside the
# container, so the lock name is derived from it.
LOCK="${ZEUS_LOCK_DIR:-/tmp}/helix-zeus-run-$(basename "$WORKDIR")".lock
exec 9>>"\$LOCK"
if ! flock -n 9; then
    echo "→ $WORKDIR busy: \$(tail -n 1 "\$LOCK" 2>/dev/null || echo another zeus-run job); waiting"
    flock 9
    echo "→ $WORKDIR free; continuing"
fi
# The lock is held here, so the file can be rewritten in place: it stays one
# line no matter how many jobs pass through it.
: > "\$LOCK"
printf 'held by pid %s: %s %s since %s\n' "\$\$" "$WHAT" "$SHORT" "\$(date '+%F %T')" >&9

# --- jobpool: join zeus's machine pool when it is installed ------------------
# The container sees the FIFO through whichever bind
# mount covers the pool's state dir; no such mount means no pool.
JP=$JOBPOOL_BIN
POOL_ENV=""
if [ ! -x "\$JP" ]; then
    :
elif ! _fifo=\$("\$JP" ensure 2>/dev/null); then
    echo "→ jobpool ensure failed on \$(hostname); sizing -j from memory"
else
    _state=\${_fifo%/fifo}
    while read -r _src _dst; do
        [ -n "\$_src" ] || continue
        case "\$_state/" in
            "\$_src"/*) POOL_ENV=\$("\$JP" container-env "\$_dst\${_state#"\$_src"}") && break ;;
        esac
    done <<< "\$(sudo -n docker inspect -f '{{range .Mounts}}{{.Source}} {{.Destination}}{{println}}{{end}}' "$CONTAINER" 2>/dev/null)"
    [ -n "\$POOL_ENV" ] || echo "→ jobpool state \$_state is not mounted in $CONTAINER; sizing -j from memory"
fi

# --- ZFS: headroom check, and heal a zfs_arc_max cap left behind ------------
# A marker "<pid> <bytes>" records a zfs_arc_max cap that a zeus-run set and did
# not undo. Once its pid is gone, the recorded bytes go back and the marker goes.
# 0 or garbage is not a value to write, so that marker stays and is named.
ARC_PARAM=${ZEUS_ARC_PARAM:-/sys/module/zfs/parameters/zfs_arc_max}
ARC_SYS_FREE=${ZEUS_ARC_SYS_FREE:-/sys/module/zfs/parameters/zfs_arc_sys_free}
ARC_MARK=${ZEUS_ARC_MARK:-/tmp/.helix-zeus-arc-orig}
if [ -e "\$ARC_MARK" ]; then
    read -r _mpid _orig < "\$ARC_MARK" || true
    if [ -n "\${_mpid:-}" ] && kill -0 "\$_mpid" 2>/dev/null; then
        :   # its run is still live and restores the value itself
    elif [ "\${_orig:-0}" -gt 0 ] 2>/dev/null && sudo -n sh -c "echo \$_orig > \$ARC_PARAM" 2>/dev/null; then
        sudo -n rm -f "\$ARC_MARK" 2>/dev/null || true
        echo "→ restored zfs_arc_max to \$_orig from leftover cap marker \$ARC_MARK"
    else
        echo "✗ leftover cap marker \$ARC_MARK ('\$(cat "\$ARC_MARK" 2>/dev/null)') holds no bytes to restore; check zfs_arc_max by hand, then delete it" >&2
    fi
fi
if _sf=\$(cat "\$ARC_SYS_FREE" 2>/dev/null) && [ "\${_sf:-0}" -lt $((SYS_FREE_FLOOR_GB << 30)) ] 2>/dev/null; then
    echo "⚠ zfs_arc_sys_free is \$_sf bytes, under ${SYS_FREE_FLOOR_GB} GiB: a burst of compilers can outrun ARC reclaim (see the zeus-run.sh header)" >&2
fi

# Derive the job count from what is free NOW, bounded by cores.
HELIX_J=\$(awk -v per=$GB_PER_JOB -v cpus="\$(nproc)" '
    /^MemAvailable/ { j = int((\$2/1048576) / per); if (j > cpus) j = cpus; if (j < 4) j = 4; print j }
' /proc/meminfo)
if [ -n "\$POOL_ENV" ]; then
    HELIX_JFLAG=""
    echo "→ MemAvailable \$(awk '/^MemAvailable/{printf "%.0fGB", \$2/1048576}' /proc/meminfo), joining jobpool: \$("\$JP" status --json 2>/dev/null | sed -n 's/.*"target":\([0-9]*\).*/target \1/p')"
else
    HELIX_JFLAG="-j\$HELIX_J"
    echo "→ MemAvailable \$(awk '/^MemAvailable/{printf "%.0fGB", \$2/1048576}' /proc/meminfo), using -j\$HELIX_J"
fi

# The container is long-lived but has no restart policy, so it is stopped after
# every NAS reboot and docker exec fails with a message about the container
# not running, several steps before anything explains why. Starting it is
# idempotent and costs nothing when it is already up.
if ! sudo -n docker ps --format '{{.Names}}' | grep -qx "$CONTAINER"; then
    echo "→ container $CONTAINER is not running; starting it"
    sudo -n docker start "$CONTAINER" >/dev/null || {
        echo "✗ could not start container $CONTAINER on \$(hostname)" >&2
        exit 1
    }
fi

D() { sudo -n docker exec -w "$WORKDIR" -e CCACHE_DIR=/work/ccache -e HELIX_J="\$HELIX_J" -e HELIX_JFLAG="\$HELIX_JFLAG" "$CONTAINER" bash -lc "\$1"; }

# A run whose ssh side died leaves its build running in the container while
# the lock is already released; resetting the tree under that build is the
# corruption the lock exists to prevent. Wait any make out before touching
# git. The poll interval is the only knob: long enough not to spam the log
# of a live box, overridable so tests can spin it fast. Zombies are excluded:
# an interrupted run's make is reparented to the container's PID 1, which
# never reaps it, so a bare pgrep -x make would wait on it forever.
while D 'pgrep -x -r R,S,D,T,t make >/dev/null'; do
    echo "→ orphaned build still running in $CONTAINER; waiting"
    sleep "${ZEUS_ORPHAN_POLL_SECS:-30}"
done

D 'git config --global --add safe.directory "*"' >/dev/null
# Submodules are fetched by the update below, for $SHA's pins only. Recursing
# here fetches the pin of every new superproject commit, and one pin to a
# submodule commit that was rebased away before pushing fails the whole fetch.
D 'git fetch --quiet --all --recurse-submodules=no'
# mutate_diff.py's default base is the nearest fork point among origin/main and the
# local main. A local main left behind by an earlier job puts that fork point
# before everything since, so the run is handed foreign hunks and refuses. Bring
# it level with the remote before the reset below.
D 'git fetch --quiet origin main && git update-ref refs/heads/main FETCH_HEAD'
D 'git reset --hard --quiet $SHA && git submodule update --init --recursive --quiet'
# A submodule already at its pin keeps the patches an earlier job applied, so a
# commit that edits a patch in patches/ meets the old revision and the build's
# drift check refuses. Reapply against this commit's patches/ every run.
D 'make reapply-patches >/dev/null'
D 'git log --oneline -1'
if [ -n "\$POOL_ENV" ]; then
    # jobpool exec on the host keeps the pool's consumer live for the whole
    # build; inside, the container opens the FIFO and exports MAKEFLAGS. The
    # FIFO is mode 600, so a container uid that is not root (or is a remapped
    # root) cannot open it; that run takes its own -j instead.
    "\$JP" exec -- sudo -n docker exec -w "$WORKDIR" -e CCACHE_DIR=/work/ccache -e HELIX_J="\$HELIX_J" -e HELIX_JFLAG= "$CONTAINER" bash -lc \
        "if { \$POOL_ENV; } 2>/dev/null; then :; else "'echo "→ uid \$(id -u) cannot open the jobpool FIFO; using -j\$HELIX_J"; HELIX_JFLAG=-j\$HELIX_J; fi; $CMD 2>&1'
else
    D '$CMD 2>&1'
fi
REMOTE

# A sanitizer run that produced no Catch2 summary ran nothing, whatever its exit
# code said. Refusing to call that a pass is the whole point of checking.
if [ "$WHAT" = asan ] && ! grep -qE 'All tests passed|test cases:|assertions:' "$LOG"; then
    echo ""
    echo "✗ no Catch2 summary in $LOG — the suite did not run, so this is not a clean ASAN result" >&2
    exit 1
fi
# A tagged TSan run tees the binary's own output, so it must carry a Catch2
# summary. The sharded full run keeps shard logs to itself and prints only the
# verdict, so that is what it must carry.
if [ "$WHAT" = tsan ]; then
    if [ -n "${TSAN_TAG:-}" ]; then _want='All tests passed|test cases:|assertions:'
    else _want='TSAN clean'; fi
    if ! grep -qE "$_want" "$LOG"; then
        echo ""
        echo "✗ no '$_want' in $LOG — the suite did not run, so this is not a clean TSAN result" >&2
        exit 1
    fi
fi

# An app run has no Catch2 summary to check; its evidence is the verdict line
# the make target prints and the per-pass lines the drive prints. The remote
# make already enforces both; this re-checks the local log so an exit 0 that
# somehow carried no verdict cannot be read as clean either.
if [ "$WHAT" = asan-app ] || [ "$WHAT" = tsan-app ]; then
    if ! grep -q 'clean — no sanitizer reports' "$LOG"; then
        echo ""
        echo "✗ no clean-verdict line in $LOG — the sanitizer verdict never ran" >&2
        exit 1
    fi
    passes=$(grep -cE '^\[screenshot\] recipe pass [0-9]+/[0-9]+' "$LOG" || true)
    if [ "$passes" -ne "$EXPECTED_REPEAT" ]; then
        echo ""
        echo "✗ expected $EXPECTED_REPEAT recipe passes in $LOG, found $passes — the drive did not complete" >&2
        exit 1
    fi
fi
