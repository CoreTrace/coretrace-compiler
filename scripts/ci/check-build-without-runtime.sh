#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Builds compilerlib and cc with CORETRACE_COMPILER_BUILD_RUNTIME=OFF and checks what
# consumers that only embed compilerlib rely on: coretrace-log is not fetched, the
# libraries, cc and the compilerlib unit tests build and pass, and --instrument needs a
# runtime built elsewhere, found through CT_RUNTIME_LIB_DIR.
#
# usage: check-build-without-runtime.sh <work dir> [cmake arguments...]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK="$1"
shift
BUILD="${WORK}/build"
mkdir -p "${WORK}"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

cmake -S "${ROOT}" -B "${BUILD}" -DCORETRACE_COMPILER_BUILD_RUNTIME=OFF "$@"
[[ ! -e "${BUILD}/_deps/coretrace_logger-src" ]] || fail "coretrace-log was fetched"

cmake --build "${BUILD}" --target compilerlib_static compilerlib_shared cc compilerlib_unit_tests \
  --parallel
ctest --test-dir "${BUILD}" --output-on-failure

printf 'int main(void) { return 0; }\n' > "${WORK}/hello.c"
if "${BUILD}/cc" --instrument "${WORK}/hello.c" -o "${WORK}/hello" 2> "${WORK}/instrument.err"; then
  fail "--instrument linked without a runtime"
fi
grep -q "instrumentation runtime archives not found" "${WORK}/instrument.err" ||
  fail "unexpected --instrument error: $(cat "${WORK}/instrument.err")"

# A runtime built by a separate, default configuration.
cmake -S "${ROOT}" -B "${WORK}/runtime-build" -DCORETRACE_BUILD_UNIT_TESTS=OFF "$@"
cmake --build "${WORK}/runtime-build" --target ct_instrument_runtime coretrace_logger --parallel
mkdir -p "${WORK}/runtime"
find "${WORK}/runtime-build" \( -name '*ct_instrument_runtime.*' -o -name '*coretrace_logger.*' \) \
  \( -name '*.a' -o -name '*.lib' \) -exec cp {} "${WORK}/runtime/" \;
CT_RUNTIME_LIB_DIR="${WORK}/runtime" "${BUILD}/cc" --instrument "${WORK}/hello.c" -o "${WORK}/hello"
"${WORK}/hello"

echo "OK: compilerlib and cc build without the runtime, which CT_RUNTIME_LIB_DIR supplies"
