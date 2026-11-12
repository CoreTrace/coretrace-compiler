// SPDX-License-Identifier: Apache-2.0
#ifndef COMPILERLIB_COMPILE_MODULE_HPP
#define COMPILERLIB_COMPILE_MODULE_HPP

#include "compilerlib/attributes.hpp"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <memory>
#include <string>
#include <vector>

namespace compilerlib
{

    struct ModuleResult
    {
        bool success = false;
        std::string diagnostics;
        /// The module, in the context compileToModule was given; null when the compilation
        /// failed.
        std::unique_ptr<llvm::Module> module;
    };

    /// Compiles one source to the module that compile() writes, created in `context`, for a
    /// host that works on the module itself: it is neither written as text or bitcode nor read
    /// back. `args` are those of an LLVM IR compilation, `-S -emit-llvm` or `-c -emit-llvm`;
    /// the output file is not written, and the other files the arguments ask for, such as a
    /// dependency file, are. With `instrument`, the module is the instrumented one.
    ///
    /// Code generation sets options of `context` as it does with clang, except whether value
    /// names are discarded, which is restored: modules read into `context` later keep theirs.
    /// With LLVM 16, a context that discards names keeps them after a compilation that names
    /// values, which LLVM 16 could not destroy in a context that discards names.
    /// No other thread may use `context` during the compilation.
    CT_NODISCARD ModuleResult compileToModule(const std::vector<std::string>& args,
                                              llvm::LLVMContext& context, bool instrument = false);

} // namespace compilerlib

#endif // COMPILERLIB_COMPILE_MODULE_HPP
