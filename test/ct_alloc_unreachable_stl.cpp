// SPDX-License-Identifier: Apache-2.0
// An unreachable allocation in a program that also builds a std::string. The program's copies
// of the library's inline allocation functions are instrumented; the runtime, which formats
// its trace into strings, must not re-enter itself through them (#197). Only the program's
// block leaks.
#include <cstdio>
#include <string>

int main()
{
    std::string text(64, 'x');
    new int;
    std::printf("size=%zu\n", text.size());
    return 0;
}
