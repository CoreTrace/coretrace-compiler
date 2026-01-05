#ifndef COMPILERLIB_INSTRUMENTATION_CONFIG_HPP
#define COMPILERLIB_INSTRUMENTATION_CONFIG_HPP

#include <string>
#include <vector>

namespace llvm {
class Module;
} // namespace llvm

namespace compilerlib {

struct RuntimeConfig {
    bool shadow_enabled = false;
    bool shadow_aggressive = false;
    bool bounds_no_abort = false;
    bool trace_enabled = true;
    bool alloc_enabled = true;
    bool bounds_enabled = true;
    bool autofree_enabled = true;
    bool alloc_trace_enabled = true;
    bool bounds_without_alloc = false;
};

void extractRuntimeConfig(const std::vector<std::string> &input,
                          std::vector<std::string> &filtered,
                          RuntimeConfig &config);

void emitRuntimeConfigGlobals(llvm::Module &module, const RuntimeConfig &config);

} // namespace compilerlib

#endif // COMPILERLIB_INSTRUMENTATION_CONFIG_HPP
