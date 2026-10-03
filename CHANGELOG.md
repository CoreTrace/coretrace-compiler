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

### Features

- **LLVM 21 to 23.** compilerlib and `cc` build with LLVM 16 to 23 (#145).

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
- **Builtins in instrumented code.** From LLVM 18, only the allocation functions the
  instrumentation rewrites lose their builtin status, instead of every C library function:
  clang optimizes calls such as `memcpy` and `memset` again (#154, #136). LLVM 16 and 17
  keep `-fno-builtin` (#153).
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
- **Auto-free scan.** It no longer crashes reading the stack of a thread that is exiting,
  which the kernel unmaps while the thread can still be suspended (#150, #148).
- **Objective-C with Clang 23.** Allocations sent through selector stubs
  (`objc_msgSend$new`, `objc_msgSend$allocWithZone:`), which Clang 23 emits for Apple
  targets, are tracked (#147).
- **Contention.** Threads waiting for the runtime's allocation or shadow lock yield the
  processor after a few attempts instead of spinning: with more threads than processors,
  a multithreaded program ran up to four times slower (#151).
- **`setjmp` in instrumented code.** From LLVM 18, the call keeps `returns_twice`, which
  keeps the optimizer from transformations such as tail calls around it (#154, #136).

### Tests and CI

- A multithreaded stress fixture, and a timeout in the runtime suites (#123).
- IR-level tests of the vtable pass (#139).
- UndefinedBehaviorSanitizer joins AddressSanitizer in CI (`ENABLE_DEBUG_UBSAN`)
  (#140).
- The passes are fuzzed with `llvm-stress` on Linux and macOS (#141).
- The fixture of a scan pass during a blocked log write no longer races with its own
  stderr restoration, and the runtime suites print the end of a failing run's log (#150).
- The LLVM versions workflow builds everything with each of LLVM 16 to 23, one job per
  version, and runs every suite and the fuzzer with it (#145).
- One job per system and LLVM version: Linux with 16 to 23 (GCC builds the project with
  LLVM 20), macOS with 19, 20 and 23, Windows with 19, 20 and 22. Each job first checks
  that the LLVM it installed is the version in its name; the relocated install test runs
  in every job (#147, #146).
- The fuzzer reports a module that `cc` cannot compile without `--instrument` either, a
  bug in LLVM's own code generation, as skipped instead of failed, and no longer stops at
  the first failing module (#147).

## v0.10.0 (2026-09-29)

Compiles in process in every output mode, quarantines released blocks, and fixes the
false positives a new differential test suite found.

### Features

- **No clang executable needed.** File output without instrumentation runs the
  frontend in process, through `clang::ExecuteCompilerInvocation`, in every output
  mode and with several sources in one call; `-E`, `-fsyntax-only`, `--analyze` and
  precompiled headers keep working (#115, #104). Without a clang, Clang's resource
  directory is enough: `CT_CLANG_RESOURCE_DIR`, `-resource-dir`, or the directory
  recorded at build time (#117, #116). Only assembling `.s` sources still runs clang.
- **Quarantine of released blocks.** `free`, `delete`, `realloc` and auto-free keep
  released blocks away from the allocator, so an access through a dangling pointer
  keeps being reported as `heap-use-after-free`. `CT_QUARANTINE_MB` sets the limit,
  256 MB by default (#111, #105).
- **Allocations and releases across the library boundary.** Releases are rewritten in
  every function, system headers included, so a block that user code allocates and
  `std::unique_ptr` or a container releases is no longer reported as a leak.
  Allocations are tracked where user code makes them (#110, #106).

### Behaviour changes

- An instrumented program keeps up to 256 MB of released memory by default; set
  `CT_QUARANTINE_MB` lower, or to 0 to release at once (#111).
- `realloc` of a tracked block always allocates a new block and copies the contents;
  `realloc` of a released block returns `NULL` with a warning (#111).
- `munmap` and a shrinking `sbrk` no longer keep a freed record (#111).
- Releasing a block the runtime did not track is traced at the info level, marked
  `(unknown)`, instead of a warning (#110).
- An allocation that libc++ or libstdc++ makes in code inlined into user functions is
  no longer tracked (#110).
- compilerlib links `clangFrontendTool` (#115).
- A failed compilation reports the compiler's diagnostic with the source excerpt
  instead of a bare `compilation failed` (#115).
- **Runtime ABI:** unchanged since 0.9.0.

### Fixes

- compilerlib builds with LLVM 19 again (#114, #112).
- On macOS, a failed sysroot detection is retried by the next compilation (#100, #98).
- The bounds pass resolves a pointer loaded from a stack slot only when the store wrote
  exactly what the load reads. This fixes invalid IR when the vectorizer copied a
  `std::shared_ptr` with one vector store at `-O2` on arm64, and wrong bases for
  fields at an offset (#109, #107).
- No false use-after-free on memory allocated outside instrumented code at the address
  of a freed block (#111, #105).
- Inserting a block no longer erases the record of a quarantined block (#114, #113).

### Tests and CI

- A differential suite builds programs without memory errors both plain and
  instrumented and requires the same output (#108).
- The runtime suites run again at `-O2` (`CT_TEST_OPT`) (#108).
- An LLVM 16 to 19 matrix and an AddressSanitizer job (#114).
- Unit tests of the bounds and alloc passes on hand-written IR (#109, #110), of the
  quarantine (#111, #114), and of compilation without a clang executable (#115,
  #117).
- The LLVM apt install refreshes its index on every attempt (#102, #101).

### Known issues

- The conservative auto-free scan (`CT_AUTOFREE_SCAN`, macOS) can hang a
  multithreaded program (#118), fixed in the next release.

## v0.9.0 (2026-09-26)

Bounds checks on stack objects, Objective-C object tracking, LLVM 16 to 20, and
source locations in leak and double-free reports.

### Features

- **Stack objects in bounds checks.** A local object is registered while its frame
  runs when a check uses it as a base or its address escapes; an access outside it is
  reported as `stack-buffer-overflow`, in its own function, in a callee, or through
  `container_of` (#86, #79, #93, #90).
- **Objective-C and Objective-C++.** On Apple targets, objects allocated with `alloc`,
  `allocWithZone:` and `new` are tracked until `-[NSObject dealloc]`, so a leaked
  object is reported; the auto-free scan never releases them (#84, #33). Smoke tests
  and documentation (#82, #83).
- **LLVM 16 to 20.** compilerlib and `cc` build with LLVM 16 to 20; CMake rejects older
  versions (#89, #87).
- **Optional runtime.** `-DCORETRACE_COMPILER_BUILD_RUNTIME=OFF` builds compilerlib and
  `cc` without the runtime; `CT_RUNTIME_LIB_DIR` then supplies it (#88, #85).
- **Sites in memory reports.** Leak and double-free lines give the allocation site as
  `alloc_site=`, and a double free the site of the second release as `site=` (#94,
  #92).
- **In-memory bitcode.** `OutputMode::ToMemoryBitcode` returns the bytes of
  `-emit-llvm -c` in `CompileResult::llvmBitcode`, without writing a file (#95).

### Behaviour changes

- **Runtime ABI:** `__ct_free` and the `__ct_delete*` entry points take the site of
  their call. Objects instrumented by 0.8.0 must be recompiled (#94).
- Sites name a file by the path the compiler was given instead of its base name; on
  Windows, the leak line uses `alloc_site=` instead of `site=` (#94).
- A failed instrumented compilation removes its output file, as clang does (#97,
  #96).
- `compile()` verifies every instrumented module and fails with the verifier's
  message, flagged as a CoreTrace bug, instead of generating code from invalid IR
  (#93).

### Fixes

- A code-generation error no longer ends the process that compiles, `cc` or a host
  calling `compile()`; the compilation fails and reports it (#93, #91).
- A write error on the output fails the compilation instead of ending the process
  (#97, #96).
- The trace prints Objective-C methods and other asm-labelled functions without LLVM's
  `\01` marker (#81).
- The vtable pass no longer crashes on opaque pointers with LLVM 16 (#89).

### Tests and CI

- In-process unit tests through `compilerlib::compile` (`test/unit/compile_test.cpp`):
  instrumented IR validity, code-generation and write errors, output removal, sites,
  in-memory bitcode (#93, #94, #95, #97).
- Fixtures for stack bounds and report sites; Objective-C smoke cases (#82, #84, #86,
  #94).
- CI builds without the runtime (#88) and with LLVM 16 on Ubuntu 22.04 (#89).

## v0.8.0 (2026-09-24)

The first release with Windows support; a relocatable install; correctness fixes in the
runtime and the passes.

### Features

- **Windows (x64).** Native build with clang-cl (`scripts/build-windows.ps1`) and a
  Windows port of the runtime (#36). `new`/`delete` are tracked under the Microsoft
  C++ ABI (#74); CI runs instrumented programs on Windows (#73).
- **Relocatable install.** `cmake --install` produces a self-contained prefix, and
  `cc` finds its runtime relative to its own location; `CT_RUNTIME_LIB_DIR` overrides
  the lookup. `cc --version` prints the compiler and LLVM versions (#52).
- **Instrumented LLVM IR and bitcode output**, with object, IR and bitcode emission in
  one module (#28, #23).
- **In-tree frontend.** `--ct-optnone` marks user functions `optnone` and `noinline`
  before code generation, and the wrapper's diagnostics are cleaner (#30, #29).
- Richer runtime logs (#32, #31).

### Behaviour changes

- A non-instrumented build exits with a non-zero status when a compile or link step
  fails (#55).
- On Windows, vtable diagnostics need `--ct-vtable-diag`, as elsewhere (#72), and the
  trace prints a C function's name once (#75).
- `scripts/build-windows.ps1 -BuildTests` controls whether unit tests are built (#73).

### Fixes

- `--help` is clearer and matches the options, and running the produced binary alone
  behaves predictably (#26, #21).
- The leak report runs at teardown without touching logger state that may be
  destroyed; logging is enabled whichever module starts first (#48).
- The auto-free scan releases its flag on every exit path, and the runtime locks the
  allocation table before reading it (#56).
- Polymorphic `delete` is tracked: allocator calls in `linkonce`/`weak` bodies such as
  deleting destructors are rewritten (#60).
- Windows: the leak report and the exception filter use lock-free writers, and DbgHelp
  calls are serialised (#77).
- With shadow memory, an access whose base is a known allocation is checked against
  its bounds first (#78), and a pointer rebuilt through integer arithmetic
  (`container_of` with `uintptr_t`) is traced back to its allocation (#78).
- Auto-free follows scalar spills and out-parameters such as `posix_memalign` (#49).
- LLVM target initialisation is thread-safe (#39).
- `compile_c` validates its arguments and never writes past the caller's buffer; `-g0`
  keeps the line tables sites need; a trailing `-o=` is handled (#53).

### Tests and CI

- Smoke coverage for the instrumentation flags (#38).
- GoogleTest unit tests of the driver helpers, the alloc pass and the runtime
  allocation table, run by `ctest` (#64, #65, #66); `compile_c` checks from C (#59).
- Runtime fixture suites in CI on Linux and macOS (#51) and on Windows (#73, #75, #77).
- Retried LLVM installation, a vendored apt.llvm.org key, and an sbrk fixture stable
  under QEMU (#58, #62, #63).

### Internal

- The compiler/runtime ABI is declared once, in `include/coretrace/runtime_abi.h`
  (#67).
- One rewrite helper in the alloc pass, one release path and one auto-free path in the
  runtime; the auto-free scan and the configuration in their own units (#68, #69, #70,
  #71, #72).
- Apache-2.0 SPDX headers across the tree (#35, #34); the superseded smoke-test script
  is removed (#76).

## v0.7.0 (2026-02-26)

- **Conservative auto-free scan** (`CT_AUTOFREE_SCAN`, macOS): a background pass scans
  stacks, registers and globals for pointers to tracked blocks and releases the ones
  no root reaches, tuned by the `CT_AUTOFREE_SCAN_*` variables (#25, #24).
- Better identification of allocators and deallocators for auto-free (#25).
- Runtime features can be read and toggled at run time with `ct_is_enabled`,
  `ct_set_enabled` and `ct_get_features`; the configuration is held in atomic state
  (#25).
- `cc --help`, and a module for argument parsing.
- clang-format configuration, `format` targets and a format check in CI (#19, #18);
  the targets are skipped when the project is built through FetchContent (#20).
- Commit messages are checked against Conventional Commits in CI and by a `commit-msg`
  pre-commit hook.
- A test stage in the Dockerfile; multi-arch Docker tests run on pushes to `main`.

## v0.6.2 (2026-01-29)

- Instrumented Linux targets are compiled and linked as position-independent
  executables.
- Toolchain resolution: clang and its resource directory are detected instead of
  relying on hard-coded C++ include paths, and `-o=`/`-x=` arguments are normalised
  (#15, #14).
- Tests use the external coretrace-testkit, installed with pip (#17, #16); multi-arch
  Docker builds for Linux in CI.

## v0.6.1 (2026-01-18)

- Allocation-detail logs take a level and follow the surrounding log level (#12, #11).
- Runtime helpers returning a value are `nodiscard`; the logger handles the result of
  `write()`.
- The vtable flags and diagnostic fixtures are documented.

## v0.6.0 (2026-01-15)

- **vtable and virtual call instrumentation** (`--ct-vtable-diag`, `--ct-vcall-trace`),
  with runtime diagnostics of suspicious virtual dispatch and module resolution on
  Linux and macOS (#10, #9).
- Allocation and shadow tables grow dynamically; allocation wrappers in the runtime.
- `realloc` logs give the old and new sizes and pointers.
- Driver diagnostics are kept with `--instrument`.

## v0.5.0 (2026-01-14)

- Instrumentation of `calloc`, `realloc`, `new` and `delete`.
- Site strings are deduplicated.
- Stress tests of allocations and shadow memory.

## v0.4.1 (2026-01-05)

- The instrumentation pipeline in the compiler: `--instrument` and the `--ct-*` flags
  select the trace, alloc, bounds and shadow modules (`--ct-modules=`).
- Diagnostics are reset between cc1 actions.

## v0.4.0 (2026-01-05)

- The instrumentation runtime and its libraries, with the trace, alloc and bounds
  passes.
- Clang's resource directory is detected generically.
- CI on Linux builds with LLVM/Clang 20.

## v0.3.1 (2025-11-29)

- The driver is switched for better compatibility, and the API example is updated.

## v0.3.0 (2025-11-19)

- Integration through CMake FetchContent, with the `extern-project` sample.
- Builds with several LLVM/Clang versions, including LLVM 16.
- GitHub Actions workflow building on Linux, Windows and macOS (#3), with a test of
  the generated output.

## v0.2.0 (2025-04-18)

- The project is exported as shared and static libraries, with a standard
  `src`/`include` layout.
- A build helper script and test samples.

## v0.1.0 (2025-04-17)

- A compiler based on Clang, with CMake configuration for LLVM and Clang.
