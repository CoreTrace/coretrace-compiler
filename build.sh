cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DLLVM_DIR=$(brew --prefix llvm)/lib/cmake/llvm \
         -DClang_DIR=$(brew --prefix llvm)/lib/cmake/clang \
         -DUSE_SHARED_LIB=OFF \
&& make -j4
