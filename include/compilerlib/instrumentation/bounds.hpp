// SPDX-License-Identifier: Apache-2.0
#ifndef COMPILERLIB_INSTRUMENTATION_BOUNDS_HPP
#define COMPILERLIB_INSTRUMENTATION_BOUNDS_HPP

namespace llvm
{
    class Module;
} // namespace llvm

namespace compilerlib
{

    void instrumentMemoryAccesses(llvm::Module& module);

} // namespace compilerlib

#endif // COMPILERLIB_INSTRUMENTATION_BOUNDS_HPP
