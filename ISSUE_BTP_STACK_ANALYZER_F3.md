# Issue: Add smoke coverage for CoreTrace instrumentation flags

## Context

We added a standalone Python smoke-test script for the CoreTrace compiler instrumentation flags. The goal is to validate that the compiler can build and run a small C program across the common flag combinations:

- no instrumentation flag
- `--ct-trace`
- `--ct-alloc`
- `--ct-bounds`
- pairwise flag combinations
- all three flags together

The script resolves the compiler from `./build/cc` or `./cc`, compiles a temporary C source, executes the result, and reports each case independently.

## Files

| File | Status | Commit message |
| --- | --- | --- |
| `BTP-STACK-ANALYZER-F3.py` | New smoke-test utility | `test: add CoreTrace instrumentation flags smoke test` |
| `ISSUE_BTP_STACK_ANALYZER_F3.md` | New issue/documentation file | `docs: document CoreTrace instrumentation smoke-test issue` |
| `build/` | Local CMake build artifacts; do not commit | No commit message; keep this directory out of source commits |

## Architecture note

The smoke coverage is kept as a standalone script instead of being mixed into the C++ runtime or instrumentation code. This keeps the validation layer separate from production code, avoids hard-coded instrumentation internals, and makes the test useful as a quick local regression check against any compiler binary path.

The script uses temporary build inputs under `build/` so it does not pollute the repository root. The flag matrix is explicit because the behavioral surface is small and the cases are easier to audit than a generated combinator loop when diagnosing failures.

## Acceptance criteria

- The script can locate a CoreTrace compiler binary from `./build/cc`, `./cc`, or `--compiler`.
- Each supported flag combination compiles the generated C smoke source.
- Each generated binary executes successfully.
- The no-instrumentation case remains warning-free.
- Generated temporary files are cleaned automatically.
- `build/` artifacts are not included in commits.

## Validation command

```sh
python3 BTP-STACK-ANALYZER-F3.py --verbose
```

## Suggested commit split

1. `test: add CoreTrace instrumentation flags smoke test`
2. `docs: document CoreTrace instrumentation smoke-test issue`

