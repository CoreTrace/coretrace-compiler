// SPDX-License-Identifier: Apache-2.0
// Reads a block after freeing it. The block is still in the quarantine, recorded as
// freed, so the read is reported as a use-after-free. The pointer read back is volatile,
// so that an optimized build keeps the read.
#include <stdlib.h>

int main(void)
{
    int* values = malloc(4 * sizeof(int));
    if (!values)
    {
        return 2;
    }
    for (int i = 0; i < 4; ++i)
    {
        values[i] = i;
    }
    int* volatile dangling = values;
    free(values);

    volatile int value = dangling[2];
    (void)value;
    return 0;
}
