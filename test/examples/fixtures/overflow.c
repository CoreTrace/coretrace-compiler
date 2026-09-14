// SPDX-License-Identifier: Apache-2.0
#include <stdlib.h>

int main(int argc, char** argv)
{
    (void)argv;
    char* buffer = malloc(8);
    if (!buffer)
        return 2;
    /* argc is 1 when run without arguments, so this writes one byte past the end. */
    buffer[argc + 7] = 1;
    free(buffer);
    return 0;
}
