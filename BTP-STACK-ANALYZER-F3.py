#!/usr/bin/env python3
"""Smoke test for CoreTrace compiler instrumentation flags.

This script compiles and runs a tiny C program with the CoreTrace compiler
using different combinations of the --ct-trace / --ct-alloc / --ct-bounds
flags. It is intended to quickly verify that the compiler and runtime still
behave correctly for common instrumentation setups.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import List, Sequence, Tuple


RESET = "\033[0m"
GREEN = "\033[32m"
RED = "\033[31m"
PURPLE = "\033[35m"
BOLD = "\033[1m"


def repo_root() -> Path:
    return Path(__file__).resolve().parent


def default_compiler() -> Path:
    candidates = [repo_root() / "build" / "cc", repo_root() / "cc"]
    for candidate in candidates:
        if candidate.exists() and os.access(candidate, os.X_OK):
            return candidate
    if shutil.which("cc"):
        return Path(shutil.which("cc") or "cc")
    raise FileNotFoundError("Unable to find a CoreTrace compiler binary. Expected ./build/cc or ./cc.")


def build_source() -> str:
    return r"""
#include <stdio.h>
#include <stdlib.h>

int add(int a, int b) {
    return a + b;
}

int main(void) {
    int *p = malloc(sizeof(int) * 2);
    if (!p) {
        return 1;
    }
    p[0] = add(1, 2);
    p[1] = add(3, 4);
    printf("%d %d\n", p[0], p[1]);
    free(p);
    return 0;
}
"""


def colorize(text: str, color: str) -> str:
    return f"{color}{text}{RESET}"


def contains_warning(output: str) -> bool:
    return "warning" in output.lower()


def run_case(
    compiler: Path,
    flags: Sequence[str],
    workdir: Path,
    verbose: bool,
    expect_no_warnings: bool = False,
) -> Tuple[bool, str]:
    source_path = workdir / "ct_smoke.c"
    binary_path = workdir / "ct_smoke"
    source_path.write_text(build_source(), encoding="utf-8")

    command = [str(compiler), "--instrument", *flags, "-o", str(binary_path), str(source_path)]
    if verbose:
        print("$", " ".join(command))

    compile_proc = subprocess.run(command, cwd=repo_root(), capture_output=True, text=True)
    if compile_proc.returncode != 0:
        combined_output = (compile_proc.stdout or "") + (compile_proc.stderr or "")
        return False, combined_output

    run_proc = subprocess.run([str(binary_path)], cwd=repo_root(), capture_output=True, text=True)
    output = (run_proc.stdout or "") + (run_proc.stderr or "")
    if run_proc.returncode != 0:
        return False, output

    if expect_no_warnings and contains_warning(output):
        return False, output

    return True, output


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Smoke-test CoreTrace --ct-trace/--ct-alloc/--ct-bounds flags")
    parser.add_argument("--compiler", type=Path, default=None, help="Path to the CoreTrace compiler binary")
    parser.add_argument("--verbose", action="store_true", help="Print each compile/run command")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    compiler = args.compiler or default_compiler()
    if not compiler.exists():
        print(f"Compiler not found: {compiler}", file=sys.stderr)
        return 2

    print(colorize(f"Using compiler: {compiler}", BOLD))
    print(colorize("Running smoke tests for CoreTrace instrumentation flags...", BOLD))
    print("")

    cases: List[Tuple[Sequence[str], str, bool]] = [
        ([], "NONE", True),
        (["--ct-trace"], "--ct-trace", False),
        (["--ct-alloc"], "--ct-alloc", False),
        (["--ct-bounds"], "--ct-bounds", False),
        (["--ct-trace", "--ct-alloc"], "--ct-trace --ct-alloc", False),
        (["--ct-trace", "--ct-bounds"], "--ct-trace --ct-bounds", False),
        (["--ct-alloc", "--ct-bounds"], "--ct-alloc --ct-bounds", False),
        (["--ct-trace", "--ct-alloc", "--ct-bounds"], "--ct-trace --ct-alloc --ct-bounds", False),
    ]

    passed = 0
    with tempfile.TemporaryDirectory(prefix="ct-smoke-", dir=str(repo_root() / "build")) as tmpdir:
        workdir = Path(tmpdir)
        for idx, (flags, case_name, expect_no_warnings) in enumerate(cases, 1):
            print(colorize("-" * 40, PURPLE))
            print(colorize(f"[{idx}/{len(cases)}]", BOLD) + f" Case: {case_name}")
            source_name = (workdir / "ct_smoke.c").name
            print(colorize("Source file:", PURPLE) + f" {colorize(source_name, PURPLE)}")

            ok, output = run_case(compiler, flags, workdir, verbose=args.verbose, expect_no_warnings=expect_no_warnings)
            if ok:
                passed += 1
                print(colorize(f"PASS  {case_name}", GREEN))
                if output.strip():
                    print(output.strip())
            else:
                print(colorize(f"FAIL  {case_name}", RED))
                if output:
                    print(output)
                print("")
                return 1
            print("")

    print(colorize(f"All {passed}/{len(cases)} smoke tests passed.", GREEN))
    return 0


if __name__ == "__main__":
    sys.exit(main())
