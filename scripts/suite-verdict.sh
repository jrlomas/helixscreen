#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Decide whether a suite run passed, from line-anchored grammar and the run's
# own exit status.
#
# Two ways a verdict goes wrong without it. A pipeline reports its LAST stage's
# status, so `make ... | tail` exits 0 over a failed build; pass the real status
# with --exit and it is honoured. And a grep for FAILED, Error or fail matches
# test NAMES, of which this suite has 199 named after failure semantics, so such
# a grep both invents failures and hides them. Only anchored grammar counts
# here: TAP's ^ok and ^not ok, make's ^make: ***, Catch2's ^FAILED: and its
# assertion and test-case summaries.
#
# A log carrying none of that grammar exits 2, INDETERMINATE. A run that died
# before it produced output must never read as green.
#
# Usage: suite-verdict.sh [--exit N] <log>...
# Exit:  0 PASS    1 FAIL    2 INDETERMINATE

set -uo pipefail

run_exit=""
logs=()
while [ $# -gt 0 ]; do
    case "$1" in
        --exit)   run_exit="${2:-}"; shift 2 ;;
        --exit=*) run_exit="${1#*=}"; shift ;;
        -h|--help) sed -n '3,20p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
        *)        logs+=("$1"); shift ;;
    esac
done

if [ "${#logs[@]}" -eq 0 ]; then
    echo "INDETERMINATE: no log given" >&2
    exit 2
fi

present=()
for l in "${logs[@]}"; do
    [ -r "$l" ] && present+=("$l")
done

if [ "${#present[@]}" -eq 0 ]; then
    echo "INDETERMINATE: no readable log among: ${logs[*]}"
    exit 2
fi

# Sharded runs prefix every line with `[shard N] `, so each pattern is anchored
# to the start of the CONTENT, not of the raw line. Without this a 96-shard run
# reads as having produced no results at all.
PFX='^(\[shard [0-9]+\] )?'
count() { cat "${present[@]}" 2>/dev/null | grep -cE "$PFX$1" || true; }

tap_ok=$(count 'ok [0-9]')
tap_bad=$(count 'not ok [0-9]')
make_bad=$(count 'make(\[[0-9]+\])?: \*\*\*')
catch_bad=$(count 'FAILED:')
catch_sum_bad=$(count '(assertions|test cases): .*\| [1-9][0-9]* failed')
catch_ok=$(count 'All tests passed \(')
shard_bad=$(count '✗')

failures=$((tap_bad + make_bad + catch_bad + catch_sum_bad + shard_bad))
recognized=$((tap_ok + catch_ok + failures))

printf 'tap:    %s ok, %s not ok\n' "$tap_ok" "$tap_bad"
printf 'catch2: %s pass-summary, %s FAILED, %s failing-summary\n' "$catch_ok" "$catch_bad" "$catch_sum_bad"
printf 'make:   %s error line(s)\n' "$make_bad"

# The exit status is authoritative: a suite can print nothing but a crash.
if [ -n "$run_exit" ] && [ "$run_exit" != "0" ]; then
    echo "FAIL: run exit status $run_exit"
    exit 1
fi

if [ "$failures" -gt 0 ]; then
    echo "FAIL: $failures anchored failure marker(s)"
    exit 1
fi

if [ "$recognized" -eq 0 ]; then
    echo "INDETERMINATE: no suite grammar found - nothing was verified"
    exit 2
fi

echo "PASS: $recognized recognized result line(s), no failure markers"
exit 0
