// SPDX-License-Identifier: Apache-2.0
// A block a destructor function releases when the program exits. The leak report must
// come after it and find nothing: on Linux, the functions of .fini_array run after the
// exit handlers, and the runtime's in link order.
#include <stdio.h>
#include <stdlib.h>

static char* block;

__attribute__((destructor)) static void release_at_exit(void)
{
    free(block);
}

int main(void)
{
    block = malloc(24);
    puts("ok");
    return block ? 0 : 2;
}
