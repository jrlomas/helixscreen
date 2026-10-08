#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# make full-test-run: the completion gate, the C++ unit sweep and the bats
# shell suite. Nothing else runs bats locally - not the commit hook, not
# test-xml - so without it the shell suite reaches CI unrun.
#
#   ZEUS=1   the sweep runs on zeus (scripts/zeus-run.sh sweep, against this
#            tree's mirror) while bats runs here; the gate costs the longer of
#            the two, and fails if either fails. bats stays here because the
#            zeus container is root with no shellcheck.
#   ZEUS=0   both run here, sweep first.
#   unset    automatic: zeus-run.sh --probe decides from the link (it prints
#            the numbers), and only while HELIX_ZEUS_AUTO=1.
#
# [.] and [slow] stay outside deliberately: quality-checks.sh runs [.] on any
# staged code change, and nightly CI runs [slow].
set -uo pipefail

# Automatic offload stays off until a warm zeus sweep is measured finishing
# before bats; turning it on is this one line.
HELIX_ZEUS_AUTO=${HELIX_ZEUS_AUTO:-0}

MAKE=${MAKE:-make}
ZEUS_RUN=${ZEUS_RUN:-$(dirname "$0")/zeus-run.sh}

case "${ZEUS:-}" in
    1) where=zeus ;;
    0) where=local ;;
    "")
        if [ "$HELIX_ZEUS_AUTO" != 1 ]; then
            echo "→ unit sweep runs here (automatic zeus offload is off; HELIX_ZEUS_AUTO=1 turns it on, ZEUS=1 forces it)"
            where=local
        elif "$ZEUS_RUN" --probe; then
            where=zeus
        else
            where=local
        fi ;;
    *) echo "✗ ZEUS must be 1, 0 or unset, not '$ZEUS'" >&2; exit 2 ;;
esac

done_msg() {
    echo "✓ Completion gate passed: unit sweep + shell suite"
    echo "  Outside this gate: [.] (commit hook) and [slow] (nightly). Both: make test-all"
}

if [ "$where" = local ]; then
    "$MAKE" --no-print-directory unit-sweep || exit $?
    "$MAKE" --no-print-directory test-shell || exit $?
    done_msg
    exit 0
fi

zlog="${TMPDIR:-/tmp}/full-test-run-zeus.$$.log"
echo "→ unit sweep on zeus (output: $zlog), shell suite here"
"$ZEUS_RUN" sweep >"$zlog" 2>&1 &
zpid=$!
"$MAKE" --no-print-directory test-shell
bats_rc=$?
if kill -0 "$zpid" 2>/dev/null; then echo "→ shell suite done; waiting for the zeus sweep"; fi
wait "$zpid"
zeus_rc=$?

verdict() { [ "$1" -eq 0 ] && echo passed || echo "FAILED (exit $1)"; }
if [ "$zeus_rc" -ne 0 ]; then
    echo "--- zeus sweep, last 40 lines of $zlog ---"
    tail -n 40 "$zlog"
fi
echo "  zeus unit sweep: $(verdict "$zeus_rc")"
echo "  local shell suite: $(verdict "$bats_rc")"
[ "$zeus_rc" -eq 0 ] && [ "$bats_rc" -eq 0 ] || exit 1
done_msg
