#ifndef COMPILERLIB_COMPILER_H
#define COMPILERLIB_COMPILER_H

#include <string>
#include <vector>

namespace compilerlib
{

enum class OutputMode
{
    ToFile,
    ToMemory,
};

struct CompileResult
{
    bool success;
    std::string diagnostics;
    std::string llvmIR;
};

// std::pair<bool, std::string> compile(const std::vector<std::string>& args);
CompileResult compile(const std::vector<std::string>& args, OutputMode mode = OutputMode::ToFile);

#ifdef __cplusplus
extern "C" {
#endif

int compile_c(int argc, const char **argv, char *output_buffer, int buffer_size);

#ifdef __cplusplus
}
#endif

} // namespace compilerlib

#endif // COMPILERLIB_COMPILER_H
