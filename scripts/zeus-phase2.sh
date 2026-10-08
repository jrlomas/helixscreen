#!/bin/bash
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# HelixScreen - zeus sanitizer host provisioning (see also scripts/zeus-run.sh)
#
# Paths come from $HOME, so there is nothing host-specific to edit; -j12 is
# conservative because ZFS hands most of zeus's RAM to the ARC (see the comment below).
#
# Bootstrapping a fresh box: clone the repo to $HOME/helix-tsan/helixscreen, then
# run these from it. zeus-setup.sh also clones if the checkout is missing, so a
# standalone copy works too.
#
# Phase 2: first test build in the container setup.sh created.
set -u
WORK=$HOME/helix-tsan
LOG=$WORK/phase2.log
REPO=$WORK/helixscreen
exec >> "$LOG" 2>&1
echo "=== $(date -Is) phase2 start ==="

D() { sudo -n docker exec -w /work helix-tsan bash -lc "$1"; }

if [ ! -d "$REPO/.git" ]; then
  echo "NO_REPO — run setup.sh first"
  exit 1
fi

# Nothing is apt-installed here. The toolchain is the image's, from
# docker/Dockerfile.sanitizer; a second package list is a second thing to keep in step.
echo "--- toolchain in use ---"
D 'clang --version | head -1; g++ --version | head -1; ld.mold --version' \
  && echo TOOLCHAIN_OK || echo TOOLCHAIN_FAIL

echo "--- git safe.directory inside the container ---"
D 'git config --global --add safe.directory /work/helixscreen
   git config --global --add safe.directory "*"' >/dev/null

# zeus reports 72 cores and 251 GB and both mislead: ZFS hands most of that RAM to the
# ARC, and there is no swap, so an overshoot goes straight to the OOM killer and a
# compile dies with no error text. A fresh box may not have its zfs_arc_sys_free
# tunable yet, so this first build stays at -j12; scripts/zeus-run.sh sizes later runs
# from MemAvailable.
echo "--- first test build ---"
D 'cd /work/helixscreen
   export CCACHE_DIR=/work/ccache
   git log --oneline -1
   time make test -j12' && echo BUILD_OK || echo BUILD_FAIL

echo "=== $(date -Is) phase2 done ==="
