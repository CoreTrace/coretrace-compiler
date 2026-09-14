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
        ToMemory,
    };

    struct CompileResult
    {
        bool success;
        std::string diagnostics;
        std::string llvmIR;
    };

    // std::pair<bool, std::string> compile(const std::vector<std::string>& args);
    CompileResult compile(const std::vector<std::string>& args,
                          OutputMode mode = OutputMode::ToFile, bool instrument = false);

    // compile_c used to be declared inside this namespace; keep that spelling valid.
    using ::compile_c;

} // namespace compilerlib

#endif // COMPILERLIB_COMPILER_H
