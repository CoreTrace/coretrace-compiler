cmake .. -DCMAKE_BUILD_TYPE=Release 				\
         -DLLVM_DIR=/opt/homebrew/opt/llvm/lib/cmake/llvm 	\
	 -DClang_DIR=/opt/homebrew/opt/llvm/lib/cmake/clang	\
&& make -j4
