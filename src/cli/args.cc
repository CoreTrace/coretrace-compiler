#include "cli/args.h"

namespace cli
{
    ParseResult parseArgs(int argc, char* argv[])
    {
        ParseResult result;
        if (argc < 2)
        {
            result.outcome = ParseOutcome::Help;
            return result;
        }

        result.compiler_args.reserve(argc - 1);
        bool passthrough_args = false;

        for (int i = 1; i < argc; ++i)
        {
            std::string arg = argv[i];

            if (passthrough_args)
            {
                result.compiler_args.push_back(std::move(arg));
                continue;
            }

            if (arg == "-h" || arg == "--help")
            {
                result.outcome = ParseOutcome::Help;
                return result;
            }

            if (arg == "--")
            {
                result.compiler_args.push_back(arg);
                passthrough_args = true;
                continue;
            }

            if (arg == "--in-mem" || arg == "--in-memory")
            {
                result.mode = compilerlib::OutputMode::ToMemory;
                continue;
            }
            if (arg == "--instrument")
            {
                result.instrument = true;
                continue;
            }

            result.compiler_args.push_back(std::move(arg));
        }

        return result;
    }
} // namespace cli
