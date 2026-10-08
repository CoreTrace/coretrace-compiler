#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Measures the run-time overhead of instrumentation on the programs in bench/.

Each program is built at -O2 without instrumentation and with each configuration below,
then run several times; the table gives the median time of the plain build and, for each
configuration, its median time as a multiple of that. Every build must print the same
output, so that instrumentation is checked not to change what the program computes.
Informational: no threshold is enforced.

Usage: overhead.py --cc <path to cc> [--runs N] [--out-dir DIR]
"""

import argparse
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PROGRAMS = ["alloc_churn.c", "array_kernel.c", "virtual_calls.cpp"]
# Name, cc options. "default" is what --instrument alone gives, its logging included; the
# others leave out the log line of each allocation, to measure the checks themselves.
CONFIGS = [
    ("default", ["--instrument"]),
    ("alloc", ["--instrument", "--ct-modules=alloc", "--ct-no-alloc-trace"]),
    ("alloc,bounds", ["--instrument", "--ct-modules=alloc,bounds", "--ct-no-alloc-trace"]),
    ("alloc,bounds,vtable",
     ["--instrument", "--ct-modules=alloc,bounds,vtable", "--ct-no-alloc-trace"]),
]


def build(cc, source, options, output):
    command = [cc, "-O2", *options, str(source), "-o", str(output)]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"build failed: {' '.join(command)}\n{result.stderr}")


def measure(binary, runs):
    """Returns the program's stdout and its median wall time in milliseconds."""
    times = []
    stdout = None
    for _ in range(runs):
        start = time.perf_counter()
        result = subprocess.run([str(binary)], stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL, text=True)
        times.append((time.perf_counter() - start) * 1000)
        if result.returncode != 0:
            sys.exit(f"{binary} exited with {result.returncode}")
        stdout = result.stdout
    return stdout, statistics.median(times)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cc", required=True, help="the cc to measure")
    parser.add_argument("--runs", type=int, default=5, help="runs per build (default 5)")
    parser.add_argument("--out-dir", help="where to build (default: a temporary directory)")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as temporary:
        out_dir = Path(args.out_dir or temporary)
        out_dir.mkdir(parents=True, exist_ok=True)
        header = ["Program", "plain (ms)"] + [name for name, _ in CONFIGS]
        rows = []
        for program in PROGRAMS:
            source = ROOT / "bench" / program
            stem = Path(program).stem
            plain = out_dir / f"{stem}.plain"
            build(args.cc, source, [], plain)
            expected, base = measure(plain, args.runs)
            row = [program, f"{base:.0f}"]
            for name, options in CONFIGS:
                binary = out_dir / f"{stem}.{name.replace(',', '_')}"
                build(args.cc, source, options, binary)
                output, median = measure(binary, args.runs)
                if output != expected:
                    sys.exit(f"{program} [{name}] printed {output!r}, plain printed {expected!r}")
                row.append(f"x{median / base:.2f}")
            rows.append(row)
            print(f"measured {program}", file=sys.stderr)

    print("| " + " | ".join(header) + " |")
    print("|" + "---|" * len(header))
    for row in rows:
        print("| " + " | ".join(row) + " |")
    print(f"\nMedian of {args.runs} runs per build, at -O2; each configuration as a multiple "
          "of the plain build's time.")


if __name__ == "__main__":
    main()
