#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Builds the probe of #187 with the given C runtime (static, as cc links, or dll) and runs it.
set -uo pipefail
crt="$1"
flags=()
[[ "${crt}" == dll ]] && flags=(-fms-runtime-lib=dll)
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="$(mktemp -d)"
clang --version | head -1
build() { clang ${flags[@]+"${flags[@]}"} -O1 "$@" || exit 1; }
build -shared "${here}/probe_user.c" -o "${out}/probe_user.dll"
build -shared "${here}/probe_helper.c" -o "${out}/probe_helper.dll"
# The import order decides which DLL the loader initializes first, and detaches last.
build "${here}/probe_exe.c" "${out}/probe_helper.lib" "${out}/probe_user.lib" -o "${out}/helper_first.exe"
build "${here}/probe_exe.c" "${out}/probe_user.lib" "${out}/probe_helper.lib" -o "${out}/user_first.exe"
for checks in none heap stdio; do
  for exe in helper_first user_first; do
    for mode in return exit exitprocess; do
      echo "=== ${crt} CRT, ${exe}, ${mode}, checks: ${checks}"
      PROBE_CHECKS="${checks}" timeout 20 "${out}/${exe}.exe" "${mode}" 2>&1
      echo "status=$?"
    done
  done
done
# Threads that allocate while the process exits: a callback after ExitProcess may find a
# heap lock held by a killed thread.
for exe in helper_first user_first; do
  hangs=0
  crashes=0
  for _ in $(seq 40); do
    PROBE_CHECKS=heap timeout 20 "${out}/${exe}.exe" exit churn >"${out}/churn.log" 2>&1
    status=$?
    [[ ${status} -eq 124 ]] && hangs=$((hangs + 1))
    [[ ${status} -ne 0 && ${status} -ne 124 ]] && crashes=$((crashes + 1))
  done
  echo "=== ${crt} CRT, ${exe}, exit with allocating threads, checks: heap: ${hangs}/40 hung, ${crashes}/40 crashed; last run:"
  cat "${out}/churn.log"
done
