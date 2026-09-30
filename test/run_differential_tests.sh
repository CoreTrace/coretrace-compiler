#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Checks that instrumentation preserves the behaviour of programs without memory errors.
# Each program is built twice, without instrumentation and with every module and
# auto-free, and both builds are run: they must print the same standard output and exit
# with the same code, and the instrumented run must report nothing. These programs free
# everything they allocate, so an auto-free or a double free there means CoreTrace
# released a block the program still owned.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CC_BIN="${CC_BIN:-${ROOT_DIR}/build/cc}"
OUT_DIR="${1:-/tmp/ct_differential_tests}"
# Optimization flag both builds use, for instance -O2. Empty keeps the driver's
# default, -O0.
CT_TEST_OPT="${CT_TEST_OPT:-}"

if [[ ! -x "${CC_BIN}" ]]; then
  echo "ERROR: ${CC_BIN} not found or not executable."
  echo "Build coretrace-compiler first (cmake --build build)."
  exit 1
fi

mkdir -p "${OUT_DIR}"

INSTRUMENT_FLAGS=(--instrument --ct-modules=all --ct-autofree)

# Diagnostics the instrumented run must not print.
FORBIDDEN_STDERR=("==ct== [ERROR]" "auto-free ptr=" "(double free)")

# Paths under test/: ordinary programs, then the runtime fixtures that contain no
# memory error.
TESTS=(
  differential/linked_list.c
  differential/hash_table.c
  differential/pointer_hiding.c
  differential/text_buffer.c
  differential/matrix.c
  differential/error_unwinding.c
  differential/containers.cpp
  differential/local_variables.c
  ct_alloc_growth.c
  ct_bounds_container_of_valid.c
  ct_bounds_stack_valid.c
  ct_bounds_stack_unwind_valid.cpp
  ct_new_delete_sized.cpp
  ct_new_delete_variants.cpp
  ct_vtable_basic.cpp
  ct_vtable_interface.cpp
  ct_vtable_multi.cpp
  ct_vtable_virtual_base.cpp
)

# Known defects, each tracked by an issue, matched as
# <test>:<system>-<machine>:<optimization>. The program still runs and its failure is
# reported as XFAIL; an unexpected pass is reported as XPASS and fails the suite, so the
# entry gets removed once the defect is fixed.
known_failure() {
  case "$1:$(uname -s)-$(uname -m):${CT_TEST_OPT:--O0}" in
  esac
  return 1
}

PASS=0
FAIL=0
XFAIL=0
XPASS=0

# Builds one variant of a program and runs it; its outputs land in ${OUT_DIR}/<base>.<variant>.*
build_and_run() {
  local source="$1"
  local prefix="$2"
  shift 2

  "${CC_BIN}" "$@" ${CT_TEST_OPT:+"${CT_TEST_OPT}"} "${source}" -o "${prefix}" \
    >"${prefix}.compile.log" 2>&1 || {
      echo "  compile failed (see ${prefix}.compile.log)"
      return 1
    }

  set +e
  "${prefix}" >"${prefix}.out.log" 2>"${prefix}.err.log"
  echo $? >"${prefix}.rc"
  set -e
}

check_one() {
  local test_file="$1"
  local source="${ROOT_DIR}/test/${test_file}"
  local base
  base="$(basename "${test_file%.*}")"
  local plain="${OUT_DIR}/${base}.plain"
  local instrumented="${OUT_DIR}/${base}.instrumented"

  build_and_run "${source}" "${plain}" || return 1
  build_and_run "${source}" "${instrumented}" "${INSTRUMENT_FLAGS[@]}" || return 1

  local plain_rc instrumented_rc
  plain_rc="$(<"${plain}.rc")"
  instrumented_rc="$(<"${instrumented}.rc")"
  if [[ "${plain_rc}" -ne "${instrumented_rc}" ]]; then
    echo "  exit ${instrumented_rc} instrumented, ${plain_rc} plain (see ${instrumented}.err.log)"
    return 1
  fi

  if ! cmp -s "${plain}.out.log" "${instrumented}.out.log"; then
    echo "  stdout differs:"
    diff "${plain}.out.log" "${instrumented}.out.log" | head -n 10 | sed 's/^/    /'
    return 1
  fi

  for needle in "${FORBIDDEN_STDERR[@]}"; do
    if grep -q -F -- "${needle}" "${instrumented}.err.log"; then
      echo "  instrumented stderr contains '${needle}' (see ${instrumented}.err.log)"
      return 1
    fi
  done
  return 0
}

for t in "${TESTS[@]}"; do
  echo "==> ${t}"
  if known_failure "${t}"; then
    if check_one "${t}"; then
      echo "  XPASS: known failure now passes, remove it from known_failure"
      XPASS=$((XPASS + 1))
    else
      echo "  XFAIL"
      XFAIL=$((XFAIL + 1))
    fi
    continue
  fi
  if check_one "${t}"; then
    echo "  OK"
    PASS=$((PASS + 1))
  else
    echo "  FAIL"
    FAIL=$((FAIL + 1))
  fi
done

echo ""
echo "Summary: ${PASS} passed, ${FAIL} failed, ${XFAIL} expected failures, ${XPASS} unexpected passes"
[[ "${FAIL}" -eq 0 && "${XPASS}" -eq 0 ]]
