// SPDX-License-Identifier: Apache-2.0
// Leaks one allocation: the leak report names where it was allocated, with the path
// the compiler was given.
#include <stdlib.h>

int main(void)
{
    char* volatile leaked = malloc(16);
    (void)leaked;
    return 0;
}
