#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Checks that install-llvm-apt.sh recovers from a mirror outage that outlasts apt's own
# retries. `apt-get update` then only warns and exits 0, so a later attempt must refresh
# the index again. apt-get, gpg and sleep are stubs: the first update fetches no index,
# and every install fails until an update has fetched one. Both installs are checked:
# LLVM's, and the gpg bootstrap that runs when gpg is missing.
#
# The script writes to /etc/apt, so this runs as root in a Debian or Ubuntu image
# without gpg:
#   docker run --rm -v "$PWD:/src:ro" ubuntu:22.04 bash /src/scripts/ci/check-install-llvm-apt.sh
set -euo pipefail

SCRIPT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/install-llvm-apt.sh"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

[[ "$(id -u)" -eq 0 ]] || fail "run as root, in a container"
if command -v gpg >/dev/null 2>&1; then
  fail "gpg is installed: the gpg bootstrap case needs an image without it"
fi

# Runs install-llvm-apt.sh against the stubs, with gpg present or not ($1), and prints
# how many times it refreshed the index.
run_case() {
  local stubs state
  stubs="$(mktemp -d)"
  state="$(mktemp -d)"

  cat >"${stubs}/apt-get" <<'STUB'
#!/usr/bin/env bash
for arg in "$@"; do
  case "${arg}" in
    update)
      count=$(( $(cat "${STUB_STATE}/updates" 2>/dev/null || echo 0) + 1 ))
      echo "${count}" >"${STUB_STATE}/updates"
      if [[ "${count}" -eq 1 ]]; then
        echo "W: Failed to fetch https://apt.llvm.org/jammy/dists/llvm-toolchain-jammy-20/InRelease  Could not resolve 'apt.llvm.org'"
        echo "W: Some index files failed to download. They have been ignored, or old ones used instead."
      else
        touch "${STUB_STATE}/fetched"
      fi
      exit 0
      ;;
    install)
      if [[ ! -e "${STUB_STATE}/fetched" ]]; then
        echo "E: Unable to locate package ${!#}" >&2
        exit 100
      fi
      if [[ " $* " == *" gnupg "* ]]; then
        printf '#!/bin/sh\nexit 0\n' >"${STUB_BIN}/gpg"
        chmod +x "${STUB_BIN}/gpg"
      fi
      exit 0
      ;;
  esac
done
STUB
  printf '#!/bin/sh\nexit 0\n' >"${stubs}/sleep"
  if [[ "$1" == with-gpg ]]; then
    printf '#!/bin/sh\nexit 0\n' >"${stubs}/gpg"
  fi
  chmod +x "${stubs}"/*

  if ! STUB_STATE="${state}" STUB_BIN="${stubs}" PATH="${stubs}:${PATH}" \
    bash "${SCRIPT}" 20 >"${state}/log" 2>&1; then
    cat "${state}/log" >&2
    fail "$1: install-llvm-apt.sh failed, index refreshed $(cat "${state}/updates") time(s)"
  fi
  cat "${state}/updates"
}

with_gpg="$(run_case with-gpg)"
without_gpg="$(run_case without-gpg)"
echo "OK: installed after a failed index fetch (index refreshed ${with_gpg} times with gpg," \
  "${without_gpg} times without)"
