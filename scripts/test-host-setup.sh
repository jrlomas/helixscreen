#!/usr/bin/env bash
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Stand up a test host for scripts/test-host-run.sh: any Linux box with Docker
# that you can ssh to with a key and run `sudo -n docker` on. Run this on the
# machine you develop on. It reads the same build-hosts file as test-host-run.sh
# (scripts/lib/build_hosts.sh) and does every step over ssh, so the host needs
# no clone of the repo.
#
#   scripts/test-host-setup.sh                    # image, container, checks
#   scripts/test-host-setup.sh --recreate         # replace the container with one from the new image
#   scripts/test-host-setup.sh --commit-checkout  # also clone HELIX_TEST_WORKDIR for --commit runs
#
# Safe to re-run. The image is built from this tree's docker/Dockerfile.sanitizer
# every time, so there is one definition of the toolchain; the container is
# created only when it is missing (or with --recreate).
#
# The container mounts one host directory at one container directory: the
# parents of HELIX_TEST_TREES_HOST and HELIX_TEST_TREES. Mirrors, the ccache and
# the --commit checkout all live under it, so the container holds nothing worth
# keeping and recreating it loses nothing.
set -euo pipefail

# shellcheck source-path=SCRIPTDIR source=lib/build_hosts.sh
. "$(dirname "${BASH_SOURCE[0]}")/lib/build_hosts.sh"
require_build_host HELIX_TEST_HOST || exit 2

RECREATE=0 CLONE=0
for a in "$@"; do
    case "$a" in
        --recreate) RECREATE=1 ;;
        --commit-checkout) CLONE=1 ;;
        *) sed -n '5,22p' "$0" | sed 's/^# \?//'; exit 2 ;;
    esac
done

HOST=$HELIX_TEST_HOST
CONTAINER="${HELIX_TEST_CONTAINER:-helix-test}"
TREES_HOST="${HELIX_TEST_TREES_HOST:-helix-test/trees}"
TREES="${HELIX_TEST_TREES:-/work/trees}"
WORKDIR="${HELIX_TEST_WORKDIR:-/work/helixscreen}"
IMAGE=helixscreen/sanitizer
WORK_HOST=$(dirname "$TREES_HOST")
WORK=$(dirname "$TREES")

step() { printf '\033[36m→\033[0m %s\n' "$*"; }
die()  { printf '\033[31m✗\033[0m %s\n' "$*" >&2; exit 1; }
on_host() { ssh -o BatchMode=yes -o ConnectTimeout=5 "$HOST" "$@"; }

step "$HOST: docker, and sudo -n for it"
on_host 'sudo -n docker info >/dev/null' ||
    die "$HOST needs Docker and passwordless sudo for docker (sudo -n docker info failed)"

# The build context goes over the ssh connection as a tar, so the host builds
# exactly this tree's Dockerfile.
step "image $IMAGE from docker/Dockerfile.sanitizer"
tar -C "$(dirname "${BASH_SOURCE[0]}")/../docker" -cf - . |
    on_host "sudo -n docker build -q -t $IMAGE -f Dockerfile.sanitizer -" >/dev/null ||
    die "image build failed on $HOST"

step "container $CONTAINER: $HOST:$WORK_HOST mounted at $WORK"
on_host bash -se <<REMOTE || die "could not start container $CONTAINER on $HOST"
set -euo pipefail
mkdir -p "$TREES_HOST"
abs=\$(cd "$WORK_HOST" && pwd)
if [ "$RECREATE" = 1 ]; then sudo -n docker rm -f "$CONTAINER" >/dev/null 2>&1 || true; fi
if ! sudo -n docker inspect "$CONTAINER" >/dev/null 2>&1; then
    # CMD in the image is sleep infinity.
    sudo -n docker run -d --name "$CONTAINER" -v "\$abs:$WORK" -w "$WORK" "$IMAGE" >/dev/null
fi
sudo -n docker start "$CONTAINER" >/dev/null
REMOTE

if [ "$CLONE" = 1 ]; then
    url=$(git remote get-url origin)
    step "--commit checkout $WORKDIR from $url"
    on_host "sudo -n docker exec $CONTAINER bash -lc '[ -d $WORKDIR/.git ] || git clone -q --recurse-submodules $url $WORKDIR'" ||
        die "clone failed: the container needs to reach $url without credentials"
fi

# Ask the compiler that does the linking, not PATH: clang resolves ld.mold from
# its own program directory first, so a stale /usr/bin/ld.mold wins the link
# even with a newer one installed, and mold 1.x serializes JSON against the ELF
# header (prestonbrown/helixscreen#1584).
step "the linker clang actually uses"
on_host "sudo -n docker exec $CONTAINER bash -lc '
    echo \"int main(){return 0;}\" > /tmp/t.cpp && clang++ /tmp/t.cpp -o /tmp/t -fuse-ld=mold || exit 1
    ! readelf -p .comment /tmp/t | grep -qi \"mold 1\\.\"'" ||
    die "clang links with mold 1.x (or cannot link at all) in $CONTAINER"

# The timezone test copies real TZif files into a fixture; without tzdata that
# one case fails, and one red case stops the mutation gate from establishing a
# baseline at all.
step "zoneinfo the suite copies from"
on_host "sudo -n docker exec $CONTAINER test -f /usr/share/zoneinfo/America/New_York" ||
    die "/usr/share/zoneinfo is missing in $CONTAINER (tzdata)"

step "ready: scripts/test-host-run.sh test '[netd]' builds and runs one tag there"
