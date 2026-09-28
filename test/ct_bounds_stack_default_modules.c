// SPDX-License-Identifier: Apache-2.0
// Reads one element past a local array whose address escapes, built with the default
// modules: the trace module's entry call runs before the array is allocated, and the
// array must still be registered. The callee is not inlined and the array is volatile,
// so that an optimized build keeps the escape, the array on the stack and the read.
__attribute__((noinline)) static int first(const volatile int* values)
{
    return values[0];
}

int main(void)
{
    volatile int values[4] = {1, 2, 3, 4};
    volatile int index = 4;
    int past = values[index];
    (void)past;
    return first(values) - 1;
}
