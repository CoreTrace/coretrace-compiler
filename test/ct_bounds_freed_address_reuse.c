// SPDX-License-Identifier: Apache-2.0
// Strings that strdup allocates inside the C library, which the runtime does not track,
// right after the program frees a tracked block of the same size: the allocator would
// commonly hand the freed block's address to strdup. Reading the strings is valid and may
// not be reported as a use-after-free.
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    static const char text[] = "0123456789abcdefghijklmnopqrstu";
    unsigned sum = 0;
    for (int i = 0; i < 64; ++i)
    {
        char* block = malloc(sizeof(text));
        if (!block)
        {
            return 2;
        }
        memcpy(block, text, sizeof(text));
        free(block);

        char* copy = strdup(text);
        if (!copy)
        {
            return 2;
        }
        for (size_t c = 0; copy[c]; ++c)
        {
            sum += (unsigned char)copy[c];
        }
        free(copy);
    }
    printf("sum=%u\n", sum);
    return 0;
}
