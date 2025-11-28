#include "compilerlib/compiler.h"
#include <llvm/Support/raw_ostream.h>
#include <iostream>

int main(int argc, char *argv[])
{
    // std::vector<std::string> args(argv + 1, argv + argc);
    // // auto [ok, err] = compilerlib::compile(args);
    // compilerlib::CompileResult result = compilerlib::compile(args);
    // bool ok = result.success;
    // std::string err = result.diagnostics;
    // llvm::errs() << err;

    std::vector<std::string> args;
    args.reserve(argc - 1);

    compilerlib::OutputMode mode = compilerlib::OutputMode::ToFile;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--in-mem" || arg == "--in-memory") {
            mode = compilerlib::OutputMode::ToMemory;
        } else {
            args.push_back(std::move(arg));
        }
    }

    auto res = compilerlib::compile(args, mode);

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
