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

    // Call-site rewriting of allocators applies to linkonce/weak user bodies too: a
    // virtual destructor's deleting variant is emitted linkonce_odr in every TU and
    // holds the operator delete call, so skipping it hides every polymorphic delete
    // from the runtime. Rewriting is identical in each copy, so merging is harmless.
    bool shouldRewriteAllocations(const llvm::Function& func);
    std::string formatSiteString(const llvm::Instruction& inst);

} // namespace compilerlib

#endif // COMPILERLIB_INSTRUMENTATION_COMMON_HPP
