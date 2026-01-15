# coretrace-compiler

#### BUILD

```zsh
mkdir -p build && cd build
./build.sh
```

or

```zsh
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DLLVM_DIR=$(brew --prefix llvm)/lib/cmake/llvm \
         -DClang_DIR=$(brew --prefix llvm)/lib/cmake/clang \
         -DUSE_SHARED_LIB=OFF
```

#### CORETRACE-COMPILER USAGE

```zsh
./cc -S -emit-llvm test.cc
./cc -S test.cc
./cc -c test.c
./cc -c test.c -O2
./cc --instrument -o app main.c
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
