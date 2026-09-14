// SPDX-License-Identifier: Apache-2.0
#include <stdlib.h>

int main(void)
{
    char* leaked = malloc(32);
    if (!leaked)
        return 2;
    leaked[0] = 'x';
    return 0;
}
