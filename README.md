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
