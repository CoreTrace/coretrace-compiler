// SPDX-License-Identifier: Apache-2.0
#include "llvm_output.hpp"
#include "../clang_compat.hpp"

#include <clang/Basic/CodeGenOptions.h>
#include <clang/Frontend/CompilerInstance.h>

#include <llvm/IR/DiagnosticHandler.h>
#include <llvm/IR/DiagnosticInfo.h>
#include <llvm/IR/DiagnosticPrinter.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

#include <memory>
#include <string>

namespace compilerlib::emit
{
    namespace
    {
        // Keeps the errors code generation reports, which LLVM prints before ending the
        // process when no handler takes them. Other diagnostics are left to LLVM.
        class CodeGenErrorCollector : public llvm::DiagnosticHandler
        {
          public:
            explicit CodeGenErrorCollector(std::string& errors) : errors_(errors) {}

            bool handleDiagnostics(const llvm::DiagnosticInfo& info) override
            {
                if (info.getSeverity() != llvm::DS_Error)
                    return false;
                llvm::raw_string_ostream stream(errors_);
                llvm::DiagnosticPrinterRawOStream printer(stream);
                stream << "error: ";
                info.print(printer);
                stream << '\n';
                return true;
            }

          private:
            std::string& errors_;
        };

        // Runs `writer` on `outputPath`. An error of the stream itself, from writing or
        // from closing, fails the output here: a stream destroyed with one ends the process.
        template <typename Writer>
        CT_NODISCARD bool writeOutputFile(llvm::StringRef outputPath, std::string& error,
                                          Writer&& writer)
        {
            std::error_code ec;
            llvm::raw_fd_ostream dest(outputPath, ec, llvm::sys::fs::OF_None);
            if (ec)
            {
                error = ec.message();
                return false;
            }

            const bool written = writer(dest);
            // Closing reports the last write errors; "-" is stdout, which stays open.
            if (outputPath == "-")
                dest.flush();
            else
                dest.close();
            if (dest.has_error())
            {
                error =
                    "error: ct: cannot write " + outputPath.str() + ": " + dest.error().message();
                dest.clear_error();
                return false;
            }
            if (!written)
            {
                if (error.empty())
                    error = "failed to write file";
                return false;
            }
            return true;
        }

        CT_NODISCARD clang::BackendAction backendAction(OutputKind kind)
        {
            switch (kind)
            {
            case OutputKind::Object:
                return clang::Backend_EmitObj;
            case OutputKind::IR:
                return clang::Backend_EmitLL;
            case OutputKind::Bitcode:
                return clang::Backend_EmitBC;
            }
            return clang::Backend_EmitObj;
        }

        // The files code generation writes besides its output. Clang's backend keeps them
        // even when code generation reported an error through the context's handler, as
        // an invalid inline assembly instruction does.
        void removeSecondaryOutputs(const clang::CodeGenOptions& options)
        {
            for (const std::string& path :
                 {options.SplitDwarfOutput, clang_compat::stackUsageOutput(options)})
            {
                if (!path.empty() && path != "-")
                    llvm::sys::fs::remove(path);
            }
        }
    } // namespace

    namespace
    {
        // Runs Clang's backend with `options`, collecting the errors it reports.
        CT_NODISCARD bool runBackend(llvm::Module& module, clang::CompilerInstance& ci,
                                     clang::CodeGenOptions& options, clang::BackendAction action,
                                     std::unique_ptr<llvm::raw_pwrite_stream> stream,
                                     std::string& error)
        {
            llvm::LLVMContext& context = module.getContext();
            std::string codegenErrors;
            std::unique_ptr<llvm::DiagnosticHandler> previous = context.getDiagnosticHandler();
            context.setDiagnosticHandler(std::make_unique<CodeGenErrorCollector>(codegenErrors));
            const unsigned errorsBefore = ci.getDiagnostics().getNumErrors();
            clang_compat::emitBackendOutput(ci, options, module, action, std::move(stream));
            context.setDiagnosticHandler(std::move(previous));

            if (!codegenErrors.empty())
            {
                error = std::move(codegenErrors);
                return false;
            }
            if (ci.getDiagnostics().getNumErrors() != errorsBefore)
            {
                error = "error: ct: code generation failed";
                return false;
            }
            return true;
        }
    } // namespace

    OptimizationRecord::~OptimizationRecord()
    {
        if (context_)
            clang_compat::releaseOptimizationRecord(*context_, file_);
    }

    bool OptimizationRecord::open(llvm::LLVMContext& context, const clang::CodeGenOptions& options,
                                  std::string& error)
    {
        llvm::Expected<clang_compat::RemarkFile> file =
            clang_compat::setupOptimizationRecord(context, options);
        if (!file)
        {
            error = "error: ct: cannot write the optimization record " + options.OptRecordFile +
                    ": " + llvm::toString(file.takeError());
            return false;
        }
        file_ = std::move(*file);
        context_ = &context;
        return true;
    }

    void OptimizationRecord::keep()
    {
        if (file_)
            file_->keep();
    }

    bool optimizeModule(llvm::Module& module, clang::CompilerInstance& ci,
                        const clang::CodeGenOptions& options, std::string& error)
    {
        clang::CodeGenOptions copy = options;
        return runBackend(module, ci, copy, clang::Backend_EmitNothing, nullptr, error);
    }

    bool emitToBuffer(llvm::Module& module, clang::CompilerInstance& ci, OutputKind kind,
                      llvm::SmallVectorImpl<char>& buffer, std::string& error)
    {
        // The optimization pipeline already ran, before the instrumentation: only the
        // output is produced, with every other option of the invocation.
        clang::CodeGenOptions options = ci.getCodeGenOpts();
        options.DisableLLVMPasses = true;
        if (runBackend(module, ci, options, backendAction(kind),
                       std::make_unique<llvm::raw_svector_ostream>(buffer), error))
        {
            return true;
        }
        removeSecondaryOutputs(options);
        return false;
    }

    bool emitToFile(llvm::Module& module, clang::CompilerInstance& ci, OutputKind kind,
                    llvm::StringRef outputPath, std::string& error)
    {
        llvm::SmallString<0> buffer;
        if (!emitToBuffer(module, ci, kind, buffer, error))
            return false;
        if (writeOutputFile(outputPath, error,
                            [&](llvm::raw_fd_ostream& dest) -> bool
                            {
                                dest << buffer;
                                return true;
                            }))
        {
            return true;
        }
        removeSecondaryOutputs(ci.getCodeGenOpts());
        return false;
    }
} // namespace compilerlib::emit
