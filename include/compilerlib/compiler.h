// SPDX-License-Identifier: Apache-2.0
#ifndef COMPILERLIB_COMPILER_H
#define COMPILERLIB_COMPILER_H

#include "compilerlib/compiler_c.h"

#include <string>
#include <vector>

namespace compilerlib
{

    enum class OutputMode
    {
        ToFile,
        /// Textual LLVM IR in `CompileResult::llvmIR`, for `-S -emit-llvm`.
        ToMemory,
        /// The bitcode `-emit-llvm -c` would write to its output file, in
        /// `CompileResult::llvmBitcode` instead; the output file is not written. Other outputs
        /// the arguments ask for, such as a dependency file, still are.
        ToMemoryBitcode,
    };

    struct CompileResult
    {
        bool success;
        std::string diagnostics;
        std::string llvmIR;
        std::string llvmBitcode;
    };

    // std::pair<bool, std::string> compile(const std::vector<std::string>& args);
    /// Compiles as clang does with `args`, in this process.
    ///
    /// `-working-directory <dir>` gives the compilation a working directory of its own: the
    /// sources, the include directories and the files it writes are relative to `<dir>`, and a
    /// relative `<dir>` to the process's working directory, which stays where it is. A
    /// compilation with a job that runs as a program of its own, as linking and assembling do,
    /// moves the whole process to `<dir>` instead, as clang does, and leaves it there.
    ///
    /// Compilations may run from several threads at once. A compilation that moves the process
    /// must not run while another compilation of the process does, nor one that passes an
    /// option that Clang applies to LLVM's state of the whole process: `-mllvm`, `-mdebug-pass`,
    /// `-mlimit-float-precision` or `-ftime-report`.
    CompileResult compile(const std::vector<std::string>& args,
                          OutputMode mode = OutputMode::ToFile, bool instrument = false);

    // compile_c used to be declared inside this namespace; keep that spelling valid.
    using ::compile_c;

} // namespace compilerlib

#endif // COMPILERLIB_COMPILER_H
