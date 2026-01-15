#ifndef COMPILERLIB_INSTRUMENTATION_ALLOC_HPP
#define COMPILERLIB_INSTRUMENTATION_ALLOC_HPP

namespace llvm {
class Module;
} // namespace llvm

namespace compilerlib {

void wrapAllocCalls(llvm::Module &module);

} // namespace compilerlib

#endif // COMPILERLIB_INSTRUMENTATION_ALLOC_HPP
