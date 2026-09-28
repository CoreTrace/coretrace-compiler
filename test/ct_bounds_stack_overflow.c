// SPDX-License-Identifier: Apache-2.0
// Reads one element past a local array. The read stays inside the frame, so nothing
// crashes, but it is out of the array's bounds. The array is volatile so that an
// optimized build keeps it on the stack and keeps the read: otherwise the optimizer
// turns the constant array into a global and drops the unused read.
int main(void)
{
    volatile int values[4] = {1, 2, 3, 4};
    volatile int index = 4;
    int past = values[index];
    (void)past;
    return 0;
}
