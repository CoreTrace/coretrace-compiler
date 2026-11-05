// SPDX-License-Identifier: Apache-2.0
#include "compilerlib/compiler.h"
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "cli/args.h"
#include "cli/help.h"

namespace
{
    // clang moves the whole process to the directory of -working-directory, so that every
    // job, the linker's included, runs there. compilerlib leaves the process where it is, and
    // only runs its own jobs in that directory: cc, a process of its own, moves there first, as
    // clang does, and then compiles without the option. False, with `error` set, when the
    // directory cannot be entered.
    bool enterWorkingDirectory(std::vector<std::string>& args, std::string& error)
    {
        std::string directory;
        bool found = false;
        std::vector<std::string> remaining;
        remaining.reserve(args.size());
        for (size_t i = 0; i < args.size(); ++i)
        {
            const std::string& arg = args[i];
            if (arg == "--")
            {
                remaining.insert(remaining.end(), args.begin() + static_cast<std::ptrdiff_t>(i),
                                 args.end());
                break;
            }
            if (arg == "-working-directory" && i + 1 < args.size())
            {
                directory = args[++i];
                found = true;
                continue;
            }
            constexpr std::string_view joined = "-working-directory=";
            if (arg.compare(0, joined.size(), joined) == 0)
            {
                directory = arg.substr(joined.size());
                found = true;
                continue;
            }
            remaining.push_back(arg);
        }
        if (!found)
            return true;

        std::error_code ec;
        std::filesystem::current_path(directory, ec);
        if (ec)
        {
            error = "error: unable to set working directory: " + directory + "\n";
            return false;
        }
        args.swap(remaining);
        return true;
    }
} // namespace

int main(int argc, char* argv[])
{
    auto parsed = cli::parseArgs(argc, argv);
    if (parsed.outcome == cli::ParseOutcome::Help)
    {
        cli::printHelp(argv[0]);
        return 0;
    }
    if (parsed.outcome == cli::ParseOutcome::Version)
    {
        cli::printVersion();
        return 0;
    }
    if (parsed.outcome == cli::ParseOutcome::Error)
    {
        std::cerr << parsed.error;
        return 1;
    }
    std::string directoryError;
    if (!enterWorkingDirectory(parsed.compiler_args, directoryError))
    {
        std::cerr << directoryError;
        return 1;
    }

    auto res = compilerlib::compile(parsed.compiler_args, parsed.mode, parsed.instrument);
    if (!res.diagnostics.empty())
    {
        std::cerr << res.diagnostics;
        if (res.diagnostics.back() != '\n')
        {
            std::cerr << '\n';
        }
    }
    if (!res.success)
    {
        return 1;
    }

    if (parsed.mode == compilerlib::OutputMode::ToMemory && !res.llvmIR.empty())
    {
        std::cout << res.llvmIR << std::endl;
    }

    return 0;
}
