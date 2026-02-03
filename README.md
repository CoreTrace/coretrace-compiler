# coretrace-compiler

#### BUILD

```zsh
mkdir -p build && cd build
./build.sh
```

### Code style (clang-format)

- Version cible : `clang-format` 17 (utilisée dans la CI).
- Formater localement : `./scripts/format.sh`
- Vérifier sans modifier : `./scripts/format-check.sh`
- CMake : `cmake --build build --target format` ou `--target format-check`
- CI : le job GitHub Actions `clang-format` échoue si un fichier n’est pas formaté.

#### BUILD (macOS)

```zsh
mkdir -p build && cd build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
         -DLLVM_DIR=$(brew --prefix llvm)/lib/cmake/llvm \
         -DClang_DIR=$(brew --prefix llvm)/lib/cmake/clang \
         -DUSE_SHARED_LIB=OFF
```

#### BUILD (Linux)

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

#### CORETRACE-COMPILER USAGE

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

Options:
- --instrument: enable instrumentation (required for --ct-* flags).
- --in-mem, --in-memory: print LLVM IR to stdout (use with -emit-llvm).
- --ct-shadow: enable shadow memory in the produced binary.
- --ct-shadow-aggressive, --ct-shadow=aggressive: enable the aggressive shadow mode.
- --ct-bounds-no-abort: do not abort on bounds errors.
- --ct-modules=trace,alloc,bounds,vtable: select which instrumentation passes to run (supports "all").
- --ct-no-trace, --ct-trace: disable/enable function entry/exit instrumentation.
- --ct-no-alloc, --ct-alloc: disable/enable malloc/free instrumentation.
- --ct-no-bounds, --ct-bounds: disable/enable bounds checks.
- --ct-no-autofree, --ct-autofree: disable/enable auto-free on unreachable allocations.
- --ct-no-alloc-trace, --ct-alloc-trace: disable/enable malloc/free tracing logs.
- --ct-vcall-trace, --ct-no-vcall-trace: enable/disable virtual call tracing (Itanium ABI).
- --ct-vtable-diag, --ct-no-vtable-diag: enable/disable vtable diagnostics (extra warnings).

All other arguments are forwarded to clang (e.g., -O2, -g, -I, -D, -L, -l, -std=...).
Alloc instrumentation rewrites malloc/free/calloc/realloc and basic C++ operator new/delete
(scalar/array). Sized/aligned new/delete overloads are not handled yet.
Vtable tooling (module vtable) requires C++ and an Itanium ABI (macOS/Linux).

#### Auto-free GC scan (conservative)

The runtime can run a conservative root scan (stack/regs/globals) to decide whether an
allocation is still reachable. This is optional and controlled by env vars.

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
- CT_AUTOFREE_SCAN=1: enable conservative scanning.
- CT_AUTOFREE_SCAN_START=1: run a scan at startup and launch a periodic scan thread.
- CT_AUTOFREE_SCAN_PERIOD_MS=N: period between scans when START=1 (default: 1000ms).
- CT_AUTOFREE_SCAN_PERIOD_US=N: period between scans in microseconds.
- CT_AUTOFREE_SCAN_PERIOD_NS=N: period between scans in nanoseconds.
- CT_AUTOFREE_SCAN_BUDGET_MS=N: time budget per scan; if exceeded, no frees are performed.
- CT_AUTOFREE_SCAN_BUDGET_US=N: time budget per scan in microseconds.
- CT_AUTOFREE_SCAN_BUDGET_NS=N: time budget per scan in nanoseconds.
- CT_AUTOFREE_SCAN_STACK=0/1: scan thread stacks (default: 1).
- CT_AUTOFREE_SCAN_REGS=0/1: scan registers (default: 1).
- CT_AUTOFREE_SCAN_GLOBALS=0/1: scan globals (__DATA segments) (default: 1).
- CT_AUTOFREE_SCAN_INTERIOR=0/1: treat interior pointers as roots (default: 1).
- CT_AUTOFREE_SCAN_PTR=0/1: enable per-pointer scan before auto-free (default: 1).
- CT_DEBUG_AUTOFREE_SCAN=1: log scan activity (timed_out/free_count).

Use-cases:
- Keep CT_AUTOFREE_SCAN_PTR=1 if you want a conservative safety check before any auto-free.
- Set CT_AUTOFREE_SCAN_PTR=0 if you want immediate auto-free for "unreachable" sites and
  only rely on periodic scans.
- Disable GLOBALS (CT_AUTOFREE_SCAN_GLOBALS=0) to reduce scan cost when you see timeouts.

Notes:
- This is conservative: stale values on stack/regs/globals can keep a pointer "reachable".
- If a scan times out (budget exceeded), nothing is freed to avoid false positives.

Vtable diagnostics (--ct-vtable-diag):
- Logs an init line with alloc-tracking state: enabled/disabled and the reason if disabled.
- Warns when vptr/vtable data is invalid: null this pointer, missing vptr, or missing typeinfo.
- Warns when vtable address cannot be resolved to a module (dladdr/phdr/dyld fallback).
- Warns on module mismatch when both vtable module and target module are resolved.
- Notes partial resolution cases (only vtable or target resolved) and escalates if target is non-exec.
- Warns on static vs dynamic type mismatch when a static type is available.
- Warns when the object appears freed (alloc table says state=freed).

#### HOW TO USE THIS CORETRACE-COMPILER IN YOUR PROJECT

You can integrate coretrace-compiler directly into your own CMake project using FetchContent or by building it as a standalone library.

Below is a minimal example using the sample project located in the extern-project directory.

##### (Please use a recent LLVM version for best compatibility : llvm@19/20/...)

```zsh
cd extern-project
mkdir -p build && cd build

cmake .. -DLLVM_DIR=$(brew --prefix llvm@20)/lib/cmake/llvm/ \
         -DClang_DIR=$(brew --prefix llvm@20)/lib/cmake/clang

make
```

This example demonstrates how to:
•	Link against compilerlib_static or compilerlib_shared
•	Use the public API from include/compilerlib/
•	Build your project with the correct LLVM/Clang version (20 on macOS)

You can use the same pattern in any external project by passing the correct LLVM_DIR and Clang_DIR paths to CMake.
