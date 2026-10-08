// SPDX-License-Identifier: Apache-2.0
// Overhead benchmark: indexed reads and writes over a heap array and a stack array, a
// smoothing pass repeated. Prints a checksum, the same in every build.
#include <stdio.h>
#include <stdlib.h>

#define SIZE 4096

int main(int argc, char** argv)
{
    const long passes = argc > 1 ? atol(argv[1]) : 30000;
    int* values = malloc(SIZE * sizeof(int));
    int smoothed[SIZE];
    for (int i = 0; i < SIZE; ++i)
        values[i] = (i * 7919) % 1000;
    for (long pass = 0; pass < passes; ++pass)
    {
        for (int i = 1; i < SIZE - 1; ++i)
            smoothed[i] = (values[i - 1] + 2 * values[i] + values[i + 1]) / 4;
        smoothed[0] = values[0];
        smoothed[SIZE - 1] = values[SIZE - 1];
        for (int i = 0; i < SIZE; ++i)
            values[i] = smoothed[i] + (int)(pass & 1);
    }
    long checksum = 0;
    for (int i = 0; i < SIZE; ++i)
        checksum += values[i];
    free(values);
    printf("checksum=%ld\n", checksum);
    return 0;
}
