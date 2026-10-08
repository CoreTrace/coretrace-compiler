#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Compiles and runs the runtime fixtures under test/ other than the auto-free ones
# (alloc tracking, new/delete variants, shadow memory, threads, vtable diagnostics) and
# checks their exit code and diagnostics.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Git Bash, on Windows, names the system MINGW64_NT-<version>.
case "$(uname -s)" in
  MINGW*|MSYS*) ON_WINDOWS=1 ;;
  *) ON_WINDOWS=0 ;;
esac
if [[ "${ON_WINDOWS}" -eq 1 ]]; then
  # Where scripts/build-windows.ps1 builds it by default.
  CC_BIN="${CC_BIN:-${ROOT_DIR}/build-win/Release/cc.exe}"
else
  CC_BIN="${CC_BIN:-${ROOT_DIR}/build/cc}"
fi
OUT_DIR="${1:-/tmp/ct_runtime_tests}"
# Optimization flag the fixtures are built with, for instance -O2. Empty keeps the
# driver's default, -O0.
CT_TEST_OPT="${CT_TEST_OPT:-}"

if [[ ! -x "${CC_BIN}" ]]; then
  echo "ERROR: ${CC_BIN} not found or not executable."
  echo "Build coretrace-compiler first (cmake --build build)."
  exit 1
fi

mkdir -p "${OUT_DIR}"

# shellcheck source-path=SCRIPTDIR source=scripts/run_with_timeout.sh
source "${ROOT_DIR}/test/scripts/run_with_timeout.sh"

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
    # Clang enables sized deallocation by default only from version 19 on.
    ct_new_delete_sized.cpp) echo "--ct-modules=alloc -fsized-deallocation" ;;
    # Without a log line per allocation: that is a million lines here.
    ct_threads_stress.c) echo "--ct-modules=alloc,bounds --ct-no-alloc-trace" ;;
    ct_fork_threads.c) echo "--ct-modules=alloc --ct-no-alloc-trace" ;;
    *)                 echo "--ct-modules=alloc" ;;
  esac
}

# Units built apart from a fixture, which run exit-time code the instrumentation does not
# see: "plain:<file>" is compiled without --instrument and linked before the fixture,
# "shared:<file>" is built into a shared library, without --instrument, that the fixture
# links with.
companion_for() {
  case "$1" in
    ct_leak_exit_uninstrumented_first.cpp) echo "plain:ct_leak_exit_uninstrumented_first_plain.cpp" ;;
    ct_leak_exit_shared_library.cpp) echo "shared:ct_leak_exit_shared_library_lib.cpp" ;;
    *) echo "" ;;
  esac
}

# Functions a "shared:" companion exports on Windows, where a DLL exports only the
# functions it names.
companion_exports_for() {
  case "$1" in
    ct_leak_exit_shared_library.cpp) echo "ct_exit_library_register" ;;
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
    ct_threads_stress.c) echo 8 ;;                  # one per thread
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
    ct_bounds_heap_use_after_free.c) echo "heap-use-after-free" ;;
    ct_bounds_stack_overflow.c|ct_bounds_stack_callee.c|ct_bounds_stack_container_of.c|\
    ct_bounds_stack_default_modules.c|ct_bounds_stack_longjmp.c)
      echo "stack-buffer-overflow" ;;
    ct_new_delete.cpp) echo "tracing-new-unreachable" ;;
    # Leaks and double frees name where the memory came from; a double free also names
    # where it happens.
    ct_leak_site.c) echo 'ct: leak ptr=.* alloc_site=[^ ]*ct_leak_site\.c:8:' ;;
    ct_double_free_site.c)
      echo 'tracing-free ptr=.* (double free) site=[^ ]*ct_double_free_site\.c:10:5 alloc_site=[^ ]*ct_double_free_site\.c:8:' ;;
    ct_double_delete_site.cpp)
      echo 'tracing-delete-array ptr=.* (double free) site=[^ ]*ct_double_delete_site\.cpp:8:5 alloc_site=[^ ]*ct_double_delete_site\.cpp:6:' ;;
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
    ct_new_delete_library.cpp) echo "owned=7 array=4 pointers=16 counts=5 shared=64" ;;
    ct_bounds_stack_longjmp.c) echo "recovered=1000" ;;
    ct_bounds_stack_deep_valid.c) echo "again=" ;;
    ct_bounds_freed_address_reuse.c) echo "sum=" ;;
    ct_leak_static_destructor.cpp|ct_leak_destructor_function.c|\
    ct_leak_exit_uninstrumented_first.cpp|ct_leak_exit_shared_library.cpp) echo "ok" ;;
    ct_threads_stress.c) echo "damaged=0" ;;
    ct_fork_threads.c) echo "hung=0" ;;
    *) echo "" ;;
  esac
}

# Substring that must NOT appear on stderr for any fixture, except the fixture whose
# expect_stderr is that exact diagnostic.
FORBIDDEN_STDERR=("heap-buffer-overflow" "heap-use-after-free" "stack-buffer-overflow"
                  "mutex lock failed" "terminating due to")

# Fixtures whose failure is a known, tracked defect, and the one failure expected of them,
# as check_one classifies it. Only that failure is reported as XFAIL: any other one, such
# as a compile error, a timeout, a crash or another wrong output, still fails the suite.
# An unexpected pass is reported as XPASS and fails the suite too, so that the entry is
# removed once the defect is fixed.
#   leak-report: the program runs correctly, but the leak report lists blocks the fixture
#                allocated, and only those.
known_failure() {
  case "$1" in
  esac
  return 1
}

# Fixtures that cannot be checked deterministically.
skip_reason() {
  case "$1" in
    ct_vtable_uaf.cpp)
      echo "reads a freed object; behaviour depends on the allocator, not checkable"
      return 0
      ;;
  esac
  # Built with ThreadSanitizer on Linux (its CI job), the fixtures whose scenario its runtime
  # changes there. Every other build checks them.
  if [[ "$(uname -s)" == Linux && " ${CT_TEST_OPT:-} " == *" -fsanitize=thread "* ]]; then
    case "$1" in
      ct_bounds_container_of_underflow.c|ct_bounds_container_of_underflow_shadow.c)
        echo "reads before its block, which ThreadSanitizer's allocator leaves unmapped on Linux"
        return 0
        ;;
      ct_vtable_diag_mismatch.cpp)
        echo "calls puts from another module, which ThreadSanitizer intercepts in the program"
        return 0
        ;;
    esac
  fi
  if [[ "${ON_WINDOWS}" -eq 1 ]]; then
    case "$1" in
      ct_threads_stress.c)
        echo "uses pthreads, which Windows does not provide"
        return 0
        ;;
      ct_fork_threads.c)
        echo "uses fork and pthreads, which Windows does not provide"
        return 0
        ;;
      ct_vtable_diag_mismatch.cpp)
        echo "uses dlfcn.h, which Windows does not provide"
        return 0
        ;;
      ct_leak_exit_shared_library.cpp)
        echo "the leak report runs before the exit-time code of DLLs (#187)"
        return 0
        ;;
      ct_alloc_basic.c|ct_new_delete.cpp)
        echo "the Windows runtime does not report unreachable allocations (#188)"
        return 0
        ;;
      ct_double_free_site.c|ct_double_delete_site.cpp)
        echo "the Windows runtime words a double free differently, and its sites have no column (#188)"
        return 0
        ;;
      ct_leak_site.c)
        echo "Windows sites have no column (#188)"
        return 0
        ;;
    esac
  fi
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
  ct_bounds_stack_longjmp.c
  ct_bounds_stack_deep_valid.c
  ct_bounds_freed_address_reuse.c
  ct_bounds_heap_use_after_free.c
  ct_realloc_zero.c
  ct_threads_stress.c
  ct_fork_threads.c
  ct_new_delete.cpp
  ct_new_delete_sized.cpp
  ct_new_delete_variants.cpp
  ct_new_delete_library.cpp
  ct_double_delete_site.cpp
  ct_leak_static_destructor.cpp
  ct_leak_destructor_function.c
  ct_leak_exit_uninstrumented_first.cpp
  ct_leak_exit_shared_library.cpp
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

# Why the last check_one failed: "leak-report" (see known_failure), or "other".
CHECK_FAILURE=""

check_one() {
  local test_file="$1"
  CHECK_FAILURE=other
  local base="${test_file%.*}"
  local bin="${OUT_DIR}/${base}"
  local compile_log="${OUT_DIR}/${base}.compile.log"
  local out_log="${OUT_DIR}/${base}.out.log"
  local err_log="${OUT_DIR}/${base}.err.log"
  local flags
  flags="$(flags_for "${test_file}")"

  local before=()
  local after=()
  local companion
  companion="$(companion_for "${test_file}")"
  if [[ -n "${companion}" ]]; then
    local unit="${ROOT_DIR}/test/${companion#*:}"
    case "${companion}" in
      plain:*)
        # shellcheck disable=SC2086
        "${CC_BIN}" ${CT_TEST_OPT:+"${CT_TEST_OPT}"} -c "${unit}" -o "${OUT_DIR}/${base}_plain.o" \
          >"${compile_log}" 2>&1 || {
            echo "  companion compile failed (see ${compile_log})"
            return 1
          }
        before=("${OUT_DIR}/${base}_plain.o")
        ;;
      shared:*)
        local library="${OUT_DIR}/lib${base}.so"
        local library_flags=(-fPIC)
        [[ "$(uname -s)" == Darwin ]] && library="${OUT_DIR}/lib${base}.dylib"
        after=("${library}" "-Wl,-rpath,${OUT_DIR}")
        if [[ "${ON_WINDOWS}" -eq 1 ]]; then
          # Clang rejects -fPIC for the MSVC target. The program links with the DLL's
          # import library, and loads the DLL from its own directory.
          library="${OUT_DIR}/lib${base}.dll"
          library_flags=()
          local symbol
          for symbol in $(companion_exports_for "${test_file}"); do
            library_flags+=("-Wl,-export:${symbol}")
          done
          after=("${OUT_DIR}/lib${base}.lib")
        fi
        # shellcheck disable=SC2086
        "${CC_BIN}" ${CT_TEST_OPT:+"${CT_TEST_OPT}"} -shared ${library_flags[@]+"${library_flags[@]}"} \
          "${unit}" -o "${library}" >"${compile_log}" 2>&1 || {
            echo "  companion compile failed (see ${compile_log})"
            return 1
          }
        ;;
    esac
  fi

  # shellcheck disable=SC2086
  "${CC_BIN}" --instrument ${CT_TEST_OPT:+"${CT_TEST_OPT}"} ${flags} \
    ${before[@]+"${before[@]}"} "${ROOT_DIR}/test/${test_file}" ${after[@]+"${after[@]}"} \
    -o "${bin}" >>"${compile_log}" 2>&1 || {
      echo "  compile failed (see ${compile_log})"
      return 1
    }

  set +e
  run_with_timeout "${bin}" >"${out_log}" 2>"${err_log}"
  local run_rc=$?
  set -e
  # Before any other check: a fixture may accept the status ThreadSanitizer exits with, and
  # a child process's report reaches the log while the parent still exits with 0.
  if grep -q "ThreadSanitizer" "${err_log}"; then
    echo "  ThreadSanitizer report (see ${err_log})"
    return 1
  fi
  if [[ "${run_rc}" -eq 124 ]]; then
    echo "  no exit after ${RUN_TIMEOUT_SECONDS}s, killed (see ${err_log})"
    return 1
  fi

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

  # An unexpected leak report is reported after the other checks, which must pass for it
  # to be the only failure.
  local leak_report=0
  local leaks
  leaks="$(expect_leaks "${test_file}")"
  case "${leaks}" in
    none)
      if grep -q "ct: leaks detected" "${err_log}"; then
        leak_report=1
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
  if [[ "${leak_report}" -eq 1 ]]; then
    echo "  unexpected leak report (see ${err_log})"
    # Only blocks the fixture allocated: a leak from elsewhere is another defect.
    local reported fixture_blocks
    reported="$(grep -c "ct: leak ptr=" "${err_log}" || true)"
    fixture_blocks="$(grep -c "ct: leak ptr=.* alloc_site=[^ ]*${test_file}:" "${err_log}" || true)"
    if [[ "${reported}" -gt 0 && "${reported}" -eq "${fixture_blocks}" ]]; then
      CHECK_FAILURE=leak-report
    fi
    return 1
  fi
  CHECK_FAILURE=""
  return 0
}

for t in "${TESTS[@]}"; do
  echo "==> ${t}"
  if reason="$(skip_reason "${t}")"; then
    echo "  SKIP: ${reason}"
    SKIP=$((SKIP + 1))
    continue
  fi
  if expected="$(known_failure "${t}")"; then
    if check_one "${t}"; then
      echo "  XPASS: known failure now passes, remove it from known_failure"
      XPASS=$((XPASS + 1))
      continue
    fi
    if [[ "${CHECK_FAILURE}" == "${expected}" ]]; then
      echo "  XFAIL (${expected})"
      XFAIL=$((XFAIL + 1))
      continue
    fi
    echo "  expected only a ${expected} failure, got another one"
  elif check_one "${t}"; then
    echo "  OK"
    PASS=$((PASS + 1))
    continue
  fi
  echo "  FAIL"
  FAIL=$((FAIL + 1))
  # The run's stderr stays on the machine that ran the suite: show its end, for CI.
  err_log="${OUT_DIR}/${t%.*}.err.log"
  if [[ -f "${err_log}" ]]; then
    tail -n 20 "${err_log}" | sed 's/^/    | /'
  fi
done

echo ""
echo "Summary: ${PASS} passed, ${FAIL} failed, ${SKIP} skipped, ${XFAIL} expected failures, ${XPASS} unexpected passes"
[[ "${FAIL}" -eq 0 && "${XPASS}" -eq 0 ]]
