#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Fails unless the LLVM and the Clang installed under a prefix have the expected major
# version. A package can hold another version than its name says (a Homebrew llvm@19 was
# seen holding 20.1.2), and a CI job must test the version it is named after.
#
# usage: check-llvm-version.sh <major> <LLVM prefix>
set -euo pipefail

EXPECTED="${1:?usage: check-llvm-version.sh <major> <LLVM prefix>}"
PREFIX="${2:?usage: check-llvm-version.sh <major> <LLVM prefix>}"

llvm_version="$("${PREFIX}/bin/llvm-config" --version)"
clang_version="$("${PREFIX}/bin/clang" --version | head -n 1)"
echo "llvm-config: ${llvm_version}"
echo "clang: ${clang_version}"

if [[ "${llvm_version%%.*}" != "${EXPECTED}" ]]; then
  echo "error: expected LLVM ${EXPECTED}, found ${llvm_version}" >&2
  exit 1
fi
if [[ ! "${clang_version}" =~ clang\ version\ ${EXPECTED}\. ]]; then
  echo "error: expected Clang ${EXPECTED}, found: ${clang_version}" >&2
  exit 1
fi
