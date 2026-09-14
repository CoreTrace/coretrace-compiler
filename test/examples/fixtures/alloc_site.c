// SPDX-License-Identifier: Apache-2.0
#include <stdlib.h>
int main()
{
    char* buffer = malloc(16);
    free(buffer);
    return 0;
}
