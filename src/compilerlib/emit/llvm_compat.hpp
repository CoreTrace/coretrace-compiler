// SPDX-License-Identifier: Apache-2.0
//
// Code generation interfaces that changed between LLVM 16 and 23. compilerlib builds
// against all of them; the differences between versions stay here.
#pragma once

#include <llvm/Config/llvm-config.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/TargetParser/Triple.h>

#include <string>

namespace compilerlib::emit::llvm_compat
{
#if LLVM_VERSION_MAJOR >= 18
    using CodeGenOptLevel = llvm::CodeGenOptLevel;
    inline constexpr CodeGenOptLevel kCodeGenOptNone = llvm::CodeGenOptLevel::None;
    inline constexpr CodeGenOptLevel kCodeGenOptLess = llvm::CodeGenOptLevel::Less;
    inline constexpr CodeGenOptLevel kCodeGenOptDefault = llvm::CodeGenOptLevel::Default;
    inline constexpr CodeGenOptLevel kCodeGenOptAggressive = llvm::CodeGenOptLevel::Aggressive;
    inline constexpr llvm::CodeGenFileType kObjectFile = llvm::CodeGenFileType::ObjectFile;
#else
    using CodeGenOptLevel = llvm::CodeGenOpt::Level;
    inline constexpr CodeGenOptLevel kCodeGenOptNone = llvm::CodeGenOpt::None;
    inline constexpr CodeGenOptLevel kCodeGenOptLess = llvm::CodeGenOpt::Less;
    inline constexpr CodeGenOptLevel kCodeGenOptDefault = llvm::CodeGenOpt::Default;
    inline constexpr CodeGenOptLevel kCodeGenOptAggressive = llvm::CodeGenOpt::Aggressive;
    inline constexpr llvm::CodeGenFileType kObjectFile = llvm::CGFT_ObjectFile;
#endif

    // A target triple as Module::setTargetTriple, TargetRegistry::lookupTarget and
    // Target::createTargetMachine take it: an llvm::Triple from LLVM 21, a string before.
#if LLVM_VERSION_MAJOR >= 21
    inline const llvm::Triple& tripleArgument(const llvm::Triple& triple)
    {
        return triple;
    }
#else
    inline const std::string& tripleArgument(const llvm::Triple& triple)
    {
        return triple.str();
    }
#endif
} // namespace compilerlib::emit::llvm_compat
