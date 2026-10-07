// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "compilerlib/attributes.hpp"
#include "../clang_compat.hpp"

#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>

#include <string>

namespace compilerlib::emit
{
    // The optimization record of -fsave-optimization-record, open on a module's context from
    // its optimization to its output, so that it holds the remarks of both, as a plain
    // compilation's does. The file stays only once kept: a failed compilation leaves none.
    class OptimizationRecord
    {
      public:
        OptimizationRecord() = default;
        ~OptimizationRecord();
        OptimizationRecord(const OptimizationRecord&) = delete;
        OptimizationRecord& operator=(const OptimizationRecord&) = delete;

        // Sets up the record `options` ask for, if any, on `context`.
        CT_NODISCARD bool open(llvm::LLVMContext& context, const clang::CodeGenOptions& options,
                               std::string& error);
        void keep();

      private:
        llvm::LLVMContext* context_ = nullptr;
        clang_compat::RemarkFile file_;
    };

    // The steps below run while BackendDiagnostics is installed on the module's context: the
    // diagnostics of the optimization and of the code generation then go to the instance's
    // diagnostics, and an error there fails the step.

    // Runs Clang's optimization pipeline on `module`, with `options`, as its frontend action
    // would.
    CT_NODISCARD bool optimizeModule(llvm::Module& module, clang::CompilerInstance& ci,
                                     const clang::CodeGenOptions& options, std::string& error);

    enum class OutputKind
    {
        Object,
        IR,
        Bitcode
    };

    // Writes the instrumented module through Clang's backend, with the instance's target and
    // code generation options and without its optimization pipeline, which ran before the
    // instrumentation (#131). Code generation also writes the secondary outputs the options
    // ask for, such as split debug information or stack usage.
    CT_NODISCARD bool emitToBuffer(llvm::Module& module, clang::CompilerInstance& ci,
                                   OutputKind kind, llvm::SmallVectorImpl<char>& buffer,
                                   std::string& error);

    // emitToBuffer, then the buffer written to `outputPath`. A failure leaves neither the
    // output nor the secondary outputs code generation started to write.
    CT_NODISCARD bool emitToFile(llvm::Module& module, clang::CompilerInstance& ci, OutputKind kind,
                                 llvm::StringRef outputPath, std::string& error);
} // namespace compilerlib::emit
