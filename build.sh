cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DLLVM_DIR=$(brew --prefix llvm@20)/lib/cmake/llvm \
         -DClang_DIR=$(brew --prefix llvm@20)/lib/cmake/clang \
         -DUSE_SHARED_LIB=OFF \
&& make -j4
