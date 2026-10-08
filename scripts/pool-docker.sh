#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# pool-docker.sh: run a container build inside the machine's jobpool budget.
#
#     scripts/pool-docker.sh docker run [OPTIONS] IMAGE make [ARGS...]
#     scripts/pool-docker.sh docker run [OPTIONS] IMAGE CMD [ARGS...]
#
# Any client with docker's `run` syntax works in place of docker.
#
# A make in the container joins the pool itself: the pool's state dir is
# mounted, the container opens the FIFO and exports MAKEFLAGS (`jobpool
# docker-args` / `container-env`), and the make's own -j is dropped, since a
# -j makes make leave a jobserver. If the container cannot open the FIFO
# (a remapped uid, a VM that cannot share a FIFO) that make keeps its -j.
#
# Any other command (ninja under idf.py, a build script) cannot join, so it
# runs under `helix-claim hold`, which holds tokens for the container's life
# and passes their count in as JOBPOOL_SLOTS and IDF_PY_BUILD_JOBS (idf.py's
# ninja -j).
#
# Without jobpool (CI, a Mac, a fresh clone), with JOBPOOL=0, or when the pool
# will not start, the command runs exactly as given.
set -u

jp=${HELIX_JOBPOOL:-jobpool}
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
if [ "${JOBPOOL:-}" = 0 ] || ! command -v "$jp" >/dev/null 2>&1; then exec "$@"; fi
if [ $# -lt 3 ] || [ "$2" != run ]; then exec "$@"; fi
docker=$1
shift 2

opts=()
while [ $# -gt 0 ] && [ "$1" != make ]; do
    opts+=("$1")
    shift
done

if [ $# -eq 0 ]; then
    # shellcheck disable=SC2016  # expanded by the inner sh, under the hold
    exec "$here/helix-claim" hold -- sh -c \
        'exec "$0" run -e JOBPOOL_SLOTS -e IDF_PY_BUILD_JOBS="$JOBPOOL_SLOTS" "$@"' \
        "$docker" "${opts[@]}"
fi

shift
dir=/run/jobpool
mount=$("$jp" docker-args "$dir" 2>/dev/null) || exec "$docker" run "${opts[@]}" make "$@"
join=$("$jp" container-env "$dir") || exec "$docker" run "${opts[@]}" make "$@"
args=() jflags=""
for a in "$@"; do
    case $a in
        -j* | --jobs*) jflags="$jflags $a" ;;
        *) args+=("$a") ;;
    esac
done
exec "$jp" exec -- "$docker" run "$mount" "${opts[@]}" sh -c \
    "if (exec 3<>$dir/fifo) 2>/dev/null; then $join; else set -- \"\$@\"$jflags; fi; exec make \"\$@\"" \
    make "${args[@]}"
