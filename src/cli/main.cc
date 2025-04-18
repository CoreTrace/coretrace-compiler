#include "compilerlib/compiler.h"
#include <llvm/Support/raw_ostream.h>
#include <iostream>

int main(int argc, char *argv[])
{
    std::vector<std::string> args(argv + 1, argv + argc);
    auto [ok, err] = compilerlib::compile(args);
    llvm::errs() << err;

    return ok ? 0 : 1;
}
