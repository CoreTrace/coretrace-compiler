#ifndef CORETRACE_CLI_ARGS_H
#define CORETRACE_CLI_ARGS_H

#include "compilerlib/compiler.h"

#include <string>
#include <vector>

namespace cli
{
    enum class ParseOutcome
    {
        Ok,
        Help,
        Error,
    };

    struct ParseResult
    {
        ParseOutcome outcome = ParseOutcome::Ok;
        compilerlib::OutputMode mode = compilerlib::OutputMode::ToFile;
        bool instrument = false;
        std::vector<std::string> compiler_args;
        std::string error;
    };

    ParseResult parseArgs(int argc, char* argv[]);
} // namespace cli

#endif // CORETRACE_CLI_ARGS_H
