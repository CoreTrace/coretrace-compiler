// SPDX-License-Identifier: Apache-2.0
//
// Code generation names that LLVM 18 turned into scoped enumerations. compilerlib builds
// against LLVM 16 and later; the differences between versions stay here.
#pragma once

#include <llvm/Config/llvm-config.h>
#include <llvm/Support/CodeGen.h>

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
} // namespace compilerlib::emit::llvm_compat
