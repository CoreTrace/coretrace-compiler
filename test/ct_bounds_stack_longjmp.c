// SPDX-License-Identifier: Apache-2.0
// A frame that longjmp leaves keeps its stack objects registered until an outer frame
// returns. A loop that recovers from errors that way, in a frame that does not return,
// must not exhaust the registry: an overflow after the loop is still reported.
#include <setjmp.h>
#include <stdio.h>

#define ROUNDS 1000

static jmp_buf recover;

__attribute__((noinline)) static void fail(int round)
{
    volatile char buffer[64];
    buffer[round % 64] = (char)round;
    longjmp(recover, 1);
}

// Reads one element past a local array: the read stays inside the frame.
__attribute__((noinline)) static int read_past(void)
{
    volatile int values[4] = {1, 2, 3, 4};
    volatile int index = 4;
    return values[index];
}

int main(void)
{
    for (volatile int round = 0; round < ROUNDS; ++round)
    {
        if (setjmp(recover) == 0)
        {
            fail(round);
        }
    }
    printf("recovered=%d\n", ROUNDS);
    (void)read_past();
    return 0;
}
