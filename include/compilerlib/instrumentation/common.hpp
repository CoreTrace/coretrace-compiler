#ifndef COMPILERLIB_INSTRUMENTATION_COMMON_HPP
#define COMPILERLIB_INSTRUMENTATION_COMMON_HPP

#include <string>

namespace llvm {
class Function;
class Instruction;
} // namespace llvm

namespace compilerlib {

bool shouldInstrument(const llvm::Function &func);
std::string formatSiteString(const llvm::Instruction &inst);

} // namespace compilerlib

#endif // COMPILERLIB_INSTRUMENTATION_COMMON_HPP
