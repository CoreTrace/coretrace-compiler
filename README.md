# CoreTrace Compiler

CoreTrace Compiler is a Clang/LLVM-based wrapper that can emit LLVM IR, build binaries, and optionally
instrument code with runtime checks (alloc/bounds/trace/vtable). It can run in a file-based mode or
an in-memory mode for tooling pipelines.

## Build

Quick build:

```zsh
mkdir -p build && cd build
./build.sh
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
./cc --in-mem -emit-llvm test.c
```

## CLI Options

Core options:
- `--instrument`: enable CoreTrace instrumentation (required for `--ct-*` flags).
- `--in-mem`, `--in-memory`: print LLVM IR to stdout (use with `-emit-llvm`).

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
- Alloc instrumentation rewrites `malloc/free/calloc/realloc` and basic C++ `operator new/delete`
  (scalar/array). Sized/aligned new/delete overloads are not handled yet.
- Vtable tooling requires C++ and an Itanium ABI (macOS/Linux).
- Clang automatically adds `optnone` at `-O0`. Use `--ct-optnone` to force the attribute even when
  passing `-Xclang -disable-O0-optnone`.

## Auto-free GC Scan (Conservative)

The runtime can run a conservative root scan (stack/regs/globals) to decide whether an
allocation is still reachable. This is optional and controlled by environment variables.

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
- `CT_AUTOFREE_SCAN_BUDGET_MS=N`: time budget per scan; if exceeded, no frees are performed.
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
- Disable `CT_AUTOFREE_SCAN_GLOBALS=0` to reduce scan cost when you see timeouts.

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
