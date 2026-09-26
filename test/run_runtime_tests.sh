#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Compiles and runs the runtime fixtures under test/ that are not covered by
# run_autofree_tests.sh (alloc tracking, new/delete variants, shadow memory,
# vtable diagnostics) and checks their exit code and diagnostics.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CC_BIN="${CC_BIN:-${ROOT_DIR}/build/cc}"
OUT_DIR="${1:-/tmp/ct_runtime_tests}"

if [[ ! -x "${CC_BIN}" ]]; then
  echo "ERROR: ${CC_BIN} not found or not executable."
  echo "Build coretrace-compiler first (cmake --build build)."
  exit 1
fi

mkdir -p "${OUT_DIR}"

# Instrumentation flags per fixture.
flags_for() {
  case "$1" in
    ct_shadow_pages.c) echo "--ct-modules=alloc,bounds --ct-shadow" ;;
    # Valid accesses: built to abort on any bounds error, so a false positive fails.
    ct_bounds_*_valid_shadow.c) echo "--ct-modules=alloc,bounds --ct-shadow-aggressive" ;;
    ct_bounds_*_valid.c|ct_bounds_*_valid.cpp) echo "--ct-modules=alloc,bounds" ;;
    # Invalid accesses: reported and survived, so the program exits normally.
    ct_bounds_*_shadow.c) echo "--ct-modules=alloc,bounds --ct-bounds-no-abort --ct-shadow-aggressive" ;;
    # The default modules, trace included.
    ct_bounds_stack_default_modules.c) echo "--ct-bounds-no-abort" ;;
    ct_bounds_*.c)        echo "--ct-modules=alloc,bounds --ct-bounds-no-abort" ;;
    ct_vtable_*.cpp)   echo "--ct-modules=alloc,vtable --ct-vtable-diag" ;;
    *)                 echo "--ct-modules=alloc" ;;
  esac
}

# Expected exit code of the instrumented program.
expect_exit() {
  echo 0
}

# Expected leak report: "none" (no report), "1" (exactly one deliberate leak),
# or "any" (libc-dependent, not checked).
expect_leaks() {
  case "$1" in
    ct_alloc_basic.c|ct_new_delete.cpp) echo 1 ;;   # deliberate unreachable allocation
    ct_leak_site.c) echo 1 ;;
    ct_realloc_zero.c) echo any ;;                  # realloc(p, 0) may allocate or free per libc
    *) echo none ;;
  esac
}

# Substring that must appear on stderr, as a grep basic regular expression.
expect_stderr() {
  case "$1" in
    ct_alloc_basic.c) echo "tracing-malloc-unreachable" ;;
    ct_bounds_container_of_underflow.c) echo "heap-buffer-overflow" ;;
    ct_bounds_container_of_underflow_shadow.c) echo "heap-buffer-overflow" ;;
    ct_bounds_container_of_overflow.c) echo "heap-buffer-overflow" ;;
    ct_bounds_stack_overflow.c|ct_bounds_stack_callee.c|ct_bounds_stack_container_of.c|\
    ct_bounds_stack_default_modules.c)
      echo "stack-buffer-overflow" ;;
    ct_new_delete.cpp) echo "tracing-new-unreachable" ;;
    # Leaks and double frees name where the memory came from.
    ct_leak_site.c) echo 'ct: leak ptr=.* alloc_site=[^ ]*ct_leak_site\.c:8:' ;;
    ct_double_free_site.c)
      echo 'tracing-free ptr=.* (double free) alloc_site=[^ ]*ct_double_free_site\.c:8:' ;;
    ct_double_delete_site.cpp)
      echo 'tracing-delete-array ptr=.* (double free) alloc_site=[^ ]*ct_double_delete_site\.cpp:6:' ;;
    ct_vtable_diag_null.cpp) echo "null this pointer" ;;
    ct_vtable_diag_fake.cpp) echo "vtable resolve failed" ;;
    ct_vtable_diag_freed.cpp) echo "vptr on freed object" ;;
    ct_vtable_diag_mismatch.cpp) echo "module mismatch" ;;
    ct_vtable_diag_stack_target.cpp) echo "target in non-exec memory" ;;
    *) echo "" ;;
  esac
}

# Substring that must appear on stdout.
expect_stdout() {
  case "$1" in
    ct_vtable_basic.cpp) echo "value=2" ;;
    ct_vtable_interface.cpp) echo "run=21" ;;
    ct_vtable_multi.cpp) echo "Derived 42" ;;
    ct_vtable_virtual_base.cpp) echo "value=99" ;;
    *) echo "" ;;
  esac
}

# Substring that must NOT appear on stderr for any fixture, except the fixture whose
# expect_stderr is that exact diagnostic.
FORBIDDEN_STDERR=("heap-buffer-overflow" "stack-buffer-overflow" "mutex lock failed"
                  "terminating due to")

# Fixtures whose failure is a known, tracked defect. The suite still runs them and
# reports XFAIL; an unexpected pass is reported as XPASS and fails the suite so the
# entry gets removed once the defect is fixed.
known_failure() {
  case "$1" in
    *) return 1 ;;
  esac
}

# Fixtures that cannot be checked deterministically.
skip_reason() {
  case "$1" in
    ct_vtable_uaf.cpp)
      echo "reads a freed object; behaviour depends on the allocator, not checkable"
      return 0
      ;;
  esac
  return 1
}

TESTS=(
  ct_alloc_basic.c
  ct_alloc_growth.c
  ct_leak_site.c
  ct_double_free_site.c
  ct_bounds_container_of_underflow.c
  ct_bounds_container_of_underflow_shadow.c
  ct_bounds_container_of_overflow.c
  ct_bounds_container_of_valid.c
  ct_bounds_container_of_valid_shadow.c
  ct_bounds_stack_overflow.c
  ct_bounds_stack_default_modules.c
  ct_bounds_stack_callee.c
  ct_bounds_stack_container_of.c
  ct_bounds_stack_valid.c
  ct_bounds_stack_unwind_valid.cpp
  ct_realloc_zero.c
  ct_new_delete.cpp
  ct_new_delete_sized.cpp
  ct_new_delete_variants.cpp
  ct_double_delete_site.cpp
  ct_shadow_pages.c
  ct_vtable_basic.cpp
  ct_vtable_interface.cpp
  ct_vtable_multi.cpp
  ct_vtable_virtual_base.cpp
  ct_vtable_diag_null.cpp
  ct_vtable_diag_fake.cpp
  ct_vtable_diag_freed.cpp
  ct_vtable_diag_mismatch.cpp
  ct_vtable_diag_stack_target.cpp
  ct_vtable_uaf.cpp
)

PASS=0
FAIL=0
SKIP=0
XFAIL=0
XPASS=0

check_one() {
  local test_file="$1"
  local base="${test_file%.*}"
  local bin="${OUT_DIR}/${base}"
  local compile_log="${OUT_DIR}/${base}.compile.log"
  local out_log="${OUT_DIR}/${base}.out.log"
  local err_log="${OUT_DIR}/${base}.err.log"
  local flags
  flags="$(flags_for "${test_file}")"

  # shellcheck disable=SC2086
  "${CC_BIN}" --instrument ${flags} "${ROOT_DIR}/test/${test_file}" -o "${bin}" \
    >"${compile_log}" 2>&1 || {
      echo "  compile failed (see ${compile_log})"
      return 1
    }

  set +e
  "${bin}" >"${out_log}" 2>"${err_log}"
  local run_rc=$?
  set -e

  local want_rc
  want_rc="$(expect_exit "${test_file}")"
  if [[ "${run_rc}" -ne "${want_rc}" ]]; then
    echo "  expected exit ${want_rc}, got ${run_rc} (see ${err_log})"
    return 1
  fi

  local expected_stderr
  expected_stderr="$(expect_stderr "${test_file}")"
  for needle in "${FORBIDDEN_STDERR[@]}"; do
    if [[ "${needle}" == "${expected_stderr}" ]]; then
      continue
    fi
    if grep -q -- "${needle}" "${err_log}"; then
      echo "  stderr contains forbidden '${needle}' (see ${err_log})"
      return 1
    fi
  done

  local leaks
  leaks="$(expect_leaks "${test_file}")"
  case "${leaks}" in
    none)
      if grep -q "ct: leaks detected" "${err_log}"; then
        echo "  unexpected leak report (see ${err_log})"
        return 1
      fi
      ;;
    any) ;;
    *)
      if ! grep -q "ct: leaks detected count=${leaks}\b" "${err_log}"; then
        echo "  expected 'ct: leaks detected count=${leaks}' (see ${err_log})"
        return 1
      fi
      ;;
  esac

  local needle
  needle="$(expect_stderr "${test_file}")"
  if [[ -n "${needle}" ]] && ! grep -q -- "${needle}" "${err_log}"; then
    echo "  stderr does not contain '${needle}' (see ${err_log})"
    return 1
  fi
  needle="$(expect_stdout "${test_file}")"
  if [[ -n "${needle}" ]] && ! grep -q -- "${needle}" "${out_log}"; then
    echo "  stdout does not contain '${needle}' (see ${out_log})"
    return 1
  fi
  return 0
}

for t in "${TESTS[@]}"; do
  echo "==> ${t}"
  if reason="$(skip_reason "${t}")"; then
    echo "  SKIP: ${reason}"
    SKIP=$((SKIP + 1))
    continue
  fi
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
echo "Summary: ${PASS} passed, ${FAIL} failed, ${SKIP} skipped, ${XFAIL} expected failures, ${XPASS} unexpected passes"
[[ "${FAIL}" -eq 0 && "${XPASS}" -eq 0 ]]
