// SPDX-License-Identifier: Apache-2.0
// Overhead benchmark: allocations of varied sizes, written, read and released, with a few
// blocks alive at a time. Prints a checksum, the same in every build.
#include <stdio.h>
#include <stdlib.h>

#define LIVE 64

int main(int argc, char** argv)
{
    const long rounds = argc > 1 ? atol(argv[1]) : 400000;
    unsigned char* live[LIVE] = {0};
    unsigned long checksum = 0;
    for (long i = 0; i < rounds; ++i)
    {
        const int slot = (int)(i % LIVE);
        free(live[slot]);
        const size_t size = 16 + (size_t)(i * 37 % 480);
        live[slot] = malloc(size);
        for (size_t j = 0; j < size; j += 8)
            live[slot][j] = (unsigned char)(i + j);
        checksum += live[slot][size / 2 & ~(size_t)7];
    }
    for (int slot = 0; slot < LIVE; ++slot)
        free(live[slot]);
    printf("checksum=%lu\n", checksum);
    return 0;
}
