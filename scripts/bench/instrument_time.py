#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Measures how the time of instrumentation grows with the accesses of a function.

A generated C function writes each element of an array through one pointer, at -O0, where each
write loads the pointer back from its variable: the pointer is either returned by a call
(malloc) or a parameter. Each is compiled with --instrument and -ftime-trace, several times;
the table gives the median time of CoreTrace's instrumentation, and how much it grew from the
previous size. With sizes that double, about x2 is linear; about x4, quadratic.
Informational: no threshold is enforced.

Usage: instrument_time.py --cc <path to cc> [--runs N] [--sizes N ...] [--out-dir DIR]
"""

import argparse
import json
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path


def call_source(size):
    writes = "".join(f"    p[{i}] = {i};\n" for i in range(size))
    return (
        "extern void *malloc(unsigned long);\n\n"
        "int f(void)\n{\n"
        f"    int *p = malloc(sizeof(int) * {size});\n"
        f"{writes}    return p[{size - 1}];\n}}\n"
    )


def parameter_source(size):
    writes = "".join(f"    p[{i}] = {i};\n" for i in range(size))
    return f"int f(int *p)\n{{\n{writes}    return p[{size - 1}];\n}}\n"


# Name, source of a function of `size` writes.
POINTERS = [
    ("returned by a call", call_source),
    ("parameter", parameter_source),
]


def instrumentation_ms(cc, source, out_dir):
    """Compiles `source` with --instrument and returns the time of the instrumentation step, in
    milliseconds, from the time trace."""
    trace = out_dir / f"{source.stem}.json"
    command = [cc, "--instrument", "-O0", "-S", "-emit-llvm", str(source),
               "-o", str(out_dir / f"{source.stem}.ll"),
               f"-ftime-trace={trace}", "-ftime-trace-granularity=0"]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"compilation failed: {' '.join(command)}\n{result.stderr}")
    events = json.loads(trace.read_text())["traceEvents"]
    durations = [e["dur"] for e in events if e.get("name") == "CoreTraceInstrumentation"]
    if not durations:
        sys.exit(f"no CoreTraceInstrumentation event in {trace}")
    return sum(durations) / 1000


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cc", required=True, help="the cc to measure")
    parser.add_argument("--runs", type=int, default=3, help="runs per size (default 3)")
    parser.add_argument("--sizes", type=int, nargs="+", default=[2000, 4000, 8000, 16000],
                        help="writes per function (default 2000 4000 8000 16000)")
    parser.add_argument("--out-dir", help="where to compile (default: a temporary directory)")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as temporary:
        out_dir = Path(args.out_dir or temporary).resolve()
        out_dir.mkdir(parents=True, exist_ok=True)
        print("| Pointer | writes | instrumentation (ms) | growth |")
        print("| --- | ---: | ---: | ---: |")
        for name, generate in POINTERS:
            previous = None
            for size in args.sizes:
                source = out_dir / f"{generate.__name__}_{size}.c"
                source.write_text(generate(size))
                median = statistics.median(
                    instrumentation_ms(args.cc, source, out_dir) for _ in range(args.runs))
                growth = f"x{median / previous:.1f}" if previous else ""
                print(f"| {name} | {size} | {median:.1f} | {growth} |", flush=True)
                previous = median


if __name__ == "__main__":
    main()
