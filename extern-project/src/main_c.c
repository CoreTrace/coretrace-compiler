// SPDX-License-Identifier: Apache-2.0
#include "compilerlib/compiler_c.h"

#include <stdio.h>

int main(int argc, const char *argv[])
{
    char diagnostics[4096];

    if (!compile_c(argc - 1, argv + 1, diagnostics, (int)sizeof(diagnostics))) {
        fputs(diagnostics, stderr);
        return 1;
    }
    return 0;
}
