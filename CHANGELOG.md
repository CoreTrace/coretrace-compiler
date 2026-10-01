# Changelog

Notable changes per release of CoreTrace Compiler. Versions follow
[Semantic Versioning](https://semver.org), derived from the
[Conventional Commits](https://www.conventionalcommits.org) this repository enforces
(see [CONTRIBUTING.md](CONTRIBUTING.md)): a `feat` moves the minor, a `fix` or `perf`
the patch. While the version is below 1.0.0, the command line, the compilerlib API and
the runtime ABI between instrumented objects and the runtime may change in a minor
release; such changes are listed under **Behaviour changes**.

From v0.8.0 on, each release also has detailed notes on the
[releases page](https://github.com/CoreTrace/coretrace-compiler/releases), citing every
pull request.

## Unreleased

### Behaviour changes

- **Runtime ABI.** Two entry points are added, `__ct_schedule_leak_report` and
  `__ct_stack_depth` (#132, #137). Objects instrumented by this version need its
  runtime; objects instrumented by 0.10.0 still link with it.
- **Leak report timing.** The report now runs after the program's own exit-time code:
  destructors of global objects, exit handlers and destructor functions (#132, #130).
  Blocks they release are no longer reported as leaks. Every module the alloc pass
  instruments gets a constructor of priority 1 for that.
- **Opting out of instrumentation.** A function declared
  `__attribute__((no_instrument_function))` or
  `__attribute__((disable_sanitizer_instrumentation))` is no longer instrumented
  (#138, #122).
- **Conservative auto-free scan** (`CT_AUTOFREE_SCAN`, macOS). With its default
  settings, a pass now completes and releases the blocks no root reaches, directly or
  through other tracked blocks; before, no pass completed and nothing was released
  (#129, #121, #127).

### Fixes

- **Auto-free at return** no longer releases a block that leaves its function through
  a second local variable, and no longer produces invalid IR for an allocation in a
  loop or a branch, which failed compilations with the default modules (#128, #125,
  #126).
- **Auto-free scan.** It no longer allocates, logs or calls dyld while other threads
  are suspended, which could hang a multithreaded program (#123, #118), and no longer
  reads the stack of a thread that exited (#129).
- **Constructor order on macOS.** Instrumented objects ran the constructors and
  destructors of a file in reverse priority order (#132, part of #131).
- **Windows.** A global object that allocates in its constructor no longer crashes the
  program at start-up (#132).
- **`fork()`.** The child of a multithreaded instrumented program no longer hangs on a
  lock another thread held (#134, #133).
- **`setjmp`/`longjmp`.** Frames left by `longjmp` no longer fill the registry of stack
  objects until stack checks stop (#137, #135).

### Tests and CI

- A multithreaded stress fixture, and a timeout in the runtime suites (#123).
- IR-level tests of the vtable pass (#139).
- UndefinedBehaviorSanitizer joins AddressSanitizer in CI (`ENABLE_DEBUG_UBSAN`)
  (#140).
- The passes are fuzzed with `llvm-stress` on Linux and macOS (#141).

## v0.10.0 (2026-09-29)

- Compiles in process in every output mode: a clang executable is no longer needed,
  only Clang's resource directory (#115, #117).
- Released blocks go to a quarantine (`CT_QUARANTINE_MB`, 256 MB by default), so a
  use-after-free keeps being reported (#111).
- Releases are rewritten in every function, so blocks released by the C++ library are
  no longer reported as leaks; allocations are tracked where user code makes them
  (#110).
- Fixes: LLVM 19 build (#114), invalid IR from the bounds pass at `-O2` (#109), lost
  freed records (#114).
- A differential test suite, runtime suites at `-O2`, an LLVM 16 to 19 matrix and an
  AddressSanitizer job in CI (#108, #114).

## v0.9.0 (2026-09-26)

- Bounds checks cover stack objects (`stack-buffer-overflow`) (#86, #93).
- Objective-C objects are tracked until they are deallocated (#84).
- Builds with LLVM 16 to 20 (#89); the runtime can be left out of the build
  (`CORETRACE_COMPILER_BUILD_RUNTIME=OFF`) (#88).
- Leak and double-free reports give the allocation site (#94); in-memory bitcode output
  (#95).
- **Runtime ABI:** `__ct_free` and `__ct_delete*` take the site of their call: objects
  instrumented by 0.8.0 must be recompiled (#94).

## v0.8.0 (2026-09-24)

- First release with Windows (x64) support, with a Windows port of the runtime (#36,
  #73, #74).
- Relocatable install; `cc --version` (#52).
- A failed compile or link of a non-instrumented build exits with a non-zero status
  (#55).

## v0.7.0 (2026-02-26)

- clang-format configuration and format check (#19, #20).
- Better garbage-collection analysis and allocator/deallocator identification (#25).

## Earlier versions

Tagged without release notes; summarized from their commits.

- **v0.6.2** (2026-01-29): position-independent code for instrumented Linux targets,
  multi-arch Docker tests, the external coretrace-testkit.
- **v0.6.1** (2026-01-18): runtime log levels and `nodiscard` helpers.
- **v0.6.0** (2026-01-15): vtable and virtual call instrumentation, with diagnostics;
  dynamic allocation and shadow tables.
- **v0.5.0** (2026-01-14): instrumentation of `calloc`, `realloc`, `new` and `delete`.
- **v0.4.1** (2026-01-05): the instrumentation pipeline in the compiler.
- **v0.4.0** (2026-01-05): the instrumentation runtime.
- **v0.3.1** (2025-11-29): driver compatibility.
- **v0.3.0** (2025-11-19): FetchContent integration and the extern-project sample.
- **v0.2.0** (2025-04-18): shared and static libraries.
- **v0.1.0** (2025-04-17): the Clang-based compiler.
