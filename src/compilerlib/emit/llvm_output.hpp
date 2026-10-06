// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "compilerlib/attributes.hpp"

#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>

#include <string>

namespace llvm
{
    class Module;
}

namespace clang
{
    class CompilerInstance;
}

namespace compilerlib::emit
{
    enum class OutputKind
    {
        Object,
        IR,
        Bitcode
    };

    // Writes the instrumented module through Clang's backend, with the instance's target and
    // code generation options and without its optimization pipeline, which ran before the
    // instrumentation (#131). Code generation also writes the secondary outputs the options
    // ask for, such as split debug information or stack usage. On failure, `error` holds the
    // errors code generation reported; those Clang reports through its own diagnostics go to
    // the instance's consumer.
    CT_NODISCARD bool emitToBuffer(llvm::Module& module, clang::CompilerInstance& ci,
                                   OutputKind kind, llvm::SmallVectorImpl<char>& buffer,
                                   std::string& error);

    // emitToBuffer, then the buffer written to `outputPath`. A failure leaves neither the
    // output nor the secondary outputs code generation started to write.
    CT_NODISCARD bool emitToFile(llvm::Module& module, clang::CompilerInstance& ci, OutputKind kind,
                                 llvm::StringRef outputPath, std::string& error);
} // namespace compilerlib::emit
