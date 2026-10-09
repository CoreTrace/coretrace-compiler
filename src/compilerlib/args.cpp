// SPDX-License-Identifier: Apache-2.0
#include "args_internal.hpp"
#include "clang_compat.hpp"

#include <llvm/Config/llvm-config.h>
#include <llvm/Option/ArgList.h>

#include <cstring>

namespace compilerlib
{
    const char* findArgValue(const llvm::opt::ArgStringList& args, llvm::StringRef opt)
    {
        for (size_t i = 0; i < args.size(); ++i)
        {
            if (opt == args[i])
            {
                return i + 1 < args.size() ? args[i + 1] : nullptr;
            }
            if (llvm::StringRef(args[i]).starts_with(opt) &&
                llvm::StringRef(args[i]).size() > opt.size() &&
                llvm::StringRef(args[i])[opt.size()] == '=')
            {
                return args[i] + opt.size() + 1;
            }
        }
        return nullptr;
    }

    bool hasArg(const std::vector<std::string>& args, llvm::StringRef opt)
    {
        for (const auto& arg : args)
        {
            if (opt == arg)
            {
                return true;
            }
        }
        return false;
    }

    bool requestsDebugInfo(const std::vector<std::string>& args)
    {
        std::vector<const char*> argv;
        argv.reserve(args.size());
        for (const auto& arg : args)
        {
            argv.push_back(arg.c_str());
        }
        unsigned missingIndex = 0;
        unsigned missingCount = 0;
        const llvm::opt::InputArgList parsed =
            clang_compat::driverOptTable().ParseArgs(argv, missingIndex, missingCount);
        // The rule of clang's driver, LLVM 16 to 23.
        const llvm::opt::Arg* last = parsed.getLastArg(clang_compat::options::OPT_g_Group);
        return last != nullptr && !last->getOption().matches(clang_compat::options::OPT_g0) &&
               !last->getOption().matches(clang_compat::options::OPT_ggdb0);
    }

    llvm::Triple effectiveTargetTriple(const std::vector<std::string>& args)
    {
        std::string target = LLVM_DEFAULT_TARGET_TRIPLE;
        for (size_t i = 0; i < args.size(); ++i)
        {
            llvm::StringRef arg = args[i];
            if ((arg == "-target" || arg == "--target") && i + 1 < args.size())
            {
                target = args[i + 1];
                ++i;
                continue;
            }
            if (arg.starts_with("--target="))
            {
                target = arg.substr(std::strlen("--target=")).str();
                continue;
            }
            if (arg.starts_with("-target="))
            {
                target = arg.substr(std::strlen("-target=")).str();
                continue;
            }
        }

        return llvm::Triple(llvm::Triple::normalize(target));
    }

    void normalizeEqualsArgs(std::vector<std::string>& args)
    {
        std::vector<std::string> out;
        out.reserve(args.size() + 2);
        for (auto& arg : args)
        {
            if (llvm::StringRef(arg).starts_with("-o="))
            {
                out.push_back("-o");
                out.push_back(arg.substr(3));
                continue;
            }
            if (llvm::StringRef(arg).starts_with("-x="))
            {
                out.push_back("-x");
                out.push_back(arg.substr(3));
                continue;
            }
            out.push_back(std::move(arg));
        }
        args.swap(out);
    }

    void appendDiagnostics(std::string& out, const std::string& extra)
    {
        if (extra.empty())
        {
            return;
        }
        if (!out.empty() && out.back() != '\n')
        {
            out.push_back('\n');
        }
        out.append(extra);
    }

    std::string mergeDiagnostics(const std::string& driver, const std::string& cc1)
    {
        std::string merged = driver;
        appendDiagnostics(merged, cc1);
        return merged;
    }

    bool isCc1Command(const llvm::opt::ArgStringList& args)
    {
        for (const char* arg : args)
        {
            if (std::strcmp(arg, "-cc1") == 0)
            {
                return true;
            }
        }
        return false;
    }

} // namespace compilerlib
