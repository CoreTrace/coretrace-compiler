# CoreTrace Compiler

CoreTrace Compiler is a Clang/LLVM-based wrapper that can emit LLVM IR, build binaries, and optionally
instrument code with runtime checks (alloc/bounds/trace/vtable). It can run in a file-based mode or
an in-memory mode for tooling pipelines.

## Build

Requirements: CMake, a C++20 compiler and LLVM/Clang 16 to 23.

CI builds and tests every combination below in its own job (the LLVM versions workflow), and
first checks that the installed LLVM is the version the job is named after:

| System | LLVM | Project built with | Tests |
| --- | --- | --- | --- |
| Linux (Ubuntu 24.04) | 16 to 23 | the same version's clang; GCC for LLVM 20 | unit, smoke, install, runtime fixtures at `-O0` and `-O2`, pass fuzzing |
| macOS 15 | 19, 20, 23 (Homebrew) | AppleClang | unit, smoke, install, runtime fixtures at `-O0` and `-O2`, pass fuzzing |
| Windows (Server 2022) | 19.1.7, 20.1.0, 22.1.8 (official archives) | the archive's clang-cl | unit, smoke, install |

On Windows, the LLVM 23 archive needs zlib and zstd, which it does not ship, to be found by
CMake: building against it is not covered yet (#149).

Instrumented code behaves differently with LLVM 16 and 17: see
[LLVM versions](#llvm-versions).

The Build workflow adds, with LLVM 20 on Linux: AddressSanitizer, LeakSanitizer and
UndefinedBehaviorSanitizer builds, a build without the runtime, and multi-arch Docker tests on
`main`.

Quick build:

```zsh
mkdir -p build && cd build
./build.sh
```

Windows (native, produces `cc.exe` under `dist/windows/bin`):

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build-windows.ps1 `
  -LLVMDir "C:\LLVM\lib\cmake\llvm" `
  -LoggerSourceDir "C:\Users\shookapic\Documents\coretrace-log" `
  -Configuration Release
```

macOS:

```zsh
mkdir -p build && cd build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_DIR=$(brew --prefix llvm)/lib/cmake/llvm \
  -DClang_DIR=$(brew --prefix llvm)/lib/cmake/clang \
  -DUSE_SHARED_LIB=OFF
```

Linux:

```zsh
mkdir -p build && cd build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_DIR=/usr/lib/llvm-${LLVM_VERSION}/lib/cmake/llvm \
  -DClang_DIR=/usr/lib/llvm-${LLVM_VERSION}/lib/cmake/clang \
  -DCLANG_LINK_CLANG_DYLIB=ON \
  -DLLVM_LINK_LLVM_DYLIB=ON \
  -DUSE_SHARED_LIB=OFF \
  && cmake --build build -j"$(nproc)"
```

Without the instrumentation runtime: a tool that only embeds `compilerlib` to produce LLVM IR can
pass `-DCORETRACE_COMPILER_BUILD_RUNTIME=OFF`. The runtime and its logger, coretrace-log, are then
neither fetched, built nor installed; `compilerlib`, `cc` and the compilerlib unit tests still build.
`--instrument` then links a runtime built elsewhere, from the directory `CT_RUNTIME_LIB_DIR` names,
and otherwise fails with "instrumentation runtime archives not found".

## Install

```zsh
cmake --install build --prefix /opt/coretrace
/opt/coretrace/bin/cc --version
```

The prefix contains `bin/cc`, `lib/libct_instrument_runtime.a`, `lib/libcoretrace_logger.a`
and `include/`. `cc` finds the runtime archives relative to its own location, so the prefix can
be moved. Overrides: `CT_RUNTIME_LIB_DIR` (directory holding both archives), `CT_CLANG`
(clang executable), `CT_CLANG_RESOURCE_DIR` (clang resource directory). Compilation runs in
process in every mode: the clang executable is only run to assemble `.s` sources. Without any
clang, Clang's resource directory is enough (`CT_CLANG_RESOURCE_DIR`, or the one recorded at
build time): the driver then assumes the standard LLVM layout, `<prefix>/bin/clang` next to
`<prefix>/lib/clang/<version>`.

## Tests

After building into `build/`:

```zsh
ctest --test-dir build --output-on-failure # unit tests (GoogleTest 1.17.0, fetched at configure time)
python3 test/examples/test_smoke.py        # compiler smoke tests (needs coretrace-testkit)
bash test/run_autofree_tests.sh            # auto-free fixtures under test/
bash test/run_runtime_tests.sh             # alloc, new/delete, shadow and vtable fixtures
bash test/run_differential_tests.sh        # instrumented vs plain builds of bug-free programs
```

The shell runners accept an output directory as first argument, `CC_BIN` to point at
another compiler binary, and `CT_TEST_OPT` to build the programs with an optimization flag
(for instance `CT_TEST_OPT=-O2`; the default is `-O0`). CI runs all of them on Linux and
macOS, and runs the shell runners a second time at `-O2`. Unit tests are built only
when this project is the top-level CMake project; pass `-DCORETRACE_BUILD_UNIT_TESTS=OFF`
to skip them.

## Code Style (clang-format)

- Target version: `clang-format` 17 (CI uses this).
- Format locally: `./scripts/format.sh`
- Check without modifying: `./scripts/format-check.sh`
- CMake targets: `cmake --build build --target format` or `--target format-check`
- CI: the `clang-format` GitHub Actions job fails if a file is not formatted.

## Usage

```zsh
./cc -S -emit-llvm test.cc
./cc -S test.cc
./cc -c test.c
./cc -c test.c -O2
./cc --instrument -o app main.c
./cc --instrument -o=app main.c
./cc -x c++ -o=appCpp main.cpp
./cc -x=c++ -o appCpp main.cpp
./cc --instrument --ct-modules=trace,alloc,bounds,vtable main.cpp
./cc --instrument --ct-shadow -o app main.c
./cc --instrument --ct-shadow-aggressive --ct-bounds-no-abort -o app main.c
./cc --instrument --ct-modules=vtable --ct-vcall-trace -o app main.cpp
./cc --in-mem -S -emit-llvm test.c
```

## CLI Options

Core options:
- `--instrument`: enable CoreTrace instrumentation (required for `--ct-*` flags).
- `--in-mem`, `--in-memory`: print LLVM IR to stdout (use with `-S -emit-llvm`).

Instrumentation toggles:
- `--ct-modules=<list>`: comma-separated list `trace,alloc,bounds,vtable,all`.
- `--ct-shadow`: enable shadow memory in the produced binary.
- `--ct-shadow-aggressive`, `--ct-shadow=aggressive`: aggressive shadow mode.
- `--ct-bounds-no-abort`: do not abort on bounds errors.
- `--ct-no-trace` / `--ct-trace`: disable/enable function entry/exit instrumentation.
- `--ct-no-alloc` / `--ct-alloc`: disable/enable malloc/free instrumentation.
- `--ct-no-bounds` / `--ct-bounds`: disable/enable bounds checks.
- `--ct-no-autofree` / `--ct-autofree`: disable/enable auto-free on unreachable allocations.
- `--ct-no-alloc-trace` / `--ct-alloc-trace`: disable/enable malloc/free tracing logs.
- `--ct-no-vcall-trace` / `--ct-vcall-trace`: disable/enable virtual call tracing (Itanium ABI).
- `--ct-no-vtable-diag` / `--ct-vtable-diag`: enable/disable vtable diagnostics.

Frontend toggles:
- `--ct-optnone`: add `optnone` and `noinline` to user-defined functions.
- `--ct-no-optnone`: disable optnone injection.

Notes:
- All other arguments are forwarded to clang (e.g. `-O2`, `-g`, `-I`, `-D`, `-L`, `-l`, `-std=...`).
- Alloc instrumentation rewrites `malloc/free/calloc/realloc/aligned_alloc/posix_memalign`,
  `mmap/munmap` and `sbrk/brk`, the global C++ `operator new` (scalar and array, plain and
  `nothrow`), and the global `operator delete` in its scalar, array, sized, `nothrow` and
  destroying forms. Aligned `operator new` (`std::align_val_t`) is not rewritten, so its blocks
  are not tracked. Aligned `operator delete` is rewritten on Itanium targets (Linux, macOS) only:
  the Microsoft CRT allocates aligned blocks with `_aligned_malloc`, which only the aligned
  delete may release.
- Allocations are tracked where user code makes them: an allocation made by system-header code,
  such as a standard container's, stays untracked, even once inlined into user code. Releases go
  through the runtime everywhere, system headers included, since library code such as
  `std::unique_ptr` releases blocks user code allocated. The release of an untracked block is only
  traced, marked `(unknown)`.
- Released blocks go to a quarantine instead of back to the allocator, as with AddressSanitizer:
  while a block is there, no other allocation receives its address, so an access through a
  dangling pointer is reported as `heap-use-after-free`. `realloc` of a tracked block moves it to
  a new block and quarantines the old one. The quarantine holds up to `CT_QUARANTINE_MB` megabytes
  (256 by default); past that, the oldest blocks are released for real, and a use-after-free or
  double free of them is no longer reported. Unmapped memory (`munmap`) is not quarantined.
- Bounds checks cover heap blocks and the stack objects of running frames, reported as
  `stack-buffer-overflow`. A local array or struct is tracked while its function runs when its
  address escapes, for instance to a callee, or when an access to it cannot be proven in bounds at
  compile time; accesses proven in bounds are not checked at all. As for heap blocks, an access is
  checked when its base is the start of the object, or any address inside it with
  `--ct-shadow-aggressive`. Variable-length arrays are not tracked, nor objects beyond the 512
  innermost ones each thread registers.
- Vtable tooling requires C++ and an Itanium ABI (macOS/Linux).
- Clang automatically adds `optnone` at `-O0`. Use `--ct-optnone` to force the attribute even when
  passing `-Xclang -disable-O0-optnone`.

## Leak report

When an instrumented program exits, the runtime lists the tracked blocks still allocated:
`ct: leaks detected count=N`, then one `ct: leak` line per block, with the site that allocated it.
The report is meant to run after the program's own exit-time code, so that the blocks that code
releases are not reported.

What the tests check, in CI:

| Exit-time code that releases blocks | Linux | macOS | Windows |
| --- | --- | --- | --- |
| Destructors of global objects of instrumented code | yes | yes | yes |
| `atexit` handlers registered by instrumented code | yes | yes | not tested |
| Destructor functions (`__attribute__((destructor))`) of default priority, in instrumented code | yes | yes | not tested |
| The same in objects compiled without `--instrument` and linked before the instrumented ones | yes | **no** (#161) | not tested |
| The same in a shared library the program links with | **no** (#158) | **no** (#158) | not tested |
| A terminator placed in `.CRT$XTU`, in instrumented code | — | — | yes |

In the cases marked **no**, the report runs before that code: the blocks it releases are listed
as leaks. The test suite expects exactly these failures, so a change in behaviour shows up.

On Windows, the report is itself a terminator, in `.CRT$XTY`, and the runtime's allocation table
and its lock are never destroyed: blocks can still be released after the destructors of every
static object.

## LLVM versions

CoreTrace Compiler builds and instruments with LLVM 16 to 23. Most behaviour is the same on
every version; this section lists what differs, and why.

### How instrumented code is compiled

The instrumentation passes run on the optimized IR, after clang's optimizations. Clang's backend
then writes the instrumented module, with every code generation option of the compilation, such
as `-ffunction-sections`, `-fdata-sections`, `-gsplit-dwarf` and `-fstack-usage`, and without
optimizing it again. With `--instrument`, the compiler adds to clang's arguments:

- `-gline-tables-only`, unless debug information is already requested: the passes record the
  source location of each allocation;
- `-fPIE` on Linux, unless `-fPIC` or `-fPIE` is given;
- one `-fno-builtin-<name>` per C allocation function the alloc module rewrites (`malloc`,
  `calloc`, `realloc`, `aligned_alloc`, `posix_memalign`, `free`). Otherwise clang may remove or
  merge their calls before the passes run, and the allocations would go untracked.

The C++ standard also lets the optimizer omit the allocation of a new-expression, and clang
marks those `operator new` and `operator delete` calls to allow it. `-fno-builtin-<name>` cannot
name these operators. From LLVM 18, a pass at the start of clang's optimization pipeline removes
that mark from the calls the alloc module tracks, so that they are still there when it runs.

### LLVM 16 and 17

Clang only lets a library run a pass at the start of its optimization pipeline from LLVM 18. On
LLVM 16 and 17, instrumented code is therefore compiled with the blanket `-fno-builtin`, which
also keeps `new`/`delete` pairs, but turns off every C library builtin. This has three
consequences, which LLVM 18 and later do not have:

- **`setjmp` and the other functions that return twice** (`sigsetjmp`, `getcontext`, `vfork`…)
  lose the `returns_twice` attribute. With optimizations on, LLVM may then apply
  transformations that assume the call returns once, such as turning it into a tail call or
  reusing a stack slot across it. A local variable read after `longjmp` can be wrong. No such
  failure has been observed at run time, but nothing rules it out. The bounds module recognizes
  these functions by name, so its record of stack objects stays correct.
- **Explicit `memcpy`, `memset` and `memmove` calls are not bounds-checked.** They stay calls to
  the C library, while the bounds module checks the memory intrinsics that clang emits for
  them from LLVM 18.
- **Calls to the C library are not optimized**, for instance `memcpy` is not expanded inline.

If a program uses `setjmp`/`longjmp`, instrument it with LLVM 18 or later, or compile the files
that use them at `-O0`, where these transformations do not run. `cc --version` prints the LLVM
version the compiler was built with. #153 tracks a fix for these two versions.

### Other notes per version

- **Windows with LLVM 23**: building CoreTrace Compiler against the official archive is not
  covered yet. The archive needs zlib and zstd, which it does not ship (#149).
- **Clang 23, Apple targets**: Objective-C messages go through selector stubs such as
  `objc_msgSend$new`. The alloc module recognizes them, so the objects are tracked as with
  earlier versions.
- **LLVM 19, arm64**: LLVM's own AArch64 code generator crashes on some IR at `-O0`, with or
  without `--instrument`. This is an LLVM bug: plain `clang -c` crashes on the same input.
  The pass fuzzer reports such modules as skipped; in CI, it has met this crash with LLVM 19
  only.

## Objective-C and Objective-C++

On macOS, `.m` and `.mm` files (or `-x objective-c` and `-x objective-c++`) are compiled against the
Apple Objective-C runtime; Objective-C++ sources also link the C++ standard library. The usual
clang flags apply, such as `-fobjc-arc`, and Foundation programs link with `-framework Foundation`:

```zsh
./cc --instrument --ct-modules=alloc -fobjc-arc -framework Foundation -o app main.m
```

- `trace` prints methods under their Objective-C name, such as `-[Greeter greet:]`.
- `alloc` tracks `malloc`/`free` and C++ `new`/`delete` called from methods, and the Objective-C
  objects the program creates with `+alloc`, `+allocWithZone:`, `+new` or `[[X alloc] init]`,
  with or without ARC, until the Objective-C runtime deallocates them. Objects still alive at
  exit are reported with the other leaks. To see deallocations, the runtime replaces
  `-[NSObject dealloc]` at start-up with a wrapper that calls the original.

Limits:
- Objects of Apple's system classes (Foundation, CoreFoundation, libobjc) and of classes outside
  the `NSObject` hierarchy, such as `NSProxy` subclasses, are not tracked: some live for the
  whole process or are released without `-[NSObject dealloc]`, and would read as leaks.
- Only macOS with the Apple runtime is tested; Linux (GNUstep) and Windows are not.

## Auto-free GC Scan (Conservative)

The runtime can run a conservative root scan (stack/regs/globals) to decide whether an
allocation is still reachable. The periodic scan keeps every block reachable from those roots,
directly or through other tracked blocks, and releases the others. It runs on macOS only, and is
optional and controlled by environment variables.

Typical usage:

```zsh
CT_AUTOFREE_SCAN=1 CT_AUTOFREE_SCAN_START=1 \
CT_AUTOFREE_SCAN_PTR=0 \
CT_AUTOFREE_SCAN_GLOBALS=0 \
CT_AUTOFREE_SCAN_PERIOD_MS=200 \
CT_AUTOFREE_SCAN_BUDGET_MS=100 \
CT_DEBUG_AUTOFREE_SCAN=1 \
./app
```

Without GC scan (auto-free only from compile-time analysis):

```zsh
./app
```

With GC scan (conservative root scan):

```zsh
CT_AUTOFREE_SCAN=1 CT_AUTOFREE_SCAN_START=1 \
CT_AUTOFREE_SCAN_PTR=1 \
CT_AUTOFREE_SCAN_GLOBALS=1 \
CT_AUTOFREE_SCAN_PERIOD_MS=200 \
CT_AUTOFREE_SCAN_BUDGET_MS=100 \
CT_DEBUG_AUTOFREE_SCAN=1 \
./app
```

Environment variables (ms can be floating-point; US/NS override MS):
- `CT_AUTOFREE_SCAN=1`: enable conservative scanning.
- `CT_AUTOFREE_SCAN_START=1`: run a scan at startup and launch a periodic scan thread.
- `CT_AUTOFREE_SCAN_PERIOD_MS=N`: period between scans when START=1 (default: 1000ms).
- `CT_AUTOFREE_SCAN_PERIOD_US=N`: period between scans in microseconds.
- `CT_AUTOFREE_SCAN_PERIOD_NS=N`: period between scans in nanoseconds.
- `CT_AUTOFREE_SCAN_BUDGET_MS=N`: time budget per scan; if exceeded, no frees are performed
  (default: 5ms; 0 for no limit).
- `CT_AUTOFREE_SCAN_BUDGET_US=N`: time budget per scan in microseconds.
- `CT_AUTOFREE_SCAN_BUDGET_NS=N`: time budget per scan in nanoseconds.
- `CT_AUTOFREE_SCAN_STACK=0/1`: scan thread stacks (default: 1).
- `CT_AUTOFREE_SCAN_REGS=0/1`: scan registers (default: 1).
- `CT_AUTOFREE_SCAN_GLOBALS=0/1`: scan globals (`__DATA` segments) (default: 1).
- `CT_AUTOFREE_SCAN_INTERIOR=0/1`: treat interior pointers as roots (default: 1).
- `CT_AUTOFREE_SCAN_PTR=0/1`: enable per-pointer scan before auto-free (default: 1).
- `CT_DEBUG_AUTOFREE_SCAN=1`: log scan activity only when a scan frees or times out.
- `CT_DEBUG_AUTOFREE_SCAN=2`: log every scan plus per-pointer scans.

Use cases:
- Keep `CT_AUTOFREE_SCAN_PTR=1` for a conservative safety check before any auto-free.
- Set `CT_AUTOFREE_SCAN_PTR=0` for immediate auto-free on unreachable sites, and rely on periodic scans.
- Disable `CT_AUTOFREE_SCAN_GLOBALS=0` to reduce scan cost when you see timeouts: the periodic
  scan then releases blocks that only globals refer to.

Notes:
- This is conservative: stale values on stack/regs/globals can keep a pointer "reachable".
- If a scan times out (budget exceeded), nothing is freed to avoid false positives.

Vtable diagnostics (`--ct-vtable-diag`):
- Logs an init line with alloc-tracking state: enabled/disabled and the reason if disabled.
- Warns when vptr/vtable data is invalid: null `this` pointer, missing vptr, or missing typeinfo.
- Warns when vtable address cannot be resolved to a module (dladdr/phdr/dyld fallback).
- Warns on module mismatch when both vtable module and target module are resolved.
- Notes partial resolution cases (only vtable or target resolved) and escalates if target is non-exec.
- Warns on static vs dynamic type mismatch when a static type is available.
- Warns when the object appears freed (alloc table says state=freed).

## Using coretrace-compiler in Your Project

You can integrate coretrace-compiler into your own CMake project using FetchContent or by building
it as a standalone library. The sample project in `extern-project` shows a minimal setup.

```zsh
cd extern-project
mkdir -p build && cd build

cmake .. -DLLVM_DIR=$(brew --prefix llvm@20)/lib/cmake/llvm/ \
  -DClang_DIR=$(brew --prefix llvm@20)/lib/cmake/clang

make
```

This example demonstrates how to:
- Link against `compilerlib_static` or `compilerlib_shared`.
- Use the public API from `include/compilerlib/`.
- Build with the correct LLVM/Clang version (20 on macOS).

You can use the same pattern in any external project by passing the correct `LLVM_DIR`
and `Clang_DIR` paths to CMake.
