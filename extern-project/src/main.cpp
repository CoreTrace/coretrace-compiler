#include "compilerlib/compiler.h"
#include <iostream>

int main(int argc, char *argv[])
{
    std::vector<std::string> args(argv + 1, argv + argc);
    auto [ok, err] = compilerlib::compile(args);

    if (!ok)
        std::cerr << err;

    return ok ? 0 : 1;
}
