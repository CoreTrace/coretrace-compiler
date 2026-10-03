// SPDX-License-Identifier: Apache-2.0
#ifndef COMPILERLIB_INSTRUMENTATION_ALLOC_HPP
#define COMPILERLIB_INSTRUMENTATION_ALLOC_HPP

namespace llvm
{
    class Module;
} // namespace llvm

namespace compilerlib
{

    void wrapAllocCalls(llvm::Module& module);

    // Runs before optimization. Clang marks a new-expression's calls to the replaceable
    // operator new and delete as builtin, which lets the optimizer remove an allocation it
    // can prove unused, as C++ allows. The calls wrapAllocCalls tracks lose that mark, so
    // that they are still there when it runs.
    void keepTrackedAllocationCalls(llvm::Module& module);

} // namespace compilerlib

#endif // COMPILERLIB_INSTRUMENTATION_ALLOC_HPP
