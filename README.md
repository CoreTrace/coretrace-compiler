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
```

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
