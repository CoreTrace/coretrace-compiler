#include "cli/help.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace cli
{
    void printHelp(const char* argv0)
    {
        std::string name = "cc";
        if (argv0 && *argv0)
        {
            std::filesystem::path path(argv0);
            auto fname = path.filename().string();
            if (!fname.empty())
            {
                name = fname;
            }
        }

        std::cout
            << "CoreTrace Compiler (based on the Clang/LLVM toolchain)\n"
            << "\n"
            << "Usage:\n"
            << "  " << name << " [options] <sources/objects>...\n"
            << "\n"
            << "Core options:\n"
            << "  -h, --help               Show this help and exit.\n"
            << "  --instrument             Enable CoreTrace instrumentation (required for "
               "--ct-*).\n"
            << "  --in-mem, --in-memory     Print LLVM IR to stdout (use with -emit-llvm).\n"
            << "\n"
            << "Instrumentation toggles:\n"
            << "  --ct-modules=<list>       Comma-separated list: trace,alloc,bounds,vtable,all.\n"
            << "  --ct-shadow               Enable shadow memory.\n"
            << "  --ct-shadow-aggressive    Enable aggressive shadow mode.\n"
            << "  --ct-shadow=aggressive    Same as --ct-shadow-aggressive.\n"
            << "  --ct-bounds-no-abort      Do not abort on bounds errors.\n"
            << "  --ct-no-trace / --ct-trace\n"
            << "  --ct-no-alloc / --ct-alloc\n"
            << "  --ct-no-bounds / --ct-bounds\n"
            << "  --ct-no-autofree / --ct-autofree\n"
            << "  --ct-no-alloc-trace / --ct-alloc-trace\n"
            << "  --ct-no-vcall-trace / --ct-vcall-trace\n"
            << "  --ct-no-vtable-diag / --ct-vtable-diag\n"
            << "\n"
            << "Defaults:\n"
            << "  instrumentation: off\n"
            << "  modules: trace,alloc,bounds (vtable disabled)\n"
            << "  shadow: off, bounds abort: on, autofree: off, alloc trace: on\n"
            << "\n"
            << "Notes:\n"
            << "  - All other arguments are forwarded to clang.\n"
            << "  - Output defaults to a.out when linking (override with -o or -o=<path>).\n"
            << "\n"
            << "Examples:\n"
            << "  " << name << " --instrument -o app main.c\n"
            << "\n"
            << "Exit codes:\n"
            << "  0 on success, 1 on compiler errors.\n";
    }
} // namespace cli
