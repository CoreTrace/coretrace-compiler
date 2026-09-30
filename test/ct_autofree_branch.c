// SPDX-License-Identifier: Apache-2.0
// A block allocated in a branch and never released: it does not reach the return on
// every path, so auto-free leaves it to the leak report. Releasing it at the return
// produced invalid IR at -O1 and above.
#include <stdio.h>
#include <stdlib.h>

static int first_byte(int n)
{
    int result = 0;
    if (n > 0)
    {
        // A volatile store stays although nothing reads it, and so does the block.
        volatile char* block = malloc(8);
        block[0] = (char)n;
        result = n;
    }
    return result;
}

int main(int argc, char** argv)
{
    (void)argv;
    printf("first=%d\n", first_byte(argc + 6));
    return 0;
}
