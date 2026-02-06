#include "llvm_output.hpp"

#include <clang/Frontend/CompilerInstance.h>

#include <llvm/Bitcode/BitcodeWriter.h>
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
        CT_NODISCARD llvm::CodeGenOptLevel toCodeGenOptLevel(unsigned level)
        {
            switch (level)
            {
            case 0:
                return llvm::CodeGenOptLevel::None;
            case 1:
                return llvm::CodeGenOptLevel::Less;
            case 2:
                return llvm::CodeGenOptLevel::Default;
            case 3:
                return llvm::CodeGenOptLevel::Aggressive;
            default:
                return llvm::CodeGenOptLevel::Default;
            }
        }

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

            if (!writer(dest))
            {
                if (error.empty())
                    error = "failed to write file";
                return false;
            }
            dest.flush();
            if (dest.has_error())
            {
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

        std::unique_ptr<llvm::TargetMachine>
        createTargetMachine(llvm::Module& module, const clang::CompilerInstance& ci,
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
            std::unique_ptr<llvm::TargetMachine> targetMachine(
                target->createTargetMachine(targetTriple, ci.getTargetOpts().CPU,
                                            buildTargetFeatures(ci), options, relocModel,
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
        std::unique_ptr<llvm::TargetMachine> targetMachine =
            createTargetMachine(module, ci, error);
        if (!targetMachine)
            return false;

        return writeOutputFile(outputPath, error, [&](llvm::raw_fd_ostream& dest) -> bool {
            llvm::legacy::PassManager pass;
            if (targetMachine->addPassesToEmitFile(pass, dest, nullptr,
                                                   llvm::CodeGenFileType::ObjectFile))
            {
                error = "target does not support object emission";
                return false;
            }

            pass.run(module);
            return true;
        });
    }

    bool emitLLVMIRFile(llvm::Module& module, llvm::StringRef outputPath, std::string& error)
    {
        return writeOutputFile(outputPath, error, [&](llvm::raw_fd_ostream& dest) -> bool {
            module.print(dest, nullptr);
            return !dest.has_error();
        });
    }

    bool emitBitcodeFile(llvm::Module& module, llvm::StringRef outputPath, std::string& error)
    {
        return writeOutputFile(outputPath, error, [&](llvm::raw_fd_ostream& dest) -> bool {
            llvm::WriteBitcodeToFile(module, dest);
            return !dest.has_error();
        });
    }
} // namespace compilerlib::emit
