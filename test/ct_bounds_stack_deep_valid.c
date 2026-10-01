// SPDX-License-Identifier: Apache-2.0
// A recursion deeper than the registry of stack objects holds: the frames past its
// capacity are not registered, and their accesses are not checked. Every frame reads its
// own array in bounds, so no access may be reported, however deep, and the frames that
// return drop their objects again.
#include <stdio.h>

#define DEPTH 2000

__attribute__((noinline)) static int descend(int depth)
{
    volatile int values[4] = {depth, depth + 1, depth + 2, depth + 3};
    int sum = values[depth % 4];
    if (depth > 0)
    {
        sum += descend(depth - 1);
    }
    return sum + values[(depth + 1) % 4];
}

int main(void)
{
    printf("sum=%d\n", descend(DEPTH));
    printf("again=%d\n", descend(10));
    return 0;
}
