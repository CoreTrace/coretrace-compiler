// SPDX-License-Identifier: Apache-2.0
#include "llvm_output.hpp"
#include "llvm_compat.hpp"

#include <clang/Basic/TargetOptions.h>
#include <clang/Frontend/CompilerInstance.h>

#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/DiagnosticHandler.h>
#include <llvm/IR/DiagnosticInfo.h>
#include <llvm/IR/DiagnosticPrinter.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>

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

        CT_NODISCARD llvm_compat::CodeGenOptLevel toCodeGenOptLevel(unsigned level)
        {
            switch (level)
            {
            case 0:
                return llvm_compat::kCodeGenOptNone;
            case 1:
                return llvm_compat::kCodeGenOptLess;
            case 2:
                return llvm_compat::kCodeGenOptDefault;
            case 3:
                return llvm_compat::kCodeGenOptAggressive;
            default:
                return llvm_compat::kCodeGenOptDefault;
            }
        }

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

        std::string buildTargetFeatures(const clang::CompilerInstance& ci)
        {
            const auto& targetOpts = ci.getTargetOpts();
            std::string features;
            for (const auto& feature : targetOpts.FeaturesAsWritten)
            {
                if (!features.empty())
                    features += ",";
                features += feature;
            }
            return features;
        }

        std::unique_ptr<llvm::TargetMachine> createTargetMachine(llvm::Module& module,
                                                                 const clang::CompilerInstance& ci,
                                                                 std::string& error)
        {
            std::string targetTriple = module.getTargetTriple();

            if (targetTriple.empty())
                targetTriple = llvm::sys::getDefaultTargetTriple();
            module.setTargetTriple(targetTriple);

            std::string targetError;
            const llvm::Target* target =
                llvm::TargetRegistry::lookupTarget(targetTriple, targetError);
            if (!target)
            {
                error = targetError;
                return nullptr;
            }

            llvm::TargetOptions options;
            auto codegenLevel = toCodeGenOptLevel(ci.getCodeGenOpts().OptimizationLevel);
            // For position-independent code (needed for instrumented code and PIE executables),
            // explicitly set the relocation model to PIC.
            llvm::Reloc::Model relocModel = llvm::Reloc::PIC_;
            std::unique_ptr<llvm::TargetMachine> targetMachine(target->createTargetMachine(
                targetTriple, ci.getTargetOpts().CPU, buildTargetFeatures(ci), options, relocModel,
                std::nullopt, codegenLevel));
            if (!targetMachine)
            {
                error = "failed to create target machine";
                return nullptr;
            }

            module.setDataLayout(targetMachine->createDataLayout());
            return targetMachine;
        }
    } // namespace

    bool emitObjectFile(llvm::Module& module, const clang::CompilerInstance& ci,
                        llvm::StringRef outputPath, std::string& error)
    {
        std::unique_ptr<llvm::TargetMachine> targetMachine = createTargetMachine(module, ci, error);
        if (!targetMachine)
            return false;

        return writeOutputFile(outputPath, error,
                               [&](llvm::raw_fd_ostream& dest) -> bool
                               {
                                   llvm::legacy::PassManager pass;
                                   if (targetMachine->addPassesToEmitFile(pass, dest, nullptr,
                                                                          llvm_compat::kObjectFile))
                                   {
                                       error = "target does not support object emission";
                                       return false;
                                   }

                                   llvm::LLVMContext& context = module.getContext();
                                   std::string codegenErrors;
                                   std::unique_ptr<llvm::DiagnosticHandler> previous =
                                       context.getDiagnosticHandler();
                                   context.setDiagnosticHandler(
                                       std::make_unique<CodeGenErrorCollector>(codegenErrors));
                                   pass.run(module);
                                   context.setDiagnosticHandler(std::move(previous));
                                   if (!codegenErrors.empty())
                                   {
                                       error = std::move(codegenErrors);
                                       return false;
                                   }
                                   return true;
                               });
    }

    bool emitLLVMIRFile(llvm::Module& module, llvm::StringRef outputPath, std::string& error)
    {
        return writeOutputFile(outputPath, error,
                               [&](llvm::raw_fd_ostream& dest) -> bool
                               {
                                   module.print(dest, nullptr);
                                   return true;
                               });
    }

    bool emitBitcodeFile(llvm::Module& module, llvm::StringRef outputPath, std::string& error)
    {
        return writeOutputFile(outputPath, error,
                               [&](llvm::raw_fd_ostream& dest) -> bool
                               {
                                   llvm::WriteBitcodeToFile(module, dest);
                                   return true;
                               });
    }
} // namespace compilerlib::emit
