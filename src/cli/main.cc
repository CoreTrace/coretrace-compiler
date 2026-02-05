#include "compilerlib/compiler.h"
#include <iostream>

#include "cli/args.h"
#include "cli/help.h"

int main(int argc, char* argv[])
{
    auto parsed = cli::parseArgs(argc, argv);
    if (parsed.outcome == cli::ParseOutcome::Help)
    {
        cli::printHelp(argv[0]);
        return 0;
    }
    if (parsed.outcome == cli::ParseOutcome::Error)
    {
        std::cerr << parsed.error;
        return 1;
    }

    auto res = compilerlib::compile(parsed.compiler_args, parsed.mode, parsed.instrument);
    if (!res.success)
    {
        std::cerr << res.diagnostics;
        return 1;
    }

    if (parsed.mode == compilerlib::OutputMode::ToMemory && !res.llvmIR.empty())
    {
        std::cout << res.llvmIR << std::endl;
    }

    return 0;
}
