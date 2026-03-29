// SPDX-License-Identifier: Apache-2.0
#include <stdlib.h>

void* bar(void)
{
    return malloc(sizeof(void*));
}

int main(void)
{
    bar();
    free(bar());
    return 0;
}
