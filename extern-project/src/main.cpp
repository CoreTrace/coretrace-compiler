#include "compilerlib/compiler.h"
#include <iostream>

int main(int argc, char *argv[])
{
    // std::vector<std::string> args(argv + 1, argv + argc);
    // auto [ok, err] = compilerlib::compile(args);

    // if (!ok)
    //     std::cerr << err;

    std::vector<std::string> args;
    args.reserve(argc - 1);

    compilerlib::OutputMode mode = compilerlib::OutputMode::ToFile;
    bool instrument = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--in-mem" || arg == "--in-memory") {
            mode = compilerlib::OutputMode::ToMemory;
        } else if (arg == "--instrument") {
            instrument = true;
        } else {
            args.push_back(std::move(arg));
        }
    }

    auto res = compilerlib::compile(args, mode, instrument);

    if (!res.success) {
        std::cerr << res.diagnostics;
        return 1;
    }

    if (mode == compilerlib::OutputMode::ToMemory && !res.llvmIR.empty()) {
        std::cout << res.llvmIR << std::endl;
    }

    // return ok ? 0 : 1;
    return 0;
}
