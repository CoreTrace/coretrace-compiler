#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Installs LLVM/Clang from apt.llvm.org. The mirror has short outages that made the
# Build workflow fail several times in one day, so every network step is retried.
#
# usage: install-llvm-apt.sh <llvm major> [extra apt packages...]
# Must run as root (or via sudo).
set -euo pipefail

LLVM_VERSION="${1:?usage: install-llvm-apt.sh <llvm major> [extra apt packages...]}"
shift
EXTRA_PACKAGES=("$@")

ATTEMPTS=5
DELAY_SECONDS=20

retry() {
  local attempt=1
  until "$@"; do
    if [[ "${attempt}" -ge "${ATTEMPTS}" ]]; then
      echo "install-llvm-apt: giving up after ${ATTEMPTS} attempts: $*" >&2
      return 1
    fi
    echo "install-llvm-apt: attempt ${attempt}/${ATTEMPTS} failed, retrying in ${DELAY_SECONDS}s: $*" >&2
    attempt=$((attempt + 1))
    sleep "${DELAY_SECONDS}"
  done
}

export DEBIAN_FRONTEND=noninteractive

retry curl -fsSL --retry 3 --retry-all-errors https://apt.llvm.org/llvm.sh -o /tmp/llvm.sh
chmod +x /tmp/llvm.sh
# llvm.sh probes apt.llvm.org itself and reports an unreachable mirror as an
# unsupported distribution; retrying the whole script covers that case too.
retry /tmp/llvm.sh "${LLVM_VERSION}"
rm -f /tmp/llvm.sh

if [[ "${#EXTRA_PACKAGES[@]}" -gt 0 ]]; then
  retry apt-get update
  retry apt-get install -y --no-install-recommends "${EXTRA_PACKAGES[@]}"
fi
