// SPDX-License-Identifier: Apache-2.0
// Windows: a function placed in .CRT$XTU, which the CRT runs as a terminator during exit,
// after the exit handlers and the destructors of global objects, releases a tracked block.
// The leak report must come after it and find nothing, and the release must not reach a
// runtime whose state is already destroyed (#152).
#include <stdio.h>
#include <stdlib.h>

static void* block;

static void release_block(void)
{
    free(block);
}

#pragma section(".CRT$XTU", read)
__declspec(allocate(".CRT$XTU"))
__attribute__((used)) static void (*const release_at_exit)(void) = release_block;

int main(void)
{
    block = malloc(16);
    puts("ok");
    return 0;
}
