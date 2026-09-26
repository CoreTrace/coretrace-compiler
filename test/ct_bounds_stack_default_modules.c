// SPDX-License-Identifier: Apache-2.0
// Reads one element past a local array whose address escapes, built with the default
// modules: the trace module's entry call runs before the array is allocated, and the
// array must still be registered.
static int first(const int* values)
{
    return values[0];
}

int main(void)
{
    int values[4] = {1, 2, 3, 4};
    volatile int index = 4;
    int past = values[index];
    (void)past;
    return first(values) - 1;
}
