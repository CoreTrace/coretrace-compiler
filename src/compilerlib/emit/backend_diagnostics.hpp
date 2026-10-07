// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <clang/Basic/SourceLocation.h>
#include <llvm/ADT/StringMap.h>

#include <memory>

namespace clang
{
    class CodeGenOptions;
    class DiagnosticsEngine;
    class LangOptions;
    class SourceManager;
} // namespace clang

namespace llvm
{
    struct DiagnosticHandler;
    class LLVMContext;
} // namespace llvm

namespace compilerlib::emit
{
    // Where the frontend declared each function of a module, by mangled name. The backend
    // locates there the diagnostics that name a function, such as a frame larger than
    // -Wframe-larger-than, and those that have no debug location.
    using FunctionLocations = llvm::StringMap<clang::SourceLocation>;

    // Reports the diagnostics of a module's optimization and code generation through Clang's
    // diagnostics, as Clang's backend consumer does: with Clang's diagnostic IDs, the -Rpass*
    // selection of the code generation options, and source locations. Warning options such
    // as -Werror and -w therefore apply to them, and errors count as Clang's (#167).
    // Installed on the module's context while it lives; the previous handler comes back
    // afterwards. The frontend action has ended: the diagnostic consumer processes the source
    // file again meanwhile, as a printer needs to show source excerpts.
    class BackendDiagnostics
    {
      public:
        BackendDiagnostics(llvm::LLVMContext& context, clang::DiagnosticsEngine& diagnostics,
                           clang::SourceManager& sources, const clang::LangOptions& language,
                           const clang::CodeGenOptions& options, FunctionLocations functions);
        ~BackendDiagnostics();
        BackendDiagnostics(const BackendDiagnostics&) = delete;
        BackendDiagnostics& operator=(const BackendDiagnostics&) = delete;

      private:
        llvm::LLVMContext& context_;
        clang::DiagnosticsEngine& diagnostics_;
        std::unique_ptr<llvm::DiagnosticHandler> previous_;
    };
} // namespace compilerlib::emit
