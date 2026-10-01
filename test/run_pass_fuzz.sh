#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Feeds random modules from llvm-stress, which LLVM ships, through the instrumentation
# passes. cc --instrument takes LLVM IR as input and fails when the passes crash or leave
# invalid IR, which it verifies. Each module is instrumented with several sets of options;
# a module that fails is kept, with the options, for replay.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CC_BIN="${CC_BIN:-${ROOT_DIR}/build/cc}"
OUT_DIR="${1:-/tmp/ct_pass_fuzz}"
# Seeds of the generated modules: CT_FUZZ_SEEDS of them, from CT_FUZZ_FIRST_SEED on.
CT_FUZZ_SEEDS="${CT_FUZZ_SEEDS:-200}"
CT_FUZZ_FIRST_SEED="${CT_FUZZ_FIRST_SEED:-1}"

if [[ ! -x "${CC_BIN}" ]]; then
  echo "ERROR: ${CC_BIN} not found or not executable."
  echo "Build coretrace-compiler first (cmake --build build)."
  exit 1
fi

# llvm-stress: LLVM_STRESS, else the one on PATH, else the one of the LLVM found through
# LLVM_DIR (<prefix>/lib/cmake/llvm).
if [[ -z "${LLVM_STRESS:-}" ]]; then
  if command -v llvm-stress >/dev/null 2>&1; then
    LLVM_STRESS="$(command -v llvm-stress)"
  elif [[ -n "${LLVM_DIR:-}" ]]; then
    LLVM_STRESS="$(cd "${LLVM_DIR}/../../.." && pwd)/bin/llvm-stress"
  fi
fi
if [[ -z "${LLVM_STRESS:-}" || ! -x "${LLVM_STRESS}" ]]; then
  echo "ERROR: llvm-stress not found: set LLVM_STRESS, or LLVM_DIR to <prefix>/lib/cmake/llvm."
  exit 1
fi

mkdir -p "${OUT_DIR}"

FLAG_SETS=(
  "--ct-modules=all --ct-autofree"
  "-O2 --ct-modules=all --ct-autofree"
  "--ct-modules=all --ct-vcall-trace --ct-vtable-diag --ct-shadow"
)

RUNS=0
FAILURES=0
last_seed=$((CT_FUZZ_FIRST_SEED + CT_FUZZ_SEEDS - 1))
for seed in $(seq "${CT_FUZZ_FIRST_SEED}" "${last_seed}"); do
  # Sizes from 50 to 1999 instructions, spread over the seeds.
  size=$((50 + (seed * 37) % 1950))
  module="${OUT_DIR}/stress.ll"
  "${LLVM_STRESS}" -seed="${seed}" -size="${size}" -o "${module}"
  for flags in "${FLAG_SETS[@]}"; do
    RUNS=$((RUNS + 1))
    # shellcheck disable=SC2086
    if ! "${CC_BIN}" --instrument ${flags} -c "${module}" -o "${OUT_DIR}/stress.o" \
      >"${OUT_DIR}/compile.log" 2>&1; then
      FAILURES=$((FAILURES + 1))
      cp "${module}" "${OUT_DIR}/seed_${seed}.ll"
      echo "FAIL: seed ${seed}, size ${size}, ${flags} (module in ${OUT_DIR}/seed_${seed}.ll)"
      grep -v "overriding the module target triple" "${OUT_DIR}/compile.log" | head -5
    fi
  done
done

echo ""
echo "Summary: ${RUNS} instrumented modules, ${FAILURES} failed"
[[ "${FAILURES}" -eq 0 ]]
