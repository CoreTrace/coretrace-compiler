#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CC_BIN="${ROOT_DIR}/build/cc"
OUT_DIR="${1:-/tmp/ct_autofree_tests}"

if command -v rg >/dev/null 2>&1; then
  MATCH_TOOL="rg"
else
  MATCH_TOOL="grep"
fi

has_match() {
  local pattern="$1"
  local file="$2"
  if [[ "${MATCH_TOOL}" == "rg" ]]; then
    rg -q "${pattern}" "${file}"
  else
    grep -q "${pattern}" "${file}"
  fi
}

if [[ ! -x "${CC_BIN}" ]]; then
  echo "ERROR: ${CC_BIN} not found or not executable."
  echo "Build coretrace-compiler first (cmake --build build)."
  exit 1
fi

mkdir -p "${OUT_DIR}"

expect_leak() {
  case "$1" in
    ct_autofree_local.c|ct_autofree_select_escape.c|ct_autofree_ptrtoint_escape.c|ct_autofree_inttoptr_escape.c)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

expect_autofree() {
  case "$1" in
    ct_autofree_return_unused.c|ct_autofree_select.c|ct_autofree_ptrtoint.c|ct_autofree_inttoptr.c|ct_autofree_new_nothrow.cpp|ct_autofree_posix_memalign.c|ct_autofree_aligned_alloc.c|ct_autofree_mmap.c|ct_autofree_sbrk.c)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

expect_nonzero_exit() {
  case "$1" in
    ct_autofree_select_escape.c|ct_autofree_ptrtoint_escape.c|ct_autofree_inttoptr_escape.c)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

TESTS=(
  ct_autofree_local.c
  ct_autofree_return_unused.c
  ct_autofree_select.c
  ct_autofree_select_escape.c
  ct_autofree_ptrtoint.c
  ct_autofree_ptrtoint_escape.c
  ct_autofree_inttoptr.c
  ct_autofree_inttoptr_escape.c
  ct_autofree_new_nothrow.cpp
  ct_autofree_posix_memalign.c
  ct_autofree_aligned_alloc.c
  ct_autofree_mmap.c
  ct_autofree_sbrk.c
  ct_autofree_brk.c
)

PASS=0
FAIL=0

run_one() {
  local test_file="$1"
  local test_path="${ROOT_DIR}/test/${test_file}"
  local base="${test_file%.*}"
  local bin="${OUT_DIR}/${base}"
  local compile_log="${OUT_DIR}/${base}.compile.log"
  local run_log="${OUT_DIR}/${base}.run.log"

  echo "==> ${test_file}"

  "${CC_BIN}" --instrument --ct-modules=trace,alloc --ct-autofree \
    "${test_path}" -o "${bin}" >"${compile_log}" 2>&1 || {
      echo "  FAIL: compile (see ${compile_log})"
      return 1
    }

  set +e
  "${bin}" >"${run_log}" 2>&1
  local run_rc=$?
  set -e

  if expect_nonzero_exit "${test_file}"; then
    if [[ "${run_rc}" -eq 0 ]]; then
      echo "  FAIL: expected non-zero exit, got 0"
      return 1
    fi
  else
    if [[ "${run_rc}" -ne 0 ]]; then
      echo "  FAIL: run (see ${run_log})"
      return 1
    fi
  fi

  if expect_leak "${test_file}"; then
    if ! has_match "ct: leaks detected" "${run_log}"; then
      echo "  FAIL: expected leak, none found"
      return 1
    fi
  else
    if has_match "ct: leaks detected" "${run_log}"; then
      echo "  FAIL: unexpected leak detected"
      return 1
    fi
  fi

  if expect_autofree "${test_file}"; then
    if ! has_match "auto-free ptr=" "${run_log}"; then
      echo "  FAIL: expected auto-free log, none found"
      return 1
    fi
  fi

  echo "  OK"
  return 0
}

for t in "${TESTS[@]}"; do
  if run_one "${t}"; then
    PASS=$((PASS + 1))
  else
    FAIL=$((FAIL + 1))
  fi
done

echo ""
echo "Summary: ${PASS} passed, ${FAIL} failed"
[[ "${FAIL}" -eq 0 ]]
