# SPDX-License-Identifier: Apache-2.0
# shellcheck shell=bash
# Sourced by the test runners.

# A run longer than this is a hang: the program is killed and its test fails.
RUN_TIMEOUT_SECONDS=60

# Runs a command, killed after RUN_TIMEOUT_SECONDS. Returns the command's status, or 124
# on timeout, as timeout(1) does, which macOS lacks. Redirections given to the call apply
# to the command.
run_with_timeout() {
  "$@" &
  local pid=$!
  local waited=0
  while kill -0 "${pid}" 2>/dev/null; do
    if [[ "${waited}" -ge $((RUN_TIMEOUT_SECONDS * 10)) ]]; then
      kill -9 "${pid}" 2>/dev/null || true
      wait "${pid}" 2>/dev/null || true
      return 124
    fi
    sleep 0.1
    waited=$((waited + 1))
  done
  wait "${pid}"
}
