// SPDX-License-Identifier: Apache-2.0
// Frees one allocation twice: the runtime skips the second free and reports where it
// happens and where the memory came from.
#include <stdlib.h>

int main(void)
{
    char* volatile buffer = malloc(16);
    free(buffer);
    free(buffer);
    return 0;
}
