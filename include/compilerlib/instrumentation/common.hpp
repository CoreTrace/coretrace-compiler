// SPDX-License-Identifier: Apache-2.0
#ifndef COMPILERLIB_INSTRUMENTATION_COMMON_HPP
#define COMPILERLIB_INSTRUMENTATION_COMMON_HPP

#include <string>

namespace llvm
{
    class Function;
    class Instruction;
} // namespace llvm

namespace compilerlib
{

    // A function defined by user code: not a declaration, not runtime-internal, not
    // opted out, not an available_externally copy, and not from a system header.
    bool isUserDefinedFunction(const llvm::Function& func);

    // isUserDefinedFunction minus linkonce/weak bodies, which every translation unit
    // duplicates and the linker merges. Entry/exit tracing and per-access checks would
    // fire from whichever copy the linker keeps and so are kept out of them.
    bool shouldInstrument(const llvm::Function& func);

    // Functions whose allocation and release calls the allocation pass rewrites: every
    // definition it may modify, system headers and linkonce/weak bodies included. Any
    // code may release a block user code allocated (a virtual destructor's deleting
    // variant, emitted linkonce_odr in every TU; std::unique_ptr; containers), so
    // releases are rewritten everywhere; allocations only where isUserCall holds.
    // Rewriting is identical in each linkonce copy, so merging is harmless.
    bool shouldRewriteAllocations(const llvm::Function& func);

    // A call written in user code: its own source location, which code inlined from a
    // system header keeps, is outside system headers. Without a location, the enclosing
    // function decides.
    bool isUserCall(const llvm::Instruction& call);
    std::string formatSiteString(const llvm::Instruction& inst);

} // namespace compilerlib

#endif // COMPILERLIB_INSTRUMENTATION_COMMON_HPP
