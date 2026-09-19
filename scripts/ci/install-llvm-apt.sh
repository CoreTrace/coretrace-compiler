#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Installs LLVM/Clang from apt.llvm.org on Debian/Ubuntu.
#
# The repository is configured directly rather than through the upstream llvm.sh:
# that script downloads the signing key from apt.llvm.org on every run, and that
# single download failed repeatedly on the CI runners (native and QEMU-emulated),
# taking the Build workflow down with it. The signing key is vendored next to this
# script (scripts/ci/apt.llvm.org.asc, fingerprint
# 6084F3CF814B57C1CF12EFD515CF4D18AF4F7421) and apt itself retries the mirror.
#
# usage: install-llvm-apt.sh <llvm major> [extra apt packages...]
# Must run as root (or via sudo).
set -euo pipefail

LLVM_VERSION="${1:?usage: install-llvm-apt.sh <llvm major> [extra apt packages...]}"
shift
EXTRA_PACKAGES=("$@")

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KEY_FILE="${SCRIPT_DIR}/apt.llvm.org.asc"
KEYRING=/etc/apt/keyrings/apt.llvm.org.gpg
SOURCES=/etc/apt/sources.list.d/apt.llvm.org.list

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
APT_OPTS=(-o Acquire::Retries=5 -o Acquire::http::Timeout=30 -o Acquire::https::Timeout=30)

# shellcheck disable=SC1091
. /etc/os-release
CODENAME="${VERSION_CODENAME:?/etc/os-release has no VERSION_CODENAME}"

if ! command -v gpg >/dev/null 2>&1; then
  retry apt-get "${APT_OPTS[@]}" update
  retry apt-get "${APT_OPTS[@]}" install -y --no-install-recommends gnupg ca-certificates
fi

install -d -m 0755 /etc/apt/keyrings
gpg --dearmor --yes -o "${KEYRING}" "${KEY_FILE}"
echo "deb [signed-by=${KEYRING}] https://apt.llvm.org/${CODENAME}/ llvm-toolchain-${CODENAME}-${LLVM_VERSION} main" \
  > "${SOURCES}"

retry apt-get "${APT_OPTS[@]}" update
# clang, llvm and the LLVM CMake package (llvm-<v>-dev provides LLVMConfig.cmake).
retry apt-get "${APT_OPTS[@]}" install -y --no-install-recommends \
  "clang-${LLVM_VERSION}" "llvm-${LLVM_VERSION}" "llvm-${LLVM_VERSION}-dev" "${EXTRA_PACKAGES[@]}"
