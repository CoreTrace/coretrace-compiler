// SPDX-License-Identifier: Apache-2.0
// A local array passed to a function that reads one element past its end: the base
// the callee checks is the caller's array.
static int sum(const int* values, int count)
{
    int total = 0;
    for (int i = 0; i <= count; ++i)
        total += values[i];
    return total;
}

int main(void)
{
    int values[4] = {1, 2, 3, 4};
    volatile int total = sum(values, 4);
    (void)total;
    return 0;
}
