#ifndef COMPILERLIB_INSTRUMENTATION_TRACE_HPP
#define COMPILERLIB_INSTRUMENTATION_TRACE_HPP

namespace llvm {
class Module;
} // namespace llvm

namespace compilerlib {

void instrumentModule(llvm::Module &module);

} // namespace compilerlib

#endif // COMPILERLIB_INSTRUMENTATION_TRACE_HPP
