#pragma once

#include "compilerlib/attributes.hpp"

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
    CT_NODISCARD bool emitObjectFile(llvm::Module& module, const clang::CompilerInstance& ci,
                                     llvm::StringRef outputPath, std::string& error);
    CT_NODISCARD bool emitLLVMIRFile(llvm::Module& module, llvm::StringRef outputPath,
                                     std::string& error);
    CT_NODISCARD bool emitBitcodeFile(llvm::Module& module, llvm::StringRef outputPath,
                                     std::string& error);
} // namespace compilerlib::emit
