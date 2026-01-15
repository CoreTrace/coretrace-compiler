#ifndef COMPILERLIB_INSTRUMENTATION_VTABLE_HPP
#define COMPILERLIB_INSTRUMENTATION_VTABLE_HPP

namespace llvm {
class Module;
} // namespace llvm

namespace compilerlib {

void instrumentVirtualCalls(llvm::Module &module, bool trace_calls, bool dump_vtable);

} // namespace compilerlib

#endif // COMPILERLIB_INSTRUMENTATION_VTABLE_HPP
