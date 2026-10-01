# Security Policy

## Supported versions

CoreTrace Compiler is below 1.0.0 and keeps no release branches. Fixes land on `main`
and ship in the next release; earlier minor versions do not receive backports.

| Version | Supported |
| ------- | --------- |
| 0.10.x  | yes       |
| < 0.10  | no        |

## Reporting a vulnerability

Report privately, through GitHub's
[private vulnerability reporting](https://github.com/CoreTrace/coretrace-compiler/security/advisories/new)
(the **Report a vulnerability** button on the repository's **Security** tab), or by
e-mail to hugo.payet@epitech.eu, a maintainer listed in [AUTHORS.md](AUTHORS.md).
Please do not open a public issue or pull request about a suspected vulnerability.

A report is most useful with:

- the version or commit, the platform, and the LLVM version the compiler was built
  with;
- whether the problem is in `cc`, in compilerlib embedded in another program, or in
  the instrumentation runtime linked into an instrumented program;
- the exact command line or `compile()` call, and the environment variables set;
- a minimal input that reproduces it: a source file, LLVM IR or bitcode;
- what you observed, what you expected, and the impact you believe it has.

This is a small project and publishes no response-time targets. You will get an
acknowledgement and then an assessment: accepted as a vulnerability, handled as an
ordinary bug, or out of scope, with the reasons. An accepted report gets a fix in a
new release and, unless you ask otherwise, credit in the release notes and the
advisory. Please coordinate disclosure while a fix is prepared; there is no bug
bounty.

## What is in scope

- **Hostile inputs to the compiler.** Sources, LLVM IR and bitcode are untrusted
  data. A crash, a hang, unbounded memory use, a memory-safety fault, or a file
  written outside the requested outputs, triggered by an input file, is in scope.
  This matters most for compilerlib, which hosts call in process: a fault there is a
  fault in the host.
- **The instrumentation runtime.** It is linked into every instrumented program. A
  defect through which the runtime itself corrupts the program's memory, or lets the
  program's input reach a memory-safety fault inside the runtime, is in scope.
- **The C entry point.** `compile_c` and the compilerlib API must not write past the
  buffers they are given.

## What is not in scope

- **Compiler arguments.** They have the power of a clang command line: `-fplugin=`,
  `-Xclang -load`, response files and `-B` load or run code by design. So do the
  variables that select tools and libraries: `CT_CLANG`, `CT_CLANG_RESOURCE_DIR` and
  `CT_RUNTIME_LIB_DIR`. Run the compiler with arguments and an environment you trust.
- **The `CT_*` variables of an instrumented program**, such as `CT_QUARANTINE_MB` or
  `CT_AUTOFREE_SCAN`. They configure a debugging tool.
- **Missed or false reports.** A memory error the runtime does not report, or a report
  of one that is not there, is a correctness bug: please open an ordinary issue with a
  reproducer.
- **Running instrumented programs in production.** The instrumentation is a debugging
  aid, not a hardening measure. Instrumented programs print heap addresses and
  allocation sites, keep released memory in a quarantine, and may release memory
  early with auto-free; build production binaries without `--instrument`.
- **LLVM and Clang.** Report their vulnerabilities to the
  [LLVM project](https://llvm.org/docs/Security.html).
