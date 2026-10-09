// SPDX-License-Identifier: Apache-2.0
// The unreachable allocation of ct_alloc_basic.c, built with --ct-autofree: the runtime
// reports it as unreachable and releases it at once, so no leak remains at exit.
#include <stdlib.h>

int main(void)
{
    malloc(16);
    return 0;
}
