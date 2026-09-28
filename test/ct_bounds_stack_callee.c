// SPDX-License-Identifier: Apache-2.0
// A local array passed to a function that reads one element past its end: the base
// the callee checks is the caller's array. The callee is not inlined, and the array and
// the count are volatile, so that an optimized build keeps the call, keeps the array on
// the stack and cannot fold the read past the end away.
__attribute__((noinline)) static int sum(const volatile int* values, int count)
{
    int total = 0;
    for (int i = 0; i <= count; ++i)
        total += values[i];
    return total;
}

int main(void)
{
    volatile int values[4] = {1, 2, 3, 4};
    volatile int count = 4;
    volatile int total = sum(values, count);
    (void)total;
    return 0;
}
